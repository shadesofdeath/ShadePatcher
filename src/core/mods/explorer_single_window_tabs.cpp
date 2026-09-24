//
// explorer-single-window-tabs - open every new File Explorer window as a tab in the window already open.
//
// Adapted from the idea behind the Windhawk mod "Explorer Single Window Tabs" (explorer-single-window-tabs)
// by ALMAS CP. The implementation here is written against this engine's API.
//
// The problem
// -----------
// File Explorer has had tabs since Windows 11 22H2, but nothing outside Explorer knows about them. A folder
// opened from the desktop, the Start menu, a "Show in folder" link or a script always arrives in a fresh window,
// so after a morning's work there are six Explorer windows on the taskbar and the tabs go unused.
//
// What this does
// --------------
// Every new Explorer window is created invisible, asked where it was going, closed again, and its folder is
// opened as a new tab in the Explorer window that was already open. The user sees the existing window come to
// the front with one more tab in it. Holding Shift while opening a folder skips all of this and gives a separate
// window, which is also how a tab is deliberately torn off into a window of its own.
//
// Control Panel lives in an Explorer window too but cannot be a tab, so a window headed there is left alone,
// and a window that only shows Control Panel is never chosen as the place to put tabs.
//
// Where the hooks go
// ------------------
// Two user32 exports, both hot, both with a cheap early exit for everything that is not an Explorer frame:
//
//   CreateWindowExW   Every Explorer window is a top-level "CabinetWClass". When one is created and another
//                     Explorer window already exists, the new one is made fully transparent (layered, alpha 0)
//                     and a helper thread takes over the redirect.
//   ShowWindow        A window headed for a tab must never flash on screen or on the taskbar, so showing it is
//                     refused while it is being created and while it is being redirected.
//
// The helper thread does the rest through the shell's own automation objects (IShellWindows / IWebBrowser2),
// which is the only public way in: it reads the hidden window's folder, sends the tab strip of the target window
// its internal "new tab" command, navigates the tab that appears and then closes the hidden window. Nothing in
// Explorer is patched; if any step fails the hidden window is simply shown, so the user always gets their
// folder one way or the other.
//
#define SP_MOD_ID "explorer-single-window-tabs"
#include "engine/modapi.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <exdisp.h>
#include <servprov.h>

#include <atomic>
#include <new>
#include <string>
#include <vector>

#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "OleAut32.lib")

namespace {

constexpr wchar_t kCabinetClass[] = L"CabinetWClass";         // a File Explorer top-level window
constexpr wchar_t kTabHostClass[] = L"ShellTabWindowClass";   // the child that owns the tab strip

// WM_COMMAND id Explorer's frame uses for "new tab". It is not documented; it is what the tab strip sends
// itself when the "+" button is clicked, and it has been stable from 22H2 through 24H2.
constexpr WPARAM kNewTabCommand = 0xA21B;

// Control Panel's parsing name. A folder under it can only live in a window of its own.
constexpr wchar_t kControlPanel[] = L"::{26EE0668-A00A-44D7-9371-BEB064C98683}";

// Time limits for the helper thread. A window that has not said where it is going after two seconds is shown
// as it is; a tab that has not appeared after three is given up on.
constexpr int kLocationPolls  = 40;    // x 50 ms
constexpr int kNewTabPolls    = 30;    // x 100 ms
constexpr int kSettlePolls    = 10;    // x 100 ms

using CreateWindowExW_t = decltype(&CreateWindowExW);
using ShowWindow_t      = decltype(&ShowWindow);

CreateWindowExW_t g_origCreateWindowExW = nullptr;
ShowWindow_t      g_origShowWindow = nullptr;

// Set between Init and BeforeUninit. The hooks pass everything through while it is clear.
std::atomic<bool> g_active{ false };

// Setting: bring the target window to the front once the folder is in it.
std::atomic<bool> g_bringToFront{ true };

// Set in BeforeUninit: redirects in progress put their window back and finish, and the engine thread waits for
// g_redirectsInFlight to reach zero before the hooks go away.
std::atomic<bool> g_stopping{ false };
std::atomic<int>  g_redirectsInFlight{ 0 };

// True on the thread that is inside the original CreateWindowExW for a window that will be redirected, so the
// ShowWindow calls the shell makes while building it can be refused before the window handle is even known.
thread_local bool t_creatingCabinet = false;

// ---------------------------------------------------------------------------------------------------------------
// Windows being redirected
//
// A window in this list has been created but must not be seen. It leaves the list when its folder has been
// moved into a tab and it is closed, or when the redirect is given up and it is shown after all.
// ---------------------------------------------------------------------------------------------------------------

SRWLOCK           g_hiddenLock = SRWLOCK_INIT;
std::vector<HWND> g_hidden;                 // guarded by g_hiddenLock
std::atomic<int>  g_hiddenCount{ 0 };       // mirror of g_hidden.size(), so the hot path can skip the lock

bool IsHidden(HWND hWnd)
{
    if (g_hiddenCount.load(std::memory_order_relaxed) == 0)
    {
        return false;
    }

    AcquireSRWLockShared(&g_hiddenLock);
    bool found = false;
    for (HWND h : g_hidden)
    {
        if (h == hWnd)
        {
            found = true;
            break;
        }
    }
    ReleaseSRWLockShared(&g_hiddenLock);
    return found;
}

void RememberHidden(HWND hWnd)
{
    AcquireSRWLockExclusive(&g_hiddenLock);
    g_hidden.push_back(hWnd);
    g_hiddenCount.store((int)g_hidden.size(), std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&g_hiddenLock);
}

void ForgetHidden(HWND hWnd)
{
    AcquireSRWLockExclusive(&g_hiddenLock);
    for (size_t i = 0; i < g_hidden.size(); ++i)
    {
        if (g_hidden[i] == hWnd)
        {
            g_hidden.erase(g_hidden.begin() + i);
            break;
        }
    }
    g_hiddenCount.store((int)g_hidden.size(), std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&g_hiddenLock);
}

std::vector<HWND> TakeAllHidden()
{
    std::vector<HWND> taken;
    AcquireSRWLockExclusive(&g_hiddenLock);
    taken.swap(g_hidden);
    g_hiddenCount.store(0, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&g_hiddenLock);
    return taken;
}

bool IsCabinetWindow(HWND hWnd)
{
    wchar_t cls[32];
    return hWnd && GetClassNameW(hWnd, cls, ARRAYSIZE(cls)) && wcscmp(cls, kCabinetClass) == 0;
}

bool ShiftHeld()
{
    return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
}

// Makes a freshly created window invisible without touching WS_VISIBLE: the shell only registers a window with
// IShellWindows once it is shown, and the location of a virtual folder can only be read through that registry.
// Fully transparent and refused by the ShowWindow hook, it is never seen even if the shell shows it another way.
void HideForRedirect(HWND hWnd)
{
    RememberHidden(hWnd);
    SetWindowLongW(hWnd, GWL_EXSTYLE, GetWindowLongW(hWnd, GWL_EXSTYLE) | WS_EX_LAYERED);
    SetLayeredWindowAttributes(hWnd, 0, 0, LWA_ALPHA);
}

// Undoes HideForRedirect. With `show` the window is also shown, for a redirect given up after the shell's own
// ShowWindow calls were already refused.
void Reveal(HWND hWnd, bool show)
{
    ForgetHidden(hWnd);
    if (!IsWindow(hWnd))
    {
        return;
    }

    LONG exStyle = GetWindowLongW(hWnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_LAYERED)
    {
        SetWindowLongW(hWnd, GWL_EXSTYLE, exStyle & ~WS_EX_LAYERED);
    }
    SetWindowPos(hWnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    if (show)
    {
        // The window is no longer in the hidden list, so the hook lets this through.
        ShowWindow(hWnd, SW_SHOW);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// COM helpers
// ---------------------------------------------------------------------------------------------------------------

// The little that is needed of a smart COM pointer, so no call path can leak a reference on an early return.
template <typename T>
class ComRef
{
public:
    ComRef() = default;
    ~ComRef() { Reset(); }

    ComRef(const ComRef& other) : m_p(other.m_p)
    {
        if (m_p) m_p->AddRef();
    }
    ComRef(ComRef&& other) noexcept : m_p(other.m_p)
    {
        other.m_p = nullptr;
    }
    ComRef& operator=(const ComRef& other)
    {
        if (this != &other)
        {
            if (other.m_p) other.m_p->AddRef();
            Reset();
            m_p = other.m_p;
        }
        return *this;
    }
    ComRef& operator=(ComRef&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            m_p = other.m_p;
            other.m_p = nullptr;
        }
        return *this;
    }

    void Reset()
    {
        if (m_p)
        {
            m_p->Release();
            m_p = nullptr;
        }
    }

    T** Put()
    {
        Reset();
        return &m_p;
    }
    T* Get() const { return m_p; }
    T* operator->() const { return m_p; }
    explicit operator bool() const { return m_p != nullptr; }

    template <typename U>
    HRESULT As(ComRef<U>* out) const
    {
        return m_p ? m_p->QueryInterface(IID_PPV_ARGS(out->Put())) : E_POINTER;
    }

private:
    T* m_p = nullptr;
};

// One entry of IShellWindows. Since 22H2 every tab is its own entry; the entries of one window share its HWND.
struct ShellTab
{
    ComRef<IDispatch>    dispatch;
    ComRef<IWebBrowser2> browser;
    HWND                 window = nullptr;
};

std::vector<ShellTab> ListShellTabs()
{
    std::vector<ShellTab> tabs;

    ComRef<IShellWindows> shellWindows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(shellWindows.Put()))))
    {
        return tabs;
    }

    long count = 0;
    if (FAILED(shellWindows->get_Count(&count)))
    {
        return tabs;
    }

    for (long i = 0; i < count; ++i)
    {
        VARIANT index;
        VariantInit(&index);
        index.vt = VT_I4;
        index.lVal = i;

        ShellTab tab;
        if (FAILED(shellWindows->Item(index, tab.dispatch.Put())) || !tab.dispatch)
        {
            continue;
        }
        if (FAILED(tab.dispatch.As(&tab.browser)) || !tab.browser)
        {
            continue;
        }

        SHANDLE_PTR handle = 0;
        if (FAILED(tab.browser->get_HWND(&handle)) || !handle)
        {
            continue;
        }
        tab.window = (HWND)(ULONG_PTR)handle;

        tabs.push_back(std::move(tab));
    }
    return tabs;
}

// Where a tab is, as a parsing name: "C:\Users\me\Documents" for a folder on disk, "::{...}" for a virtual one
// such as This PC or the Recycle Bin. Empty while the tab is still navigating.
std::wstring LocationOf(const ShellTab& tab)
{
    std::wstring location;

    // A folder on disk has a file:// URL.
    BSTR url = nullptr;
    if (SUCCEEDED(tab.browser->get_LocationURL(&url)) && url && *url)
    {
        wchar_t path[MAX_PATH * 2];
        DWORD cch = ARRAYSIZE(path);
        location = SUCCEEDED(PathCreateFromUrlW(url, path, &cch, 0)) ? path : url;
    }
    if (url)
    {
        SysFreeString(url);
    }
    if (!location.empty())
    {
        return location;
    }

    // A virtual folder has no URL; its identity is the PIDL of the folder the active view shows.
    ComRef<IServiceProvider> services;
    if (FAILED(tab.dispatch.As(&services)))
    {
        return location;
    }
    ComRef<IShellBrowser> shellBrowser;
    if (FAILED(services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(shellBrowser.Put()))) || !shellBrowser)
    {
        return location;
    }
    ComRef<IShellView> view;
    if (FAILED(shellBrowser->QueryActiveShellView(view.Put())) || !view)
    {
        return location;
    }
    ComRef<IFolderView> folderView;
    if (FAILED(view.As(&folderView)))
    {
        return location;
    }
    ComRef<IPersistFolder2> folder;
    if (FAILED(folderView->GetFolder(IID_PPV_ARGS(folder.Put()))) || !folder)
    {
        return location;
    }

    PIDLIST_ABSOLUTE pidl = nullptr;
    if (SUCCEEDED(folder->GetCurFolder(&pidl)) && pidl)
    {
        PWSTR name = nullptr;
        if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_DESKTOPABSOLUTEPARSING, &name)) && name)
        {
            location = name;
            CoTaskMemFree(name);
        }
        CoTaskMemFree(pidl);
    }
    return location;
}

// A location that can be opened as a tab. Control Panel and its pages cannot, and neither can a shell: URL that
// only ever names one of those.
bool IsRedirectable(const std::wstring& location)
{
    if (location.empty())
    {
        return false;
    }
    if (StrStrIW(location.c_str(), kControlPanel) != nullptr)
    {
        return false;
    }
    if (location.compare(0, 9, L"shell:::{") == 0)
    {
        return false;
    }
    return true;
}

// How many tabs a window has, and its last one, which is where a new tab is appended.
int CountTabs(const std::vector<ShellTab>& tabs, HWND window, const ShellTab** last)
{
    int count = 0;
    if (last)
    {
        *last = nullptr;
    }
    for (const ShellTab& tab : tabs)
    {
        if (tab.window == window)
        {
            ++count;
            if (last)
            {
                *last = &tab;
            }
        }
    }
    return count;
}

// Sends a tab to a folder. The PIDL form works for everything, including virtual folders; the text form is
// only the fallback for a name the parser does not understand.
bool Navigate(const ShellTab& tab, const std::wstring& location)
{
    VARIANT empty;
    VariantInit(&empty);
    HRESULT hr = E_FAIL;

    PIDLIST_ABSOLUTE pidl = nullptr;
    if (SUCCEEDED(SHParseDisplayName(location.c_str(), nullptr, &pidl, 0, nullptr)) && pidl)
    {
        const UINT size = ILGetSize(pidl);
        SAFEARRAY* bytes = SafeArrayCreateVector(VT_UI1, 0, size);
        if (bytes)
        {
            void* data = nullptr;
            if (SUCCEEDED(SafeArrayAccessData(bytes, &data)) && data)
            {
                memcpy(data, pidl, size);
                SafeArrayUnaccessData(bytes);

                VARIANT where;
                VariantInit(&where);
                where.vt = VT_ARRAY | VT_UI1;
                where.parray = bytes;
                hr = tab.browser->Navigate2(&where, &empty, &empty, &empty, &empty);
            }
            SafeArrayDestroy(bytes);
        }
        CoTaskMemFree(pidl);
    }

    if (FAILED(hr))
    {
        BSTR text = SysAllocString(location.c_str());
        if (text)
        {
            VARIANT where;
            VariantInit(&where);
            where.vt = VT_BSTR;
            where.bstrVal = text;
            hr = tab.browser->Navigate2(&where, &empty, &empty, &empty, &empty);
            SysFreeString(text);
        }
    }

    if (FAILED(hr))
    {
        SP_LogError(L"The new tab could not be sent to %s: 0x%08X", location.c_str(), hr);
    }
    return SUCCEEDED(hr);
}

// ---------------------------------------------------------------------------------------------------------------
// Choosing the window that receives the tab
//
// The visible Explorer windows are collected in Z order, so the one the user worked in last wins; the first that
// shows at least one ordinary folder is the target. The enumeration is cheap and runs first, so the COM part is
// skipped altogether when no Explorer window is open, which is the common case of the very first window.
// ---------------------------------------------------------------------------------------------------------------

struct CabinetList
{
    HWND              exclude;
    std::vector<HWND> windows;
};

BOOL CALLBACK CollectCabinets(HWND hWnd, LPARAM lParam)
{
    CabinetList* list = (CabinetList*)lParam;
    if (hWnd != list->exclude && IsWindowVisible(hWnd) && !IsHidden(hWnd) && IsCabinetWindow(hWnd))
    {
        list->windows.push_back(hWnd);
    }
    return TRUE;
}

HWND FindRedirectTarget(HWND exclude)
{
    CabinetList list{ exclude, {} };
    EnumWindows(CollectCabinets, (LPARAM)&list);
    if (list.windows.empty())
    {
        return nullptr;
    }

    std::vector<ShellTab> tabs = ListShellTabs();
    for (HWND candidate : list.windows)
    {
        for (const ShellTab& tab : tabs)
        {
            if (tab.window == candidate && IsRedirectable(LocationOf(tab)))
            {
                return candidate;
            }
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------------------------------
// The redirect itself, on a helper thread
// ---------------------------------------------------------------------------------------------------------------

struct RedirectJob
{
    HWND newWindow;
    HWND target;
};

// The hidden window's folder, once the shell has navigated it there.
std::wstring WaitForLocation(HWND window)
{
    for (int attempt = 0; attempt < kLocationPolls; ++attempt)
    {
        if (g_stopping.load(std::memory_order_relaxed))
        {
            break;
        }
        Sleep(50);
        if (!IsWindow(window))
        {
            break;
        }
        for (const ShellTab& tab : ListShellTabs())
        {
            if (tab.window == window)
            {
                std::wstring location = LocationOf(tab);
                if (!location.empty())
                {
                    return location;
                }
            }
        }
    }
    return {};
}

// The tab that appears in `target` after the new-tab command: its tab count grows and the new one is last.
ShellTab WaitForNewTab(HWND target, int tabsBefore)
{
    for (int attempt = 0; attempt < kNewTabPolls; ++attempt)
    {
        if (g_stopping.load(std::memory_order_relaxed))
        {
            break;
        }
        Sleep(100);
        std::vector<ShellTab> tabs = ListShellTabs();
        const ShellTab* last = nullptr;
        if (CountTabs(tabs, target, &last) > tabsBefore && last)
        {
            return *last;
        }
    }
    return {};
}

// A new tab starts by navigating to the user's home page. Sending it somewhere else while that is still in
// progress risks the home navigation landing second and winning, so the tab is given a moment to settle.
void WaitForTabToSettle(const ShellTab& tab)
{
    for (int attempt = 0; attempt < kSettlePolls; ++attempt)
    {
        if (g_stopping.load(std::memory_order_relaxed) || !LocationOf(tab).empty())
        {
            return;
        }
        Sleep(100);
    }
}

void BringToFront(HWND window)
{
    if (IsIconic(window))
    {
        ShowWindow(window, SW_RESTORE);
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    AllowSetForegroundWindow(pid);
    SetForegroundWindow(window);
}

// Returns false when the hidden window should be shown after all.
bool TryRedirect(HWND newWindow, HWND target)
{
    std::wstring location = WaitForLocation(newWindow);
    if (location.empty())
    {
        SP_LogDebug(L"Window %p did not report a folder in time; it is shown as it is", newWindow);
        return false;
    }
    if (!IsRedirectable(location))
    {
        SP_LogDebug(L"%s cannot be a tab; window %p is shown as it is", location.c_str(), newWindow);
        return false;
    }

    // The target may have been closed in the meantime.
    if (!IsWindow(target) || target == newWindow)
    {
        target = FindRedirectTarget(newWindow);
    }
    if (!target)
    {
        SP_LogDebug(L"No Explorer window is left to take %s", location.c_str());
        return false;
    }

    HWND tabHost = FindWindowExW(target, nullptr, kTabHostClass, nullptr);
    if (!tabHost)
    {
        SP_LogError(L"Window %p has no tab strip", target);
        return false;
    }

    const int tabsBefore = CountTabs(ListShellTabs(), target, nullptr);
    PostMessageW(tabHost, WM_COMMAND, kNewTabCommand, 0);

    ShellTab tab = WaitForNewTab(target, tabsBefore);
    if (!tab.browser)
    {
        SP_LogError(L"Window %p did not open a new tab", target);
        return false;
    }
    WaitForTabToSettle(tab);

    if (!Navigate(tab, location))
    {
        return false;
    }

    SP_Log(L"%s opened as a tab in window %p", location.c_str(), target);

    if (g_bringToFront.load(std::memory_order_relaxed))
    {
        BringToFront(target);
    }

    // The folder is in its tab; the window the shell made is closed unseen.
    PostMessageW(newWindow, WM_CLOSE, 0, 0);
    ForgetHidden(newWindow);
    return true;
}

DWORD WINAPI RedirectThread(LPVOID param)
{
    RedirectJob* job = (RedirectJob*)param;
    const HWND newWindow = job->newWindow;
    const HWND target = job->target;
    delete job;

    // IShellWindows lives in the shell's main apartment; an apartment of our own is what the calls marshal from.
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
    {
        if (!TryRedirect(newWindow, target))
        {
            Reveal(newWindow, true);
        }
        CoUninitialize();
    }
    else
    {
        Reveal(newWindow, true);
    }

    g_redirectsInFlight.fetch_sub(1, std::memory_order_acq_rel);
    return 0;
}

bool StartRedirect(HWND newWindow, HWND target)
{
    RedirectJob* job = new (std::nothrow) RedirectJob{ newWindow, target };
    if (!job)
    {
        return false;
    }

    g_redirectsInFlight.fetch_add(1, std::memory_order_acq_rel);
    HANDLE hThread = CreateThread(nullptr, 0, RedirectThread, job, 0, nullptr);
    if (!hThread)
    {
        g_redirectsInFlight.fetch_sub(1, std::memory_order_acq_rel);
        delete job;
        SP_LogError(L"The redirect thread could not be started: %lu", GetLastError());
        return false;
    }
    CloseHandle(hThread);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// The hooks
// ---------------------------------------------------------------------------------------------------------------

HWND WINAPI CreateWindowExW_Hook(DWORD dwExStyle, LPCWSTR lpClassName, LPCWSTR lpWindowName, DWORD dwStyle,
                                 int X, int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu,
                                 HINSTANCE hInstance, LPVOID lpParam)
{
    // Class names may be atoms; only a real string can be an Explorer frame.
    const bool isCabinet = lpClassName && !IS_INTRESOURCE(lpClassName) && wcscmp(lpClassName, kCabinetClass) == 0;

    if (!isCabinet || !g_active.load(std::memory_order_relaxed) || ShiftHeld())
    {
        return g_origCreateWindowExW(dwExStyle, lpClassName, lpWindowName, dwStyle, X, Y, nWidth, nHeight,
                                     hWndParent, hMenu, hInstance, lpParam);
    }

    // The first Explorer window is the one the others will go into; it is created normally.
    HWND target = FindRedirectTarget(nullptr);
    if (!target)
    {
        return g_origCreateWindowExW(dwExStyle, lpClassName, lpWindowName, dwStyle, X, Y, nWidth, nHeight,
                                     hWndParent, hMenu, hInstance, lpParam);
    }

    t_creatingCabinet = true;
    HWND hWnd = g_origCreateWindowExW(dwExStyle, lpClassName, lpWindowName, dwStyle, X, Y, nWidth, nHeight,
                                      hWndParent, hMenu, hInstance, lpParam);
    t_creatingCabinet = false;

    if (!hWnd)
    {
        return nullptr;
    }

    SP_LogDebug(L"Explorer window %p will be redirected into %p", hWnd, target);
    HideForRedirect(hWnd);

    if (!StartRedirect(hWnd, target))
    {
        // The shell has not shown the window yet and will do so itself; only the transparency has to go.
        Reveal(hWnd, false);
    }
    return hWnd;
}

BOOL WINAPI ShowWindow_Hook(HWND hWnd, int nCmdShow)
{
    if (g_active.load(std::memory_order_relaxed) && (t_creatingCabinet || IsHidden(hWnd)) && IsCabinetWindow(hWnd))
    {
        SP_LogDebug(L"Showing %p is held back", hWnd);
        return TRUE;
    }
    return g_origShowWindow(hWnd, nCmdShow);
}

// ---------------------------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------------------------

void LoadSettings()
{
    g_bringToFront.store(SP_GetIntSetting(L"BringToFront", 1) != 0, std::memory_order_relaxed);
}

BOOL Init()
{
    LoadSettings();
    g_stopping.store(false, std::memory_order_relaxed);

    if (!SP_HookBegin())
    {
        return FALSE;
    }

    if (!SP_SetExportHook(L"user32.dll", "CreateWindowExW", CreateWindowExW_Hook, &g_origCreateWindowExW) ||
        !SP_SetExportHook(L"user32.dll", "ShowWindow", ShowWindow_Hook, &g_origShowWindow))
    {
        SP_HookAbort();
        SP_LogError(L"The window functions could not be hooked");
        return FALSE;
    }

    if (!SP_HookCommit())
    {
        return FALSE;
    }

    g_active.store(true, std::memory_order_release);
    SP_Log(L"New Explorer windows will open as tabs");
    return TRUE;
}

void BeforeUninit()
{
    g_active.store(false, std::memory_order_relaxed);
    g_stopping.store(true, std::memory_order_relaxed);

    // A redirect in progress notices the flag, shows its window and finishes. It is waited for here, while the
    // code it runs still belongs to a loaded mod.
    for (int i = 0; i < 100 && g_redirectsInFlight.load(std::memory_order_acquire) > 0; ++i)
    {
        Sleep(50);
    }
    if (g_redirectsInFlight.load(std::memory_order_acquire) > 0)
    {
        SP_LogError(L"A redirect was still running after five seconds");
    }

    // Nothing should be left hidden by now; anything that is gets shown rather than lost.
    for (HWND hWnd : TakeAllHidden())
    {
        Reveal(hWnd, true);
    }
}

}   // namespace

SP_MOD_DEFINE(g_modExplorerSingleWindowTabs) =
{
    /* id             */ SP_MOD_ID,
    /* name           */ L"Open new folders as tabs in the File Explorer window that is already open",
    /* basedOn        */ "explorer-single-window-tabs",
    /* originalAuthor */ "ALMAS CP",
    /* targets        */ SP_TARGET_EXPLORER,
    /* minOsBuild     */ 22621,     // File Explorer tabs arrived in 22H2
    /* flags          */ 0,
    /* Init           */ Init,
    /* AfterInit      */ nullptr,
    /* SettingsChanged*/ LoadSettings,
    /* BeforeUninit   */ BeforeUninit,
    /* Uninit         */ nullptr,
};
