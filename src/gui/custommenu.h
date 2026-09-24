#pragma once
//
// custommenu.h - the editor for the user's own context menu entries.
//
// The settings page engine draws every option from settings.reg. A list the user adds to and deletes from is
// not fixed text, so it is turned into ordinary page lines here and drawn by the window itself: a drop-down of
// ready-made entries to add one, and a text box per field to edit one. There is no dialog of its own.
//
// What it edits is the list in common/menuitems.h, which the mod inside explorer.exe reads back.
//
#include <Windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// Replaces the ";k" line in a page definition with the controls for the stored menu entries.
//
// Returns a buffer owned by this module, valid until the next call, or NULL when the page has no ";k" line. The
// caller does not free it.
PVOID SP_ExpandCustomMenuLines(const void* page, DWORD pageSize, DWORD* expandedSize);

// Handles the actions the expanded lines produce: ";cm_add" and ";cm_del_<n>".
// Returns TRUE when the list changed and the page has to be rebuilt.
BOOL SP_HandleCustomMenuAction(HWND hParent, const char* action);

#ifdef __cplusplus
}
#endif
