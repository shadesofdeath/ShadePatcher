#pragma once
//
// safemode.h - the guard that keeps a bad mod from costing the user their desktop.
//
// Everything here runs inside explorer.exe. If a mod crashes the shell while it is starting, Windows restarts
// the shell, the mod runs again, and it crashes again: the user is left with a flashing desktop and no way in to
// turn anything off.
//
// So the engine counts how often the shell has started in quick succession. Past a threshold it assumes it is
// the cause, loads no mods at all and records that it did so. The desktop comes back, the settings window opens
// normally, and the user can turn the offending mod off or uninstall.
//
// The counter is cleared once the shell has been up long enough to be considered healthy, so an ordinary
// sign-out and sign-in never trips it.
//
#include <Windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// Records this shell start and reports whether the engine should stand down. Call once, before any mod loads.
BOOL SP_SafeModeCheckOnStart(void);

// Called by the engine once the shell has stayed up for the healthy interval: clears the counter so the next
// start begins from zero.
void SP_SafeModeMarkHealthy(void);

// Whether the shell's engine stood down this session. Read by the engine in the Start menu's process, which
// follows the shell's verdict instead of counting starts of its own.
BOOL SP_SafeModeIsActive(void);

// How long the shell has to survive before it counts as healthy, in milliseconds.
#define SP_SAFEMODE_HEALTHY_AFTER_MS 60000

#ifdef __cplusplus
}
#endif
