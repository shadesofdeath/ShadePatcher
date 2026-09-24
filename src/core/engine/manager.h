#pragma once
//
// manager.h - the mod host.
//
// One instance runs inside each patched process. It decides which mods belong there, drives their lifecycle and
// keeps watching the settings so that turning a mod on or off takes effect without restarting the shell.
//
#include <Windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// Starts the engine on the calling thread and returns once the mods are up. The caller is a worker thread
// created by DllMain; nothing here may run under the loader lock.
void SP_EngineRun(void);

// Asks the engine to unload its mods and return from SP_EngineRun.
void SP_EngineStop(void);

#ifdef __cplusplus
}
#endif
