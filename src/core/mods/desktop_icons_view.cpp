//
// desktop-icons-view - show the desktop icons as a list, in details, as small icons or as tiles.
//
// Adapted from the idea behind the Windhawk mod "Desktop icons view" (desktop-icons-view) by m417z, which in
// turn grew out of deanm's "List view desktop". The implementation here is written against this engine's API.
//
// How it works
// ------------
// The desktop is an ordinary SysListView32. It is called "FolderView", it sits inside the shell's
// SHELLDLL_DefView, and it answers the same LVM_SETVIEW message as the list in any File Explorer window. The
// shell only ever asks it for the large icon view, but nothing stops anyone else from asking for another one.
//
// Two things have to go with the view change:
//   * The desktop list has LVS_NOSCROLL, which the documentation says cannot be combined with the list view.
//     It is taken off for the other views and put back when the mod is turned off.
//   * The list normally spans the whole virtual screen, every monitor included. A list or details view laid
//     out over several monitors is unreadable, so the window is confined to the work area of the primary
//     monitor (above the taskbar) while the mod is on, and stretched back over its parent when it is off.
//
// Where it hooks
// --------------
// The shell rebuilds the desktop list now and then: at sign-in, after a shell restart, and on some display
// changes. So CreateWindowExW is hooked, the new window is recognised by its class, name and ancestry, and the
// view is applied one second later on a timer. The delay is deliberate: at creation the list has no columns
// and no items yet, and the shell has not finished setting it up; the original mod uses the same wait. The
// timer belongs to the thread that created the window, which is the desktop's own thread, so the messages it
// sends never cross threads.
//
// The lifecycle callbacks run on the engine thread and do cross threads. Everything they send goes through
// SendMessageTimeout, so a hung desktop can never hang the engine with it.
//
// Nothing is written to disk. The shell keeps its own idea of the view (large icons), so the next sign-in
// starts from the user's real setting whatever state the mod left the list in.
//
#define SP_MOD_ID "desktop-icons-view"
#include "engine/modapi.h"

#include <commctrl.h>

#include <atomic>

namespace {

// The view the user asked for, as an LV_VIEW_* value. Written on the engine thread, read on the desktop's.
std::atomic<int> g_view{ LV_VIEW_LIST };

// Width of the name column, in pixels. Only the list and details views have one.
std::atomic<int> g_columnWidth{ 500 };

// FALSE before Init and after BeforeUninit: the hook and a timer still pending then leave the desktop alone.
std::atomic<bool> g_active{ false };

// What the list looked like before the mod first touched it, so it can be put back exactly. -1 = not recorded.
std::atomic<int> g_origNoScroll{ -1 };
std::atomic<int> g_origDoubleBuffer{ -1 };

// The one-shot timer the hook sets. Creating the list twice within the second resets it rather than adding one.
std::atomic<UINT_PTR> g_timerId{ 0 };

constexpr UINT kSettleDelayMs = 1000;
constexpr UINT kSendTimeoutMs = 5000;

constexpr int kDefaultColumnWidth = 500;
constexpr int kMinColumnWidth = 50;
constexpr int kMaxColumnWidth = 4000;

// ---------------------------------------------------------------------------------------------------------------
// Finding the desktop list
// ---------------------------------------------------------------------------------------------------------------

bool ClassIs(HWND hWnd, const wchar_t* name)
{
    wchar_t wszClass[64];
    return GetClassNameW(hWnd, wszClass, ARRAYSIZE(wszClass)) && _wcsicmp(wszClass, name) == 0;
}

// TRUE for the desktop's own list view and nothing else: a SysListView32 named "FolderView", inside an unnamed
// SHELLDLL_DefView, which in turn sits on Progman (the shell window). A File Explorer window has the same two
// inner classes but a different grandparent, and its DefView carries the folder's name.
bool IsDesktopFolderView(HWND hWnd)
{
    if (!hWnd || !ClassIs(hWnd, L"SysListView32"))
    {
        return false;
    }

    wchar_t wszText[64];
    if (!GetWindowTextW(hWnd, wszText, ARRAYSIZE(wszText)) || _wcsicmp(wszText, L"FolderView") != 0)
    {
        return false;
    }

    HWND hView = GetAncestor(hWnd, GA_PARENT);
    if (!hView || !ClassIs(hView, L"SHELLDLL_DefView") || GetWindowTextLengthW(hView) > 0)
    {
        return false;
    }

    HWND hTop = GetAncestor(hView, GA_PARENT);
    if (!hTop)
    {
        return false;
    }

    return ClassIs(hTop, L"Progman") || hTop == GetShellWindow();
}

// The list that exists right now, or NULL. The DefView normally hangs off Progman, but the shell moves it under
// a WorkerW window during some wallpaper transitions, so both places are searched. Only a window of this
// process is returned: a second explorer.exe has a Progman of its own and it is not the desktop.
HWND FindDesktopFolderView()
{
    HWND hView = NULL;

    if (HWND hProgman = FindWindowW(L"Progman", NULL))
    {
        hView = FindWindowExW(hProgman, NULL, L"SHELLDLL_DefView", NULL);
    }

    if (!hView)
    {
        HWND hWorker = NULL;
        while ((hWorker = FindWindowExW(NULL, hWorker, L"WorkerW", NULL)) != NULL)
        {
            hView = FindWindowExW(hWorker, NULL, L"SHELLDLL_DefView", NULL);
            if (hView)
            {
                break;
            }
        }
    }

    HWND hList = hView ? FindWindowExW(hView, NULL, L"SysListView32", L"FolderView") : NULL;
    if (!hList)
    {
        return NULL;
    }

    DWORD processId = 0;
    if (!GetWindowThreadProcessId(hList, &processId) || processId != GetCurrentProcessId())
    {
        return NULL;
    }

    return hList;
}

// ---------------------------------------------------------------------------------------------------------------
// Changing the view
// ---------------------------------------------------------------------------------------------------------------

// A plain SendMessage from the engine thread would wait for as long as the desktop thread takes, and forever if
// it is stuck. This waits a bounded time and gives up on a window that has stopped answering.
LRESULT Send(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    DWORD_PTR result = 0;
    if (!SendMessageTimeoutW(hWnd, msg, wParam, lParam, SMTO_NORMAL | SMTO_ABORTIFHUNG, kSendTimeoutMs, &result))
    {
        SP_LogDebug(L"Message %u to %p was not answered", msg, hWnd);
        return 0;
    }
    return (LRESULT)result;
}

// Where the list goes while a non-icon view is on: the work area of the primary monitor, in the coordinates of
// the list's parent. FALSE if the monitor could not be asked, in which case the window is left where it is.
bool GetPrimaryWorkArea(HWND hParent, RECT* out)
{
    const POINT origin = { 0, 0 };
    HMONITOR hMonitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    if (!hMonitor)
    {
        return false;
    }

    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(hMonitor, &info))
    {
        return false;
    }

    RECT rc = info.rcWork;
    MapWindowPoints(NULL, hParent, (POINT*)&rc, 2);
    *out = rc;
    return true;
}

// Records the two things the mod changes besides the view itself, the first time it sees the list. The shell
// creates every desktop list the same way, so a value recorded on one is right for a later one too.
void RememberOriginalState(HWND hList)
{
    if (g_origNoScroll.load(std::memory_order_relaxed) < 0)
    {
        LONG_PTR style = GetWindowLongPtrW(hList, GWL_STYLE);
        g_origNoScroll.store((style & LVS_NOSCROLL) ? 1 : 0, std::memory_order_relaxed);
    }

    if (g_origDoubleBuffer.load(std::memory_order_relaxed) < 0)
    {
        LRESULT ext = Send(hList, LVM_GETEXTENDEDLISTVIEWSTYLE, 0, 0);
        g_origDoubleBuffer.store((ext & LVS_EX_DOUBLEBUFFER) ? 1 : 0, std::memory_order_relaxed);
    }
}

// Puts the list into `view`. LV_VIEW_ICON means "back to how the shell had it".
void ApplyView(HWND hList, int view)
{
    if (!hList || !IsWindow(hList))
    {
        return;
    }

    HWND hParent = GetAncestor(hList, GA_PARENT);
    if (!hParent)
    {
        return;
    }

    RememberOriginalState(hList);

    LONG_PTR style = GetWindowLongPtrW(hList, GWL_STYLE);
    RECT rc = {};
    bool haveRect = false;

    if (view == LV_VIEW_ICON)
    {
        Send(hList, LVM_SETVIEW, LV_VIEW_ICON, 0);

        if (g_origNoScroll.load(std::memory_order_relaxed) != 0)
        {
            SetWindowLongPtrW(hList, GWL_STYLE, style | LVS_NOSCROLL);
        }

        if (g_origDoubleBuffer.load(std::memory_order_relaxed) != 0)
        {
            Send(hList, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_DOUBLEBUFFER, LVS_EX_DOUBLEBUFFER);
        }

        // The shell sizes the list to fill its parent, which spans every monitor.
        haveRect = GetClientRect(hParent, &rc) != FALSE;
    }
    else
    {
        // The style first: with LVS_NOSCROLL still on, the list view does not lay itself out properly.
        SetWindowLongPtrW(hList, GWL_STYLE, style & ~LVS_NOSCROLL);

        if (Send(hList, LVM_SETVIEW, (WPARAM)view, 0) != 1)
        {
            SP_LogError(L"The desktop list refused view %d", view);
        }

        // Double buffering is switched off for these views; with it on, the list paints over the wallpaper
        // with a solid background. The original mod does the same.
        Send(hList, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_DOUBLEBUFFER, 0);

        const int width = g_columnWidth.load(std::memory_order_relaxed);
        Send(hList, LVM_SETCOLUMNWIDTH, 0, MAKELPARAM(width, 0));

        haveRect = GetPrimaryWorkArea(hParent, &rc);
    }

    if (haveRect)
    {
        SetWindowPos(hList, NULL, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    InvalidateRect(hList, NULL, TRUE);
    UpdateWindow(hList);

    SP_Log(L"Desktop list %p is now in view %d", hList, view);
}

// ---------------------------------------------------------------------------------------------------------------
// The hook
// ---------------------------------------------------------------------------------------------------------------

// Runs on the thread that created the list, one second after it did.
void CALLBACK SettleTimerProc(HWND hWnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
{
    UNREFERENCED_PARAMETER(hWnd);
    UNREFERENCED_PARAMETER(uMsg);
    UNREFERENCED_PARAMETER(dwTime);

    // One shot. The id is cleared only if it is still ours, so a timer set after this one is not forgotten.
    KillTimer(NULL, idEvent);
    UINT_PTR expected = idEvent;
    g_timerId.compare_exchange_strong(expected, 0, std::memory_order_relaxed);

    if (!g_active.load(std::memory_order_relaxed))
    {
        return;
    }

    // Looked up afresh rather than remembered from the hook: the window may have been destroyed and made again
    // in the meantime, and the lookup proves it is still the desktop.
    if (HWND hList = FindDesktopFolderView())
    {
        ApplyView(hList, g_view.load(std::memory_order_relaxed));
    }
    else
    {
        SP_LogDebug(L"The desktop list was gone again before the view could be applied");
    }
}

using CreateWindowExW_t = decltype(&CreateWindowExW);
CreateWindowExW_t g_origCreateWindowExW = nullptr;

HWND WINAPI CreateWindowExW_Hook(DWORD dwExStyle, LPCWSTR lpClassName, LPCWSTR lpWindowName, DWORD dwStyle,
                                 int X, int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu,
                                 HINSTANCE hInstance, LPVOID lpParam)
{
    HWND hWnd = g_origCreateWindowExW(dwExStyle, lpClassName, lpWindowName, dwStyle, X, Y, nWidth, nHeight,
                                      hWndParent, hMenu, hInstance, lpParam);

    // This is one of the most called functions in the process, so the cheap tests come first: the desktop list
    // always has a parent and a name. Only then is the window itself asked about.
    if (!hWnd || !hWndParent || !lpWindowName || !g_active.load(std::memory_order_relaxed))
    {
        return hWnd;
    }

    if (!IsDesktopFolderView(hWnd))
    {
        return hWnd;
    }

    SP_Log(L"The desktop list was created: %p", hWnd);

    // Passing the previous id resets that timer instead of starting another; 0 starts a new one.
    UINT_PTR id = SetTimer(NULL, g_timerId.load(std::memory_order_relaxed), kSettleDelayMs, SettleTimerProc);
    if (id)
    {
        g_timerId.store(id, std::memory_order_relaxed);
    }
    else
    {
        SP_LogError(L"No timer for the new desktop list: %lu", GetLastError());
    }

    return hWnd;
}

// ---------------------------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------------------------

void LoadSettings()
{
    int view = SP_GetIntSetting(L"View", LV_VIEW_LIST);
    if (view != LV_VIEW_DETAILS && view != LV_VIEW_SMALLICON && view != LV_VIEW_LIST && view != LV_VIEW_TILE)
    {
        view = LV_VIEW_LIST;
    }
    g_view.store(view, std::memory_order_relaxed);

    int width = SP_GetIntSetting(L"ColumnWidth", kDefaultColumnWidth);
    if (width < kMinColumnWidth || width > kMaxColumnWidth)
    {
        width = kDefaultColumnWidth;
    }
    g_columnWidth.store(width, std::memory_order_relaxed);

    SP_Log(L"View is %d, column width %d", view, width);
}

BOOL Init()
{
    LoadSettings();
    g_active.store(true, std::memory_order_relaxed);

    if (!SP_HookBegin())
    {
        return FALSE;
    }

    if (!SP_SetExportHook(L"user32.dll", "CreateWindowExW", CreateWindowExW_Hook, &g_origCreateWindowExW))
    {
        SP_HookAbort();
        SP_LogError(L"CreateWindowExW could not be hooked");
        g_active.store(false, std::memory_order_relaxed);
        return FALSE;
    }

    if (!SP_HookCommit())
    {
        g_active.store(false, std::memory_order_relaxed);
        return FALSE;
    }

    return TRUE;
}

void AfterInit()
{
    // The desktop is normally there long before the mod is; a list that exists is changed now, and one that is
    // made later goes through the hook.
    if (HWND hList = FindDesktopFolderView())
    {
        ApplyView(hList, g_view.load(std::memory_order_relaxed));
    }
    else
    {
        SP_Log(L"There is no desktop list yet; waiting for the shell to create one");
    }
}

void SettingsChanged()
{
    LoadSettings();

    if (HWND hList = FindDesktopFolderView())
    {
        ApplyView(hList, g_view.load(std::memory_order_relaxed));
    }
}

void BeforeUninit()
{
    g_active.store(false, std::memory_order_relaxed);

    // A timer set within the last second belongs to the desktop thread and can only be killed there. It checks
    // g_active and does nothing, but it must not run after the engine is gone, so wait for it to fire. The wait
    // is bounded: if the desktop thread is not pumping messages there is nothing to wait for.
    for (int i = 0; i < 30 && g_timerId.load(std::memory_order_relaxed) != 0; ++i)
    {
        Sleep(50);
    }

    if (HWND hList = FindDesktopFolderView())
    {
        ApplyView(hList, LV_VIEW_ICON);
        SP_Log(L"Desktop icons restored");
    }
}

}   // namespace

SP_MOD_DEFINE(g_modDesktopIconsView) =
{
    /* id             */ SP_MOD_ID,
    /* name           */ L"Show the desktop icons as a list, details, small icons or tiles",
    /* basedOn        */ "desktop-icons-view",
    /* originalAuthor */ "m417z",
    /* targets        */ SP_TARGET_EXPLORER,
    /* minOsBuild     */ 0,
    /* flags          */ 0,
    /* Init           */ Init,
    /* AfterInit      */ AfterInit,
    /* SettingsChanged*/ SettingsChanged,
    /* BeforeUninit   */ BeforeUninit,
    /* Uninit         */ nullptr,
};
