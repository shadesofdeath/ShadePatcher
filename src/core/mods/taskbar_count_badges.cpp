//
// taskbar_count_badges - shows how many windows each Windows 11 taskbar button stands for.
//
// Adapted from the idea behind the Windhawk mod "Taskbar Count Badges" (taskbar-count-badges) by digART. The
// logic is written against this engine's API and keeps the core of it: a small badge on every taskbar button
// that represents two or more windows, drawn either as a number or as a row of dots, on the side of the icon
// the user picks, in the colour the user picks. The original's shape, size, offset, border, font and
// maximum-number options, its "bottom dots replace the running indicator" mode and its enumeration of existing
// buttons through taskbar.dll internals are not carried over.
//
// What it does
// ------------
// Every button on the XAML taskbar is a TaskListButton whose view model (TaskListGroupViewModel) knows how many
// windows it groups. The count is read from that view model and shown inside the button's IconPanel:
//
//   Style 0  a number in a small filled circle (a pill for two digits, "99+" beyond)
//   Style 1  one dot per window, up to five; five means five or more
//
//   Position  which side of the icon: 0 right, 1 left, 2 top, 3 bottom. For the number that is the top-right
//             or top-left corner, or the centre of the top / bottom edge; for the dots it is a vertical stack
//             beside the icon or a horizontal row above / below it.
//   MinimumCount  the badge is shown from this many windows (default 2: a single window has no badge).
//   BadgeColor    RRGGBB; empty means the original's red for the number and the theme's text colour for the
//                 dots. The number's text picks black or white by the colour's brightness.
//
// Where it hooks (all in Taskbar.View.dll, the XAML taskbar)
// ----------------------------------------------------------
// TaskListButton::UpdateVisualStates runs whenever a button lays itself out after a change, and its
// implementation pointer gives the XAML element (the fourth slot, the way every mod on this taskbar reads it).
// The button is remembered there, and the badge work is queued to the button's own dispatcher rather than done
// inside the call: adding a child during a layout pass re-enters the layout.
//
// TryGetItemFromContainer<TaskListGroupViewModel> maps the button element to its view model, and the view
// model's get_ViewModelCount is the count. The value Windows reports there is one more than the number of
// windows (verified by the original on 24H2; kept as is here), so one is taken off. get_ViewModelCount is
// also hooked, only to learn that a count changed: XAML reads it after every change, and the read queues one
// refresh of the remembered buttons. Nothing is touched inside that getter.
//
// Existing buttons
// ----------------
// A button that already exists when the mod starts is not laid out again until something changes on it. The
// original walked taskbar.dll's TaskbarHost to reach the XAML root; here the first UpdateVisualStates on any
// button of a taskbar sweeps its sibling buttons in the same repeater, so the first hover or window change
// after enabling covers the whole taskbar. A WM_SETTINGCHANGE is sent to the taskbar windows once the hooks are
// in, which on most builds makes the buttons update their states straight away.
//
// Threads
// -------
// The hooks run on the taskbar's UI thread (one per taskbar; secondary taskbars have their own). The list of
// remembered buttons is shared under a mutex and only ever holds weak references plus each button's
// dispatcher, the one member of a XAML object that may be used from any thread. Each entry's XAML state is
// touched only on its own thread, selected with CoreDispatcher::HasThreadAccess. Settings live in atomics.
//
// Exceptions
// ----------
// C++/WinRT reports failures by throwing, so this file is compiled with exceptions on while the rest of the
// engine is not. Everything XAML can call into is wrapped: an exception reaching the taskbar's UI thread would
// end the shell.
//
#define SP_MOD_ID "taskbar-count-badges"
#include "engine/modapi.h"

#include <unknwn.h>

#include <atomic>
#include <climits>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// windows.h defines GetCurrentTime as a macro, which collides with a XAML method of the same name.
#undef GetCurrentTime

#include <winrt/Windows.Foundation.h>
// Panel children are an IVector, whose methods only become callable with this header.
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>

namespace {

using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using winrt::Windows::UI::Color;
using winrt::Windows::UI::Core::CoreDispatcher;
using winrt::Windows::UI::Core::CoreDispatcherPriority;
using winrt::Windows::UI::Xaml::Media::SolidColorBrush;
using winrt::Windows::UI::Xaml::Media::VisualTreeHelper;

// ---------------------------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------------------------

enum class BadgeStyle
{
    Number = 0,
    Dots = 1,
};

enum class BadgeSide
{
    Right = 0,
    Left = 1,
    Top = 2,
    Bottom = 3,
};

constexpr int    kDefaultMinimumCount = 2;
constexpr double kBadgeSize = 16.0;       // the number badge's height and minimum width
constexpr double kBadgeFontSize = 10.0;
constexpr double kDotSize = 4.0;
constexpr double kDotSpacing = 2.0;
constexpr unsigned kMaxDots = 5;
constexpr unsigned kMaxNumber = 99;

// The original's badge red.
constexpr Color kDefaultBadgeColor{ 255, 217, 0, 0 };
constexpr Color kWhite{ 255, 255, 255, 255 };
constexpr Color kBlack{ 255, 0, 0, 0 };

std::atomic<BadgeStyle> g_style{ BadgeStyle::Number };
std::atomic<BadgeSide>  g_side{ BadgeSide::Right };
std::atomic<unsigned>   g_minimumCount{ kDefaultMinimumCount };
std::atomic<uint32_t>   g_color{ 0 };           // ARGB; 0 = default per style
std::atomic<uint32_t>   g_generation{ 1 };      // bumped on every settings load, so styled badges are redone

std::atomic<bool> g_hooked{ false };
std::atomic<bool> g_unloading{ false };
std::atomic<int>  g_pendingDispatches{ 0 };

// Names given to the elements this mod adds, so a leftover from an earlier instance is recognised and reused
// or removed rather than duplicated.
constexpr wchar_t kBadgeName[] = L"ShadePatcherCountBadge";
constexpr wchar_t kTextName[]  = L"ShadePatcherCountText";
constexpr wchar_t kDotsName[]  = L"ShadePatcherCountDots";

// RRGGBB or AARRGGBB, with or without a leading '#'. Returns 0 for anything else.
uint32_t ParseColor(const wchar_t* text)
{
    if (!text)
    {
        return 0;
    }
    if (*text == L'#')
    {
        ++text;
    }

    size_t length = wcslen(text);
    if (length != 6 && length != 8)
    {
        return 0;
    }

    uint32_t value = 0;
    for (size_t i = 0; i < length; ++i)
    {
        wchar_t c = text[i];
        int digit;
        if (c >= L'0' && c <= L'9')
        {
            digit = c - L'0';
        }
        else if (c >= L'a' && c <= L'f')
        {
            digit = c - L'a' + 10;
        }
        else if (c >= L'A' && c <= L'F')
        {
            digit = c - L'A' + 10;
        }
        else
        {
            return 0;
        }
        value = (value << 4) | (uint32_t)digit;
    }

    if (length == 6)
    {
        value |= 0xFF000000u;
    }
    // A fully transparent colour is indistinguishable from "unset"; treat it as the default.
    return value;
}

void LoadSettings()
{
    int style = SP_GetIntSetting(L"Style", (int)BadgeStyle::Number);
    if (style < (int)BadgeStyle::Number || style > (int)BadgeStyle::Dots)
    {
        style = (int)BadgeStyle::Number;
    }
    g_style.store((BadgeStyle)style, std::memory_order_relaxed);

    int side = SP_GetIntSetting(L"Position", (int)BadgeSide::Right);
    if (side < (int)BadgeSide::Right || side > (int)BadgeSide::Bottom)
    {
        side = (int)BadgeSide::Right;
    }
    g_side.store((BadgeSide)side, std::memory_order_relaxed);

    int minimum = SP_GetIntSetting(L"MinimumCount", kDefaultMinimumCount);
    if (minimum < 1 || minimum > 99)
    {
        minimum = kDefaultMinimumCount;
    }
    g_minimumCount.store((unsigned)minimum, std::memory_order_relaxed);

    wchar_t colorText[32] = {};
    SP_GetStringSetting(L"BadgeColor", colorText, ARRAYSIZE(colorText), L"");
    uint32_t color = ParseColor(colorText);
    if (colorText[0] && !color)
    {
        SP_LogError(L"BadgeColor \"%s\" is not RRGGBB; using the default colour", colorText);
    }
    g_color.store(color, std::memory_order_relaxed);

    g_generation.fetch_add(1, std::memory_order_relaxed);

    SP_Log(L"Style %d, position %d, shown from %d window(s), colour %08X", style, side, minimum, color);
}

// ---------------------------------------------------------------------------------------------------------------
// Resolved functions
// ---------------------------------------------------------------------------------------------------------------

using TaskListButton_UpdateVisualStates_t = void(WINAPI*)(void* pThis);
// Returns a C++/WinRT object, so on x64 the result goes through a hidden first argument; the container is a
// pointer to the projected UIElement, which is one ABI pointer wide.
using TryGetGroupViewModel_t = void*(WINAPI*)(void** result, void* container);
using GetViewModelCount_t = HRESULT(WINAPI*)(void* pThis, unsigned int* count);

TaskListButton_UpdateVisualStates_t g_origUpdateVisualStates = nullptr;
TryGetGroupViewModel_t              g_TryGetGroupViewModel = nullptr;
GetViewModelCount_t                 g_origGetViewModelCount = nullptr;

// ---------------------------------------------------------------------------------------------------------------
// Remembered buttons
// ---------------------------------------------------------------------------------------------------------------

struct TrackedButton
{
    void*                             identity = nullptr;   // the element's IUnknown, for lookup
    winrt::weak_ref<FrameworkElement> element;
    winrt::weak_ref<Border>           badge;
    CoreDispatcher                    dispatcher{ nullptr };
    std::atomic<bool>                 sweepPending{ false };  // look at the sibling buttons on the next refresh

    // Only touched on the button's own thread.
    unsigned lastCount = UINT_MAX;
    uint32_t lastGeneration = 0;
};

struct KnownDispatcher
{
    CoreDispatcher                     dispatcher{ nullptr };
    std::shared_ptr<std::atomic<bool>> refreshQueued;
};

std::mutex                                  g_lock;
std::vector<std::shared_ptr<TrackedButton>> g_tracked;
std::vector<KnownDispatcher>                g_dispatchers;

bool SameObject(CoreDispatcher const& a, CoreDispatcher const& b)
{
    return winrt::get_abi(a) == winrt::get_abi(b);
}

void* IdentityOf(FrameworkElement const& element)
{
    if (auto unknown = element.try_as<winrt::Windows::Foundation::IUnknown>())
    {
        return winrt::get_abi(unknown);
    }
    return winrt::get_abi(element);
}

// Caller holds g_lock.
void RememberDispatcherLocked(CoreDispatcher const& dispatcher)
{
    for (auto const& known : g_dispatchers)
    {
        if (SameObject(known.dispatcher, dispatcher))
        {
            return;
        }
    }
    KnownDispatcher known;
    known.dispatcher = dispatcher;
    known.refreshQueued = std::make_shared<std::atomic<bool>>(false);
    g_dispatchers.push_back(known);
}

// UI thread of the element. Returns the entry for it, new or existing. The taskbar recycles its button
// containers, so an entry whose element has died at the same address is started over.
std::shared_ptr<TrackedButton> TrackButton(FrameworkElement const& element, bool sweepSiblings)
{
    void* identity = IdentityOf(element);
    CoreDispatcher dispatcher = element.Dispatcher();

    std::lock_guard<std::mutex> lock(g_lock);
    for (auto const& entry : g_tracked)
    {
        if (entry->identity != identity)
        {
            continue;
        }
        if (!entry->element.get())
        {
            entry->element = winrt::make_weak(element);
            entry->badge = {};
            entry->dispatcher = dispatcher;
            entry->lastCount = UINT_MAX;
            entry->lastGeneration = 0;
            if (sweepSiblings)
            {
                entry->sweepPending.store(true, std::memory_order_relaxed);
            }
            RememberDispatcherLocked(dispatcher);
        }
        return entry;
    }

    auto entry = std::make_shared<TrackedButton>();
    entry->identity = identity;
    entry->element = winrt::make_weak(element);
    entry->dispatcher = dispatcher;
    entry->sweepPending.store(sweepSiblings, std::memory_order_relaxed);
    g_tracked.push_back(entry);
    RememberDispatcherLocked(dispatcher);
    return entry;
}

void Forget(std::shared_ptr<TrackedButton> const& entry)
{
    std::lock_guard<std::mutex> lock(g_lock);
    for (size_t i = 0; i < g_tracked.size(); ++i)
    {
        if (g_tracked[i] == entry)
        {
            g_tracked.erase(g_tracked.begin() + i);
            return;
        }
    }
}

// The entries whose XAML objects belong to the calling thread. HasThreadAccess is the one question a
// dispatcher answers from any thread, which is what makes this safe to ask under the lock.
std::vector<std::shared_ptr<TrackedButton>> SnapshotForThisThread()
{
    std::vector<std::shared_ptr<TrackedButton>> result;
    std::lock_guard<std::mutex> lock(g_lock);
    for (auto const& entry : g_tracked)
    {
        try
        {
            if (entry->dispatcher && entry->dispatcher.HasThreadAccess())
            {
                result.push_back(entry);
            }
        }
        catch (...)
        {
        }
    }
    return result;
}

// ---------------------------------------------------------------------------------------------------------------
// Running on a taskbar's UI thread
// ---------------------------------------------------------------------------------------------------------------

// Queues `func` on the dispatcher. The pending counter is what Uninit waits on, so a queued callback can never
// run after this code has been unloaded.
template <typename F>
bool RunOnDispatcher(CoreDispatcher const& dispatcher, F func)
{
    if (!dispatcher)
    {
        return false;
    }

    g_pendingDispatches.fetch_add(1, std::memory_order_relaxed);
    try
    {
        dispatcher.TryRunAsync(CoreDispatcherPriority::Normal, [func]() {
            try
            {
                func();
            }
            catch (winrt::hresult_error const& e)
            {
                SP_LogDebug(L"Badge work failed: 0x%08X", (unsigned)e.code());
            }
            catch (...)
            {
                SP_LogDebug(L"Badge work failed");
            }
            g_pendingDispatches.fetch_sub(1, std::memory_order_relaxed);
        });
        return true;
    }
    catch (...)
    {
        g_pendingDispatches.fetch_sub(1, std::memory_order_relaxed);
        return false;
    }
}

void RefreshOnThisThread();

// Queues one refresh per taskbar thread (or for one dispatcher only), coalescing until the refresh has run:
// count reads caused by the refresh's own XAML work collapse into it instead of queueing another.
void QueueRefresh(CoreDispatcher const* only)
{
    if (g_unloading.load(std::memory_order_relaxed))
    {
        return;
    }

    std::vector<KnownDispatcher> targets;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        for (auto const& known : g_dispatchers)
        {
            if (only && !SameObject(known.dispatcher, *only))
            {
                continue;
            }
            if (!known.refreshQueued->exchange(true, std::memory_order_acq_rel))
            {
                targets.push_back(known);
            }
        }
    }

    for (auto const& target : targets)
    {
        auto queued = target.refreshQueued;
        bool posted = RunOnDispatcher(target.dispatcher, [queued]() {
            try
            {
                RefreshOnThisThread();
            }
            catch (...)
            {
                queued->store(false, std::memory_order_release);
                throw;
            }
            queued->store(false, std::memory_order_release);
        });
        if (!posted)
        {
            queued->store(false, std::memory_order_release);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// XAML helpers (UI thread)
// ---------------------------------------------------------------------------------------------------------------

FrameworkElement FindDescendantByName(DependencyObject const& root, const wchar_t* name, int maxDepth)
{
    if (!root || maxDepth < 0)
    {
        return nullptr;
    }

    const int count = VisualTreeHelper::GetChildrenCount(root);
    for (int i = 0; i < count; ++i)
    {
        auto child = VisualTreeHelper::GetChild(root, i);
        if (auto element = child.try_as<FrameworkElement>())
        {
            if (element.Name() == name)
            {
                return element;
            }
        }
        if (auto found = FindDescendantByName(child, name, maxDepth - 1))
        {
            return found;
        }
    }
    return nullptr;
}

// The XAML object behind a TaskListButton implementation pointer. The implementation object starts with one
// vtable pointer per interface it implements, and the fourth slot is the one the original mods on this taskbar
// all read the element through; asking it for FrameworkElement goes through a normal QueryInterface.
FrameworkElement ButtonFromImplementation(void* pThis)
{
    if (!pThis)
    {
        return nullptr;
    }

    void* abi = (void**)pThis + 3;
    if (!*(void**)abi)
    {
        return nullptr;
    }

    winrt::Windows::Foundation::IUnknown unknown{ nullptr };
    winrt::copy_from_abi(unknown, abi);
    return unknown.try_as<FrameworkElement>();
}

bool IsTaskListButton(FrameworkElement const& element)
{
    if (element.Name() == L"TaskListButton")
    {
        return true;
    }
    try
    {
        return winrt::get_class_name(element) == L"Taskbar.TaskListButton";
    }
    catch (...)
    {
        return false;
    }
}

void RemoveFromParent(Border const& badge)
{
    auto parent = badge.Parent().try_as<Panel>();
    if (!parent)
    {
        return;
    }
    auto children = parent.Children();
    uint32_t index = 0;
    if (children.IndexOf(badge, index))
    {
        children.RemoveAt(index);
    }
}

// Removes every badge of ours under a button's IconPanel, whoever added it.
void RemoveLeftoverBadges(FrameworkElement const& button)
{
    auto iconPanel = FindDescendantByName(button, L"IconPanel", 6).try_as<Panel>();
    if (!iconPanel)
    {
        return;
    }
    auto children = iconPanel.Children();
    for (uint32_t i = children.Size(); i > 0; --i)
    {
        auto child = children.GetAt(i - 1).try_as<FrameworkElement>();
        if (child && child.Name() == kBadgeName)
        {
            children.RemoveAt(i - 1);
        }
    }
}

// The number of windows behind a button, through its group view model. False when the button has none (a
// pinned program that is not running, or a container being recycled).
bool WindowCountOf(FrameworkElement const& button, unsigned* count)
{
    *count = 0;
    if (!g_TryGetGroupViewModel || !g_origGetViewModelCount)
    {
        return false;
    }

    UIElement uiElement = button;
    void* container = winrt::get_abi(uiElement);

    winrt::com_ptr<::IUnknown> viewModel;
    g_TryGetGroupViewModel(viewModel.put_void(), &container);
    if (!viewModel)
    {
        return false;
    }

    unsigned reported = 0;
    HRESULT hr = g_origGetViewModelCount(viewModel.get(), &reported);
    if (FAILED(hr))
    {
        return false;
    }

    // Windows reports one more than the number of windows (1 window -> 2). Windows' value is left alone; only
    // the number shown is corrected.
    *count = reported > 0 ? reported - 1 : 0;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Drawing the badge (UI thread)
// ---------------------------------------------------------------------------------------------------------------

Color ColorFromArgb(uint32_t argb)
{
    Color color{};
    color.A = (uint8_t)(argb >> 24);
    color.R = (uint8_t)(argb >> 16);
    color.G = (uint8_t)(argb >> 8);
    color.B = (uint8_t)argb;
    return color;
}

// Black on a light badge, white on a dark one.
Color TextColorFor(Color const& background)
{
    const int luminance = (299 * background.R + 587 * background.G + 114 * background.B) / 1000;
    return luminance > 160 ? kBlack : kWhite;
}

// The taskbar's own text colour, so dots with no colour set follow the theme; white when it cannot be read.
Color ThemeForegroundColor()
{
    try
    {
        auto app = Application::Current();
        if (app)
        {
            auto resources = app.Resources();
            auto key = winrt::box_value(winrt::hstring(L"TextFillColorPrimaryBrush"));
            if (resources.HasKey(key))
            {
                if (auto brush = resources.Lookup(key).try_as<SolidColorBrush>())
                {
                    return brush.Color();
                }
            }
        }
    }
    catch (...)
    {
    }
    return kWhite;
}

Border CreateBadge(Panel const& parent)
{
    Border badge;
    badge.Name(kBadgeName);
    badge.IsHitTestVisible(false);

    Grid grid;

    TextBlock text;
    text.Name(kTextName);
    text.HorizontalAlignment(HorizontalAlignment::Center);
    text.VerticalAlignment(VerticalAlignment::Center);
    text.TextAlignment(TextAlignment::Center);
    grid.Children().Append(text);

    StackPanel dots;
    dots.Name(kDotsName);
    dots.HorizontalAlignment(HorizontalAlignment::Center);
    dots.VerticalAlignment(VerticalAlignment::Center);
    grid.Children().Append(dots);

    badge.Child(grid);

    // Above the icon and whatever else the panel holds. A child of a Grid sits in column 0 unless told
    // otherwise, which keeps the badge on the icon when the panel has a label column too.
    Canvas::SetZIndex(badge, 100);
    parent.Children().Append(badge);
    return badge;
}

// Lays the badge out for the current settings and count. False when the badge is not one of ours.
bool ApplyStyle(Border const& badge, unsigned count)
{
    auto text = FindDescendantByName(badge, kTextName, 3).try_as<TextBlock>();
    auto dots = FindDescendantByName(badge, kDotsName, 3).try_as<StackPanel>();
    if (!text || !dots)
    {
        return false;
    }

    const BadgeStyle style = g_style.load(std::memory_order_relaxed);
    const BadgeSide side = g_side.load(std::memory_order_relaxed);
    const uint32_t argb = g_color.load(std::memory_order_relaxed);
    const double automatic = std::numeric_limits<double>::quiet_NaN();

    HorizontalAlignment horizontal = HorizontalAlignment::Center;
    if (side == BadgeSide::Right)
    {
        horizontal = HorizontalAlignment::Right;
    }
    else if (side == BadgeSide::Left)
    {
        horizontal = HorizontalAlignment::Left;
    }

    badge.Margin(Thickness{ 1, 1, 1, 1 });

    if (style == BadgeStyle::Number)
    {
        const Color background = argb ? ColorFromArgb(argb) : kDefaultBadgeColor;

        // A corner for right and left, the middle of the edge for top and bottom.
        badge.HorizontalAlignment(horizontal);
        badge.VerticalAlignment(side == BadgeSide::Bottom ? VerticalAlignment::Bottom : VerticalAlignment::Top);
        badge.Height(kBadgeSize);
        badge.MinWidth(kBadgeSize);
        badge.Width(automatic);
        badge.CornerRadius(CornerRadius{ kBadgeSize / 2, kBadgeSize / 2, kBadgeSize / 2, kBadgeSize / 2 });
        badge.Background(SolidColorBrush(background));

        std::wstring label = count > kMaxNumber ? std::to_wstring(kMaxNumber) + L"+" : std::to_wstring(count);
        // Two digits or more turn the circle into a pill.
        badge.Padding(label.size() > 1 ? Thickness{ 3, 0, 3, 0 } : Thickness{ 0, 0, 0, 0 });

        dots.Visibility(Visibility::Collapsed);
        text.Visibility(Visibility::Visible);
        text.FontSize(kBadgeFontSize);
        text.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        text.Foreground(SolidColorBrush(TextColorFor(background)));
        // Digits sit below the middle of their line box; one pixel up centres them in the circle.
        text.Margin(Thickness{ 0, -1, 0, 0 });
        text.Text(winrt::hstring(label));
        return true;
    }

    const Color dotColor = argb ? ColorFromArgb(argb) : ThemeForegroundColor();
    const bool vertical = (side == BadgeSide::Right || side == BadgeSide::Left);

    badge.HorizontalAlignment(horizontal);
    if (vertical)
    {
        badge.VerticalAlignment(VerticalAlignment::Center);
    }
    else
    {
        badge.VerticalAlignment(side == BadgeSide::Top ? VerticalAlignment::Top : VerticalAlignment::Bottom);
    }
    badge.Height(automatic);
    badge.MinWidth(0);
    badge.Width(automatic);
    badge.CornerRadius(CornerRadius{ 0, 0, 0, 0 });
    badge.Background(nullptr);
    badge.Padding(Thickness{ 0, 0, 0, 0 });

    text.Visibility(Visibility::Collapsed);
    dots.Visibility(Visibility::Visible);
    dots.Orientation(vertical ? Orientation::Vertical : Orientation::Horizontal);

    auto children = dots.Children();
    children.Clear();

    SolidColorBrush brush(dotColor);
    const unsigned shown = count < kMaxDots ? count : kMaxDots;
    for (unsigned i = 0; i < shown; ++i)
    {
        Border dot;
        dot.Width(kDotSize);
        dot.Height(kDotSize);
        dot.CornerRadius(CornerRadius{ kDotSize / 2, kDotSize / 2, kDotSize / 2, kDotSize / 2 });
        dot.Background(brush);
        dot.IsHitTestVisible(false);
        if (i > 0)
        {
            dot.Margin(vertical ? Thickness{ 0, kDotSpacing, 0, 0 } : Thickness{ kDotSpacing, 0, 0, 0 });
        }
        children.Append(dot);
    }
    return true;
}

// Puts the badge for `count` on a button, or hides it. False when the button has no panel to draw in.
bool UpdateBadge(TrackedButton& entry, FrameworkElement const& button, unsigned count)
{
    auto iconPanel = FindDescendantByName(button, L"IconPanel", 6).try_as<Panel>();
    if (!iconPanel)
    {
        if (auto badge = entry.badge.get())
        {
            RemoveFromParent(badge);
        }
        entry.badge = {};
        return false;
    }

    Border badge = entry.badge.get();
    if (badge)
    {
        // The panel can be rebuilt under a recycled button; a badge left in an old panel is dropped.
        auto parent = badge.Parent().try_as<Panel>();
        if (!parent || parent != iconPanel)
        {
            RemoveFromParent(badge);
            badge = nullptr;
            entry.badge = {};
        }
    }

    if (!badge)
    {
        // A badge an earlier instance of this mod left behind is taken over when it is whole.
        auto leftover = FindDescendantByName(iconPanel, kBadgeName, 1).try_as<Border>();
        if (leftover)
        {
            if (FindDescendantByName(leftover, kTextName, 3) && FindDescendantByName(leftover, kDotsName, 3))
            {
                badge = leftover;
            }
            else
            {
                RemoveFromParent(leftover);
            }
        }
    }

    if (count < g_minimumCount.load(std::memory_order_relaxed))
    {
        if (badge)
        {
            badge.Visibility(Visibility::Collapsed);
            entry.badge = winrt::make_weak(badge);
        }
        return true;
    }

    if (!badge)
    {
        badge = CreateBadge(iconPanel);
    }
    entry.badge = winrt::make_weak(badge);

    if (!ApplyStyle(badge, count))
    {
        badge.Visibility(Visibility::Collapsed);
        return false;
    }

    badge.Visibility(Visibility::Visible);
    return true;
}

// Reads the count and redraws only when the count or the settings changed since the last time.
void ApplyCountToButton(TrackedButton& entry, FrameworkElement const& button)
{
    unsigned count = 0;
    WindowCountOf(button, &count);

    const uint32_t generation = g_generation.load(std::memory_order_relaxed);
    const bool expected = count >= g_minimumCount.load(std::memory_order_relaxed);

    if (entry.lastCount == count && entry.lastGeneration == generation && (!expected || entry.badge.get()))
    {
        return;
    }

    // Marked before touching XAML, so a synchronous re-entry from the layout does not redo the same work.
    const unsigned previousCount = entry.lastCount;
    const uint32_t previousGeneration = entry.lastGeneration;
    entry.lastCount = count;
    entry.lastGeneration = generation;

    bool done = false;
    try
    {
        done = UpdateBadge(entry, button, count);
    }
    catch (winrt::hresult_error const& e)
    {
        SP_LogDebug(L"The badge could not be updated: 0x%08X", (unsigned)e.code());
    }
    catch (...)
    {
        SP_LogDebug(L"The badge could not be updated");
    }

    if (!done)
    {
        entry.lastCount = previousCount;
        entry.lastGeneration = previousGeneration;
    }
}

// Remembers every TaskListButton next to this one in its repeater, so buttons that existed before the hooks
// went in are covered by the first button that updates.
void SweepSiblings(FrameworkElement const& button)
{
    auto parent = VisualTreeHelper::GetParent(button);
    if (!parent)
    {
        return;
    }

    int found = 0;
    const int count = VisualTreeHelper::GetChildrenCount(parent);
    for (int i = 0; i < count; ++i)
    {
        auto sibling = VisualTreeHelper::GetChild(parent, i).try_as<FrameworkElement>();
        if (sibling && IsTaskListButton(sibling))
        {
            TrackButton(sibling, false);
            ++found;
        }
    }
    SP_LogDebug(L"Swept %d button(s) on this taskbar", found);
}

// Queued refresh: brings every remembered button on this thread up to date.
void RefreshOnThisThread()
{
    if (g_unloading.load(std::memory_order_relaxed))
    {
        return;
    }

    auto snapshot = SnapshotForThisThread();
    bool swept = false;
    for (auto const& entry : snapshot)
    {
        if (entry->sweepPending.exchange(false, std::memory_order_relaxed))
        {
            try
            {
                if (auto button = entry->element.get())
                {
                    SweepSiblings(button);
                    swept = true;
                }
            }
            catch (...)
            {
                SP_LogDebug(L"The sibling buttons could not be swept");
            }
        }
    }
    if (swept)
    {
        snapshot = SnapshotForThisThread();
    }

    for (auto const& entry : snapshot)
    {
        if (g_unloading.load(std::memory_order_relaxed))
        {
            return;
        }
        auto button = entry->element.get();
        if (!button)
        {
            Forget(entry);      // the badge died with its panel
            continue;
        }
        ApplyCountToButton(*entry, button);
    }
}

// BeforeUninit: takes every badge on this thread out of the tree and forgets the buttons.
void CleanupOnThisThread()
{
    auto snapshot = SnapshotForThisThread();
    int removed = 0;
    for (auto const& entry : snapshot)
    {
        try
        {
            if (auto badge = entry->badge.get())
            {
                RemoveFromParent(badge);
                ++removed;
            }
            if (auto button = entry->element.get())
            {
                RemoveLeftoverBadges(button);
            }
        }
        catch (...)
        {
        }
        entry->badge = {};
        Forget(entry);
    }
    SP_Log(L"Removed %d badge(s) from %u button(s) on this taskbar", removed, (unsigned)snapshot.size());
}

// ---------------------------------------------------------------------------------------------------------------
// Hooks in Taskbar.View.dll
// ---------------------------------------------------------------------------------------------------------------

void WINAPI TaskListButton_UpdateVisualStates_Hook(void* pThis)
{
    g_origUpdateVisualStates(pThis);

    if (g_unloading.load(std::memory_order_relaxed))
    {
        return;
    }

    // XAML is on the stack below; nothing may escape back into it, and nothing in the tree is changed here.
    try
    {
        auto button = ButtonFromImplementation(pThis);
        if (!button)
        {
            return;
        }

        auto entry = TrackButton(button, true);
        QueueRefresh(&entry->dispatcher);
    }
    catch (winrt::hresult_error const& e)
    {
        SP_LogDebug(L"A button could not be tracked: 0x%08X", (unsigned)e.code());
    }
    catch (...)
    {
        SP_LogDebug(L"A button could not be tracked");
    }
}

// Notification only. XAML reads this property after every change, which is the one reliable sign that a count
// moved; the refresh is queued so nothing happens while the repeater may be measuring a button.
HRESULT WINAPI TaskListGroupViewModel_get_ViewModelCount_Hook(void* pThis, unsigned int* count)
{
    HRESULT hr = g_origGetViewModelCount(pThis, count);

    if (FAILED(hr) || !count || g_unloading.load(std::memory_order_relaxed))
    {
        return hr;
    }

    try
    {
        QueueRefresh(nullptr);
    }
    catch (...)
    {
    }
    return hr;
}

// ---------------------------------------------------------------------------------------------------------------
// Installing
// ---------------------------------------------------------------------------------------------------------------

BOOL InstallTaskbarViewHooks(HMODULE hTaskbarView)
{
    static const wchar_t* const kUpdateVisualStates[] =
    {
        LR"(private: void __cdecl winrt::Taskbar::implementation::TaskListButton::UpdateVisualStates(void))",
    };
    static const wchar_t* const kTryGetGroupViewModel[] =
    {
        LR"(struct winrt::Taskbar::TaskListGroupViewModel __cdecl TryGetItemFromContainer<struct winrt::Taskbar::TaskListGroupViewModel>(struct winrt::Windows::UI::Xaml::UIElement const &))",
    };
    static const wchar_t* const kGetViewModelCount[] =
    {
        LR"(public: virtual int __cdecl winrt::impl::produce<struct winrt::Taskbar::implementation::TaskListGroupViewModel,struct winrt::Taskbar::ITaskListGroupViewModel>::get_ViewModelCount(unsigned int *))",
    };

    // All optional so that the batch goes in whatever is missing; what is missing is reported below, and the
    // mod stays inert unless all three resolved, because without any one of them there is nothing to show.
    SP_SymbolHook hooks[3] = {};

    hooks[0].symbols = kUpdateVisualStates;
    hooks[0].symbolCount = ARRAYSIZE(kUpdateVisualStates);
    hooks[0].pOriginal = (void**)&g_origUpdateVisualStates;
    hooks[0].hookFunction = (void*)TaskListButton_UpdateVisualStates_Hook;
    hooks[0].optional = TRUE;

    hooks[1].symbols = kTryGetGroupViewModel;
    hooks[1].symbolCount = ARRAYSIZE(kTryGetGroupViewModel);
    hooks[1].pOriginal = (void**)&g_TryGetGroupViewModel;
    hooks[1].hookFunction = nullptr;    // called, not intercepted
    hooks[1].optional = TRUE;

    hooks[2].symbols = kGetViewModelCount;
    hooks[2].symbolCount = ARRAYSIZE(kGetViewModelCount);
    hooks[2].pOriginal = (void**)&g_origGetViewModelCount;
    hooks[2].hookFunction = (void*)TaskListGroupViewModel_get_ViewModelCount_Hook;
    hooks[2].optional = TRUE;

    if (!SP_HookSymbols(hTaskbarView, hooks, ARRAYSIZE(hooks)))
    {
        SP_LogError(L"The taskbar button functions could not be hooked");
        return FALSE;
    }

    if (!g_origUpdateVisualStates)
    {
        SP_LogError(L"TaskListButton::UpdateVisualStates was not found; no button is ever seen");
    }
    if (!g_TryGetGroupViewModel)
    {
        SP_LogError(L"TryGetItemFromContainer<TaskListGroupViewModel> was not found; no counts can be read");
    }
    if (!g_origGetViewModelCount)
    {
        SP_LogError(L"TaskListGroupViewModel::get_ViewModelCount was not found; no counts can be read");
    }

    if (!g_origUpdateVisualStates || !g_TryGetGroupViewModel || !g_origGetViewModelCount)
    {
        return FALSE;
    }

    SP_Log(L"Count badge hooks are in: UpdateVisualStates and get_ViewModelCount hooked, "
           L"TryGetItemFromContainer resolved");
    return TRUE;
}

// Asks every taskbar of this process to re-read its settings. On builds where that makes the buttons update
// their visual states, the badges appear at once; otherwise the first hover or window change brings them.
void NudgeTaskbars()
{
    struct Nudged
    {
        int count = 0;
    } nudged;

    EnumWindows(
        [](HWND hWnd, LPARAM lParam) -> BOOL {
            DWORD processId = 0;
            wchar_t className[32];
            if (GetWindowThreadProcessId(hWnd, &processId) && processId == GetCurrentProcessId() &&
                GetClassNameW(hWnd, className, ARRAYSIZE(className)) &&
                (_wcsicmp(className, L"Shell_TrayWnd") == 0 || _wcsicmp(className, L"Shell_SecondaryTrayWnd") == 0))
            {
                // A timeout rather than a plain send: the shell's UI thread must never be able to hold this
                // thread forever. The plain setting change does nothing for the buttons on build 26200; the
                // "ImmersiveColorSet" one (a theme colour refresh) makes every button run UpdateVisualStates,
                // which is what the sweep hangs on. Both are sent.
                SendMessageTimeoutW(hWnd, WM_SETTINGCHANGE, 0, 0, SMTO_NORMAL | SMTO_ABORTIFHUNG, 2000, nullptr);
                SendMessageTimeoutW(hWnd, WM_SETTINGCHANGE, 0, (LPARAM)L"ImmersiveColorSet",
                                    SMTO_NORMAL | SMTO_ABORTIFHUNG, 2000, nullptr);
                ++reinterpret_cast<Nudged*>(lParam)->count;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&nudged));

    SP_LogDebug(L"Nudged %d taskbar window(s)", nudged.count);
}

// Runs on a helper thread once Taskbar.View.dll is in the process; at once when it already is.
void OnTaskbarViewLoaded(HMODULE hTaskbarView, void*)
{
    if (!hTaskbarView)
    {
        SP_LogError(L"Taskbar.View.dll never loaded; this build's taskbar is not the XAML one");
        return;
    }

    if (!InstallTaskbarViewHooks(hTaskbarView))
    {
        return;
    }

    g_hooked.store(true, std::memory_order_relaxed);

    // Buttons that already exist were laid out before the hooks; see NudgeTaskbars for what this does for them.
    NudgeTaskbars();
}

// ---------------------------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------------------------

BOOL Init()
{
    g_unloading.store(false, std::memory_order_relaxed);
    LoadSettings();

    // The XAML taskbar is a separate package the shell brings up a moment after the process starts, so on a
    // cold sign-in it is not there yet when this runs. The hooks go in when it appears.
    if (!SP_WaitForModule(L"Taskbar.View.dll", 60000, OnTaskbarViewLoaded, nullptr))
    {
        SP_LogError(L"Could not wait for Taskbar.View.dll");
        return FALSE;
    }

    return TRUE;
}

void SettingsChanged()
{
    LoadSettings();

    // The generation moved, so every remembered button redraws its badge on its own thread.
    if (g_hooked.load(std::memory_order_relaxed))
    {
        QueueRefresh(nullptr);
    }
}

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

void BeforeUninit()
{
    // From here the hooks only pass through and no refresh does anything; the badges are taken out on each
    // taskbar's own thread while the hooks are still in place.
    g_unloading.store(true, std::memory_order_relaxed);

    std::vector<CoreDispatcher> dispatchers;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        for (auto const& known : g_dispatchers)
        {
            dispatchers.push_back(known.dispatcher);
        }
    }

    for (auto const& dispatcher : dispatchers)
    {
        auto waiter = std::make_shared<Waiter>();
        if (!waiter->hEvent)
        {
            continue;
        }

        bool posted = RunOnDispatcher(dispatcher, [waiter]() {
            try
            {
                CleanupOnThisThread();
            }
            catch (...)
            {
                SP_LogError(L"The badges could not all be removed");
            }
            SetEvent(waiter->hEvent);
        });

        // Briefly: the restore should be done before the hooks go, but a UI thread that does not answer must
        // not hold the engine.
        if (posted)
        {
            WaitForSingleObject(waiter->hEvent, 3000);
        }
    }

    // Whatever is left holds only weak references and dispatchers, both safe to drop from this thread.
    std::lock_guard<std::mutex> lock(g_lock);
    g_tracked.clear();
    g_dispatchers.clear();
}

void Uninit()
{
    g_hooked.store(false, std::memory_order_relaxed);

    // A callback still queued on a taskbar thread must run (or be dropped) before this code goes away.
    for (int i = 0; i < 40 && g_pendingDispatches.load(std::memory_order_relaxed) > 0; ++i)
    {
        Sleep(50);
    }
}

}   // namespace

SP_MOD_DEFINE(g_modTaskbarCountBadges) =
{
    /* id             */ SP_MOD_ID,
    /* name           */ L"Show the window count on taskbar buttons",
    /* basedOn        */ "taskbar-count-badges",
    /* originalAuthor */ "digART",
    /* targets        */ SP_TARGET_EXPLORER,
    /* minOsBuild     */ 22000,
    /* flags          */ 0,
    /* Init           */ Init,
    /* AfterInit      */ nullptr,
    /* SettingsChanged*/ SettingsChanged,
    /* BeforeUninit   */ BeforeUninit,
    /* Uninit         */ Uninit,
};
