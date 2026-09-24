//
// surfaces.c - the watchers behind the shared input surfaces declared in input.h.
//
// Each surface is started the first time a mod subscribes to it and stopped when the last one leaves, so a
// surface nobody asked for is never touched.
//
// Two things make the desktop harder than it looks, and both were the reason the double click never arrived:
//
//   1. SetWindowSubclass only works from the thread that owns the window. The engine runs on its own thread, so
//      a direct call from here fails silently on comctl32 v6. The request is therefore carried to the desktop's
//      thread with a WH_CALLWNDPROC hook and a sent message, which is the same trick Windhawk's
//      SetWindowSubclassFromAnyThread uses.
//
//   2. On a cold sign-in the engine starts before the desktop exists, and the desktop is rebuilt whenever the
//      wallpaper style changes. So the surface is not attached once: a watcher thread keeps looking for the
//      desktop until it is found, and looks again whenever the window it was attached to goes away.
//
// Only SP_SURFACE_DESKTOP is implemented so far. SP_SURFACE_TASKBAR_EMPTY needs the taskbar's own window layout
// and arrives with the first taskbar mod; until then subscribing to it fails loudly rather than silently doing
// nothing.
//
#include "input.h"
#include "log.h"

#include <commctrl.h>
#include <windowsx.h>
#include <stdlib.h>

#pragma comment(lib, "Comctl32.lib")

#define TAG "surface"

// A subclass id unique to this component; the desktop is shared with other software, so it must not collide.
#define SP_DESKTOP_SUBCLASS_ID 0x53506431   // 'SPd1'

// How often the watcher looks for a desktop it has not got hold of yet.
#define SP_DESKTOP_POLL_MS     750

static HWND   g_desktopView = NULL;      // the SHELLDLL_DefView that was subclassed
static HWND   g_desktopListView = NULL;  // its SysListView32 child
static HANDLE g_desktopWatchThread = NULL;
static HANDLE g_desktopWatchStop = NULL;

// ---------------------------------------------------------------------------------------------------------------
// Subclassing from another thread
// ---------------------------------------------------------------------------------------------------------------

typedef struct SubclassRequest
{
    HWND         hWnd;
    SUBCLASSPROC proc;
    UINT_PTR     id;
    DWORD_PTR    refData;
    BOOL         install;   // FALSE removes
    BOOL         handled;
    BOOL         result;
} SubclassRequest;

// A private message carrying the request. Registered rather than WM_USER based, so it cannot collide with
// anything the shell's own windows understand.
static UINT g_subclassMessage = 0;

// The marker in wParam that says lParam really is one of our requests.
#define SP_SUBCLASS_MAGIC ((WPARAM)0x53506463)   // 'SPdc'

static LRESULT CALLBACK SubclassHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION)
    {
        const CWPSTRUCT* cwp = (const CWPSTRUCT*)lParam;
        if (cwp->message == g_subclassMessage && cwp->wParam == SP_SUBCLASS_MAGIC && cwp->lParam)
        {
            SubclassRequest* request = (SubclassRequest*)cwp->lParam;
            if (!request->handled && request->hWnd == cwp->hwnd)
            {
                // This runs on the window's own thread, which is the one place the call is allowed from.
                request->result = request->install
                    ? SetWindowSubclass(request->hWnd, request->proc, request->id, request->refData)
                    : RemoveWindowSubclass(request->hWnd, request->proc, request->id);
                request->handled = TRUE;
            }
        }
    }
    return CallNextHookEx(NULL, code, wParam, lParam);
}

static BOOL SubclassFromAnyThreadEx(HWND hWnd, SUBCLASSPROC proc, UINT_PTR id, DWORD_PTR refData, BOOL install)
{
    DWORD processId = 0;
    DWORD threadId = GetWindowThreadProcessId(hWnd, &processId);
    if (!threadId || processId != GetCurrentProcessId())
    {
        // Another process's window cannot be subclassed at all. This is what the self test hits, since it runs
        // outside the shell and the desktop it finds is not its own.
        return FALSE;
    }

    if (threadId == GetCurrentThreadId())
    {
        return install ? SetWindowSubclass(hWnd, proc, id, refData) : RemoveWindowSubclass(hWnd, proc, id);
    }

    if (!g_subclassMessage)
    {
        g_subclassMessage = RegisterWindowMessageW(L"ShadePatcher.Surface.Subclass");
    }

    // A thread-local hook needs no module handle when the thread belongs to this process.
    HHOOK hHook = SetWindowsHookExW(WH_CALLWNDPROC, SubclassHookProc, NULL, threadId);
    if (!hHook)
    {
        SP_LOG_ERR(TAG, L"The hook on thread %lu could not be set: %lu", threadId, GetLastError());
        return FALSE;
    }

    SubclassRequest request;
    ZeroMemory(&request, sizeof(request));
    request.hWnd = hWnd;
    request.proc = proc;
    request.id = id;
    request.refData = refData;
    request.install = install;

    // Sent, not posted, so the answer is known when this returns. A timeout keeps a hung desktop thread from
    // taking the engine down with it.
    DWORD_PTR ignored = 0;
    SendMessageTimeoutW(hWnd, g_subclassMessage, SP_SUBCLASS_MAGIC, (LPARAM)&request,
                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &ignored);

    UnhookWindowsHookEx(hHook);

    return request.handled && request.result;
}

static BOOL SubclassFromAnyThread(HWND hWnd, SUBCLASSPROC proc, UINT_PTR id, BOOL install)
{
    return SubclassFromAnyThreadEx(hWnd, proc, id, 0, install);
}

BOOL SP_SetWindowSubclassFromAnyThread(HWND hWnd, SUBCLASSPROC proc, UINT_PTR idSubclass, DWORD_PTR refData)
{
    return SubclassFromAnyThreadEx(hWnd, proc, idSubclass, refData, TRUE);
}

BOOL SP_RemoveWindowSubclassFromAnyThread(HWND hWnd, SUBCLASSPROC proc, UINT_PTR idSubclass)
{
    return SubclassFromAnyThreadEx(hWnd, proc, idSubclass, 0, FALSE);
}

// ---------------------------------------------------------------------------------------------------------------
// Desktop
//
// The desktop icons live in a SysListView32 inside a SHELLDLL_DefView, which sits under either Progman or one of
// the WorkerW windows depending on whether a slideshow wallpaper is active. Both parents have to be searched.
// ---------------------------------------------------------------------------------------------------------------

static HWND FindDesktopShellView(void)
{
    HWND hProgman = FindWindowW(L"Progman", NULL);
    if (hProgman)
    {
        HWND hView = FindWindowExW(hProgman, NULL, L"SHELLDLL_DefView", NULL);
        if (hView)
        {
            return hView;
        }
    }

    HWND hWorker = NULL;
    while ((hWorker = FindWindowExW(NULL, hWorker, L"WorkerW", NULL)) != NULL)
    {
        HWND hView = FindWindowExW(hWorker, NULL, L"SHELLDLL_DefView", NULL);
        if (hView)
        {
            return hView;
        }
    }

    return NULL;
}

static void DispatchDesktopGesture(HWND hWnd, SP_InputGesture gesture, WPARAM wParam, LPARAM lParam,
                                   BOOL* consumed)
{
    SP_InputEvent ev;
    ZeroMemory(&ev, sizeof(ev));
    ev.surface = SP_SURFACE_DESKTOP;
    ev.gesture = gesture;
    ev.hWnd = hWnd;
    ev.wParam = wParam;
    ev.lParam = lParam;

    if (gesture == SP_GESTURE_WHEEL)
    {
        ev.wheelDelta = GET_WHEEL_DELTA_WPARAM(wParam);
        ev.ptScreen.x = GET_X_LPARAM(lParam);
        ev.ptScreen.y = GET_Y_LPARAM(lParam);
    }
    else
    {
        GetCursorPos(&ev.ptScreen);
    }

    *consumed = SP_InputDispatch(&ev);
}

// The view itself only sees the gestures that miss the list view: the wheel, and every click while the list is
// hidden (which is what the icon toggle does). The view's window class has no CS_DBLCLKS, so it never receives
// WM_LBUTTONDBLCLK; the double click is recognised here from two presses inside the system's double click time
// and distance, the same rule the window manager applies.
static DWORD g_viewLastClickTime = 0;
static POINT g_viewLastClickPos = { 0, 0 };

static LRESULT CALLBACK DesktopSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                            UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    UNREFERENCED_PARAMETER(uIdSubclass);
    UNREFERENCED_PARAMETER(dwRefData);

    switch (uMsg)
    {
        case WM_LBUTTONDOWN:
        {
            DWORD now = GetTickCount();
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            BOOL isDouble = (now - g_viewLastClickTime) <= GetDoubleClickTime() &&
                            abs(pt.x - g_viewLastClickPos.x) <= GetSystemMetrics(SM_CXDOUBLECLK) &&
                            abs(pt.y - g_viewLastClickPos.y) <= GetSystemMetrics(SM_CYDOUBLECLK);
            g_viewLastClickTime = isDouble ? 0 : now;
            g_viewLastClickPos = pt;

            if (isDouble)
            {
                BOOL consumed = FALSE;
                DispatchDesktopGesture(hWnd, SP_GESTURE_DOUBLE_CLICK, wParam, lParam, &consumed);
                if (consumed)
                {
                    return 0;
                }
            }
            break;
        }

        case WM_LBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        {
            BOOL consumed = FALSE;
            DispatchDesktopGesture(hWnd, (uMsg == WM_LBUTTONDBLCLK) ? SP_GESTURE_DOUBLE_CLICK : SP_GESTURE_MIDDLE_CLICK,
                                   wParam, lParam, &consumed);
            if (consumed)
            {
                return 0;
            }
            break;
        }

        case WM_MOUSEWHEEL:
        {
            BOOL consumed = FALSE;
            DispatchDesktopGesture(hWnd, SP_GESTURE_WHEEL, wParam, lParam, &consumed);
            if (consumed)
            {
                return 0;
            }
            break;
        }

        case WM_NCDESTROY:
        {
            // The desktop is rebuilt when the wallpaper style changes; forget the window so the watcher finds
            // the new one. The subclass goes away with the window, nothing needs removing.
            if (hWnd == g_desktopView)
            {
                InterlockedExchangePointer((PVOID volatile*)&g_desktopView, NULL);
                InterlockedExchangePointer((PVOID volatile*)&g_desktopListView, NULL);
                SP_LOG_INF(TAG, L"The desktop view went away; watching for the next one");
            }
            break;
        }
    }

    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

// A double click on the icon area is delivered to the list view, not to the view, so both are subclassed. The
// list view only forwards the gesture when the click landed on empty space; a click on an icon is left alone.
static LRESULT CALLBACK DesktopListViewSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                                    UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    UNREFERENCED_PARAMETER(uIdSubclass);
    UNREFERENCED_PARAMETER(dwRefData);

    switch (uMsg)
    {
        case WM_LBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        {
            LVHITTESTINFO hit;
            ZeroMemory(&hit, sizeof(hit));
            hit.pt.x = GET_X_LPARAM(lParam);
            hit.pt.y = GET_Y_LPARAM(lParam);

            // Hitting an item means the user aimed at an icon, so the gesture belongs to the shell.
            if (ListView_HitTest(hWnd, &hit) == -1)
            {
                BOOL consumed = FALSE;
                DispatchDesktopGesture(hWnd, (uMsg == WM_LBUTTONDBLCLK) ? SP_GESTURE_DOUBLE_CLICK : SP_GESTURE_MIDDLE_CLICK,
                                       wParam, lParam, &consumed);
                if (consumed)
                {
                    return 0;
                }
            }
            break;
        }

        case WM_MOUSEWHEEL:
        {
            BOOL consumed = FALSE;
            DispatchDesktopGesture(hWnd, SP_GESTURE_WHEEL, wParam, lParam, &consumed);
            if (consumed)
            {
                return 0;
            }
            break;
        }

        case WM_NCDESTROY:
        {
            if (hWnd == g_desktopListView)
            {
                InterlockedExchangePointer((PVOID volatile*)&g_desktopListView, NULL);
            }
            break;
        }
    }

    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

// One attempt to get hold of the desktop. Returns TRUE when the surface is attached after the call.
static BOOL AttachDesktop(void)
{
    if (g_desktopView && IsWindow(g_desktopView))
    {
        // Attached already. The list view can be created after the view, so it is picked up late if needed.
        if (!g_desktopListView)
        {
            HWND hList = FindWindowExW(g_desktopView, NULL, L"SysListView32", NULL);
            if (hList && SubclassFromAnyThread(hList, DesktopListViewSubclassProc, SP_DESKTOP_SUBCLASS_ID, TRUE))
            {
                g_desktopListView = hList;
                SP_LOG_INF(TAG, L"Watching the desktop icon list %p", hList);
            }
        }
        return TRUE;
    }

    g_desktopView = NULL;
    g_desktopListView = NULL;

    HWND hView = FindDesktopShellView();
    if (!hView)
    {
        return FALSE;
    }

    if (!SubclassFromAnyThread(hView, DesktopSubclassProc, SP_DESKTOP_SUBCLASS_ID, TRUE))
    {
        SP_LOG_ERR(TAG, L"The desktop view %p could not be subclassed", hView);
        return FALSE;
    }
    g_desktopView = hView;

    HWND hList = FindWindowExW(hView, NULL, L"SysListView32", NULL);
    if (hList && SubclassFromAnyThread(hList, DesktopListViewSubclassProc, SP_DESKTOP_SUBCLASS_ID, TRUE))
    {
        g_desktopListView = hList;
    }

    SP_LOG_INF(TAG, L"Watching the desktop (view %p, list %p)", g_desktopView, g_desktopListView);
    return TRUE;
}

static void DetachDesktop(void)
{
    HWND hList = (HWND)InterlockedExchangePointer((PVOID volatile*)&g_desktopListView, NULL);
    HWND hView = (HWND)InterlockedExchangePointer((PVOID volatile*)&g_desktopView, NULL);

    if (hList && IsWindow(hList))
    {
        SubclassFromAnyThread(hList, DesktopListViewSubclassProc, SP_DESKTOP_SUBCLASS_ID, FALSE);
    }
    if (hView && IsWindow(hView))
    {
        SubclassFromAnyThread(hView, DesktopSubclassProc, SP_DESKTOP_SUBCLASS_ID, FALSE);
        SP_LOG_INF(TAG, L"Stopped watching the desktop");
    }
}

// Keeps the surface attached for as long as someone wants it: finds the desktop when it appears, and finds it
// again after it is rebuilt.
static DWORD WINAPI DesktopWatchThread(LPVOID lpParameter)
{
    UNREFERENCED_PARAMETER(lpParameter);

    int missed = 0;
    for (;;)
    {
        if (WaitForSingleObject(g_desktopWatchStop, SP_DESKTOP_POLL_MS) != WAIT_TIMEOUT)
        {
            return 0;
        }

        if (AttachDesktop())
        {
            missed = 0;
            continue;
        }

        // Say so once in a while rather than on every miss, so a session without a desktop does not flood
        // the log.
        if (++missed == 40)
        {
            SP_LOG_DBG(TAG, L"Still waiting for the desktop view");
            missed = 0;
        }
    }
}

static BOOL StartDesktop(void)
{
    if (g_desktopWatchThread)
    {
        return TRUE;
    }

    g_desktopWatchStop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_desktopWatchStop)
    {
        return FALSE;
    }

    // The first attempt is made right here, so a desktop that already exists is watched before the subscriber's
    // Init returns; the thread only covers the cases where it does not exist yet or goes away later.
    if (!AttachDesktop())
    {
        SP_LOG_INF(TAG, L"The desktop view is not up yet; it will be watched as soon as it appears");
    }

    g_desktopWatchThread = CreateThread(NULL, 0, DesktopWatchThread, NULL, 0, NULL);
    if (!g_desktopWatchThread)
    {
        CloseHandle(g_desktopWatchStop);
        g_desktopWatchStop = NULL;
        DetachDesktop();
        return FALSE;
    }

    return TRUE;
}

static void StopDesktop(void)
{
    if (g_desktopWatchStop)
    {
        SetEvent(g_desktopWatchStop);
    }
    if (g_desktopWatchThread)
    {
        WaitForSingleObject(g_desktopWatchThread, 5000);
        CloseHandle(g_desktopWatchThread);
        g_desktopWatchThread = NULL;
    }
    if (g_desktopWatchStop)
    {
        CloseHandle(g_desktopWatchStop);
        g_desktopWatchStop = NULL;
    }

    DetachDesktop();
}

// ---------------------------------------------------------------------------------------------------------------
// Dispatch table
// ---------------------------------------------------------------------------------------------------------------

BOOL SP_SurfaceStart(SP_InputSurface surface)
{
    switch (surface)
    {
        case SP_SURFACE_DESKTOP:
            return StartDesktop();

        case SP_SURFACE_TASKBAR_EMPTY:
            SP_LOG_ERR(TAG, L"The taskbar surface is not implemented yet");
            return FALSE;

        default:
            return FALSE;
    }
}

void SP_SurfaceStop(SP_InputSurface surface)
{
    switch (surface)
    {
        case SP_SURFACE_DESKTOP:
            StopDesktop();
            break;

        default:
            break;
    }
}
