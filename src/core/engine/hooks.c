#include "hooks.h"
#include "log.h"

#include <KNSoft/SlimDetours/SlimDetours.h>
#include <string.h>

#define TAG "hooks"

// One entry per installed hook. `original` points at the mod's variable, which holds the trampoline while the
// hook is installed; detaching needs both that pointer and the detour.
typedef struct HookEntry
{
    const char* owner;
    void*       detour;
    void**      original;
    void*       target;     // the address that was hooked, kept for logging
    BOOL        installed;
} HookEntry;

// A hook queued by SP_SetFunctionHookOwned but not yet committed.
typedef struct PendingHook
{
    const char* owner;
    void*       target;
    void*       detour;
    void**      original;
} PendingHook;

#define SP_MAX_HOOKS   512
#define SP_MAX_PENDING 128

static HookEntry   g_hooks[SP_MAX_HOOKS];
static int         g_hookCount = 0;

static PendingHook g_pending[SP_MAX_PENDING];
static int         g_pendingCount = 0;

static CRITICAL_SECTION g_lock;        // guards the tables
static CRITICAL_SECTION g_transaction; // held for the whole Begin..Commit window
static BOOL g_initialized = FALSE;

static BOOL OwnerMatches(const char* entryOwner, const char* wanted)
{
    if (wanted == NULL)
    {
        return TRUE;    // "every hook"
    }
    if (entryOwner == NULL)
    {
        return FALSE;
    }
    return strcmp(entryOwner, wanted) == 0;
}

BOOL SP_HooksInitialize(void)
{
    if (g_initialized)
    {
        return TRUE;
    }
    InitializeCriticalSection(&g_lock);
    InitializeCriticalSection(&g_transaction);
    g_initialized = TRUE;
    return TRUE;
}

void SP_HooksShutdown(void)
{
    if (!g_initialized)
    {
        return;
    }
    SP_RemoveHooksOf(NULL);
    SlimDetoursUninitialize();
    DeleteCriticalSection(&g_lock);
    DeleteCriticalSection(&g_transaction);
    g_initialized = FALSE;
}

BOOL SP_HookBegin(void)
{
    if (!g_initialized)
    {
        return FALSE;
    }

    EnterCriticalSection(&g_transaction);

    HRESULT hr = SlimDetoursTransactionBegin();
    if (FAILED(hr))
    {
        SP_LOG_ERR(TAG, L"SlimDetoursTransactionBegin failed: 0x%08X", hr);
        LeaveCriticalSection(&g_transaction);
        return FALSE;
    }

    // Every other thread is suspended from here to Commit/Abort; one of them may hold the log's lock, so this
    // thread's lines are kept aside until then (a wait for that lock would never end).
    SP_LogDeferBegin();
    g_pendingCount = 0;
    return TRUE;
}

BOOL SP_SetFunctionHookOwned(const char* owner, void* target, void* detour, void** original)
{
    if (!target || !detour || !original)
    {
        SP_LOG_ERR(TAG, L"Hook rejected: a null argument was passed");
        return FALSE;
    }
    if (g_pendingCount >= SP_MAX_PENDING)
    {
        SP_LOG_ERR(TAG, L"Hook rejected: more than %d hooks in one transaction", SP_MAX_PENDING);
        return FALSE;
    }
    if (g_hookCount + g_pendingCount >= SP_MAX_HOOKS)
    {
        SP_LOG_ERR(TAG, L"Hook rejected: the process already holds %d hooks", SP_MAX_HOOKS);
        return FALSE;
    }

    // SlimDetoursAttach takes the target in *original and replaces it with the trampoline.
    *original = target;

    HRESULT hr = SlimDetoursAttach(original, detour);
    if (FAILED(hr))
    {
        SP_LOG_ERR(TAG, L"SlimDetoursAttach(%p) failed: 0x%08X", target, hr);
        *original = NULL;
        return FALSE;
    }

    g_pending[g_pendingCount].owner = owner;
    g_pending[g_pendingCount].target = target;
    g_pending[g_pendingCount].detour = detour;
    g_pending[g_pendingCount].original = original;
    g_pendingCount++;

    SP_LOG_DBG(TAG, L"Queued hook at %p for %S", target, owner ? owner : "engine");
    return TRUE;
}

BOOL SP_HookCommit(void)
{
    HRESULT hr = SlimDetoursTransactionCommit();
    SP_LogDeferEnd();   // the threads run again, whichever way the commit went
    if (FAILED(hr))
    {
        SP_LOG_ERR(TAG, L"SlimDetoursTransactionCommit failed: 0x%08X (%d hook(s) discarded)", hr, g_pendingCount);
        // The transaction rolled back, so none of the queued trampolines are valid.
        for (int i = 0; i < g_pendingCount; ++i)
        {
            *g_pending[i].original = NULL;
        }
        g_pendingCount = 0;
        LeaveCriticalSection(&g_transaction);
        return FALSE;
    }

    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_pendingCount; ++i)
    {
        g_hooks[g_hookCount].owner = g_pending[i].owner;
        g_hooks[g_hookCount].detour = g_pending[i].detour;
        g_hooks[g_hookCount].original = g_pending[i].original;
        g_hooks[g_hookCount].target = g_pending[i].target;
        g_hooks[g_hookCount].installed = TRUE;
        g_hookCount++;
    }
    LeaveCriticalSection(&g_lock);

    SP_LOG_DBG(TAG, L"Committed %d hook(s); %d installed in total", g_pendingCount, g_hookCount);
    g_pendingCount = 0;
    LeaveCriticalSection(&g_transaction);
    return TRUE;
}

void SP_HookAbort(void)
{
    SlimDetoursTransactionAbort();
    SP_LogDeferEnd();
    for (int i = 0; i < g_pendingCount; ++i)
    {
        *g_pending[i].original = NULL;
    }
    g_pendingCount = 0;
    LeaveCriticalSection(&g_transaction);
}

BOOL SP_RemoveHooksOf(const char* owner)
{
    if (!g_initialized)
    {
        return FALSE;
    }

    EnterCriticalSection(&g_transaction);

    HRESULT hr = SlimDetoursTransactionBegin();
    if (FAILED(hr))
    {
        SP_LOG_ERR(TAG, L"SlimDetoursTransactionBegin (detach) failed: 0x%08X", hr);
        LeaveCriticalSection(&g_transaction);
        return FALSE;
    }
    SP_LogDeferBegin();

    EnterCriticalSection(&g_lock);

    int detached = 0;
    for (int i = 0; i < g_hookCount; ++i)
    {
        if (!g_hooks[i].installed || !OwnerMatches(g_hooks[i].owner, owner))
        {
            continue;
        }
        hr = SlimDetoursDetach(g_hooks[i].original, g_hooks[i].detour);
        if (FAILED(hr))
        {
            SP_LOG_ERR(TAG, L"SlimDetoursDetach(%p) failed: 0x%08X", g_hooks[i].target, hr);
            continue;
        }
        g_hooks[i].installed = FALSE;
        detached++;
    }

    LeaveCriticalSection(&g_lock);

    hr = SlimDetoursTransactionCommit();
    SP_LogDeferEnd();
    if (FAILED(hr))
    {
        SP_LOG_ERR(TAG, L"SlimDetoursTransactionCommit (detach) failed: 0x%08X", hr);
        LeaveCriticalSection(&g_transaction);
        return FALSE;
    }

    // Compact the table now that the detaches are live.
    EnterCriticalSection(&g_lock);
    int out = 0;
    for (int i = 0; i < g_hookCount; ++i)
    {
        if (g_hooks[i].installed)
        {
            g_hooks[out++] = g_hooks[i];
        }
        else
        {
            *g_hooks[i].original = NULL;
        }
    }
    g_hookCount = out;
    LeaveCriticalSection(&g_lock);

    SP_LOG_DBG(TAG, L"Detached %d hook(s); %d still installed", detached, g_hookCount);
    LeaveCriticalSection(&g_transaction);
    return TRUE;
}

BOOL SP_SetSingleFunctionHook(const char* owner, void* target, void* detour, void** original)
{
    if (!SP_HookBegin())
    {
        return FALSE;
    }
    if (!SP_SetFunctionHookOwned(owner, target, detour, original))
    {
        SP_HookAbort();
        return FALSE;
    }
    return SP_HookCommit();
}

BOOL SP_HookExport(const char* owner, const wchar_t* moduleName, const char* exportName, void* detour, void** original)
{
    HMODULE hModule = GetModuleHandleW(moduleName);
    if (!hModule)
    {
        SP_LOG_ERR(TAG, L"Module %s is not loaded", moduleName);
        return FALSE;
    }
    FARPROC pfn = GetProcAddress(hModule, exportName);
    if (!pfn)
    {
        SP_LOG_ERR(TAG, L"%s does not export %S", moduleName, exportName);
        return FALSE;
    }
    return SP_SetFunctionHookOwned(owner, (void*)pfn, detour, original);
}
