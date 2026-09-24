#include "modules.h"
#include "hooks.h"
#include "log.h"

#include <Shlwapi.h>
#include <string.h>

#pragma comment(lib, "Shlwapi.lib")

#define TAG "modules"

// Two ways to notice a module. The poll is the safety net; the LoadLibraryExW hook is what makes the callback
// run at the right moment. Taskbar.View.dll builds the taskbar, the tray and their icons in the milliseconds
// after it is loaded, and a mod that styles those as they are created must have its hooks in before that,
// which a poll cannot guarantee. So the hook runs the callback on the loading thread, right after the load
// returns and before the caller has used the module, the way Windhawk mods do it. That thread is the taskbar's
// own, so a callback must be quick; a wait registered with SP_MODULE_WAIT_ON_WORKER is left to the poll thread
// instead, which the hook wakes so that it runs the callback where blocking does no harm.
#define SP_MODULE_POLL_MS 500

#define SP_MAX_MODULE_WAITS 32

typedef struct ModuleWait
{
    const char*         owner;
    wchar_t             moduleName[64];
    DWORD               timeoutMs;
    SP_ModuleLoadedProc callback;
    void*               context;
    HANDLE              hThread;
    HANDLE              hStop;
    HANDLE              hWake;       // set by the loader hook for waits that must not run on its thread
    DWORD               flags;
    BOOL                inUse;
    volatile LONG       fired;       // set by whichever of the poll and the hook gets there first
} ModuleWait;

static ModuleWait       g_waits[SP_MAX_MODULE_WAITS];
static CRITICAL_SECTION g_lock;
static BOOL             g_initialized = FALSE;

typedef HMODULE (WINAPI* LoadLibraryExW_t)(LPCWSTR, HANDLE, DWORD);
static LoadLibraryExW_t g_origLoadLibraryExW = NULL;

static void EnsureInitialized(void)
{
    // Called before any wait is registered, always from the engine thread, so a plain flag is enough.
    if (!g_initialized)
    {
        InitializeCriticalSection(&g_lock);
        g_initialized = TRUE;
    }
}

// Runs the callback once, whichever path noticed the module. Returns FALSE when it already ran.
static BOOL Fire(ModuleWait* wait, HMODULE hModule, const wchar_t* how)
{
    if (InterlockedCompareExchange(&wait->fired, 1, 0) != 0)
    {
        return FALSE;
    }
    SP_LOG_DBG(TAG, L"%s is loaded (%s); telling %S", wait->moduleName, how, wait->owner);
    wait->callback(hModule, wait->context);
    return TRUE;
}

static HMODULE WINAPI LoadLibraryExW_Hook(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
{
    HMODULE hModule = g_origLoadLibraryExW(lpLibFileName, hFile, dwFlags);

    // Only a real load of a module somebody is waiting for is interesting; resource-only mappings are not code.
    if (!hModule || !lpLibFileName || !g_initialized ||
        (dwFlags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE)))
    {
        return hModule;
    }

    const wchar_t* name = PathFindFileNameW(lpLibFileName);

    // The matching waits are collected under the lock and called outside it: a callback installs hooks and may
    // register further waits.
    ModuleWait* matched[SP_MAX_MODULE_WAITS];
    int n = 0;

    EnterCriticalSection(&g_lock);
    for (int i = 0; i < SP_MAX_MODULE_WAITS; ++i)
    {
        if (g_waits[i].inUse && !g_waits[i].fired && _wcsicmp(g_waits[i].moduleName, name) == 0)
        {
            matched[n++] = &g_waits[i];
        }
    }
    LeaveCriticalSection(&g_lock);

    for (int i = 0; i < n; ++i)
    {
        if (matched[i]->flags & SP_MODULE_WAIT_ON_WORKER)
        {
            // Not from here: the poll thread is woken and fires the callback on its own time.
            SetEvent(matched[i]->hWake);
            continue;
        }
        if (Fire(matched[i], hModule, L"loader hook"))
        {
            SetEvent(matched[i]->hStop);    // the poll thread has nothing left to do
        }
    }

    return hModule;
}

void SP_ModulesInitialize(void)
{
    EnsureInitialized();

    // Owned by the engine (NULL), so it stays for the life of the engine and goes with the final hook sweep.
    // A failure here only costs the early notification; the poll still works.
    if (!SP_HookBegin())
    {
        return;
    }
    if (!SP_HookExport(NULL, L"kernelbase.dll", "LoadLibraryExW", (void*)LoadLibraryExW_Hook, (void**)&g_origLoadLibraryExW) ||
        !SP_HookCommit())
    {
        SP_HookAbort();
        g_origLoadLibraryExW = NULL;
        SP_LOG_ERR(TAG, L"LoadLibraryExW could not be hooked; late modules are noticed by polling only");
    }
}

static DWORD WINAPI WaitThread(LPVOID lpParameter)
{
    ModuleWait* wait = (ModuleWait*)lpParameter;
    ULONGLONG deadline = wait->timeoutMs ? GetTickCount64() + wait->timeoutMs : 0;

    for (;;)
    {
        HMODULE hModule = GetModuleHandleW(wait->moduleName);
        if (hModule)
        {
            Fire(wait, hModule, L"poll");
            return 0;
        }
        if (wait->fired)
        {
            return 0;   // the loader hook got there first
        }

        if (deadline && GetTickCount64() >= deadline)
        {
            SP_LOG_ERR(TAG, L"%s never loaded; %S gives up waiting", wait->moduleName, wait->owner);
            return 0;
        }

        HANDLE handles[2] = { wait->hStop, wait->hWake };
        if (WaitForMultipleObjects(2, handles, FALSE, SP_MODULE_POLL_MS) == WAIT_OBJECT_0)
        {
            return 0;   // cancelled
        }
        // Either the poll interval passed or the loader hook says the module is there now; both re-check.
    }
}

BOOL SP_WaitForModuleOwned(const char* owner, const wchar_t* moduleName, DWORD timeoutMs,
                           SP_ModuleLoadedProc callback, void* context)
{
    return SP_WaitForModuleOwnedEx(owner, moduleName, timeoutMs, callback, context, 0);
}

BOOL SP_WaitForModuleOwnedEx(const char* owner, const wchar_t* moduleName, DWORD timeoutMs,
                             SP_ModuleLoadedProc callback, void* context, DWORD flags)
{
    if (!owner || !moduleName || !callback || wcslen(moduleName) >= 64)
    {
        return FALSE;
    }

    EnsureInitialized();
    EnterCriticalSection(&g_lock);

    ModuleWait* wait = NULL;
    for (int i = 0; i < SP_MAX_MODULE_WAITS; ++i)
    {
        if (!g_waits[i].inUse)
        {
            wait = &g_waits[i];
            break;
        }
        // A finished wait whose slot was never reclaimed: its thread has ended, so it can be reused.
        if (g_waits[i].hThread && WaitForSingleObject(g_waits[i].hThread, 0) == WAIT_OBJECT_0)
        {
            CloseHandle(g_waits[i].hThread);
            CloseHandle(g_waits[i].hStop);
            CloseHandle(g_waits[i].hWake);
            ZeroMemory(&g_waits[i], sizeof(g_waits[i]));
            wait = &g_waits[i];
            break;
        }
    }

    if (!wait)
    {
        LeaveCriticalSection(&g_lock);
        SP_LOG_ERR(TAG, L"%S: no room for another module wait", owner);
        return FALSE;
    }

    ZeroMemory(wait, sizeof(*wait));
    wait->owner = owner;
    wcscpy_s(wait->moduleName, 64, moduleName);
    wait->timeoutMs = timeoutMs;
    wait->callback = callback;
    wait->context = context;
    wait->flags = flags;
    wait->hStop = CreateEventW(NULL, TRUE, FALSE, NULL);
    wait->hWake = CreateEventW(NULL, TRUE, FALSE, NULL);
    wait->inUse = TRUE;

    if (!wait->hStop || !wait->hWake)
    {
        if (wait->hStop)
        {
            CloseHandle(wait->hStop);
        }
        if (wait->hWake)
        {
            CloseHandle(wait->hWake);
        }
        ZeroMemory(wait, sizeof(*wait));
        LeaveCriticalSection(&g_lock);
        return FALSE;
    }

    wait->hThread = CreateThread(NULL, 0, WaitThread, wait, 0, NULL);
    if (!wait->hThread)
    {
        CloseHandle(wait->hStop);
        CloseHandle(wait->hWake);
        ZeroMemory(wait, sizeof(*wait));
        LeaveCriticalSection(&g_lock);
        return FALSE;
    }

    LeaveCriticalSection(&g_lock);
    SP_LOG_DBG(TAG, L"%S is waiting for %s", owner, moduleName);
    return TRUE;
}

void SP_CancelModuleWaits(const char* owner)
{
    if (!g_initialized)
    {
        return;
    }

    // Stopped outside the lock: a callback that is running right now may itself register a wait.
    HANDLE threads[SP_MAX_MODULE_WAITS];
    HANDLE stops[SP_MAX_MODULE_WAITS];
    HANDLE wakes[SP_MAX_MODULE_WAITS];
    int n = 0;

    EnterCriticalSection(&g_lock);
    for (int i = 0; i < SP_MAX_MODULE_WAITS; ++i)
    {
        if (g_waits[i].inUse && (!owner || strcmp(g_waits[i].owner, owner) == 0))
        {
            threads[n] = g_waits[i].hThread;
            stops[n] = g_waits[i].hStop;
            wakes[n] = g_waits[i].hWake;
            n++;
            ZeroMemory(&g_waits[i], sizeof(g_waits[i]));
        }
    }
    LeaveCriticalSection(&g_lock);

    for (int i = 0; i < n; ++i)
    {
        SetEvent(stops[i]);
        // The callback may be in the middle of installing hooks; give it a moment rather than tearing the mod
        // down underneath it.
        WaitForSingleObject(threads[i], 10000);
        CloseHandle(threads[i]);
        CloseHandle(stops[i]);
        CloseHandle(wakes[i]);
    }
}
