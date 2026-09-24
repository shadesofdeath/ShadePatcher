//
// start_power_buttons - one-click power buttons in the Windows 11 Start menu.
//
// Adapted from the idea behind the Windhawk mod "Windows 11 Start Menu Power Buttons" (win11-power-buttons) by
// Hakuuyosei. The implementation here is written against this engine's API.
//
// What it does
// ------------
// The Start menu's power button opens a flyout with Sleep, Shut down and Restart in it. This mod hides that
// button and puts a row of one-click buttons (Shut down, Restart, Sign out, Sleep, Hibernate, Lock; the user
// picks which) in its place, in the same panel of the navigation bar. Clicking one runs the action at once,
// or after a Yes/No question when the user asked for one.
//
// Where the Start menu lives, and why this may do nothing
// -------------------------------------------------------
// The Windhawk original runs in two processes: its buttons live in StartMenuExperienceHost.exe, where the Start
// menu XAML is, and a proxy window in explorer.exe carries out the power action because the Start menu process is
// an AppContainer with no right to shut the machine down.
//
// This engine runs only inside explorer.exe. On the build this was written on (26200) the Start menu XAML
// (StartDocked.dll, on a Windows.UI.Core.CoreWindow) is in StartMenuExperienceHost.exe and nothing of it is
// reachable from explorer.exe, so on that build the mod waits and never draws anything. Windows has hosted the
// Start menu inside explorer.exe on some builds, and may again; this mod is written for that case:
//
//   * SP_WaitForModule(StartDocked.dll) is the guard. Until the Start menu's own XAML library is in this process
//     nothing is hooked and nothing is touched.
//   * Once it is, CreateWindowInBand / CreateWindowInBandEx (the user32 exports every CoreWindow is created
//     through) are hooked to catch the Start menu's CoreWindow as it appears, and the CoreWindows that already
//     exist are picked up by enumeration.
//   * Each CoreWindow found is subclassed, so the XAML work runs on its own thread: the thread's CoreWindow gives
//     the VisibilityChanged / Activated events, and Window::Current().Content() is the tree the "PowerButton"
//     element is searched in.
//
// Nothing is guessed about symbol names inside StartDocked.dll: everything goes through public XAML.
//
// Power actions
// -------------
// The buttons and the action are in the same process here, so no proxy window is needed. explorer.exe has the
// user's full token; SE_SHUTDOWN_NAME is enabled on it and ExitWindowsEx / SetSuspendState / LockWorkStation are
// called directly, on a worker thread so that the confirmation box never blocks the shell's UI thread.
//
// Exceptions
// ----------
// C++/WinRT reports failures by throwing, so this one file is compiled with exceptions on while the rest of the
// engine is not. Every path that XAML or the window procedure can call into is wrapped: an exception reaching a
// shell thread would end the process.
//
#define SP_MOD_ID "win11-power-buttons"
#include "engine/modapi.h"

#undef GetCurrentTime

#include <commctrl.h>
#include <powrprof.h>

#include <atomic>
#include <cwchar>
#include <cwctype>
#include <mutex>
#include <string>
#include <vector>

#include <unknwn.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Media.h>

#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "comctl32.lib")

namespace {

namespace wux  = winrt::Windows::UI::Xaml;
namespace wuxc = winrt::Windows::UI::Xaml::Controls;
namespace wuxm = winrt::Windows::UI::Xaml::Media;
namespace wuc  = winrt::Windows::UI::Core;
using winrt::Windows::Foundation::IInspectable;

// ---------------------------------------------------------------------------------------------------------------
// The buttons
// ---------------------------------------------------------------------------------------------------------------

enum class PowerAction
{
    Shutdown = 0,
    Restart,
    SignOut,
    Sleep,
    Hibernate,
    Lock,
    Count
};

struct ButtonDefinition
{
    PowerAction    action;
    const wchar_t* keyword;        // the word the Order setting uses
    const wchar_t* settingName;    // the per-button toggle
    int            shownByDefault;
    const wchar_t* glyph;          // Segoe Fluent Icons
};

// The order the buttons take when the user has not arranged them: the mild ones first, the one that ends the
// session last, which puts Shut down where the power button used to be.
const ButtonDefinition kButtons[] =
{
    { PowerAction::Lock,      L"lock",      L"ShowLock",      0, L"\xE72E" },
    { PowerAction::Sleep,     L"sleep",     L"ShowSleep",     1, L"\xE708" },
    { PowerAction::Hibernate, L"hibernate", L"ShowHibernate", 0, L"\xE823" },
    { PowerAction::SignOut,   L"signout",   L"ShowSignOut",   1, L"\xF3B1" },
    { PowerAction::Restart,   L"restart",   L"ShowRestart",   1, L"\xE777" },
    { PowerAction::Shutdown,  L"shutdown",  L"ShowShutdown",  1, L"\xE7E8" },
};

const ButtonDefinition* FindDefinition(PowerAction action)
{
    for (const auto& def : kButtons)
    {
        if (def.action == action)
        {
            return &def;
        }
    }
    return nullptr;
}

// Written on the engine thread, read on the Start menu's UI thread and on the worker that runs an action.
std::atomic<bool> g_enabled{ true };
std::atomic<bool> g_confirm{ false };
std::atomic<bool> g_forceClose{ false };
std::atomic<int>  g_alignment{ 0 };         // 0 right, 1 left, 2 center
std::atomic<bool> g_rebuild{ false };       // the settings changed: build the row again rather than reuse it
std::atomic<bool> g_unloading{ false };

std::mutex g_buttonsLock;
std::vector<PowerAction> g_buttons;         // what to show, in order

std::vector<PowerAction> CurrentButtons()
{
    std::lock_guard<std::mutex> lock(g_buttonsLock);
    return g_buttons;
}

// ---------------------------------------------------------------------------------------------------------------
// Text
//
// The tooltips and the confirmation box follow the shell's UI language between English and Turkish, the two
// languages the product ships in; anything else gets English.
// ---------------------------------------------------------------------------------------------------------------

bool IsTurkishUi()
{
    return PRIMARYLANGID(GetThreadUILanguage()) == LANG_TURKISH;
}

std::wstring ActionName(PowerAction action)
{
    const bool tr = IsTurkishUi();
    switch (action)
    {
    case PowerAction::Shutdown:  return tr ? L"Kapat" : L"Shut down";
    case PowerAction::Restart:   return tr ? L"Yeniden ba\x015Flat" : L"Restart";
    case PowerAction::SignOut:   return tr ? L"Oturumu kapat" : L"Sign out";
    case PowerAction::Sleep:     return tr ? L"Uyku" : L"Sleep";
    case PowerAction::Hibernate: return tr ? L"Haz\x0131rda beklet" : L"Hibernate";
    case PowerAction::Lock:      return tr ? L"Kilitle" : L"Lock";
    default:                     return L"";
    }
}

std::wstring ConfirmTitle()
{
    return IsTurkishUi() ? L"G\x00FC\x00E7 i\x015Flemi" : L"Confirm power action";
}

std::wstring ConfirmMessage(PowerAction action)
{
    std::wstring name = ActionName(action);
    if (IsTurkishUi())
    {
        return name + L" i\x015Flemi yap\x0131ls\x0131n m\x0131?";
    }
    for (auto& c : name)
    {
        c = (wchar_t)towlower(c);
    }
    return L"Are you sure you want to " + name + L"?";
}

// ---------------------------------------------------------------------------------------------------------------
// Running an action
// ---------------------------------------------------------------------------------------------------------------

void EnableShutdownPrivilege()
{
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
    {
        return;
    }

    TOKEN_PRIVILEGES tp = {};
    if (LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid))
    {
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(hToken, FALSE, &tp, 0, nullptr, nullptr);
    }
    CloseHandle(hToken);
}

void PerformPowerAction(PowerAction action)
{
    EnableShutdownPrivilege();

    // The original always forces applications closed. That loses unsaved work without a word, so here the
    // default only forces the ones that stopped responding, the way the shell's own power menu does; ForceClose
    // restores the original's behaviour.
    const UINT force = g_forceClose.load(std::memory_order_relaxed) ? EWX_FORCE : EWX_FORCEIFHUNG;
    const DWORD reason = SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED;

    BOOL ok = TRUE;
    switch (action)
    {
    case PowerAction::Shutdown:
        ok = ExitWindowsEx(EWX_SHUTDOWN | EWX_POWEROFF | force, reason);
        break;
    case PowerAction::Restart:
        ok = ExitWindowsEx(EWX_REBOOT | force, reason);
        break;
    case PowerAction::SignOut:
        ok = ExitWindowsEx(EWX_LOGOFF | force, 0);
        break;
    case PowerAction::Sleep:
        ok = SetSuspendState(FALSE, FALSE, FALSE);
        break;
    case PowerAction::Hibernate:
        ok = SetSuspendState(TRUE, FALSE, FALSE);
        break;
    case PowerAction::Lock:
        ok = LockWorkStation();
        break;
    default:
        return;
    }

    if (!ok)
    {
        SP_LogError(L"Power action %d failed: %lu", (int)action, GetLastError());
    }
}

// The confirmation box is modal, so the action gets a thread of its own: the Start menu's UI thread must not
// stop while the question is on screen.
DWORD WINAPI ActionThread(LPVOID param)
{
    const PowerAction action = (PowerAction)(ULONG_PTR)param;

    if (g_confirm.load(std::memory_order_relaxed))
    {
        std::wstring title = ConfirmTitle();
        std::wstring message = ConfirmMessage(action);
        int answer = MessageBoxW(nullptr, message.c_str(), title.c_str(),
                                 MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2 | MB_TOPMOST | MB_SETFOREGROUND);
        if (answer != IDYES)
        {
            SP_LogDebug(L"Power action %d declined", (int)action);
            return 0;
        }
    }

    SP_Log(L"Running power action %d", (int)action);
    PerformPowerAction(action);
    return 0;
}

void StartPowerAction(PowerAction action)
{
    if (g_unloading.load(std::memory_order_relaxed))
    {
        return;
    }
    if (HANDLE thread = CreateThread(nullptr, 0, ActionThread, (LPVOID)(ULONG_PTR)action, 0, nullptr))
    {
        CloseHandle(thread);
    }
    else
    {
        SP_LogError(L"The action thread could not be started: %lu", GetLastError());
    }
}

// ---------------------------------------------------------------------------------------------------------------
// The XAML side
//
// Everything below runs on the Start menu's UI thread, inside the subclass procedure of its CoreWindow.
// ---------------------------------------------------------------------------------------------------------------

constexpr wchar_t kContainerTag[] = L"ShadePatcher_PowerButtons";

// There is one Start menu, so one set of references is enough. They are weak: the tree belongs to the shell and
// may be torn down at any time, and a strong reference from here would only keep dead elements alive.
winrt::weak_ref<wuxc::Panel>           g_parentPanel{ nullptr };
winrt::weak_ref<wux::FrameworkElement> g_originalPowerButton{ nullptr };
winrt::weak_ref<wuxc::StackPanel>      g_container{ nullptr };

// Whether two projected objects are the same XAML object. C++/WinRT's == compares the pointers it holds, and a
// Panel and the DependencyObject the tree hands back for it hold different interfaces of one object, so the
// COM identity (the IUnknown pointer) is what is compared.
bool SameObject(IInspectable const& a, IInspectable const& b)
{
    if (!a || !b)
    {
        return false;
    }

    ::IUnknown* identityA = nullptr;
    ::IUnknown* identityB = nullptr;
    static_cast<::IUnknown*>(winrt::get_abi(a))->QueryInterface(IID_IUnknown, (void**)&identityA);
    static_cast<::IUnknown*>(winrt::get_abi(b))->QueryInterface(IID_IUnknown, (void**)&identityB);
    if (identityA)
    {
        identityA->Release();
    }
    if (identityB)
    {
        identityB->Release();
    }
    return identityA && identityA == identityB;
}

wux::HorizontalAlignment AlignmentSetting()
{
    switch (g_alignment.load(std::memory_order_relaxed))
    {
    case 1:  return wux::HorizontalAlignment::Left;
    case 2:  return wux::HorizontalAlignment::Center;
    default: return wux::HorizontalAlignment::Right;
    }
}

wuxc::Button MakeButton(PowerAction action, const wchar_t* glyph, bool last)
{
    wuxc::Button button;
    button.Width(40);
    button.Height(40);
    // A gap between the buttons, none at the edges.
    button.Margin(last ? wux::Thickness{ 0, 0, 0, 0 } : wux::Thickness{ 0, 0, 4, 0 });
    // Flat, like the power button it replaces.
    button.Background(wuxm::SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));
    button.BorderThickness(wux::Thickness{ 0, 0, 0, 0 });
    button.CornerRadius(wux::CornerRadius{ 4, 4, 4, 4 });

    wuxc::FontIcon icon;
    icon.Glyph(glyph);
    icon.FontFamily(wuxm::FontFamily(L"Segoe Fluent Icons"));
    icon.FontSize(16);
    button.Content(icon);

    wuxc::ToolTipService::SetToolTip(button, winrt::box_value(winrt::hstring(ActionName(action))));

    button.Click([action](IInspectable const&, wux::RoutedEventArgs const&) {
        // XAML calls this; nothing may escape back into it.
        try
        {
            StartPowerAction(action);
        }
        catch (...)
        {
        }
    });
    return button;
}

// Looks for the row this mod added among the panel's children; the cached reference is preferred, but only
// while it is still attached where it was put.
wuxc::StackPanel FindContainer(wuxc::Panel const& parent)
{
    if (auto cached = g_container.get())
    {
        try
        {
            if (SameObject(wuxm::VisualTreeHelper::GetParent(cached), parent))
            {
                return cached;
            }
        }
        catch (...)
        {
        }
        g_container = nullptr;
    }

    auto children = parent.Children();
    for (uint32_t i = 0; i < children.Size(); ++i)
    {
        if (auto panel = children.GetAt(i).try_as<wuxc::StackPanel>())
        {
            auto tag = panel.Tag();
            if (tag && winrt::unbox_value_or<winrt::hstring>(tag, L"") == kContainerTag)
            {
                g_container = panel;
                return panel;
            }
        }
    }
    return nullptr;
}

// Hides the shell's power button and puts the row in its panel, or brings an existing row up to date.
void InjectButtons(wuxc::Panel const& parent, wux::FrameworkElement const& powerButton)
{
    const std::vector<PowerAction> buttons = CurrentButtons();

    if (powerButton.Visibility() != wux::Visibility::Collapsed)
    {
        powerButton.Visibility(wux::Visibility::Collapsed);
    }

    wuxc::StackPanel container = FindContainer(parent);
    if (!container)
    {
        container = wuxc::StackPanel();
        container.Tag(winrt::box_value(winrt::hstring(kContainerTag)));
        container.Orientation(wuxc::Orientation::Horizontal);
        container.VerticalAlignment(wux::VerticalAlignment::Center);

        // When the panel is a Grid the row goes in the same cell as the button it replaces; otherwise it is
        // simply appended and the alignment places it.
        if (parent.try_as<wuxc::Grid>())
        {
            wuxc::Grid::SetColumn(container, wuxc::Grid::GetColumn(powerButton));
            wuxc::Grid::SetRow(container, wuxc::Grid::GetRow(powerButton));
            wuxc::Grid::SetColumnSpan(container, wuxc::Grid::GetColumnSpan(powerButton));
            wuxc::Grid::SetRowSpan(container, wuxc::Grid::GetRowSpan(powerButton));
        }

        parent.Children().Append(container);
        g_container = container;
    }

    container.Visibility(wux::Visibility::Visible);
    container.HorizontalAlignment(AlignmentSetting());
    container.Margin(wux::Thickness{ 0, 0, 0, 0 });

    // A row with the right number of buttons is kept unless the settings changed under it.
    if (!g_rebuild.exchange(false) && container.Children().Size() == buttons.size())
    {
        return;
    }

    container.Children().Clear();
    for (size_t i = 0; i < buttons.size(); ++i)
    {
        const ButtonDefinition* def = FindDefinition(buttons[i]);
        if (def)
        {
            container.Children().Append(MakeButton(def->action, def->glyph, i + 1 == buttons.size()));
        }
    }

    SP_Log(L"Power row built with %u button(s)", (unsigned)container.Children().Size());
}

// Puts the Start menu back the way it was: the row is removed and the shell's power button shown again.
void RestoreOriginal()
{
    try
    {
        auto panel = g_parentPanel.get();
        auto container = g_container.get();
        if (panel && container)
        {
            uint32_t index = 0;
            if (panel.Children().IndexOf(container, index))
            {
                panel.Children().RemoveAt(index);
            }
        }
        if (auto powerButton = g_originalPowerButton.get())
        {
            powerButton.Visibility(wux::Visibility::Visible);
        }
    }
    catch (winrt::hresult_error const& e)
    {
        SP_LogError(L"The Start menu could not be restored: 0x%08X", (unsigned)e.code());
    }
    catch (...)
    {
        SP_LogError(L"The Start menu could not be restored");
    }

    g_parentPanel = nullptr;
    g_originalPowerButton = nullptr;
    g_container = nullptr;
}

// Depth-first search for the element the shell names "PowerButton".
wux::FrameworkElement FindPowerButton(wux::DependencyObject const& from)
{
    if (!from)
    {
        return nullptr;
    }

    const int count = wuxm::VisualTreeHelper::GetChildrenCount(from);
    for (int i = 0; i < count; ++i)
    {
        auto child = wuxm::VisualTreeHelper::GetChild(from, i);
        if (auto element = child.try_as<wux::FrameworkElement>())
        {
            if (element.Name() == L"PowerButton")
            {
                return element;
            }
        }
        if (auto found = FindPowerButton(child))
        {
            return found;
        }
    }
    return nullptr;
}

// TRUE when the row is in place. The cached elements are used while they are alive and attached; otherwise the
// tree is searched again, which also covers a Start menu that was rebuilt since the last time.
bool TryInject()
{
    if (g_unloading.load(std::memory_order_relaxed))
    {
        return false;
    }

    try
    {
        auto panel = g_parentPanel.get();
        auto powerButton = g_originalPowerButton.get();
        bool cachedIsLive = false;
        if (panel && powerButton)
        {
            try
            {
                cachedIsLive = SameObject(wuxm::VisualTreeHelper::GetParent(powerButton), panel);
            }
            catch (...)
            {
            }
        }

        if (!cachedIsLive)
        {
            auto window = wux::Window::Current();
            if (!window)
            {
                return false;
            }
            auto content = window.Content();
            if (!content)
            {
                return false;
            }

            powerButton = FindPowerButton(content);
            if (!powerButton)
            {
                SP_LogDebug(L"No PowerButton in this window's tree");
                return false;
            }

            panel = wuxm::VisualTreeHelper::GetParent(powerButton).try_as<wuxc::Panel>();
            if (!panel)
            {
                SP_LogDebug(L"The power button's parent is not a panel");
                return false;
            }

            g_parentPanel = panel;
            g_originalPowerButton = powerButton;
            g_rebuild.store(true, std::memory_order_relaxed);
        }

        InjectButtons(panel, powerButton);
        return true;
    }
    catch (winrt::hresult_error const& e)
    {
        SP_LogError(L"The power row could not be added: 0x%08X", (unsigned)e.code());
    }
    catch (...)
    {
        SP_LogError(L"The power row could not be added");
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------
// Watching a CoreWindow
//
// A CoreWindow the mod is interested in is subclassed, and every piece of XAML work is posted to it as a
// control message, so it always runs on the window's own thread: the one XAML allows.
// ---------------------------------------------------------------------------------------------------------------

constexpr UINT_PTR kSubclassId = 0x5350'5742;      // 'SPWB'
constexpr UINT_PTR kRetryTimer = 0x5350'5743;
constexpr int      kMaxRetries = 50;               // 50 x 100 ms: XAML has always been up long before this

// wParam of the control message.
enum : WPARAM
{
    kProbe   = 1,   // attach to the thread's CoreWindow; retried until XAML is ready
    kRefresh = 2,   // (re)build the row if the window is showing the Start menu
    kDetach  = 3,   // restore the shell and stop watching
};

UINT g_controlMessage = 0;

struct Watch
{
    HWND                hwnd = nullptr;
    int                 retries = 0;
    bool                monitoring = false;
    wuc::CoreWindow     coreWindow{ nullptr };
    winrt::event_token  visibilityToken{};
    winrt::event_token  activatedToken{};
};

std::mutex        g_watchLock;
std::vector<HWND> g_watched;

bool Remember(HWND hwnd)
{
    std::lock_guard<std::mutex> lock(g_watchLock);
    for (HWND h : g_watched)
    {
        if (h == hwnd)
        {
            return false;
        }
    }
    g_watched.push_back(hwnd);
    return true;
}

void Forget(HWND hwnd)
{
    std::lock_guard<std::mutex> lock(g_watchLock);
    for (size_t i = 0; i < g_watched.size(); ++i)
    {
        if (g_watched[i] == hwnd)
        {
            g_watched.erase(g_watched.begin() + i);
            return;
        }
    }
}

std::vector<HWND> WatchedWindows()
{
    std::lock_guard<std::mutex> lock(g_watchLock);
    return g_watched;
}

// Hooks the thread's CoreWindow events. Fails while XAML has not set the CoreWindow up yet, in which case the
// caller retries a little later.
bool StartMonitoring(Watch* watch)
{
    try
    {
        wuc::CoreWindow coreWindow = wuc::CoreWindow::GetForCurrentThread();
        if (!coreWindow)
        {
            return false;
        }

        const HWND hwnd = watch->hwnd;

        // Both handlers only post back to the window: the tree is touched from the message, not from inside
        // the event.
        watch->visibilityToken = coreWindow.VisibilityChanged(
            [hwnd](IInspectable const&, wuc::VisibilityChangedEventArgs const& args) {
                try
                {
                    if (args.Visible())
                    {
                        PostMessageW(hwnd, g_controlMessage, kRefresh, 0);
                    }
                }
                catch (...)
                {
                }
            });

        watch->activatedToken = coreWindow.Activated(
            [hwnd](IInspectable const&, wuc::WindowActivatedEventArgs const& args) {
                try
                {
                    if (args.WindowActivationState() != wuc::CoreWindowActivationState::Deactivated)
                    {
                        PostMessageW(hwnd, g_controlMessage, kRefresh, 0);
                    }
                }
                catch (...)
                {
                }
            });

        watch->coreWindow = coreWindow;
        watch->monitoring = true;

        if (coreWindow.Visible())
        {
            PostMessageW(hwnd, g_controlMessage, kRefresh, 0);
        }

        SP_Log(L"Watching CoreWindow %p", hwnd);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void StopMonitoring(Watch* watch)
{
    if (!watch->monitoring)
    {
        return;
    }
    try
    {
        if (watch->coreWindow)
        {
            if (watch->visibilityToken)
            {
                watch->coreWindow.VisibilityChanged(watch->visibilityToken);
            }
            if (watch->activatedToken)
            {
                watch->coreWindow.Activated(watch->activatedToken);
            }
        }
    }
    catch (...)
    {
    }
    watch->visibilityToken = {};
    watch->activatedToken = {};
    watch->coreWindow = nullptr;
    watch->monitoring = false;
}

void OnProbe(Watch* watch)
{
    if (g_unloading.load(std::memory_order_relaxed))
    {
        return;
    }
    if (watch->monitoring)
    {
        return;
    }
    if (StartMonitoring(watch))
    {
        return;
    }
    if (++watch->retries < kMaxRetries)
    {
        SetTimer(watch->hwnd, kRetryTimer, 100, nullptr);
    }
    else
    {
        SP_LogDebug(L"Window %p never got a CoreWindow for its thread; left alone", watch->hwnd);
    }
}

void OnRefresh(Watch* watch)
{
    if (g_unloading.load(std::memory_order_relaxed) || !watch->monitoring)
    {
        return;
    }
    if (!g_enabled.load(std::memory_order_relaxed))
    {
        RestoreOriginal();
        return;
    }
    TryInject();
}

LRESULT CALLBACK CoreWindowSubclass(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                    UINT_PTR idSubclass, DWORD_PTR refData)
{
    Watch* watch = (Watch*)refData;

    if (msg == g_controlMessage && g_controlMessage != 0 && watch)
    {
        try
        {
            switch (wParam)
            {
            case kProbe:
                OnProbe(watch);
                break;

            case kRefresh:
                OnRefresh(watch);
                break;

            case kDetach:
                KillTimer(hwnd, kRetryTimer);
                RestoreOriginal();
                StopMonitoring(watch);
                RemoveWindowSubclass(hwnd, CoreWindowSubclass, idSubclass);
                Forget(hwnd);
                delete watch;
                break;
            }
        }
        catch (...)
        {
        }
        return 0;
    }

    switch (msg)
    {
    case WM_TIMER:
        if (wParam == kRetryTimer && watch)
        {
            KillTimer(hwnd, kRetryTimer);
            try
            {
                OnProbe(watch);
            }
            catch (...)
            {
            }
            return 0;
        }
        break;

    case WM_NCDESTROY:
        // The window and its XAML are going away; the tokens die with the CoreWindow, so nothing is unhooked.
        RemoveWindowSubclass(hwnd, CoreWindowSubclass, idSubclass);
        Forget(hwnd);
        if (watch)
        {
            try
            {
                watch->coreWindow = nullptr;
            }
            catch (...)
            {
            }
            delete watch;
        }
        break;
    }

    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// Starts watching one CoreWindow. Callable from any thread; the first probe is posted so that the XAML work
// happens on the window's thread once it is pumping messages.
void Attach(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd) || g_unloading.load(std::memory_order_relaxed))
    {
        return;
    }
    if (!Remember(hwnd))
    {
        return;
    }

    Watch* watch = new (std::nothrow) Watch();
    if (!watch)
    {
        Forget(hwnd);
        return;
    }
    watch->hwnd = hwnd;

    BOOL ok;
    if (GetWindowThreadProcessId(hwnd, nullptr) == GetCurrentThreadId())
    {
        ok = SetWindowSubclass(hwnd, CoreWindowSubclass, kSubclassId, (DWORD_PTR)watch);
    }
    else
    {
        ok = SP_SetWindowSubclassFromAnyThread(hwnd, CoreWindowSubclass, kSubclassId, (DWORD_PTR)watch);
    }

    if (!ok)
    {
        SP_LogError(L"CoreWindow %p could not be subclassed", hwnd);
        Forget(hwnd);
        delete watch;
        return;
    }

    SP_LogDebug(L"Attached to CoreWindow %p", hwnd);
    PostMessageW(hwnd, g_controlMessage, kProbe, 0);
}

bool IsCoreWindowClass(LPCWSTR className)
{
    // An atom rather than a name is never the class this wants.
    return className && !IS_INTRESOURCE(className) && _wcsicmp(className, L"Windows.UI.Core.CoreWindow") == 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Finding the CoreWindow
//
// Every CoreWindow is created through these two user32 exports, so hooking them catches the Start menu's as it
// appears. The ones that already exist are picked up by EnumWindows.
// ---------------------------------------------------------------------------------------------------------------

using CreateWindowInBand_t = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU,
                                           HINSTANCE, PVOID, DWORD);
using CreateWindowInBandEx_t = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU,
                                             HINSTANCE, PVOID, DWORD, DWORD);

CreateWindowInBand_t   g_origCreateWindowInBand = nullptr;
CreateWindowInBandEx_t g_origCreateWindowInBandEx = nullptr;

HWND WINAPI CreateWindowInBand_Hook(DWORD exStyle, LPCWSTR className, LPCWSTR windowName, DWORD style,
                                    int x, int y, int width, int height, HWND parent, HMENU menu,
                                    HINSTANCE instance, PVOID param, DWORD band)
{
    HWND hwnd = g_origCreateWindowInBand(exStyle, className, windowName, style, x, y, width, height, parent,
                                         menu, instance, param, band);
    if (hwnd && IsCoreWindowClass(className))
    {
        try
        {
            Attach(hwnd);
        }
        catch (...)
        {
        }
    }
    return hwnd;
}

HWND WINAPI CreateWindowInBandEx_Hook(DWORD exStyle, LPCWSTR className, LPCWSTR windowName, DWORD style,
                                      int x, int y, int width, int height, HWND parent, HMENU menu,
                                      HINSTANCE instance, PVOID param, DWORD band, DWORD typeFlags)
{
    HWND hwnd = g_origCreateWindowInBandEx(exStyle, className, windowName, style, x, y, width, height, parent,
                                           menu, instance, param, band, typeFlags);
    if (hwnd && IsCoreWindowClass(className))
    {
        try
        {
            Attach(hwnd);
        }
        catch (...)
        {
        }
    }
    return hwnd;
}

BOOL InstallWindowHooks()
{
    if (!SP_HookBegin())
    {
        return FALSE;
    }

    const BOOL band = SP_SetExportHook(L"user32.dll", "CreateWindowInBand",
                                       CreateWindowInBand_Hook, &g_origCreateWindowInBand);
    // The Ex form is newer and not on every build; either one is enough to see the window.
    const BOOL bandEx = SP_SetExportHook(L"user32.dll", "CreateWindowInBandEx",
                                         CreateWindowInBandEx_Hook, &g_origCreateWindowInBandEx);
    if (!band && !bandEx)
    {
        SP_HookAbort();
        SP_LogError(L"user32 does not export CreateWindowInBand on this build");
        return FALSE;
    }

    if (!SP_HookCommit())
    {
        return FALSE;
    }

    SP_Log(L"Watching for CoreWindows (%s%s)", band ? L"CreateWindowInBand" : L"",
           bandEx ? L" CreateWindowInBandEx" : L"");
    return TRUE;
}

BOOL CALLBACK AttachExistingCoreWindow(HWND hwnd, LPARAM)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId())
    {
        return TRUE;
    }

    wchar_t className[64] = {};
    if (GetClassNameW(hwnd, className, ARRAYSIZE(className)) && IsCoreWindowClass(className))
    {
        Attach(hwnd);
    }
    return TRUE;
}

// Runs on the engine's helper thread once StartDocked.dll is in this process.
void OnStartMenuLibraryLoaded(HMODULE, void*)
{
    if (g_unloading.load(std::memory_order_relaxed))
    {
        return;
    }

    SP_Log(L"StartDocked.dll is in this process; the Start menu XAML may be reachable");

    if (!InstallWindowHooks())
    {
        return;
    }

    EnumWindows(AttachExistingCoreWindow, 0);
}

// ---------------------------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------------------------

bool KeywordMatches(const std::wstring& word, const wchar_t* keyword)
{
    return _wcsicmp(word.c_str(), keyword) == 0;
}

// Builds the list of buttons: the toggles say which, the Order setting says in what order (any it leaves out
// follow in the default order).
void BuildButtonList()
{
    bool shown[(int)PowerAction::Count] = {};
    for (const auto& def : kButtons)
    {
        shown[(int)def.action] = SP_GetIntSetting(def.settingName, def.shownByDefault) != 0;
    }

    wchar_t order[256] = {};
    SP_GetStringSetting(L"Order", order, ARRAYSIZE(order), L"");

    std::vector<PowerAction> list;
    bool placed[(int)PowerAction::Count] = {};

    std::wstring word;
    for (const wchar_t* p = order; ; ++p)
    {
        const wchar_t c = *p;
        if (c == L',' || c == L';' || c == L' ' || c == L'\0')
        {
            if (!word.empty())
            {
                for (const auto& def : kButtons)
                {
                    const int index = (int)def.action;
                    if (KeywordMatches(word, def.keyword) && !placed[index])
                    {
                        placed[index] = true;
                        if (shown[index])
                        {
                            list.push_back(def.action);
                        }
                        break;
                    }
                }
                word.clear();
            }
            if (c == L'\0')
            {
                break;
            }
        }
        else
        {
            word += c;
        }
    }

    for (const auto& def : kButtons)
    {
        const int index = (int)def.action;
        if (!placed[index] && shown[index])
        {
            list.push_back(def.action);
        }
    }

    std::lock_guard<std::mutex> lock(g_buttonsLock);
    g_buttons.swap(list);
}

void LoadSettings()
{
    g_enabled.store(SP_GetIntSetting(L"Enabled", 1) != 0, std::memory_order_relaxed);
    g_confirm.store(SP_GetIntSetting(L"ConfirmBeforeAction", 0) != 0, std::memory_order_relaxed);
    g_forceClose.store(SP_GetIntSetting(L"ForceClose", 0) != 0, std::memory_order_relaxed);
    g_alignment.store(SP_GetIntSetting(L"Alignment", 0), std::memory_order_relaxed);
    BuildButtonList();

    SP_LogDebug(L"Settings: enabled=%d confirm=%d force=%d alignment=%d buttons=%u",
                (int)g_enabled.load(), (int)g_confirm.load(), (int)g_forceClose.load(),
                g_alignment.load(), (unsigned)CurrentButtons().size());
}

// ---------------------------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------------------------

BOOL Init()
{
    LoadSettings();

    g_controlMessage = RegisterWindowMessageW(L"ShadePatcher.StartPowerButtons.Control");
    if (!g_controlMessage)
    {
        return FALSE;
    }

    // No timeout: on a build that hosts the Start menu in explorer.exe its library is loaded the first time the
    // menu opens, which may be long after sign-in. On a build that does not, the wait costs one module lookup
    // every half second and nothing is ever hooked.
    if (!SP_WaitForModule(L"StartDocked.dll", 0, OnStartMenuLibraryLoaded, nullptr))
    {
        SP_LogError(L"The wait for the Start menu library could not be set up");
        return FALSE;
    }

    SP_Log(L"Waiting for the Start menu library (StartDocked.dll) to be in this process");
    return TRUE;
}

void SettingsChanged()
{
    LoadSettings();
    g_rebuild.store(true, std::memory_order_relaxed);

    for (HWND hwnd : WatchedWindows())
    {
        PostMessageW(hwnd, g_controlMessage, kRefresh, 0);
    }
}

void BeforeUninit()
{
    g_unloading.store(true, std::memory_order_relaxed);

    // The row must be gone and the subclass removed before this code is unloaded; the detach runs on each
    // window's own thread and this waits for it. A thread that does not answer keeps a subclass pointing at a
    // procedure about to vanish, so its subclass is pulled from here as a last resort.
    for (HWND hwnd : WatchedWindows())
    {
        if (!IsWindow(hwnd))
        {
            Forget(hwnd);
            continue;
        }

        DWORD_PTR result = 0;
        if (!SendMessageTimeoutW(hwnd, g_controlMessage, kDetach, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 3000, &result))
        {
            SP_LogError(L"CoreWindow %p did not answer; removing the subclass from outside", hwnd);
            SP_RemoveWindowSubclassFromAnyThread(hwnd, CoreWindowSubclass, kSubclassId);
            Forget(hwnd);
        }
    }
}

void Uninit()
{
    // The engine has removed the hooks and cancelled the module wait; only the references are left.
    g_parentPanel = nullptr;
    g_originalPowerButton = nullptr;
    g_container = nullptr;
}

}   // namespace

SP_MOD_DEFINE(g_modStartPowerButtons) =
{
    /* id             */ SP_MOD_ID,
    /* name           */ L"One-click power buttons in the Start menu",
    /* basedOn        */ "win11-power-buttons",
    /* originalAuthor */ "Hakuuyosei",
    /* targets        */ SP_TARGET_EXPLORER,
    /* minOsBuild     */ 22000,
    /* flags          */ 0,
    /* Init           */ Init,
    /* AfterInit      */ nullptr,
    /* SettingsChanged*/ SettingsChanged,
    /* BeforeUninit   */ BeforeUninit,
    /* Uninit         */ Uninit,
};
