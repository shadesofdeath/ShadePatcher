//
// taskbar_notification_icon_spacing - width of the tray icons on the Windows 11 taskbar, an optional grid of
// tray icons, and the size of the icons in the tray overflow popup.
//
// Adapted from the idea behind the Windhawk mod "Taskbar tray icon spacing and grid"
// (taskbar-notification-icon-spacing) by m417z. The implementation here is written against this engine's API.
//
// What it does
// ------------
// Every icon in the notification area of the Windows 11 taskbar is 32 pixels wide, the icons in the overflow
// popup (behind the chevron) are 40 pixels wide and five to a row, and none of that is a setting. This mod makes
// those numbers the user's:
//
//   NotificationIconWidth   the width of one tray icon, the chevron, and the icons next to the clock
//   NotificationIconRows    how many rows the tray icons are laid out in (1 = the ordinary single row)
//   GridArrangement         the order the icons fill a grid of more than one row in
//   OverflowIconWidth       the width and height of one icon in the overflow popup
//   OverflowIconsPerRow     how many icons the overflow popup puts in a row
//
// How the numbers are applied
// ---------------------------
// The tray is XAML. Each icon is a SystemTray.NotifyIconView (tray area and overflow), a SystemTray.IconView
// (the icons next to the clock, the control center) or a SystemTray.ChevronIconView (the chevron), and all of
// them are built through the same constructor,
//     winrt::SystemTray::implementation::IconView::IconView
// which is hooked to attach a Loaded handler to the new element. When the element is in the tree the handler
// looks at what it is and where it sits and sets its MinWidth, drops the padding of its ContainerGrid and, for
// the language indicator, gives it a MinWidth that fits the text. Nothing in the shell has to be persuaded to
// re-layout: the values are ordinary dependency properties and XAML measures the tray again on its own.
//
// A grid of more than one row is not something the tray's StackPanel knows how to do, so it is faked: every
// item keeps its slot in the single row and is moved with a RenderTransform to the row and column it belongs
// in, and the panel's width is reduced to the number of columns. The stack's view model reindexes its icons in
//     winrt::SystemTray::implementation::StackViewModel::UpdateIconIndexes
// whenever an icon is added or removed, and that is hooked to lay the grid out again.
//
// The overflow popup lives in its own XAML island, created by
//     winrt::SystemTray::OverflowXamlIslandManager::InitializeIfNeeded
// the first time it is opened. The island's root grid holds a WrapGrid whose ItemWidth / ItemHeight and
// MaximumRowsOrColumns are the overflow numbers; the hook remembers the grid so a settings change can reach it.
//
// A settings change, and enabling the mod on a running shell, walks the tray's tree from the taskbar's XamlRoot
// and applies the same styling to every icon that is already on screen. The XamlRoot is reached the way the
// other taskbar mods do it: from the tray window's task band (taskbar.dll) to its TaskbarHost and the frame
// element it holds. The work is handed to the frame's dispatcher, since XAML objects belong to their own thread.
//
// Where the types live
// --------------------
// Up to Taskbar.View.dll 2603 the SystemTray types are inside Taskbar.View.dll; from 2604 on they are in
// SystemTray.dll (build 26200 has 2607). The mod waits for Taskbar.View.dll and then hooks whichever of the two
// holds the tray on this build, waiting for SystemTray.dll when it is not loaded yet.
//
// Unloading
// ---------
// BeforeUninit applies the stock numbers (32, one row, 40, five per row) to everything on screen while the
// hooks are still there, which is what the original does too; icons created after that are the shell's own.
//
// Exceptions
// ----------
// C++/WinRT reports failures by throwing, so this file is compiled with exceptions on while the rest of the
// engine is not. Every hook body, every Loaded handler and every dispatched callback is wrapped: an exception
// reaching the shell's UI thread would end the process.
//
#define SP_MOD_ID "taskbar-notification-icon-spacing"
#include "engine/modapi.h"

#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>

#undef GetCurrentTime

#include <unknwn.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>

namespace {

using namespace winrt::Windows::UI::Xaml;
using winrt::Windows::Foundation::IInspectable;
using winrt::Windows::UI::Core::CoreDispatcher;
using winrt::Windows::UI::Core::CoreDispatcherPriority;
using winrt::Windows::UI::Xaml::Media::VisualTreeHelper;

// ---------------------------------------------------------------------------------------------------------------
// Settings and state shared between the engine thread and the tray's thread
// ---------------------------------------------------------------------------------------------------------------

// What the shell uses on its own; applied again when the mod goes.
constexpr int kStockIconWidth = 32;
constexpr int kStockRows = 1;
constexpr int kStockOverflowIconWidth = 40;
constexpr int kStockOverflowIconsPerRow = 5;

// The order icons fill a grid of more than one row in. Examples with icons A-G and two rows:
//
//   0 row-first, left to right         A B C D / E F G
//   1 column-first, top to bottom      A C E G / B D F
//   2 row-first, bottom row first      E F G   / A B C D
//   3 column-first, bottom to top      B D F   / A C E G
//   4 column-first, bottom to top, right to left    _ F D B / G E C A
enum GridArrangement
{
    kRowFirstLeftToRight = 0,
    kColumnFirstTopToBottom = 1,
    kRowFirstBottomRowFirst = 2,
    kColumnFirstBottomToTop = 3,
    kColumnFirstBottomToTopRightToLeft = 4,
};

// Written on the engine thread, read inside the hooks and the Loaded handlers.
std::atomic<int> g_settingIconWidth{ kStockIconWidth };
std::atomic<int> g_settingRows{ kStockRows };
std::atomic<int> g_settingArrangement{ kRowFirstLeftToRight };
std::atomic<int> g_settingOverflowIconWidth{ kStockOverflowIconWidth };
std::atomic<int> g_settingOverflowIconsPerRow{ kStockOverflowIconsPerRow };

// Set in BeforeUninit: from then on every styling pass uses the stock numbers.
std::atomic<bool> g_unloading{ false };

// True between Init and Uninit. A Loaded handler attached to an icon that outlives the mod checks this and does
// nothing, so a stale callback can never style anything after the mod has been turned off.
std::atomic<bool> g_active{ false };

// The tray module's hooks are in place.
std::atomic<bool> g_trayHooked{ false };

// Callbacks handed to a XAML dispatcher that have not run yet. Uninit waits for them (briefly).
std::atomic<int> g_pendingDispatches{ 0 };

// The numbers a styling pass uses right now.
struct Numbers
{
    int iconWidth;
    int rows;
    int arrangement;
    int overflowIconWidth;
    int overflowIconsPerRow;
};

Numbers CurrentNumbers()
{
    Numbers n{};
    if (g_unloading.load(std::memory_order_relaxed))
    {
        n.iconWidth = kStockIconWidth;
        n.rows = kStockRows;
        n.arrangement = kRowFirstLeftToRight;
        n.overflowIconWidth = kStockOverflowIconWidth;
        n.overflowIconsPerRow = kStockOverflowIconsPerRow;
    }
    else
    {
        n.iconWidth = g_settingIconWidth.load(std::memory_order_relaxed);
        n.rows = g_settingRows.load(std::memory_order_relaxed);
        n.arrangement = g_settingArrangement.load(std::memory_order_relaxed);
        n.overflowIconWidth = g_settingOverflowIconWidth.load(std::memory_order_relaxed);
        n.overflowIconsPerRow = g_settingOverflowIconsPerRow.load(std::memory_order_relaxed);
    }
    return n;
}

// The two elements the hooks remember: the StackPanel holding the tray icons (needed to lay the grid out again
// when icons come and go) and the root grid of the overflow popup (needed to reach it on a settings change).
// Weak, so the mod never keeps a part of the tray alive; written on the tray's thread, read from the engine
// thread too, hence the lock.
std::mutex g_elementsLock;
winrt::weak_ref<FrameworkElement> g_trayIconsStackPanel;
winrt::weak_ref<FrameworkElement> g_overflowRootGrid;

void RememberTrayIconsStackPanel(FrameworkElement const& panel)
{
    std::lock_guard<std::mutex> guard(g_elementsLock);
    g_trayIconsStackPanel = panel;
}

FrameworkElement RememberedTrayIconsStackPanel()
{
    std::lock_guard<std::mutex> guard(g_elementsLock);
    return g_trayIconsStackPanel.get();
}

void RememberOverflowRootGrid(FrameworkElement const& grid)
{
    std::lock_guard<std::mutex> guard(g_elementsLock);
    g_overflowRootGrid = grid;
}

FrameworkElement RememberedOverflowRootGrid()
{
    std::lock_guard<std::mutex> guard(g_elementsLock);
    return g_overflowRootGrid.get();
}

void ForgetElements()
{
    std::lock_guard<std::mutex> guard(g_elementsLock);
    g_trayIconsStackPanel = nullptr;
    g_overflowRootGrid = nullptr;
}

// ---------------------------------------------------------------------------------------------------------------
// Visual tree helpers
// ---------------------------------------------------------------------------------------------------------------

// Calls `callback` with every direct child that is a FrameworkElement, in order; stops and returns the child
// for which the callback returns true.
template <typename F>
FrameworkElement EnumChildElements(FrameworkElement const& element, F&& callback)
{
    int count = VisualTreeHelper::GetChildrenCount(element);
    for (int i = 0; i < count; ++i)
    {
        auto child = VisualTreeHelper::GetChild(element, i).try_as<FrameworkElement>();
        if (!child)
        {
            continue;
        }
        if (callback(child))
        {
            return child;
        }
    }
    return nullptr;
}

FrameworkElement FindChildByName(FrameworkElement const& element, const wchar_t* name)
{
    return EnumChildElements(element, [name](FrameworkElement const& child) {
        return child.Name() == name;
    });
}

FrameworkElement FindChildByClassName(FrameworkElement const& element, const wchar_t* className)
{
    return EnumChildElements(element, [className](FrameworkElement const& child) {
        return winrt::get_class_name(child) == className;
    });
}

// Whether an ancestor of the element has that name. The walk is capped; the tray is nowhere near that deep.
bool HasAncestorNamed(FrameworkElement const& element, const wchar_t* name)
{
    DependencyObject current = element;
    for (int depth = 0; depth < 64; ++depth)
    {
        current = VisualTreeHelper::GetParent(current);
        if (!current)
        {
            return false;
        }
        if (auto parent = current.try_as<FrameworkElement>())
        {
            if (parent.Name() == name)
            {
                return true;
            }
        }
    }
    return false;
}

bool HasAncestorOfClass(FrameworkElement const& element, const wchar_t* className)
{
    DependencyObject current = element;
    for (int depth = 0; depth < 64; ++depth)
    {
        current = VisualTreeHelper::GetParent(current);
        if (!current)
        {
            return false;
        }
        if (winrt::get_class_name(current) == className)
        {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------
// Styling one icon
//
// Every routine here runs on the tray's thread, inside a Loaded handler, a hook or a dispatched callback, and
// the caller has wrapped it in try/catch.
// ---------------------------------------------------------------------------------------------------------------

// The icon's template: NotifyIconView > ContainerGrid > ContentPresenter > ContentGrid > <content>, where the
// content is a SystemTray.ImageIconContent (an image), a SystemTray.TextIconContent (a glyph) or a
// SystemTray.LanguageTextIconContent (the "ENG" of the input indicator), each with a padded ContainerGrid.
FrameworkElement IconContentGrid(FrameworkElement const& iconView)
{
    FrameworkElement child = iconView;
    if ((child = FindChildByName(child, L"ContainerGrid")) &&
        (child = FindChildByName(child, L"ContentPresenter")) &&
        (child = FindChildByName(child, L"ContentGrid")))
    {
        return child;
    }
    return nullptr;
}

// An icon in the tray area, the chevron, or an icon next to the clock.
void StyleTrayIcon(FrameworkElement const& iconView, int width)
{
    SP_LogDebug(L"Tray icon %s: MinWidth %d", winrt::get_class_name(iconView).c_str(), width);
    iconView.MinWidth(width);

    FrameworkElement contentGrid = IconContentGrid(iconView);
    if (!contentGrid)
    {
        return;
    }

    EnumChildElements(contentGrid, [width](FrameworkElement const& content) {
        auto className = winrt::get_class_name(content);
        if (className == L"SystemTray.TextIconContent" || className == L"SystemTray.ImageIconContent")
        {
            if (auto grid = FindChildByName(content, L"ContainerGrid").try_as<Controls::Grid>())
            {
                grid.Padding(Thickness{ 0, 0, 0, 0 });
            }
        }
        else if (className == L"SystemTray.LanguageTextIconContent")
        {
            // Every language has a different width ("ENG" is 24) and the stock width is 44; let the text size
            // the element, with a floor that keeps it looking like the icons around it.
            content.Width(std::numeric_limits<double>::quiet_NaN());
            content.MinWidth(width + 12.0);
        }
        else
        {
            SP_LogDebug(L"Unfamiliar icon content %s", className.c_str());
        }
        return false;
    });
}

// An icon in the overflow popup: square, so the height is set too.
void StyleOverflowIcon(FrameworkElement const& iconView, int width)
{
    SP_LogDebug(L"Overflow icon: MinWidth and Height %d", width);
    iconView.MinWidth(width);
    iconView.Height(width);

    FrameworkElement contentGrid = IconContentGrid(iconView);
    if (!contentGrid)
    {
        return;
    }

    EnumChildElements(contentGrid, [](FrameworkElement const& content) {
        if (winrt::get_class_name(content) == L"SystemTray.ImageIconContent")
        {
            if (auto grid = FindChildByName(content, L"ContainerGrid").try_as<Controls::Grid>())
            {
                grid.Padding(Thickness{ 0, 0, 0, 0 });
            }
        }
        return false;
    });
}

// An icon inside the control center button (network, volume, battery). Those are glyphs whose ContainerGrid
// carries 4 pixels of padding on each side at the stock width; the padding follows the width so the glyphs
// keep their distance from each other.
void StyleControlCenterIcon(FrameworkElement const& iconView, int width)
{
    FrameworkElement child = iconView;
    if ((child = FindChildByName(child, L"ContainerGrid")) &&
        (child = FindChildByName(child, L"ContentGrid")) &&
        (child = FindChildByClassName(child, L"SystemTray.TextIconContent")) &&
        (child = FindChildByName(child, L"ContainerGrid")))
    {
        auto grid = child.try_as<Controls::Grid>();
        if (!grid)
        {
            return;
        }

        int padding = 4;
        if (width > 32)
        {
            padding = (8 + width - 32) / 2;
        }
        else if (width < 24)
        {
            padding = (8 + width - 24) / 2;
            if (padding < 0)
            {
                padding = 0;
            }
        }

        SP_LogDebug(L"Control center icon: padding %d", padding);
        grid.Padding(Thickness{ (double)padding, 0, (double)padding, 0 });
    }
}

// ---------------------------------------------------------------------------------------------------------------
// The grid of tray icons
// ---------------------------------------------------------------------------------------------------------------

// Lays the children of the tray's StackPanel out in `rows` rows. With one row every override is cleared, which
// is also how the grid is undone.
void LayOutTrayIconsGrid(FrameworkElement const& stackPanel, Numbers const& n)
{
    const int rows = n.rows;
    const int width = n.iconWidth;

    double itemHeight = 0;
    if (rows > 1)
    {
        // The rows share the panel's height; an even gap keeps the 16 px icons on whole pixels.
        double gap = stackPanel.ActualHeight() - 16.0 * rows;
        double gapPerItem = std::fmax(gap, 0.0) / (rows + 1);
        int gapPerItemEven = (int)gapPerItem / 2 * 2;
        itemHeight = 16.0 + gapPerItemEven;
    }

    int childCount = VisualTreeHelper::GetChildrenCount(stackPanel);
    int cols = rows > 0 ? (childCount + rows - 1) / rows : childCount;
    if (cols < 1)
    {
        cols = 1;
    }

    int index = 0;
    EnumChildElements(stackPanel, [&](FrameworkElement const& child) {
        const int i = index++;

        if (winrt::get_class_name(child) != L"Windows.UI.Xaml.Controls.ContentPresenter")
        {
            return false;
        }

        if (rows > 1)
        {
            child.Height(itemHeight);

            int col = 0;
            int row = 0;
            switch (n.arrangement)
            {
            case kColumnFirstTopToBottom:
                col = i / rows;
                row = i % rows;
                break;
            case kRowFirstBottomRowFirst:
                col = i % cols;
                row = (rows - 1) - (i / cols);
                break;
            case kColumnFirstBottomToTop:
                col = i / rows;
                row = (rows - 1) - (i % rows);
                break;
            case kColumnFirstBottomToTopRightToLeft:
                col = (cols - 1) - (i / rows);
                row = (rows - 1) - (i % rows);
                break;
            case kRowFirstLeftToRight:
            default:
                col = i % cols;
                row = i / cols;
                break;
            }

            // The child keeps its slot in the single row; the transform moves it to its cell.
            Media::TranslateTransform transform;
            transform.X((double)(width * (col - i)));
            transform.Y(itemHeight * row - itemHeight * (rows - 1) / 2);
            child.RenderTransform(transform);
        }
        else
        {
            child.ClearValue(FrameworkElement::HeightProperty());
            child.ClearValue(UIElement::RenderTransformProperty());
        }
        return false;
    });

    if (rows > 1)
    {
        stackPanel.Width((double)(width * ((index + rows - 1) / rows)));
    }
    else
    {
        stackPanel.ClearValue(FrameworkElement::WidthProperty());
    }

    RememberTrayIconsStackPanel(stackPanel);
}

// The same, starting from one icon: NotifyIconView > ContentPresenter > StackPanel.
void LayOutTrayIconsGridOfIcon(FrameworkElement const& iconView, Numbers const& n)
{
    auto presenter = VisualTreeHelper::GetParent(iconView).try_as<FrameworkElement>();
    if (!presenter || winrt::get_class_name(presenter) != L"Windows.UI.Xaml.Controls.ContentPresenter")
    {
        return;
    }

    auto stackPanel = VisualTreeHelper::GetParent(presenter).try_as<FrameworkElement>();
    if (!stackPanel || winrt::get_class_name(stackPanel) != L"Windows.UI.Xaml.Controls.StackPanel")
    {
        return;
    }

    LayOutTrayIconsGrid(stackPanel, n);
}

// ---------------------------------------------------------------------------------------------------------------
// Styling everything that is on screen
// ---------------------------------------------------------------------------------------------------------------

// NotificationAreaIcons > ItemsPresenter > StackPanel > ContentPresenter* > NotifyItemIcon
bool StyleNotificationAreaIcons(FrameworkElement const& notificationAreaIcons, Numbers const& n)
{
    FrameworkElement child = notificationAreaIcons;
    if (!(child = FindChildByClassName(child, L"Windows.UI.Xaml.Controls.ItemsPresenter")) ||
        !(child = FindChildByClassName(child, L"Windows.UI.Xaml.Controls.StackPanel")))
    {
        return false;
    }
    FrameworkElement stackPanel = child;

    EnumChildElements(stackPanel, [&n](FrameworkElement const& presenter) {
        if (winrt::get_class_name(presenter) != L"Windows.UI.Xaml.Controls.ContentPresenter")
        {
            return false;
        }
        if (auto iconView = FindChildByName(presenter, L"NotifyItemIcon"))
        {
            StyleTrayIcon(iconView, n.iconWidth);
        }
        return false;
    });

    LayOutTrayIconsGrid(stackPanel, n);
    return true;
}

// ControlCenterButton > Grid > ContentPresenter > ItemsPresenter > StackPanel > ContentPresenter* > SystemTrayIcon
bool StyleControlCenterButton(FrameworkElement const& button, Numbers const& n)
{
    FrameworkElement child = button;
    if (!(child = FindChildByClassName(child, L"Windows.UI.Xaml.Controls.Grid")) ||
        !(child = FindChildByName(child, L"ContentPresenter")) ||
        !(child = FindChildByClassName(child, L"Windows.UI.Xaml.Controls.ItemsPresenter")) ||
        !(child = FindChildByClassName(child, L"Windows.UI.Xaml.Controls.StackPanel")))
    {
        return false;
    }

    EnumChildElements(child, [&n](FrameworkElement const& presenter) {
        if (winrt::get_class_name(presenter) != L"Windows.UI.Xaml.Controls.ContentPresenter")
        {
            return false;
        }
        if (auto iconView = FindChildByName(presenter, L"SystemTrayIcon"))
        {
            StyleControlCenterIcon(iconView, n.iconWidth);
        }
        return false;
    });
    return true;
}

// <stack> > Content > IconStack > ItemsPresenter > StackPanel > ContentPresenter* > icon. NotifyIconStack holds
// the chevron; MainStack and NonActivatableStack hold the icons next to the clock.
bool StyleIconStack(const wchar_t* stackName, FrameworkElement const& container, Numbers const& n)
{
    FrameworkElement child = container;
    if (!(child = FindChildByName(child, L"Content")) ||
        !(child = FindChildByName(child, L"IconStack")) ||
        !(child = FindChildByClassName(child, L"Windows.UI.Xaml.Controls.ItemsPresenter")) ||
        !(child = FindChildByClassName(child, L"Windows.UI.Xaml.Controls.StackPanel")))
    {
        return false;
    }

    const bool chevronStack = wcscmp(stackName, L"NotifyIconStack") == 0;
    EnumChildElements(child, [&n, chevronStack](FrameworkElement const& presenter) {
        if (winrt::get_class_name(presenter) != L"Windows.UI.Xaml.Controls.ContentPresenter")
        {
            return false;
        }
        FrameworkElement iconView = chevronStack
            ? FindChildByClassName(presenter, L"SystemTray.ChevronIconView")
            : FindChildByName(presenter, L"SystemTrayIcon");
        if (iconView)
        {
            StyleTrayIcon(iconView, n.iconWidth);
        }
        return false;
    });
    return true;
}

// Styles every part of the tray under the taskbar's XamlRoot. Returns false when the tray was not found.
bool StyleTray(XamlRoot const& xamlRoot, Numbers const& n)
{
    FrameworkElement child = xamlRoot.Content().try_as<FrameworkElement>();
    if (!child ||
        !(child = FindChildByClassName(child, L"SystemTray.SystemTrayFrame")) ||
        !(child = FindChildByName(child, L"SystemTrayFrameGrid")))
    {
        return false;
    }
    FrameworkElement frameGrid = child;

    bool anything = false;

    if (auto area = FindChildByName(frameGrid, L"NotificationAreaIcons"))
    {
        anything |= StyleNotificationAreaIcons(area, n);
    }

    if (auto button = FindChildByName(frameGrid, L"ControlCenterButton"))
    {
        anything |= StyleControlCenterButton(button, n);
    }

    static const wchar_t* const kStacks[] = { L"NotifyIconStack", L"MainStack", L"NonActivatableStack" };
    for (const wchar_t* stackName : kStacks)
    {
        if (auto container = FindChildByName(frameGrid, stackName))
        {
            anything |= StyleIconStack(stackName, container, n);
        }
    }

    return anything;
}

// OverflowRootGrid > ItemsControl > ItemsPresenter > WrapGrid > ContentPresenter* > NotifyIconView
void StyleOverflow(FrameworkElement const& overflowRootGrid, Numbers const& n)
{
    FrameworkElement child = overflowRootGrid;
    if (!(child = FindChildByClassName(child, L"Windows.UI.Xaml.Controls.ItemsControl")) ||
        !(child = FindChildByClassName(child, L"Windows.UI.Xaml.Controls.ItemsPresenter")) ||
        !(child = FindChildByClassName(child, L"Windows.UI.Xaml.Controls.WrapGrid")))
    {
        SP_LogDebug(L"The overflow popup has no WrapGrid");
        return;
    }

    auto wrapGrid = child.try_as<Controls::WrapGrid>();
    if (!wrapGrid)
    {
        return;
    }

    SP_LogDebug(L"Overflow: item size %d, %d per row", n.overflowIconWidth, n.overflowIconsPerRow);
    wrapGrid.ItemWidth(n.overflowIconWidth);
    wrapGrid.ItemHeight(n.overflowIconWidth);
    wrapGrid.MaximumRowsOrColumns(n.overflowIconsPerRow);

    EnumChildElements(wrapGrid, [&n](FrameworkElement const& presenter) {
        if (winrt::get_class_name(presenter) != L"Windows.UI.Xaml.Controls.ContentPresenter")
        {
            return false;
        }
        if (auto iconView = FindChildByClassName(presenter, L"SystemTray.NotifyIconView"))
        {
            StyleOverflowIcon(iconView, n.overflowIconWidth);
        }
        return false;
    });
}

// ---------------------------------------------------------------------------------------------------------------
// The hooks (SystemTray.dll, or Taskbar.View.dll on builds before 2604)
// ---------------------------------------------------------------------------------------------------------------

// A freshly built icon has no parent yet, so what it is and where it sits is only known once it is loaded.
void OnIconViewLoaded(IInspectable const& sender)
{
    if (!g_active.load(std::memory_order_relaxed))
    {
        return;
    }

    auto iconView = sender.try_as<FrameworkElement>();
    if (!iconView)
    {
        return;
    }

    const Numbers n = CurrentNumbers();
    auto className = winrt::get_class_name(iconView);

    if (className == L"SystemTray.NotifyIconView")
    {
        if (HasAncestorOfClass(iconView, L"SystemTray.NotificationAreaOverflow"))
        {
            StyleOverflowIcon(iconView, n.overflowIconWidth);

            // The popup's root grid, when InitializeIfNeeded saw it before it was loaded: the first grid above
            // the icon that holds the ItemsControl (root > ItemsControl > ItemsPresenter > WrapGrid > icons).
            if (!RememberedOverflowRootGrid())
            {
                DependencyObject current = iconView;
                for (int depth = 0; depth < 16 && (current = VisualTreeHelper::GetParent(current)); ++depth)
                {
                    auto candidate = current.try_as<FrameworkElement>();
                    if (candidate && winrt::get_class_name(candidate) == L"Windows.UI.Xaml.Controls.Grid" &&
                        FindChildByClassName(candidate, L"Windows.UI.Xaml.Controls.ItemsControl"))
                    {
                        SP_LogDebug(L"Overflow root grid found above its first icon");
                        RememberOverflowRootGrid(candidate);
                        StyleOverflow(candidate, n);
                        break;
                    }
                }
            }
        }
        else
        {
            StyleTrayIcon(iconView, n.iconWidth);
            if (n.rows > 1)
            {
                LayOutTrayIconsGridOfIcon(iconView, n);
            }
        }
    }
    else if (className == L"SystemTray.IconView")
    {
        if (iconView.Name() == L"SystemTrayIcon")
        {
            if (HasAncestorNamed(iconView, L"ControlCenterButton"))
            {
                StyleControlCenterIcon(iconView, n.iconWidth);
            }
            else if (HasAncestorNamed(iconView, L"MainStack") || HasAncestorNamed(iconView, L"NonActivatableStack"))
            {
                StyleTrayIcon(iconView, n.iconWidth);
            }
        }
    }
    else if (className == L"SystemTray.ChevronIconView")
    {
        if (HasAncestorNamed(iconView, L"NotifyIconStack"))
        {
            StyleTrayIcon(iconView, n.iconWidth);
        }
    }
}

using IconView_IconView_t = void*(WINAPI*)(void* pThis);
IconView_IconView_t g_origIconViewCtor = nullptr;

void* WINAPI IconView_IconView_Hook(void* pThis)
{
    void* ret = g_origIconViewCtor(pThis);

    if (!g_active.load(std::memory_order_relaxed) || !pThis)
    {
        return ret;
    }

    try
    {
        // The implementation object's second pointer is the inner base object the view composes; asking it for
        // IFrameworkElement yields the element the view is, under its own class name.
        auto* inner = ((::IUnknown**)pThis)[1];
        if (!inner)
        {
            return ret;
        }

        FrameworkElement iconView{ nullptr };
        inner->QueryInterface(winrt::guid_of<FrameworkElement>(), winrt::put_abi(iconView));
        if (!iconView)
        {
            return ret;
        }

        // The handler runs once, then takes itself off the element, as the original's revoker did.
        auto token = std::make_shared<winrt::event_token>();
        *token = iconView.Loaded([token](IInspectable const& sender, RoutedEventArgs const&) {
            // XAML calls this; nothing may escape back into it.
            try
            {
                if (auto element = sender.try_as<FrameworkElement>())
                {
                    element.Loaded(*token);
                }
                OnIconViewLoaded(sender);
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
    }
    catch (...)
    {
        SP_LogError(L"A new tray icon could not be watched");
    }

    return ret;
}

using OverflowXamlIslandManager_InitializeIfNeeded_t = void(WINAPI*)(void* pThis);
OverflowXamlIslandManager_InitializeIfNeeded_t g_origOverflowInitializeIfNeeded = nullptr;

void WINAPI OverflowXamlIslandManager_InitializeIfNeeded_Hook(void* pThis)
{
    g_origOverflowInitializeIfNeeded(pThis);

    if (!g_active.load(std::memory_order_relaxed) || !pThis)
    {
        return;
    }

    try
    {
        if (RememberedOverflowRootGrid())
        {
            return;     // already known and styled
        }

        // The manager keeps the island's root grid in its sixth pointer.
        auto* unknown = ((::IUnknown**)pThis)[5];
        if (!unknown)
        {
            SP_LogDebug(L"The overflow island has no root grid yet");
            return;
        }

        FrameworkElement grid{ nullptr };
        unknown->QueryInterface(winrt::guid_of<Controls::Grid>(), winrt::put_abi(grid));
        if (!grid)
        {
            SP_LogDebug(L"The overflow island's root is not a Grid");
            return;
        }

        if (!grid.IsLoaded())
        {
            // The first call builds the island and the grid is not in a tree yet. Subscribing to its Loaded
            // event here took explorer down on build 26200 (access violation inside Windows.UI.Xaml.dll), so,
            // like the original, nothing is done now: the popup's first icon to load finds the grid through
            // its ancestors (OnIconViewLoaded), and the next opening lands here with the grid loaded.
            SP_LogDebug(L"The overflow root grid is not loaded yet; it is styled from its first icon");
            return;
        }

        RememberOverflowRootGrid(grid);
        StyleOverflow(grid, CurrentNumbers());
    }
    catch (winrt::hresult_error const& e)
    {
        SP_LogError(L"Styling the overflow popup failed: 0x%08X", (unsigned)e.code());
    }
    catch (...)
    {
        SP_LogError(L"Styling the overflow popup failed");
    }
}

using StackViewModel_UpdateIconIndexes_t = void(WINAPI*)(void* pThis);
StackViewModel_UpdateIconIndexes_t g_origUpdateIconIndexes = nullptr;

void WINAPI StackViewModel_UpdateIconIndexes_Hook(void* pThis)
{
    g_origUpdateIconIndexes(pThis);

    if (!g_active.load(std::memory_order_relaxed))
    {
        return;
    }

    try
    {
        const Numbers n = CurrentNumbers();
        if (n.rows > 1)
        {
            if (auto stackPanel = RememberedTrayIconsStackPanel())
            {
                LayOutTrayIconsGrid(stackPanel, n);
            }
        }
    }
    catch (...)
    {
        SP_LogError(L"Laying the tray grid out again failed");
    }
}

// Build 26200 has none of the taskbar.dll symbols that lead from the tray window to its XAML (below), so an
// existing tray could not be reached at all. SystemTrayController::UpdateFrameSize runs on the tray's thread
// whenever the taskbar is told its logical DPI override changed (a WM_SETTINGCHANGE the mod can send), and the
// controller's View() hands out the SystemTrayFrame: the element is remembered from there, and its XamlRoot is
// the tray's.
using Controller_UpdateFrameSize_t = void(WINAPI*)(void* pThis);
using Controller_View_t = void*(WINAPI*)(void* pThis, void** result);
Controller_UpdateFrameSize_t g_origControllerUpdateFrameSize = nullptr;
Controller_View_t            g_pfnControllerView = nullptr;
std::mutex                        g_capturedLock;
winrt::weak_ref<FrameworkElement> g_capturedTrayFrame;

void WINAPI SystemTrayController_UpdateFrameSize_Hook(void* pThis)
{
    g_origControllerUpdateFrameSize(pThis);

    if (!g_pfnControllerView)
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
            std::lock_guard<std::mutex> guard(g_capturedLock);
            g_capturedTrayFrame = winrt::make_weak(frame);
        }
    }
    catch (...)
    {
    }
}

FrameworkElement CapturedTrayFrame()
{
    std::lock_guard<std::mutex> guard(g_capturedLock);
    return g_capturedTrayFrame ? g_capturedTrayFrame.get() : nullptr;
}

// Makes the tray's controller run UpdateFrameSize (and so the capture above) on its own thread.
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

BOOL InstallTrayHooks(HMODULE module, const wchar_t* moduleName)
{
    static const wchar_t* const kControllerUpdateFrameSize[] = {
        LR"(private: void __cdecl winrt::SystemTray::implementation::SystemTrayController::UpdateFrameSize(void))",
    };
    static const wchar_t* const kControllerView[] = {
        LR"(public: struct winrt::Windows::UI::Xaml::FrameworkElement __cdecl winrt::SystemTray::implementation::SystemTrayController::View(void))",
    };
    static const wchar_t* const kIconViewCtor[] = {
        LR"(public: __cdecl winrt::SystemTray::implementation::IconView::IconView(void))",
    };
    static const wchar_t* const kOverflowInitializeIfNeeded[] = {
        LR"(private: void __cdecl winrt::SystemTray::OverflowXamlIslandManager::InitializeIfNeeded(void))",
    };
    static const wchar_t* const kUpdateIconIndexes[] = {
        LR"(private: void __cdecl winrt::SystemTray::implementation::StackViewModel::UpdateIconIndexes(void))",
    };

    SP_SymbolHook hooks[5] = {};

    hooks[3].symbols = kControllerUpdateFrameSize;
    hooks[3].symbolCount = ARRAYSIZE(kControllerUpdateFrameSize);
    hooks[3].pOriginal = (void**)&g_origControllerUpdateFrameSize;
    hooks[3].hookFunction = (void*)SystemTrayController_UpdateFrameSize_Hook;
    hooks[3].optional = TRUE;

    hooks[4].symbols = kControllerView;
    hooks[4].symbolCount = ARRAYSIZE(kControllerView);
    hooks[4].pOriginal = (void**)&g_pfnControllerView;
    hooks[4].optional = TRUE;

    hooks[0].symbols = kIconViewCtor;
    hooks[0].symbolCount = ARRAYSIZE(kIconViewCtor);
    hooks[0].pOriginal = (void**)&g_origIconViewCtor;
    hooks[0].hookFunction = (void*)IconView_IconView_Hook;
    hooks[0].optional = FALSE;

    hooks[1].symbols = kOverflowInitializeIfNeeded;
    hooks[1].symbolCount = ARRAYSIZE(kOverflowInitializeIfNeeded);
    hooks[1].pOriginal = (void**)&g_origOverflowInitializeIfNeeded;
    hooks[1].hookFunction = (void*)OverflowXamlIslandManager_InitializeIfNeeded_Hook;
    hooks[1].optional = TRUE;

    hooks[2].symbols = kUpdateIconIndexes;
    hooks[2].symbolCount = ARRAYSIZE(kUpdateIconIndexes);
    hooks[2].pOriginal = (void**)&g_origUpdateIconIndexes;
    hooks[2].hookFunction = (void*)StackViewModel_UpdateIconIndexes_Hook;
    hooks[2].optional = TRUE;

    if (!SP_HookSymbols(module, hooks, ARRAYSIZE(hooks)))
    {
        SP_LogError(L"The tray icon constructor was not found in %s; nothing can be styled", moduleName);
        return FALSE;
    }

    if (!g_origOverflowInitializeIfNeeded)
    {
        SP_Log(L"OverflowXamlIslandManager::InitializeIfNeeded not found; the overflow popup keeps its own layout");
    }
    if (!g_origUpdateIconIndexes)
    {
        SP_Log(L"StackViewModel::UpdateIconIndexes not found; a grid of rows is not re-laid out when icons come and go");
    }

    SP_Log(L"Tray icon hooks installed in %s (overflow popup: %s, grid refresh: %s)", moduleName,
           g_origOverflowInitializeIfNeeded ? L"yes" : L"no", g_origUpdateIconIndexes ? L"yes" : L"no");
    g_trayHooked.store(true, std::memory_order_release);
    return TRUE;
}

// ---------------------------------------------------------------------------------------------------------------
// From the taskbar window to its XAML
//
// The tray window keeps its task band (CTaskBand, in taskbar.dll) in the window bytes of its TaskbandHWND child;
// the band's ITaskListWndSite interface hands out a shared_ptr<TaskbarHost>, and the host holds the XAML frame
// element a few bytes in. That offset is read from TaskbarHost::FrameHeight, which starts by adding it to
// `this`. Every symbol is resolved only, none is hooked. Without them a settings change only reaches icons
// created afterwards, and that is logged.
// ---------------------------------------------------------------------------------------------------------------

using GetTaskbarHost_t = void*(WINAPI*)(void* pThis, void** result);
using RefCountDecref_t = void(WINAPI*)(void* pThis);

void*            g_CTaskBand_ITaskListWndSite_vftable = nullptr;
GetTaskbarHost_t g_CTaskBand_GetTaskbarHost = nullptr;
void*            g_TaskbarHost_FrameHeight = nullptr;
RefCountDecref_t g_RefCountBase_Decref = nullptr;

std::atomic<bool> g_taskbarDllReady{ false };

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
    static const wchar_t* const kTaskBandGetTaskbarHost[] = {
        LR"(public: virtual class std::shared_ptr<class TaskbarHost> __cdecl CTaskBand::GetTaskbarHost(void)const )",
    };
    static const wchar_t* const kTaskbarHostFrameHeight[] = {
        LR"(public: int __cdecl TaskbarHost::FrameHeight(void)const )",
    };
    static const wchar_t* const kRefCountBaseDecref[] = {
        LR"(public: void __cdecl std::_Ref_count_base::_Decref(void))",
    };

    SP_SymbolHook hooks[4] = {};
    hooks[0].symbols = kTaskBandVftable;
    hooks[0].symbolCount = ARRAYSIZE(kTaskBandVftable);
    hooks[0].pOriginal = &g_CTaskBand_ITaskListWndSite_vftable;
    hooks[1].symbols = kTaskBandGetTaskbarHost;
    hooks[1].symbolCount = ARRAYSIZE(kTaskBandGetTaskbarHost);
    hooks[1].pOriginal = (void**)&g_CTaskBand_GetTaskbarHost;
    hooks[2].symbols = kTaskbarHostFrameHeight;
    hooks[2].symbolCount = ARRAYSIZE(kTaskbarHostFrameHeight);
    hooks[2].pOriginal = &g_TaskbarHost_FrameHeight;
    hooks[3].symbols = kRefCountBaseDecref;
    hooks[3].symbolCount = ARRAYSIZE(kRefCountBaseDecref);
    hooks[3].pOriginal = (void**)&g_RefCountBase_Decref;
    for (auto& hook : hooks)
    {
        hook.hookFunction = nullptr;    // resolve only
        hook.optional = FALSE;
    }

    if (!SP_ResolveSymbols(module, hooks, ARRAYSIZE(hooks)))
    {
        SP_LogError(L"The taskbar.dll symbols were not all found; settings changes only reach new tray icons");
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

// The XAML frame of this process's primary taskbar. The notification area only exists on the primary one.
FrameworkElement PrimaryTaskbarFrame()
{
    HWND hTaskbarWnd = nullptr;
    EnumWindows(
        [](HWND hWnd, LPARAM lParam) -> BOOL {
            DWORD processId = 0;
            wchar_t className[32];
            if (GetWindowThreadProcessId(hWnd, &processId) && processId == GetCurrentProcessId() &&
                GetClassNameW(hWnd, className, ARRAYSIZE(className)) && _wcsicmp(className, L"Shell_TrayWnd") == 0)
            {
                *(HWND*)lParam = hWnd;
                return FALSE;
            }
            return TRUE;
        },
        (LPARAM)&hTaskbarWnd);
    if (!hTaskbarWnd)
    {
        SP_LogDebug(L"This process has no taskbar window");
        return nullptr;
    }

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

// Runs `work` on the dispatcher's thread and waits for it, briefly: BeforeUninit needs the restore done before
// the hooks go, and a UI thread that does not answer must not hold the engine.
template <typename F>
void RunOnDispatcherAndWait(CoreDispatcher const& dispatcher, const wchar_t* what, F work)
{
    auto waiter = std::make_shared<Waiter>();
    if (!waiter->hEvent)
    {
        return;
    }

    g_pendingDispatches.fetch_add(1, std::memory_order_relaxed);
    try
    {
        dispatcher.TryRunAsync(CoreDispatcherPriority::High, [work, waiter, what]() {
            try
            {
                work();
            }
            catch (winrt::hresult_error const& e)
            {
                SP_LogError(L"%s failed: 0x%08X", what, (unsigned)e.code());
            }
            catch (...)
            {
                SP_LogError(L"%s failed", what);
            }
            g_pendingDispatches.fetch_sub(1, std::memory_order_relaxed);
            SetEvent(waiter->hEvent);
        });
    }
    catch (...)
    {
        g_pendingDispatches.fetch_sub(1, std::memory_order_relaxed);
        SP_LogError(L"The dispatcher refused the callback for %s", what);
        return;
    }

    WaitForSingleObject(waiter->hEvent, 3000);
}

// Engine thread or the module-wait thread. Styles the tray and the overflow popup as they are right now.
std::mutex g_applyLock;

void ApplySettings()
{
    std::lock_guard<std::mutex> guard(g_applyLock);

    if (!g_trayHooked.load(std::memory_order_acquire))
    {
        return;
    }

    const Numbers n = CurrentNumbers();
    SP_Log(L"Applying: icon width %d, rows %d, arrangement %d, overflow %d px and %d per row",
           n.iconWidth, n.rows, n.arrangement, n.overflowIconWidth, n.overflowIconsPerRow);

    FrameworkElement frame{ nullptr };
    if (g_taskbarDllReady.load(std::memory_order_relaxed))
    {
        try
        {
            frame = PrimaryTaskbarFrame();
        }
        catch (...)
        {
            SP_LogError(L"Reading the taskbar's XAML frame failed");
        }
    }
    if (!frame && g_origControllerUpdateFrameSize && g_pfnControllerView)
    {
        // Build 26200: the tray frame remembered from the controller, asked for if not seen yet.
        frame = CapturedTrayFrame();
        if (!frame)
        {
            // The controller answers the setting change a moment after the message returns.
            NudgeTrayFrame();
            for (int i = 0; i < 40 && !frame; ++i)
            {
                Sleep(50);
                frame = CapturedTrayFrame();
            }
        }
    }

    if (frame)
    {
        RunOnDispatcherAndWait(frame.Dispatcher(), L"Styling the tray", [frame, n]() {
            if (!StyleTray(frame.XamlRoot(), n))
            {
                SP_LogDebug(L"The system tray was not found under the taskbar's XamlRoot");
            }
        });
    }
    else
    {
        SP_LogDebug(L"No XAML frame behind the taskbar window; icons created from now on are styled");
    }

    // The overflow popup has its own island; the grid is only known once the popup has been opened.
    if (auto overflow = RememberedOverflowRootGrid())
    {
        RunOnDispatcherAndWait(overflow.Dispatcher(), L"Styling the overflow popup", [overflow, n]() {
            StyleOverflow(overflow, n);
        });
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Waiting for the tray module
// ---------------------------------------------------------------------------------------------------------------

// The major part of a module's file version, read from its version resource. version.dll is not linked, so the
// VS_FIXEDFILEINFO block is found by its signature. 0 when unknown.
WORD GetModuleVersionMajor(HMODULE hModule)
{
    HRSRC hRes = FindResourceW(hModule, MAKEINTRESOURCEW(VS_VERSION_INFO), RT_VERSION);
    if (!hRes)
    {
        return 0;
    }
    HGLOBAL hGlobal = LoadResource(hModule, hRes);
    const BYTE* data = hGlobal ? (const BYTE*)LockResource(hGlobal) : nullptr;
    DWORD size = SizeofResource(hModule, hRes);
    if (!data || size < sizeof(VS_FIXEDFILEINFO))
    {
        return 0;
    }

    for (DWORD at = 0; at + sizeof(VS_FIXEDFILEINFO) <= size && at < 0x80; at += sizeof(DWORD))
    {
        const VS_FIXEDFILEINFO* info = (const VS_FIXEDFILEINFO*)(data + at);
        if (info->dwSignature == 0xFEEF04BD)
        {
            return HIWORD(info->dwFileVersionMS);
        }
    }
    return 0;
}

void OnSystemTrayLoaded(HMODULE hModule, void*)
{
    if (!g_active.load(std::memory_order_relaxed))
    {
        return;
    }
    if (InstallTrayHooks(hModule, L"SystemTray.dll"))
    {
        ApplySettings();
    }
}

void OnTaskbarViewLoaded(HMODULE hTaskbarView, void*)
{
    if (!g_active.load(std::memory_order_relaxed))
    {
        return;
    }

    if (HMODULE hSystemTray = GetModuleHandleW(L"SystemTray.dll"))
    {
        if (InstallTrayHooks(hSystemTray, L"SystemTray.dll"))
        {
            ApplySettings();
        }
        return;
    }

    // Builds before Taskbar.View.dll 2604 keep the tray's types inside it.
    const WORD major = GetModuleVersionMajor(hTaskbarView);
    if (major && major < 2604)
    {
        SP_Log(L"Taskbar.View.dll %u holds the tray itself", major);
        if (InstallTrayHooks(hTaskbarView, L"Taskbar.View.dll"))
        {
            ApplySettings();
        }
        return;
    }

    SP_Log(L"Taskbar.View.dll %u is loaded; waiting for SystemTray.dll", major);
    if (!SP_WaitForModule(L"SystemTray.dll", 60000, OnSystemTrayLoaded, nullptr))
    {
        SP_LogError(L"Could not wait for SystemTray.dll");
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------------------------

int ClampSetting(int value, int low, int high)
{
    return value < low ? low : (value > high ? high : value);
}

void LoadSettings()
{
    g_settingIconWidth.store(ClampSetting(SP_GetIntSetting(L"NotificationIconWidth", kStockIconWidth), 1, 256),
                             std::memory_order_relaxed);
    g_settingRows.store(ClampSetting(SP_GetIntSetting(L"NotificationIconRows", kStockRows), 1, 8),
                        std::memory_order_relaxed);
    g_settingArrangement.store(ClampSetting(SP_GetIntSetting(L"GridArrangement", kRowFirstLeftToRight),
                                            kRowFirstLeftToRight, kColumnFirstBottomToTopRightToLeft),
                               std::memory_order_relaxed);
    g_settingOverflowIconWidth.store(
        ClampSetting(SP_GetIntSetting(L"OverflowIconWidth", kStockOverflowIconWidth), 1, 256),
        std::memory_order_relaxed);
    g_settingOverflowIconsPerRow.store(
        ClampSetting(SP_GetIntSetting(L"OverflowIconsPerRow", kStockOverflowIconsPerRow), 1, 64),
        std::memory_order_relaxed);

    SP_Log(L"Settings: icon width %d, rows %d, arrangement %d, overflow icon width %d, %d per row",
           g_settingIconWidth.load(), g_settingRows.load(), g_settingArrangement.load(),
           g_settingOverflowIconWidth.load(), g_settingOverflowIconsPerRow.load());
}

BOOL Init()
{
    g_unloading.store(false, std::memory_order_relaxed);
    g_trayHooked.store(false, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> guard(g_capturedLock);
        g_capturedTrayFrame = nullptr;
    }
    g_origIconViewCtor = nullptr;
    g_origOverflowInitializeIfNeeded = nullptr;
    g_origUpdateIconIndexes = nullptr;
    ForgetElements();

    LoadSettings();

    // Not fatal: without taskbar.dll a settings change waits for icons to be created anew.
    g_taskbarDllReady.store(ResolveTaskbarDllSymbols() ? true : false, std::memory_order_relaxed);

    g_active.store(true, std::memory_order_release);

    // The taskbar comes up after the engine on a cold sign-in; the tray hooks go in when it is there.
    if (!SP_WaitForModule(L"Taskbar.View.dll", 60000, OnTaskbarViewLoaded, nullptr))
    {
        SP_LogError(L"Could not wait for Taskbar.View.dll");
        g_active.store(false, std::memory_order_relaxed);
        return FALSE;
    }

    SP_Log(L"Waiting for the taskbar");
    return TRUE;
}

void AfterInit()
{
    // Already hooked on a warm start: the module callback has applied the settings. On a build where the
    // callback ran before the taskbar's tree existed this is the second chance.
    try
    {
        ApplySettings();
    }
    catch (...)
    {
        SP_LogError(L"The first styling pass failed");
    }
}

void SettingsChanged()
{
    LoadSettings();
    try
    {
        ApplySettings();
    }
    catch (...)
    {
        SP_LogError(L"Re-applying the tray style failed");
    }
}

void BeforeUninit()
{
    // Still hooked. Everything on screen gets the stock numbers back through the same code that styled it.
    g_unloading.store(true, std::memory_order_relaxed);
    try
    {
        ApplySettings();
    }
    catch (...)
    {
        SP_LogError(L"Restoring the tray style failed");
    }
}

void Uninit()
{
    g_active.store(false, std::memory_order_release);

    // Give the callbacks queued on a XAML dispatcher a moment to finish; each one is short.
    for (int i = 0; i < 100 && g_pendingDispatches.load(std::memory_order_relaxed) > 0; ++i)
    {
        Sleep(20);
    }

    ForgetElements();
}

}   // namespace

SP_MOD_DEFINE(g_modTaskbarNotificationIconSpacing) =
{
    /* id             */ SP_MOD_ID,
    /* name           */ L"Change the spacing of the tray icons and the overflow popup",
    /* basedOn        */ "taskbar-notification-icon-spacing",
    /* originalAuthor */ "m417z",
    /* targets        */ SP_TARGET_EXPLORER,
    /* minOsBuild     */ 22000,
    /* flags          */ 0,
    /* Init           */ Init,
    /* AfterInit      */ AfterInit,
    /* SettingsChanged*/ SettingsChanged,
    /* BeforeUninit   */ BeforeUninit,
    /* Uninit         */ Uninit,
};
