//
// start-menu-all-apps - open the Windows 11 Start menu on the "All apps" list instead of the pinned page.
//
// Adapted from the idea behind the Windhawk mod "Show all apps by default in start menu" (start-menu-all-apps)
// by m417z. The implementation here is written against this engine's API.
//
// Where the Start menu lives
// --------------------------
// The Start menu's XAML is StartMenu.dll (package MicrosoftWindows.Client.Core). Its frame class,
//     winrt::StartMenu::implementation::StartInnerFrame
// is built on a DockedStartController, whose implementation is in StartDocked.dll, and switches between the
// pinned page and the app list through two projected calls on that controller:
//     IDockedStartControllerOverrides::ShowAllApps
//     IDockedStartControllerOverrides::HideAllApps
//
// The original mod runs inside StartMenuExperienceHost.exe, which is where the Start menu is hosted on most
// builds. This engine only lives in explorer.exe. On build 26200 explorer.exe does load StartMenu.dll, so the
// hooks are installed there, but whether the frame is ever constructed in explorer.exe is a property of the
// build, not of this mod: when the menu is hosted by StartMenuExperienceHost.exe the hooks are simply never
// reached and nothing changes. The log says which case applies (see the constructor hook).
//
// How it works
// ------------
// Three things, all in StartMenu.dll, all found through the PDB:
//
//   * HideAllApps is what the shell calls when the menu closes, to put the pinned page back for next time.
//     The hook calls ShowAllApps instead, so every opening after the first starts on the app list.
//
//   * StartInnerFrame's constructor is hooked so that the very first opening starts on the app list too:
//     once the frame is built, ShowAllApps is called on its controller.
//
//   * Which controller. Newer builds (KB5055627 and later) no longer keep the constructor's parameter; the
//     frame converts it with winrt::impl::as<IDockedStartControllerOverrides>() and keeps the result. That
//     conversion is hooked, and the first one made while the constructor is running is the object to call
//     ShowAllApps on. On builds without that function the constructor's parameter is used directly, as the
//     original mod does.
//
// Nothing here is C++/WinRT: the hooks pass opaque pointers back to the shell's own code, so the file is
// compiled without exceptions like the rest of the engine.
//
#define SP_MOD_ID "start-menu-all-apps"
#include "engine/modapi.h"

#include <atomic>

namespace {

// Cleared in BeforeUninit so the hooks fall through to the shell's own behaviour while they are still in place.
std::atomic<bool> g_enabled{ true };

// ---------------------------------------------------------------------------------------------------------------
// The functions in StartMenu.dll
//
// Every one of them is __cdecl on x64 with `this` as the first argument. The projected calls take the address of
// a projected object (one interface pointer wide) as `this`, which is why the pointers below are never
// dereferenced here: they are only handed back to the shell's own consume_ functions.
// ---------------------------------------------------------------------------------------------------------------

using ShowAllApps_t = void (*)(void* pThis);
using HideAllApps_t = void (*)(void* pThis);
using StartInnerFrameCtor_t = void* (*)(void* pThis, void* dockedStartController, void* param2);
using AsOverrides_t = void* (*)(void* pResult, void* pFrom);

ShowAllApps_t         g_origShowAllApps = nullptr;      // resolved, not hooked
HideAllApps_t         g_origHideAllApps = nullptr;
StartInnerFrameCtor_t g_origStartInnerFrameCtor = nullptr;
AsOverrides_t         g_origAsOverrides = nullptr;      // optional: absent on builds before KB5055627

// The thread that is inside StartInnerFrame's constructor right now, or 0. The as<> hook runs on the same thread
// as the constructor that called it, so this is what tells a conversion made for the frame apart from any other.
std::atomic<DWORD> g_constructorThread{ 0 };

// The controller-overrides object captured by the as<> hook during the current construction.
std::atomic<void*> g_capturedOverrides{ nullptr };

// ---------------------------------------------------------------------------------------------------------------
// The hooks
// ---------------------------------------------------------------------------------------------------------------

// Called by the shell when the menu is dismissed, to reset it to the pinned page. Showing the app list instead
// is what makes the next opening start there.
void HideAllApps_Hook(void* pThis)
{
    if (g_enabled.load(std::memory_order_relaxed) && g_origShowAllApps && pThis)
    {
        SP_LogDebug(L"HideAllApps: showing all apps instead");
        g_origShowAllApps(pThis);
        return;
    }

    g_origHideAllApps(pThis);
}

// The frame keeps the controller as IDockedStartControllerOverrides, converted inside the constructor. The first
// conversion made on the constructor's thread is the one the frame stores; its result slot is the object
// ShowAllApps wants as `this`.
void* AsOverrides_Hook(void* pResult, void* pFrom)
{
    if (g_constructorThread.load(std::memory_order_acquire) == GetCurrentThreadId())
    {
        void* expected = nullptr;
        if (g_capturedOverrides.compare_exchange_strong(expected, pResult, std::memory_order_acq_rel))
        {
            SP_LogDebug(L"Captured the frame's controller overrides");
        }
    }

    return g_origAsOverrides(pResult, pFrom);
}

void* StartInnerFrameCtor_Hook(void* pThis, void* dockedStartController, void* param2)
{
    // This line is the evidence that the Start menu is hosted in this process at all.
    SP_Log(L"StartInnerFrame is being constructed in this process");

    g_capturedOverrides.store(nullptr, std::memory_order_relaxed);
    g_constructorThread.store(GetCurrentThreadId(), std::memory_order_release);

    void* ret = g_origStartInnerFrameCtor(pThis, dockedStartController, param2);

    g_constructorThread.store(0, std::memory_order_release);
    void* overrides = g_capturedOverrides.exchange(nullptr, std::memory_order_acq_rel);

    if (!g_enabled.load(std::memory_order_relaxed) || !g_origShowAllApps)
    {
        return ret;
    }

    if (!overrides)
    {
        if (g_origAsOverrides)
        {
            // The conversion exists on this build but was not seen during construction, so the parameter is not
            // known to be the object the consume_ call expects. Better to leave the first opening alone than to
            // call through the wrong interface.
            SP_LogError(L"The frame's controller was not captured; the first opening keeps the pinned page");
            return ret;
        }
        // Older builds: the constructor's parameter is the overrides object itself.
        overrides = dockedStartController;
    }

    if (overrides)
    {
        SP_Log(L"Showing all apps on the freshly built frame");
        g_origShowAllApps(overrides);
    }

    return ret;
}

// ---------------------------------------------------------------------------------------------------------------
// Installing the hooks
// ---------------------------------------------------------------------------------------------------------------

BOOL InstallHooks(HMODULE hStartMenu)
{
    // The spellings are the ones the original mod lists, newest first; the engine matches the first that the
    // PDB for the loaded build actually contains.
    static const wchar_t* const kShowAllApps[] =
    {
        LR"(public: __cdecl winrt::impl::consume_WindowsUdk_UI_StartScreen_Implementation_IDockedStartControllerOverrides<struct winrt::WindowsUdk::UI::StartScreen::Implementation::IDockedStartControllerOverrides>::ShowAllApps(void)const )",
        LR"(public: __cdecl winrt::impl::consume_WindowsUdk_UI_StartScreen_Implementation_IDockedStartControllerOverrides<struct winrt::WindowsUdk::UI::StartScreen::Implementation::DockedStartController>::ShowAllApps(void)const )",
        LR"(public: void __cdecl winrt::impl::consume_WindowsUdk_UI_StartScreen_Implementation_IDockedStartControllerOverrides<struct winrt::WindowsUdk::UI::StartScreen::Implementation::DockedStartController>::ShowAllApps(void)const )",
    };

    static const wchar_t* const kHideAllApps[] =
    {
        LR"(public: __cdecl winrt::impl::consume_WindowsUdk_UI_StartScreen_Implementation_IDockedStartControllerOverrides<struct winrt::WindowsUdk::UI::StartScreen::Implementation::IDockedStartControllerOverrides>::HideAllApps(void)const )",
        LR"(public: __cdecl winrt::impl::consume_WindowsUdk_UI_StartScreen_Implementation_IDockedStartControllerOverrides<struct winrt::WindowsUdk::UI::StartScreen::Implementation::DockedStartController>::HideAllApps(void)const )",
        LR"(public: void __cdecl winrt::impl::consume_WindowsUdk_UI_StartScreen_Implementation_IDockedStartControllerOverrides<struct winrt::WindowsUdk::UI::StartScreen::Implementation::DockedStartController>::HideAllApps(void)const )",
    };

    static const wchar_t* const kStartInnerFrameCtor[] =
    {
        LR"(public: __cdecl winrt::StartMenu::implementation::StartInnerFrame::StartInnerFrame(struct winrt::WindowsUdk::UI::StartScreen::Implementation::DockedStartController const &,struct winrt::Windows::Foundation::IInspectable const &))",
    };

    static const wchar_t* const kAsOverrides[] =
    {
        LR"(struct winrt::WindowsUdk::UI::StartScreen::Implementation::IDockedStartControllerOverrides __cdecl winrt::impl::as<struct winrt::WindowsUdk::UI::StartScreen::Implementation::IDockedStartControllerOverrides,struct winrt::impl::abi<struct winrt::Windows::Foundation::IUnknown,void>::type,0>(struct winrt::impl::abi<struct winrt::Windows::Foundation::IUnknown,void>::type *))",
    };

    SP_SymbolHook hooks[4] = {};

    // ShowAllApps is only called, never intercepted.
    hooks[0].symbols = kShowAllApps;
    hooks[0].symbolCount = ARRAYSIZE(kShowAllApps);
    hooks[0].pOriginal = (void**)&g_origShowAllApps;
    hooks[0].hookFunction = nullptr;
    hooks[0].optional = FALSE;

    hooks[1].symbols = kHideAllApps;
    hooks[1].symbolCount = ARRAYSIZE(kHideAllApps);
    hooks[1].pOriginal = (void**)&g_origHideAllApps;
    hooks[1].hookFunction = (void*)HideAllApps_Hook;
    hooks[1].optional = FALSE;

    hooks[2].symbols = kStartInnerFrameCtor;
    hooks[2].symbolCount = ARRAYSIZE(kStartInnerFrameCtor);
    hooks[2].pOriginal = (void**)&g_origStartInnerFrameCtor;
    hooks[2].hookFunction = (void*)StartInnerFrameCtor_Hook;
    hooks[2].optional = FALSE;

    // Added in KB5055627; older builds pass the controller straight into the constructor.
    hooks[3].symbols = kAsOverrides;
    hooks[3].symbolCount = ARRAYSIZE(kAsOverrides);
    hooks[3].pOriginal = (void**)&g_origAsOverrides;
    hooks[3].hookFunction = (void*)AsOverrides_Hook;
    hooks[3].optional = TRUE;

    if (!SP_HookSymbols(hStartMenu, hooks, ARRAYSIZE(hooks)))
    {
        SP_LogError(L"The Start menu functions were not found in this build of StartMenu.dll");
        return FALSE;
    }

    SP_Log(L"Hooked StartMenu.dll%s", g_origAsOverrides ? L"" : L" (older build: no controller conversion)");
    return TRUE;
}

// Runs on the engine's helper thread as soon as StartMenu.dll is in the process.
void OnStartMenuLoaded(HMODULE hModule, void* /*context*/)
{
    if (!hModule)
    {
        return;
    }
    InstallHooks(hModule);
}

// ---------------------------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------------------------

BOOL Init()
{
    g_enabled.store(true, std::memory_order_relaxed);

    // StartMenu.dll comes and goes with the shell's own timing: on a cold sign-in it is not there yet, and on
    // some builds it only appears when the menu is first opened. So there is no deadline: the poll is cheap and
    // the engine cancels it when the mod unloads.
    if (!SP_WaitForModule(L"StartMenu.dll", 0, OnStartMenuLoaded, nullptr))
    {
        SP_LogError(L"Could not wait for StartMenu.dll");
        return FALSE;
    }

    SP_Log(L"Waiting for StartMenu.dll");
    return TRUE;
}

void BeforeUninit()
{
    // Still hooked; from here on HideAllApps does what the shell meant, so the next close puts the pinned page
    // back and the menu is exactly as it was before the mod.
    g_enabled.store(false, std::memory_order_relaxed);
}

}   // namespace

SP_MOD_DEFINE(g_modStartMenuAllApps) =
{
    /* id             */ SP_MOD_ID,
    /* name           */ L"Show all apps when the Start menu opens",
    /* basedOn        */ "start-menu-all-apps",
    /* originalAuthor */ "m417z",
    /* targets        */ SP_TARGET_EXPLORER,
    /* minOsBuild     */ 22000,     // the Windows 11 Start menu (StartMenu.dll)
    /* flags          */ 0,
    /* Init           */ Init,
    /* AfterInit      */ nullptr,
    /* SettingsChanged*/ nullptr,
    /* BeforeUninit   */ BeforeUninit,
    /* Uninit         */ nullptr,
};
