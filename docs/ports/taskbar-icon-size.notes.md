# Port notes: taskbar-icon-size

Source: Windhawk mod `taskbar-icon-size` v1.3.10 by m417z ("Taskbar height and icon size").
Written: `src/core/mods/taskbar_icon_size.cpp`. Nothing else was touched.

## 1. Declaration

```c
SP_MOD_DECLARE(g_modTaskbarIconSize);
```

Suggested place in `mod_table.c`: the "Taskbar" group, next to `g_modTrayShowAllIcons`.
`minOsBuild` is 22621 (the symbol names are those of the 22H2+ XAML taskbar; 21H2-only spellings were dropped).

## 2. Exceptions

**Yes**, the file uses C++/WinRT (`Windows.UI.Xaml`) and needs in `core.vcxproj`:

```xml
<ClCompile Include="mods\taskbar_icon_size.cpp">
  <ExceptionHandling>Sync</ExceptionHandling>
</ClCompile>
```

Same reason as `taskbar_menu_entry.cpp`. No new link dependencies: `runtimeobject.lib` is already linked; the
DPI call (`GetDpiForMonitor`) is fetched from shcore.dll at run time and the module version is read from the
version resource by hand, so neither shcore.lib nor version.lib is needed.

## 3. Settings (HKCU\Software\ShadePatcher\Mods\taskbar-icon-size)

| Name                 | Type  | Default | Meaning                                                              |
|----------------------|-------|---------|----------------------------------------------------------------------|
| `Enabled`            | dword | 0       | engine toggle                                                        |
| `TaskbarHeight`      | dword | 48      | height of the taskbar frame and the tray, in logical pixels (2..256) |
| `IconSize`           | dword | 24      | size the taskbar draws application icons at, in pixels (1..128)      |
| `TaskbarButtonWidth` | dword | 44      | width of one taskbar button and of Start/search/widgets (1..512)     |

Out-of-range values are clamped. Choice lists offered in the settings window:

- TaskbarHeight: 34, 40, 44, **48**, 52, 56, 60, 64
- IconSize: 16, 20, **24**, 28, 32, 36, 40 (16 and 32 give crisp icons; Windows ships icons at those sizes)
- TaskbarButtonWidth: 28, 32, 36, 40, **44**, 48, 52, 56

Sensible pairs, from the original's screenshots: height 52 + icon 32; height 34 + icon 16 (+ width 28 for a
compact bar). Any combination is accepted.

The Windhawk `IconSizeSmall` / `TaskbarButtonWidthSmall` settings (the "smaller taskbar buttons" posture,
new in mid-2025 builds) are **not** ported: that posture keeps the stock 16 px icon and 32 px button.

## 4. settings.reg lines and strings (ids 1360-1369)

The `;x` option labels are literal text where they are plain numbers (the GUI only substitutes `%R:` when it
is present), so only the three "(default)" labels need ids.

```
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-icon-size]
;b %R:1360%
"Enabled"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-icon-size]
;c 8 %R:1361%
;x 34 34 px
;x 40 40 px
;x 44 44 px
;x 48 %R:1364%
;x 52 52 px
;x 56 56 px
;x 60 60 px
;x 64 64 px
"TaskbarHeight"=dword:00000030
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-icon-size]
;c 7 %R:1362%
;x 16 16 px
;x 20 20 px
;x 24 %R:1365%
;x 28 28 px
;x 32 32 px
;x 36 36 px
;x 40 40 px
"IconSize"=dword:00000018
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-icon-size]
;c 8 %R:1363%
;x 28 28 px
;x 32 32 px
;x 36 36 px
;x 40 40 px
;x 44 %R:1366%
;x 48 48 px
;x 52 52 px
;x 56 56 px
"TaskbarButtonWidth"=dword:0000002c
```

Put the block under the Taskbar heading (`;a %R:1103%`).

strings.h:

```c
#define IDS_MOD_ICONSIZE                1360
#define IDS_MOD_ICONSIZE_HEIGHT         1361
#define IDS_MOD_ICONSIZE_ICON           1362
#define IDS_MOD_ICONSIZE_WIDTH          1363
#define IDS_MOD_ICONSIZE_HEIGHT_DEFAULT 1364
#define IDS_MOD_ICONSIZE_ICON_DEFAULT   1365
#define IDS_MOD_ICONSIZE_WIDTH_DEFAULT  1366
```

gui.en-US.rc:

```
    IDS_MOD_ICONSIZE                "Taskbar height and icon size"
    IDS_MOD_ICONSIZE_HEIGHT         "Taskbar height"
    IDS_MOD_ICONSIZE_ICON           "Icon size"
    IDS_MOD_ICONSIZE_WIDTH          "Taskbar button width"
    IDS_MOD_ICONSIZE_HEIGHT_DEFAULT "48 px (default)"
    IDS_MOD_ICONSIZE_ICON_DEFAULT   "24 px (default)"
    IDS_MOD_ICONSIZE_WIDTH_DEFAULT  "44 px (default)"
```

gui.tr-TR.rc:

```
    IDS_MOD_ICONSIZE                "Görev çubuğu yüksekliği ve simge boyutu"
    IDS_MOD_ICONSIZE_HEIGHT         "Görev çubuğu yüksekliği"
    IDS_MOD_ICONSIZE_ICON           "Simge boyutu"
    IDS_MOD_ICONSIZE_WIDTH          "Görev çubuğu düğme genişliği"
    IDS_MOD_ICONSIZE_HEIGHT_DEFAULT "48 px (varsayılan)"
    IDS_MOD_ICONSIZE_ICON_DEFAULT   "24 px (varsayılan)"
    IDS_MOD_ICONSIZE_WIDTH_DEFAULT  "44 px (varsayılan)"
```

1367-1369 are unused.

## 5. What was left out, and why

- **Small posture settings** (`IconSizeSmall`, `TaskbarButtonWidthSmall`): out of scope; the small posture
  keeps stock 16 / 32. The `TaskbarSettings::Size` override (small -> regular, as the original does) is kept
  so a user who set "smaller taskbar buttons: always" still gets the customized regular posture.
- **Windows 11 21H2** (`__real@4048000000000000` patch, 21H2 symbol spellings, `TaskbarFrame::Height` is
  still hooked since it is a 22H2 symbol), **ExplorerExtensions.dll** (the ExplorerPatcher host) and
  **ARM64** code paths (`TaskbarConfiguration::UpdateFrameSize` + `event::operator()` tinkering). x64 only.
- **LoadLibraryExW hook** for late modules: replaced by `SP_WaitForModule` (Taskbar.View.dll 60 s,
  SearchUx.UI.dll no timeout since it only loads when the search button is shown, SystemTray.dll 60 s started
  from the Taskbar.View.dll callback once its version says the tray types live there, i.e. >= 2604; this
  machine has 2607). On a cold start the taskbar can be up to 500 ms (the engine's poll interval) before the
  view hooks land; the callback then applies the settings itself, so the taskbar is re-laid out with them.
- **Windhawk's disassembler** (`Wh_Disasm` + regex) for the five private field offsets: replaced by byte
  patterns on the same instructions (the original already used byte patterns for three of them on x64). A
  pattern that does not match leaves that offset 0 and the hook that needs it passes through; the offsets are
  logged at debug level.
- **Every symbol is optional.** The original refuses to load on any missing non-optional symbol; here a
  missing name costs only its hook and is logged, so a future rename degrades rather than disables. Missing
  `TaskbarConfiguration::GetFrameSize` or `TrayUI::_HandleSettingChange` are logged as errors because the
  height cannot be changed / applied live without them.
- `Wh_ModUninit`'s wait loop is capped at 5 s.

Kept: both icon-size paths (dynamic icon scaling, on since KB5044384 and on this machine, and the older
constant-per-posture path with the taskbar.dll icon loader steering, which is the "icon quality" part), the
resource dictionary override for the button width (Taskbar.View.dll and SearchUx.UI.dll), all TaskListButton
posture swaps (padding, badge, overlay, multi-window clip, visual states, progress bar), the Start / search /
widgets button width and the widgets content margins, the secondary taskbar placement (SHAppBarMessage),
`TrayUI::GetMinSize`, the tray frame measure trick, and the live apply (WM_SETTINGCHANGE +
`TrayUI::_StuckTrayChange` + task band 0x452).

## 6. Threading and unload

- Settings are `std::atomic<int>`; hook bodies read them relaxed. Per-call markers are `thread_local`.
- `ApplySettings` is serialised with a mutex because it can run on the engine thread (AfterInit,
  SettingsChanged, BeforeUninit) and on the module-wait thread (first application after the view is hooked).
  It sends WM_SETTINGCHANGE to Shell_TrayWnd and waits up to 10 s for the frame's MeasureOverride to run.
- BeforeUninit sets `g_unloading` (every hook then answers stock numbers) and applies the original height;
  Uninit waits (max 5 s) for a MeasureOverride that may still be inside the hook.
- First run downloads PDBs for taskbar.dll, Taskbar.View.dll, SystemTray.dll and SearchUx.UI.dll (four
  symbol lookups; the engine caches them).

## 7. How to verify on a live shell

1. Enable the mod with defaults (48 / 24 / 44): nothing should change visibly; `Logging=2` shows
   "Dynamic icon scaling: on" and four non-zero offsets in the "Offsets:" line.
2. Set TaskbarHeight 52 and IconSize 32: within a second the taskbar grows by 4 px (clock/tray and the task
   list stay vertically centred, the desktop work area shrinks: maximise a window and check its bottom edge),
   icons are visibly larger and crisp (compare an app with a 32 px icon such as Explorer).
3. Set TaskbarHeight 34, IconSize 16, TaskbarButtonWidth 28: a compact bar; buttons sit closer together,
   Start and the search icon narrow too; a running-app badge (e.g. Teams/Telegram unread count) still shows
   as a badge, not a dot.
4. Change only IconSize (same height): the taskbar still refreshes (it goes through height-1 and back).
5. Open the overflow flyout of the task list (many windows) after changing the width: explorer must not
   restart (this is what the GetMetrics hook prevents).
6. Right-click a button, hover a group: the multi-window strip and the progress indicator (copy a large
   file) keep their proper size.
7. Two monitors: the secondary taskbar takes the same height; with auto-hide it slides fully in and out.
8. Disable the mod: the taskbar returns to 48 / 24 / 44 without a shell restart.
9. Vertical taskbar (if any mod provides one): heights are left alone; only widths/icons change.
