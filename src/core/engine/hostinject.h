#pragma once
//
// hostinject.h - brings the engine into the Start menu's process.
//
// The Start menu is not part of explorer.exe: it is drawn by StartMenuExperienceHost.exe, a separate process that
// runs as the user at medium integrity (not in an AppContainer). A mod that changes the Start menu itself has to
// run there, so while at least one enabled mod targets SP_TARGET_STARTMENU the explorer engine watches for that
// process and loads this same core DLL into it. The DLL starts an engine of its own there, which loads the
// Start-menu mods and follows the settings exactly like the shell's engine does.
//
// Nothing is ever unloaded from the Start menu: turning a mod off unloads it through the settings watch of the
// engine in that process, and the process itself is short-lived enough that the engine goes with it.
//
#include <Windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// Explorer's engine thread. Starts or stops the watcher to match whether a Start-menu mod is wanted.
void SP_HostInjectUpdate(BOOL wanted);

// Stops the watcher, if it runs. Safe to call at any time.
void SP_HostInjectStop(void);

#ifdef __cplusplus
}
#endif
