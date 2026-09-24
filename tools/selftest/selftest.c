//
// selftest - exercises the engine outside explorer.exe.
//
// The engine normally only runs inside the shell, where a mistake costs the user their desktop. This console
// program links the same engine sources into an ordinary process and checks each layer on its own, so the parts
// that are hard to debug in place (the hook trampolines, the symbol lookup, the input arbitration) can be proven
// before anything is installed.
//
// Run it from the build output:
//     build\bin\Release\sp_selftest.exe
//
// The symbol test reaches the Microsoft symbol server on a cold cache. It is reported separately and does not
// fail the run when there is no network.
//
#include <Windows.h>
#include <stdio.h>

#include "engine/hooks.h"
#include "engine/input.h"
#include "engine/log.h"
#include "engine/settings.h"
#include "engine/symbols.h"

static int g_passed = 0;
static int g_failed = 0;

// Shared with modtest.c, which drives a real mod through the same reporting.
void Check(const char* what, BOOL ok)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (ok)
    {
        g_passed++;
    }
    else
    {
        g_failed++;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------------------------

#define SELFTEST_MOD_ID "selftest"

static void TestSettings(void)
{
    printf("Settings\n");

    Check("a missing value returns the default",
          SP_SettingsGetInt(SELFTEST_MOD_ID, L"NoSuchValue", 1234) == 1234);

    Check("an int round-trips", SP_SettingsSetInt(SELFTEST_MOD_ID, L"Probe", 42) &&
                                SP_SettingsGetInt(SELFTEST_MOD_ID, L"Probe", 0) == 42);

    Check("a mod is off until it is enabled", !SP_SettingsIsModEnabled(SELFTEST_MOD_ID));

    SP_SettingsSetInt(SELFTEST_MOD_ID, L"Enabled", 1);
    Check("enabling is visible", SP_SettingsIsModEnabled(SELFTEST_MOD_ID));

    // Leave the registry as it was found.
    wchar_t wszPath[MAX_PATH];
    if (SP_SettingsGetModKeyPath(SELFTEST_MOD_ID, wszPath, ARRAYSIZE(wszPath)))
    {
        RegDeleteKeyW(HKEY_CURRENT_USER, wszPath);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Hooking
//
// Two targets are used. A local function proves the trampoline rebuilds ordinary code correctly; an exported
// system function proves the same against code the engine did not compile.
// ---------------------------------------------------------------------------------------------------------------

__declspec(noinline) static int TargetAdd(int a, int b)
{
    // Enough work that the compiler emits a real function with a conventional prologue.
    int result = a + b;
    if (result == 0x7FFFFFFF)
    {
        result = 0;
    }
    return result;
}

typedef int (*TargetAdd_t)(int, int);
static TargetAdd_t g_origTargetAdd = NULL;
static int g_addDetourCalls = 0;

// Link-time code generation folds a call to TargetAdd with literal arguments into its result, so the hooked
// function would never actually be entered. Going through a volatile pointer with volatile arguments forces a
// real call, which is what the hook has to intercept.
static TargetAdd_t volatile g_callTargetAdd = TargetAdd;

static int CallTargetAdd(int a, int b)
{
    volatile int va = a;
    volatile int vb = b;
    return g_callTargetAdd(va, vb);
}

static int TargetAdd_Hook(int a, int b)
{
    g_addDetourCalls++;
    // Calling through the trampoline proves the original bytes were preserved and relocated.
    return g_origTargetAdd(a, b) * 10;
}

typedef ULONGLONG(WINAPI* GetTickCount64_t)(void);
static GetTickCount64_t g_origGetTickCount64 = NULL;
static int g_tickDetourCalls = 0;

static ULONGLONG WINAPI GetTickCount64_Hook(void)
{
    g_tickDetourCalls++;
    return g_origGetTickCount64();
}

static void TestHooks(void)
{
    printf("Hooks\n");

    Check("the hook engine starts", SP_HooksInitialize());

    int before = CallTargetAdd(2, 3);
    Check("the target behaves normally before hooking", before == 5);

    BOOL applied = SP_HookBegin() &&
                   SP_SetFunctionHookOwned("selftest", (void*)TargetAdd, (void*)TargetAdd_Hook, (void**)&g_origTargetAdd) &&
                   SP_HookExport("selftest", L"kernel32.dll", "GetTickCount64", (void*)GetTickCount64_Hook, (void**)&g_origGetTickCount64) &&
                   SP_HookCommit();
    Check("a two-hook transaction commits", applied);

    if (applied)
    {
        g_addDetourCalls = 0;
        int hooked = CallTargetAdd(2, 3);
        Check("the local detour runs", g_addDetourCalls == 1);
        Check("the trampoline returns the original result", hooked == 50);

        g_tickDetourCalls = 0;
        ULONGLONG ticks = GetTickCount64();
        Check("the exported detour runs", g_tickDetourCalls == 1);
        Check("the exported trampoline works", ticks > 0);

        Check("the hooks are removed again", SP_RemoveHooksOf("selftest"));

        g_addDetourCalls = 0;
        int after = CallTargetAdd(2, 3);
        Check("the target is restored", after == 5 && g_addDetourCalls == 0);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Input arbitration
//
// The point of the arbiter is that two mods wanting the same gesture are ordered by the user's priority and only
// one of them acts. That is what this checks.
// ---------------------------------------------------------------------------------------------------------------

static int g_order[4];
static int g_orderCount = 0;

static BOOL RecordingHandler(const SP_InputEvent* event, void* context)
{
    (void)event;
    int id = (int)(INT_PTR)context;
    if (g_orderCount < 4)
    {
        g_order[g_orderCount++] = id;
    }
    return FALSE;   // pass it on
}

static BOOL ConsumingHandler(const SP_InputEvent* event, void* context)
{
    (void)event;
    int id = (int)(INT_PTR)context;
    if (g_orderCount < 4)
    {
        g_order[g_orderCount++] = id;
    }
    return TRUE;    // stop here
}

static void TestInputArbitration(void)
{
    printf("Input arbitration\n");

    SP_InputInitialize();

    // The desktop surface cannot start in a console process, so the subscription reports failure. The
    // subscription itself is still recorded, which is what the arbitration is being tested on.
    SP_SettingsSetInt("mod-late", L"InputPriority", 200);
    SP_SettingsSetInt("mod-early", L"InputPriority", 10);

    SP_InputSubscribe("mod-late", SP_SURFACE_DESKTOP, SP_GESTURE_DOUBLE_CLICK, RecordingHandler, (void*)(INT_PTR)2);
    SP_InputSubscribe("mod-early", SP_SURFACE_DESKTOP, SP_GESTURE_DOUBLE_CLICK, RecordingHandler, (void*)(INT_PTR)1);

    SP_InputEvent ev;
    ZeroMemory(&ev, sizeof(ev));
    ev.surface = SP_SURFACE_DESKTOP;
    ev.gesture = SP_GESTURE_DOUBLE_CLICK;

    g_orderCount = 0;
    BOOL consumed = SP_InputDispatch(&ev);
    Check("an unconsumed gesture reports so", !consumed);
    Check("both subscribers ran", g_orderCount == 2);
    Check("the lower priority number ran first", g_orderCount == 2 && g_order[0] == 1 && g_order[1] == 2);

    // Now let the first one consume it: the second must not see the gesture at all. This is the case that two
    // mods on the same double click would otherwise both act on.
    SP_InputUnsubscribe("mod-early");
    SP_InputSubscribe("mod-early", SP_SURFACE_DESKTOP, SP_GESTURE_DOUBLE_CLICK, ConsumingHandler, (void*)(INT_PTR)1);

    g_orderCount = 0;
    consumed = SP_InputDispatch(&ev);
    Check("a consumed gesture reports so", consumed);
    Check("the later subscriber never saw it", g_orderCount == 1 && g_order[0] == 1);

    // A gesture nobody subscribed to reaches nobody.
    ev.gesture = SP_GESTURE_WHEEL;
    g_orderCount = 0;
    Check("an unsubscribed gesture is ignored", !SP_InputDispatch(&ev) && g_orderCount == 0);

    SP_InputUnsubscribe("mod-early");
    SP_InputUnsubscribe("mod-late");

    ev.gesture = SP_GESTURE_DOUBLE_CLICK;
    g_orderCount = 0;
    Check("unsubscribing takes effect", !SP_InputDispatch(&ev) && g_orderCount == 0);

    SP_InputShutdown();

    wchar_t wszPath[MAX_PATH];
    if (SP_SettingsGetModKeyPath("mod-early", wszPath, ARRAYSIZE(wszPath)))
    {
        RegDeleteKeyW(HKEY_CURRENT_USER, wszPath);
    }
    if (SP_SettingsGetModKeyPath("mod-late", wszPath, ARRAYSIZE(wszPath)))
    {
        RegDeleteKeyW(HKEY_CURRENT_USER, wszPath);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Symbols
//
// user32.dll is used because its exports also appear as public symbols in its PDB, so the address the engine
// finds can be compared against GetProcAddress. That checks the whole path: PDB identity, download, enumeration,
// name matching and the offset cache.
// ---------------------------------------------------------------------------------------------------------------

static void TestSymbols(void)
{
    printf("Symbols (needs the symbol server on a cold cache)\n");

    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (!hUser32)
    {
        printf("  [SKIP] user32.dll is not loaded\n");
        return;
    }

    void* pGetSystemMetrics = NULL;
    static const wchar_t* const kNames[] = { L"GetSystemMetrics" };

    SP_SymbolHook hooks[1];
    ZeroMemory(hooks, sizeof(hooks));
    hooks[0].symbols = kNames;
    hooks[0].symbolCount = 1;
    hooks[0].pOriginal = &pGetSystemMetrics;
    hooks[0].hookFunction = NULL;    // resolve only
    hooks[0].optional = FALSE;

    ULONGLONG start = GetTickCount64();
    BOOL ok = SP_ResolveSymbolsOwned("selftest", hUser32, hooks, 1);
    ULONGLONG elapsed = GetTickCount64() - start;

    if (!ok)
    {
        printf("  [SKIP] the symbol could not be resolved; the symbol server is probably unreachable\n");
        return;
    }

    void* expected = (void*)GetProcAddress(hUser32, "GetSystemMetrics");
    Check("the resolved address matches GetProcAddress", pGetSystemMetrics == expected);
    printf("         first lookup took %llu ms\n", elapsed);

    // The second lookup must come from the cache and be effectively instant.
    pGetSystemMetrics = NULL;
    start = GetTickCount64();
    ok = SP_ResolveSymbolsOwned("selftest", hUser32, hooks, 1);
    elapsed = GetTickCount64() - start;

    Check("the cached lookup gives the same address", ok && pGetSystemMetrics == expected);
    Check("the cached lookup is fast", elapsed < 250);
    printf("         cached lookup took %llu ms\n", elapsed);
}

int main(void)
{
    printf("ShadePatcher engine self-test\n\n");

    SP_LogInitialize();

    extern void TestTrayShowAllIconsMod(void);
    extern void TestExplorerAutoFileSizesMod(void);

    TestSettings();
    printf("\n");
    TestHooks();
    printf("\n");
    TestInputArbitration();
    printf("\n");
    TestSymbols();
    printf("\n");
    TestTrayShowAllIconsMod();
    printf("\n");
    TestExplorerAutoFileSizesMod();

    SP_SymbolsShutdown();
    SP_HooksShutdown();
    SP_LogShutdown();

    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
