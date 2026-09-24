# remove-context-menu-items - integration notes

Source: `src/core/mods/remove_context_menu_items.cpp`
Based on the Windhawk mod `remove-context-menu-items` v1.12.0 by Armaninyow (MIT).

## 1. SP_MOD_DECLARE symbol

`g_modRemoveContextMenuItems`

Suggested placement in `mod_table.c`: the "File Explorer" group (next to `g_modExplorerAutoFileSizes` /
`g_modExtensionChangeNoWarning`). The mod's own filtering runs on both the desktop and folder-window menus, so
the Desktop group would also be defensible; File Explorer is where the settings page ids (1440-1459) live.

Ordering relative to `desktop-menu-entry` does not matter: both hook the same six `ContextMenuPresenter`
members, each does its own thing on the flyout, and neither depends on the other having run. The only visible
interaction is that a user who lists "ShadePatcher" under CustomLabels will hide the product's own row when
this mod's hook runs after that mod's (hook installed later runs first).

## 2. ExceptionHandling

**Yes**: `<ExceptionHandling>Sync</ExceptionHandling>` is required. The file uses C++/WinRT
(`winrt/Microsoft.UI.Xaml.Controls.h`, `.Primitives.h`, `winrt/Windows.Foundation.Collections.h`), exactly the
same projection headers and include path as `desktop_menu_entry.cpp`. Every WinRT call is inside try/catch;
the classic-menu path is plain Win32 but also wrapped, since `std::vector`/`std::wstring` can throw.

`core.vcxproj` line:

```
    <ClCompile Include="mods\remove_context_menu_items.cpp">
      <ExceptionHandling>Sync</ExceptionHandling>
    </ClCompile>
```

`#include <unknwn.h>` precedes the first winrt include, per the lessons list.

## 3. Settings read

All under `HKCU\Software\ShadePatcher\Mods\remove-context-menu-items`. All dwords are 0/1 (1 = hide the row).
Defaults follow the original: Microsoft's "bloatware" rows are hidden out of the box, shell rows are kept.

| Name                          | Type  | Default | Rows hidden when 1 (English spellings; all 11 languages of the original are matched) |
|-------------------------------|-------|---------|-------------------------------------------------------------------------------------|
| `HideShare`                   | dword | 0       | Share (the icon in the WinUI strip, and the classic row)                             |
| `HideCopyAsPath`              | dword | 0       | Copy as path                                                                         |
| `HideOpenInTerminal`          | dword | 0       | Open in Terminal                                                                     |
| `HideAddToFavorites`          | dword | 0       | Add to Favorites                                                                     |
| `HideCopilot`                 | dword | 1       | Ask Copilot; Ask Microsoft 365 Copilot                                               |
| `HideRotate`                  | dword | 0       | Rotate left; Rotate right                                                            |
| `HideSetAsBackground`         | dword | 0       | Set as desktop background                                                            |
| `HideGiveAccessTo`            | dword | 0       | Give access to                                                                       |
| `HideRestorePreviousVersions` | dword | 0       | Restore previous versions                                                            |
| `HideSendTo`                  | dword | 0       | Send to                                                                              |
| `HideOpenWith`                | dword | 0       | Open with                                                                            |
| `HideOneDrive`                | dword | 1       | Move to OneDrive; Always keep on this device; Free up space                          |
| `HideDefender`                | dword | 1       | Scan with Microsoft Defender...                                                      |
| `HideDesigner`                | dword | 1       | Create with Designer                                                                 |
| `HideClipchamp`               | dword | 1       | Edit with Clipchamp                                                                  |
| `HidePinToStart`              | dword | 0       | Pin to Start                                                                         |
| `HidePinToQuickAccess`        | dword | 0       | Pin to Quick access                                                                  |
| `HideEditInNotepad`           | dword | 0       | Edit in Notepad; Edit with Notepad++                                                 |
| `CustomLabels`                | sz    | `""`    | Semicolon-separated labels to hide, e.g. `Open in new tab;Pin to*;Cast to Device`   |

`CustomLabels` rules: each piece is trimmed, lower-cased (CharLowerBuffW, so Turkish/Cyrillic fold too), and
`...` / U+2026 are treated alike; a piece ending in `*` matches every row that starts with the text before it;
a piece of just `*` is refused (logged) because it would empty the menu. Custom labels are additive: they hide
a row even when that row's own switch is off. Up to 4095 characters are read.

Settings are re-read on `SettingsChanged`; no restart needed. When every switch is 0 and CustomLabels is empty
the hooks stay installed but return immediately (`g_anything == false`).

## 4. Proposed settings.reg lines and strings (ids 1440-1459, all used)

```
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\remove-context-menu-items]
;b %R:1440%
"Enabled"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\remove-context-menu-items]
;b %R:1441%
"HideShare"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\remove-context-menu-items]
;b %R:1442%
"HideCopyAsPath"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\remove-context-menu-items]
;b %R:1443%
"HideOpenInTerminal"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\remove-context-menu-items]
;b %R:1444%
"HideAddToFavorites"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\remove-context-menu-items]
;b %R:1445%
"HideCopilot"=dword:00000001
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\remove-context-menu-items]
;b %R:1446%
"HideRotate"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\remove-context-menu-items]
;b %R:1447%
"HideSetAsBackground"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\remove-context-menu-items]
;b %R:1448%
"HideGiveAccessTo"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\remove-context-menu-items]
;b %R:1449%
"HideRestorePreviousVersions"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\remove-context-menu-items]
;b %R:1450%
"HideSendTo"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\remove-context-menu-items]
;b %R:1451%
"HideOpenWith"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\remove-context-menu-items]
;b %R:1452%
"HideOneDrive"=dword:00000001
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\remove-context-menu-items]
;b %R:1453%
"HideDefender"=dword:00000001
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\remove-context-menu-items]
;b %R:1454%
"HideDesigner"=dword:00000001
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\remove-context-menu-items]
;b %R:1455%
"HideClipchamp"=dword:00000001
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\remove-context-menu-items]
;b %R:1456%
"HidePinToStart"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\remove-context-menu-items]
;b %R:1457%
"HidePinToQuickAccess"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\remove-context-menu-items]
;b %R:1458%
"HideEditInNotepad"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\remove-context-menu-items]
;w %R:1459%
;%R:1459%
;
"CustomLabels"=""
```

The `;w` block copies the shape of `taskbar-empty-space-clicks` / `CustomCommand`; adjust to whatever the GUI
expects for a text box. No `*` restart marker: everything takes effect on the next right-click. The dword
defaults in the .reg match the defaults compiled into `kOptions`, so a fresh install and a reset agree.

`strings.h` (File Explorer page range):

```
#define IDS_MOD_REMOVEMENUITEMS                 1440
#define IDS_MOD_REMOVEMENUITEMS_SHARE           1441
#define IDS_MOD_REMOVEMENUITEMS_COPYASPATH      1442
#define IDS_MOD_REMOVEMENUITEMS_TERMINAL        1443
#define IDS_MOD_REMOVEMENUITEMS_FAVORITES       1444
#define IDS_MOD_REMOVEMENUITEMS_COPILOT         1445
#define IDS_MOD_REMOVEMENUITEMS_ROTATE          1446
#define IDS_MOD_REMOVEMENUITEMS_BACKGROUND      1447
#define IDS_MOD_REMOVEMENUITEMS_GIVEACCESS      1448
#define IDS_MOD_REMOVEMENUITEMS_PREVVERSIONS    1449
#define IDS_MOD_REMOVEMENUITEMS_SENDTO          1450
#define IDS_MOD_REMOVEMENUITEMS_OPENWITH        1451
#define IDS_MOD_REMOVEMENUITEMS_ONEDRIVE        1452
#define IDS_MOD_REMOVEMENUITEMS_DEFENDER        1453
#define IDS_MOD_REMOVEMENUITEMS_DESIGNER        1454
#define IDS_MOD_REMOVEMENUITEMS_CLIPCHAMP       1455
#define IDS_MOD_REMOVEMENUITEMS_PINTOSTART      1456
#define IDS_MOD_REMOVEMENUITEMS_QUICKACCESS     1457
#define IDS_MOD_REMOVEMENUITEMS_NOTEPAD         1458
#define IDS_MOD_REMOVEMENUITEMS_CUSTOM          1459
```

Strings (quotes inside .rc strings must be doubled: `Hide ""Share""`):

| Id   | English | Turkish |
|------|---------|---------|
| 1440 | Remove items from the context menu | Sağ tık menüsünden öğeleri kaldır |
| 1441 | Hide "Share" | "Paylaş" öğesini gizle |
| 1442 | Hide "Copy as path" | "Yolu kopyala" öğesini gizle |
| 1443 | Hide "Open in Terminal" | "Terminal'de aç" öğesini gizle |
| 1444 | Hide "Add to Favorites" | "Sık kullanılanlara ekle" öğesini gizle |
| 1445 | Hide "Ask Copilot" and "Ask Microsoft 365 Copilot" | "Copilot'a sor" ve "Microsoft 365 Copilot'a sor" öğelerini gizle |
| 1446 | Hide "Rotate left" and "Rotate right" | "Sola döndür" ve "Sağa döndür" öğelerini gizle |
| 1447 | Hide "Set as desktop background" | "Masaüstü arka planı olarak ayarla" öğesini gizle |
| 1448 | Hide "Give access to" | "Erişim ver" öğesini gizle |
| 1449 | Hide "Restore previous versions" | "Önceki sürümleri geri yükle" öğesini gizle |
| 1450 | Hide "Send to" | "Gönder" öğesini gizle |
| 1451 | Hide "Open with" | "Birlikte aç" öğesini gizle |
| 1452 | Hide the OneDrive items ("Move to OneDrive", "Always keep on this device", "Free up space") | OneDrive öğelerini gizle ("OneDrive'a taşı", "Her zaman bu cihazda tut", "Alan boşalt") |
| 1453 | Hide "Scan with Microsoft Defender" | "Microsoft Defender ile tara" öğesini gizle |
| 1454 | Hide "Create with Designer" | "Designer ile oluştur" öğesini gizle |
| 1455 | Hide "Edit with Clipchamp" | "Clipchamp ile düzenle" öğesini gizle |
| 1456 | Hide "Pin to Start" | "Başlat'a sabitle" öğesini gizle |
| 1457 | Hide "Pin to Quick access" | "Hızlı erişime sabitle" öğesini gizle |
| 1458 | Hide "Edit in Notepad" and "Edit with Notepad++" | "Not Defteri'nde düzenle" ve "Notepad++ ile düzenle" öğelerini gizle |
| 1459 | Other labels to hide, separated by semicolons (end one with * to match a prefix) | Gizlenecek başka etiketler, noktalı virgülle ayrılmış (önek eşleşmesi için sonuna * ekleyin) |

## 5. What the port does, what changed, what was left out

### Covered

- **The WinUI menu (new in the port).** The original only handles the classic menu and tells Windows 11 users
  to install the "classic context menu" mod. This port also filters the default Windows 11 menu: it hooks the
  same six optional `ContextMenuPresenter` / `ContextMenuPresenter_Old` members as `desktop_menu_entry.cpp`
  (`RegisterTappedOnShowMoreOptions`, `HandleDuplicateAccessKeys`, `SetAccessKeyScope`), installed from
  `SP_WaitForModule(L"Windows.UI.FileExplorer.dll", 0, ...)`. In the hook the `CommandBarFlyout` is read with
  the same pointer-to-copy / direct double reading, then `PrimaryCommands` (icon strip) and
  `SecondaryCommands` (labelled list) are walked backwards; an `AppBarButton` / `AppBarToggleButton` whose
  `Label()` matches is `RemoveAt()`-ed, an `AppBarButton` with a `MenuFlyout` has its `MenuFlyoutItem` /
  `MenuFlyoutSubItem` rows checked too (depth-limited), and doubled/leading/trailing `AppBarSeparator` /
  `MenuFlyoutSeparator` entries are cleaned up. A second pass over an already-filtered flyout removes nothing,
  so it does not matter how many of the six members fire.
- **The classic menu**, as in the original: `TrackPopupMenuEx` and `TrackPopupMenu` are hooked in one
  transaction. The owner window is classified (desktop = root is `GetShellWindow()` or a Progman/WorkerW
  hosting a `SHELLDLL_DefView`; otherwise an ancestor named `SHELLDLL_DefView` = file view, or
  `NamespaceTreeControl` = navigation pane); anything else (taskbar, tray, Start, toolbars) is skipped. Rows
  are read with `GetMenuItemInfoW` (separators, bitmaps and owner-drawn rows skipped), removed with
  `RemoveMenu`, a detached sub-menu is stashed and `DestroyMenu`-ed once the outer tracking call returns, and
  separators are tidied the same way the original does.
- **Lazy sub-menus** ("Send to", "Open with", "New"): as in the original, the outermost call of a session
  installs a thread-local `WH_CALLWNDPROCRET` hook and filters each sub-menu when its `WM_INITMENUPOPUP`
  returns. `TPM_NONOTIFY` falls back to walking sub-menus up front. Nested `TrackPopupMenu` ->
  `TrackPopupMenuEx` calls are handled by a depth counter in a POD `thread_local` (no destructor, on purpose).
  All live message hooks are tracked in a global list; `BeforeUninit` unhooks them and sets a flag so no new
  one is installed after the sweep.
- **Matching**: identical normalisation to the original (cut at `\t`, U+2026 -> `...`, lone `&` dropped for
  HMENU text only, `CharLowerBuffW`, trim), exact match against the table, custom labels checked first with
  the trailing-`*` prefix rule. The label table is the original's, filtered to the rows the 18 switches cover:
  231 spellings across en, ja, pt-BR, pt-PT, es-MX, cs, de, tr, pl, fr, ru, generated by script from the
  Windhawk source (`scratchpad/ports/gen_labels.py`) with every non-ASCII character as a `\xNNNN` escape and
  the literal split wherever the next character is a hex digit, so the file is pure ASCII.

### Changed

- **Settings flattened.** The original's ~55 per-row booleans and three string arrays became 18 dword switches
  (grouped where the original had several rows for one idea: Copilot + M365 Copilot, Rotate left + right, the
  three OneDrive rows, Notepad + Notepad++) and one `CustomLabels` string (semicolon-separated, replaces the
  `customItems[%d]` array). Ids 1440-1459 allowed exactly 18 switches; the rows chosen are the ones that appear
  in the Windows 11 menus most. Everything else the original could hide (Cut, Copy, Delete, Rename, Open,
  Properties, View, Sort by, New, Refresh, Cast to Device, Include in library, Extract All, Display settings,
  Personalize, Customize this folder, Print, Play, Preview, Edit, Edit with Photos/Paint, NVIDIA Control Panel,
  VLC / Media Player rows, Quick Share, WinRAR, Open in new tab/window, Create shortcut) is still reachable
  through `CustomLabels` by typing the visible text.
- **"Compress to ZIP file"** was in the task's example list but is not in the original's table, so it has no
  switch; `CustomLabels` = `Compress to*` covers it (and "Compress to..." on 24H2).
- **No unload-when-idle.** The original refuses to load when nothing is configured; here the hooks stay and
  every hook body returns at once on `g_anything == false`, so turning a switch on later needs no reload.
- **minOsBuild 22000**, not 22621: the classic hooks work on every Windows 11 build and the WinUI hooks are
  optional (a missing presenter symbol is logged, not fatal).

### Left out

- **Extension filtering** (show "Edit in Notepad"/WinRAR only for whitelisted extensions). It needs
  `IShellWindows` / `IShellBrowser` / `IFolderView` lookups of the current selection on every menu, COM init on
  the menu thread, and two more string-array settings; exotic for the benefit, and the WinUI menu would have
  needed a different selection lookup entirely.
- **Alt-to-bypass** (hold Alt while right-clicking to see the full menu). No id left for its switch and it
  should not be on unconditionally, since Alt+click has other meanings in Explorer.
- **"Paste only when greyed out"**: Paste has no switch here; a greyed-state rule only made sense for it.
- **`Wh_ModSettingsChanged` returning FALSE to unload**: see "No unload-when-idle".

### Assumptions the integrator should know about

- The WinUI rows are assumed to be `AppBarButton`s (plus `AppBarToggleButton`) whose `Label()` is the visible
  text, including the icon strip (Cut/Copy/Paste/Rename/Share/Delete carry a Label used for the tooltip). If a
  build uses a custom `ICommandBarElement` type, `LabelOf` returns empty and that row is simply not touched;
  turn on debug logging to see "Nothing to hide in this WinUI menu".
- Removing rows from the collections before the presenter's own member runs was chosen over
  `Visibility(Collapsed)` so that access-key and layout passes never see the hidden row. If
  `RegisterTappedOnShowMoreOptions` turns out to require the "Show more options" row to exist, a user who hides
  that row through `CustomLabels` would be the only one affected; nothing in the port hides it by default.
- The six presenter symbol names are the exact ones `desktop_menu_entry.cpp` resolves on this machine
  (26200); nothing new to download.

## 6. How to verify on a live shell

1. Enable the mod (settings window, or `Enabled=1` under `HKCU\Software\ShadePatcher\Mods\remove-context-menu-items`).
2. **WinUI menu, defaults:** right-click a file on the desktop or in a folder window. "Ask Copilot" (and, if
   OneDrive / Defender / Designer / Clipchamp are installed, their rows) are gone from the labelled list; the
   rest of the menu, including "Show more options", is unchanged.
3. **WinUI icon strip:** set `HideShare=1`, right-click a file: the Share icon in the top strip is gone. Set it
   back to 0: the icon is back on the next right-click (no restart).
4. **WinUI sub-menu rows and custom labels:** set `CustomLabels` to `Open in new tab;Pin to*`, right-click a
   folder: "Open in new tab" and "Pin to Quick access"/"Pin to Start" are gone. Set `HideOpenWith=1` and
   right-click a file: the "Open with" row (which carries a sub-flyout) is gone.
5. **Classic menu:** Shift+right-click (or right-click, then "Show more options") a file in a folder window.
   With `HideSendTo=1` and `HideRestorePreviousVersions=1` those rows are gone and no doubled separator is left
   where they were. Open the "Send to" sub-menu of a file with `CustomLabels=Desktop (create shortcut)`: that
   sub-row is gone (this exercises the WM_INITMENUPOPUP path).
6. **Desktop classic menu:** Shift+right-click empty desktop: with `HideOpenInTerminal=1` "Open in Terminal"
   is gone; with `CustomLabels=Display settings` that row is gone.
7. **Not touched:** right-click the taskbar, a tray icon and the Start button: those menus are unchanged
   whatever the settings (owner-window classification rejects them).
8. **Idle:** set every switch to 0 and clear `CustomLabels`: menus are exactly as stock, and the log shows
   "0 of 18 built-in rows hidden, 0 custom label(s); nothing to hide, menus are left alone".
9. **Log lines** (with `Logging=1`): on load `Classic menu hooks in place (TrackPopupMenuEx, TrackPopupMenu);
   NNN label(s) known; waiting for the WinUI menu builder`, then `WinUI menu: N of 6 presenter members hooked`
   once Windows.UI.FileExplorer.dll is in (expect 3 or 6 depending on which presenter this build has). Per
   menu: `N row(s) hidden from the WinUI menu; M remain in the list` or `TrackPopupMenuEx: N row(s) hidden from
   a classic menu on ... (file view)`. Debug level adds one line per hidden row with its label.
10. **Unload:** disable the mod while a classic menu is open, dismiss the menu, then right-click again: no
    crash, menu is stock (BeforeUninit swept the message hook; the function hooks were removed by the engine).
