# Port notes: taskbar-notification-icon-spacing

Source: Windhawk mod `taskbar-notification-icon-spacing` v1.3.1 by m417z ("Taskbar tray icon spacing and grid").
Written: `src/core/mods/taskbar_notification_icon_spacing.cpp`. Nothing else was touched.

## 1. Declaration

```c
SP_MOD_DECLARE(g_modTaskbarNotificationIconSpacing);
```

Mod id: `taskbar-notification-icon-spacing`. `minOsBuild` 22000, `targets` SP_TARGET_EXPLORER, `flags` 0.
Suggested place in `mod_table.c`: the Taskbar group, next to `g_modTrayShowAllIcons` / `g_modTaskbarIconSize`.

## 2. Exception handling

**Yes**, the file uses C++/WinRT (Windows.UI.Xaml, Windows.UI.Core) and needs in `core.vcxproj`:

```xml
<ClCompile Include="mods\taskbar_notification_icon_spacing.cpp">
  <ExceptionHandling>Sync</ExceptionHandling>
</ClCompile>
```

Same reason as `taskbar_menu_entry.cpp`. No new link dependencies: `runtimeobject.lib` is already linked, the
module version is read from the version resource by hand (no version.lib), taskbar.dll / SystemTray.dll are
reached through symbols. x64 only (`#error` otherwise), like the other taskbar mods.

## 3. Settings (HKCU\Software\ShadePatcher\Mods\taskbar-notification-icon-spacing)

| Name                    | Type  | Default | Meaning                                                                                  |
|-------------------------|-------|---------|------------------------------------------------------------------------------------------|
| `Enabled`               | dword | 0       | engine toggle                                                                            |
| `NotificationIconWidth` | dword | 32      | width of one tray icon, of the chevron and of the icons next to the clock (1..256)        |
| `NotificationIconRows`  | dword | 1       | rows the tray icons are laid out in; 1 = ordinary single row (1..8)                      |
| `GridArrangement`       | dword | 0       | fill order when rows > 1: 0..4, see below (registry-only unless the integrator exposes it)|
| `OverflowIconWidth`     | dword | 40      | width and height of one icon in the overflow popup behind the chevron (1..256)           |
| `OverflowIconsPerRow`   | dword | 5       | maximum icons per row in the overflow popup (1..64)                                      |

Out-of-range values are clamped. All defaults are the Windows 11 stock values, so enabling the mod with nothing
changed alters nothing visible (the Windhawk default for the tray width is 24; here it is 32 as the brief asks).

`GridArrangement` (only matters with `NotificationIconRows` >= 2; examples with icons A-G and 2 rows):

| Value | Original name                        | Layout                    |
|-------|--------------------------------------|---------------------------|
| 0     | rowFirstLeftToRight (default)        | `A B C D` / `E F G`       |
| 1     | columnFirstTopToBottom               | `A C E G` / `B D F`       |
| 2     | rowFirstBottomRowFirst               | `E F G` / `A B C D`       |
| 3     | columnFirstBottomToTop               | `B D F` / `A C E G`       |
| 4     | columnFirstBottomToTopRightToLeft    | `_ F D B` / `G E C A`     |

Choice lists offered in the settings window:

- NotificationIconWidth: 16, 18, 20, 24, 28, **32**, 36, 40
- NotificationIconRows: **1**, 2, 3
- OverflowIconWidth: 24, 28, 32, 36, **40**, 48
- OverflowIconsPerRow: 3, 4, **5**, 6, 8, 10

All settings are re-read on `SettingsChanged` and applied to the live tray without a restart.

## 4. settings.reg lines and strings (ids 1620-1629)

Plain-number `;x` labels are literal text (the GUI only substitutes `%R:` when it is present), so only the
toggle, the four labels and the four "(default)" option labels need ids.

```
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-notification-icon-spacing]
;b %R:1620%
"Enabled"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-notification-icon-spacing]
;c 8 %R:1621%
;x 16 16 px
;x 18 18 px
;x 20 20 px
;x 24 24 px
;x 28 28 px
;x 32 %R:1622%
;x 36 36 px
;x 40 40 px
"NotificationIconWidth"=dword:00000020
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-notification-icon-spacing]
;c 3 %R:1623%
;x 1 %R:1624%
;x 2 2
;x 3 3
"NotificationIconRows"=dword:00000001
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-notification-icon-spacing]
;c 6 %R:1625%
;x 24 24 px
;x 28 28 px
;x 32 32 px
;x 36 36 px
;x 40 %R:1626%
;x 48 48 px
"OverflowIconWidth"=dword:00000028
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-notification-icon-spacing]
;c 6 %R:1627%
;x 3 3
;x 4 4
;x 5 %R:1628%
;x 6 6
;x 8 8
;x 10 10
"OverflowIconsPerRow"=dword:00000005
```

Put the block under the Taskbar heading (`;a %R:1103%`), next to the tray mods.

`GridArrangement` is read by the mod but **not** in the block above: its five option labels would need five
more ids than are assigned (1629 is the only one left). Either leave it registry-only (rows > 1 then fill
row-first, left to right, which is what the original's screenshots show), or add this optional block with the
label on 1629 and literal English option labels, and localize the options later when ids are available:

```
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-notification-icon-spacing]
;c 5 %R:1629%
;x 0 Row-first, left to right
;x 1 Column-first, top to bottom
;x 2 Row-first, bottom row first
;x 3 Column-first, bottom to top
;x 4 Column-first, bottom to top, right to left
"GridArrangement"=dword:00000000
```

strings.h:

```c
#define IDS_MOD_TRAYSPACING                   1620
#define IDS_MOD_TRAYSPACING_WIDTH             1621
#define IDS_MOD_TRAYSPACING_WIDTH_DEFAULT     1622
#define IDS_MOD_TRAYSPACING_ROWS              1623
#define IDS_MOD_TRAYSPACING_ROWS_DEFAULT      1624
#define IDS_MOD_TRAYSPACING_OVERFLOW          1625
#define IDS_MOD_TRAYSPACING_OVERFLOW_DEFAULT  1626
#define IDS_MOD_TRAYSPACING_PERROW            1627
#define IDS_MOD_TRAYSPACING_PERROW_DEFAULT    1628
#define IDS_MOD_TRAYSPACING_ARRANGEMENT       1629   // only if GridArrangement is exposed
```

gui.en-US.rc:

```
    IDS_MOD_TRAYSPACING                   "Change the spacing of the tray icons and the overflow popup"
    IDS_MOD_TRAYSPACING_WIDTH             "Tray icon width"
    IDS_MOD_TRAYSPACING_WIDTH_DEFAULT     "32 px (default)"
    IDS_MOD_TRAYSPACING_ROWS              "Tray icon rows"
    IDS_MOD_TRAYSPACING_ROWS_DEFAULT      "1 row (default)"
    IDS_MOD_TRAYSPACING_OVERFLOW          "Overflow popup icon width"
    IDS_MOD_TRAYSPACING_OVERFLOW_DEFAULT  "40 px (default)"
    IDS_MOD_TRAYSPACING_PERROW            "Overflow popup icons per row"
    IDS_MOD_TRAYSPACING_PERROW_DEFAULT    "5 (default)"
    IDS_MOD_TRAYSPACING_ARRANGEMENT       "Grid arrangement (with more than one row)"
```

gui.tr-TR.rc:

```
    IDS_MOD_TRAYSPACING                   "Tepsi simgelerinin ve taşma penceresinin aralığını değiştir"
    IDS_MOD_TRAYSPACING_WIDTH             "Tepsi simgesi genişliği"
    IDS_MOD_TRAYSPACING_WIDTH_DEFAULT     "32 px (varsayılan)"
    IDS_MOD_TRAYSPACING_ROWS              "Tepsi simgesi satır sayısı"
    IDS_MOD_TRAYSPACING_ROWS_DEFAULT      "1 satır (varsayılan)"
    IDS_MOD_TRAYSPACING_OVERFLOW          "Taşma penceresi simge genişliği"
    IDS_MOD_TRAYSPACING_OVERFLOW_DEFAULT  "40 px (varsayılan)"
    IDS_MOD_TRAYSPACING_PERROW            "Taşma penceresinde satır başına simge"
    IDS_MOD_TRAYSPACING_PERROW_DEFAULT    "5 (varsayılan)"
    IDS_MOD_TRAYSPACING_ARRANGEMENT       "Izgara düzeni (birden çok satırda)"
```

## 5. What was left out, and why

- **`gridArrangement` as a string choice**: kept in code as a dword 0..4 (`GridArrangement`, default 0) since
  it is a few lines inside the same grid routine, but it is not in the proposed settings block (not enough ids
  for its five option labels; see section 4). Registry-only until the integrator decides.
- **`LoadLibraryExW` hook** for the late tray module: replaced by `SP_WaitForModule`. The mod waits for
  Taskbar.View.dll (60 s); in that callback it hooks SystemTray.dll if it is loaded, hooks Taskbar.View.dll
  itself if its major version is below 2604 (older builds keep the tray types there), and otherwise waits for
  SystemTray.dll (60 s, nested wait). On this machine (Taskbar.View.dll 2607) the hooks land in SystemTray.dll.
- **`RunFromWindowThread` (WH_CALLWNDPROC + SendMessage to Shell_TrayWnd)**: replaced. The taskbar's XAML
  frame is found from the tray window with the same taskbar.dll symbols the original resolves (`CTaskBand`
  `ITaskListWndSite` vftable, `CTaskBand::GetTaskbarHost`, `TaskbarHost::FrameHeight`,
  `std::_Ref_count_base::_Decref`; resolve-only, no hooks, a miss is logged and not fatal) and the styling pass
  is handed to the frame's `CoreDispatcher`, waiting up to 3 s so BeforeUninit's restore is done before the
  hooks go. The overflow popup's grid is dispatched through its own dispatcher (its island may be on another
  thread).
- **The list of Loaded revokers** (`g_autoRevokerList`): each Loaded handler revokes itself with its own token
  and reads the settings at the moment it runs, so there is nothing to clear on a settings change. A handler
  left on an element after the mod is turned off checks `g_active` and returns.
- **ARM64** default-offset path: x64 only.
- `Wh_ModInit` returning FALSE when the tray module's symbols are missing: here Init only fails if the module
  wait cannot be set up. `IconView::IconView` is the one required symbol (nothing can be styled without it, an
  error is logged); `OverflowXamlIslandManager::InitializeIfNeeded` and `StackViewModel::UpdateIconIndexes`
  are optional and their loss is logged ("overflow popup keeps its own layout" / "grid not re-laid out when
  icons come and go").

Small improvement over the original: when the overflow island is created its root grid is usually not loaded
yet; the original then waits for the next opening of the popup before it can set the WrapGrid's numbers. Here a
one-shot Loaded handler on the grid applies them on the first opening.

Kept 1:1: the per-icon styling (MinWidth, zero padding of the content's ContainerGrid, NaN width + width+12
MinWidth for the language indicator), the control center glyph padding formula, the chevron and clock-side
icon stacks (NotifyIconStack, MainStack, NonActivatableStack), the row grid with its even gap, the
TranslateTransform placement and panel width, the overflow WrapGrid ItemWidth/ItemHeight/MaximumRowsOrColumns
plus per-icon MinWidth/Height, and the "stock numbers on unload" restore (32 / 1 row / 40 / 5).

## 6. Things the integrator must know

- Hooks in the tray module (one `SP_HookSymbols` batch, installed from the module-wait callback):
  `winrt::SystemTray::implementation::IconView::IconView(void)` (required),
  `winrt::SystemTray::OverflowXamlIslandManager::InitializeIfNeeded(void)` (optional),
  `winrt::SystemTray::implementation::StackViewModel::UpdateIconIndexes(void)` (optional). Windhawk's exact
  spellings are used. First run downloads the PDB for SystemTray.dll (and taskbar.dll if no other mod has yet).
- taskbar.dll: four symbols resolved only, in Init (loads taskbar.dll with LOAD_LIBRARY_SEARCH_SYSTEM32, which
  is already loaded in the shell anyway).
- Two raw pointer reads inherited from the original and confirmed there on current builds: the IconView
  implementation object's second pointer slot (`((IUnknown**)pThis)[1]`) is the composed inner base and is
  queried for IFrameworkElement; the OverflowXamlIslandManager's sixth pointer slot (`[5]`) is the island's
  root Grid. A layout change in a future SystemTray.dll would hit those first; the log line "A new tray icon
  could not be watched" / "The overflow island's root is not a Grid" is where it would show.
- Threading: settings are `std::atomic<int>`; the two remembered elements (tray StackPanel, overflow root
  grid) are `winrt::weak_ref` behind a mutex; `ApplySettings` is serialised with a mutex because it runs on the
  engine thread (AfterInit, SettingsChanged, BeforeUninit) and on the module-wait thread (first application
  once the hooks are in). Every hook body, Loaded handler and dispatched callback is wrapped in try/catch.
- Unload: BeforeUninit sets `g_unloading` (every styling pass then uses stock numbers) and applies once;
  Uninit clears `g_active`, waits up to 2 s for callbacks still queued on a dispatcher, and drops the weak
  refs. Init resets all state so the mod can be turned off and on again without a shell restart.
- The restore on unload is the original's: stock numbers are *applied*, not cleared (MinWidth 32, padding 0 on
  the icon content grids, control center padding 4). It looks like stock; a shell restart makes it exact.
- Only the primary taskbar is styled; secondary taskbars have no notification area on Windows 11.
- Enabled with defaults the mod changes nothing visible; the log shows "Tray icon hooks installed in
  SystemTray.dll (overflow popup: yes, grid refresh: yes)" and one "Applying: ..." line per pass.

## 7. How to verify on a live shell

1. Enable the mod with defaults (32 / 1 / 40 / 5): nothing should change. With `Logging=2` the log has the
   "Tray icon hooks installed in SystemTray.dll" line and one "Tray icon ...: MinWidth 32" line per icon.
2. Set NotificationIconWidth to 24, then 18: within a moment the tray icons move closer together (the whole
   tray shrinks towards the clock), the chevron and the icons next to the clock (network / volume / battery,
   the input indicator) follow. The control center glyphs keep a small gap between them. The language
   indicator ("ENG") stays readable, just narrower.
3. Set it to 40: the icons spread out.
4. Click the chevron: the overflow popup shows icons 40 px apart, 5 per row. Set OverflowIconWidth 32 and
   OverflowIconsPerRow 8, reopen the popup (or leave it open): the icons are tighter and the rows are 8 wide.
5. Set NotificationIconRows to 2 with width 18-24: the tray icons form two rows inside the taskbar height,
   row-first (first half of the icons on top). Start an app with a tray icon (or exit one): the grid re-flows
   (this is the UpdateIconIndexes hook). Set GridArrangement 1 in the registry: the order becomes
   column-first. Set rows back to 1: the single row comes back, no leftover transforms.
6. Change any value while the overflow popup is open: it updates in place.
7. Disable the mod: the tray returns to 32 px icons in one row and the overflow to 40 / 5 without a shell
   restart. Re-enable: the settings apply again at once.
8. Sign out and in with the mod on (cold start): the tray comes up already styled; the log shows the
   Taskbar.View.dll wait, then "waiting for SystemTray.dll" (if it loads later), then the hook line.
