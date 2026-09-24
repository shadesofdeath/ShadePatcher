#pragma once
//
// menuitems.h - the user's own entries in the shell context menu.
//
// Both halves of the product use this: the settings window writes the list, and the mod inside explorer.exe
// reads it every time the menu opens. Keeping the format in one place is what stops the two from drifting.
//
// Layout in the registry:
//
//   HKCU\Software\ShadePatcher\Mods\taskbar-menu-entry\Items
//       Count                 dword    how many entries follow
//   HKCU\Software\ShadePatcher\Mods\taskbar-menu-entry\Items\0
//       Name                  sz       what the menu shows
//       Command               sz       what to run when it is chosen
//       Arguments             sz       optional arguments
//       Icon                  sz       optional: a path to an image, or a Segoe Fluent Icons glyph given either
//                                      as the character itself or as its code (E713, 0xE713, , U+E713)
//
// Entries are numbered from zero and renumbered on every save, so the order in the registry is the order in the
// menu and there are never gaps.
//
#include <Windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SP_MENU_MAX_ITEMS       32
#define SP_MENU_NAME_LENGTH     128
#define SP_MENU_COMMAND_LENGTH  512
#define SP_MENU_ARGS_LENGTH     512
#define SP_MENU_ICON_LENGTH     MAX_PATH

typedef struct SP_MenuItem
{
    wchar_t name[SP_MENU_NAME_LENGTH];
    wchar_t command[SP_MENU_COMMAND_LENGTH];
    wchar_t arguments[SP_MENU_ARGS_LENGTH];
    wchar_t icon[SP_MENU_ICON_LENGTH];
} SP_MenuItem;

// Reads the list into `items`, up to `maxItems`. Returns how many were read; zero when there are none.
// The list belonging to one menu, named by the id of the mod that draws it. Each menu keeps its own list,
// so the taskbar and the desktop can carry different entries.
int  SP_MenuItemsLoadFor(const wchar_t* modId, SP_MenuItem* items, int maxItems);
BOOL SP_MenuItemsSaveFor(const wchar_t* modId, const SP_MenuItem* items, int count);

int SP_MenuItemsLoad(SP_MenuItem* items, int maxItems);

// Replaces the stored list with the first `count` entries. Any previously stored entries beyond that are
// removed, so the registry always matches what the user sees.
BOOL SP_MenuItemsSave(const SP_MenuItem* items, int count);

// Runs one entry. Returns FALSE when the command could not be started.
BOOL SP_MenuItemExecute(const SP_MenuItem* item);

// Reads an entry's Icon field as a Segoe Fluent Icons glyph. The user may have typed the character itself or
// its code in any of the usual spellings (E713, e713, 0xE713, , U+E713, &#xE713;); all of them come back
// as the single character. Returns 0 when the text is not a glyph, in which case it is a path.
wchar_t SP_MenuItemIconGlyph(const wchar_t* icon);

#ifdef __cplusplus
}
#endif
