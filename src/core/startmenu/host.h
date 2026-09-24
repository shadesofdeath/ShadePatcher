#pragma once
//
// host.h - runs the Start menu inside explorer.exe and routes the Start button and the Windows key to it.
//
// The routing is the one Open-Shell uses on Windows 11 (Open-Shell-Menu, StartMenuDLL.cpp), reimplemented here:
//
//   Start button   A WH_MOUSE hook on the taskbar's thread swallows the left-button messages that land on the
//                  Start button (the hidden "Start" child window of Shell_TrayWnd marks where it is) before XAML
//                  sees them, and asks for our menu instead. A WH_GETMESSAGE hook does the same for pointer
//                  (touch and pen) messages. Shift+click is let through to open the Windows menu when enabled.
//
//   Windows key    twinui.dll claims Win and Ctrl+Esc as shell hotkeys through user32's private
//                  ShellRegisterHotKey (ordinal 2671). With those registrations refused (the mod hooks that
//                  function) or withdrawn (an APC on the registering thread calls UnregisterHotKey), the system
//                  falls back to its classic behaviour: it posts WM_SYSCOMMAND / SC_TASKLIST to the shell window
//                  (Progman). A WH_GETMESSAGE hook on Progman's thread turns that message into ours.
//
// The Windows menu stays reachable: Shift+click is let through to XAML, and with the Windows-key option off the
// SC_TASKLIST message is let through to Progman, which opens it.
//
// Hooks are thread hooks installed from the menu's own thread and removed by it before it exits: Windows ties a
// hook to the thread that set it.
//
#include <Windows.h>

#include "view.h"

namespace sm {

struct HostSettings
{
    bool winKey = true;              // Win and Ctrl+Esc open this menu
    bool startButton = true;         // a click on Start opens this menu
    bool shiftClickWindows = true;   // Shift+click on Start opens the Windows menu
    int middleClick = 0;             // 0: middle clicks on Start stay with Windows; see ViewOptions::middleClick
    bool fullscreenGuard = true;     // the Windows key does nothing while a full-screen app or game is in front
    ViewOptions view;
};

// The menu's options as the settings window stores them, under HKCU\Software\ShadePatcher\Mods\custom-start-menu.
// Shared by the mod and the preview, so the two can never read them differently.
HostSettings ReadHostSettings();

// Starts the menu thread and installs the hooks. Engine thread.
bool HostStart(const HostSettings& settings);

// Applies new settings without restarting anything.
void HostUpdate(const HostSettings& settings);

// Closes the menu, removes the hooks and ends the thread.
void HostStop();

// Puts the default pinned apps back (the settings window's "reset" link). Deletes pinned.json when the menu is
// not running.
void HostResetPins();

// kCommandShowHidden / kCommandClearHistory (view.h). When the menu is not running, the file is deleted.
void HostCommand(WPARAM command);

// Whether the Windows key should be taken from twinui. Read by the ShellRegisterHotKey detour on any thread.
bool HostWantsWinKey();

} // namespace sm
