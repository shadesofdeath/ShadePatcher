# win11-power-buttons -> src/core/mods/start_power_buttons.cpp

Port of the Windhawk mod "Windows 11 Start Menu Power Buttons" (win11-power-buttons, v1.0.2, Hakuuyosei).
Written against the engine API; no code copied. Credited in `basedOn` / `originalAuthor`.

## READ FIRST: on build 26200 this mod cannot reach the Start menu from explorer.exe

Checked on this machine (Windows 11 Pro 10.0.26200, x64) while writing the port:

* The Start menu XAML runs in **StartMenuExperienceHost.exe** (PID 8748 at the time), which has
  `StartDocked.dll`, `StartMenu.dll`, `Windows.UI.Xaml.dll` loaded and owns the Start menu's
  `Windows.UI.Core.CoreWindow`.
* **explorer.exe** (PID 15300) has `Taskbar.View.dll`, `Windows.UI.Xaml.dll` and a `StartMenu.dll` (the
  shell-side half from `MicrosoftWindows.Client.Core`), but **not** `StartDocked.dll`, and its only
  `Windows.UI.Core.CoreWindow` is the hidden `DesktopWindowXamlSource` dummy that XAML islands create. The
  `PowerButton` element the mod looks for is not in that process.
* The Windhawk original is a two-process mod: `@include StartMenuExperienceHost.exe` for the buttons and
  `@include explorer.exe` for a privileged proxy window that runs the power action (the Start menu process is an
  AppContainer and cannot call ExitWindowsEx / SetSuspendState). The engine only supports `SP_TARGET_EXPLORER`, so
  the half that draws the buttons has no host here.

Consequence: **on this build the mod loads, waits and never draws anything.** It is written for the case where
Windows hosts the Start menu inside explorer.exe (Microsoft has done that on some builds behind a feature flag and
may again), and it is guarded so that it is inert everywhere else:

1. `Init` only calls `SP_WaitForModule(L"StartDocked.dll", 0, ...)`. Nothing is hooked until the Start menu's own
   XAML library is in this process. Timeout is 0 (forever) because on a build that hosts Start in explorer.exe the
   library is loaded when the menu first opens, which can be long after sign-in; the cost is one
   `GetModuleHandle` every 500 ms on the engine's helper thread. Change the second argument if that is unwanted.
2. In the callback it hooks the user32 exports `CreateWindowInBand` / `CreateWindowInBandEx` (every CoreWindow is
   created through them, this is what the original does) and enumerates the CoreWindows that already exist.
3. Each CoreWindow found is subclassed (`SetWindowSubclass` on its own thread, or
   `SP_SetWindowSubclassFromAnyThread` from the helper thread) and driven with a registered control message, so
   every XAML call runs on that window's thread. On that thread it attaches `CoreWindow::VisibilityChanged` /
   `Activated`, and on each of those searches `Window::Current().Content()` for the element named `PowerButton`,
   hides it and appends a horizontal `StackPanel` of `Button`s to its parent panel (same Grid cell when the parent
   is a Grid).

If a future build hosts the Start menu in explorer.exe as a **XAML island** rather than a CoreWindow,
`Window::Current().Content()` is null there and this route still finds nothing; the only host-independent way to
reach island content is the XAML diagnostics API (`InitializeXamlDiagnosticsEx` + `IVisualTreeServiceCallback2`,
which the Windhawk stylers use). That needs the engine DLL to export `DllGetClassObject`, i.e. an engine change,
so it was not attempted here. If the newer Start (StartMenu.dll based) is what gets hosted, change the module name
in `Init` to `StartMenu.dll` and check that the element is still called `PowerButton`.

The power-action side (`ExitWindowsEx`, `SetSuspendState`, `LockWorkStation`, SE_SHUTDOWN_NAME) works in
explorer.exe as is and needs no proxy; the original's proxy window was dropped (see "Left out").

## 1. Declaration

```c
SP_MOD_DECLARE(g_modStartPowerButtons);
```

File: `src/core/mods/start_power_buttons.cpp`, `SP_MOD_ID "win11-power-buttons"`, targets `SP_TARGET_EXPLORER`,
`minOsBuild` 22000, flags 0. Suggested table group: shell-wide / Start menu.

## 2. Exception handling

**Yes**: the file uses C++/WinRT (Windows.UI.Xaml, Windows.UI.Core). core.vcxproj needs

```xml
<ClCompile Include="mods\start_power_buttons.cpp">
  <ExceptionHandling>Sync</ExceptionHandling>
</ClCompile>
```

Every path XAML or the window procedure can enter is wrapped in try/catch. Libraries are pulled by
`#pragma comment(lib, "powrprof.lib")` and `#pragma comment(lib, "comctl32.lib")` inside the file, so no
vcxproj link change is needed.

## 3. Settings (HKCU\Software\ShadePatcher\Mods\win11-power-buttons)

| Name                  | Type  | Default | Meaning |
|-----------------------|-------|---------|---------|
| `Enabled`             | dword | 0 (engine) / 1 read by the mod | Engine load toggle; the mod also reads it and restores the shell's power button when it turns 0 without an unload. |
| `ShowShutdown`        | dword | 1 | Show the Shut down button. |
| `ShowRestart`         | dword | 1 | Show the Restart button. |
| `ShowSignOut`         | dword | 1 | Show the Sign out button. |
| `ShowSleep`           | dword | 1 | Show the Sleep button. |
| `ShowHibernate`       | dword | 0 | Show the Hibernate button. |
| `ShowLock`            | dword | 0 | Show the Lock button. |
| `ConfirmBeforeAction` | dword | 0 | 1 = a Yes/No box (on a worker thread, never on the UI thread) before the action runs. |
| `Alignment`           | dword | 0 | Where the row sits in its panel: 0 = right, 1 = left, 2 = center. Registry-only (no string ids left for the three choice labels). |
| `ForceClose`          | dword | 0 | 0 = `EWX_FORCEIFHUNG` (like the shell's own power menu, apps may block with the "this app is preventing" screen); 1 = `EWX_FORCE` like the original (kills apps, unsaved work is lost). Registry-only. |
| `Order`               | sz    | "" | Optional. Comma/semicolon/space separated keywords `shutdown restart signout sleep hibernate lock`, case-insensitive; sets the left-to-right order. Buttons not named follow in the default order. Only buttons whose toggle is on are shown. Registry-only. |

Default order when `Order` is empty: Lock, Sleep, Hibernate, Sign out, Restart, Shut down. With the default
toggles that renders Sleep, Sign out, Restart, Shut down, which is exactly the original's default
`[sleep, signout, restart, shutdown]`.

The original's `buttons[]` array setting (which buttons + their order in one list) was flattened into the six
per-button toggles plus the optional `Order` string, as the brief asks.

## 4. Proposed settings.reg lines and strings (ids 1540-1549)

Put the block under a heading of the integrator's choice (there is no "Start menu" heading id in the range given;
either add one from the shared range or place it under the Taskbar heading %R:1103%).

```
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\win11-power-buttons]
;b %R:1540%
"Enabled"=dword:00000000
;e %R:1549%
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\win11-power-buttons]
;b %R:1541%
"ShowShutdown"=dword:00000001
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\win11-power-buttons]
;b %R:1542%
"ShowRestart"=dword:00000001
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\win11-power-buttons]
;b %R:1543%
"ShowSignOut"=dword:00000001
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\win11-power-buttons]
;b %R:1544%
"ShowSleep"=dword:00000001
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\win11-power-buttons]
;b %R:1545%
"ShowHibernate"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\win11-power-buttons]
;b %R:1546%
"ShowLock"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\win11-power-buttons]
;b %R:1547%
"ConfirmBeforeAction"=dword:00000000
```

`;b` for `Enabled` is the mod's on/off row (1540); `;e %R:1549%` is an explanatory line right under it saying
why nothing may happen on this build. 1548 is unused (reserved for an Alignment choice label if the choice
strings ever get ids).

strings.h:

```c
#define IDS_MOD_POWERBUTTONS            1540
#define IDS_MOD_POWERBUTTONS_SHUTDOWN   1541
#define IDS_MOD_POWERBUTTONS_RESTART    1542
#define IDS_MOD_POWERBUTTONS_SIGNOUT    1543
#define IDS_MOD_POWERBUTTONS_SLEEP      1544
#define IDS_MOD_POWERBUTTONS_HIBERNATE  1545
#define IDS_MOD_POWERBUTTONS_LOCK       1546
#define IDS_MOD_POWERBUTTONS_CONFIRM    1547
#define IDS_MOD_POWERBUTTONS_ALIGNMENT  1548    // reserved, not used yet
#define IDS_MOD_POWERBUTTONS_NOTE       1549
```

gui.en-US.rc:

```
    IDS_MOD_POWERBUTTONS            "Replace the Start menu power flyout with one-click power buttons"
    IDS_MOD_POWERBUTTONS_SHUTDOWN   "Shut down button"
    IDS_MOD_POWERBUTTONS_RESTART    "Restart button"
    IDS_MOD_POWERBUTTONS_SIGNOUT    "Sign out button"
    IDS_MOD_POWERBUTTONS_SLEEP      "Sleep button"
    IDS_MOD_POWERBUTTONS_HIBERNATE  "Hibernate button"
    IDS_MOD_POWERBUTTONS_LOCK       "Lock button"
    IDS_MOD_POWERBUTTONS_CONFIRM    "Ask for confirmation before running a power action"
    IDS_MOD_POWERBUTTONS_ALIGNMENT  "Alignment of the buttons"
    IDS_MOD_POWERBUTTONS_NOTE       "Only takes effect when Windows hosts the Start menu inside explorer.exe; on builds where it runs in StartMenuExperienceHost.exe nothing changes."
```

gui.tr-TR.rc:

```
    IDS_MOD_POWERBUTTONS            "Başlat menüsündeki güç menüsünü tek tıklamalı güç düğmeleriyle değiştir"
    IDS_MOD_POWERBUTTONS_SHUTDOWN   "Kapat düğmesi"
    IDS_MOD_POWERBUTTONS_RESTART    "Yeniden başlat düğmesi"
    IDS_MOD_POWERBUTTONS_SIGNOUT    "Oturumu kapat düğmesi"
    IDS_MOD_POWERBUTTONS_SLEEP      "Uyku düğmesi"
    IDS_MOD_POWERBUTTONS_HIBERNATE  "Hazırda beklet düğmesi"
    IDS_MOD_POWERBUTTONS_LOCK       "Kilitle düğmesi"
    IDS_MOD_POWERBUTTONS_CONFIRM    "Güç işleminden önce onay iste"
    IDS_MOD_POWERBUTTONS_ALIGNMENT  "Düğmelerin hizalaması"
    IDS_MOD_POWERBUTTONS_NOTE       "Yalnızca Windows Başlat menüsünü explorer.exe içinde çalıştırdığında etkili olur; StartMenuExperienceHost.exe içinde çalıştığı yapılarda hiçbir şey değişmez."
```

(The .rc files are plain ASCII-with-UTF-8 text in the repo; the Turkish lines above carry the characters as the
existing entries do. The mod source itself is ASCII only; its own Turkish tooltips are written as `\x` escapes.)

## 5. Left out, and why

* **The explorer.exe proxy window** (`PowerActionProxy_Class`, `ChangeWindowMessageFilterEx`, the proxy thread).
  Buttons and action are in one process here, so the action is run directly on a worker thread. The proxy also
  let any lower-integrity process on the desktop post a "shut down now" message to explorer.exe; not something
  to keep when it is not needed.
* **`Wh_GetIntSetting(L"buttons[%d]")` array**: flattened to six toggles plus an optional `Order` string.
* **Chinese localisation** of tooltips / confirmation: replaced by English + Turkish (the product's languages),
  chosen by `GetThreadUILanguage`.
* **`EWX_FORCE` as the only behaviour**: default is now `EWX_FORCEIFHUNG`; `ForceClose=1` brings the original
  back. Silent data loss on one click was not something to ship as the default.
* **Alignment in the settings window**: kept as a registry value; there were not enough string ids for the three
  choice labels.
* **DispatcherQueue**: the original re-enters the tree from a low-priority dispatcher callback; here the event
  handlers post the control message back to the CoreWindow instead, which does the same job without
  Windows.System and keeps all XAML work inside the subclass procedure.
* **Windhawk's WH_CALLWNDPROC "call on window thread" helper**: replaced by the subclass + control message, and by
  `SP_SetWindowSubclassFromAnyThread` for the cross-thread attach.

Kept: the hidden `PowerButton`, the horizontal `StackPanel` tagged so it can be found again, 40x40 flat buttons
with 4 px gaps, Segoe Fluent Icons glyphs (E7E8 E777 F3B1 E708 E823 E72E), tooltips, confirmation box,
SE_SHUTDOWN_NAME privilege, rebuild on settings change, cleanup on unload (row removed, power button shown again,
subclass removed synchronously in `BeforeUninit` via `SendMessageTimeout`).

## 6. Verifying on a live shell

On this build (26200, Start in StartMenuExperienceHost.exe) the only thing to verify is that the mod is inert:

1. Enable the mod, set `Logging=2`. Expect the log line "Waiting for the Start menu library (StartDocked.dll) to
   be in this process" and nothing else, ever. Open and close Start: unchanged. Turn the mod off: nothing
   changes, no crash, no lingering thread (the engine cancels the wait).

On a build where `StartDocked.dll` shows up in explorer.exe's module list (Process Explorer / `Get-Process
explorer | % Modules`):

1. Log shows "StartDocked.dll is in this process", "Watching for CoreWindows", then "Attached to CoreWindow"
   and "Watching CoreWindow" for each CoreWindow.
2. Click Start. The power (circle) button at the bottom right of the navigation bar disappears and a row of
   round-cornered icon buttons appears in its place, right-aligned: Sleep (moon), Sign out, Restart, Shut down
   (power). Log: "Power row built with 4 button(s)".
3. Hover a button: tooltip with its name (English, or Turkish when the shell UI is Turkish).
4. In the settings window tick Lock and Hibernate: the Start menu (open or next time it opens) shows 6 buttons
   without a shell restart ("Power row built with 6 button(s)"). Untick Shut down: 5 buttons.
5. Tick "Ask for confirmation", click Lock: a Yes/No box "Are you sure you want to lock?" appears with No as the
   default; Yes locks the workstation. Untick it, click Lock: locks immediately. (Test Lock/Sleep, not Shut down,
   unless you mean it.)
6. Set `Alignment=1` in the registry: the row moves to the left of its panel.
7. Turn the mod off: the row disappears and the shell's power button comes back without reopening Start.
