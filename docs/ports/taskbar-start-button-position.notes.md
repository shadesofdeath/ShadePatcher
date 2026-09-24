# taskbar-start-button-position - port notes

Source file: `src/core/mods/taskbar_start_button_position.cpp`
Original: Windhawk `taskbar-start-button-position` v1.3.2 by m417z (`basedOn` / `originalAuthor` set).

## 1. SP_MOD_DECLARE symbol

```c
SP_MOD_DECLARE(g_modTaskbarStartButtonPosition);
```

Add it to `mod_table.c` and to the table. Mod id: `taskbar-start-button-position`. `minOsBuild` 22000,
`targets` SP_TARGET_EXPLORER, `flags` 0.

## 2. Exception handling

**Yes**, the file needs `<ExceptionHandling>Sync</ExceptionHandling>` in `core.vcxproj`, the same way
`taskbar_menu_entry.cpp` and `desktop_menu_entry.cpp` have it: it uses C++/WinRT (Windows.UI.Xaml,
Windows.UI.Core). Every hook body and every dispatched callback is wrapped in try/catch.

No new link dependencies: `dwmapi` is reached through `GetProcAddress` and the export hook; taskbar.dll and
Taskbar.View.dll through symbols. Only the SDK's own C++/WinRT headers are used (no WinUI 2 / WinUI 3 projection;
see section 5).

## 3. Settings read

Registry key: `HKCU\Software\ShadePatcher\Mods\taskbar-start-button-position`

| Name                      | Type  | Default | Meaning |
|---------------------------|-------|---------|---------|
| `SearchAndTaskViewOnLeft` | dword | 0       | 0: only the Start button is pinned left. 1: the search button/box and the task view button are pinned left too, in that order after Start, leaving only the app icons centered. (Original: `otherSystemButtonsOnTheLeft`.) |
| `StartMenuOnLeft`         | dword | 1       | 1: the Start button's context menu (Win+X / right-click on Start) aligns to the button's leading edge instead of being centered on it, and the search flyout (SearchHost.exe) is moved to the left edge of the work area when it opens while the Start menu is showing. 0: neither is touched. (Original: `startMenuOnTheLeft`; see section 5 for what part of it could not be ported.) |

Both are plain booleans (0/1). Both are re-read on `SettingsChanged` and applied to the live taskbar without a
restart.

## 4. Proposed settings.reg lines and strings

Place under the Taskbar heading (`;a %R:1103%`), after the tray mods. Ids assigned: 1380-1382 (1383-1389 free).

```
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-start-button-position]
;b %R:1380%
"Enabled"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-start-button-position]
;b %R:1381%
"SearchAndTaskViewOnLeft"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-start-button-position]
;b %R:1382%
"StartMenuOnLeft"=dword:00000001
```

strings.h:

```c
#define IDS_MOD_STARTBUTTONPOS          1380
#define IDS_MOD_STARTBUTTONPOS_OTHERS   1381
#define IDS_MOD_STARTBUTTONPOS_MENUS    1382
```

gui.en-US.rc:

```
    IDS_MOD_STARTBUTTONPOS          "Keep the Start button on the left while the other icons stay centered"
    IDS_MOD_STARTBUTTONPOS_OTHERS   "Also move the search and task view buttons to the left"
    IDS_MOD_STARTBUTTONPOS_MENUS    "Open the Start button menu and the search flyout at the left edge too"
```

gui.tr-TR.rc:

```
    IDS_MOD_STARTBUTTONPOS          "Diğer simgeler ortada kalırken Başlat düğmesini solda tut"
    IDS_MOD_STARTBUTTONPOS_OTHERS   "Arama ve görev görünümü düğmelerini de sola taşı"
    IDS_MOD_STARTBUTTONPOS_MENUS    "Başlat düğmesi menüsünü ve arama panelini de sol kenarda aç"
```

If the integrator prefers not to expose `StartMenuOnLeft` (1382), the mod still reads it with default 1; the
line can simply be left out of settings.reg.

## 5. What was left out, and why

- **The StartMenuExperienceHost.exe half.** The original also injects into StartMenuExperienceHost.exe and
  moves the Start menu itself (Canvas.Left on `StartDocked.StartSizingFrame`, or `HorizontalAlignment` of the
  redesigned menu's `FrameRoot`) so it opens above the pinned button. The engine only runs inside explorer.exe,
  so that cannot be ported. Result: with centered taskbar icons the Start menu still opens centered, as Windows
  places it; only the button, the Win+X menu and the search flyout move. `StartMenuOnLeft` therefore controls
  just those two explorer-side effects. If the engine ever grows a StartMenuExperienceHost target, the original's
  `StartMenuUI` namespace is the part to port next.
- **`searchMenuPositionInAllCases`** (default off in the original). Dropped; the search flyout is only moved when
  it opens while the Start menu is showing, which is the original's default behaviour. Win+S or the taskbar
  search button open it where Windows puts it.
- **`ExplorerExtensions.dll`** as an alternative home for the taskbar XAML (pre-22621 builds). Only
  `Taskbar.View.dll` is waited for, per the brief.
- **ARM64** byte pattern for `TaskbarHost::FrameHeight`. x64 only (`#error` otherwise), matching the project.
- **WinUI 2 `ItemsRepeater` enumeration.** The original reads the repeater's realized items through the WinUI 2
  projection (`ItemsSourceView` / `TryGetElement`) to skip recycled cache items. This repo vendors only WinUI 3
  headers, whose interface IIDs differ, so the port walks the visual tree and skips elements the repeater has
  parked at (-10000, -10000) (`ItemsRepeater::ClearedElementsArrangeBounds`), which is the same set.
- **`LoadLibraryExW` hook** to catch Taskbar.View.dll loading: replaced by `SP_WaitForModule`.
- **`RunFromWindowThread` (WH_CALLWNDPROC + SendMessage)**: replaced. The taskbar frame element is found from the
  tray window with the same taskbar.dll symbols (resolve-only, no hooks) and the work is handed to its
  `CoreDispatcher`, which is the one XAML member readable from any thread. The engine thread waits up to 3 s per
  taskbar for the callback so BeforeUninit's restore is done before the hooks go.
- **`Wh_ModAfterInit` apply**: not needed; the module-wait callback applies as soon as the hooks are in, and a
  repeater the callback could not reach (cold sign-in, taskbar not built yet) is styled at its first layout pass
  by the Arrange hook, which remembers every repeater it meets. That list is also the fallback for
  SettingsChanged / BeforeUninit if the taskbar.dll symbols are missing on some build.

Everything else is ported 1:1: the negative-right-margin collapse, the Arrange-time pin at X = 0 (then search,
then task view), the widgets button nudge (44 px or the cluster width), the self-adjusting margin with the
1-second collapse throttle when there is no widgets button, the Start button padding fix, the Win+X alignment
override bracketed around the coroutine resume, and the search flyout move/restore.

## 6. Things the integrator must know

- Hooks on **Taskbar.View.dll** (from the wait callback): `TaskbarCollapsibleLayout` `ArrangeOverride`
  (required), `ExperienceToggleButton::UpdateButtonPadding`, `TaskbarFrame` `get_Alignment`,
  `ShowStartButtonContextMenuAsync$_ResumeCoro$1` (all three optional; the mod logs and degrades).
- **Windows.UI.Xaml.dll `IUIElement::Arrange`** is hooked by vtable slot (92) from inside the first
  ArrangeOverride call, with `SP_SetFunctionHookNow` on the taskbar's UI thread (needs a XAML thread to create a
  `Rectangle` and read its vtable). It is owned by the mod, so the engine removes it on unload. The slot is
  derived in a comment in the file; it is the same one the original uses.
- **dwmapi.dll `DwmSetWindowAttribute`** export hook, installed in Init.
- **taskbar.dll**: six symbols resolved only (`CTaskBand`/`CSecondaryTaskBand` `ITaskListWndSite` vftables,
  both `GetTaskbarHost`, `TaskbarHost::FrameHeight`, `std::_Ref_count_base::_Decref`). A miss is logged and
  not fatal.
- Init never fails for a missing symbol; it only fails if `SP_WaitForModule` itself cannot be set up.
- Init resets all state (known repeaters, Arrange-hook flag), so turning the mod off and on again without a
  shell restart works.
- Uninit waits up to 2 s for callbacks still queued on the taskbar dispatcher (they all return at once when
  `g_unloading` is set).

## 7. Verifying on a live shell

Prerequisite: Settings > Personalization > Taskbar > Taskbar behaviors > Taskbar alignment = **Center**.

1. Turn the mod on. Within a moment the Start button should jump to the far left edge of the taskbar; the app
   icons (and search, task view, widgets if shown) should stay centered, with no empty gap where Start used to
   be. If widgets is on, the widgets button sits right after Start.
2. Open and close a few apps: the centered group re-centers; Start stays at the left. Without the widgets
   button, drag the group to crowd it (open many apps): Start reserves its width instead of being overlapped,
   and gives it back when apps close.
3. Turn on "Also move the search and task view buttons": search and task view line up after Start on the left,
   only the app icons remain centered. Turn it off: they go back into the centered group. No restart needed.
4. Right-click the Start button (or Win+X): the menu should open aligned with the button's left edge, not
   centered on it. With `StartMenuOnLeft` = 0 it opens centered on the button again.
5. Open the Start menu and start typing so the search flyout appears: it should sit at the left edge of the
   work area. Close it: no leftover offset next time it is opened via Win+S.
6. On a second monitor the same should hold for its taskbar.
7. Turn the mod off: Start returns to the centered group immediately (margins restored, no reopen of explorer).
8. Sign out/in (cold start) with the mod on: the taskbar comes up with Start already on the left; check the
   log for "Taskbar repeater met for the first time; styling it" if `Logging` is enabled.

The Start menu itself is moved by a second mod entry with the same id, `start_menu_on_left.cpp`, which runs in
StartMenuExperienceHost.exe (target `SP_TARGET_STARTMENU`; the explorer engine loads the core DLL there while this
mod is enabled, see `engine/hostinject.c`). It follows the original's Start menu part: the classic menu's
`StartDocked.StartSizingFrame` gets Canvas.Left = 12, the redesigned menu's `FrameRoot` gets
HorizontalAlignment = Left, both re-applied on every show and when Windows moves them back, and restored when the
option or the mod is turned off. To verify: with the mod on, look for "Engine loaded into the Start menu process"
in the explorer log and "Attached to the Start menu" in `StartMenuExperienceHost.exe-<pid>.log`, then open Start.
