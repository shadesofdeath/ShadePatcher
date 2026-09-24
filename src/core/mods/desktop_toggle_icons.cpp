//
// desktop-toggle-icons - hide and show the desktop icons with a double click on empty desktop.
//
// Adapted from the idea behind the Windhawk mod "ZenDesktop: Desktop Icon Toggle" (zen-desktop-toggle-icons) by
// Lanbo. The implementation here is written against this engine's API.
//
// The gesture is not taken directly. The mod subscribes to the shared desktop surface, so if another mod also
// wants the desktop double click the two are ordered by the user's InputPriority setting and only one of them
// acts. See docs/engine.md.
//
// Hiding is done by hiding the icon list view itself rather than by changing the shell's "show desktop icons"
// setting. Nothing is written anywhere, so whatever state the desktop is left in, the next sign-in starts from
// the user's real setting.
//
#define SP_MOD_ID "desktop-toggle-icons"
#include "engine/modapi.h"

#include <atomic>

namespace {

// TRUE while the mod is the reason the icons are hidden. Written on the engine thread and on the desktop's
// thread, read by both, so it is atomic.
std::atomic<bool> g_hiddenByUs{ false };

// The event's window is either the icon list view or the view that contains it, depending on where the click
// landed. Both lead to the same list view.
HWND GetListViewFrom(HWND hWnd)
{
    wchar_t wszClass[64];
    if (GetClassNameW(hWnd, wszClass, ARRAYSIZE(wszClass)) && _wcsicmp(wszClass, L"SysListView32") == 0)
    {
        return hWnd;
    }
    return FindWindowExW(hWnd, NULL, L"SysListView32", NULL);
}

BOOL OnDoubleClick(const SP_InputEvent* event, void* context)
{
    UNREFERENCED_PARAMETER(context);

    HWND hListView = GetListViewFrom(event->hWnd);
    if (!hListView)
    {
        SP_LogDebug(L"No icon list view behind %p", event->hWnd);
        return FALSE;   // not ours; let another subscriber try
    }

    BOOL visible = IsWindowVisible(hListView);
    ShowWindow(hListView, visible ? SW_HIDE : SW_SHOW);
    g_hiddenByUs.store(visible, std::memory_order_relaxed);

    SP_Log(L"Desktop icons are now %s", visible ? L"hidden" : L"shown");

    return TRUE;    // consumed: nobody behind this mod sees the double click
}

BOOL Init()
{
    if (!SP_SubscribeInput(SP_SURFACE_DESKTOP, SP_GESTURE_DOUBLE_CLICK, OnDoubleClick, nullptr))
    {
        SP_LogError(L"The desktop surface could not be watched");
        return FALSE;
    }
    return TRUE;
}

void BeforeUninit()
{
    // Leaving the desktop empty after the mod is turned off would look like a broken shell, so the icons are put
    // back if this mod is why they are gone.
    if (!g_hiddenByUs.exchange(false, std::memory_order_relaxed))
    {
        return;
    }

    HWND hProgman = FindWindowW(L"Progman", NULL);
    HWND hView = hProgman ? FindWindowExW(hProgman, NULL, L"SHELLDLL_DefView", NULL) : NULL;
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

    HWND hListView = hView ? GetListViewFrom(hView) : NULL;
    if (hListView)
    {
        ShowWindow(hListView, SW_SHOW);
        SP_Log(L"Desktop icons restored");
    }
}

}   // namespace

SP_MOD_DEFINE(g_modDesktopToggleIcons) =
{
    /* id             */ SP_MOD_ID,
    /* name           */ L"Toggle desktop icons with a double click",
    /* basedOn        */ "zen-desktop-toggle-icons",
    /* originalAuthor */ "Lanbo",
    /* targets        */ SP_TARGET_EXPLORER,
    /* minOsBuild     */ 0,
    /* flags          */ 0,
    /* Init           */ Init,
    /* AfterInit      */ nullptr,
    /* SettingsChanged*/ nullptr,
    /* BeforeUninit   */ BeforeUninit,
    /* Uninit         */ nullptr,
};
