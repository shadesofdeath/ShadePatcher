# hide-desktop-icon-text - integration notes

Source: `src/core/mods/hide_desktop_icon_text.cpp`
Original: Windhawk `hide-desktop-icon-text` v1.5.0 by kivsak

## 1. Declaration

```c
SP_MOD_DECLARE(g_modHideDesktopIconText);
```

`SP_MOD_ID` is `"hide-desktop-icon-text"`. `minOsBuild` is 0 (the desktop list view is the same on every
supported build), `targets` is `SP_TARGET_EXPLORER`, `flags` is 0.

## 2. Exception handling

Not needed. No C++/WinRT, no `throw`. The file only uses Win32, plain COM (IShellFolder / IImageList) and the
STL (`std::wstring`, `std::unordered_set`, `std::vector`), so the project's existing setting is fine either way.

Link pragmas inside the file: `Comctl32.lib`, `Shell32.lib`, `Shlwapi.lib`, `Gdi32.lib`. Nothing to add to
core.vcxproj.

## 3. Settings read (HKCU\Software\ShadePatcher\Mods\hide-desktop-icon-text)

| Name         | Type  | Default | Meaning |
|--------------|-------|---------|---------|
| `Enabled`    | dword | 0       | Engine toggle (read by the engine, not the mod). |
| `HideText`   | dword | 1       | 1 = draw desktop icon labels as empty text, except folder names. 0 = labels are drawn normally. |
| `HideArrows` | dword | 1       | 1 = point the shell's link overlay at a transparent image (affects the whole explorer.exe, every File Explorer window included). 0 = put the stock arrow back. |

Both are on/off only (0 / non-zero). The Windhawk names were `hide_text` and `hide_arrows`; they were renamed
to the engine's PascalCase convention. Changes are applied live from `SettingsChanged`, no restart needed.

## 4. Proposed settings.reg lines and strings (ids 1510-1513)

```
;a %R:1513%
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\hide-desktop-icon-text]
;b %R:1510%
"Enabled"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\hide-desktop-icon-text]
;b %R:1511%
"HideText"=dword:00000001
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\hide-desktop-icon-text]
;b %R:1512%
"HideArrows"=dword:00000001
```

The `;a %R:1513%` heading line is optional; drop it if the mod goes under an existing desktop heading
(e.g. next to `desktop-toggle-icons`).

| Id   | Suggested define                  | English | Turkish |
|------|-----------------------------------|---------|---------|
| 1510 | `IDS_MOD_DESKTOPICONTEXT`         | Hide desktop icon text and shortcut arrows | Masaustu simge yazilarini ve kisayol oklarini gizle |
| 1511 | `IDS_MOD_DESKTOPICONTEXT_TEXT`    | Hide the text under desktop icons (folder names are kept) | Masaustu simgelerinin altindaki yaziyi gizle (klasor adlari kalir) |
| 1512 | `IDS_MOD_DESKTOPICONTEXT_ARROWS`  | Remove the shortcut arrow from icons (applies to every Explorer window) | Simgelerdeki kisayol okunu kaldir (tum Gezgin pencerelerine uygulanir) |
| 1513 | `IDS_MOD_DESKTOPICONTEXT_HEADING` | Desktop icons | Masaustu simgeleri |

Turkish with proper characters for the .rc file (the ASCII rows above are only so this file stays ASCII):

- 1510: `Masaüstü simge yazılarını ve kısayol oklarını gizle`
- 1511: `Masaüstü simgelerinin altındaki yazıyı gizle (klasör adları kalır)`
- 1512: `Simgelerdeki kısayol okunu kaldır (tüm Gezgin pencerelerine uygulanır)`
- 1513: `Masaüstü simgeleri`

Ids 1514-1519 are unused.

## 5. What the port does, and what was left out or changed

Kept (core behaviour):

- Labels: the desktop `SysListView32` is subclassed; during its `WM_PAINT` / `WM_PRINTCLIENT` a thread-local
  flag is set and the hooked `DrawTextW`, `DrawTextExW` (user32) and `DrawThemeTextEx` (uxtheme) draw an empty
  string for any text that is not a folder name on the desktop. Folder names are enumerated with
  `SHGetDesktopFolder` (folders with `SFGAO_FOLDER` and without `SFGAO_STREAM`, so .zip files are treated as
  files), at most once a second.
- Arrows: a transparent icon is added to the five shared system image lists (`SHIL_*`) and the link overlay
  slot (`SHGetIconOverlayIndexW(IDO_SHGIOI_LINK)`) is pointed at it. `SHChangeNotify(SHCNE_ASSOCCHANGED)` is
  sent afterwards, as the original did, so open views refresh their icons.
- `CreateWindowExW` is hooked to re-subclass the desktop when the shell recreates it (theme change, "Show
  desktop icons" toggle). The existing desktop is picked up in `AfterInit`.

Changed / added:

- `DrawTextExW` is hooked in addition to the original's `DrawTextW` and `DrawThemeTextEx`. It is the sibling
  entry point comctl32 can use; the check is one thread-local read, so it costs nothing elsewhere.
- The folder cache is also marked stale when the view inserts, deletes or renames an item (`LVM_INSERTITEM`,
  `LVM_DELETEITEM`, `LVM_SETITEMTEXT`, ...), so a newly created folder keeps its label at the next paint
  instead of up to a second later.
- The original could not restore the arrows without an Explorer restart. The port remembers the image it added
  to each list and, when `HideArrows` is turned off or the mod is disabled, replaces that image in place with
  the stock link overlay (`SHGetStockIconInfo(SIID_LINK)` + `SHDefExtractIconW` at the list's size, honouring a
  Shell Icons override). This is best effort; an Explorer restart always restores the original.
- Settings changes are applied live (`SettingsChanged`) instead of reloading the mod.
- The desktop is only invalidated (`RedrawWindow` with `RDW_INVALIDATE`) from the engine thread; the original's
  synchronous `UpdateWindow` from a foreign thread was dropped to avoid sending into the desktop's thread.
- `IsDesktopListView` no longer requires the window caption "FolderView": class `SysListView32` under
  `SHELLDLL_DefView` under Progman / WorkerW / `GetShellWindow()` is specific enough, and it also catches the
  case where the caption is set after creation. WorkerW was added as an accepted grandparent (the shell parks
  the desktop view there in some configurations).
- The mod subclasses the desktop list view directly with `SP_SetWindowSubclassFromAnyThread` rather than
  `SP_SubscribeInput`: it needs `WM_PAINT` and list-view messages, which are not gestures. The engine's own
  desktop-surface subclass and this one coexist (different `SUBCLASSPROC`).

Left out:

- Nothing functional. The original has no other features.

Known limitations (inherited from the original, by design of the approach):

- Matching is by drawn text only, so a *file* that has exactly the same display name as a *folder* on the
  desktop keeps its label too.
- The list view caches the measured label rectangle per item; after enabling the mod the highlight box of a
  selected item may keep its old size until the shell re-measures (any item change, theme change, or
  restart). The text itself is hidden immediately.
- The arrow removal is process-wide (shared image lists): File Explorer windows lose the arrow too. That is
  how the original behaves and is documented in its readme.

## 6. Verifying on a live shell

1. Enable the mod (`Enabled=1`) with both options on. Within a moment the desktop should repaint: files and
   shortcuts show only their icons, folders (and Recycle Bin / This PC, which are shell folders) keep their
   names. Shortcut icons on the desktop and in any File Explorer window lose the arrow overlay (there may be
   one refresh flash from `SHCNE_ASSOCCHANGED`).
2. Right-click the desktop > New > Folder: the new folder's name stays visible. Create a new text file: its
   name is hidden.
3. Turn `HideText` off in the settings window: labels reappear on the next repaint (move the mouse over the
   desktop or click empty space if it does not repaint by itself). Turn it back on: they vanish again.
4. Turn `HideArrows` off: the arrows come back on the desktop and in Explorer windows (best-effort restore).
   Turn it on again: they disappear.
5. Right-click the desktop > View > untick and re-tick "Show desktop icons": the shell recreates the list
   view; the labels must still be hidden (this exercises the `CreateWindowExW` path).
6. Disable the mod: labels return, arrows return, no crash. Restart Explorer to be sure the original overlay
   image is back if the best-effort restore looked off.
7. With `Logging=2` the log shows "Watching the desktop list view", "N folder name(s) on the desktop" and
   "Link overlay hidden in 5 image list(s)".
