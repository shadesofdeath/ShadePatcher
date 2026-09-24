#include "manager.h"
#include "update.h"
#include "mod.h"
#include "hooks.h"
#include "input.h"
#include "modules.h"
#include "log.h"
#include "settings.h"
#include "symbols.h"
#include "safemode.h"
#include "osversion.h"
#include "config.h"
#include "shortcut.h"
#include "hostinject.h"

#include <tchar.h>

#include <Shlwapi.h>

#define TAG "engine"

// The table of every mod built into this DLL, from mods/mod_table.c.
extern const SP_Mod* const g_modTable[];
extern const int           g_modTableCount;

// Per-mod state the engine keeps. Kept parallel to g_modTable rather than inside SP_Mod, so the mod definitions
// stay constant data.
#define SP_MAX_MODS 128

static BOOL   g_loaded[SP_MAX_MODS];
static HANDLE g_stopEvent = NULL;
static DWORD  g_targets = 0;

// Which process this is, expressed as the SP_ModTarget bits a mod must ask for to run here.
static DWORD DetectTargets(void)
{
    wchar_t wszPath[MAX_PATH];
    if (!GetModuleFileNameW(NULL, wszPath, MAX_PATH))
    {
        return 0;
    }
    PathStripPathW(wszPath);

    if (_wcsicmp(wszPath, L"explorer.exe") == 0)
    {
        return SP_TARGET_EXPLORER;
    }
    if (_wcsicmp(wszPath, L"StartMenuExperienceHost.exe") == 0)
    {
        return SP_TARGET_STARTMENU;
    }
    return 0;
}

// Whether the user has asked for this mod. A mod marked always-on is not the user's to turn off.
static BOOL ModWanted(const SP_Mod* mod)
{
    if (mod->flags & SP_MOD_ALWAYS_ON)
    {
        return TRUE;
    }
    return SP_SettingsIsModEnabled(mod->id);
}

static BOOL ModBelongsHere(const SP_Mod* mod)
{
    if ((mod->targets & g_targets) == 0)
    {
        return FALSE;
    }
    if (mod->minOsBuild != 0 && global_rovi.dwBuildNumber < mod->minOsBuild)
    {
        SP_LOG_INF(TAG, L"%S needs build %lu; this is %lu", mod->id, mod->minOsBuild, global_rovi.dwBuildNumber);
        return FALSE;
    }
    return TRUE;
}

// Explorer only: whether any enabled mod needs the engine in the Start menu's process as well.
static BOOL StartMenuHostWanted(void)
{
    for (int i = 0; i < g_modTableCount; ++i)
    {
        const SP_Mod* mod = g_modTable[i];
        if ((mod->targets & SP_TARGET_STARTMENU) &&
            (mod->minOsBuild == 0 || global_rovi.dwBuildNumber >= mod->minOsBuild) &&
            ModWanted(mod))
        {
            return TRUE;
        }
    }
    return FALSE;
}

static void LoadMod(int index)
{
    const SP_Mod* mod = g_modTable[index];
    if (g_loaded[index])
    {
        return;
    }

    SP_LOG_INF(TAG, L"Loading %S", mod->id);

    if (mod->Init && !mod->Init())
    {
        SP_LOG_ERR(TAG, L"%S refused to start; its hooks are being removed", mod->id);
        // Init may have installed some hooks before it gave up, so the mod is cleaned up as if it had loaded.
        SP_CancelModuleWaits(mod->id);
        SP_RemoveHooksOf(mod->id);
        SP_InputUnsubscribe(mod->id);
        return;
    }

    g_loaded[index] = TRUE;
}

static void UnloadMod(int index)
{
    const SP_Mod* mod = g_modTable[index];
    if (!g_loaded[index])
    {
        return;
    }

    SP_LOG_INF(TAG, L"Unloading %S", mod->id);

    // A callback waiting for a late module must not fire in the middle of the teardown, so the waits go first.
    SP_CancelModuleWaits(mod->id);

    // BeforeUninit runs while the hooks are still live, so a mod can undo what it did to windows that already
    // exist. Only then are the hooks taken away and Uninit called.
    if (mod->BeforeUninit)
    {
        mod->BeforeUninit();
    }

    SP_InputUnsubscribe(mod->id);
    SP_RemoveHooksOf(mod->id);

    if (mod->Uninit)
    {
        mod->Uninit();
    }

    g_loaded[index] = FALSE;
}

// Brings the loaded set in line with the settings, and tells the mods that stayed loaded to re-read theirs.
static void ApplySettings(BOOL notifyChanged)
{
    for (int i = 0; i < g_modTableCount; ++i)
    {
        const SP_Mod* mod = g_modTable[i];
        if (!ModBelongsHere(mod))
        {
            continue;
        }

        BOOL wanted = ModWanted(mod);

        if (wanted && !g_loaded[i])
        {
            LoadMod(i);
            if (g_loaded[i] && mod->AfterInit)
            {
                mod->AfterInit();
            }
        }
        else if (!wanted && g_loaded[i])
        {
            UnloadMod(i);
        }
        else if (wanted && g_loaded[i] && notifyChanged && mod->SettingsChanged)
        {
            mod->SettingsChanged();
        }
    }

    SP_InputReloadPriorities();

    if (g_targets & SP_TARGET_EXPLORER)
    {
        SP_HostInjectUpdate(StartMenuHostWanted());
    }
}

void SP_EngineRun(void)
{
    SP_LogInitialize();

    g_targets = DetectTargets();
    if (g_targets == 0)
    {
        // Loaded into a process no mod targets. This is normal: the proxy DLL is loaded by anything that draws
        // with DXGI, and those processes simply get the pass-through exports and nothing else.
        SP_LOG_DBG(TAG, L"No mods target this process");
        return;
    }

    InitializeGlobalVersionAndUBR();
    const BOOL isShell = (g_targets & SP_TARGET_EXPLORER) != 0;
    SP_LOG_INF(TAG, _T(PRODUCT_NAME) L" engine starting in %s, Windows build %lu.%lu",
               isShell ? L"explorer" : L"the Start menu host",
               global_rovi.dwBuildNumber, global_ubr);

    if (g_modTableCount > SP_MAX_MODS)
    {
        SP_LOG_ERR(TAG, L"The mod table holds %d entries; the limit is %d", g_modTableCount, SP_MAX_MODS);
        return;
    }

    g_stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_stopEvent)
    {
        SP_LOG_ERR(TAG, L"The stop event could not be created: %lu", GetLastError());
        return;
    }

    // If the shell has been restarting over and over, this engine is the likely cause, so it stands down for
    // the session and leaves the user a working desktop to turn the offending mod off from.
    // The Start menu's host restarts on its own schedule, so it does not count as a shell start; it follows the
    // shell's verdict instead.
    BOOL safeMode = isShell ? SP_SafeModeCheckOnStart() : SP_SafeModeIsActive();

    SP_HooksInitialize();
    SP_SymbolsInitialize();
    SP_InputInitialize();
    SP_ModulesInitialize();

    int loadedCount = 0;

    if (safeMode)
    {
        SP_LOG_ERR(TAG, L"Safe mode: no mods will be loaded until the shell is restarted again");
    }
    else
    {
        // Initialize every enabled mod first, then give them all AfterInit. A mod that walks existing windows in
        // AfterInit is then guaranteed that every other mod's hooks are already catching new ones.
        for (int i = 0; i < g_modTableCount; ++i)
        {
            const SP_Mod* mod = g_modTable[i];
            if (ModBelongsHere(mod) && ModWanted(mod))
            {
                LoadMod(i);
                if (g_loaded[i])
                {
                    loadedCount++;
                }
            }
        }

        for (int i = 0; i < g_modTableCount; ++i)
        {
            if (g_loaded[i] && g_modTable[i]->AfterInit)
            {
                g_modTable[i]->AfterInit();
            }
        }

        SP_LOG_INF(TAG, L"%d of %d mod(s) are active", loadedCount, g_modTableCount);

        if (isShell)
        {
            SP_HostInjectUpdate(StartMenuHostWanted());
        }
    }

    // Independent of the mods: one look for a newer release, on a thread of its own.
    if (isShell)
    {
        SP_UpdateStart();
    }

    // The settings should be a Start search away. The installer makes an all-users entry; when the engine runs
    // from a folder that was never installed (sp_inject, the restart helper) a per-user one is made here, pointing
    // at whichever copy of the core DLL this is.
    if (isShell)
    {
        extern HMODULE SP_GetCoreModule(void);
        wchar_t wszCore[MAX_PATH];
        if (GetModuleFileNameW(SP_GetCoreModule(), wszCore, MAX_PATH) && !EnsureSettingsShortcut(wszCore))
        {
            SP_LOG_INF(TAG, L"The Start menu shortcut could not be written");
        }
    }

    // Watch the settings. RegNotifyChangeKeyValue wakes this thread on any change under the product key; the
    // timeout is also what paces the healthy check below.
    ULONGLONG startedAt = GetTickCount64();
    BOOL markedHealthy = FALSE;

    for (;;)
    {
        BOOL changed = SP_SettingsWaitForChange(g_stopEvent, 15000);

        if (WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0)
        {
            break;
        }

        // Surviving this long means the engine did not bring the shell down, so the crash counter is cleared
        // and the next start begins from zero. This runs in safe mode too: that is how a session recovers.
        if (isShell && !markedHealthy && GetTickCount64() - startedAt >= SP_SAFEMODE_HEALTHY_AFTER_MS)
        {
            SP_SafeModeMarkHealthy();
            markedHealthy = TRUE;
        }

        if (changed)
        {
            SP_LogInitialize();     // the log level may have changed too

            // In safe mode the settings are deliberately not acted on. Loading the mod that was just blamed for
            // the crash loop would put the user straight back into it; the change takes effect on the next
            // shell start instead.
            if (!safeMode)
            {
                ApplySettings(TRUE);
            }
        }
    }

    SP_LOG_INF(TAG, L"Engine stopping");

    SP_SettingsStopWatching();

    SP_HostInjectStop();
    if (isShell)
    {
        SP_UpdateStop();
    }

    for (int i = 0; i < g_modTableCount; ++i)
    {
        UnloadMod(i);
    }

    SP_InputShutdown();
    SP_SymbolsShutdown();
    SP_HooksShutdown();

    CloseHandle(g_stopEvent);
    g_stopEvent = NULL;

    SP_LogShutdown();
}

void SP_EngineStop(void)
{
    if (g_stopEvent)
    {
        SetEvent(g_stopEvent);
    }
}
