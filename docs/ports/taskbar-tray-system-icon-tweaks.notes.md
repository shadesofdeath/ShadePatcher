# Port notes: taskbar-tray-system-icon-tweaks

Source file: `src/core/mods/taskbar_tray_system_icon_tweaks.cpp`
Original: Windhawk mod `taskbar-tray-system-icon-tweaks` v1.3 by m417z (credited in `basedOn` / `originalAuthor`).

## 1. Declaration

```c
SP_MOD_DECLARE(g_modTaskbarTraySystemIconTweaks);
```

Mod id: `taskbar-tray-system-icon-tweaks`. `minOsBuild` 22000, `targets` SP_TARGET_EXPLORER, `flags` 0.

## 2. Exception handling

**Yes**: the file uses C++/WinRT (Windows.UI.Xaml, Windows.UI.Core), so its `ClCompile` entry in `core.vcxproj`
needs `<ExceptionHandling>Sync</ExceptionHandling>`, like `taskbar_menu_entry.cpp`. Every hook body, event
handler and dispatched callback is wrapped in try/catch. No new link dependencies (WinRT resolves through
`runtimeobject.lib`, already linked; the module version is read from the resource block without `version.lib`;
taskbar.dll and SystemTray.dll are reached through symbols only). One function (`ModuleImageSize`) uses
`__try/__except`; it holds no C++ objects, so it compiles under /EHsc.

## 3. Settings (HKCU\Software\ShadePatcher\Mods\taskbar-tray-system-icon-tweaks)

All dword, all re-read on `SettingsChanged` and applied to the live taskbar without a restart.

| Name | Type | Default | Meaning |
|------|------|---------|---------|
| `Enabled` | dword | 0 | Engine toggle. |
| `HideVolume` | dword | 0 | 1 = hide the volume icon in the quick settings button. |
| `HideNetwork` | dword | 0 | 1 = hide the network (Wi-Fi / Ethernet / airplane) icon. |
| `HideBattery` | dword | 0 | 1 = hide the battery icon. |
| `HideMicrophone` | dword | 0 | 1 = hide the "an app is using your microphone" icon. |
| `HideLocation` | dword | 0 | 1 = hide the "an app is using your location" icon. The combined microphone-and-location glyph is hidden only when both `HideMicrophone` and `HideLocation` are 1. |
| `HideLanguageBar` | dword | 0 | 1 = hide the language bar icon (ENG / TR ...). Input-method helper icons next to it (half/full width, hiragana ...) are not touched. |
| `BellMode` | dword | 0 | 0 = never hide the bell; 1 = hide the bell while there are no new notifications (empty bell, with or without "do not disturb"); 2 = always hide the bell. Out-of-range values read as 0. |
| `HideShowDesktopButton` | dword | 0 | 1 = hide the "Show desktop" strip at the far end of the taskbar (its width is pinned to 0). Wins over `ShowDesktopButtonWidth`. |
| `ShowDesktopButtonWidth` | dword | 0 | Width of the "Show desktop" strip in XAML pixels; 0 = leave the shell's own width alone. Values outside 0..200 read as 0. |

When the quick settings button has all three of its icons hidden the whole strip collapses (the button is no
longer clickable there); the volume/network/battery flyout can still be reached with Win+A.

## 4. Proposed settings.reg lines

Goes under the taskbar heading of the Mods page. Ids 1600-1613 used, 1614-1619 free.

```
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-tray-system-icon-tweaks]
;b %R:1600%
"Enabled"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-tray-system-icon-tweaks]
;b %R:1601%
"HideVolume"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-tray-system-icon-tweaks]
;b %R:1602%
"HideNetwork"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-tray-system-icon-tweaks]
;b %R:1603%
"HideBattery"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-tray-system-icon-tweaks]
;b %R:1604%
"HideMicrophone"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-tray-system-icon-tweaks]
;b %R:1605%
"HideLocation"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-tray-system-icon-tweaks]
;b %R:1606%
"HideLanguageBar"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-tray-system-icon-tweaks]
;c 3 %R:1607%
;x 0 %R:1608%
;x 1 %R:1609%
;x 2 %R:1610%
"BellMode"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-tray-system-icon-tweaks]
;b %R:1611%
"HideShowDesktopButton"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-tray-system-icon-tweaks]
;c 7 %R:1612%
;x 0 %R:1613%
;x 4 4
;x 8 8
;x 12 12
;x 16 16
;x 24 24
;x 32 32
"ShowDesktopButtonWidth"=dword:00000000
```

Notes for the integrator:
- `ShowDesktopButtonWidth` is offered as a `;c` list (same approach as the clock mod's FontSize) so no numeric
  input control is needed; the mod accepts any 0..200 if the GUI grows a number box. The `;x 4 4` style lines
  use the number as its own label, as in the clock notes. 12 is roughly the stock width.
- Choice items for `BellMode`: 0 = 1608 (Never), 1 = 1609 (When there are no new notifications), 2 = 1610
  (Always).
- Choice items for `ShowDesktopButtonWidth`: 0 = 1613 (Default), then 4, 8, 12, 16, 24, 32 literal.

### String ids and texts

| Id | English | Turkish |
|----|---------|---------|
| 1600 | Hide system icons in the taskbar tray (volume, network, battery, bell ...) | Görev çubuğu tepsisindeki sistem simgelerini gizle (ses, ağ, pil, zil ...) |
| 1601 | Hide the volume icon | Ses simgesini gizle |
| 1602 | Hide the network icon | Ağ simgesini gizle |
| 1603 | Hide the battery icon | Pil simgesini gizle |
| 1604 | Hide the microphone icon | Mikrofon simgesini gizle |
| 1605 | Hide the location icon | Konum simgesini gizle |
| 1606 | Hide the language bar | Dil çubuğunu gizle |
| 1607 | Hide the notification bell | Bildirim zilini gizle |
| 1608 | Never | Hiçbir zaman |
| 1609 | When there are no new notifications | Yeni bildirim yokken |
| 1610 | Always | Her zaman |
| 1611 | Hide the "Show desktop" button | "Masaüstünü göster" düğmesini gizle |
| 1612 | "Show desktop" button width | "Masaüstünü göster" düğmesinin genişliği |
| 1613 | Default | Varsayılan |

Suggested `strings.h` names: `IDS_MOD_TRAYICONS`, `IDS_MOD_TRAYICONS_VOLUME`, `IDS_MOD_TRAYICONS_NETWORK`,
`IDS_MOD_TRAYICONS_BATTERY`, `IDS_MOD_TRAYICONS_MIC`, `IDS_MOD_TRAYICONS_LOCATION`, `IDS_MOD_TRAYICONS_LANGBAR`,
`IDS_MOD_TRAYICONS_BELL`, `IDS_MOD_TRAYICONS_BELL_NEVER`, `IDS_MOD_TRAYICONS_BELL_WHENEMPTY`,
`IDS_MOD_TRAYICONS_BELL_ALWAYS`, `IDS_MOD_TRAYICONS_SHOWDESKTOP_HIDE`, `IDS_MOD_TRAYICONS_SHOWDESKTOP_WIDTH`,
`IDS_MOD_TRAYICONS_SHOWDESKTOP_DEFAULT`.

## 5. What was left out and why

- **Grayscale battery icon** (`grayscaleBatteryIcon`): out of scope; it clears and re-clears the Foreground
  brush of the battery TextBlocks and needs its own restore bookkeeping. Could be added later as one more bool.
- **Studio Effects and Recall icons** (`hideStudioEffectsIcon`, `hideRecallIcon`): out of scope. Their glyphs
  are still recognised (so they are logged by kind) but never hidden.
- **Language supplementary icons** (`hideLanguageSupplementaryIcons`): out of scope. `HideLanguageBar` hides
  only the main language icon (`SystemTray.LanguageTextIconContent` / `LanguageImageIconContent`); helper
  icons (`TextIconContent` with an IME glyph, `ImageIconContent`) are always shown.
- **Bell mode "when there are no new notifications and Do not disturb is off"** (`whenInactiveAndNoDnd`): the
  brief asks for three modes. Mode 1 hides both empty-bell glyphs (with and without DND), which is the
  original's `whenInactive`.
- **`showDesktopButtonWidth` as the hide switch**: the original hides the strip by setting its width to 0. The
  brief asks for a separate `HideShowDesktopButton` bool; the mod maps it to width 0 and treats
  `ShowDesktopButtonWidth` 0 as "default" (values cleared), so a fresh install changes nothing. The original's
  default of 12 is not applied automatically.
- **Windows 10 taskbar / ExplorerPatcher**: not supported; Windows 11 XAML taskbar only.
- **`LoadLibraryExW` hook** to catch the tray module loading: replaced by `SP_WaitForModule`.
- **`RunFromWindowThread` (WH_CALLWNDPROC + SendMessage)**: replaced by the frame element's `CoreDispatcher`,
  found through the same taskbar.dll symbols (resolve-only), with a 3 s wait per taskbar. Secondary taskbars
  (`Shell_SecondaryTrayWnd`, via `CSecondaryTaskBand`) are included; the original only handled the primary.
- **Single watched main-stack icon**: the original registers the glyph-changed callback for the first
  microphone/location icon only. The port registers one per icon and keeps them in a list, so several
  main-stack icons are all followed.
- **Revoking pending Loaded handlers on every settings pass**: the original clears its revoker list at each
  `ApplySettings`, which drops icons constructed but not yet loaded at that moment. The port revokes pending
  handlers only on unload; a settings pass leaves them in place so those icons are still styled when they load.

Everything else is ported 1:1: the glyph tables, the five container rules (MainStack, NonActivatableStack,
ControlCenterButton, NotificationCenterButton, ShowDesktopStack), the "collapse the quick settings strip when
nothing is enabled in it" rule, the bell's MaxWidth=0 on the presenter when the clock is hidden, the bell retry
through the dispatcher, the empty-element fix for the language bar, and the Min/MaxWidth pinning of the "Show
desktop" strip and its stack.

## 6. Things the integrator must know

- **Module choice**: waits for `Taskbar.View.dll` (60 s). In that callback it hooks `SystemTray.dll` if loaded;
  otherwise, if Taskbar.View.dll's major version is below 2604, it hooks Taskbar.View.dll; otherwise it issues
  a nested `SP_WaitForModule(L"SystemTray.dll")`. On this machine (26200, Taskbar.View.dll 2607) the hook lands
  in SystemTray.dll; the symbol engine needs its PDB (already cached if the volume/clock/icon-size mods ran).
- **One symbol hook**, required: `public: __cdecl winrt::SystemTray::implementation::IconView::IconView(void)`.
  If it is missing the mod logs "IconView::IconView was not found in <module>; no tray icon will be hidden"
  and does nothing else (Init still returns TRUE because the wait was set up).
- **taskbar.dll**: six symbols resolved only (`CTaskBand` / `CSecondaryTaskBand` ITaskListWndSite vftables,
  both `GetTaskbarHost`, `TaskbarHost::FrameHeight`, `std::_Ref_count_base::_Decref`), same set as the start
  button mod. A miss is logged and not fatal: settings changes then reach the tray through one remembered icon
  per taskbar thread (taken from the constructor hook), which covers everything except "turn the mod on while
  the taskbar is already up and no icon has been created since".
- **Constructor hook layout check**: the element is taken from the second pointer of the implementation object
  (`((IUnknown**)pThis)[1]`, the composed inner XAML object), as the original does. The port first checks that
  the pointer does not fall inside the tray module's image (which would make it a vtable, i.e. an unexpected
  layout) before calling QueryInterface on it; in that case the icon is left alone rather than risking the shell.
- **Property watchers**: `TextBlock.Text` on each main-stack glyph and `AutomationProperties.Name` on the bell
  are registered with `RegisterPropertyChangedCallback` and remembered; each pass unregisters the ones on its
  thread and re-registers what the rules still need. On unload they all come off before the hooks go.
- **Unload**: `BeforeUninit` sets `g_unloading`, then runs a pass on every reachable taskbar: every rule
  restores (visibility Visible, IsEnabled true, strip visible, presenter MaxWidth cleared, show desktop
  Min/MaxWidth cleared), watchers and pending Loaded handlers are revoked. `Uninit` waits up to 2 s for
  callbacks still queued on the dispatchers.
- `Init` resets all state, so turning the mod off and on without a shell restart works.
- With `Logging`=2 the log shows one line per icon met ("Quick settings icon U+E995 kind 2: hide=1", "Bell
  U+F2A3 kind 8: hide=1", "Show desktop button: width 0" ...), and the "Tray icon hook installed in
  SystemTray.dll (IconView::IconView: yes; taskbar.dll route: yes)" line says which routes are available.

## 7. Verifying on a live shell

1. Turn the mod on with all options at their defaults: nothing in the tray should change.
2. Set `HideVolume` = 1: the speaker glyph disappears from the quick settings button at once, the network (and
   battery) glyph stays and the button still opens quick settings. Set `HideNetwork` = 1 (and `HideBattery` on
   a laptop): with every glyph hidden the whole quick settings strip collapses; Win+A still opens the flyout.
   Turn them off again: the glyphs and the strip come back without a restart.
3. Set `BellMode` = 1: with no new notifications the bell vanishes (the clock moves to the edge). Trigger a
   notification (e.g. `powershell -c "[reflection.assembly]::loadwithpartialname('System.Windows.Forms');
   $n=new-object System.Windows.Forms.NotifyIcon; $n.Icon=[System.Drawing.SystemIcons]::Information;
   $n.Visible=$true; $n.ShowBalloonTip(3000,'t','m',[System.Windows.Forms.ToolTipIcon]::Info)"` or any app
   notification): the full bell appears; open and dismiss the notification centre so the bell empties: it hides
   again. Set `BellMode` = 2: the bell is gone regardless. Also try with the clock hidden (Settings >
   Time & language > Date & time > "Show time and date in the System tray" off): the bell's slot should take no
   room when hidden.
4. Start a call app or a voice recorder so the microphone icon shows in the tray, set `HideMicrophone` = 1:
   the icon disappears; while it is still recording, open Maps or anything using location so the glyph turns
   into the combined microphone+location one: it reappears unless `HideLocation` is 1 too.
5. With two input languages installed (so the ENG/TR box shows), set `HideLanguageBar` = 1: the language box
   goes; Win+Space still switches layouts. Off again: it returns.
6. Set `ShowDesktopButtonWidth` = 32: the strip at the far right grows to a visible sliver (hover shows the
   desktop peek as usual). Set `HideShowDesktopButton` = 1: the strip is gone, the clock reaches the edge.
   Back to 0 / 0: the stock strip returns.
7. On a second monitor the same rules apply to its taskbar.
8. Turn the mod off: everything returns to stock immediately (no explorer restart).

## 2026-09-22 changes

- "Show desktop" width: pinning MinWidth/MaxWidth on the icon and ShowDesktopStack (what the original does) left
  the strip at 12 dip on build 26200 (measured through UI Automation: `SystemTray.ShowDesktopButton` stayed 18 px at
  150 %). The rule now pins Width/MinWidth/MaxWidth on every element from the icon up to the stack, overrides a fixed
  Width on single-child wrappers above it, widens a fixed-width SystemTrayFrameGrid column that holds it, and
  restores the shell's exact local values afterwards instead of clearing them. Verify with width 32: the clock moves
  left by 20 dip.
- A remembered taskbar whose dispatcher is gone (the taskbar was recreated, e.g. on a display change) used to fail
  every pass with "The taskbar's dispatcher refused the callback"; it is now forgotten so its replacement is used.
