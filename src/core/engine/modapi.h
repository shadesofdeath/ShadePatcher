#pragma once
//
// modapi.h - everything a mod is allowed to call.
//
// A mod includes this header after defining its own id:
//
//     #define SP_MOD_ID "tray-show-all-icons"
//     #include "engine/modapi.h"
//
// The macros below bind that id to the engine's per-mod services, so a mod never passes its own identity around
// and two mods can never accidentally touch each other's hooks or settings.
//
// This is deliberately close to the Windhawk mod API. Porting the behaviour of a Windhawk mod is then a matter
// of rewriting its logic against these calls rather than reworking its structure:
//
//     Wh_ModInit              ->  the Init callback in the mod's SP_Mod
//     Wh_SetFunctionHook      ->  SP_SetFunctionHook
//     WindhawkUtils::HookSymbols -> SP_HookSymbols
//     Wh_GetIntSetting        ->  SP_GetIntSetting
//     Wh_Log                  ->  SP_Log
//     SetWindowSubclassFromAnyThread -> SP_SetWindowSubclassFromAnyThread
//     "wait for Taskbar.View.dll" loops -> SP_WaitForModule
//
#include <Windows.h>

#include "mod.h"
#include "hooks.h"
#include "log.h"
#include "settings.h"
#include "symbols.h"
#include "input.h"
#include "modules.h"

#ifndef SP_MOD_ID
#error "Define SP_MOD_ID with the mod's id before including modapi.h"
#endif

// --- Logging -----------------------------------------------------------------------------------------------
// Off unless the user turns it on, and the arguments are only evaluated when it is.

#define SP_Log(...)      SP_LogWrite(SP_LOG_INFO,  SP_MOD_ID, __VA_ARGS__)
#define SP_LogDebug(...) SP_LogWrite(SP_LOG_DEBUG, SP_MOD_ID, __VA_ARGS__)
#define SP_LogError(...) SP_LogWrite(SP_LOG_ERROR, SP_MOD_ID, __VA_ARGS__)

// --- Settings ----------------------------------------------------------------------------------------------
// Read from HKCU\Software\ShadePatcher\Mods\<SP_MOD_ID>. A missing value yields the default, so a mod works on a
// fresh install with nothing written yet.

#define SP_GetIntSetting(name, defaultValue) \
    SP_SettingsGetInt(SP_MOD_ID, (name), (defaultValue))

#define SP_GetStringSetting(name, buffer, cch, defaultValue) \
    SP_SettingsGetString(SP_MOD_ID, (name), (buffer), (cch), (defaultValue))

#define SP_SetIntSetting(name, value) \
    SP_SettingsSetInt(SP_MOD_ID, (name), (value))

// --- Hooking -----------------------------------------------------------------------------------------------
// Several hooks belong in one transaction: open it, queue them, commit. A single hook can use
// SP_SetFunctionHookNow, which wraps a transaction around itself.

#define SP_SetFunctionHook(target, detour, original) \
    SP_SetFunctionHookOwned(SP_MOD_ID, (void*)(target), (void*)(detour), (void**)(original))

#define SP_SetFunctionHookNow(target, detour, original) \
    SP_SetSingleFunctionHook(SP_MOD_ID, (void*)(target), (void*)(detour), (void**)(original))

// Hooks an exported function by module and name. Must be called inside a transaction.
#define SP_SetExportHook(moduleName, exportName, detour, original) \
    SP_HookExport(SP_MOD_ID, (moduleName), (exportName), (void*)(detour), (void**)(original))

// --- Symbols -----------------------------------------------------------------------------------------------
// For functions Windows does not export. See symbols.h for how the lookup and its cache work.

#define SP_HookSymbols(module, hooks, count) \
    SP_HookSymbolsOwned(SP_MOD_ID, (module), (hooks), (count))

#define SP_ResolveSymbols(module, hooks, count) \
    SP_ResolveSymbolsOwned(SP_MOD_ID, (module), (hooks), (count))

// --- Shared input ------------------------------------------------------------------------------------------
// Never subclass the desktop or the taskbar directly: subscribe instead, so that two mods wanting the same
// gesture are ordered by the user's priority rather than by which one loaded first. See input.h.

#define SP_SubscribeInput(surface, gesture, handler, context) \
    SP_InputSubscribe(SP_MOD_ID, (surface), (gesture), (handler), (context))

// --- Late modules ------------------------------------------------------------------------------------------
// Taskbar.View.dll, Windows.UI.FileExplorer.dll and the Start menu are loaded after the engine starts on a
// cold sign-in. Ask to be called back when the module is there (at once if it already is) instead of failing
// Init. The wait is cancelled when the mod unloads. See modules.h.
//
// SP_WaitForModule runs the callback on the thread that loads the module (the taskbar's own UI thread, for
// Taskbar.View.dll), so the callback must install its hooks and return: anything that sleeps or polls there
// freezes the taskbar. SP_WaitForModuleOnWorker runs the callback on a thread of its own instead, a moment
// after the load; that is the one to use for a callback that waits for a window to appear.

#define SP_WaitForModule(moduleName, timeoutMs, callback, context) \
    SP_WaitForModuleOwned(SP_MOD_ID, (moduleName), (timeoutMs), (callback), (context))

#define SP_WaitForModuleOnWorker(moduleName, timeoutMs, callback, context) \
    SP_WaitForModuleOwnedEx(SP_MOD_ID, (moduleName), (timeoutMs), (callback), (context), SP_MODULE_WAIT_ON_WORKER)
