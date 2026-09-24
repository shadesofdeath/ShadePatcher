# taskbar-wheel-cycle - port notes

Source written: `src/core/mods/taskbar_wheel_cycle.cpp`
Original: Windhawk mod `taskbar-wheel-cycle` ("Cycle taskbar buttons with mouse wheel") by m417z.

## 1. SP_MOD_DECLARE symbol

```c
SP_MOD_DECLARE(g_modTaskbarWheelCycle);
```

Suggested place in `g_modTable[]`: the "Taskbar" group. `minOsBuild` is 22000, `flags` 0, `targets` SP_TARGET_EXPLORER.

## 2. Exception handling

**Yes**, the file uses C++/WinRT (Windows.UI.Xaml projection: `PointerRoutedEventArgs`, `VisualTreeHelper`,
`get_class_name`) and needs the same `core.vcxproj` entry as `taskbar_menu_entry.cpp`:

```xml
<ClCompile Include="mods\taskbar_wheel_cycle.cpp">
  <ExceptionHandling>Sync</ExceptionHandling>
</ClCompile>
```

Every path XAML calls into is wrapped in try/catch. One function (`ProbeGroupsSlot`) uses SEH `__try/__except`; it
holds only plain int arrays, so it compiles under /EHs without C2712.

## 3. Settings read

All under `HKCU\Software\ShadePatcher\Mods\taskbar-wheel-cycle`, all `dword`, all re-read on `SettingsChanged`
(no restart needed):

| Name                        | Type  | Default | Meaning |
|-----------------------------|-------|---------|---------|
| `SkipMinimized`             | dword | 1       | 1 = minimized windows are stepped over; 0 = they are visited like any other button. |
| `WrapAround`                | dword | 1       | 1 = past the last button the walk continues from the first (and vice versa); 0 = it stops at the edge. |
| `ReverseScrollingDirection` | dword | 0       | 0 = wheel down moves to the right / next button, wheel up to the left / previous; 1 = the opposite. |

Values are booleans (any non-zero is true).

## 4. Proposed settings.reg lines and strings (ids 1330-1339)

settings.reg (Mods page, taskbar heading, next to the other taskbar mods):

```
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-wheel-cycle]
;b %R:1330%
"Enabled"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-wheel-cycle]
;b %R:1331%
"SkipMinimized"=dword:00000001
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-wheel-cycle]
;b %R:1332%
"WrapAround"=dword:00000001
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-wheel-cycle]
;b %R:1333%
"ReverseScrollingDirection"=dword:00000000
```

strings.h:

```c
#define IDS_MOD_WHEELCYCLE              1330
#define IDS_MOD_WHEELCYCLE_SKIPMIN      1331
#define IDS_MOD_WHEELCYCLE_WRAP         1332
#define IDS_MOD_WHEELCYCLE_REVERSE      1333
```

gui.en-US.rc:

```
IDS_MOD_WHEELCYCLE              "Cycle through the taskbar buttons with the mouse wheel"
IDS_MOD_WHEELCYCLE_SKIPMIN      "Skip minimized windows"
IDS_MOD_WHEELCYCLE_WRAP         "Wrap around at the ends"
IDS_MOD_WHEELCYCLE_REVERSE      "Reverse the scrolling direction"
```

gui.tr-TR.rc:

```
IDS_MOD_WHEELCYCLE              "Görev çubuğu düğmeleri arasında fare tekerleğiyle geçiş yap"
IDS_MOD_WHEELCYCLE_SKIPMIN      "Simge durumuna küçültülmüş pencereleri atla"
IDS_MOD_WHEELCYCLE_WRAP         "Sona gelince başa dön"
IDS_MOD_WHEELCYCLE_REVERSE      "Kaydırma yönünü ters çevir"
```

Ids 1334-1339 are unused. If the settings window supports a description line under a mod (`;e`), 1334 could
carry "Scroll over a taskbar button to activate the previous or next window; the empty area and the tray are
left alone." / "Önceki veya sonraki pencereyi etkinleştirmek için bir görev çubuğu düğmesinin üzerinde
kaydırın; boş alan ve bildirim alanı olduğu gibi bırakılır." Optional.

## 5. What was left out and why

- **Keyboard shortcuts (`Alt+[` / `Alt+]`)**: dropped. Not straightforward here: `RegisterHotKey` must be
  called from the thread owning the target window (it fails with ERROR_WINDOW_OF_OTHER_THREAD otherwise), so
  the original hooks `CTaskBand::v_WndProc` to register on WM_CREATE, unregister on WM_DESTROY, and relays a
  private message from the settings thread; on top of that it needs two string settings, a parser for
  "Alt+VK_OEM_4"-style strings with a 170-entry VK table, and per-monitor taskbar lookup for the hotkey. The
  cycling core (`Cycle` / `FindActive` / `OnTaskListWheel` in the file) is shared, so a later port only needs
  the registration plumbing (`SP_SetWindowSubclassFromAnyThread` on MSTaskSwWClass + `RegisterHotKey` from a
  message handled on that thread) and can call `OnTaskListWheel`-equivalent logic with +/-1 steps.
- **`enableMouseWheelCycling`**: redundant with the mod's own Enabled toggle.
- **`customScrollRegions`** (pixel/percent regions along the taskbar): exotic; the XAML hit test below replaces
  it with a fixed and more precise boundary.
- **`oldTaskbarOnWin11`, Windows 10 path, ExplorerPatcher path**: out of scope. The `TrayUI::WndProc` /
  `CSecondaryTray::v_WndProc` WM_MOUSEWHEEL hooks and the `LoadLibraryExW` hook that watched for
  Taskbar.View.dll / ep_taskbar are all gone; `SP_WaitForModule` does the waiting.
- **`CTaskListWnd::_GetTBGroupFromGroup`**: the original resolves it and never uses it.

Added relative to the original: when no activation has been observed yet (mod just loaded), the walk starts
from the button whose window is the foreground window instead of from the edge.

## 6. Boundary with taskbar-volume-control (important for the integrator)

Both mods want `TaskbarFrame::OnPointerWheelChanged` in Taskbar.View.dll. This mod claims a wheel event **only
when the element under the pointer (`PointerRoutedEventArgs.OriginalSource`) is, or is inside, a
`Taskbar.TaskListButton`** (walk up via `VisualTreeHelper::GetParent`, stopping at `Taskbar.TaskbarFrame`).
When it claims one it marks the args Handled and returns S_OK without calling the original. Everything else -
empty taskbar space, Start, search, widgets, task view, the tray, the clock - is passed to the original
function untouched, which is where a second mod's hook (installed on the same symbol) sits in the chain.

Consequences:

- If the volume mod is ported to act on the empty area and/or the tray only, the two coexist with no further
  arbitration: over a button this mod wins, elsewhere this mod is invisible. The hook installation order does
  not matter as long as the volume mod does not claim wheel events over `Taskbar.TaskListButton`.
- If the volume mod is ported with a "whole taskbar" mode and both are enabled, whoever's hook was installed
  last runs first and consumes; the user should not enable that combination. Recommend that the volume port
  excludes `Taskbar.TaskListButton` ancestors the same way (the helper `IsOverTaskListButton` in this file is
  the reference; it is a 20-line function that can be duplicated, since mods share no headers).
- Note that on the XAML path the *original* Windhawk mod actually consumes the wheel over the whole taskbar
  (its Win11 code never checks the task list rect; only the Win10/ExplorerPatcher path does). The port is
  deliberately narrower, per the brief.

## 7. Threading and lifecycle summary

- `Init`: reads settings, registers two `SP_WaitForModule` waits (`taskbar.dll`, `Taskbar.View.dll`, 60 s).
  Returns TRUE immediately.
- `taskbar.dll` callback (helper thread): `SP_HookSymbols` with 10 resolve-only entries (2 CTaskListWnd
  vtables, CImmersiveTaskItem vtable, GetButtonGroupCount, GetGroupType, GetNumItems, GetTaskItem, both
  GetWindow, SwitchToItem) + 1 hook (`CTaskListWnd::_SetActiveItem`). Then probes the group array slot and sets
  `g_modelReady`. Any failure logs an error and leaves the wheel inert (mod stays loaded, does nothing).
- `Taskbar.View.dll` callback: 1 hook (`TaskbarFrame::OnPointerWheelChanged`, the exact Windhawk symbol
  string). The hook passes everything through until `g_modelReady`.
- Hook bodies run on the taskbar UI thread; settings are atomics; the active-item map and wheel remainder are
  under one `std::mutex`.
- `Uninit`: clears the map. Nothing on live windows or XAML is modified, so `BeforeUninit` is null.
- The PDB for taskbar.dll is fetched on the first run (helper thread, not the engine thread).

## 8. How to verify on a live shell

1. Enable the mod, open 4-5 windows (mix in a Store/UWP app such as Settings or Calculator), minimize one.
2. Hover a taskbar button and roll the wheel down: the next window to the right becomes foreground and its
   button shows active; roll up: previous. The activated window comes to the front (the SendInput trick makes
   `SetForegroundWindow` succeed; if instead a button only flashes, that trick is being blocked).
3. With `SkipMinimized`=1 the minimized window's button is stepped over; set it to 0 (no restart) and it is
   restored/activated when reached.
4. `WrapAround`=1: from the last button rolling down lands on the first; =0: it stays on the last.
5. `ReverseScrollingDirection`=1: down now goes left.
6. Boundary: roll the wheel over the empty taskbar area, over Start, and over the clock/tray: nothing happens
   (or the other mod acts, if one is installed). Over any task button it cycles.
7. Grouped buttons (several windows of one app, combined): each wheel notch steps through the windows inside
   the group before moving to the next group.
8. Second monitor: each taskbar cycles its own buttons.
9. With Logging=2, DbgView shows `Wheel over <class>` per event, `N step(s) from group G item I` on a switch,
   and at start-up `Task list model ready; the group array is at slot N` and `Watching the taskbar for the
   wheel`. If the first of those is missing, the taskbar.dll symbols did not resolve on this build.
