//
// mod_table.c - the list of mods built into this DLL.
//
// Adding a mod is two lines: declare its SP_Mod here and add it to the table. Nothing is registered by magic, so
// this file answers "what is in the build?" on its own.
//
// The order is the order mods are initialized in. It does not decide who wins a shared gesture: that is the
// user's InputPriority setting, arbitrated in engine/input.c.
//
#include "engine/mod.h"

SP_MOD_DECLARE(g_modTrayShowAllIcons);
SP_MOD_DECLARE(g_modTrayIconsRebroadcast);
SP_MOD_DECLARE(g_modDesktopToggleIcons);
SP_MOD_DECLARE(g_modContextMenuPreloader);
SP_MOD_DECLARE(g_modExplorerAutoFileSizes);
SP_MOD_DECLARE(g_modExplorerDoubleClickUp);
SP_MOD_DECLARE(g_modShellMenuEntry);
SP_MOD_DECLARE(g_modTaskbarMenuEntry);
SP_MOD_DECLARE(g_modDesktopMenuEntry);
SP_MOD_DECLARE(g_modDesktopIconsView);
SP_MOD_DECLARE(g_modExtensionChangeNoWarning);
SP_MOD_DECLARE(g_modTaskbarButtonClick);
SP_MOD_DECLARE(g_modStartMenuAllApps);
SP_MOD_DECLARE(g_modHideDesktopIconText);
SP_MOD_DECLARE(g_modExplorerSingleWindowTabs);
SP_MOD_DECLARE(g_modExplorerReopenClosedTab);
SP_MOD_DECLARE(g_modTaskbarLabels);
SP_MOD_DECLARE(g_modTaskbarWheelCycle);
SP_MOD_DECLARE(g_modTaskbarStartButtonPosition);
SP_MOD_DECLARE(g_modTaskbarEmptySpaceClicks);
SP_MOD_DECLARE(g_modTaskbarVolumeControl);
SP_MOD_DECLARE(g_modTaskbarClockCustomization);
SP_MOD_DECLARE(g_modTaskbarIconSize);
SP_MOD_DECLARE(g_modTaskbarThumbnailSize);
SP_MOD_DECLARE(g_modTaskbarNotificationIconSpacing);
SP_MOD_DECLARE(g_modDesktopIconSelectionStyle);
SP_MOD_DECLARE(g_modTaskbarTraySystemIconTweaks);
SP_MOD_DECLARE(g_modExplorerHideNavItems);
SP_MOD_DECLARE(g_modRemoveContextMenuItems);
SP_MOD_DECLARE(g_modLegacyFileCopy);
SP_MOD_DECLARE(g_modLegacyTrayFlyouts);
SP_MOD_DECLARE(g_modTaskbarCountBadges);
SP_MOD_DECLARE(g_modDesktopIconsSpotlight);
SP_MOD_DECLARE(g_modTaskbarStyler);
SP_MOD_DECLARE(g_modStartPowerButtons);
SP_MOD_DECLARE(g_modStartMenuOnLeft);
SP_MOD_DECLARE(g_modCustomStartMenu);

const SP_Mod* const g_modTable[] =
{
    // Shell-wide
    &g_modShellMenuEntry,
    &g_modTaskbarMenuEntry,
    &g_modDesktopMenuEntry,

    // Taskbar
    &g_modTrayShowAllIcons,
    &g_modTrayIconsRebroadcast,
    &g_modTaskbarButtonClick,
    &g_modTaskbarLabels,
    &g_modTaskbarCountBadges,
    &g_modTaskbarWheelCycle,
    &g_modTaskbarStartButtonPosition,
    &g_modTaskbarEmptySpaceClicks,
    &g_modTaskbarVolumeControl,
    &g_modTaskbarClockCustomization,
    &g_modTaskbarIconSize,
    &g_modTaskbarThumbnailSize,
    &g_modTaskbarStyler,
    &g_modTaskbarNotificationIconSpacing,
    &g_modTaskbarTraySystemIconTweaks,
    &g_modLegacyTrayFlyouts,

    // Desktop and Start
    &g_modDesktopToggleIcons,
    &g_modDesktopIconsView,
    &g_modHideDesktopIconText,
    &g_modDesktopIconSelectionStyle,
    &g_modDesktopIconsSpotlight,
    &g_modStartMenuAllApps,
    &g_modStartPowerButtons,
    &g_modStartMenuOnLeft,     // StartMenuExperienceHost.exe; shares its id with the Start button mod
    &g_modCustomStartMenu,     // ShadePatcher's own Start menu (src/core/startmenu)

    // File Explorer
    &g_modContextMenuPreloader,
    &g_modExplorerAutoFileSizes,
    &g_modExplorerDoubleClickUp,
    &g_modExtensionChangeNoWarning,
    &g_modExplorerSingleWindowTabs,
    &g_modExplorerReopenClosedTab,
    &g_modExplorerHideNavItems,
    &g_modRemoveContextMenuItems,
    &g_modLegacyFileCopy,
};

const int g_modTableCount = (int)(sizeof(g_modTable) / sizeof(g_modTable[0]));
