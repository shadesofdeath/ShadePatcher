//
// explorer-double-click-up - double click the empty space of a folder to go up to its parent.
//
// Adapted from the idea behind the Windhawk mod "Explorer Double Click Up" (explorer-double-click-up) by
// wrldspawn. The implementation here is written against this engine's API.
//
// What this does
// --------------
// A double click on the background of a file list (not on a file, not on a group header's items) takes the
// window to the parent folder, the way the classic Windows XP file manager habit and every two-pane commander
// works. A double click on an item still opens it, and a click on the background still clears the selection as
// before: only the second click within the double-click time and rectangle is turned into "up".
//
// How it works
// ------------
// FileCabinet_CreateViewWindow2 in ExplorerFrame.dll is called every time a tab shows a folder. It is handed the
// tab's IShellBrowser and returns the view window it made (class SHELLDLL_DefView), whose child DirectUIHWND is
// the items view. The view window is subclassed on its own thread. The child sends its parent a WM_PARENTNOTIFY
// for every left button press, so the subclass sees each click without touching the DirectUI control itself.
//
// Whether the click landed on empty space is answered by UI Automation, which is what the shell uses to describe
// the items view: the element under the cursor is the list itself (class UIItemsView) or a group header
// (UIGroupItem) for empty space, and UIItem for a file or folder. Two such clicks on the same view within
// GetDoubleClickTime and the system double-click rectangle are a double click, and the view is asked to go up
// through IShellBrowser::BrowseObject(SBSP_PARENT), the same call the "Up" button makes.
//
// The navigation is posted back to the view rather than done inside WM_PARENTNOTIFY: that message arrives while
// the items view is still handling the button press, and destroying the view underneath it is not something to
// do from there.
//
// Windows that are already open when the mod is enabled have not been through the hook. Their views are adopted
// at start-up, and the browser they belong to is looked up through IShellWindows the first time one of them has
// to go up.
//
#define SP_MOD_ID "explorer-double-click-up"
#include "engine/modapi.h"

#include <unknwn.h>
#include <commctrl.h>
#include <windowsx.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <exdisp.h>
#include <servprov.h>
#include <uiautomation.h>

#include <atomic>
#include <new>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "OleAut32.lib")

namespace {

constexpr wchar_t kFrameClass[] = L"CabinetWClass";
constexpr wchar_t kTabClass[]   = L"ShellTabWindowClass";
constexpr wchar_t kViewClass[]  = L"SHELLDLL_DefView";

constexpr UINT_PTR kSubclassId = 1;

std::atomic<bool> g_enabled{ true };

// Posted to a view window when its background was double clicked.
UINT g_msgGoUp = 0;

// Per-view state: the browser that owns the view and the last background click seen.
struct ViewState
{
    IShellBrowser* browser = nullptr;   // unowned: the browser owns the tab, which outlives the view window
    DWORD          lastTick = 0;
    POINT          lastPoint = {};
    bool           lastOnBackground = false;
};

// ---------------------------------------------------------------------------------------------------------------
// UI Automation: what is under the cursor
//
// CUIAutomation is free threaded, so one instance serves every view thread. It is created on first use, on the
// view's thread, and released when the mod unloads.
// ---------------------------------------------------------------------------------------------------------------

SRWLOCK        g_uiaLock = SRWLOCK_INIT;
IUIAutomation* g_uia = nullptr;

IUIAutomation* Automation()
{
    AcquireSRWLockExclusive(&g_uiaLock);
    if (!g_uia)
    {
        IUIAutomation* uia = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia));
        if (SUCCEEDED(hr) && uia)
        {
            g_uia = uia;
        }
        else
        {
            SP_LogError(L"UI Automation could not be created: 0x%08X", (unsigned)hr);
        }
    }
    IUIAutomation* result = g_uia;
    if (result)
    {
        result->AddRef();
    }
    ReleaseSRWLockExclusive(&g_uiaLock);
    return result;
}

void ReleaseAutomation()
{
    AcquireSRWLockExclusive(&g_uiaLock);
    IUIAutomation* uia = g_uia;
    g_uia = nullptr;
    ReleaseSRWLockExclusive(&g_uiaLock);
    if (uia)
    {
        uia->Release();
    }
}

// TRUE when the screen point is over the background of an items view: the list itself or a group header, not an
// item. Anything UI Automation cannot answer counts as "not background", so a doubtful click never navigates.
bool IsBackgroundAt(POINT screen)
{
    IUIAutomation* uia = Automation();
    if (!uia)
    {
        return false;
    }

    bool background = false;
    IUIAutomationElement* element = nullptr;
    if (SUCCEEDED(uia->ElementFromPoint(screen, &element)) && element)
    {
        BSTR className = nullptr;
        if (SUCCEEDED(element->get_CurrentClassName(&className)) && className)
        {
            background = wcscmp(className, L"UIItemsView") == 0 || wcscmp(className, L"UIGroupItem") == 0;
            if (SP_LogEnabled(SP_LOG_DEBUG))
            {
                SP_LogDebug(L"Click on %s at %ld,%ld", className, screen.x, screen.y);
            }
            SysFreeString(className);
        }
        element->Release();
    }
    uia->Release();
    return background;
}

// ---------------------------------------------------------------------------------------------------------------
// Going up
// ---------------------------------------------------------------------------------------------------------------

// The browser of a view adopted without going through the hook: the IShellWindows entry whose top-level browser
// owns the tab the view sits in. Looked up on the view's own thread, so the proxy is used where it was made.
IShellBrowser* FindBrowserOf(HWND hView)
{
    HWND hTab = GetAncestor(hView, GA_PARENT);
    while (hTab)
    {
        wchar_t cls[64] = {};
        GetClassNameW(hTab, cls, ARRAYSIZE(cls));
        if (wcscmp(cls, kTabClass) == 0)
        {
            break;
        }
        hTab = GetAncestor(hTab, GA_PARENT);
    }
    if (!hTab)
    {
        return nullptr;
    }

    IShellBrowser* found = nullptr;
    IShellWindows* windows = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows))) || !windows)
    {
        return nullptr;
    }

    long count = 0;
    windows->get_Count(&count);
    for (long i = 0; i < count && !found; ++i)
    {
        VARIANT index;
        VariantInit(&index);
        index.vt = VT_I4;
        index.lVal = i;

        IDispatch* dispatch = nullptr;
        if (FAILED(windows->Item(index, &dispatch)) || !dispatch)
        {
            continue;
        }

        IServiceProvider* services = nullptr;
        if (SUCCEEDED(dispatch->QueryInterface(IID_PPV_ARGS(&services))) && services)
        {
            IShellBrowser* browser = nullptr;
            if (SUCCEEDED(services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) && browser)
            {
                HWND hOwner = nullptr;
                if (SUCCEEDED(browser->GetWindow(&hOwner)) && hOwner == hTab)
                {
                    found = browser;    // the reference is handed to the caller
                }
                else
                {
                    browser->Release();
                }
            }
            services->Release();
        }
        dispatch->Release();
    }
    windows->Release();
    return found;
}

void GoUp(HWND hView, ViewState* state)
{
    if (!g_enabled.load(std::memory_order_relaxed) || !IsWindow(hView))
    {
        return;
    }

    IShellBrowser* browser = state->browser;
    IShellBrowser* owned = nullptr;
    if (!browser)
    {
        owned = FindBrowserOf(hView);
        browser = owned;
    }
    if (!browser)
    {
        SP_LogError(L"View %p has no browser to go up with", hView);
        return;
    }

    HRESULT hr = browser->BrowseObject(nullptr, SBSP_SAMEBROWSER | SBSP_PARENT);
    if (FAILED(hr))
    {
        SP_LogDebug(L"View %p could not go up: 0x%08X", hView, (unsigned)hr);
    }
    else
    {
        SP_Log(L"Background double clicked; view %p goes to the parent folder", hView);
    }

    if (owned)
    {
        owned->Release();
    }
}

// ---------------------------------------------------------------------------------------------------------------
// The subclass
// ---------------------------------------------------------------------------------------------------------------

void OnButtonDown(HWND hView, ViewState* state)
{
    if (!g_enabled.load(std::memory_order_relaxed))
    {
        state->lastOnBackground = false;
        return;
    }

    const DWORD now = GetMessageTime();
    POINT pt;
    const DWORD packed = GetMessagePos();
    pt.x = GET_X_LPARAM(packed);
    pt.y = GET_Y_LPARAM(packed);

    const bool onBackground = IsBackgroundAt(pt);

    bool isDoubleClick = false;
    if (onBackground && state->lastOnBackground)
    {
        const DWORD delta = now - state->lastTick;
        const int dx = pt.x - state->lastPoint.x;
        const int dy = pt.y - state->lastPoint.y;
        const int cx = GetSystemMetrics(SM_CXDOUBLECLK) / 2;
        const int cy = GetSystemMetrics(SM_CYDOUBLECLK) / 2;
        isDoubleClick = delta <= GetDoubleClickTime() && dx <= cx && dx >= -cx && dy <= cy && dy >= -cy;
    }

    if (isDoubleClick)
    {
        // The pair is used up: a third click starts a new pair rather than navigating twice.
        state->lastOnBackground = false;
        PostMessageW(hView, g_msgGoUp, 0, 0);
        return;
    }

    state->lastTick = now;
    state->lastPoint = pt;
    state->lastOnBackground = onBackground;
}

LRESULT CALLBACK ViewSubclass(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                              UINT_PTR idSubclass, DWORD_PTR refData)
{
    UNREFERENCED_PARAMETER(idSubclass);
    ViewState* state = (ViewState*)refData;

    if (uMsg == WM_PARENTNOTIFY && LOWORD(wParam) == WM_LBUTTONDOWN)
    {
        OnButtonDown(hWnd, state);
    }
    else if (g_msgGoUp && uMsg == g_msgGoUp)
    {
        GoUp(hWnd, state);
        return 0;
    }
    else if (uMsg == WM_NCDESTROY)
    {
        RemoveWindowSubclass(hWnd, ViewSubclass, kSubclassId);
        delete state;
        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }

    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

bool ClassIs(HWND hWnd, const wchar_t* className)
{
    wchar_t buffer[64] = {};
    return hWnd && GetClassNameW(hWnd, buffer, ARRAYSIZE(buffer)) && wcscmp(buffer, className) == 0;
}

// A view inside a File Explorer tab. The desktop and the panes of file dialogs come through the same hook and
// are left alone, as in the original.
bool IsTabView(HWND hView)
{
    if (!ClassIs(hView, kViewClass))
    {
        return false;
    }
    for (HWND h = GetAncestor(hView, GA_PARENT); h; h = GetAncestor(h, GA_PARENT))
    {
        if (ClassIs(h, kTabClass))
        {
            return ClassIs(GetAncestor(h, GA_ROOT), kFrameClass);
        }
    }
    return false;
}

// `browser` may be null for a view adopted at start-up. `ownThread` is TRUE on the thread that owns the window.
void Watch(HWND hView, IShellBrowser* browser, bool ownThread)
{
    if (!hView || !IsWindow(hView) || !IsTabView(hView))
    {
        return;
    }

    DWORD_PTR existing = 0;
    if (GetWindowSubclass(hView, ViewSubclass, kSubclassId, &existing))
    {
        return;
    }

    ViewState* state = new (std::nothrow) ViewState();
    if (!state)
    {
        return;
    }
    state->browser = browser;

    BOOL ok = ownThread ? SetWindowSubclass(hView, ViewSubclass, kSubclassId, (DWORD_PTR)state)
                        : SP_SetWindowSubclassFromAnyThread(hView, ViewSubclass, kSubclassId, (DWORD_PTR)state);
    if (!ok)
    {
        delete state;
        SP_LogError(L"The view window could not be watched: %lu", GetLastError());
        return;
    }

    SP_LogDebug(L"Watching view window %p", hView);
}

// ---------------------------------------------------------------------------------------------------------------
// The hook
// ---------------------------------------------------------------------------------------------------------------

using CreateViewWindow2_t = HRESULT(__cdecl*)(IShellBrowser* browser, void* folderSettings, IShellView* newView,
                                              IShellView* previousView, RECT* bounds, HWND* result);
CreateViewWindow2_t g_origCreateViewWindow2 = nullptr;

HRESULT __cdecl CreateViewWindow2_Hook(IShellBrowser* browser, void* folderSettings, IShellView* newView,
                                       IShellView* previousView, RECT* bounds, HWND* result)
{
    HRESULT hr = g_origCreateViewWindow2(browser, folderSettings, newView, previousView, bounds, result);

    if (SUCCEEDED(hr) && result && *result)
    {
        Watch(*result, browser, true);
    }
    return hr;
}

// ---------------------------------------------------------------------------------------------------------------
// Windows already open
// ---------------------------------------------------------------------------------------------------------------

BOOL CALLBACK AdoptChild(HWND hChild, LPARAM)
{
    if (ClassIs(hChild, kViewClass))
    {
        Watch(hChild, nullptr, false);
    }
    return TRUE;
}

BOOL CALLBACK AdoptWindow(HWND hWnd, LPARAM)
{
    DWORD process = 0;
    GetWindowThreadProcessId(hWnd, &process);
    if (process == GetCurrentProcessId() && ClassIs(hWnd, kFrameClass))
    {
        EnumChildWindows(hWnd, AdoptChild, 0);
    }
    return TRUE;
}

// ---------------------------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------------------------

void LoadSettings()
{
    g_enabled.store(SP_GetIntSetting(L"Enabled", 1) != 0, std::memory_order_relaxed);
}

BOOL Init()
{
    LoadSettings();

    g_msgGoUp = RegisterWindowMessageW(L"ShadePatcher." SP_MOD_ID ".GoUp");
    if (!g_msgGoUp)
    {
        SP_LogError(L"The window message could not be registered");
        return FALSE;
    }

    HMODULE hFrame = GetModuleHandleW(L"ExplorerFrame.dll");
    if (!hFrame)
    {
        hFrame = LoadLibraryW(L"ExplorerFrame.dll");
    }
    if (!hFrame)
    {
        SP_LogError(L"ExplorerFrame.dll could not be loaded");
        return FALSE;
    }

    static const wchar_t* const kNames[] =
    {
        L"long __cdecl FileCabinet_CreateViewWindow2(struct IShellBrowser *,struct tagFolderSetDataBase *,struct IShellView *,struct IShellView *,struct tagRECT *,struct HWND__ * *)",
    };

    SP_SymbolHook hooks[1] = {};
    hooks[0].symbols = kNames;
    hooks[0].symbolCount = ARRAYSIZE(kNames);
    hooks[0].pOriginal = (void**)&g_origCreateViewWindow2;
    hooks[0].hookFunction = (void*)CreateViewWindow2_Hook;
    hooks[0].optional = FALSE;

    if (!SP_HookSymbols(hFrame, hooks, ARRAYSIZE(hooks)))
    {
        SP_LogError(L"The view window builder was not found in this build");
        return FALSE;
    }

    SP_Log(L"Watching for file lists; a double click on their background goes up");
    return TRUE;
}

void AfterInit()
{
    EnumWindows(AdoptWindow, 0);
}

void BeforeUninit()
{
    // Views keep their subclass until they are destroyed, which is harmless: the setting is read on every click,
    // so turning the mod off stops the behaviour immediately. The engine keeps the code mapped for them.
    g_enabled.store(false, std::memory_order_relaxed);
}

void Uninit()
{
    ReleaseAutomation();
}

}   // namespace

SP_MOD_DEFINE(g_modExplorerDoubleClickUp) =
{
    /* id             */ SP_MOD_ID,
    /* name           */ L"Double click the empty space of a folder to go up",
    /* basedOn        */ "explorer-double-click-up",
    /* originalAuthor */ "wrldspawn",
    /* targets        */ SP_TARGET_EXPLORER,
    /* minOsBuild     */ 0,
    /* flags          */ 0,
    /* Init           */ Init,
    /* AfterInit      */ AfterInit,
    /* SettingsChanged*/ LoadSettings,
    /* BeforeUninit   */ BeforeUninit,
    /* Uninit         */ Uninit,
};
