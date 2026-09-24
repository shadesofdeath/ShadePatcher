#pragma once
//
// taskbar_styler_themes.h - the seven themes shipped with taskbar-styler.
//
// The rule strings below are data, not code: a theme is exactly its set of targets and styles, so they are kept
// word for word as published in the Windows 11 taskbar styling guide
// (https://github.com/ramensoftware/windows-11-taskbar-styling-guide) and bundled with the Windhawk mod
// "windows-11-taskbar-styler" by m417z. The engine that reads them (taskbar_styler.cpp) is written for
// ShadePatcher.
//
// Theme credits (the author named on each theme's page in the guide):
//   TranslucentTaskbar   Undisputed00x
//   DockLike             Amber (AmberWat)
//   SimplyTransparent    Osprey00
//   Squircle             AsvnDG
//   Matter               ZoraizLajwer
//   Surface              Jimmy (JimmyLlancaMelo)
//   Luminosity           mendes.image (Dock, Classic and Compact variants)
//
// Adjusted for ShadePatcher: themes written for a taskbar of their own height (Dock 58, Compact 30) use
// $TaskbarHeight, the real height (taskbar_styler.cpp), and Surface keeps a 4 DIP gap under the bar
// instead of 10, so a two-line clock and the item backgrounds fit the default 48 DIPs.
// See each theme's README under Themes/<name>/ in the guide for the author's own notes and screenshots.
//
// Each theme is a function-local static so nothing is constructed while the DLL is being loaded.
//
#include <Windows.h>
#include <vector>

namespace stylerthemes {

struct ThemeTargetStyles
{
    PCWSTR target;
    std::vector<PCWSTR> styles;
};

struct Theme
{
    std::vector<ThemeTargetStyles> targetStyles;
    std::vector<PCWSTR> styleConstants;
};

inline const Theme& Theme_TranslucentTaskbar()
{
    static const Theme theme = {{
        ThemeTargetStyles{L"Taskbar.TaskbarFrame > Grid#RootGrid > Taskbar.TaskbarBackground > Grid > Rectangle#BackgroundFill", {
            L"Fill:=$CommonBgBrush"}},
        ThemeTargetStyles{L"Taskbar.TaskbarBackground#HoverFlyoutBackgroundControl > Grid > Rectangle#BackgroundFill", {
            L"Fill:=$CommonBgBrush"}},
        ThemeTargetStyles{L"Rectangle#BackgroundStroke", {
            L"Visibility=Collapsed"}},
        ThemeTargetStyles{L"MenuFlyoutPresenter > Border", {
            L"Background:=$CommonBgBrush",
            L"BorderThickness=0,0,0,0",
            L"CornerRadius=14",
            L"Padding=3,4,3,4"}},
        ThemeTargetStyles{L"Border#OverflowFlyoutBackgroundBorder", {
            L"Background:=$CommonBgBrush",
            L"BorderThickness=0,0,0,0",
            L"CornerRadius=15",
            L"Margin=-2,-2,-2,-2"}},
        ThemeTargetStyles{L"Grid#ConfirmatorMainGrid", {
            L"Background:=$CommonBgBrush",
            L"BorderThickness=0"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.TextInput.Common.InputSwitcher > ContentControl > ContentPresenter > Grid", {
            L"Background:=<WindhawkBlur BlurAmount=\"25\" TintColor=\"#25323232\"/>",
            L"BorderThickness=0"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.TextInput.Common.InputSwitcher > ContentControl > ContentPresenter > Grid > Grid", {
            L"Background:=<SolidColorBrush Color=\"Transparent\"/>"}},
    }, {
        L"CommonBgBrush=<WindhawkBlur BlurAmount=\"18\" TintColor=\"#25323232\"/>",
    }};
    return theme;
}

inline const Theme& Theme_DockLike()
{
    static const Theme theme = {{
        ThemeTargetStyles{L"Taskbar.TaskbarFrame > Grid#RootGrid@DockingStates", {
            L"Tag=horizontal",
            L"Tag@DockedLeft=vertical",
            L"Tag@DockedRight=vertical",
            L"Tag=>taskbarDock"}},
        ThemeTargetStyles{L"Taskbar.TaskbarFrame", {
            L"Width={{taskbarDock==`vertical`?skip():`Auto`}}",
            L"HorizontalAlignment=Center",
            L"Margin=250,0,250,0"}},
        ThemeTargetStyles{L"Taskbar.TaskbarFrame > Grid#RootGrid", {
            L"Background:=<AcrylicBrush TintColor=\"{ThemeResource SystemChromeAltHighColor}\" TintOpacity=\"0.8\" FallbackColor=\"{ThemeResource SystemChromeLowColor}\" />",
            L"Padding=6,0,6,0",
            L"CornerRadius=8,8,0,0",
            L"BorderBrush:=<SolidColorBrush Color=\"{ThemeResource SurfaceStrokeColorDefault}\" />"}},
        ThemeTargetStyles{L"Taskbar.TaskbarFrame > Grid#RootGrid > Taskbar.TaskbarBackground > Grid > Rectangle#BackgroundFill", {
            L"Visibility=Collapsed"}},
        ThemeTargetStyles{L"Rectangle#BackgroundStroke", {
            L"Visibility=Collapsed"}},
        ThemeTargetStyles{L"Taskbar.AugmentedEntryPointButton#AugmentedEntryPointButton > Taskbar.TaskListButtonPanel#ExperienceToggleButtonRootPanel", {
            L"Margin=0"}},
        ThemeTargetStyles{L"StackPanel#SystemTrayFrameGrid, Grid#SystemTrayFrameGrid", {
            L"Background:=<AcrylicBrush TintColor=\"{ThemeResource SystemChromeAltHighColor}\" TintOpacity=\"0.8\" FallbackColor=\"{ThemeResource SystemChromeLowColor}\" />",
            L"Margin=-4,-8,-4,-8",
            L"CornerRadius=10",
            L"BorderThickness=12,12,12,12",
            L"BackgroundSizing=InnerBorderEdge"}},
        ThemeTargetStyles{L"SystemTray.ChevronIconView", {
            L"Padding=0"}},
        ThemeTargetStyles{L"SystemTray.NotifyIconView#NotifyItemIcon", {
            L"Padding=0"}},
        ThemeTargetStyles{L"SystemTray.OmniButton", {
            L"Padding=0"}},
        ThemeTargetStyles{L"SystemTray.CopilotIcon", {
            L"Padding=0"}},
        ThemeTargetStyles{L"SystemTray.OmniButton#NotificationCenterButton > Grid > ContentPresenter > ItemsPresenter > StackPanel > ContentPresenter > SystemTray.IconView#SystemTrayIcon > Grid", {
            L"Padding=4,0,4,0"}},
        ThemeTargetStyles{L"SystemTray.IconView#SystemTrayIcon > Grid#ContainerGrid > ContentPresenter#ContentPresenter > Grid#ContentGrid > SystemTray.TextIconContent > Grid#ContainerGrid", {
            L"Padding=0"}},
        ThemeTargetStyles{L"SystemTray.StackListView#IconStack > ItemsPresenter > StackPanel > ContentPresenter > SystemTray.IconView#SystemTrayIcon", {
            L"Padding=0"}},
        ThemeTargetStyles{L"SystemTray.Stack#ShowDesktopStack", {
            L"Margin=0,-4,-12,-4"}},
        ThemeTargetStyles{L"Taskbar.Gripper#GripperControl", {
            L"Width=Auto",
            L"MinWidth=24"}},
    }};
    return theme;
}

inline const Theme& Theme_SimplyTransparent()
{
    static const Theme theme = {{
        ThemeTargetStyles{L"Taskbar.TaskbarFrame > Grid#RootGrid > Taskbar.TaskbarBackground > Grid > Rectangle#BackgroundFill", {
            L"Fill=Transparent"}},
        ThemeTargetStyles{L"Rectangle#BackgroundStroke", {
            L"Fill=Transparent"}},
    }};
    return theme;
}

inline const Theme& Theme_Squircle()
{
    static const Theme theme = {{
        ThemeTargetStyles{L"Taskbar.TaskbarFrame > Grid#RootGrid > Taskbar.TaskbarBackground > Grid > Rectangle#BackgroundFill", {
            L"Fill=Transparent"}},
        ThemeTargetStyles{L"Taskbar.TaskbarBackground#HoverFlyoutBackgroundControl > Grid > Rectangle#BackgroundFill", {
            L"Fill=#CC222222"}},
        ThemeTargetStyles{L"Taskbar.TaskListButtonPanel@CommonStates > Grid > Border#BackgroundElement, Taskbar.TaskListButtonPanel@CommonStates > Border#BackgroundElement, SearchUx.SearchUI.SearchButtonRootGrid#SearchBoxButtonRootPanel@CommonStates > Grid > Border#BackgroundElement, SearchUx.SearchUI.SearchButtonRootGrid#SearchBoxButtonRootPanel@CommonStates > Border#BackgroundElement", {
            L"CornerRadius=5",
            L"Background:=<AcrylicBrush TintColor=\"Black\" TintOpacity=\"0.8\" FallbackColor=\"#BB222222\" />",
            L"Background@InactivePointerOver:=<AcrylicBrush TintColor=\"Black\" TintOpacity=\"0.8\" FallbackColor=\"#CC222222\" />",
            L"Background@ActivePointerOver:=<AcrylicBrush TintColor=\"Black\" TintOpacity=\"0.9\" FallbackColor=\"#CC222222\" />",
            L"Background@ActiveNormal:=<AcrylicBrush TintColor=\"Black\" TintOpacity=\"0.8\" FallbackColor=\"#CC222222\" />",
            L"Background@InactiveNormal:=<AcrylicBrush TintColor=\"Black\" TintOpacity=\"0.7\" FallbackColor=\"#BB222222\" />",
            L"Background@InactivePressed:=<AcrylicBrush TintColor=\"Black\" TintOpacity=\"0.8\" FallbackColor=\"#CC222222\" />",
            L"Background@ActivePressed:=<AcrylicBrush TintColor=\"Black\" TintOpacity=\"0.8\" FallbackColor=\"#CC222222\" />"}},
        ThemeTargetStyles{L"StackPanel#SystemTrayFrameGrid, Grid#SystemTrayFrameGrid", {
            L"Background:=<AcrylicBrush TintColor=\"Black\" TintOpacity=\"0.8\" FallbackColor=\"#BB222222\"/>",
            L"CornerRadius=5",
            L"Margin=0,5,14,5",
            L"Padding=10,0,0,0"}},
        ThemeTargetStyles{L"Rectangle#RunningIndicator", {
            L"Fill=Transparent",
            L"RadiusX=5",
            L"RadiusY=5",
            L"Height=38",
            L"Width=40"}},
        ThemeTargetStyles{L"Grid#IconPanel > TextBlock#LabelControl, Taskbar.TaskListLabeledButtonPanel > TextBlock#LabelControl", {
            L"Margin=4,0,0,0",
            L"Foreground=White"}},
        ThemeTargetStyles{L"Taskbar.SearchBoxButton", {
            L"Foreground=White",
            L"Margin=-11,0,0,0"}},
        ThemeTargetStyles{L"TextBlock#SearchBoxTextBlock", {
            L"FontSize=12",
            L"Foreground=White"}},
        ThemeTargetStyles{L"Rectangle#BackgroundStroke", {
            L"Fill=Transparent"}},
        ThemeTargetStyles{L"Grid", {
            L"RequestedTheme=2"}},
        ThemeTargetStyles{L"Taskbar.TaskListButton#TaskListButton[AutomationProperties.Name=Copilot] > Grid#IconPanel > Border#BackgroundElement, Taskbar.TaskListButton#TaskListButton[AutomationProperties.Name=Copilot] > Taskbar.TaskListLabeledButtonPanel#IconPanel > Border#BackgroundElement", {
            L"Background:=<AcrylicBrush TintColor=\"Red\" TintOpacity=\"0.8\" />"}},
        ThemeTargetStyles{L"Border#BackgroundBorder", {
            L"Margin=0,3,0,3",
            L"CornerRadius=5"}},
        ThemeTargetStyles{L"Taskbar.AugmentedEntryPointButton#AugmentedEntryPointButton > Taskbar.TaskListButtonPanel#ExperienceToggleButtonRootPanel > Grid > Border#BackgroundElement@CommonStates, Taskbar.AugmentedEntryPointButton#AugmentedEntryPointButton > Taskbar.TaskListButtonPanel#ExperienceToggleButtonRootPanel > Border#BackgroundElement@CommonStates", {
            L"Background@InactivePointerOver:=<AcrylicBrush TintColor=\"Black\" TintOpacity=\"0\" />",
            L"Background:=<AcrylicBrush TintColor=\"Black\" TintOpacity=\"0.8\" FallbackColor=\"#BB222222\" />"}},
        ThemeTargetStyles{L"Border#MultiWindowElement", {
            L"Background:=<AcrylicBrush TintColor=\"Black\" TintOpacity=\"0.8\" FallbackColor=\"#CC222222\" />"}},
        ThemeTargetStyles{L"TextBlock#TimeInnerTextBlock", {
            L"Foreground=White"}},
        ThemeTargetStyles{L"TextBlock#DateInnerTextBlock", {
            L"Foreground=White"}},
        ThemeTargetStyles{L"SystemTray.TextIconContent > Grid > SystemTray.AdaptiveTextBlock#Base > TextBlock", {
            L"Foreground=White"}},
        ThemeTargetStyles{L"Border#BackgroundElement", {
            L"BorderThickness=0"}},
        ThemeTargetStyles{L"Taskbar.AugmentedEntryPointButton#AugmentedEntryPointButton", {
            L"Margin=-11,0,0,0"}},
        ThemeTargetStyles{L"Taskbar.ExperienceToggleButton#LaunchListButton[AutomationProperties.Name=Task View]", {
            L"Margin=-12,0,0,0"}},
        ThemeTargetStyles{L"Grid#IconPanel@RunningIndicatorStates > Border, Taskbar.TaskListLabeledButtonPanel@RunningIndicatorStates > Border", {
            L"Background@ActiveRunningIndicator:=<AcrylicBrush TintOpacity=\"0.8\" TintColor=\"Black\" />",
            L"Background@InactiveRunningIndicator:=<AcrylicBrush TintOpacity=\"0.8\" TintColor=\"Black\" />",
            L"Background@InactiveRunningIndicatorPointerOver:=<AcrylicBrush TintOpacity=\"0.8\" TintColor=\"Black\" />"}},
        ThemeTargetStyles{L"Grid#IconPanel@CommonStates > Border#BackgroundElement, Taskbar.TaskListLabeledButtonPanel@CommonStates > Border#BackgroundElement", {
            L"Background@InactivePointerOver:=<AcrylicBrush TintOpacity=\"0.8\" TintColor=\"Black\" FallbackColor=\"#DD222222\"/>",
            L"Background@ActivePointerOver:=<AcrylicBrush TintOpacity=\"0.8\" TintColor=\"Black\" FallbackColor=\"#EE222222\"/>",
            L"Background@InactiveNormal:=<AcrylicBrush TintOpacity=\"0.2\" TintColor=\"Black\" FallbackColor=\"#BB222222\"/>",
            L"Background@ActiveNormal:=<AcrylicBrush TintOpacity=\"0.8\" TintColor=\"Black\" FallbackColor=\"#CC222222\"/>",
            L"Background@ActivePressed:=<AcrylicBrush TintOpacity=\"0.8\" TintColor=\"#333333\" FallbackColor=\"#BB333333\" />",
            L"Background@InactivePressed:=<AcrylicBrush TintOpacity=\"0.8\" TintColor=\"#333333\" FallbackColor=\"#BB333333\" />",
            L"CornerRadius=5",
            L"Margin=1"}},
    }};
    return theme;
}

inline const Theme& Theme_Matter()
{
    static const Theme theme = {{
        ThemeTargetStyles{L"Taskbar.TaskbarFrame > Grid#RootGrid > Taskbar.TaskbarBackground > Grid > Rectangle#BackgroundFill", {
            L"Fill := $transparent"}},
        ThemeTargetStyles{L"Rectangle#BackgroundStroke", {
            L"Fill := $transparent"}},
        ThemeTargetStyles{L"Taskbar.TaskbarBackground#HoverFlyoutBackgroundControl", {
            L"Fill:=$base",
            L"CornerRadius = $mainRadius"}},
        ThemeTargetStyles{L"Taskbar.AugmentedEntryPointButton#AugmentedEntryPointButton", {
            L"Margin=-1,1,1,1"}},
        ThemeTargetStyles{L"Taskbar.TaskListButtonPanel@CommonStates > Grid > Border#BackgroundElement, Taskbar.TaskListButtonPanel@CommonStates > Border#BackgroundElement", {
            L"CornerRadius = $mainRadius",
            L"Background :=$base",
            L"Background@InactivePointerOver :=$overlay2",
            L"Background@ActivePointerOver:=$overlay",
            L"Background@ActiveNormal :=$active"}},
        ThemeTargetStyles{L"Taskbar.ExperienceToggleButton#LaunchListButton[AutomationProperties.Name=Task View]", {
            L"Margin=0,0,2,0"}},
        ThemeTargetStyles{L"Taskbar.TaskListButton#TaskListButton[AutomationProperties.Name=Copilot] > Grid#IconPanel > Border#BackgroundElement, Taskbar.TaskListButton#TaskListButton[AutomationProperties.Name=Copilot] > Taskbar.TaskListLabeledButtonPanel#IconPanel > Border#BackgroundElement", {
            L"Visibility = 1"}},
        ThemeTargetStyles{L"Taskbar.SearchBoxButton", {
            L"Margin=0,0,2,0"}},
        ThemeTargetStyles{L"SearchUx.SearchUI.SearchButtonControl", {
            L"Margin=0,0,2,0"}},
        ThemeTargetStyles{L"Border#BackgroundElement", {
            L"BorderThickness=0"}},
        ThemeTargetStyles{L"Grid#IconPanel@CommonStates > Border#BackgroundElement, Taskbar.TaskListLabeledButtonPanel@CommonStates > Border#BackgroundElement", {
            L"Background@InactiveNormal :=$base",
            L"Background@ActiveNormal :=$active",
            L"Background@InactivePointerOver :=$overlay2",
            L"Background@ActivePointerOver:=$overlay",
            L"CornerRadius = $mainRadius",
            L"Margin = 1,0,1,0",
            L"Background@MultiWindowNormal:=$base",
            L"Background@MultiWindowPointerOver:=$overlay2",
            L"Background@MultiWindowActive:=$active",
            L"Background@MultiWindowPressed:=$overlay"}},
        ThemeTargetStyles{L"Border#MultiWindowElement", {
            L"CornerRadius = $mainRadius",
            L"Padding = 7,0,8,0",
            L"Background :=$accentColor"}},
        ThemeTargetStyles{L"Grid#IconPanel > TextBlock#LabelControl, Taskbar.TaskListLabeledButtonPanel > TextBlock#LabelControl", {
            L"Margin=0,0,2,0"}},
        ThemeTargetStyles{L"Grid#IconPanel@RunningIndicatorStates > Rectangle#RunningIndicator, Taskbar.TaskListLabeledButtonPanel@RunningIndicatorStates > Rectangle#RunningIndicator", {
            L"Fill := $inverseBW",
            L"RadiusX=1.5",
            L"RadiusY=1.5",
            L"Height=4",
            L"Width=12",
            L"Fill@ActiveRunningIndicator :=$accentColor",
            L"Width@ActiveRunningIndicator=21"}},
        ThemeTargetStyles{L"StackPanel#SystemTrayFrameGrid, Grid#SystemTrayFrameGrid", {
            L"Background:=$base",
            L"CornerRadius = $mainRadius",
            L"Margin=0,5,12,5",
            L"Padding=5,0,0,0"}},
        ThemeTargetStyles{L"Border#BackgroundBorder", {
            L"Margin=2,5,2,5",
            L"CornerRadius=8",
            L"BorderThickness = 0"}},
        ThemeTargetStyles{L"Grid#OverflowRootGrid > Border", {
            L"Background:=$base",
            L"Shadow :=",
            L"CornerRadius = 14"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Shapes.Rectangle#HorizontalTrackRect", {
            L"Height = 8",
            L"Margin = 0",
            L"Fill := $overlay"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Shapes.Rectangle#HorizontalDecreaseRect", {
            L"Height = 8"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.TextBlock#volumeLevelText", {
            L"FontFamily = Tektur",
            L"Margin = 0,-2,0,0"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.Grid#VolumeConfirmator", {
            L"Padding = 8,0,3,0",
            L"CornerRadius = 20"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.Grid#ConfirmatorMainGrid", {
            L"Background :=$base",
            L"CornerRadius = 14",
            L"BorderThickness = 0",
            L"Margin = 0,0,0,10",
            L"Shadow :="}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.Grid#BrightnessConfirmator", {
            L"Padding = 15,0,17,0",
            L"CornerRadius = 20"}},
        ThemeTargetStyles{L"Microsoft.UI.Xaml.Controls.AnimatedIcon#BrightnessIcon", {
            L"Margin = 0,-1,12,0"}},
        ThemeTargetStyles{L"Microsoft.UI.Xaml.Controls.ProgressBar#ProgressIndicator", {
            L"Margin = 0,0,0,1"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Shapes.Rectangle#ProgressBarTrack", {
            L"Fill := $inverseBW",
            L"RadiusX = 1.5",
            L"RadiusY = 1.5"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Shapes.Rectangle#DeterminateProgressBarIndicator", {
            L"Fill :=$accentColor"}},
        ThemeTargetStyles{L"Taskbar.TaskListButton#TaskListButton > Grid#IconPanel > Microsoft.UI.Xaml.Controls.ProgressBar#ProgressIndicator, Taskbar.TaskListButton#TaskListButton > Taskbar.TaskListLabeledButtonPanel#IconPanel > Microsoft.UI.Xaml.Controls.ProgressBar#ProgressIndicator", {
            L"MinHeight = 4",
            L"Width = 26"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.ContentPresenter#ContentPresenter", {
            L"BorderThickness = 0"}},
        ThemeTargetStyles{L"Taskbar.ExperienceToggleButton#LaunchListButton[AutomationProperties.Name=Start]", {
            L"Margin = 0,0,2,0"}},
        ThemeTargetStyles{L"Taskbar.Badge#BadgeControl", {
            L"Height = 14",
            L"MinWidth = 14",
            L"Margin = 0,0,0,0",
            L"CornerRadius = 20"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Shapes.Rectangle#BackgroundRect", {
            L"RadiusX = 4",
            L"RadiusY = 4"}},
        ThemeTargetStyles{L"MenuFlyoutPresenter", {
            L"Background := $base",
            L"Shadow :=",
            L"CornerRadius = 8"}},
        ThemeTargetStyles{L"SearchUx.SearchUI.SearchButtonRootGrid@CommonStates > Grid > Border#BackgroundElement, SearchUx.SearchUI.SearchButtonRootGrid@CommonStates > Border#BackgroundElement", {
            L"Background@InactiveNormal :=$base",
            L"CornerRadius = $mainRadius",
            L"Background :=$base",
            L"Background@InactivePointerOver :=$overlay2",
            L"Background@ActivePointerOver:=$overlay",
            L"Background@ActiveNormal :=$active"}},
    }, {
        L"mainRadius = 8",
        L"transparent = <SolidColorBrush Color=\"Transparent\"/>",
        L"base = <AcrylicBrush TintColor=\"{ThemeResource SystemAltLowColor}\" TintOpacity=\"1\" TintLuminosityOpacity=\"0.7\" FallbackColor=\"{ThemeResource SystemChromeLowColor}\" />",
        L"overlay = <AcrylicBrush TintColor=\"{ThemeResource SystemAltLowColor}\" TintOpacity=\"1\" TintLuminosityOpacity=\"0.8\" FallbackColor=\"{ThemeResource CardStrokeColorDefaultSolid}\" />",
        L"overlay2 = <AcrylicBrush TintColor=\"{ThemeResource SystemAltLowColor}\" TintOpacity=\"1\" TintLuminosityOpacity=\"0.5\" FallbackColor=\"{ThemeResource CardStrokeColorDefaultSolid}\" />",
        L"accentColor = <SolidColorBrush Color=\"{ThemeResource SystemAccentColor}\" Opacity = \"1\" />",
        L"inverseBW = <SolidColorBrush Color=\"{ThemeResource SystemBaseHighColor}\" Opacity = \"1\" />",
        L"active = <AcrylicBrush TintColor=\"{ThemeResource SystemAltLowColor}\" TintOpacity=\"1\" TintLuminosityOpacity=\"1\" FallbackColor=\"{ThemeResource CardStrokeColorDefaultSolid}\" />",
    }};
    return theme;
}

inline const Theme& Theme_Surface()
{
    static const Theme theme = {{
        ThemeTargetStyles{L"Grid#RootGrid > Taskbar.TaskbarBackground", {
            L"Margin=-20,0,-20,0"}},
        ThemeTargetStyles{L"Grid#RootGrid > Taskbar.TaskbarBackground > Grid", {
            L"CornerRadius=20",
            L"BorderThickness=1",
            L"BorderBrush=#40FFFFFF",
            L"Padding=-1"}},
        ThemeTargetStyles{L"Rectangle#BackgroundStroke", {
            L"Fill=Transparent"}},
        ThemeTargetStyles{L"Taskbar.TaskbarFrame > Grid#RootGrid@DockingStates", {
            L"Tag=horizontal",
            L"Tag@DockedLeft=vertical",
            L"Tag@DockedRight=vertical",
            L"Tag=>taskbarDock"}},
        ThemeTargetStyles{L"Taskbar.TaskbarFrame", {
            L"Width={{taskbarDock==`vertical`?skip():`Auto`}}",
            L"HorizontalAlignment=Center"}},
        ThemeTargetStyles{L"Taskbar.TaskbarFrame > Grid#RootGrid", {
            L"Visibility=Visible",
            L"Margin=0,0,0,4",
            L"Padding=20,0,20,0"}},
        ThemeTargetStyles{L"StackPanel#SystemTrayFrameGrid, Grid#SystemTrayFrameGrid", {
            L"Margin=0,0,0,4",
            L"CornerRadius=20,0,0,20",
            L"BorderThickness=1,1,0,1",
            L"BorderBrush=#66FFFFFF",
            L"Padding=10,1,0,1",
            L"Background:=<WindhawkBlur BlurAmount=\"5\" TintColor=\"{ThemeResource SystemChromeAltHighColor}\" TintOpacity=\"0.5\" />",
            L"Visibility=Visible"}},
        ThemeTargetStyles{L"Taskbar.TaskbarFrame > Grid#RootGrid > Taskbar.TaskbarBackground > Grid > Rectangle#BackgroundFill", {
            L"Fill:=<WindhawkBlur BlurAmount=\"5\" TintColor=\"#12FFFFFF\"/>"}},
        ThemeTargetStyles{L"Grid#IconPanel@RunningIndicatorStates > Border#BackgroundElement, Taskbar.TaskListLabeledButtonPanel@RunningIndicatorStates > Border#BackgroundElement", {
            L"Background:=$TaskItemBackground",
            L"Margin=-1,3,1,2",
            L"CornerRadius=12",
            L"BorderThickness=2,1,0.5,2",
            L"BorderBrush:=$TaskItemBorder"}},
        ThemeTargetStyles{L"Taskbar.TaskListButtonPanel#ExperienceToggleButtonRootPanel > Grid > Border#BackgroundElement, Taskbar.TaskListButtonPanel#ExperienceToggleButtonRootPanel > Border#BackgroundElement, SearchUx.SearchUI.SearchButtonRootGrid#SearchBoxButtonRootPanel > Grid > Border#BackgroundElement, SearchUx.SearchUI.SearchButtonRootGrid#SearchBoxButtonRootPanel > Border#BackgroundElement", {
            L"Background:=$SystemItemBackground",
            L"CornerRadius=12",
            L"Margin=-1,3,2.5,2",
            L"BorderThickness=2,1,0.5,2",
            L"BorderBrush:=$SystemItemBorder"}},
        ThemeTargetStyles{L"Grid#IconPanel@CommonStates > Rectangle#RunningIndicator, Taskbar.TaskListLabeledButtonPanel@CommonStates > Rectangle#RunningIndicator", {
            L"Margin=0,0,0,4"}},
        ThemeTargetStyles{L"Border#MultiWindowElement", {
            L"Height=0"}},
    }, {
        L"TaskItemBackground=<AcrylicBrush TintColor=\"{ThemeResource SystemChromeAltHighColor}\" TintOpacity=\"0.9\" FallbackColor=\"{ThemeResource SystemChromeMediumColor}\" />",
        L"TaskItemBorder=<LinearGradientBrush StartPoint=\"0,0\" EndPoint=\"0.5,1\"><GradientStop Color=\"#00000000\" Offset=\"0\" /><GradientStop Color=\"#33000000\" Offset=\"1.5\" /></LinearGradientBrush>",
        L"SystemItemBackground=<AcrylicBrush TintColor=\"{ThemeResource SystemChromeAltHighColor}\" TintOpacity=\"0.8\" FallbackColor=\"{ThemeResource SystemChromeLowColor}\" />",
        L"SystemItemBorder=<LinearGradientBrush StartPoint=\"0,0\" EndPoint=\"0.5,1\"><GradientStop Color=\"#00000000\" Offset=\"0\" /><GradientStop Color=\"#33000000\" Offset=\"1.5\" /></LinearGradientBrush>",
    }};
    return theme;
}

inline const Theme& Theme_Luminosity_variant_Dock()
{
    static const Theme theme = {{
        ThemeTargetStyles{L"Taskbar.TaskbarFrame > Grid#RootGrid > Taskbar.TaskbarBackground > Grid > Rectangle#BackgroundFill", {
            L"Fill:=$mbg"}},
        ThemeTargetStyles{L"Taskbar.TaskListButtonPanel#ExperienceToggleButtonRootPanel > Windows.UI.Xaml.Controls.Grid > Windows.UI.Xaml.Controls.Border#BackgroundElement, Taskbar.TaskListButtonPanel#ExperienceToggleButtonRootPanel > Windows.UI.Xaml.Controls.Border#BackgroundElement", {
            L"CornerRadius=$bcr"}},
        ThemeTargetStyles{L"Taskbar.ExperienceToggleButton", {
            L"CornerRadius=$bcr"}},
        ThemeTargetStyles{L"Taskbar.TaskListButton", {
            L"CornerRadius=$bcr"}},
        ThemeTargetStyles{L"SearchUx.SearchUI.SearchButtonRootGrid#SearchBoxButtonRootPanel > Windows.UI.Xaml.Controls.Grid > Windows.UI.Xaml.Controls.Border#BackgroundElement, SearchUx.SearchUI.SearchButtonRootGrid#SearchBoxButtonRootPanel > Windows.UI.Xaml.Controls.Border#BackgroundElement", {
            L"CornerRadius=$bcr"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.Border#SearchPillBackgroundElement", {
            L"CornerRadius=$bcr"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.Border#MultiWindowElement", {
            L"CornerRadius=$bcr"}},
        ThemeTargetStyles{L"SystemTray.ChevronIconView > Windows.UI.Xaml.Controls.Grid#ContainerGrid > Windows.UI.Xaml.Controls.Border#BackgroundBorder", {
            L"CornerRadius=$bcr",
            L"Margin=0,4,2,4"}},
        ThemeTargetStyles{L"SystemTray.NotifyIconView > Windows.UI.Xaml.Controls.Grid#ContainerGrid > Windows.UI.Xaml.Controls.Border#BackgroundBorder", {
            L"CornerRadius=$bcr",
            L"Margin=2,4,2,4"}},
        ThemeTargetStyles{L"SystemTray.IconView#SystemTrayIcon > Windows.UI.Xaml.Controls.Grid#ContainerGrid > Windows.UI.Xaml.Controls.Border#BackgroundBorder", {
            L"CornerRadius=$bcr",
            L"Margin=2,4,2,4"}},
        ThemeTargetStyles{L"SystemTray.OmniButton#ControlCenterButton > Windows.UI.Xaml.Controls.Grid > Windows.UI.Xaml.Controls.Border#BackgroundBorder", {
            L"CornerRadius=$bcr",
            L"Margin=2,4,2,4"}},
        ThemeTargetStyles{L"SystemTray.OmniButton#NotificationCenterButton > Windows.UI.Xaml.Controls.Grid > Windows.UI.Xaml.Controls.Border#BackgroundBorder", {
            L"CornerRadius=$bcr",
            L"Margin=2,4,2,4"}},
        ThemeTargetStyles{L"Border#OverflowFlyoutBackgroundBorder", {
            L"Background:=$mbg",
            L"CornerRadius:=$wcr",
            L"BorderThickness=$bt",
            L"BorderBrush=$bb",
            L"Shadow:="}},
        ThemeTargetStyles{L"Taskbar.OverflowToggleButton#OverflowButton > Taskbar.TaskListButtonPanel#OverflowToggleButtonRootPanel > Windows.UI.Xaml.Controls.Grid > Windows.UI.Xaml.Controls.Border#BackgroundElement, Taskbar.OverflowToggleButton#OverflowButton > Taskbar.TaskListButtonPanel#OverflowToggleButtonRootPanel > Windows.UI.Xaml.Controls.Border#BackgroundElement", {
            L"CornerRadius=$bcr"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.Grid#ConfirmatorMainGrid", {
            L"Background:=$mbg",
            L"CornerRadius=$wcr",
            L"BorderThickness=$bt",
            L"BorderBrush=$bb",
            L"Shadow:="}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Shapes.Rectangle#HorizontalTrackRect", {
            L"Fill:=#10FFFFFF"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.TextInput.Common.InputSwitcher > ContentControl > ContentPresenter > Grid", {
            L"Background:=$mbg",
            L"CornerRadius=$wcr",
            L"BorderThickness=$bt",
            L"BorderBrush=$bb",
            L"Shadow:="}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.TextInput.Common.InputSwitcher > ContentControl > ContentPresenter > Grid > Grid", {
            L"Background:=$t"}},
        ThemeTargetStyles{L"Taskbar.TaskbarBackground#HoverFlyoutBackgroundControl > Grid > Rectangle#BackgroundFill", {
            L"Fill:=$t"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.Grid#HoverFlyoutGrid > Windows.UI.Xaml.Controls.Border#HoverFlyoutBackground", {
            L"Background:=$mbg",
            L"CornerRadius=$mcr",
            L"BorderThickness=$bt",
            L"BorderBrush=$bb",
            L"Shadow:="}},
        ThemeTargetStyles{L"Taskbar.TaskItemThumbnailView > Grid@CommonStates > Border#BackgroundBorder", {
            L"Background=$t",
            L"CornerRadius=$mcr",
            L"BorderThickness@Normal=0",
            L"BorderThickness@PointerOver=0.05,0,0.05,1",
            L"BorderBrush@Normal=$t",
            L"BorderBrush@PointerOver:=$AccentColor"}},
        ThemeTargetStyles{L"Taskbar.TaskItemThumbnailView > Grid > Button#CloseButton", {
            L"CornerRadius=$mcr"}},
        ThemeTargetStyles{L"Taskbar.ThumbBarButton#ThumbBarButton > Windows.UI.Xaml.Controls.ContentPresenter#BorderElement@CommonStates", {
            L"CornerRadius=16",
            L"Margin=-1.5",
            L"Background@Disabled:=$t",
            L"Background@Normal:=$t",
            L"Background@PointerOver:=$nbth",
            L"Background@Pressed:=$nbtp",
            L"BorderThickness=2",
            L"BorderBrush@Disabled:=$t",
            L"BorderBrush@Normal:=$t",
            L"BorderBrush@PointerOver:=$nbb",
            L"BorderBrush@Pressed:=$nbb",
            L"BackgroundSizing=InnerBorderEdge",
            L"BackgroundTransition:=<BrushTransition Duration=\"0:0:0.200\" />"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.AltTab > Windows.UI.Xaml.Controls.Grid#ModalRootGrid > Windows.UI.Xaml.Controls.Border#BackgroundElement", {
            L"Background=$t",
            L"CornerRadius=$wcr",
            L"BorderThickness=0",
            L"Shadow:="}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.AltTab > Windows.UI.Xaml.Controls.Grid#ModalRootGrid > Windows.UI.Xaml.Controls.Border#BackgroundElement > WindowsInternal.ComposableShell.Experiences.Switcher.SwitchItemList", {
            L"Background:=$mbg",
            L"CornerRadius=$wcr",
            L"BorderThickness=$bt",
            L"BorderBrush=$bb"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.DynamicFlowPanel > WindowsInternal.ComposableShell.Experiences.Switcher.SwitchItemListViewItem > Windows.UI.Xaml.Controls.Grid#Root@CommonStates > Windows.UI.Xaml.Controls.Border#BackgroundBorder", {
            L"Background:=#09FFFFFF",
            L"CornerRadius=$mcr",
            L"BorderThickness=0.05,1,0.05,0",
            L"BorderBrush@Normal=$t",
            L"BorderBrush@PointerOver:=$AccentColor"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.SwitchItemControl > Grid#Root > WindowsInternal.ComposableShell.Experiences.Switcher.SwitchItemThumbnailButton#ThumbnailHost > Windows.UI.Xaml.Controls.Grid#RootGrid", {
            L"CornerRadius=$bcr",
            L"Margin=5"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.Border#BackgroundDimmingLayer", {
            L"Background:=$t"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.VirtualDesktopBarElement > Windows.UI.Xaml.Controls.Grid#GridElement > Windows.UI.Xaml.Controls.Border#VirtualDesktopSwitcherBackground", {
            L"CornerRadius=$wcr",
            L"BorderThickness=$bt",
            L"BorderBrush=$bb",
            L"Margin=-2,1,-1,2"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.DynamicFlowPanel#DFCPanel > WindowsInternal.ComposableShell.Experiences.Switcher.SwitchItemListViewItem > Windows.UI.Xaml.Controls.Grid#Root@CommonStates > Windows.UI.Xaml.Controls.Border#BackgroundBorder", {
            L"Background:=$mbg",
            L"CornerRadius=$wcr,$wcr,$bcr,$bcr",
            L"Margin:=-5,0,-5,-5",
            L"BorderThickness=0.05,1,0.05,0",
            L"BorderBrush@Normal=$t",
            L"BorderBrush@PointerOver:=$AccentColor"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.SwitchItemElement > Windows.UI.Xaml.Controls.Grid#RootGrid > Windows.UI.Xaml.Controls.Grid#TitleGrid > Windows.UI.Xaml.Controls.Border#BackgroundBorder", {
            L"Background:=$t"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.SwitchItemElement > Windows.UI.Xaml.Controls.Grid#RootGrid > Windows.UI.Xaml.Controls.Grid#TitleGrid > Image#IconImage", {
            L"RenderTransform:=<TranslateTransform X=\"0\" Y=\"1\" />"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.SwitchItemElement > Windows.UI.Xaml.Controls.Grid#RootGrid > Windows.UI.Xaml.Controls.Grid#TitleGrid > TextBlock#DisplayName", {
            L"RenderTransform:=<TranslateTransform X=\"0\" Y=\"1\" />"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.Button#SwitchItemElementCloseButton > ContentPresenter#ContentPresenter", {
            L"CornerRadius=$mcr",
            L"Margin=5"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.Button#SwitchItemElementCloseButton > ContentPresenter#ContentPresenter > TextBlock", {
            L"RenderTransform:=<TranslateTransform X=\"-0.8\" Y=\"-0.6\" />"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.SwitchItemElement > Grid#RootGrid > WindowsInternal.ComposableShell.Experiences.Switcher.SwitchItemThumbnailButton#ThumbnailHost > Grid#RootGrid", {
            L"CornerRadius=0"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.VirtualDesktopBarElement#VirtualDesktopBar > Grid > Border", {
            L"Background:=$mbg",
            L"CornerRadius=$wcr",
            L"BorderThickness=$bt",
            L"BorderBrush=$bb",
            L"Margin=-1,2,0,0"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.VirtualDesktopBarElement#VirtualDesktopBar", {
            L"Width=Auto",
            L"HorizontalAlignment=1"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.VirtualDesktopBarElement > Grid > Border", {
            L"Background:=$mbg",
            L"Shadow:="}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.VirtualDesktopElementThemed > Windows.UI.Xaml.Controls.Grid#MainGrid > Windows.UI.Xaml.Controls.Border#MainBorder", {
            L"CornerRadius=$mcr"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.VirtualDesktopElementThemed > Windows.UI.Xaml.Controls.Grid#MainGrid > Windows.UI.Xaml.Controls.Border#BorderHighlight", {
            L"CornerRadius=$mcr"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.NewVirtualDesktopElementThemed#NewVirtualDesktopButtonThemed > Windows.UI.Xaml.Controls.Grid#MainGrid", {
            L"CornerRadius=$mcr",
            L"BorderThickness=$bt",
            L"Margin=2"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.NewVirtualDesktopElementThemed#NewVirtualDesktopButtonThemed > Windows.UI.Xaml.Controls.Grid#MainGrid > Windows.UI.Xaml.Controls.Border#MainBorder", {
            L"CornerRadius=$mcr"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.NewVirtualDesktopElementThemed#NewVirtualDesktopButtonThemed > Windows.UI.Xaml.Controls.Grid#MainGrid > Windows.UI.Xaml.Controls.Border#BorderHighlight", {
            L"CornerRadius=$mcr"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.VirtualDesktopThumbnailButton#ThumbnailButtonElement", {
            L"CornerRadius=$bcr"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.Button#VirtualDesktopElementCloseButton", {
            L"CornerRadius=$bcr"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.Border#SnapBarBorder", {
            L"Background:=$mbg",
            L"CornerRadius=$mcr",
            L"BorderThickness=$bt",
            L"BorderBrush=$bb",
            L"Shadow:="}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.Border#SnapPickerBorder", {
            L"Background:=$mbg",
            L"CornerRadius=$mcr",
            L"BorderThickness=$bt",
            L"BorderBrush=$bb"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.FlyoutPresenter > Border", {
            L"Shadow:="}},
        ThemeTargetStyles{L"MenuFlyoutPresenter", {
            L"CornerRadius=$mcr",
            L"Shadow:="}},
        ThemeTargetStyles{L"MenuFlyoutPresenter > Border", {
            L"Background:=$mbg",
            L"CornerRadius=$mcr",
            L"BorderThickness=$bt",
            L"BorderBrush=$bb"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.MenuFlyoutItem", {
            L"CornerRadius=$bcr"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.MenuFlyoutSubItem", {
            L"CornerRadius=$bcr"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.ToolTip > Windows.UI.Xaml.Controls.ContentPresenter#LayoutRoot", {
            L"Background:=$mbg",
            L"CornerRadius=$mcr",
            L"BorderThickness=$bt",
            L"BorderBrush=$bb",
            L"Shadow:="}},
        ThemeTargetStyles{L"ScrollViewer#MenuFlyoutPresenterScrollViewer > Border > Grid > ScrollContentPresenter > ItemsPresenter > StackPanel", {
            L"ChildrenTransitions:=<TransitionCollection><EntranceThemeTransition $AnimationSettings /></TransitionCollection>"}},
        ThemeTargetStyles{L"Grid#LayoutRoot", {
            L"BackgroundTransition:=<BrushTransition Duration=\"0:0:0.100\" />"}},
        ThemeTargetStyles{L"Border#BackgroundBorder", {
            L"BackgroundTransition:=<BrushTransition Duration=\"0:0:0.100\" />"}},
        ThemeTargetStyles{L"Taskbar.TaskbarFrame > Grid#RootGrid > Taskbar.TaskbarBackground > Grid > Rectangle#BackgroundFill", {
            L"Visibility=Collapsed"}},
        ThemeTargetStyles{L"Taskbar.TaskbarFrame > Grid#RootGrid@DockingStates", {
            L"Tag=horizontal",
            L"Tag@DockedLeft=vertical",
            L"Tag@DockedRight=vertical",
            L"Tag=>taskbarDock"}},
        ThemeTargetStyles{L"Taskbar.TaskbarFrame", {
            L"Width={{taskbarDock==`vertical`?skip():`Auto`}}",
            L"HorizontalAlignment=Stretch",
            L"Margin=$DockMargin,0,$DockMargin,0"}},
        ThemeTargetStyles{L"Taskbar.TaskbarFrame > Grid#RootGrid", {
            L"Background:=$mbg",
            L"BorderThickness=$bt",
            L"BorderBrush=$bb",
            L"CornerRadius=$mcr",
            L"Margin=0,$DockTopGap,0,$DockBottomGap"}},
        ThemeTargetStyles{L"Taskbar.TaskbarBackground#BackgroundControl > Windows.UI.Xaml.Controls.Grid > Windows.UI.Xaml.Shapes.Rectangle#BackgroundStroke", {
            L"Visibility=Collapsed"}},
        ThemeTargetStyles{L"Taskbar.TaskListButtonPanel#ExperienceToggleButtonRootPanel > Windows.UI.Xaml.Controls.Grid > Windows.UI.Xaml.Controls.Grid#AugmentedEntryPointContentGrid, Taskbar.TaskListButtonPanel#ExperienceToggleButtonRootPanel > Windows.UI.Xaml.Controls.Grid#AugmentedEntryPointContentGrid", {
            L"RenderTransform:=<TranslateTransform X=\"0\" Y=\"-1\" />"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.Border#LargeTicker1", {
            L"Margin=0,0,0,-2"}},
        ThemeTargetStyles{L"Taskbar.AugmentedEntryPointButton#AugmentedEntryPointButton", {
            L"Margin=0,0,$WidgetGap57,0"}},
        ThemeTargetStyles{L"SystemTray.SystemTrayFrame", {
            L"VerticalAlignment=Center",
            L"Margin=0,$DockTopGap,$DockMargin,$DockBottomGap"}},
        ThemeTargetStyles{L"Taskbar.TaskbarFrame", {
            L"Height={{taskbarDock==`vertical`?skip():$DockHeight}}"}},
    }, {
        L"DockMargin=250",
        L"DockHeight=$TaskbarHeight",
        L"DockTopGap=3",
        L"DockBottomGap=3",
        L"WidgetGap=-",
        L"AccentColor=<SolidColorBrush Color=\"{ThemeResource SystemAccentColorLight2}\" Opacity=\"1.0\" />",
        L"AnimationSettings=IsStaggeringEnabled=\"True\" FromHorizontalOffset=\"-50\" FromVerticalOffset=\"50\"",
        L"mbg=<WindhawkBlur BlurAmount=\"30\" TintColor=\"{ThemeResource CardStrokeColorDefaultSolid}\" TintOpacity=\"0.0\" TintLuminosityOpacity=\"1.0\" TintSaturation=\"1.0\" NoiseDensity=\"1.0\" NoiseOpacity=\"0.1\" />",
        L"bcr=10",
        L"wcr=20",
        L"mcr=15",
        L"t=Transparent",
        L"bb=#20FFFFFF",
        L"bt=1",
        L"nbb=<LinearGradientBrush x:Key=\"ShellTaskbarItemGradientStrokeColorSecondaryBrush\" MappingMode=\"Absolute\" StartPoint=\"0,0\" EndPoint=\"0,3\"><LinearGradientBrush.GradientStops><GradientStop Offset=\"0.33\" Color=\"#1AFFFFFF\" /><GradientStop Offset=\"1\" Color=\"#0FFFFFFF\" /></LinearGradientBrush.GradientStops></LinearGradientBrush>",
        L"nbt=<SolidColorBrush Color=\"{ThemeResource ControlFillColorDefault}\" />",
        L"nbth=<SolidColorBrush Color=\"{ThemeResource ControlFillColorSecondary}\" />",
        L"nbtp=<SolidColorBrush Color=\"{ThemeResource ControlFillColorTertiary}\" />",
    }};
    return theme;
}

inline const Theme& Theme_Luminosity_variant_Classic()
{
    static const Theme theme = {{
        ThemeTargetStyles{L"Taskbar.TaskbarFrame > Grid#RootGrid > Taskbar.TaskbarBackground > Grid > Rectangle#BackgroundFill", {
            L"Fill:=$mbg"}},
        ThemeTargetStyles{L"Taskbar.TaskListButtonPanel#ExperienceToggleButtonRootPanel > Windows.UI.Xaml.Controls.Grid > Windows.UI.Xaml.Controls.Border#BackgroundElement, Taskbar.TaskListButtonPanel#ExperienceToggleButtonRootPanel > Windows.UI.Xaml.Controls.Border#BackgroundElement", {
            L"CornerRadius=$bcr"}},
        ThemeTargetStyles{L"Taskbar.ExperienceToggleButton", {
            L"CornerRadius=$bcr"}},
        ThemeTargetStyles{L"Taskbar.TaskListButton", {
            L"CornerRadius=$bcr"}},
        ThemeTargetStyles{L"SearchUx.SearchUI.SearchButtonRootGrid#SearchBoxButtonRootPanel > Windows.UI.Xaml.Controls.Grid > Windows.UI.Xaml.Controls.Border#BackgroundElement, SearchUx.SearchUI.SearchButtonRootGrid#SearchBoxButtonRootPanel > Windows.UI.Xaml.Controls.Border#BackgroundElement", {
            L"CornerRadius=$bcr"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.Border#SearchPillBackgroundElement", {
            L"CornerRadius=$bcr"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.Border#MultiWindowElement", {
            L"CornerRadius=$bcr"}},
        ThemeTargetStyles{L"SystemTray.ChevronIconView > Windows.UI.Xaml.Controls.Grid#ContainerGrid > Windows.UI.Xaml.Controls.Border#BackgroundBorder", {
            L"CornerRadius=$bcr",
            L"Margin=0,4,2,4"}},
        ThemeTargetStyles{L"SystemTray.NotifyIconView > Windows.UI.Xaml.Controls.Grid#ContainerGrid > Windows.UI.Xaml.Controls.Border#BackgroundBorder", {
            L"CornerRadius=$bcr",
            L"Margin=2,4,2,4"}},
        ThemeTargetStyles{L"SystemTray.IconView#SystemTrayIcon > Windows.UI.Xaml.Controls.Grid#ContainerGrid > Windows.UI.Xaml.Controls.Border#BackgroundBorder", {
            L"CornerRadius=$bcr",
            L"Margin=2,4,2,4"}},
        ThemeTargetStyles{L"SystemTray.OmniButton#ControlCenterButton > Windows.UI.Xaml.Controls.Grid > Windows.UI.Xaml.Controls.Border#BackgroundBorder", {
            L"CornerRadius=$bcr",
            L"Margin=2,4,2,4"}},
        ThemeTargetStyles{L"SystemTray.OmniButton#NotificationCenterButton > Windows.UI.Xaml.Controls.Grid > Windows.UI.Xaml.Controls.Border#BackgroundBorder", {
            L"CornerRadius=$bcr",
            L"Margin=2,4,2,4"}},
        ThemeTargetStyles{L"Border#OverflowFlyoutBackgroundBorder", {
            L"Background:=$mbg",
            L"CornerRadius:=$wcr",
            L"BorderThickness=$bt",
            L"BorderBrush=$bb",
            L"Shadow:="}},
        ThemeTargetStyles{L"Taskbar.OverflowToggleButton#OverflowButton > Taskbar.TaskListButtonPanel#OverflowToggleButtonRootPanel > Windows.UI.Xaml.Controls.Grid > Windows.UI.Xaml.Controls.Border#BackgroundElement, Taskbar.OverflowToggleButton#OverflowButton > Taskbar.TaskListButtonPanel#OverflowToggleButtonRootPanel > Windows.UI.Xaml.Controls.Border#BackgroundElement", {
            L"CornerRadius=$bcr"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.Grid#ConfirmatorMainGrid", {
            L"Background:=$mbg",
            L"CornerRadius=$wcr",
            L"BorderThickness=$bt",
            L"BorderBrush=$bb",
            L"Shadow:="}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Shapes.Rectangle#HorizontalTrackRect", {
            L"Fill:=#10FFFFFF"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.TextInput.Common.InputSwitcher > ContentControl > ContentPresenter > Grid", {
            L"Background:=$mbg",
            L"CornerRadius=$wcr",
            L"BorderThickness=$bt",
            L"BorderBrush=$bb",
            L"Shadow:="}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.TextInput.Common.InputSwitcher > ContentControl > ContentPresenter > Grid > Grid", {
            L"Background:=$t"}},
        ThemeTargetStyles{L"Taskbar.TaskbarBackground#HoverFlyoutBackgroundControl > Grid > Rectangle#BackgroundFill", {
            L"Fill:=$t"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.Grid#HoverFlyoutGrid > Windows.UI.Xaml.Controls.Border#HoverFlyoutBackground", {
            L"Background:=$mbg",
            L"CornerRadius=$mcr",
            L"BorderThickness=$bt",
            L"BorderBrush=$bb",
            L"Shadow:="}},
        ThemeTargetStyles{L"Taskbar.TaskItemThumbnailView > Grid@CommonStates > Border#BackgroundBorder", {
            L"Background=$t",
            L"CornerRadius=$mcr",
            L"BorderThickness@Normal=0",
            L"BorderThickness@PointerOver=0.05,0,0.05,1",
            L"BorderBrush@Normal=$t",
            L"BorderBrush@PointerOver:=$AccentColor"}},
        ThemeTargetStyles{L"Taskbar.TaskItemThumbnailView > Grid > Button#CloseButton", {
            L"CornerRadius=$mcr"}},
        ThemeTargetStyles{L"Taskbar.ThumbBarButton#ThumbBarButton > Windows.UI.Xaml.Controls.ContentPresenter#BorderElement@CommonStates", {
            L"CornerRadius=16",
            L"Margin=-1.5",
            L"Background@Disabled:=$t",
            L"Background@Normal:=$t",
            L"Background@PointerOver:=$nbth",
            L"Background@Pressed:=$nbtp",
            L"BorderThickness=2",
            L"BorderBrush@Disabled:=$t",
            L"BorderBrush@Normal:=$t",
            L"BorderBrush@PointerOver:=$nbb",
            L"BorderBrush@Pressed:=$nbb",
            L"BackgroundSizing=InnerBorderEdge",
            L"BackgroundTransition:=<BrushTransition Duration=\"0:0:0.200\" />"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.AltTab > Windows.UI.Xaml.Controls.Grid#ModalRootGrid > Windows.UI.Xaml.Controls.Border#BackgroundElement", {
            L"Background=$t",
            L"CornerRadius=$wcr",
            L"BorderThickness=0",
            L"Shadow:="}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.AltTab > Windows.UI.Xaml.Controls.Grid#ModalRootGrid > Windows.UI.Xaml.Controls.Border#BackgroundElement > WindowsInternal.ComposableShell.Experiences.Switcher.SwitchItemList", {
            L"Background:=$mbg",
            L"CornerRadius=$wcr",
            L"BorderThickness=$bt",
            L"BorderBrush=$bb"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.DynamicFlowPanel > WindowsInternal.ComposableShell.Experiences.Switcher.SwitchItemListViewItem > Windows.UI.Xaml.Controls.Grid#Root@CommonStates > Windows.UI.Xaml.Controls.Border#BackgroundBorder", {
            L"Background:=#09FFFFFF",
            L"CornerRadius=$mcr",
            L"BorderThickness=0.05,1,0.05,0",
            L"BorderBrush@Normal=$t",
            L"BorderBrush@PointerOver:=$AccentColor"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.SwitchItemControl > Grid#Root > WindowsInternal.ComposableShell.Experiences.Switcher.SwitchItemThumbnailButton#ThumbnailHost > Windows.UI.Xaml.Controls.Grid#RootGrid", {
            L"CornerRadius=$bcr",
            L"Margin=5"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.Border#BackgroundDimmingLayer", {
            L"Background:=$t"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.VirtualDesktopBarElement > Windows.UI.Xaml.Controls.Grid#GridElement > Windows.UI.Xaml.Controls.Border#VirtualDesktopSwitcherBackground", {
            L"CornerRadius=$wcr",
            L"BorderThickness=$bt",
            L"BorderBrush=$bb",
            L"Margin=-2,1,-1,2"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.DynamicFlowPanel#DFCPanel > WindowsInternal.ComposableShell.Experiences.Switcher.SwitchItemListViewItem > Windows.UI.Xaml.Controls.Grid#Root@CommonStates > Windows.UI.Xaml.Controls.Border#BackgroundBorder", {
            L"Background:=$mbg",
            L"CornerRadius=$wcr,$wcr,$bcr,$bcr",
            L"Margin:=-5,0,-5,-5",
            L"BorderThickness=0.05,1,0.05,0",
            L"BorderBrush@Normal=$t",
            L"BorderBrush@PointerOver:=$AccentColor"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.SwitchItemElement > Windows.UI.Xaml.Controls.Grid#RootGrid > Windows.UI.Xaml.Controls.Grid#TitleGrid > Windows.UI.Xaml.Controls.Border#BackgroundBorder", {
            L"Background:=$t"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.SwitchItemElement > Windows.UI.Xaml.Controls.Grid#RootGrid > Windows.UI.Xaml.Controls.Grid#TitleGrid > Image#IconImage", {
            L"RenderTransform:=<TranslateTransform X=\"0\" Y=\"1\" />"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.SwitchItemElement > Windows.UI.Xaml.Controls.Grid#RootGrid > Windows.UI.Xaml.Controls.Grid#TitleGrid > TextBlock#DisplayName", {
            L"RenderTransform:=<TranslateTransform X=\"0\" Y=\"1\" />"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.Button#SwitchItemElementCloseButton > ContentPresenter#ContentPresenter", {
            L"CornerRadius=$mcr",
            L"Margin=5"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.Button#SwitchItemElementCloseButton > ContentPresenter#ContentPresenter > TextBlock", {
            L"RenderTransform:=<TranslateTransform X=\"-0.8\" Y=\"-0.6\" />"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.SwitchItemElement > Grid#RootGrid > WindowsInternal.ComposableShell.Experiences.Switcher.SwitchItemThumbnailButton#ThumbnailHost > Grid#RootGrid", {
            L"CornerRadius=0"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.VirtualDesktopBarElement#VirtualDesktopBar > Grid > Border", {
            L"Background:=$mbg",
            L"CornerRadius=$wcr",
            L"BorderThickness=$bt",
            L"BorderBrush=$bb",
            L"Margin=-1,2,0,0"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.VirtualDesktopBarElement#VirtualDesktopBar", {
            L"Width=Auto",
            L"HorizontalAlignment=1"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.VirtualDesktopBarElement > Grid > Border", {
            L"Background:=$mbg",
            L"Shadow:="}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.VirtualDesktopElementThemed > Windows.UI.Xaml.Controls.Grid#MainGrid > Windows.UI.Xaml.Controls.Border#MainBorder", {
            L"CornerRadius=$mcr"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.VirtualDesktopElementThemed > Windows.UI.Xaml.Controls.Grid#MainGrid > Windows.UI.Xaml.Controls.Border#BorderHighlight", {
            L"CornerRadius=$mcr"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.NewVirtualDesktopElementThemed#NewVirtualDesktopButtonThemed > Windows.UI.Xaml.Controls.Grid#MainGrid", {
            L"CornerRadius=$mcr",
            L"BorderThickness=$bt",
            L"Margin=2"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.NewVirtualDesktopElementThemed#NewVirtualDesktopButtonThemed > Windows.UI.Xaml.Controls.Grid#MainGrid > Windows.UI.Xaml.Controls.Border#MainBorder", {
            L"CornerRadius=$mcr"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.NewVirtualDesktopElementThemed#NewVirtualDesktopButtonThemed > Windows.UI.Xaml.Controls.Grid#MainGrid > Windows.UI.Xaml.Controls.Border#BorderHighlight", {
            L"CornerRadius=$mcr"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.VirtualDesktopThumbnailButton#ThumbnailButtonElement", {
            L"CornerRadius=$bcr"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.Button#VirtualDesktopElementCloseButton", {
            L"CornerRadius=$bcr"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.Border#SnapBarBorder", {
            L"Background:=$mbg",
            L"CornerRadius=$mcr",
            L"BorderThickness=$bt",
            L"BorderBrush=$bb",
            L"Shadow:="}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.Border#SnapPickerBorder", {
            L"Background:=$mbg",
            L"CornerRadius=$mcr",
            L"BorderThickness=$bt",
            L"BorderBrush=$bb"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.FlyoutPresenter > Border", {
            L"Shadow:="}},
        ThemeTargetStyles{L"MenuFlyoutPresenter", {
            L"CornerRadius=$mcr",
            L"Shadow:="}},
        ThemeTargetStyles{L"MenuFlyoutPresenter > Border", {
            L"Background:=$mbg",
            L"CornerRadius=$mcr",
            L"BorderThickness=$bt",
            L"BorderBrush=$bb"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.MenuFlyoutItem", {
            L"CornerRadius=$bcr"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.MenuFlyoutSubItem", {
            L"CornerRadius=$bcr"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.ToolTip > Windows.UI.Xaml.Controls.ContentPresenter#LayoutRoot", {
            L"Background:=$mbg",
            L"CornerRadius=$mcr",
            L"BorderThickness=$bt",
            L"BorderBrush=$bb",
            L"Shadow:="}},
        ThemeTargetStyles{L"ScrollViewer#MenuFlyoutPresenterScrollViewer > Border > Grid > ScrollContentPresenter > ItemsPresenter > StackPanel", {
            L"ChildrenTransitions:=<TransitionCollection><EntranceThemeTransition $AnimationSettings /></TransitionCollection>"}},
        ThemeTargetStyles{L"Grid#LayoutRoot", {
            L"BackgroundTransition:=<BrushTransition Duration=\"0:0:0.100\" />"}},
        ThemeTargetStyles{L"Border#BackgroundBorder", {
            L"BackgroundTransition:=<BrushTransition Duration=\"0:0:0.100\" />"}},
    }, {
        L"AccentColor=<SolidColorBrush Color=\"{ThemeResource SystemAccentColorLight2}\" Opacity=\"1.0\" />",
        L"AnimationSettings=IsStaggeringEnabled=\"True\" FromHorizontalOffset=\"-50\" FromVerticalOffset=\"50\"",
        L"mbg=<WindhawkBlur BlurAmount=\"30\" TintColor=\"{ThemeResource CardStrokeColorDefaultSolid}\" TintOpacity=\"0.0\" TintLuminosityOpacity=\"1.0\" TintSaturation=\"1.0\" NoiseDensity=\"1.0\" NoiseOpacity=\"0.1\" />",
        L"bcr=10",
        L"wcr=20",
        L"mcr=15",
        L"t=Transparent",
        L"bb=#20FFFFFF",
        L"bt=1",
        L"nbb=<LinearGradientBrush x:Key=\"ShellTaskbarItemGradientStrokeColorSecondaryBrush\" MappingMode=\"Absolute\" StartPoint=\"0,0\" EndPoint=\"0,3\"><LinearGradientBrush.GradientStops><GradientStop Offset=\"0.33\" Color=\"#1AFFFFFF\" /><GradientStop Offset=\"1\" Color=\"#0FFFFFFF\" /></LinearGradientBrush.GradientStops></LinearGradientBrush>",
        L"nbt=<SolidColorBrush Color=\"{ThemeResource ControlFillColorDefault}\" />",
        L"nbth=<SolidColorBrush Color=\"{ThemeResource ControlFillColorSecondary}\" />",
        L"nbtp=<SolidColorBrush Color=\"{ThemeResource ControlFillColorTertiary}\" />",
    }};
    return theme;
}

inline const Theme& Theme_Luminosity_variant_Compact()
{
    static const Theme theme = {{
        ThemeTargetStyles{L"Taskbar.TaskbarFrame > Grid#RootGrid > Taskbar.TaskbarBackground > Grid > Rectangle#BackgroundFill", {
            L"Fill:=$mbg"}},
        ThemeTargetStyles{L"Taskbar.AugmentedEntryPointButton#AugmentedEntryPointButton", {
            L"Margin=0,0,$WidgetGap57,0"}},
        ThemeTargetStyles{L"Taskbar.TaskListButtonPanel#ExperienceToggleButtonRootPanel > Windows.UI.Xaml.Controls.Grid > Windows.UI.Xaml.Controls.Border#BackgroundElement, Taskbar.TaskListButtonPanel#ExperienceToggleButtonRootPanel > Windows.UI.Xaml.Controls.Border#BackgroundElement", {
            L"CornerRadius=$bcr"}},
        ThemeTargetStyles{L"Taskbar.ExperienceToggleButton", {
            L"CornerRadius=$bcr"}},
        ThemeTargetStyles{L"Taskbar.TaskListButton", {
            L"CornerRadius=$bcr"}},
        ThemeTargetStyles{L"SearchUx.SearchUI.SearchButtonRootGrid#SearchBoxButtonRootPanel > Windows.UI.Xaml.Controls.Grid > Windows.UI.Xaml.Controls.Border#BackgroundElement, SearchUx.SearchUI.SearchButtonRootGrid#SearchBoxButtonRootPanel > Windows.UI.Xaml.Controls.Border#BackgroundElement", {
            L"CornerRadius=$bcr"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.Border#SearchPillBackgroundElement", {
            L"CornerRadius=$bcr"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.Border#MultiWindowElement", {
            L"CornerRadius=$bcr"}},
        ThemeTargetStyles{L"SystemTray.ChevronIconView > Windows.UI.Xaml.Controls.Grid#ContainerGrid > Windows.UI.Xaml.Controls.Border#BackgroundBorder", {
            L"CornerRadius=$bcr",
            L"Margin=0,4,2,4"}},
        ThemeTargetStyles{L"SystemTray.NotifyIconView > Windows.UI.Xaml.Controls.Grid#ContainerGrid > Windows.UI.Xaml.Controls.Border#BackgroundBorder", {
            L"CornerRadius=$bcr",
            L"Margin=2,4,2,4"}},
        ThemeTargetStyles{L"SystemTray.IconView#SystemTrayIcon > Windows.UI.Xaml.Controls.Grid#ContainerGrid > Windows.UI.Xaml.Controls.Border#BackgroundBorder", {
            L"CornerRadius=$bcr",
            L"Margin=2,4,2,4"}},
        ThemeTargetStyles{L"SystemTray.OmniButton#ControlCenterButton > Windows.UI.Xaml.Controls.Grid > Windows.UI.Xaml.Controls.Border#BackgroundBorder", {
            L"CornerRadius=$bcr",
            L"Margin=2,4,2,4"}},
        ThemeTargetStyles{L"SystemTray.OmniButton#NotificationCenterButton > Windows.UI.Xaml.Controls.Grid > Windows.UI.Xaml.Controls.Border#BackgroundBorder", {
            L"CornerRadius=$bcr",
            L"Margin=2,4,2,4"}},
        ThemeTargetStyles{L"Border#OverflowFlyoutBackgroundBorder", {
            L"Background:=$mbg",
            L"CornerRadius:=$wcr",
            L"BorderThickness=$bt",
            L"BorderBrush=$bb",
            L"Shadow:="}},
        ThemeTargetStyles{L"Taskbar.OverflowToggleButton#OverflowButton > Taskbar.TaskListButtonPanel#OverflowToggleButtonRootPanel > Windows.UI.Xaml.Controls.Grid > Windows.UI.Xaml.Controls.Border#BackgroundElement, Taskbar.OverflowToggleButton#OverflowButton > Taskbar.TaskListButtonPanel#OverflowToggleButtonRootPanel > Windows.UI.Xaml.Controls.Border#BackgroundElement", {
            L"CornerRadius=$bcr"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.Grid#ConfirmatorMainGrid", {
            L"Background:=$mbg",
            L"CornerRadius=$wcr",
            L"BorderThickness=$bt",
            L"BorderBrush=$bb",
            L"Shadow:="}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Shapes.Rectangle#HorizontalTrackRect", {
            L"Fill:=#10FFFFFF"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.TextInput.Common.InputSwitcher > ContentControl > ContentPresenter > Grid", {
            L"Background:=$mbg",
            L"CornerRadius=$wcr",
            L"BorderThickness=$bt",
            L"BorderBrush=$bb",
            L"Shadow:="}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.TextInput.Common.InputSwitcher > ContentControl > ContentPresenter > Grid > Grid", {
            L"Background:=$t"}},
        ThemeTargetStyles{L"Taskbar.TaskbarBackground#HoverFlyoutBackgroundControl > Grid > Rectangle#BackgroundFill", {
            L"Fill:=$t"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.Grid#HoverFlyoutGrid > Windows.UI.Xaml.Controls.Border#HoverFlyoutBackground", {
            L"Background:=$mbg",
            L"CornerRadius=$mcr",
            L"BorderThickness=$bt",
            L"BorderBrush=$bb",
            L"Shadow:="}},
        ThemeTargetStyles{L"Taskbar.TaskItemThumbnailView > Grid@CommonStates > Border#BackgroundBorder", {
            L"Background=$t",
            L"CornerRadius=$mcr",
            L"BorderThickness@Normal=0",
            L"BorderThickness@PointerOver=0.05,0,0.05,1",
            L"BorderBrush@Normal=$t",
            L"BorderBrush@PointerOver:=$AccentColor"}},
        ThemeTargetStyles{L"Taskbar.TaskItemThumbnailView > Grid > Button#CloseButton", {
            L"CornerRadius=$mcr"}},
        ThemeTargetStyles{L"Taskbar.ThumbBarButton#ThumbBarButton > Windows.UI.Xaml.Controls.ContentPresenter#BorderElement@CommonStates", {
            L"CornerRadius=16",
            L"Margin=-1.5",
            L"Background@Disabled:=$t",
            L"Background@Normal:=$t",
            L"Background@PointerOver:=$nbth",
            L"Background@Pressed:=$nbtp",
            L"BorderThickness=2",
            L"BorderBrush@Disabled:=$t",
            L"BorderBrush@Normal:=$t",
            L"BorderBrush@PointerOver:=$nbb",
            L"BorderBrush@Pressed:=$nbb",
            L"BackgroundSizing=InnerBorderEdge",
            L"BackgroundTransition:=<BrushTransition Duration=\"0:0:0.200\" />"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.AltTab > Windows.UI.Xaml.Controls.Grid#ModalRootGrid > Windows.UI.Xaml.Controls.Border#BackgroundElement", {
            L"Background=$t",
            L"CornerRadius=$wcr",
            L"BorderThickness=0",
            L"Shadow:="}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.AltTab > Windows.UI.Xaml.Controls.Grid#ModalRootGrid > Windows.UI.Xaml.Controls.Border#BackgroundElement > WindowsInternal.ComposableShell.Experiences.Switcher.SwitchItemList", {
            L"Background:=$mbg",
            L"CornerRadius=$wcr",
            L"BorderThickness=$bt",
            L"BorderBrush=$bb"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.DynamicFlowPanel > WindowsInternal.ComposableShell.Experiences.Switcher.SwitchItemListViewItem > Windows.UI.Xaml.Controls.Grid#Root@CommonStates > Windows.UI.Xaml.Controls.Border#BackgroundBorder", {
            L"Background:=#09FFFFFF",
            L"CornerRadius=$mcr",
            L"BorderThickness=0.05,1,0.05,0",
            L"BorderBrush@Normal=$t",
            L"BorderBrush@PointerOver:=$AccentColor"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.SwitchItemControl > Grid#Root > WindowsInternal.ComposableShell.Experiences.Switcher.SwitchItemThumbnailButton#ThumbnailHost > Windows.UI.Xaml.Controls.Grid#RootGrid", {
            L"CornerRadius=$bcr",
            L"Margin=5"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.Border#BackgroundDimmingLayer", {
            L"Background:=$t"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.VirtualDesktopBarElement > Windows.UI.Xaml.Controls.Grid#GridElement > Windows.UI.Xaml.Controls.Border#VirtualDesktopSwitcherBackground", {
            L"CornerRadius=$wcr",
            L"BorderThickness=$bt",
            L"BorderBrush=$bb",
            L"Margin=-2,1,-1,2"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.DynamicFlowPanel#DFCPanel > WindowsInternal.ComposableShell.Experiences.Switcher.SwitchItemListViewItem > Windows.UI.Xaml.Controls.Grid#Root@CommonStates > Windows.UI.Xaml.Controls.Border#BackgroundBorder", {
            L"Background:=$mbg",
            L"CornerRadius=$wcr,$wcr,$bcr,$bcr",
            L"Margin:=-5,0,-5,-5",
            L"BorderThickness=0.05,1,0.05,0",
            L"BorderBrush@Normal=$t",
            L"BorderBrush@PointerOver:=$AccentColor"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.SwitchItemElement > Windows.UI.Xaml.Controls.Grid#RootGrid > Windows.UI.Xaml.Controls.Grid#TitleGrid > Windows.UI.Xaml.Controls.Border#BackgroundBorder", {
            L"Background:=$t"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.SwitchItemElement > Windows.UI.Xaml.Controls.Grid#RootGrid > Windows.UI.Xaml.Controls.Grid#TitleGrid > Image#IconImage", {
            L"RenderTransform:=<TranslateTransform X=\"0\" Y=\"1\" />"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.SwitchItemElement > Windows.UI.Xaml.Controls.Grid#RootGrid > Windows.UI.Xaml.Controls.Grid#TitleGrid > TextBlock#DisplayName", {
            L"RenderTransform:=<TranslateTransform X=\"0\" Y=\"1\" />"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.Button#SwitchItemElementCloseButton > ContentPresenter#ContentPresenter", {
            L"CornerRadius=$mcr",
            L"Margin=5"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.Button#SwitchItemElementCloseButton > ContentPresenter#ContentPresenter > TextBlock", {
            L"RenderTransform:=<TranslateTransform X=\"-0.8\" Y=\"-0.6\" />"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.SwitchItemElement > Grid#RootGrid > WindowsInternal.ComposableShell.Experiences.Switcher.SwitchItemThumbnailButton#ThumbnailHost > Grid#RootGrid", {
            L"CornerRadius=0"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.VirtualDesktopBarElement#VirtualDesktopBar > Grid > Border", {
            L"Background:=$mbg",
            L"CornerRadius=$wcr",
            L"BorderThickness=$bt",
            L"BorderBrush=$bb",
            L"Margin=-1,2,0,0"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.VirtualDesktopBarElement#VirtualDesktopBar", {
            L"Width=Auto",
            L"HorizontalAlignment=1"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.VirtualDesktopBarElement > Grid > Border", {
            L"Background:=$mbg",
            L"Shadow:="}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.VirtualDesktopElementThemed > Windows.UI.Xaml.Controls.Grid#MainGrid > Windows.UI.Xaml.Controls.Border#MainBorder", {
            L"CornerRadius=$mcr"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.VirtualDesktopElementThemed > Windows.UI.Xaml.Controls.Grid#MainGrid > Windows.UI.Xaml.Controls.Border#BorderHighlight", {
            L"CornerRadius=$mcr"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.NewVirtualDesktopElementThemed#NewVirtualDesktopButtonThemed > Windows.UI.Xaml.Controls.Grid#MainGrid", {
            L"CornerRadius=$mcr",
            L"BorderThickness=$bt",
            L"Margin=2"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.NewVirtualDesktopElementThemed#NewVirtualDesktopButtonThemed > Windows.UI.Xaml.Controls.Grid#MainGrid > Windows.UI.Xaml.Controls.Border#MainBorder", {
            L"CornerRadius=$mcr"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.NewVirtualDesktopElementThemed#NewVirtualDesktopButtonThemed > Windows.UI.Xaml.Controls.Grid#MainGrid > Windows.UI.Xaml.Controls.Border#BorderHighlight", {
            L"CornerRadius=$mcr"}},
        ThemeTargetStyles{L"WindowsInternal.ComposableShell.Experiences.Switcher.VirtualDesktopThumbnailButton#ThumbnailButtonElement", {
            L"CornerRadius=$bcr"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.Button#VirtualDesktopElementCloseButton", {
            L"CornerRadius=$bcr"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.Border#SnapBarBorder", {
            L"Background:=$mbg",
            L"CornerRadius=$mcr",
            L"BorderThickness=$bt",
            L"BorderBrush=$bb",
            L"Shadow:="}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.Border#SnapPickerBorder", {
            L"Background:=$mbg",
            L"CornerRadius=$mcr",
            L"BorderThickness=$bt",
            L"BorderBrush=$bb"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.FlyoutPresenter > Border", {
            L"Shadow:="}},
        ThemeTargetStyles{L"MenuFlyoutPresenter", {
            L"CornerRadius=$mcr",
            L"Shadow:="}},
        ThemeTargetStyles{L"MenuFlyoutPresenter > Border", {
            L"Background:=$mbg",
            L"CornerRadius=$mcr",
            L"BorderThickness=$bt",
            L"BorderBrush=$bb"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.MenuFlyoutItem", {
            L"CornerRadius=$bcr"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.MenuFlyoutSubItem", {
            L"CornerRadius=$bcr"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.ToolTip > Windows.UI.Xaml.Controls.ContentPresenter#LayoutRoot", {
            L"Background:=$mbg",
            L"CornerRadius=$mcr",
            L"BorderThickness=$bt",
            L"BorderBrush=$bb",
            L"Shadow:="}},
        ThemeTargetStyles{L"ScrollViewer#MenuFlyoutPresenterScrollViewer > Border > Grid > ScrollContentPresenter > ItemsPresenter > StackPanel", {
            L"ChildrenTransitions:=<TransitionCollection><EntranceThemeTransition $AnimationSettings /></TransitionCollection>"}},
        ThemeTargetStyles{L"Grid#LayoutRoot", {
            L"BackgroundTransition:=<BrushTransition Duration=\"0:0:0.100\" />"}},
        ThemeTargetStyles{L"Border#BackgroundBorder", {
            L"BackgroundTransition:=<BrushTransition Duration=\"0:0:0.100\" />"}},
        ThemeTargetStyles{L"Taskbar.TaskbarFrame", {
            L"Height=$TaskbarHeight"}},
        ThemeTargetStyles{L"Taskbar.TaskListButtonPanel#ExperienceToggleButtonRootPanel > Windows.UI.Xaml.Controls.Grid > Windows.UI.Xaml.Controls.Grid#AugmentedEntryPointContentGrid, Taskbar.TaskListButtonPanel#ExperienceToggleButtonRootPanel > Windows.UI.Xaml.Controls.Grid#AugmentedEntryPointContentGrid", {
            L"RenderTransform:=<TranslateTransform X=\"0\" Y=\"-1\" />"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.Border#LargeTicker1", {
            L"Margin=1,-5,0,0",
            L"RenderTransform:=<ScaleTransform ScaleX=\"0.75\" ScaleY=\"0.75\" />"}},
        ThemeTargetStyles{L"SearchUx.SearchUI.SearchButtonRootGrid#SearchBoxButtonRootPanel > Windows.UI.Xaml.Controls.Grid > Windows.UI.Xaml.Controls.Border#BackgroundElement, SearchUx.SearchUI.SearchButtonRootGrid#SearchBoxButtonRootPanel > Windows.UI.Xaml.Controls.Border#BackgroundElement", {
            L"Margin=0,4,0,4"}},
        ThemeTargetStyles{L"SearchUx.SearchUI.SearchButtonControl", {
            L"Margin=0,-4,0,-4"}},
        ThemeTargetStyles{L"Microsoft.UI.Xaml.Controls.AnimatedVisualPlayer#Icon", {
            L"Width=16",
            L"Height=16"}},
        ThemeTargetStyles{L"Windows.UI.Xaml.Controls.Image#Icon", {
            L"Width=16",
            L"Height=16"}},
        ThemeTargetStyles{L"Taskbar.TaskListButton#TaskListButton > Grid#IconPanel > Windows.UI.Xaml.Controls.TextBlock#LabelControl, Taskbar.TaskListButton#TaskListButton > Taskbar.TaskListLabeledButtonPanel#IconPanel > Windows.UI.Xaml.Controls.TextBlock#LabelControl", {
            L"RenderTransform:=<TranslateTransform X=\"0\" Y=\"-1\" />"}},
        ThemeTargetStyles{L"Taskbar.AugmentedEntryPointButton#AugmentedEntryPointButton", {
            L"Margin=0,0,$WidgetGap57,0"}},
        ThemeTargetStyles{L"SystemTray.SystemTrayFrame", {
            L"Height=$TaskbarHeight"}},
        ThemeTargetStyles{L"SystemTray.TextIconContent > Grid#ContainerGrid > SystemTray.AdaptiveTextBlock#Base > TextBlock#InnerTextBlock", {
            L"FontSize=14"}},
        ThemeTargetStyles{L"SystemTray.ImageIconContent > Grid#ContainerGrid > Image", {
            L"Width=14",
            L"Height=14"}},
    }, {
        L"WidgetGap=-",
        L"AccentColor=<SolidColorBrush Color=\"{ThemeResource SystemAccentColorLight2}\" Opacity=\"1.0\" />",
        L"AnimationSettings=IsStaggeringEnabled=\"True\" FromHorizontalOffset=\"-50\" FromVerticalOffset=\"50\"",
        L"mbg=<WindhawkBlur BlurAmount=\"30\" TintColor=\"{ThemeResource CardStrokeColorDefaultSolid}\" TintOpacity=\"0.0\" TintLuminosityOpacity=\"1.0\" TintSaturation=\"1.0\" NoiseDensity=\"1.0\" NoiseOpacity=\"0.1\" />",
        L"bcr=10",
        L"wcr=20",
        L"mcr=15",
        L"t=Transparent",
        L"bb=#20FFFFFF",
        L"bt=1",
        L"nbb=<LinearGradientBrush x:Key=\"ShellTaskbarItemGradientStrokeColorSecondaryBrush\" MappingMode=\"Absolute\" StartPoint=\"0,0\" EndPoint=\"0,3\"><LinearGradientBrush.GradientStops><GradientStop Offset=\"0.33\" Color=\"#1AFFFFFF\" /><GradientStop Offset=\"1\" Color=\"#0FFFFFFF\" /></LinearGradientBrush.GradientStops></LinearGradientBrush>",
        L"nbt=<SolidColorBrush Color=\"{ThemeResource ControlFillColorDefault}\" />",
        L"nbth=<SolidColorBrush Color=\"{ThemeResource ControlFillColorSecondary}\" />",
        L"nbtp=<SolidColorBrush Color=\"{ThemeResource ControlFillColorTertiary}\" />",
    }};
    return theme;
}

}   // namespace stylerthemes
