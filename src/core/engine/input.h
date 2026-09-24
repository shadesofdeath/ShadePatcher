#pragma once
//
// input.h - shared input surfaces, and the arbitration between mods that want the same gesture.
//
// The problem this solves
// -----------------------
// Several mods want the same gesture. One turns desktop icons on and off with a double click on the desktop;
// another opens Task Manager on the same double click. In Windhawk each mod subclasses the desktop itself, both
// see the click and both act, which is not what the user asked for and is impossible to reason about.
//
// Here the engine owns each shared surface. It installs exactly one subclass per surface, no matter how many
// mods are interested, and hands the gesture to the subscribers in priority order. The first subscriber that
// returns TRUE consumes the gesture and the ones behind it never see it. So two mods on the same gesture are
// resolved by a number the user controls instead of by load order.
//
// Priority comes from the mod's own "InputPriority" setting (lower runs first, default 100), so the settings
// window can expose it as an ordinary option and the user decides who wins.
//
#include <Windows.h>
#include <commctrl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Where the gesture happened.
typedef enum SP_InputSurface
{
    SP_SURFACE_DESKTOP = 0,        // the desktop icon view (SHELLDLL_DefView / SysListView32)
    SP_SURFACE_TASKBAR_EMPTY,      // empty space on the taskbar, away from buttons and the tray
    SP_SURFACE_COUNT
} SP_InputSurface;

// What the gesture was.
typedef enum SP_InputGesture
{
    SP_GESTURE_DOUBLE_CLICK = 0,
    SP_GESTURE_MIDDLE_CLICK,
    SP_GESTURE_WHEEL,
    SP_GESTURE_COUNT
} SP_InputGesture;

typedef struct SP_InputEvent
{
    SP_InputSurface surface;
    SP_InputGesture gesture;
    HWND            hWnd;          // the window the gesture landed on
    POINT           ptScreen;      // cursor position, in screen coordinates
    int             wheelDelta;    // SP_GESTURE_WHEEL only; a multiple of WHEEL_DELTA
    WPARAM          wParam;        // the original message parameters, for a mod that needs the modifier keys
    LPARAM          lParam;
} SP_InputEvent;

// Returns TRUE to consume the gesture: later subscribers do not run and the original window never sees it.
typedef BOOL (*SP_InputHandler)(const SP_InputEvent* event, void* context);

void SP_InputInitialize(void);
void SP_InputShutdown(void);

// Registers interest in one gesture on one surface. A mod may subscribe to several. The first call for a surface
// makes the engine start watching it; the last unsubscribe makes it stop, so a surface nobody wants costs
// nothing.
BOOL SP_InputSubscribe(const char* modId, SP_InputSurface surface, SP_InputGesture gesture,
                       SP_InputHandler handler, void* context);

// Drops every subscription belonging to a mod. Called for you when the mod is unloaded.
void SP_InputUnsubscribe(const char* modId);

// Re-reads every subscriber's InputPriority setting and re-sorts. Called when settings change.
void SP_InputReloadPriorities(void);

// Offers an event to the subscribers. Returns TRUE if one of them consumed it. Called by the surface watchers;
// a mod never calls this itself.
BOOL SP_InputDispatch(const SP_InputEvent* event);

// ---------------------------------------------------------------------------------------------------------------
// Subclassing a window that belongs to another thread
//
// SetWindowSubclass only works from the thread that owns the window, and mods run their lifecycle on the engine
// thread. These carry the call over to the window's thread (with a WH_CALLWNDPROC hook and a sent message, the
// way Windhawk's SetWindowSubclassFromAnyThread does) and wait for the answer. The window must belong to this
// process. Used by the shared surfaces above and available to mods for windows the engine does not own, such as
// a taskbar or a File Explorer view that is not a shared surface.
// ---------------------------------------------------------------------------------------------------------------

BOOL SP_SetWindowSubclassFromAnyThread(HWND hWnd, SUBCLASSPROC proc, UINT_PTR idSubclass, DWORD_PTR refData);
BOOL SP_RemoveWindowSubclassFromAnyThread(HWND hWnd, SUBCLASSPROC proc, UINT_PTR idSubclass);

#ifdef __cplusplus
}
#endif
