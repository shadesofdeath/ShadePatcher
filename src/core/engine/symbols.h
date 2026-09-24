#pragma once
//
// symbols.h - finding undocumented functions inside Windows modules.
//
// Most of the shell is not exported. A function like
//   "public: void __cdecl winrt::SystemTray::implementation::VolumeSystemTrayIconDataModel::OnIconClicked(...)"
// only has a name in the PDB Microsoft publishes on its symbol server, so the engine downloads that PDB once,
// reads the addresses out of it and remembers them.
//
// Three layers, cheapest first:
//   1. Registry cache, keyed by the module's PDB identity (GUID + age). A cached entry is exact: a Windows
//      update changes the identity, so a stale offset can never be used against a new build.
//   2. The PDB already in the local symbol folder.
//   3. A download from the Microsoft symbol server.
//
// The cache is shared by every mod rather than kept per mod, so the second mod that needs a symbol from
// taskbar.dll pays nothing.
//
// Safety: a cached offset or a PDB symbol that falls outside the module image is rejected. The cache lives in a
// writable registry key and the symbol folder is writable too, so neither is treated as trusted input; without
// this check a planted entry could point a hook at an address of the attacker's choosing.
//
#include <Windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// One function to find. Several spellings may be given: Windows renames symbols between builds, so a mod lists
// every name it knows and the first that matches wins.
//
// hookFunction NULL means "only resolve": *pOriginal receives the address and nothing is patched. That is how a
// mod gets a pointer it wants to call rather than intercept.
typedef struct SP_SymbolHook
{
    const wchar_t* const* symbols;      // candidate undecorated names
    size_t                symbolCount;
    void**                pOriginal;
    void*                 hookFunction; // NULL to resolve without hooking
    BOOL                  optional;     // TRUE: a miss is not a failure
} SP_SymbolHook;

void SP_SymbolsInitialize(void);
void SP_SymbolsShutdown(void);

// Resolves every entry and installs the hooks in one transaction. Returns FALSE if a required symbol could not
// be found or a hook could not be applied; optional entries that are missing leave *pOriginal as NULL.
//
// `owner` is the mod id, used for hook ownership and for log messages.
BOOL SP_HookSymbolsOwned(const char* owner, HMODULE module, const SP_SymbolHook* hooks, size_t hookCount);

// Resolves without hooking. Useful when a mod only needs addresses.
BOOL SP_ResolveSymbolsOwned(const char* owner, HMODULE module, const SP_SymbolHook* hooks, size_t hookCount);

// The same, for a module whose path cannot be recovered from its handle. A module mapped with
// LOAD_LIBRARY_AS_IMAGE_RESOURCE is not in the process module list, so GetModuleFileName fails on it; the path
// has to be supplied. Pass NULL for modulePath to fall back to the handle, which is what the two calls above do.
BOOL SP_ResolveSymbolsAtOwned(const char* owner, HMODULE module, const wchar_t* modulePath,
                              const SP_SymbolHook* hooks, size_t hookCount);

// Walks every symbol in a module's PDB, undecorated exactly the way SP_HookSymbols matches them. Return FALSE
// from the callback to stop early.
//
// This exists for working out what a function is called on a given Windows build: names change between
// releases, and a mod that stops working needs the new spelling. It reads the PDB every time and takes seconds,
// so it belongs in tooling rather than in a mod's start-up path.
typedef BOOL (*SP_SymbolEnumProc)(const wchar_t* undecorated, const wchar_t* decorated, ULONGLONG offset, void* context);

BOOL SP_EnumSymbolsAtOwned(const char* owner, HMODULE module, const wchar_t* modulePath,
                           SP_SymbolEnumProc callback, void* context);

#ifdef __cplusplus
}
#endif
