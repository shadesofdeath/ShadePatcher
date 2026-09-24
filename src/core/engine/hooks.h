#pragma once
//
// hooks.h - the inline hooking layer.
//
// Built on SlimDetours, the same library Windhawk uses underneath its MinHook-compatible shim. Every hook is
// applied inside a transaction that suspends the other threads, so a thread can never be executing the middle of
// a function while its first bytes are being rewritten.
//
// Ownership: every hook is recorded against the mod that asked for it, so turning one mod off removes exactly its
// hooks and leaves the others in place. The owner is the mod id string.
//
// Usage from a mod (SP_SetFunctionHook in modapi.h fills in the owner):
//
//     SP_HookBegin();
//     SP_SetFunctionHook(target, detour, (void**)&originalPtr);
//     SP_HookCommit();
//
// Between Begin and Commit nothing is patched yet, so a batch of hooks either all take effect or none do.
//
#include <Windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// Prepares the hook engine. Called once by the engine before any mod runs.
BOOL SP_HooksInitialize(void);
void SP_HooksShutdown(void);

// Opens a hook transaction on the calling thread. Transactions are process-wide and serialized: a second caller
// blocks until the first commits or aborts.
BOOL SP_HookBegin(void);

// Queues one hook. `owner` is the mod id, or NULL for a hook owned by the engine itself. On success *original
// receives the trampoline through which the detour calls the untouched function.
BOOL SP_SetFunctionHookOwned(const char* owner, void* target, void* detour, void** original);

// Applies everything queued since SP_HookBegin.
BOOL SP_HookCommit(void);

// Discards everything queued since SP_HookBegin.
void SP_HookAbort(void);

// Removes every hook owned by `owner`, in one transaction. NULL removes all hooks, which is what shutdown wants.
BOOL SP_RemoveHooksOf(const char* owner);

// Convenience for a single hook applied in its own transaction.
BOOL SP_SetSingleFunctionHook(const char* owner, void* target, void* detour, void** original);

// Resolves an exported function and queues a hook on it. Must be called inside a transaction.
// Returns FALSE if the module is not loaded or does not export the name.
BOOL SP_HookExport(const char* owner, const wchar_t* moduleName, const char* exportName, void* detour, void** original);

#ifdef __cplusplus
}
#endif
