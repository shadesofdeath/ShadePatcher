//
// custom-start-menu - ShadePatcher's own Start menu, in place of the Windows one.
//
// An original feature, not a Windhawk port. The design comes from design/startmenu (a hand-off package: README.md
// is the specification, StartMenuTokens.h the tokens); the menu itself lives in src/core/startmenu:
//
//   text.*      Figtree from the DLL's resources, text formats, the menu's own strings (Turkish and English)
//   catalog.*   installed apps (shell:AppsFolder), icons, pinned apps, recent files, the user; launching, power
//   view.*      the window: Win32 input, DirectComposition visual tree and animation, Direct2D, DirectWrite
//   host.*      the menu's thread and the hooks that bring the Start button and the Windows key to it
//
// Taking over Start
// -----------------
// The approach is Open-Shell's on Windows 11 (see host.h for the details): the Start button's clicks are
// swallowed by a mouse hook on the taskbar thread before XAML sees them, and the Windows key is made to arrive as
// WM_SYSCOMMAND / SC_TASKLIST on the shell window by keeping twinui.dll from registering Win and Ctrl+Esc as shell
// hotkeys. This file owns that last part, because it is an inline hook and inline hooks belong to the engine:
//
//   user32!ShellRegisterHotKey (ordinal 2671, not exported by name)
//       Refuses Win and Ctrl+Esc while the menu wants the Windows key, and remembers the refused registrations
//       so they can be made after all when the menu is turned off.
//
// When the engine starts after twinui has registered them (the usual case: the shell is up before the engine),
// the registrations are withdrawn instead, by an APC on the registering thread (hotkeys belong to a thread),
// with the ids Open-Shell established: 1 is Win, 2 is Ctrl+Esc. Those cannot be registered again without
// twinui's arguments, but nothing is lost: without them Windows falls back to SC_TASKLIST, which opens its own
// menu once this mod lets the message through.
//
#define SP_MOD_ID "custom-start-menu"
#include "engine/modapi.h"

#include <mutex>
#include <vector>

#include "startmenu/host.h"

namespace {

// ---------------------------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------------------------

// The value names and their ranges are documented in docs/startmenu.md and read in startmenu/host.cpp.
sm::HostSettings ReadSettings()
{
    return sm::ReadHostSettings();
}

// ---------------------------------------------------------------------------------------------------------------
// ShellRegisterHotKey
// ---------------------------------------------------------------------------------------------------------------

using ShellRegisterHotKey_t = BOOL(WINAPI*)(HWND hwnd, int id, UINT modifiers, UINT vk, HWND target);

ShellRegisterHotKey_t g_shellRegisterHotKey = nullptr;       // the real function, for re-registering
ShellRegisterHotKey_t g_origShellRegisterHotKey = nullptr;   // trampoline

struct Registration
{
    HWND hwnd;
    int id;
    UINT modifiers;
    UINT vk;
    HWND target;
    DWORD thread;
};

std::mutex g_refusedLock;
std::vector<Registration> g_refused;   // registrations kept from twinui while the menu has the Windows key
bool g_withdrawn = false;              // registrations made before the engine came were withdrawn by APC

bool IsStartHotkey(UINT modifiers, UINT vk)
{
    modifiers &= ~MOD_NOREPEAT;
    return (modifiers == MOD_WIN && vk == 0) || (modifiers == MOD_CONTROL && vk == VK_ESCAPE);
}

BOOL WINAPI ShellRegisterHotKeyHook(HWND hwnd, int id, UINT modifiers, UINT vk, HWND target)
{
    if (IsStartHotkey(modifiers, vk))
    {
        SP_Log(L"ShellRegisterHotKey(id %d, modifiers 0x%X, vk 0x%X) on thread %lu", id, modifiers, vk,
               GetCurrentThreadId());
        if (sm::HostWantsWinKey())
        {
            std::lock_guard<std::mutex> lock(g_refusedLock);
            g_refused.push_back({ hwnd, id, modifiers, vk, target, GetCurrentThreadId() });
            return FALSE;
        }
    }
    return g_origShellRegisterHotKey(hwnd, id, modifiers, vk, target);
}

DWORD ImmersiveShellThread()
{
    HWND window = FindWindowW(L"ApplicationManager_ImmersiveShellWindow", nullptr);
    return window ? GetWindowThreadProcessId(window, nullptr) : 0;
}

void NTAPI WithdrawApc(ULONG_PTR)
{
    BOOL win = UnregisterHotKey(nullptr, 1);        // Win
    BOOL ctrlEscape = UnregisterHotKey(nullptr, 2); // Ctrl+Esc
    SP_Log(L"Shell hotkeys withdrawn on thread %lu: Win %s, Ctrl+Esc %s", GetCurrentThreadId(),
           win ? L"yes" : L"no (not registered with these ids)", ctrlEscape ? L"yes" : L"no");
}

void NTAPI RegisterApc(ULONG_PTR parameter)
{
    Registration* registration = reinterpret_cast<Registration*>(parameter);
    if (g_shellRegisterHotKey)
    {
        g_shellRegisterHotKey(registration->hwnd, registration->id, registration->modifiers, registration->vk,
                              registration->target);
    }
    delete registration;
}

bool QueueOnThread(DWORD threadId, PAPCFUNC function, ULONG_PTR parameter)
{
    // The APC runs whenever that thread next waits alertably, possibly after this mod (or the engine) is gone.
    // Keep the DLL mapped for the rest of the process so the call can never land in freed code.
    static bool pinned = false;
    if (!pinned)
    {
        HMODULE self = nullptr;
        pinned = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                    reinterpret_cast<LPCWSTR>(&QueueOnThread), &self) != FALSE;
    }
    HANDLE thread = threadId ? OpenThread(THREAD_SET_CONTEXT, FALSE, threadId) : nullptr;
    if (!thread)
    {
        return false;
    }
    bool queued = QueueUserAPC(function, thread, parameter) != 0;
    CloseHandle(thread);
    return queued;
}

// Takes the Windows key from twinui if it already has it.
void WithdrawStartHotkeys()
{
    if (g_withdrawn)
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_refusedLock);
        if (!g_refused.empty())
        {
            return;   // the hook saw the registrations and refused them; nothing is registered
        }
    }
    DWORD thread = ImmersiveShellThread();
    if (!thread)
    {
        // A fresh shell: twinui has not registered them yet, and the hook will refuse them when it does.
        SP_Log(L"The shell's hotkeys are not registered yet; the hook will keep Win and Ctrl+Esc");
        return;
    }
    if (QueueOnThread(thread, WithdrawApc, 0))
    {
        g_withdrawn = true;
        SP_Log(L"Withdrawal of Win and Ctrl+Esc queued to the shell's hotkey thread");
    }
    else
    {
        SP_LogError(L"The shell's Start hotkeys could not be withdrawn; the Windows key keeps opening Windows Start");
    }
}

// Gives back the registrations the hook refused. Registrations withdrawn by APC stay withdrawn (their arguments
// are unknown); SC_TASKLIST then opens the Windows menu on its own.
void RestoreStartHotkeys()
{
    std::vector<Registration> refused;
    {
        std::lock_guard<std::mutex> lock(g_refusedLock);
        refused.swap(g_refused);
    }
    for (const Registration& registration : refused)
    {
        Registration* copy = new Registration(registration);
        if (!QueueOnThread(registration.thread, RegisterApc, reinterpret_cast<ULONG_PTR>(copy)))
        {
            delete copy;
        }
    }
    if (!refused.empty())
    {
        SP_Log(L"%u Start hotkey registrations handed back to the shell", (unsigned)refused.size());
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------------------------

sm::HostSettings g_settings;

BOOL Init()
{
    g_settings = ReadSettings();
    sm::HostUpdate(g_settings);   // the hotkey hook below asks the host whether the Windows key is wanted
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    g_shellRegisterHotKey =
        user32 ? reinterpret_cast<ShellRegisterHotKey_t>(GetProcAddress(user32, MAKEINTRESOURCEA(2671))) : nullptr;
    if (g_shellRegisterHotKey)
    {
        if (!SP_SetFunctionHookNow(g_shellRegisterHotKey, ShellRegisterHotKeyHook, &g_origShellRegisterHotKey))
        {
            SP_LogError(L"ShellRegisterHotKey could not be hooked");
        }
    }
    else
    {
        SP_LogError(L"user32 has no ShellRegisterHotKey (ordinal 2671) on this build");
    }
    return TRUE;
}

// The settings window's "reset pinned apps" link sets ResetPins; the value is cleared once acted on.
void HandlePinReset()
{
    if (SP_GetIntSetting(L"ResetPins", 0) != 0)
    {
        SP_SetIntSetting(L"ResetPins", 0);
        sm::HostResetPins();
    }
    // The same kind of one-shot request for the hidden apps and the search history.
    if (SP_GetIntSetting(L"ResetHidden", 0) != 0)
    {
        SP_SetIntSetting(L"ResetHidden", 0);
        sm::HostCommand(sm::kCommandShowHidden);
    }
    if (SP_GetIntSetting(L"ClearHistory", 0) != 0)
    {
        SP_SetIntSetting(L"ClearHistory", 0);
        sm::HostCommand(sm::kCommandClearHistory);
    }
}

void AfterInit()
{
    HandlePinReset();   // before the menu starts: it then loads the defaults
    if (!sm::HostStart(g_settings))
    {
        return;
    }
    if (g_settings.winKey)
    {
        WithdrawStartHotkeys();
    }
}

void SettingsChanged()
{
    sm::HostSettings settings = ReadSettings();
    bool winKeyChanged = settings.winKey != g_settings.winKey;
    g_settings = settings;
    sm::HostUpdate(settings);
    HandlePinReset();
    if (winKeyChanged)
    {
        if (settings.winKey)
        {
            WithdrawStartHotkeys();
        }
        else
        {
            RestoreStartHotkeys();
        }
    }
}

void BeforeUninit()
{
    sm::HostStop();
}

void Uninit()
{
    // The hook is gone by now, so the refused registrations can go straight to the real function.
    RestoreStartHotkeys();
}

} // namespace

SP_MOD_DEFINE(g_modCustomStartMenu) =
{
    /* id             */ SP_MOD_ID,
    /* name           */ L"ShadePatcher Start menu",
    /* basedOn        */ nullptr,
    /* originalAuthor */ nullptr,
    /* targets        */ SP_TARGET_EXPLORER,
    /* minOsBuild     */ 17763,
    /* flags          */ 0,
    /* Init           */ Init,
    /* AfterInit      */ AfterInit,
    /* SettingsChanged*/ SettingsChanged,
    /* BeforeUninit   */ BeforeUninit,
    /* Uninit         */ Uninit,
};
