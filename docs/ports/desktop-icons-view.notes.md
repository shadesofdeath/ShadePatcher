# desktop-icons-view port notes

Source file: `src/core/mods/desktop_icons_view.cpp`
Windhawk original: `desktop-icons-view` v1.0.2 by m417z (based on deanm's "List view desktop")

## 1. SP_MOD_DECLARE symbol

`g_modDesktopIconsView`

```c
SP_MOD_DECLARE(g_modDesktopIconsView);
```

## 2. Exception handling

No. Plain Win32 only (user32 / comctl32 messages), no C++/WinRT, nothing throws. The file compiles with the
engine's default (exceptions off). No extra libraries: `ListView_*` is not used, the LVM_* messages are sent
directly, so no `Comctl32.lib` pragma is needed either.

## 3. Settings read

Key: `HKCU\Software\ShadePatcher\Mods\desktop-icons-view`

| Name          | Type  | Default | Meaning |
|---------------|-------|---------|---------|
| `Enabled`     | dword | 0       | Read by the engine, not the mod. Off = the mod is not loaded and the desktop is untouched. |
| `View`        | dword | 3       | The list view mode, as the Win32 `LV_VIEW_*` value. Allowed: `1` = Details (LV_VIEW_DETAILS), `2` = Small icons (LV_VIEW_SMALLICON), `3` = List (LV_VIEW_LIST), `4` = Tiles (LV_VIEW_TILE). Anything else falls back to 3. `0` (large icons) is deliberately not offered: that is what turning the mod off gives. |
| `ColumnWidth` | dword | 500     | Width in pixels of the name column in the List and Details views (Windhawk's `colwidth`). Clamped to 50..4000, out-of-range falls back to 500. Optional in the GUI; the mod works with the default if no row is added. |

Both are re-read and applied live in `SettingsChanged`; no shell restart is needed.

## 4. Proposed settings.reg lines and strings

Goes in the desktop group (`;a %R:1104%`), after `desktop-toggle-icons`:

```
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\desktop-icons-view]
;b %R:1520%
"Enabled"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\desktop-icons-view]
;c 4 %R:1521%
;x 3 %R:1522%
;x 1 %R:1523%
;x 2 %R:1524%
;x 4 %R:1525%
"View"=dword:00000003
```

Optional third row (only if the integrator wants the column width in the GUI; the mod reads the value either
way). Uses the remaining ids 1526-1529:

```
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\desktop-icons-view]
;c 3 %R:1526%
;x 300 %R:1527%
;x 500 %R:1528%
;x 800 %R:1529%
"ColumnWidth"=dword:000001f4
```

Suggested `strings.h` names and texts:

| Id   | Define                          | English                                                              | Turkish |
|------|---------------------------------|----------------------------------------------------------------------|---------|
| 1520 | `IDS_MOD_DESKTOPVIEW`           | Show the desktop icons as a list, details, small icons or tiles      | Masaüstü simgelerini liste, ayrıntılar, küçük simgeler veya kutucuklar olarak göster |
| 1521 | `IDS_MOD_DESKTOPVIEW_VIEW`      | Desktop icons view                                                   | Masaüstü simge görünümü |
| 1522 | `IDS_MOD_DESKTOPVIEW_LIST`      | List                                                                 | Liste |
| 1523 | `IDS_MOD_DESKTOPVIEW_DETAILS`   | Details                                                              | Ayrıntılar |
| 1524 | `IDS_MOD_DESKTOPVIEW_SMALL`     | Small icons                                                          | Küçük simgeler |
| 1525 | `IDS_MOD_DESKTOPVIEW_TILES`     | Tiles                                                                | Kutucuklar |
| 1526 | `IDS_MOD_DESKTOPVIEW_COLWIDTH`  | Width of the file names (list and details)                           | Dosya adlarının genişliği (liste ve ayrıntılar) |
| 1527 | `IDS_MOD_DESKTOPVIEW_COL_NARROW`| Narrow                                                               | Dar |
| 1528 | `IDS_MOD_DESKTOPVIEW_COL_NORMAL`| Normal                                                               | Normal |
| 1529 | `IDS_MOD_DESKTOPVIEW_COL_WIDE`  | Wide                                                                 | Geniş |

(The Turkish strings above contain non-ASCII characters; that is fine in the .rc language files, which already
do. The mod source itself is ASCII only.)

## 5. What was left out and why

- **`monitor` setting (which monitor holds the desktop list).** Dropped as a per-monitor extra. While a
  non-icon view is on, the list is confined to the work area of the *primary* monitor
  (`MonitorFromPoint({0,0}, MONITOR_DEFAULTTOPRIMARY)`), which is what Windhawk's default `monitor: 1`
  produces on practically every setup. Windows itself puts the desktop icons on the primary monitor, so this
  is what users expect.
- **`Wh_ModSettingsChanged` -> full reload.** Windhawk asks for a mod reload on every settings change. Here
  `SettingsChanged` re-reads the values and re-applies the view to the live list directly.
- **Restoring by repositioning to the work area.** Windhawk's uninit puts the list back into large icons but
  leaves it sized to one monitor's work area, so icons on other monitors are stranded until the shell rebuilds
  the desktop. This port records the original `LVS_NOSCROLL` and `LVS_EX_DOUBLEBUFFER` state the first time
  it touches the list, and on `BeforeUninit` restores both and stretches the list back over its parent's whole
  client area (the shell's own layout, spanning every monitor).
- **Remembering the HWND from the hook for the timer.** The timer looks the desktop list up afresh when it
  fires instead of using a static HWND; the window can be destroyed and recreated within the second, and the
  lookup proves the target is still the desktop.
- **Column width as a free number.** Windhawk exposes an arbitrary integer. The registry value still is one
  (any 50..4000 works), but the GUI row, if added, offers three presets since the settings window has no
  numeric field.

Behavioural parity kept: same `CreateWindowExW` hook and recognition (SysListView32 "FolderView" -> unnamed
SHELLDLL_DefView -> Progman / shell window), same one-second settle delay on the creating thread, same style
dance (`LVS_NOSCROLL` off, `LVM_SETVIEW`, double buffering off, column width, position to work area).

Known limitation shared with the original: the shell sometimes puts the list back to large icons on its own
(e.g. the user picks View > Medium icons from the desktop context menu, or some display changes that do not
recreate the window). Toggling any setting (or the mod) re-applies the view; a shell rebuild goes through the
hook automatically.

## 6. How to verify on a live shell

1. Enable the mod in the settings window (default View = List). Within a moment the desktop icons should
   re-flow into vertical columns of small icons with names, starting at the top-left of the primary monitor
   and staying above the taskbar. On a multi-monitor setup the list should no longer span secondary monitors.
2. Change the view dropdown:
   - **Details**: a single "Name" column with a header row; the filenames are one per line.
   - **Small icons**: small icons laid out left-to-right in rows.
   - **Tiles**: medium icons with the name (and type/size for files) to the right.
   Each change applies immediately, no restart.
3. If the optional column width row is added: switch between Narrow / Normal / Wide in List or Details view;
   long filenames should be cut off earlier or later.
4. Double-click a file or folder in the new view: it opens as before. Right-click the desktop: the normal
   desktop menu still appears. Drag an icon: in list/details the shell arranges items, so dragging reorders
   rather than places (expected, same as the original).
5. Disable the mod: the desktop returns to large icons spanning the full desktop. If icon positions look
   rearranged, press F5 on the desktop; the shell restores its saved positions.
6. Restart the shell (Task Manager > restart Windows Explorer) with the mod on: after the desktop comes back,
   about one second later the chosen view is applied on its own (this exercises the `CreateWindowExW` hook +
   timer path rather than `AfterInit`).
7. With `Logging=1` under `HKCU\Software\ShadePatcher`, DbgView shows lines tagged `desktop-icons-view`:
   "View is 3, column width 500", "Desktop list 0x... is now in view 3", and on a shell restart
   "The desktop list was created: ...".

## Integrator checklist

- `mod_table.c`: `SP_MOD_DECLARE(g_modDesktopIconsView);` and an entry in the table.
- `core.vcxproj`: `<ClCompile Include="mods\desktop_icons_view.cpp" />` in the Mods group. No
  `ExceptionHandling` override.
- `settings.reg` / `strings.h` / `gui.en-US.rc` / `gui.tr-TR.rc`: ids 1520-1525 required, 1526-1529 only
  with the optional column width row.
- No new libraries and no new shared headers.
- Note for the engine: the mod hooks the `user32.dll` export `CreateWindowExW`, one of the hottest functions
  in the process. The hook body rejects non-candidates on three pointer checks before calling any API, so the
  cost is negligible, but if another mod ever hooks the same export both go through the same transaction
  machinery as usual.
