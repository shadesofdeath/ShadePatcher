# taskbar-volume-control - integration notes

Source file: `src/core/mods/taskbar_volume_control.cpp`
Windhawk original: `taskbar-volume-control` 1.3.1 by m417z (credited in `basedOn` / `originalAuthor`).

## 1. SP_MOD_DECLARE symbol

`g_modTaskbarVolumeControl`

    SP_MOD_DECLARE(g_modTaskbarVolumeControl);

`SP_MOD_ID` is `"taskbar-volume-control"`, `minOsBuild` 22000, `targets` SP_TARGET_EXPLORER, `flags` 0.

## 2. ExceptionHandling

Not needed. The file uses no C++/WinRT; it talks to Core Audio through plain COM interfaces (`IMMDeviceEnumerator`,
`IAudioEndpointVolume`) and checks HRESULTs. A plain `<ClCompile Include="mods\taskbar_volume_control.cpp" />` is
enough. It links `Comctl32.lib` and `Ole32.lib` through `#pragma comment(lib, ...)`.

## 3. Settings (HKCU\Software\ShadePatcher\Mods\taskbar-volume-control)

| Name | Type | Default | Meaning |
|------|------|---------|---------|
| `Enabled` | dword | 0 | engine toggle |
| `VolumeStep` | dword | 2 | percent of master volume per wheel notch; clamped to 1..50 in code. Any value works, not only multiples of 2 (see 5). UI offers 1, 2, 5, 10. |
| `ScrollAreaMode` | dword | 0 | 0 = the whole taskbar, 1 = the notification area only (TrayNotifyWnd on the primary taskbar; the clock/tray bridge on a secondary one, with a last-50-DIP fallback). Anything else is read as 0. |
| `MiddleClickMute` | dword | 1 | 1 = a middle click on the tray volume icon toggles mute (the original's default is on). |
| `ScrollAnywhereModifier` | dword | 0 | registry-only, no UI proposed (no string ids left in 1320-1329). Bit mask: 1 = Ctrl, 2 = Shift, 3 = Ctrl+Shift. Non-zero installs a WH_MOUSE_LL hook on a dedicated thread; holding exactly those keys (and neither Alt nor Win) makes the wheel change the volume anywhere on screen and swallows the wheel for the window underneath. Changing it in the registry is picked up by SettingsChanged without a restart. |

All values are re-read in `SettingsChanged`; none needs a shell restart.

## 4. Proposed settings.reg lines and strings (ids 1320-1329)

    [HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-volume-control]
    ;b %R:1320%
    "Enabled"=dword:00000000
    [HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-volume-control]
    ;c 4 %R:1321%
    ;x 1 %R:1322%
    ;x 2 %R:1323%
    ;x 5 %R:1324%
    ;x 10 %R:1325%
    "VolumeStep"=dword:00000002
    [HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-volume-control]
    ;c 2 %R:1326%
    ;x 0 %R:1327%
    ;x 1 %R:1328%
    "ScrollAreaMode"=dword:00000000
    [HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-volume-control]
    ;b %R:1329%
    "MiddleClickMute"=dword:00000001

Suggested `strings.h` names and texts:

| Id | Define | English | Turkish |
|----|--------|---------|---------|
| 1320 | `IDS_MOD_TASKBARVOLUME` | Change the volume by scrolling over the taskbar | Görev çubuğu üzerinde kaydırarak ses düzeyini değiştir |
| 1321 | `IDS_MOD_TASKBARVOLUME_STEP` | Volume change per wheel notch | Tekerlek adımı başına ses değişimi |
| 1322 | `IDS_MOD_TASKBARVOLUME_STEP_1` | 1 percent | yüzde 1 |
| 1323 | `IDS_MOD_TASKBARVOLUME_STEP_2` | 2 percent | yüzde 2 |
| 1324 | `IDS_MOD_TASKBARVOLUME_STEP_5` | 5 percent | yüzde 5 |
| 1325 | `IDS_MOD_TASKBARVOLUME_STEP_10` | 10 percent | yüzde 10 |
| 1326 | `IDS_MOD_TASKBARVOLUME_AREA` | Where scrolling changes the volume | Kaydırmanın ses düzeyini değiştirdiği alan |
| 1327 | `IDS_MOD_TASKBARVOLUME_AREA_0` | The whole taskbar | Görev çubuğunun tamamı |
| 1328 | `IDS_MOD_TASKBARVOLUME_AREA_1` | The notification area only | Yalnızca bildirim alanı |
| 1329 | `IDS_MOD_TASKBARVOLUME_MIDDLECLICK` | Middle click on the volume icon mutes and unmutes | Ses simgesine orta tıklama sesi kapatır ve açar |

The step texts avoid a literal `%` because some GUI strings (IDS_ABOUT_*) go through printf-style formatting; if
the choice renderer shows option strings verbatim, "1%", "2%", "5%", "10%" / "%1", "%2", "%5", "%10" read better.

If four more ids are ever available, `ScrollAnywhereModifier` can be exposed as
`;c 4 <label>` with `;x 0 Off`, `;x 1 Ctrl`, `;x 2 Shift`, `;x 3 Ctrl+Shift`.

## 5. What the port does, and how it differs from the original

Kept (Windows 11 only):

- Wheel over the taskbar changes the master volume. On Windows 11 the wheel arrives as `WM_POINTERWHEEL` at the
  XAML island's `Windows.UI.Input.InputSite.WindowClass` window, never as `WM_MOUSEWHEEL` at `Shell_TrayWnd`. As in
  the original, that window's procedure is hooked as a function (it cannot be subclassed: inputsite.dll verifies
  its wndproc). One hook covers every InputSite window in the process, so secondary taskbars are covered too.
- The Windows 11 volume flyout: reproduced exactly as the original's Win11 path. The mod posts
  `HSHELL_APPCOMMAND` / `APPCOMMAND_VOLUME_UP|DOWN` (the "SHELLHOOK" registered message) to the shell-hook window
  `Shell_TrayWnd > ReBarWindow32 > MSTaskSwWClass`, which is how a keyboard volume key ends up being handled, so
  the standard OSD appears and the shell does its own 2% step. Any remaining percent is applied first through
  `IAudioEndpointVolume::SetMasterVolumeLevelScalar`. Small improvement over the original: a step of 1 (or any
  odd step) works, because the endpoint is moved `total - 2` (which may be -1) before the shell adds 2. If the
  shell-hook window is missing on some build, the full change is applied through Core Audio without the OSD.
- Sub-notch wheel deltas (touchpads) accumulate for 5 seconds, as in the original.
- Scroll area: whole taskbar or notification area only. On this machine (26200) `TrayNotifyWnd` has a real
  rectangle (probed: 1465..1707 of a 1707-wide bar), so "tray only" is exact on the primary taskbar. Secondary
  taskbars use the original's heuristic (the second `DesktopWindowContentBridge` whose rect is not the whole bar),
  then the last 50 DIPs.
- Middle click on the tray volume icon toggles mute through Core Audio (`IAudioEndpointVolume::SetMute`), by
  hooking `VolumeSystemTrayIconDataModel::OnIconClicked` with the same PDB names the original uses. The mod
  waits for `Taskbar.View.dll` with `SP_WaitForModule`; in the callback it hooks `SystemTray.dll` if loaded,
  else `Taskbar.View.dll` when its major version is below 2604, else it waits for `SystemTray.dll` (a second
  `SP_WaitForModule`, registered from inside the first callback, which modules.c allows). On this machine
  Taskbar.View.dll is 2607 and SystemTray.dll 2607 is loaded, so the SystemTray.dll produce<> thunk is what gets
  hooked. The hook is always installed; the setting is checked per click, so toggling it needs no reload
  (the original required a reload).
- Scroll anywhere with Ctrl and/or Shift (registry-only setting, see 3): a `WH_MOUSE_LL` hook on its own thread
  with a message loop, which only posts the wheel to the primary taskbar; the volume work happens on the taskbar
  thread inside the subclass, never in the hook procedure.
- Taskbar windows are subclassed with `SP_SetWindowSubclassFromAnyThread` (or `SetWindowSubclass` directly when
  already on the taskbar thread, i.e. from the creation hooks) for: the scroll-anywhere message, a
  `WM_MOUSEWHEEL` fallback used only if the InputSite hook failed, and `WM_NCDESTROY` bookkeeping. Existing
  taskbars are enumerated in `AfterInit`; new ones (monitor plugged in, shell restart) are caught by hooking
  `user32!CreateWindowExW` (export hook) and `user32!CreateWindowInBand` (undocumented export, resolved with
  GetProcAddress, skipped if absent). The InputSite window is caught the same way on a cold sign-in.

Left out, and why:

- Windows 10 / Windows 7 code paths and the "old taskbar on Windows 11" (ExplorerPatcher) mode: out of scope.
- The `volumeIndicator` choice (Win10 "modern" indicator via ShellExperienceHost, Win7 SndVol.exe launch and
  positioning, the WM_COPYDATA / Shell_NotifyIconGetRect trick, the close timer, the
  `ForceFocusBasedMouseWheelRouting` hook in ShellExperienceHost/SndVol): all belong to the dropped indicators.
  Only the Windows 11 OSD remains. The mod runs only in explorer.exe.
- `additionalScrollRegions` (custom pixel/percent ranges) and the `taskbarWithoutNotificationArea` / `none`
  scroll areas: settings nobody needs for the core behaviour; `ScrollAreaMode` accepts 0 and 1 only.
- `ctrlScrollVolumeChange` (require Ctrl for taskbar scrolling): dropped.
- `noAutomaticMuteToggle`: only meaningful for the dropped indicators; with the shell handling the last 2% the
  mute behaviour is the shell's own (VOLUME_UP unmutes, like the keyboard key).
- `fullScreenScrolling` (scroll at the taskbar's position while a full-screen window covers it): dropped.
- Alt and Win as scroll-anywhere modifiers: they would open the Start menu / a menu bar on release, and the
  original masks that with a `WH_KEYBOARD_LL` hook and injected 0xE8 key events. Not simple; Ctrl and Shift only.
- `ExplorerExtensions.dll` as a third home for the tray types (pre-Taskbar.View.dll 22H2 builds): dropped; on
  such a build the wait for Taskbar.View.dll simply times out after 60 s and only middle-click mute is missing.

## 6. Integrator notes and caveats

- Unload order: manager.c cancels module waits before `BeforeUninit`. The callbacks here do nothing but install
  symbol hooks, so the cancel never waits long. `BeforeUninit` sets `g_active` false, stops the mouse-hook thread
  and removes the subclasses from the primary and every remembered secondary taskbar; the engine then removes the
  four function hooks (CreateWindowExW, CreateWindowInBand, the InputSite wndproc, OnIconClicked). `Uninit`
  releases the Core Audio enumerator.
- The InputSite wndproc hook is taken from `GWLP_WNDPROC` of the taskbar's InputSite window and installed with
  `SP_SetFunctionHookNow`, possibly from inside the `CreateWindowInBand` detour on the taskbar thread (the
  original does the same with `Wh_ApplyHookOperations`). If the engine ever forbids opening a hook transaction
  from a detour, move `HookInputSiteProc` to `AfterInit` only (warm start still works; cold sign-in then relies on
  `FindExistingTaskbars` not running before the window exists, which it would, so keep it if at all possible).
- A zeroed `SendInput` is issued before each volume change, as in the original ("allows to steal focus"). It is
  harmless; remove it if it ever shows up as a problem.
- `SP_HookExport(L"user32.dll", "CreateWindowExW")` is a hot hook (every window creation in explorer); the detour
  does three `_wcsicmp`s and returns. This mirrors the original.
- Logging: `SP_Log` on lifecycle, `SP_LogDebug` per wheel notch and per middle click.

## 7. Verifying on a live shell

1. Enable the mod, `Logging`=2. Expect log lines: "Taskbar window ...", "Hooked the InputSite window procedure
   at ...", and after a few seconds "Watching the volume icon's click handler in SystemTray.dll".
2. Scroll the wheel over an empty part of the taskbar: the Windows 11 volume flyout appears at the bottom-left
   and the value changes by `VolumeStep` per notch (default 2). Scrolling down to 0 and up again behaves like
   the keyboard volume keys.
3. Set `VolumeStep` to 5 (or 1) in the settings window: the flyout now moves 5 (or 1) per notch, without a
   restart.
4. Set `ScrollAreaMode` to 1: scrolling over the pinned apps does nothing; scrolling over the clock / tray icons
   (roughly the right-most 240 px at 100% DPI on this machine) changes the volume.
5. Middle-click the speaker icon in the tray: the sound mutes (icon shows the muted glyph); middle-click again to
   unmute. Turn `MiddleClickMute` off: a middle click does whatever the shell does by default (nothing).
6. With a second monitor: scroll over the secondary taskbar; the volume changes and the flyout appears.
7. Optional: set `ScrollAnywhereModifier`=1 in the registry; hold Ctrl and scroll over the desktop or a browser
   page: the volume changes, the page does not zoom. Set it back to 0: Ctrl+wheel zooms the page again.
8. Disable the mod: scrolling over the taskbar does nothing again, and the shell stays up.
