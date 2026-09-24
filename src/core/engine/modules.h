#pragma once
//
// modules.h - waiting for a module the shell loads late.
//
// On a cold sign-in the engine starts before the taskbar (Taskbar.View.dll), the shell views
// (Windows.UI.FileExplorer.dll) and the Start menu exist. A mod that hooks one of those cannot fail its Init
// because the library is not there yet; it asks to be called back when it is. The callback runs on a helper
// thread, once, with the module handle; a module that is already loaded is reported at once.
//
// Where the callback runs matters. When the module is noticed by the engine's LoadLibraryExW hook the callback
// runs on the thread that is loading the module, right after the load returns and before the caller has used
// it: that is what lets a mod hook the module's functions before they are first called. That thread is usually
// the taskbar's or a folder window's UI thread, so a callback must install its hooks and return; a callback
// that sleeps or polls for a window on it freezes the taskbar for as long as it waits. A mod that needs to wait
// for something asks with SP_MODULE_WAIT_ON_WORKER, and its callback then runs on a thread of its own, right
// after the load, where it may take as long as it likes.
//
// Every wait is recorded against the mod that asked, so unloading the mod cancels its waits before its code
// goes away.
//
#include <Windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*SP_ModuleLoadedProc)(HMODULE hModule, void* context);

// Flags for SP_WaitForModuleOwnedEx.
#define SP_MODULE_WAIT_ON_WORKER    0x1     // run the callback on a worker thread, never on the loading thread

// Installs the engine's LoadLibraryExW hook, which is what lets a callback run the moment a module is loaded,
// on the loading thread and before the caller has used the module. Called once by the engine after the hook
// layer is up; without it the waits fall back to polling.
void SP_ModulesInitialize(void);

// Calls `callback` when `moduleName` is loaded in this process, polling until `timeoutMs` (0 = forever). Returns
// FALSE only when the wait could not be set up. The module being already loaded counts as success and the
// callback runs on a helper thread right away. Otherwise the callback runs on the loading thread (see above)
// unless `flags` has SP_MODULE_WAIT_ON_WORKER.
BOOL SP_WaitForModuleOwnedEx(const char* owner, const wchar_t* moduleName, DWORD timeoutMs,
                             SP_ModuleLoadedProc callback, void* context, DWORD flags);

BOOL SP_WaitForModuleOwned(const char* owner, const wchar_t* moduleName, DWORD timeoutMs,
                           SP_ModuleLoadedProc callback, void* context);

// Cancels every wait belonging to `owner` and does not return until their threads are gone. NULL cancels all.
void SP_CancelModuleWaits(const char* owner);

#ifdef __cplusplus
}
#endif
