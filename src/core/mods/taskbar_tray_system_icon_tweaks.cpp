//
// taskbar-tray-system-icon-tweaks - hides chosen system icons in the Windows 11 taskbar tray and sizes the
// "Show desktop" button.
//
// Adapted from the idea behind the Windhawk mod "Taskbar tray system icon tweaks" (taskbar-tray-system-icon-tweaks)
// by m417z. The logic here is written against this engine's API and keeps the Windows 11 XAML taskbar behaviour
// only.
//
// What it does
// ------------
// The tray's system icons are XAML elements, one SystemTray.IconView per icon, grouped in five containers under
// SystemTray.SystemTrayFrame > SystemTrayFrameGrid:
//
//     MainStack                 microphone / location ("an app is using your ...") icons
//     NonActivatableStack       the language bar (ENG, TR, ...) and input-method helper icons
//     ControlCenterButton       the volume, network and battery icons of the quick settings button
//     NotificationCenterButton  the bell
//     ShowDesktopStack          the thin "Show desktop" strip at the far end
//
// The mod collapses the content of the icons the user asked to hide, keeps the button clickable only while
// something is left in it, hides the bell always or only while it is empty, and pins the "Show desktop" strip
// to a width of the user's choice (0 hides it). Everything is applied live and undone when the mod is turned
// off.
//
// Where it hooks
// --------------
// Every tray icon goes through
//     winrt::SystemTray::implementation::IconView::IconView()
// so that constructor is hooked: the new element is watched for its Loaded event, and once it sits in the tree
// its container says which rule applies. On this build (26200, Taskbar.View.dll 2607) that class lives in
// SystemTray.dll; before version 2604 it was in Taskbar.View.dll. The mod waits for Taskbar.View.dll and then
// hooks whichever of the two holds the class, waiting for SystemTray.dll if it is not in yet.
//
// A settings change, and enabling the mod while the taskbar is already up, cannot rely on constructors that
// already ran. For that the taskbar's XAML root is reached from its window through taskbar.dll
// (CTaskBand::GetTaskbarHost, resolved only, never hooked) and the whole tray is walked on the taskbar's own
// thread. When those symbols are missing the elements met through the hook are remembered as a way back in.
//
// Icons that change while shown (the microphone/location glyph, the bell going from empty to full) are followed
// through property-changed callbacks, so a rule keeps holding without a new pass.
//
// Exceptions
// ----------
// C++/WinRT reports failures by throwing, so this file is compiled with exceptions on. Every hook body, event
// handler and dispatched callback is wrapped: nothing may escape into the shell.
//
#define SP_MOD_ID "taskbar-tray-system-icon-tweaks"
#include "engine/modapi.h"

#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <vector>

#undef GetCurrentTime

#include <unknwn.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>

namespace {

using namespace winrt::Windows::UI::Xaml;
using winrt::Windows::Foundation::IInspectable;
using winrt::Windows::UI::Core::CoreDispatcher;
using winrt::Windows::UI::Core::CoreDispatcherPriority;
using winrt::Windows::UI::Xaml::Controls::Control;
using winrt::Windows::UI::Xaml::Controls::TextBlock;
using winrt::Windows::UI::Xaml::Media::VisualTreeHelper;

// ---------------------------------------------------------------------------------------------------------------
// Settings and shared state
// ---------------------------------------------------------------------------------------------------------------

enum BellMode
{
    kBellNever   = 0,   // never hidden
    kBellWhenNoNew = 1, // hidden while there are no new notifications (empty bell, with or without "do not disturb")
    kBellAlways  = 2,   // always hidden
};

struct Settings
{
    std::atomic<bool> hideVolume{ false };
    std::atomic<bool> hideNetwork{ false };
    std::atomic<bool> hideBattery{ false };
    std::atomic<bool> hideMicrophone{ false };
    std::atomic<bool> hideLocation{ false };
    std::atomic<bool> hideLanguageBar{ false };
    std::atomic<int>  bellMode{ kBellNever };
    std::atomic<bool> hideShowDesktop{ false };
    std::atomic<int>  showDesktopWidth{ 0 };   // 0 = leave the shell's width alone
};

Settings g_settings;

// Set in BeforeUninit: from then on every pass restores instead of applies, and no new watcher is registered.
std::atomic<bool> g_unloading{ false };
std::atomic<bool> g_trayHooked{ false };
std::atomic<bool> g_taskbarDllReady{ false };
std::atomic<int>  g_pendingDispatches{ 0 };

// The module holding IconView, for the sanity check in the constructor hook.
std::atomic<BYTE*> g_trayModuleBase{ nullptr };
std::atomic<SIZE_T> g_trayModuleSize{ 0 };

void LoadSettings()
{
    g_settings.hideVolume.store(SP_GetIntSetting(L"HideVolume", 0) != 0, std::memory_order_relaxed);
    g_settings.hideNetwork.store(SP_GetIntSetting(L"HideNetwork", 0) != 0, std::memory_order_relaxed);
    g_settings.hideBattery.store(SP_GetIntSetting(L"HideBattery", 0) != 0, std::memory_order_relaxed);
    g_settings.hideMicrophone.store(SP_GetIntSetting(L"HideMicrophone", 0) != 0, std::memory_order_relaxed);
    g_settings.hideLocation.store(SP_GetIntSetting(L"HideLocation", 0) != 0, std::memory_order_relaxed);
    g_settings.hideLanguageBar.store(SP_GetIntSetting(L"HideLanguageBar", 0) != 0, std::memory_order_relaxed);

    int bell = SP_GetIntSetting(L"BellMode", kBellNever);
    if (bell < kBellNever || bell > kBellAlways)
    {
        bell = kBellNever;
    }
    g_settings.bellMode.store(bell, std::memory_order_relaxed);

    g_settings.hideShowDesktop.store(SP_GetIntSetting(L"HideShowDesktopButton", 0) != 0, std::memory_order_relaxed);

    int width = SP_GetIntSetting(L"ShowDesktopButtonWidth", 0);
    if (width < 0 || width > 200)
    {
        width = 0;
    }
    g_settings.showDesktopWidth.store(width, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------------------------------
// Visual tree helpers
// ---------------------------------------------------------------------------------------------------------------

// The first child for which `match` returns true, or null. A null parent yields null, so lookups chain.
template <typename F>
FrameworkElement FirstChild(FrameworkElement const& parent, F&& match)
{
    if (!parent)
    {
        return nullptr;
    }
    const int count = VisualTreeHelper::GetChildrenCount(parent);
    for (int i = 0; i < count; ++i)
    {
        auto child = VisualTreeHelper::GetChild(parent, i).try_as<FrameworkElement>();
        if (child && match(child))
        {
            return child;
        }
    }
    return nullptr;
}

FrameworkElement ChildByName(FrameworkElement const& parent, const wchar_t* name)
{
    return FirstChild(parent, [name](FrameworkElement const& child) { return child.Name() == name; });
}

FrameworkElement ChildByClass(FrameworkElement const& parent, const wchar_t* className)
{
    return FirstChild(parent, [className](FrameworkElement const& child) {
        return winrt::get_class_name(child) == className;
    });
}

FrameworkElement ParentOf(FrameworkElement const& element)
{
    if (!element)
    {
        return nullptr;
    }
    auto parent = VisualTreeHelper::GetParent(element);
    return parent ? parent.try_as<FrameworkElement>() : nullptr;
}

FrameworkElement AncestorByName(FrameworkElement const& element, const wchar_t* name)
{
    FrameworkElement current = ParentOf(element);
    for (int depth = 0; current && depth < 64; ++depth)
    {
        if (current.Name() == name)
        {
            return current;
        }
        current = ParentOf(current);
    }
    return nullptr;
}

bool IsClass(FrameworkElement const& element, const wchar_t* className)
{
    return element && winrt::get_class_name(element) == className;
}

constexpr wchar_t kContentPresenterClass[] = L"Windows.UI.Xaml.Controls.ContentPresenter";
constexpr wchar_t kStackPanelClass[]       = L"Windows.UI.Xaml.Controls.StackPanel";
constexpr wchar_t kItemsPresenterClass[]   = L"Windows.UI.Xaml.Controls.ItemsPresenter";
constexpr wchar_t kGridClass[]             = L"Windows.UI.Xaml.Controls.Grid";

// ---------------------------------------------------------------------------------------------------------------
// Telling the icons apart
//
// Each text icon is a single glyph from Segoe Fluent Icons (or the tray's own AXPIcons font), so the character
// says what the icon is. The ranges are the ones the original mod collected.
// ---------------------------------------------------------------------------------------------------------------

enum class IconKind
{
    Unknown,
    None,                  // empty text: the icon is on its way out
    Volume,
    Network,
    Battery,
    Microphone,
    Location,
    MicrophoneAndLocation,
    BellEmpty,
    BellEmptyDnd,
    BellFull,
    BellFullDnd,
    Language,
    StudioEffects,
    Recall,
};

IconKind IdentifyIcon(winrt::hstring const& text)
{
    if (text.empty())
    {
        return IconKind::None;
    }
    if (text.size() != 1)
    {
        return IconKind::Unknown;
    }

    const unsigned c = text[0];

    // Volume: Mute, Volume0..3, VolumeDisabled, VolumeBars.
    if (c == 0xE74F || (c >= 0xE992 && c <= 0xE995) || c == 0xEA85 || c == 0xEBC5)
    {
        return IconKind::Volume;
    }

    // Network: Airplane, TVMonitor, Ethernet, SignalBars1..5, the cellular glyphs, SignalRoaming, MobWifi1..4,
    // NetworkOffline and the Sys* family (F8C0..F8CC).
    if (c == 0xE709 || c == 0xE7F4 || c == 0xE839 || (c >= 0xE86C && c <= 0xE870) ||
        (c >= 0xEAA1 && c <= 0xEAA5) || c == 0xEAA8 || c == 0xEC1E || (c >= 0xEC3C && c <= 0xEC3F) ||
        c == 0xF384 || (c >= 0xF8C0 && c <= 0xF8CC))
    {
        return IconKind::Network;
    }

    // Battery: the private-use charging levels, MobBattery0..10, MobBatteryCharging0..10, MobBatterySaver0..10
    // and three loose glyphs.
    if ((c >= 0xE3C1 && c <= 0xE3CB) || (c >= 0xE408 && c <= 0xE41D) || (c >= 0xEBA0 && c <= 0xEBC0) ||
        c == 0xEB17 || c == 0xEC02 || c == 0xF1E8)
    {
        return IconKind::Battery;
    }

    switch (c)
    {
    case 0xE361:
    case 0xE720:    // Microphone
    case 0xEC71:    // MicOn
        return IconKind::Microphone;

    case 0xE37A:
        return IconKind::Location;

    case 0xF47F:
        return IconKind::MicrophoneAndLocation;

    case 0xF2A3:    // empty bell
        return IconKind::BellEmpty;
    case 0xF285:    // empty bell, do not disturb
        return IconKind::BellEmptyDnd;
    case 0xF2A5:    // full bell
        return IconKind::BellFull;
    case 0xF2A8:    // full bell, do not disturb
        return IconKind::BellFullDnd;

    // Input-method helper glyphs (half/full width, hiragana/katakana, private mode ...).
    case 0xE4D7:
    case 0xE4D8:
    case 0xE5BF:
    case 0xE97E:
    case 0xE97F:
    case 0xE980:
    case 0xE982:
    case 0xE983:
    case 0xE986:
    case 0xE987:
    case 0xE988:
    case 0xEB90:
    case 0xEE41:
    case 0xEE42:
    case 0xEE43:
    case 0xEE44:
    case 0xEE45:
    case 0xEE75:
    case 0xEE76:
        return IconKind::Language;

    case 0xEABC:
        return IconKind::StudioEffects;

    case 0xEC83:
    case 0xEADD:
    case 0xEB16:
    case 0xEF97:
    case 0xF1C6:
        return IconKind::Recall;

    default:
        break;
    }

    return IconKind::Unknown;
}

// The TextBlock that carries the glyph of a SystemTray.TextIconContent: ContainerGrid > Base > InnerTextBlock.
TextBlock InnerTextBlockOf(FrameworkElement const& textIconContent)
{
    FrameworkElement element = ChildByName(textIconContent, L"ContainerGrid");
    element = ChildByName(element, L"Base");
    element = ChildByName(element, L"InnerTextBlock");
    return element ? element.try_as<TextBlock>() : nullptr;
}

unsigned FirstChar(winrt::hstring const& text)
{
    return text.empty() ? 0u : (unsigned)text[0];
}

// ---------------------------------------------------------------------------------------------------------------
// Property watchers
//
// A glyph that changes while the icon is shown (the microphone icon giving way to the microphone-and-location
// one, the bell filling up) must re-evaluate its rule. XAML tells us through a property-changed callback; the
// registrations are remembered so that a settings pass and the unload can take them off again. A callback can
// only be removed on its element's thread, so removal is done per pass, for the elements of that thread.
// ---------------------------------------------------------------------------------------------------------------

struct Watcher
{
    winrt::weak_ref<DependencyObject> element;
    DependencyProperty                property{ nullptr };
    int64_t                           token = 0;
};

std::mutex           g_watchLock;
std::vector<Watcher> g_watchers;

bool IsWatched(DependencyObject const& element, DependencyProperty const& property)
{
    std::lock_guard<std::mutex> lock(g_watchLock);
    for (auto const& w : g_watchers)
    {
        if (w.property == property && w.element.get() == element)
        {
            return true;
        }
    }
    return false;
}

void RememberWatcher(DependencyObject const& element, DependencyProperty const& property, int64_t token)
{
    std::lock_guard<std::mutex> lock(g_watchLock);
    Watcher w;
    w.element = winrt::make_weak(element);
    w.property = property;
    w.token = token;
    g_watchers.push_back(std::move(w));
}

// UI thread. Takes off every watcher whose element belongs to this thread, and forgets the dead ones.
void RemoveWatchersOnThisThread()
{
    std::vector<Watcher> mine;
    {
        std::lock_guard<std::mutex> lock(g_watchLock);
        for (size_t i = 0; i < g_watchers.size();)
        {
            bool drop = true;
            try
            {
                if (auto element = g_watchers[i].element.get())
                {
                    if (element.Dispatcher().HasThreadAccess())
                    {
                        mine.push_back(g_watchers[i]);
                    }
                    else
                    {
                        drop = false;   // another taskbar's thread will handle it
                    }
                }
            }
            catch (...)
            {
            }

            if (drop)
            {
                g_watchers.erase(g_watchers.begin() + (ptrdiff_t)i);
            }
            else
            {
                ++i;
            }
        }
    }

    for (auto const& w : mine)
    {
        try
        {
            if (auto element = w.element.get())
            {
                element.UnregisterPropertyChangedCallback(w.property, w.token);
            }
        }
        catch (...)
        {
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Rules, one per container
// ---------------------------------------------------------------------------------------------------------------

bool MainStackShouldHide(IconKind kind)
{
    if (g_unloading.load(std::memory_order_relaxed))
    {
        return false;
    }
    switch (kind)
    {
    case IconKind::Microphone:
        return g_settings.hideMicrophone.load(std::memory_order_relaxed);
    case IconKind::Location:
        return g_settings.hideLocation.load(std::memory_order_relaxed);
    case IconKind::MicrophoneAndLocation:
        return g_settings.hideMicrophone.load(std::memory_order_relaxed) &&
               g_settings.hideLocation.load(std::memory_order_relaxed);
    default:
        // Studio Effects, Recall, an icon on its way out, or something new: left as the shell shows it.
        return false;
    }
}

// MainStack: the microphone and location icons. The whole IconView is collapsed, and its glyph is watched so
// that a change between microphone, location and the combined icon re-applies the rule.
void ApplyMainStackIcon(FrameworkElement const& iconView)
{
    FrameworkElement element = ChildByName(iconView, L"ContainerGrid");
    element = ChildByName(element, L"ContentPresenter");
    element = ChildByName(element, L"ContentGrid");
    element = ChildByClass(element, L"SystemTray.TextIconContent");
    TextBlock text = InnerTextBlockOf(element);
    if (!text)
    {
        SP_LogDebug(L"A main stack icon has no text block");
        return;
    }

    const auto glyph = text.Text();
    const IconKind kind = IdentifyIcon(glyph);
    const bool hide = MainStackShouldHide(kind);
    SP_LogDebug(L"Main stack icon U+%04X kind %d: hide=%d", FirstChar(glyph), (int)kind, hide ? 1 : 0);

    iconView.Visibility(hide ? Visibility::Collapsed : Visibility::Visible);

    if (g_unloading.load(std::memory_order_relaxed))
    {
        return;
    }

    const DependencyProperty property = TextBlock::TextProperty();
    if (IsWatched(text, property))
    {
        return;
    }

    auto weakView = winrt::make_weak(iconView);
    const int64_t token = text.RegisterPropertyChangedCallback(
        property, [weakView](DependencyObject const& sender, DependencyProperty const&) {
            try
            {
                if (g_unloading.load(std::memory_order_relaxed))
                {
                    return;
                }
                auto block = sender.try_as<TextBlock>();
                auto view = weakView.get();
                if (!block || !view)
                {
                    return;
                }
                const bool hideNow = MainStackShouldHide(IdentifyIcon(block.Text()));
                view.Visibility(hideNow ? Visibility::Collapsed : Visibility::Visible);
            }
            catch (...)
            {
            }
        });
    RememberWatcher(text, property, token);
}

// NonActivatableStack: the language bar. An IconView here carries either the main language icon
// (SystemTray.LanguageTextIconContent / LanguageImageIconContent) or one of the input-method helper glyphs
// (TextIconContent / ImageIconContent). Only the main icon is subject to the setting; the helpers stay.
void ApplyLanguageBarIcon(FrameworkElement const& iconView)
{
    FrameworkElement contentGrid = ChildByName(iconView, L"ContainerGrid");
    contentGrid = ChildByName(contentGrid, L"ContentPresenter");
    contentGrid = ChildByName(contentGrid, L"ContentGrid");

    bool hide = false;
    bool isMainIcon = false;
    FrameworkElement found = FirstChild(contentGrid, [&hide, &isMainIcon](FrameworkElement const& child) {
        const auto className = winrt::get_class_name(child);
        if (className == L"SystemTray.LanguageTextIconContent" || className == L"SystemTray.LanguageImageIconContent")
        {
            isMainIcon = true;
            hide = g_settings.hideLanguageBar.load(std::memory_order_relaxed);
            return true;
        }
        if (className == L"SystemTray.TextIconContent" || className == L"SystemTray.ImageIconContent")
        {
            hide = false;
            return true;
        }
        return false;
    });

    if (!found)
    {
        // An input method that switches from a text icon to an image icon while the element is collapsed
        // leaves it empty rather than repopulating it, and it would then stay invisible for good. Making an
        // empty element visible lets the shell fill it again (the original mod's fix).
        if (VisualTreeHelper::GetChildrenCount(iconView) == 0)
        {
            iconView.Visibility(Visibility::Visible);
        }
        return;
    }

    hide = hide && !g_unloading.load(std::memory_order_relaxed);
    SP_LogDebug(L"Language bar %s icon: hide=%d", isMainIcon ? L"main" : L"helper", hide ? 1 : 0);
    iconView.Visibility(hide ? Visibility::Collapsed : Visibility::Visible);
}

// ControlCenterButton: volume, network and battery. The icon's content is collapsed rather than the IconView,
// the IconView is disabled so the flyout no longer opens from it, and when nothing in the button is left
// enabled the whole strip is collapsed too.
void ApplyControlCenterIcon(FrameworkElement const& iconView)
{
    FrameworkElement contentGrid = ChildByName(iconView, L"ContainerGrid");
    contentGrid = ChildByName(contentGrid, L"ContentGrid");
    if (!contentGrid)
    {
        SP_LogDebug(L"A quick settings icon has no content grid");
        return;
    }

    const bool unloading = g_unloading.load(std::memory_order_relaxed);
    bool hide = false;

    FrameworkElement content = ChildByClass(contentGrid, L"SystemTray.BatteryIconContent");
    if (content)
    {
        hide = !unloading && g_settings.hideBattery.load(std::memory_order_relaxed);
        SP_LogDebug(L"Battery icon: hide=%d", hide ? 1 : 0);
    }
    else
    {
        content = ChildByClass(contentGrid, L"SystemTray.TextIconContent");
        TextBlock text = InnerTextBlockOf(content);
        if (!text)
        {
            SP_LogDebug(L"A quick settings icon has no text block");
            return;
        }

        const auto glyph = text.Text();
        const IconKind kind = IdentifyIcon(glyph);
        if (!unloading)
        {
            switch (kind)
            {
            case IconKind::Volume:
                hide = g_settings.hideVolume.load(std::memory_order_relaxed);
                break;
            case IconKind::Network:
                hide = g_settings.hideNetwork.load(std::memory_order_relaxed);
                break;
            case IconKind::Battery:
                hide = g_settings.hideBattery.load(std::memory_order_relaxed);
                break;
            default:
                break;
            }
        }
        SP_LogDebug(L"Quick settings icon U+%04X kind %d: hide=%d", FirstChar(glyph), (int)kind, hide ? 1 : 0);
    }

    const bool hidden = content.Visibility() == Visibility::Collapsed;
    if (hide == hidden)
    {
        return;
    }

    content.Visibility(hide ? Visibility::Collapsed : Visibility::Visible);
    if (auto control = iconView.try_as<Control>())
    {
        control.IsEnabled(!hide);
    }

    // The strip the icons sit in: IconView < ContentPresenter < StackPanel.
    FrameworkElement presenter = ParentOf(iconView);
    if (!IsClass(presenter, kContentPresenterClass))
    {
        return;
    }
    FrameworkElement stack = ParentOf(presenter);
    if (!IsClass(stack, kStackPanelClass))
    {
        return;
    }

    bool anyEnabled = false;
    FirstChild(stack, [&anyEnabled](FrameworkElement const& child) {
        if (!IsClass(child, kContentPresenterClass))
        {
            return false;
        }
        FrameworkElement icon = ChildByName(child, L"SystemTrayIcon");
        if (!icon)
        {
            return false;
        }
        if (auto control = icon.try_as<Control>())
        {
            if (control.IsEnabled())
            {
                anyEnabled = true;
                return true;
            }
        }
        return false;
    });
    stack.Visibility(anyEnabled ? Visibility::Visible : Visibility::Collapsed);
}

bool BellShouldHideWhenNoNew(IconKind kind)
{
    switch (kind)
    {
    case IconKind::BellEmpty:
    case IconKind::BellEmptyDnd:
        return true;
    default:
        // A full bell, or a glyph that is not a bell at all: shown.
        return false;
    }
}

void ApplyBellIcon(FrameworkElement const& iconView);

// When the clock is hidden the bell's content is rebuilt every time the bell changes, so right after a change
// there may be nothing under the element yet. A few low-priority retries on the element's own thread cover it.
void ApplyBellIconWithRetry(FrameworkElement const& iconView, int attempt)
{
    if (g_unloading.load(std::memory_order_relaxed) || attempt >= 10)
    {
        return;
    }

    if (ChildByName(iconView, L"ContainerGrid"))
    {
        ApplyBellIcon(iconView);
        return;
    }

    g_pendingDispatches.fetch_add(1, std::memory_order_relaxed);
    try
    {
        iconView.Dispatcher().TryRunAsync(CoreDispatcherPriority::Low, [iconView, attempt]() {
            try
            {
                ApplyBellIconWithRetry(iconView, attempt + 1);
            }
            catch (...)
            {
            }
            g_pendingDispatches.fetch_sub(1, std::memory_order_relaxed);
        });
    }
    catch (...)
    {
        g_pendingDispatches.fetch_sub(1, std::memory_order_relaxed);
    }
}

// NotificationCenterButton: the bell. Its content is collapsed; when the clock is hidden the bell sits in a
// presenter of its own whose width is zeroed as well, or the empty slot would still take room.
void ApplyBellIcon(FrameworkElement const& iconView)
{
    FrameworkElement containerGrid = ChildByName(iconView, L"ContainerGrid");
    if (!containerGrid)
    {
        return;
    }

    // Clock hidden:  ContainerGrid > ContentPresenter > ContentGrid > SystemTray.TextIconContent
    // Clock shown:   ContainerGrid > ContentGrid > SystemTray.TextIconContent
    FrameworkElement presenterForHiddenClock = ChildByName(containerGrid, L"ContentPresenter");
    FrameworkElement element = presenterForHiddenClock ? presenterForHiddenClock : containerGrid;
    element = ChildByName(element, L"ContentGrid");
    FrameworkElement content = ChildByClass(element, L"SystemTray.TextIconContent");
    if (!content)
    {
        return;
    }

    const int mode = g_unloading.load(std::memory_order_relaxed) ? (int)kBellNever
                                                                 : g_settings.bellMode.load(std::memory_order_relaxed);
    bool hide = false;
    if (mode == kBellAlways)
    {
        hide = true;
    }
    else if (mode == kBellWhenNoNew)
    {
        TextBlock text = InnerTextBlockOf(content);
        if (!text)
        {
            SP_LogDebug(L"The bell has no text block");
            return;
        }
        const auto glyph = text.Text();
        const IconKind kind = IdentifyIcon(glyph);
        hide = BellShouldHideWhenNoNew(kind);
        SP_LogDebug(L"Bell U+%04X kind %d: hide=%d", FirstChar(glyph), (int)kind, hide ? 1 : 0);

        // The bell's accessible name changes with its state ("No new notifications" / "N new notifications"),
        // and unlike the glyph it changes even when the content is rebuilt, so that is what is watched.
        const DependencyProperty property = Automation::AutomationProperties::NameProperty();
        if (!IsWatched(iconView, property))
        {
            const int64_t token = iconView.RegisterPropertyChangedCallback(
                property, [](DependencyObject const& sender, DependencyProperty const&) {
                    try
                    {
                        if (g_unloading.load(std::memory_order_relaxed))
                        {
                            return;
                        }
                        if (auto bell = sender.try_as<FrameworkElement>())
                        {
                            ApplyBellIconWithRetry(bell, 0);
                        }
                    }
                    catch (...)
                    {
                    }
                });
            RememberWatcher(iconView, property, token);
        }
    }
    else
    {
        SP_LogDebug(L"Bell: hide=0");
    }

    content.Visibility(hide ? Visibility::Collapsed : Visibility::Visible);

    if (FrameworkElement outerPresenter = ParentOf(iconView))
    {
        if (hide && presenterForHiddenClock)
        {
            outerPresenter.MaxWidth(0.0);
        }
        else
        {
            outerPresenter.ClearValue(FrameworkElement::MaxWidthProperty());
        }
    }
}

// ShowDesktopStack: the "Show desktop" strip. 0 hides the strip; with no width chosen (and on unload) the shell's
// own values are put back.
//
// Pinning MinWidth/MaxWidth on the icon and its stack is what the original does, and on build 26200 it is not
// enough: the strip keeps its 12 px because something between the stack and SystemTrayFrameGrid sizes it too (a
// fixed Width on an element of the chain, or a fixed-width grid column). So every element from the icon up to the
// frame grid gets the width, fixed-width columns on the way are widened, and whatever the shell had set locally
// is remembered and restored exactly, instead of being cleared.

// A local value as it was before the mod touched it; `value` is null when nothing was set locally.
struct SavedLocalValue
{
    winrt::weak_ref<FrameworkElement> element;
    DependencyProperty                property{ nullptr };
    IInspectable                      value{ nullptr };
};

struct SavedColumnWidth
{
    winrt::weak_ref<Controls::ColumnDefinition> column;
    GridLength                                  width{};
};

std::mutex                    g_showDesktopLock;
std::vector<SavedLocalValue>  g_savedValues;
std::vector<SavedColumnWidth> g_savedColumns;

// UI thread. Remembers the element's local value of `property` once, then sets it.
void PinValue(FrameworkElement const& element, DependencyProperty const& property, double value)
{
    {
        std::lock_guard<std::mutex> lock(g_showDesktopLock);
        bool saved = false;
        for (auto const& entry : g_savedValues)
        {
            if (entry.property == property && entry.element.get() == element)
            {
                saved = true;
                break;
            }
        }
        if (!saved)
        {
            SavedLocalValue entry;
            entry.element = winrt::make_weak(element);
            entry.property = property;
            IInspectable local = element.ReadLocalValue(property);
            entry.value = (local == DependencyProperty::UnsetValue()) ? nullptr : local;
            g_savedValues.push_back(std::move(entry));
        }
    }
    element.SetValue(property, winrt::box_value(value));
}

// UI thread. Remembers a fixed column width once, then widens it.
void PinColumn(Controls::ColumnDefinition const& column, double width)
{
    {
        std::lock_guard<std::mutex> lock(g_showDesktopLock);
        bool saved = false;
        for (auto const& entry : g_savedColumns)
        {
            if (entry.column.get() == column)
            {
                saved = true;
                break;
            }
        }
        if (!saved)
        {
            g_savedColumns.push_back({ winrt::make_weak(column), column.Width() });
        }
    }
    column.Width(GridLength{ width, GridUnitType::Pixel });
}

// UI thread. Puts back every value this thread's elements had before; entries of other threads' taskbars, and
// of elements that are gone, are handled by their own pass or dropped.
void RestoreShowDesktopValues(CoreDispatcher const& dispatcher)
{
    std::vector<SavedLocalValue>  values;
    std::vector<SavedColumnWidth> columns;
    {
        std::lock_guard<std::mutex> lock(g_showDesktopLock);
        for (auto it = g_savedValues.begin(); it != g_savedValues.end();)
        {
            auto element = it->element.get();
            if (!element)
            {
                it = g_savedValues.erase(it);
            }
            else if (element.Dispatcher() == dispatcher)
            {
                values.push_back(std::move(*it));
                it = g_savedValues.erase(it);
            }
            else
            {
                ++it;
            }
        }
        for (auto it = g_savedColumns.begin(); it != g_savedColumns.end();)
        {
            auto column = it->column.get();
            if (!column)
            {
                it = g_savedColumns.erase(it);
            }
            else if (column.Dispatcher() == dispatcher)
            {
                columns.push_back(std::move(*it));
                it = g_savedColumns.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    for (auto const& entry : values)
    {
        try
        {
            if (auto element = entry.element.get())
            {
                if (entry.value)
                {
                    element.SetValue(entry.property, entry.value);
                }
                else
                {
                    element.ClearValue(entry.property);
                }
            }
        }
        catch (...)
        {
            // A value that cannot be put back (a binding expression, say) must not stop the others.
        }
    }
    for (auto const& entry : columns)
    {
        if (auto column = entry.column.get())
        {
            column.Width(entry.width);
        }
    }
}

void ApplyShowDesktop(FrameworkElement const& iconView)
{
    FrameworkElement stack = AncestorByName(iconView, L"ShowDesktopStack");
    if (!stack)
    {
        SP_LogDebug(L"The show desktop icon is not under ShowDesktopStack");
        return;
    }

    int width = -1;
    if (!g_unloading.load(std::memory_order_relaxed))
    {
        if (g_settings.hideShowDesktop.load(std::memory_order_relaxed))
        {
            width = 0;
        }
        else
        {
            const int chosen = g_settings.showDesktopWidth.load(std::memory_order_relaxed);
            if (chosen > 0)
            {
                width = chosen;
            }
        }
    }

    // Always from the shell's own values, so a smaller width after a larger one is not held up by the old pin.
    RestoreShowDesktopValues(iconView.Dispatcher());

    if (width < 0)
    {
        SP_LogDebug(L"Show desktop button: default width");
        return;
    }

    const double w = (double)width;

    // The icon's own content (the ShowDesktopButton template root) may carry a fixed width as well.
    if (FrameworkElement inner = FirstChild(iconView, [](FrameworkElement const&) { return true; }))
    {
        PinValue(inner, FrameworkElement::WidthProperty(), w);
    }

    // The icon, the stack, and everything between them are pinned. Above the stack, up to the frame grid, only a
    // wrapper (an element whose one child is the path taken) has a fixed Width overridden, so the rest of the tray
    // keeps its layout; a fixed-width grid column holding the path is widened.
    bool pastStack = false;
    FrameworkElement child = nullptr;
    for (FrameworkElement element = iconView; element; child = element, element = ParentOf(element))
    {
        if (auto grid = element.try_as<Controls::Grid>(); grid && child)
        {
            const int32_t index = Controls::Grid::GetColumn(child);
            auto columns = grid.ColumnDefinitions();
            if (index >= 0 && (uint32_t)index < columns.Size() && Controls::Grid::GetColumnSpan(child) == 1)
            {
                auto column = columns.GetAt(index);
                if (column.Width().GridUnitType == GridUnitType::Pixel)
                {
                    PinColumn(column, w);
                }
            }
        }

        if (element.Name() == L"SystemTrayFrameGrid")
        {
            break;
        }

        if (!pastStack)
        {
            PinValue(element, FrameworkElement::MinWidthProperty(), w);
            PinValue(element, FrameworkElement::MaxWidthProperty(), w);
            PinValue(element, FrameworkElement::WidthProperty(), w);
            pastStack = (element == stack);
        }
        else if (!std::isnan(element.Width()) && VisualTreeHelper::GetChildrenCount(element) == 1)
        {
            PinValue(element, FrameworkElement::WidthProperty(), w);
        }
    }

    SP_LogDebug(L"Show desktop button: width %d (icon %.1f, stack %.1f before layout)", width, iconView.ActualWidth(),
                stack.ActualWidth());
}

// ---------------------------------------------------------------------------------------------------------------
// Walking a whole tray
// ---------------------------------------------------------------------------------------------------------------

// The StackPanel of a MainStack / NonActivatableStack / ShowDesktopStack container.
FrameworkElement StackPanelOfContainer(FrameworkElement const& container)
{
    FrameworkElement element = ChildByName(container, L"Content");
    element = ChildByName(element, L"IconStack");
    element = ChildByClass(element, kItemsPresenterClass);
    element = ChildByClass(element, kStackPanelClass);
    return element;
}

// The StackPanel of the ControlCenterButton / NotificationCenterButton.
FrameworkElement StackPanelOfButton(FrameworkElement const& button)
{
    FrameworkElement element = ChildByClass(button, kGridClass);
    element = ChildByName(element, L"ContentPresenter");
    element = ChildByClass(element, kItemsPresenterClass);
    element = ChildByClass(element, kStackPanelClass);
    return element;
}

// Applies `rule` to each SystemTrayIcon under a StackPanel. Returns false when the panel is not there.
bool ForEachIconView(FrameworkElement const& stackPanel, void (*rule)(FrameworkElement const&))
{
    if (!stackPanel)
    {
        return false;
    }

    FirstChild(stackPanel, [rule](FrameworkElement const& child) {
        if (!IsClass(child, kContentPresenterClass))
        {
            return false;
        }
        if (FrameworkElement icon = ChildByName(child, L"SystemTrayIcon"))
        {
            try
            {
                rule(icon);
            }
            catch (winrt::hresult_error const& e)
            {
                SP_LogDebug(L"A tray icon rule failed: 0x%08X", (unsigned)e.code());
            }
            catch (...)
            {
                SP_LogDebug(L"A tray icon rule failed");
            }
        }
        return false;   // keep going
    });
    return true;
}

// UI thread. Applies every rule to the tray under a taskbar's XAML root. Returns false when no tray was found.
bool ApplyStyleFromRoot(XamlRoot const& root)
{
    if (!root)
    {
        return false;
    }

    auto content = root.Content();
    FrameworkElement grid = content ? content.try_as<FrameworkElement>() : nullptr;
    grid = ChildByClass(grid, L"SystemTray.SystemTrayFrame");
    grid = ChildByName(grid, L"SystemTrayFrameGrid");
    if (!grid)
    {
        return false;
    }

    const bool results[] = {
        ForEachIconView(StackPanelOfContainer(ChildByName(grid, L"MainStack")), ApplyMainStackIcon),
        ForEachIconView(StackPanelOfContainer(ChildByName(grid, L"NonActivatableStack")), ApplyLanguageBarIcon),
        ForEachIconView(StackPanelOfButton(ChildByName(grid, L"ControlCenterButton")), ApplyControlCenterIcon),
        ForEachIconView(StackPanelOfButton(ChildByName(grid, L"NotificationCenterButton")), ApplyBellIcon),
        ForEachIconView(StackPanelOfContainer(ChildByName(grid, L"ShowDesktopStack")), ApplyShowDesktop),
    };
    for (bool result : results)
    {
        if (result)
        {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------
// Ways back into a taskbar's XAML from the engine thread
//
// First choice: the tray window keeps its task band (CTaskBand, in taskbar.dll) in the window bytes of the
// TaskbandHWND child; the band's ITaskListWndSite interface hands out a shared_ptr<TaskbarHost>, and the host
// holds the XAML frame element a few bytes in. That offset is read from TaskbarHost::FrameHeight, which starts
// by adding it to `this`. Every symbol is resolved only, none is hooked.
//
// Fallback: one element per taskbar thread remembered from the constructor hook; its XamlRoot is the same one.
// ---------------------------------------------------------------------------------------------------------------

using GetTaskbarHost_t = void*(WINAPI*)(void* pThis, void** result);
using RefCountDecref_t = void(WINAPI*)(void* pThis);

void*            g_CTaskBand_ITaskListWndSite_vftable = nullptr;
void*            g_CSecondaryTaskBand_ITaskListWndSite_vftable = nullptr;
GetTaskbarHost_t g_CTaskBand_GetTaskbarHost = nullptr;
GetTaskbarHost_t g_CSecondaryTaskBand_GetTaskbarHost = nullptr;
void*            g_TaskbarHost_FrameHeight = nullptr;
RefCountDecref_t g_RefCountBase_Decref = nullptr;

BOOL ResolveTaskbarDllSymbols()
{
    HMODULE module = LoadLibraryExW(L"taskbar.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module)
    {
        SP_LogError(L"taskbar.dll could not be loaded");
        return FALSE;
    }

    static const wchar_t* const kTaskBandVftable[] = {
        LR"(const CTaskBand::`vftable'{for `ITaskListWndSite'})",
    };
    static const wchar_t* const kSecondaryTaskBandVftable[] = {
        LR"(const CSecondaryTaskBand::`vftable'{for `ITaskListWndSite'})",
    };
    static const wchar_t* const kTaskBandGetTaskbarHost[] = {
        LR"(public: virtual class std::shared_ptr<class TaskbarHost> __cdecl CTaskBand::GetTaskbarHost(void)const )",
    };
    static const wchar_t* const kTaskbarHostFrameHeight[] = {
        LR"(public: int __cdecl TaskbarHost::FrameHeight(void)const )",
    };
    static const wchar_t* const kSecondaryTaskBandGetTaskbarHost[] = {
        LR"(public: virtual class std::shared_ptr<class TaskbarHost> __cdecl CSecondaryTaskBand::GetTaskbarHost(void)const )",
    };
    static const wchar_t* const kRefCountBaseDecref[] = {
        LR"(public: void __cdecl std::_Ref_count_base::_Decref(void))",
    };

    SP_SymbolHook hooks[6] = {};
    hooks[0].symbols = kTaskBandVftable;
    hooks[0].symbolCount = ARRAYSIZE(kTaskBandVftable);
    hooks[0].pOriginal = &g_CTaskBand_ITaskListWndSite_vftable;
    hooks[1].symbols = kSecondaryTaskBandVftable;
    hooks[1].symbolCount = ARRAYSIZE(kSecondaryTaskBandVftable);
    hooks[1].pOriginal = &g_CSecondaryTaskBand_ITaskListWndSite_vftable;
    hooks[2].symbols = kTaskBandGetTaskbarHost;
    hooks[2].symbolCount = ARRAYSIZE(kTaskBandGetTaskbarHost);
    hooks[2].pOriginal = (void**)&g_CTaskBand_GetTaskbarHost;
    hooks[3].symbols = kTaskbarHostFrameHeight;
    hooks[3].symbolCount = ARRAYSIZE(kTaskbarHostFrameHeight);
    hooks[3].pOriginal = &g_TaskbarHost_FrameHeight;
    hooks[4].symbols = kSecondaryTaskBandGetTaskbarHost;
    hooks[4].symbolCount = ARRAYSIZE(kSecondaryTaskBandGetTaskbarHost);
    hooks[4].pOriginal = (void**)&g_CSecondaryTaskBand_GetTaskbarHost;
    hooks[5].symbols = kRefCountBaseDecref;
    hooks[5].symbolCount = ARRAYSIZE(kRefCountBaseDecref);
    hooks[5].pOriginal = (void**)&g_RefCountBase_Decref;
    for (auto& hook : hooks)
    {
        hook.hookFunction = nullptr;    // resolve only
        hook.optional = FALSE;
    }

    if (!SP_ResolveSymbols(module, hooks, ARRAYSIZE(hooks)))
    {
        SP_LogError(L"The taskbar.dll symbols were not all found; settings apply through the icons met so far");
        return FALSE;
    }
    return TRUE;
}

// The frame element held by a shared_ptr<TaskbarHost> {object, control block}. The control block's reference
// is released here, which is what the shared_ptr's destructor would have done.
FrameworkElement TaskbarFrameFromHost(void* hostSharedPtr[2])
{
    if (!hostSharedPtr[0] && !hostSharedPtr[1])
    {
        return nullptr;
    }

    size_t frameOffset = 0x48;
#if defined(_M_X64)
    {
        // 48:83EC 28 | sub rsp,28
        // 48:83C1 48 | add rcx,48
        const BYTE* b = (const BYTE*)g_TaskbarHost_FrameHeight;
        if (b[0] == 0x48 && b[1] == 0x83 && b[2] == 0xEC && b[4] == 0x48 && b[5] == 0x83 && b[6] == 0xC1 &&
            b[7] <= 0x7F)
        {
            frameOffset = b[7];
        }
        else
        {
            SP_LogDebug(L"TaskbarHost::FrameHeight has an unfamiliar prologue; using offset 0x48");
        }
    }
#else
#error "Only x64 is supported"
#endif

    FrameworkElement frame{ nullptr };
    if (hostSharedPtr[0])
    {
        auto* unknown = *(::IUnknown**)((BYTE*)hostSharedPtr[0] + frameOffset);
        if (unknown)
        {
            unknown->QueryInterface(winrt::guid_of<FrameworkElement>(), winrt::put_abi(frame));
        }
    }

    if (hostSharedPtr[1])
    {
        g_RefCountBase_Decref(hostSharedPtr[1]);
    }

    return frame;
}

// The sub-object of the band that implements ITaskListWndSite: the first pointer slot holding that vftable.
void* FindTaskListWndSite(void* taskBand, void* vftable)
{
    if (!taskBand || !vftable)
    {
        return nullptr;
    }
    void** slot = (void**)taskBand;
    for (int i = 0; i < 20; ++i, ++slot)
    {
        if (*slot == vftable)
        {
            return slot;
        }
    }
    return nullptr;
}

FrameworkElement TaskbarFrameOfPrimary(HWND hTaskbarWnd)
{
    HWND hTaskSwWnd = (HWND)GetPropW(hTaskbarWnd, L"TaskbandHWND");
    if (!hTaskSwWnd)
    {
        return nullptr;
    }

    void* taskBand = (void*)GetWindowLongPtrW(hTaskSwWnd, 0);
    void* site = FindTaskListWndSite(taskBand, g_CTaskBand_ITaskListWndSite_vftable);
    if (!site)
    {
        return nullptr;
    }

    void* host[2] = {};
    g_CTaskBand_GetTaskbarHost(site, host);
    return TaskbarFrameFromHost(host);
}

FrameworkElement TaskbarFrameOfSecondary(HWND hSecondaryTaskbarWnd)
{
    HWND hTaskSwWnd = FindWindowExW(hSecondaryTaskbarWnd, nullptr, L"WorkerW", nullptr);
    if (!hTaskSwWnd)
    {
        return nullptr;
    }

    void* taskBand = (void*)GetWindowLongPtrW(hTaskSwWnd, 0);
    void* site = FindTaskListWndSite(taskBand, g_CSecondaryTaskBand_ITaskListWndSite_vftable);
    if (!site)
    {
        return nullptr;
    }

    void* host[2] = {};
    g_CSecondaryTaskBand_GetTaskbarHost(site, host);
    return TaskbarFrameFromHost(host);
}

// One taskbar to style: its dispatcher, and either the frame element (taskbar.dll route) or a remembered icon.
struct ApplyTarget
{
    CoreDispatcher                    dispatcher{ nullptr };
    FrameworkElement                  frame{ nullptr };
    winrt::weak_ref<FrameworkElement> element;
};

void CollectTargetsFromTaskbarWindows(std::vector<ApplyTarget>& targets)
{
    EnumWindows(
        [](HWND hWnd, LPARAM lParam) -> BOOL {
            auto* out = (std::vector<ApplyTarget>*)lParam;

            DWORD processId = 0;
            if (!GetWindowThreadProcessId(hWnd, &processId) || processId != GetCurrentProcessId())
            {
                return TRUE;
            }

            wchar_t className[64];
            if (!GetClassNameW(hWnd, className, ARRAYSIZE(className)))
            {
                return TRUE;
            }

            try
            {
                FrameworkElement frame{ nullptr };
                if (_wcsicmp(className, L"Shell_TrayWnd") == 0)
                {
                    frame = TaskbarFrameOfPrimary(hWnd);
                }
                else if (_wcsicmp(className, L"Shell_SecondaryTrayWnd") == 0)
                {
                    frame = TaskbarFrameOfSecondary(hWnd);
                }
                else
                {
                    return TRUE;
                }

                if (frame)
                {
                    ApplyTarget target;
                    target.dispatcher = frame.Dispatcher();
                    target.frame = frame;
                    out->push_back(target);
                }
                else
                {
                    SP_LogDebug(L"No XAML frame behind taskbar window %p", hWnd);
                }
            }
            catch (...)
            {
                SP_LogDebug(L"Reading the XAML frame of taskbar window %p failed", hWnd);
            }
            return TRUE;
        },
        (LPARAM)&targets);
}

// The fallback: one remembered icon per taskbar thread, taken from the constructor hook.
struct KnownRoot
{
    winrt::weak_ref<FrameworkElement> element;
    CoreDispatcher                    dispatcher{ nullptr };
};

std::mutex             g_rootsLock;
std::vector<KnownRoot> g_roots;

// UI thread.
void RememberRoot(FrameworkElement const& element)
{
    CoreDispatcher dispatcher = element.Dispatcher();

    std::lock_guard<std::mutex> lock(g_rootsLock);
    for (auto& known : g_roots)
    {
        if (known.dispatcher == dispatcher)
        {
            if (!known.element.get())
            {
                known.element = winrt::make_weak(element);
            }
            return;
        }
    }
    if (g_roots.size() < 8)
    {
        KnownRoot known;
        known.element = winrt::make_weak(element);
        known.dispatcher = dispatcher;
        g_roots.push_back(std::move(known));
    }
}

// ---------------------------------------------------------------------------------------------------------------
// The constructor hook
// ---------------------------------------------------------------------------------------------------------------

struct PendingLoaded
{
    winrt::weak_ref<FrameworkElement> element;
    winrt::event_token                token;
};

std::mutex                 g_pendingLock;
std::vector<PendingLoaded> g_pending;

void ForgetPending(int64_t tokenValue)
{
    std::lock_guard<std::mutex> lock(g_pendingLock);
    for (size_t i = 0; i < g_pending.size(); ++i)
    {
        if (g_pending[i].token.value == tokenValue)
        {
            g_pending.erase(g_pending.begin() + (ptrdiff_t)i);
            return;
        }
    }
}

// UI thread, on unload: takes the Loaded handlers off the icons of this thread that never got loaded, so no
// handler of ours is left behind on a live element.
void RevokePendingOnThisThread()
{
    std::vector<PendingLoaded> mine;
    {
        std::lock_guard<std::mutex> lock(g_pendingLock);
        for (size_t i = 0; i < g_pending.size();)
        {
            bool drop = true;
            try
            {
                if (auto element = g_pending[i].element.get())
                {
                    if (element.Dispatcher().HasThreadAccess())
                    {
                        mine.push_back(g_pending[i]);
                    }
                    else
                    {
                        drop = false;
                    }
                }
            }
            catch (...)
            {
            }

            if (drop)
            {
                g_pending.erase(g_pending.begin() + (ptrdiff_t)i);
            }
            else
            {
                ++i;
            }
        }
    }

    for (auto const& pending : mine)
    {
        try
        {
            if (auto element = pending.element.get())
            {
                element.Loaded(pending.token);
            }
        }
        catch (...)
        {
        }
    }
}

enum class Container
{
    None,
    MainStack,
    NonActivatableStack,
    ControlCenterButton,
    NotificationCenterButton,
    ShowDesktopStack,
};

// The nearest named container above an icon, in one walk.
Container ContainerOf(FrameworkElement const& element)
{
    FrameworkElement current = ParentOf(element);
    for (int depth = 0; current && depth < 64; ++depth)
    {
        const auto name = current.Name();
        if (!name.empty())
        {
            if (name == L"MainStack")                return Container::MainStack;
            if (name == L"NonActivatableStack")      return Container::NonActivatableStack;
            if (name == L"ControlCenterButton")      return Container::ControlCenterButton;
            if (name == L"NotificationCenterButton") return Container::NotificationCenterButton;
            if (name == L"ShowDesktopStack")         return Container::ShowDesktopStack;
        }
        current = ParentOf(current);
    }
    return Container::None;
}

// UI thread, once the icon is in the tree. `view` is the event's sender, which is the IconView itself.
void OnIconViewLoaded(FrameworkElement const& view)
{
    if (g_unloading.load(std::memory_order_relaxed))
    {
        return;
    }

    if (winrt::get_class_name(view) != L"SystemTray.IconView" || view.Name() != L"SystemTrayIcon")
    {
        return;
    }

    RememberRoot(view);

    switch (ContainerOf(view))
    {
    case Container::MainStack:
        ApplyMainStackIcon(view);
        break;
    case Container::NonActivatableStack:
        ApplyLanguageBarIcon(view);
        break;
    case Container::ControlCenterButton:
        ApplyControlCenterIcon(view);
        break;
    case Container::NotificationCenterButton:
        ApplyBellIcon(view);
        break;
    case Container::ShowDesktopStack:
        ApplyShowDesktop(view);
        break;
    default:
        SP_LogDebug(L"A tray icon loaded outside the known containers");
        break;
    }
}

void WatchLoaded(FrameworkElement const& view)
{
    // The handler removes itself with its own token, which it only knows once the registration returns.
    auto tokenBox = std::make_shared<winrt::event_token>();

    const winrt::event_token token = view.Loaded([tokenBox](IInspectable const& sender, RoutedEventArgs const&) {
        try
        {
            ForgetPending(tokenBox->value);
            auto loaded = sender.try_as<FrameworkElement>();
            if (!loaded)
            {
                return;
            }
            loaded.Loaded(*tokenBox);
            OnIconViewLoaded(loaded);
        }
        catch (winrt::hresult_error const& e)
        {
            SP_LogError(L"Styling a tray icon failed: 0x%08X", (unsigned)e.code());
        }
        catch (...)
        {
            SP_LogError(L"Styling a tray icon failed");
        }
    });
    *tokenBox = token;

    std::lock_guard<std::mutex> lock(g_pendingLock);
    PendingLoaded pending;
    pending.element = winrt::make_weak(view);
    pending.token = token;
    g_pending.push_back(std::move(pending));
}

bool PointsIntoTrayModule(const void* p)
{
    const BYTE* base = g_trayModuleBase.load(std::memory_order_relaxed);
    const SIZE_T size = g_trayModuleSize.load(std::memory_order_relaxed);
    return base && p >= base && p < base + size;
}

using IconViewCtor_t = void*(WINAPI*)(void* pThis);
IconViewCtor_t g_origIconViewCtor = nullptr;

// The implementation object starts with the vtable of its one projected interface, and right after it comes
// the inner XAML object it composes (the C++/WinRT m_inner). That is where the element is taken from. A pointer
// into the module's own image would be a vtable instead, meaning a layout this code does not know: then the
// icon is simply left alone.
void* WINAPI IconViewCtor_Hook(void* pThis)
{
    void* ret = g_origIconViewCtor(pThis);

    if (!pThis || g_unloading.load(std::memory_order_relaxed))
    {
        return ret;
    }

    try
    {
        ::IUnknown* inner = ((::IUnknown**)pThis)[1];
        if (!inner || PointsIntoTrayModule(inner))
        {
            return ret;
        }

        FrameworkElement view{ nullptr };
        if (FAILED(inner->QueryInterface(winrt::guid_of<FrameworkElement>(), winrt::put_abi(view))) || !view)
        {
            return ret;
        }

        WatchLoaded(view);
    }
    catch (...)
    {
        SP_LogDebug(L"A new tray icon could not be watched");
    }

    return ret;
}

// ---------------------------------------------------------------------------------------------------------------
// Applying from the engine thread
// ---------------------------------------------------------------------------------------------------------------

// An event that outlives whichever side finishes last, so a callback that runs after the wait timed out can
// still signal safely.
struct Waiter
{
    HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~Waiter()
    {
        if (hEvent)
        {
            CloseHandle(hEvent);
        }
    }
};

// UI thread. One pass over one taskbar.
void ApplyOnUiThread(ApplyTarget const& target)
{
    if (g_unloading.load(std::memory_order_relaxed))
    {
        RevokePendingOnThisThread();
    }
    // The rules below register the watchers they still need.
    RemoveWatchersOnThisThread();

    XamlRoot root{ nullptr };
    if (target.frame)
    {
        root = target.frame.XamlRoot();
    }
    else if (auto element = target.element.get())
    {
        root = element.XamlRoot();
    }

    if (!ApplyStyleFromRoot(root))
    {
        SP_LogDebug(L"No tray under this taskbar's XAML root");
    }
}

// Runs the pass for one taskbar on its own thread and waits for it, briefly: BeforeUninit needs the restore done
// before the hooks go, and a UI thread that does not answer must not hold the engine. False when the dispatcher
// is gone (its taskbar was destroyed, as happens on a display change).
bool ApplyOnTarget(ApplyTarget const& target)
{
    auto waiter = std::make_shared<Waiter>();
    if (!waiter->hEvent)
    {
        return true;
    }

    g_pendingDispatches.fetch_add(1, std::memory_order_relaxed);
    try
    {
        target.dispatcher.TryRunAsync(CoreDispatcherPriority::High, [target, waiter]() {
            try
            {
                ApplyOnUiThread(target);
            }
            catch (winrt::hresult_error const& e)
            {
                SP_LogError(L"Applying the tray icon settings failed: 0x%08X", (unsigned)e.code());
            }
            catch (...)
            {
                SP_LogError(L"Applying the tray icon settings failed");
            }
            g_pendingDispatches.fetch_sub(1, std::memory_order_relaxed);
            SetEvent(waiter->hEvent);
        });
    }
    catch (...)
    {
        g_pendingDispatches.fetch_sub(1, std::memory_order_relaxed);
        SP_LogDebug(L"A taskbar's dispatcher refused the callback; that taskbar is gone");
        return false;
    }

    WaitForSingleObject(waiter->hEvent, 3000);
    return true;
}

// Engine thread (or the module-wait thread). Styles every taskbar that can be reached right now.
// Defined with the tray hooks below.
using Controller_UpdateFrameSize_t = void(WINAPI*)(void* pThis);
using Controller_View_t = void*(WINAPI*)(void* pThis, void** result);
extern Controller_UpdateFrameSize_t g_origControllerUpdateFrameSize;
extern Controller_View_t            g_pfnControllerView;
void NudgeTrayFrame();

void ApplySettings()
{
    std::vector<ApplyTarget> targets;

    if (g_taskbarDllReady.load(std::memory_order_relaxed))
    {
        CollectTargetsFromTaskbarWindows(targets);
    }

    auto collectRoots = [&targets]() {
        std::lock_guard<std::mutex> lock(g_rootsLock);
        for (auto const& known : g_roots)
        {
            ApplyTarget target;
            target.dispatcher = known.dispatcher;
            target.element = known.element;
            targets.push_back(target);
        }
    };
    if (targets.empty())
    {
        collectRoots();
    }

    if (targets.empty() && g_origControllerUpdateFrameSize && g_pfnControllerView && !g_unloading.load(std::memory_order_relaxed))
    {
        // Build 26200: ask the tray's controller to run UpdateFrameSize, which remembers the tray frame as a root
        // a moment after the message returns.
        NudgeTrayFrame();
        for (int i = 0; i < 40 && targets.empty(); ++i)
        {
            Sleep(50);
            collectRoots();
        }
    }

    if (targets.empty())
    {
        SP_LogDebug(L"No taskbar to style yet; the icons are styled as they load");
        return;
    }

    for (auto const& target : targets)
    {
        if (!ApplyOnTarget(target) && !target.frame)
        {
            // A remembered icon of a destroyed taskbar: forget it, so its replacement is remembered instead.
            std::lock_guard<std::mutex> lock(g_rootsLock);
            for (auto it = g_roots.begin(); it != g_roots.end(); ++it)
            {
                if (it->dispatcher == target.dispatcher)
                {
                    g_roots.erase(it);
                    break;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Finding the tray module
// ---------------------------------------------------------------------------------------------------------------

// The major file version of a loaded module, read from its version resource without the version API: the
// VS_FIXEDFILEINFO block is found by its signature. 0 when unknown.
WORD ModuleMajorVersion(HMODULE hModule)
{
    HRSRC hRes = FindResourceW(hModule, MAKEINTRESOURCEW(VS_VERSION_INFO), RT_VERSION);
    if (!hRes)
    {
        return 0;
    }
    HGLOBAL hGlobal = LoadResource(hModule, hRes);
    if (!hGlobal)
    {
        return 0;
    }
    const BYTE* data = (const BYTE*)LockResource(hGlobal);
    DWORD size = SizeofResource(hModule, hRes);
    if (!data || size < sizeof(VS_FIXEDFILEINFO))
    {
        return 0;
    }

    for (DWORD at = 0; at + sizeof(VS_FIXEDFILEINFO) <= size; at += sizeof(DWORD))
    {
        const VS_FIXEDFILEINFO* info = (const VS_FIXEDFILEINFO*)(data + at);
        if (info->dwSignature == 0xFEEF04BD)
        {
            return HIWORD(info->dwFileVersionMS);
        }
    }
    return 0;
}

SIZE_T ModuleImageSize(HMODULE hModule)
{
    __try
    {
        const BYTE* base = (const BYTE*)hModule;
        auto dos = (const IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        {
            return 0;
        }
        auto nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
        {
            return 0;
        }
        return nt->OptionalHeader.SizeOfImage;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

// Build 26200 has none of the taskbar.dll symbols that lead from the tray window to its XAML, and at a runtime
// enable no icon constructor runs, so nothing could be reached. SystemTrayController::UpdateFrameSize runs on
// the tray's thread whenever the taskbar is told its logical DPI override changed (a WM_SETTINGCHANGE the mod
// can send), and the controller's View() hands out the SystemTrayFrame: that element is remembered as a root.
using Controller_UpdateFrameSize_t = void(WINAPI*)(void* pThis);
using Controller_View_t = void*(WINAPI*)(void* pThis, void** result);
Controller_UpdateFrameSize_t g_origControllerUpdateFrameSize = nullptr;
Controller_View_t            g_pfnControllerView = nullptr;

void WINAPI SystemTrayController_UpdateFrameSize_Hook(void* pThis)
{
    g_origControllerUpdateFrameSize(pThis);

    if (!g_pfnControllerView || g_unloading.load(std::memory_order_relaxed))
    {
        return;
    }
    try
    {
        void* abi = nullptr;
        g_pfnControllerView(pThis, &abi);
        FrameworkElement frame{ nullptr };
        winrt::attach_abi(frame, abi);
        if (frame)
        {
            RememberRoot(frame);
        }
    }
    catch (...)
    {
    }
}

void NudgeTrayFrame()
{
    EnumWindows(
        [](HWND hWnd, LPARAM) -> BOOL {
            DWORD processId = 0;
            wchar_t className[32];
            if (GetWindowThreadProcessId(hWnd, &processId) && processId == GetCurrentProcessId() &&
                GetClassNameW(hWnd, className, ARRAYSIZE(className)) && _wcsicmp(className, L"Shell_TrayWnd") == 0)
            {
                SendMessageTimeoutW(hWnd, WM_SETTINGCHANGE, SPI_SETLOGICALDPIOVERRIDE, 0,
                                    SMTO_NORMAL | SMTO_ABORTIFHUNG, 3000, nullptr);
            }
            return TRUE;
        },
        0);
}

BOOL InstallTrayHooks(HMODULE hModule, const wchar_t* moduleName)
{
    if (g_trayHooked.exchange(true))
    {
        return TRUE;
    }

    static const wchar_t* const kIconViewCtor[] = {
        LR"(public: __cdecl winrt::SystemTray::implementation::IconView::IconView(void))",
    };
    static const wchar_t* const kControllerUpdateFrameSize[] = {
        LR"(private: void __cdecl winrt::SystemTray::implementation::SystemTrayController::UpdateFrameSize(void))",
    };
    static const wchar_t* const kControllerView[] = {
        LR"(public: struct winrt::Windows::UI::Xaml::FrameworkElement __cdecl winrt::SystemTray::implementation::SystemTrayController::View(void))",
    };

    SP_SymbolHook hooks[3] = {};

    hooks[1].symbols = kControllerUpdateFrameSize;
    hooks[1].symbolCount = ARRAYSIZE(kControllerUpdateFrameSize);
    hooks[1].pOriginal = (void**)&g_origControllerUpdateFrameSize;
    hooks[1].hookFunction = (void*)SystemTrayController_UpdateFrameSize_Hook;
    hooks[1].optional = TRUE;

    hooks[2].symbols = kControllerView;
    hooks[2].symbolCount = ARRAYSIZE(kControllerView);
    hooks[2].pOriginal = (void**)&g_pfnControllerView;
    hooks[2].optional = TRUE;
    hooks[0].symbols = kIconViewCtor;
    hooks[0].symbolCount = ARRAYSIZE(kIconViewCtor);
    hooks[0].pOriginal = (void**)&g_origIconViewCtor;
    hooks[0].hookFunction = (void*)IconViewCtor_Hook;
    hooks[0].optional = FALSE;

    // The hook may fire the moment it is committed, so its sanity range is set first.
    g_trayModuleBase.store((BYTE*)hModule, std::memory_order_relaxed);
    g_trayModuleSize.store(ModuleImageSize(hModule), std::memory_order_relaxed);

    if (!SP_HookSymbols(hModule, hooks, ARRAYSIZE(hooks)))
    {
        SP_LogError(L"IconView::IconView was not found in %s; no tray icon will be hidden", moduleName);
        g_trayHooked.store(false);
        return FALSE;
    }

    SP_Log(L"Tray icon hook installed in %s (IconView::IconView: yes; taskbar.dll route: %s)",
           moduleName, g_taskbarDllReady.load(std::memory_order_relaxed) ? L"yes" : L"no");

    // The taskbar may already be up (the mod was turned on from the settings window): style it now. On a cold
    // sign-in there is nothing yet, and the icons are styled as their constructors run.
    try
    {
        ApplySettings();
    }
    catch (...)
    {
        SP_LogError(L"The first style pass failed");
    }
    return TRUE;
}

void OnSystemTrayLoaded(HMODULE hModule, void*)
{
    InstallTrayHooks(hModule, L"SystemTray.dll");
}

// Runs on a helper thread once Taskbar.View.dll is in the process. The tray types moved from Taskbar.View.dll to
// SystemTray.dll at version 2604; the version decides which module to look in.
void OnTaskbarViewLoaded(HMODULE hTaskbarView, void*)
{
    if (HMODULE hSystemTray = GetModuleHandleW(L"SystemTray.dll"))
    {
        InstallTrayHooks(hSystemTray, L"SystemTray.dll");
        return;
    }

    const WORD major = ModuleMajorVersion(hTaskbarView);
    if (major && major < 2604)
    {
        InstallTrayHooks(hTaskbarView, L"Taskbar.View.dll");
        return;
    }

    // Not loaded yet: the taskbar brings SystemTray.dll in a moment after its own library.
    SP_Log(L"Taskbar.View.dll %u; waiting for SystemTray.dll", major);
    if (!SP_WaitForModule(L"SystemTray.dll", 60000, OnSystemTrayLoaded, nullptr))
    {
        SP_LogError(L"Could not wait for SystemTray.dll; no tray icon will be hidden");
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------------------------

void ClearLists()
{
    {
        std::lock_guard<std::mutex> lock(g_watchLock);
        g_watchers.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_pendingLock);
        g_pending.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_rootsLock);
        g_roots.clear();
    }
}

BOOL Init()
{
    g_unloading.store(false, std::memory_order_relaxed);
    g_trayHooked.store(false, std::memory_order_relaxed);
    g_origIconViewCtor = nullptr;
    ClearLists();

    LoadSettings();

    // Not fatal: without taskbar.dll a settings change reaches the tray through the icons met by the hook.
    g_taskbarDllReady.store(ResolveTaskbarDllSymbols() ? true : false, std::memory_order_relaxed);

    if (!SP_WaitForModule(L"Taskbar.View.dll", 60000, OnTaskbarViewLoaded, nullptr))
    {
        SP_LogError(L"Could not wait for Taskbar.View.dll");
        return FALSE;
    }

    SP_Log(L"Waiting for the taskbar");
    return TRUE;
}

void SettingsChanged()
{
    LoadSettings();

    if (g_trayHooked.load(std::memory_order_acquire))
    {
        try
        {
            ApplySettings();
        }
        catch (...)
        {
            SP_LogError(L"Re-applying the tray icon settings failed");
        }
    }
}

void BeforeUninit()
{
    // Still hooked. Every rule now restores, the watchers and the pending Loaded handlers come off.
    g_unloading.store(true, std::memory_order_relaxed);

    if (g_trayHooked.load(std::memory_order_acquire))
    {
        try
        {
            ApplySettings();
        }
        catch (...)
        {
            SP_LogError(L"Restoring the tray icons failed");
        }
    }
}

void Uninit()
{
    // Give the callbacks queued on the taskbar's dispatcher a moment to finish; each one checks g_unloading and
    // returns at once, but it still has to run before this code can go.
    for (int i = 0; i < 100 && g_pendingDispatches.load(std::memory_order_relaxed) > 0; ++i)
    {
        Sleep(20);
    }

    ClearLists();
}

}   // namespace

SP_MOD_DEFINE(g_modTaskbarTraySystemIconTweaks) =
{
    /* id             */ SP_MOD_ID,
    /* name           */ L"Hide system icons in the taskbar tray",
    /* basedOn        */ "taskbar-tray-system-icon-tweaks",
    /* originalAuthor */ "m417z",
    /* targets        */ SP_TARGET_EXPLORER,
    /* minOsBuild     */ 22000,
    /* flags          */ 0,
    /* Init           */ Init,
    /* AfterInit      */ nullptr,
    /* SettingsChanged*/ SettingsChanged,
    /* BeforeUninit   */ BeforeUninit,
    /* Uninit         */ Uninit,
};
