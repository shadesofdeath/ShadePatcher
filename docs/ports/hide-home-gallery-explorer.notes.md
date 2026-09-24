# hide-home-gallery-explorer - integration notes

Source: `src/core/mods/explorer_hide_nav_items.cpp`
Based on the Windhawk mod `hide-home-gallery-explorer` v0.3 by rinosaur681 (credited in `basedOn` / `originalAuthor`).

## 1. SP_MOD_DECLARE symbol

```c
SP_MOD_DECLARE(g_modExplorerHideNavItems);
```

Mod id `hide-home-gallery-explorer`, `targets` SP_TARGET_EXPLORER, `minOsBuild` 22000, `flags` 0.
Suggested placement in `mod_table.c`: the File Explorer group (next to `g_modExplorerDoubleClickUp` /
`g_modExtensionChangeNoWarning`). Order does not matter; it uses no shared gestures.

## 2. ExceptionHandling

**No.** Plain Win32 + a raw COM call (`SHCreateItemFromParsingName` / `IShellItem::GetDisplayName`, no C++/WinRT),
nothing throws. A plain `<ClCompile Include="mods\explorer_hide_nav_items.cpp" />` is enough.
Libraries: `Comctl32.lib`, `Shell32.lib`, `Ole32.lib` via `#pragma comment`, all already linked by other mods.

## 3. Settings (HKCU\Software\ShadePatcher\Mods\hide-home-gallery-explorer)

| Name | Type | Default | Meaning |
|------|------|---------|---------|
| `Enabled` | dword | 0 | Engine toggle. Also read by the mod on every SettingsChanged: 0 stops pruning at once. |
| `HideHome` | dword | 1 | Remove the "Home" entry (matched by the localized name of `::{f874310e-b6b7-47dc-bc84-b9e6b38f5903}`, plus "Home" as a fallback). |
| `HideGallery` | dword | 1 | Remove the "Gallery" entry (localized name of `::{e88865ea-0e1c-4e20-9aa6-edcd0212c87c}`, plus "Gallery"). |
| `HideOneDrive` | dword | 1 | Remove every entry whose name contains "OneDrive" ("OneDrive - Personal", a business account, ...). |
| `CustomLabels` | sz | "" | Extra entries to remove, separated by `;`. Exact, case-insensitive match. `label*` = starts with, `*label*` (or `*label`) = contains. A bare `*` is ignored. Example: `Network;Linux;*Drive*`. |

Defaults mirror the original (all three built-ins on). If hiding OneDrive by default is too aggressive for the
product, set `HideOneDrive` to 0 in settings.reg; the mod's own default is only used when the value is missing.

Labels are matched against the text the pane shows, so on a Turkish system `CustomLabels` must use the Turkish names
(`Ağ`, `Bu bilgisayar`, `Geri Dönüşüm Kutusu`...). Home/Gallery need no translation: their names come from the shell.

## 4. Proposed settings.reg lines and strings

Ids 1460-1465 used (1466-1469 free). Goes in the File Explorer section (`;T %R:1401%`), e.g. after the
`file-explorer-reopen-closed-tab` block. No `*` restart marker: everything applies live.

```
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\hide-home-gallery-explorer]
;b %R:1460%
"Enabled"=dword:00000000
;e %R:1465%
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\hide-home-gallery-explorer]
;b %R:1461%
"HideHome"=dword:00000001
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\hide-home-gallery-explorer]
;b %R:1462%
"HideGallery"=dword:00000001
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\hide-home-gallery-explorer]
;b %R:1463%
"HideOneDrive"=dword:00000001
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\hide-home-gallery-explorer]
;w %R:1464%
;%R:1464%
;
"CustomLabels"=""
```

`strings.h` (File Explorer page range 1401-1499):

```
#define IDS_MOD_HIDENAVITEMS            1460
#define IDS_MOD_HIDENAVITEMS_HOME       1461
#define IDS_MOD_HIDENAVITEMS_GALLERY    1462
#define IDS_MOD_HIDENAVITEMS_ONEDRIVE   1463
#define IDS_MOD_HIDENAVITEMS_CUSTOM     1464
#define IDS_MOD_HIDENAVITEMS_HELP       1465
```

| Id | English (`lang/gui.en-US.rc`) | Turkish (`lang/gui.tr-TR.rc`) |
|----|----------|---------|
| 1460 | Hide Home, Gallery and OneDrive from the File Explorer navigation pane | Dosya Gezgini gezinti bölmesinden Giriş, Galeri ve OneDrive'ı gizle |
| 1461 | Hide Home | Giriş'i gizle |
| 1462 | Hide Gallery | Galeri'yi gizle |
| 1463 | Hide OneDrive (every entry whose name contains OneDrive) | OneDrive'ı gizle (adında OneDrive geçen her öğe) |
| 1464 | Other entries to hide, separated by semicolons; * is a wildcard (e.g. Network;Linux;*Drive*) | Gizlenecek diğer öğeler, noktalı virgülle ayrılmış; * joker karakterdir (örn. Ağ;Linux;*Drive*) |
| 1465 | Entries are removed as the pane is filled and come back when the window is reopened. Nothing is written to the registry. | Öğeler bölme dolarken kaldırılır, pencere yeniden açılınca geri gelir. Kayıt defterine hiçbir şey yazılmaz. |

Drop 1465 if the page gets crowded; nothing depends on it.

## 5. What was left out / changed, and why

Kept: the core behaviour (delete the Home / Gallery / OneDrive tree items, plus user-named items, from every
Explorer navigation pane; dry run first so a pane with nothing to hide is never touched; redraw frozen while
deleting; no registry changes).

Changed:

- **No worker thread, no polling.** The original scans every visible `CabinetWClass` window every 300 ms from a
  thread and only finds the *first* `SysTreeView32` per window (so with tabs only one tab's pane was pruned). Here
  `CreateWindowExW` (user32 export, same hook the single-window-tabs mod uses) catches every navigation pane
  (`SysTreeView32` whose parent is `NamespaceTreeControl` and whose root is `CabinetWClass`) when it is created and
  subclasses it on its own thread. The subclass restarts a 60 ms timer on every `TVM_INSERTITEM`, so a burst of
  inserts (initial fill, F5, folder expansion, the shell re-adding an entry) ends in one prune on the UI thread.
  Panes that already exist at AfterInit are found with `EnumWindows` (this process only) and subclassed through
  `SP_SetWindowSubclassFromAnyThread`, then sent a registered "prune" message.
- **Localized names instead of `homeText` / `galleryText` / `onedriveText` settings.** The names of Home and
  Gallery are read from the shell (`SHCreateItemFromParsingName("::{CLSID}")` + `SIGDN_NORMALDISPLAY`) on the engine
  thread at load / SettingsChanged, with "Home" / "Gallery" kept as fallbacks. Verified on this machine (Turkish,
  26200.9457): Home resolves to "Giriş"; the Gallery CLSID is not registered here (no Gallery on this install), so
  only the English fallback is in the list for it. OneDrive is matched with "contains OneDrive" as in the original.
  Only `SHCreateItemFromParsingName` is used, so an item that does not exist simply yields no rule.
- **`customEnabled` / `customText` / `customList` / `matchMode` / `caseSensitive` collapsed into one `CustomLabels`
  string.** Separator is `;` only (the original also took `,` and newlines); match mode is expressed per label with
  `*` (prefix / contains) instead of a global mode; matching is always case-insensitive (the original's
  `caseSensitive` option was dropped: nobody needs two nav-pane entries that differ only by case).
- **`timing.scanIntervalMs` / `timing.initialDelayMs` dropped.** There is no scan; the settle timer (60 ms after
  the last insert) replaces both.
- **Scroll handling.** The original selects the root item (`TVM_SELECTITEM` with `TVGN_CARET`) and forces
  `WM_VSCROLL SB_TOP` after every deletion, which moves the pane's selection highlight. The port remembers the
  item at the top of the view before deleting and restores it with `TVGN_FIRSTVISIBLE` (scroll only, no selection
  change, so the shell never navigates); if that item was among the deleted ones it scrolls to the first item.
  Same visible result at first open (no jump to the bottom), no caret side effect.
- **Recursion budget.** At most 4000 items are examined per prune (each text read goes through the shell's
  `TVN_GETDISPINFO`), so a huge expanded tree can never stall the pane; the original had no limit.
- **Unload.** BeforeUninit sends each pane a registered "detach" message (SendMessageTimeout 2 s) so the timer is
  killed and the subclass removed on the pane's own thread; a pane that does not answer is unhooked with
  `SP_RemoveWindowSubclassFromAnyThread`. As with the original, entries already deleted only come back when the
  window is reopened (the shell does not rebuild the pane's roots on F5 as far as I can tell; see step 6 of
  verification, worth confirming once).

Behavioural limits worth knowing (same as the original):

- Turning a hide option *off* (or disabling the mod) does not restore an entry in an open window; reopen it.
- Only Explorer frames of the shell process are affected. A folder opened with `explorer.exe <path>` from another
  process runs in its own explorer.exe (verified here: the shell is pid 6620, a spawned window landed in pid 11760)
  and is not seen.
- File dialogs' navigation panes are deliberately not touched (their root window is a dialog, not `CabinetWClass`).
- The pane's own selection is lost if the current folder's entry is deleted (e.g. the window is showing Home);
  the view itself is unaffected.

## 6. How to verify on a live shell

Note for this machine: the navigation pane currently shows one root "Masaüstü" with Masaüstü, İndirilenler,
Belgeler, Resimler, Müzikler, Videolar, Geri Dönüşüm Kutusu, Bu bilgisayar, Linux beneath it, and no Home /
Gallery / OneDrive entries (they are already gone or not installed). So the built-in switches will not show a
difference here; use `CustomLabels` to see the mechanism work, and test Home/Gallery on a stock install.

1. Enable the mod (`Enabled=1`), set `Logging=2` under `HKCU\Software\ShadePatcher`. The log should show
   `CreateWindowExW hooked; navigation panes are pruned as they are filled`, one `Hiding home=1 gallery=1
   onedrive=1, custom labels "": N rule(s)` line, and `<n> existing navigation pane(s) watched`.
2. Set `CustomLabels` to `Linux;Geri Dönüşüm Kutusu` (on an English system: `Linux;Recycle Bin`). Open a new
   Explorer window from the taskbar / Win+E: the pane must appear without those two entries and without a visible
   flash or a jump to the bottom; the log shows `Removed 2 item(s) from navigation pane ...`.
3. With that window open, change `CustomLabels` to add `Bu bilgisayar*` (or `This PC*`): the entry disappears
   from the open window within a moment (SettingsChanged posts a prune to every watched pane), no restart.
4. Press F5 in the window and expand/collapse folders in the pane: hidden entries must not reappear; other
   entries expand normally; clicking entries navigates as usual.
5. Open a second tab (Ctrl+T): the new tab's pane is pruned too (the original only handled the first tree per
   window).
6. Clear `CustomLabels`: nothing comes back in the open window (expected); a newly opened window shows every
   entry again. (Also try F5 in the old window: if the entries come back, the shell does rebuild the pane on
   refresh and the "reopen the window" note in 1465 can be softened.)
7. On a stock English/Turkish install with Home and Gallery present: enable the mod with defaults; a new window
   must open without Home ("Giriş") and Gallery ("Galeri"); with OneDrive signed in, "OneDrive - Personal" is
   gone too. Set `HideHome=0`: a *new* window shows Home again.
8. Set `Enabled=0` without unloading (the mod reads it on SettingsChanged): the log shows no further prunes; open a
   new window: all entries present. Set `Enabled=1` again: pruning resumes at once for every open pane (panes are
   subclassed even while disabled, only the engine's unload detaches them) and for new windows.
9. Regression check for the CreateWindowExW hook: open a file dialog (e.g. Notepad > Open, which runs in Notepad's
   process, and the shell's own "Select folder" dialogs such as a pinned-folder properties page): their panes are
   untouched. Desktop right-click menus, taskbar, Start all behave as before.
