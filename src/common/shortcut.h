#pragma once
//
// shortcut.h - the Start menu entry for the settings window.
//
// Typing the product name into Start search should find the settings, the way it finds ExplorerPatcher's
// "Properties". A shortcut in the Programs folder does that: rundll32 "<core dll>",ZZGUI with the product icon.
// The installer makes it for all users; the engine makes a per-user one when it runs from a folder that was
// never installed, so the build-folder workflow gets it too.
//
#include <Windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// TRUE when a settings shortcut (in either language's name) exists in the all-users or the user's Programs folder.
BOOL SettingsShortcutExists(BOOL allUsers);

// Writes the shortcut, pointing at `coreDllPath`. Needs administrator rights when `allUsers` is TRUE.
BOOL CreateSettingsShortcut(const wchar_t* coreDllPath, BOOL allUsers);

// Creates the per-user shortcut unless one already exists anywhere.
BOOL EnsureSettingsShortcut(const wchar_t* coreDllPath);

// Removes every settings shortcut, all users' and the current user's.
void RemoveSettingsShortcuts(void);

#ifdef __cplusplus
}
#endif
