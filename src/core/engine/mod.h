#pragma once
//
// mod.h - the contract every mod implements.
//
// A mod is a single translation unit under src/core/mods that defines one SP_Mod and is listed in mod_table.c.
// The engine calls the five callbacks below; everything else a mod needs (hooking, settings, symbols, shared
// input) comes from modapi.h.
//
// Lifecycle, in order:
//
//   Init()            The mod installs its hooks. Return FALSE to refuse to load; nothing else is called.
//   AfterInit()       Every mod has been initialized and all hooks are live. Safe to touch existing windows
//                     here: anything created from now on already goes through the hooks.
//   SettingsChanged() A value under the mod's key changed. Re-read and apply without a restart.
//   BeforeUninit()    Still hooked. Undo what AfterInit did to live windows.
//   Uninit()          Hooks are already removed by the engine. Release the remaining resources.
//
// Only Init is required; the rest may be NULL.
//
// Threading: Init, AfterInit, BeforeUninit, Uninit and SettingsChanged all run on the engine thread, one mod at
// a time, so a mod never needs to guard its own state against them. A detour, by contrast, runs on whatever
// thread called the hooked function, so state shared with a detour needs an interlocked read or a lock.
//
#include <Windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// Which process a mod belongs in. The engine only starts mods whose target matches the host process, so a mod
// never has to check where it is running.
typedef enum SP_ModTarget
{
    SP_TARGET_EXPLORER = 0x0001,   // explorer.exe: taskbar, desktop, File Explorer windows
    SP_TARGET_STARTMENU = 0x0002,  // StartMenuExperienceHost.exe: the Start menu. The explorer engine loads the
                                   // core DLL into it while a mod with this target is enabled (hostinject.c).
} SP_ModTarget;

// Flags that change how the engine treats a mod.
typedef enum SP_ModFlags
{
    // The mod is part of the product rather than something to switch on: the engine loads it without looking
    // for an Enabled value, and the settings window shows no toggle for it. Reaching the settings from the
    // taskbar is the case this exists for; an option to turn that off would only ever strand the user.
    SP_MOD_ALWAYS_ON = 0x0001,
} SP_ModFlags;

typedef struct SP_Mod
{
    // Identity. `id` is the registry key name and the log tag, so it is ASCII, lowercase and stable: renaming it
    // loses the user's settings.
    const char*    id;
    const wchar_t* name;

    // Attribution for the mod this one was adapted from. Both are shown in the settings window and the README.
    // The logic is written from scratch against this engine's API; these fields credit the original idea.
    const char*    basedOn;        // Windhawk mod id, or NULL for an original mod
    const char*    originalAuthor; // NULL when basedOn is NULL

    DWORD targets;        // a combination of SP_ModTarget
    DWORD minOsBuild;     // 0 when the mod runs on every supported build
    DWORD flags;          // a combination of SP_ModFlags

    BOOL (*Init)(void);
    void (*AfterInit)(void);
    void (*SettingsChanged)(void);
    void (*BeforeUninit)(void);
    void (*Uninit)(void);
} SP_Mod;

// Declares the SP_Mod a mod file defines. Put SP_MOD_DECLARE(name) in mod_table.c and
// SP_MOD_DEFINE(name) = { ... }; in the mod itself, so the two sides can never drift apart silently.
#define SP_MOD_DECLARE(symbol) extern const SP_Mod symbol
#define SP_MOD_DEFINE(symbol)  extern "C" const SP_Mod symbol

#ifdef __cplusplus
}
#endif
