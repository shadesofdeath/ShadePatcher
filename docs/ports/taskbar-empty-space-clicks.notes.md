# taskbar-empty-space-clicks - integration notes

Source: `src/core/mods/taskbar_empty_space_clicks.cpp`
Based on the Windhawk mod `taskbar-empty-space-clicks` v2.6 by m1lhaus. Windows 11 XAML taskbar path only.

## 1. SP_MOD_DECLARE symbol

`g_modTaskbarEmptySpaceClicks`

Suggested placement in `mod_table.c`: the "Taskbar" group, after `g_modTrayIconsRebroadcast`. Order does not
matter: the mod does not use the shared input surfaces (`SP_SURFACE_TASKBAR_EMPTY` is not implemented in
`surfaces.c`, and the XAML taskbar's clicks do not reach `Shell_TrayWnd` anyway, see section 5).

## 2. ExceptionHandling

No. Plain Win32 + classic COM (UI Automation, MMDevice); no C++/WinRT, nothing throws. A plain
`<ClCompile Include="mods\taskbar_empty_space_clicks.cpp" />` line in `core.vcxproj` is enough.

Libraries are pulled in with `#pragma comment(lib, ...)` inside the file (Comctl32, Shell32, Ole32, OleAut32);
nothing to add to `AdditionalDependencies`. The pointer-message macros (`IS_POINTER_FIRSTBUTTON_WPARAM`,
`GetPointerType`) need `WINVER >= 0x0602`, which is the SDK default when the project defines nothing (it does not).

## 3. Settings read

All under `HKCU\Software\ShadePatcher\Mods\taskbar-empty-space-clicks`.

| Name | Type | Default | Meaning |
|------|------|---------|---------|
| `DoubleClickAction` | dword | 0 | Action for a left double click (or double tap) on empty taskbar space. Index into the action list below. |
| `MiddleClickAction` | dword | 0 | Action for a middle click on empty taskbar space. Same index list. |
| `CustomCommand` | sz | `""` | What the "Run a command" action runs: a program with arguments, a folder, a URL or a `shell:` name (goes through `ShellExecuteEx`). Environment variables are expanded. Prefix `uac;` asks for elevation. Quotes are honoured; an unquoted path with spaces is recognised if it exists on disk. |
| `DoubleClickModifier` | dword | 0 | **Optional, registry only (not in the GUI, no string ids left).** Bitmask of keys that must be held for the double click to count: 1 Ctrl, 2 Shift, 4 Alt, 8 Win. 0 = modifiers are ignored (the original mod's "None"). When non-zero the held set must match exactly. |
| `MiddleClickModifier` | dword | 0 | Same for the middle click. |

Action index (shared by both choice lists; the numbers are stored, do not reorder):

| Value | Action | String id |
|-------|--------|-----------|
| 0 | Do nothing | 1313 |
| 1 | Show the desktop | 1314 |
| 2 | Open the Start menu | 1315 |
| 3 | Open Task Manager | 1316 |
| 4 | Run a command (`CustomCommand`) | 1317 |
| 5 | Mute or unmute the sound | 1318 |
| 6 | Toggle taskbar auto-hide | 1319 |

Out-of-range values are treated as 0. `SettingsChanged` re-reads everything; no restart needed.

## 4. Proposed settings.reg lines and strings

Ids 1310-1319 are all used. The `CustomCommand` text box has no id of its own, so it reuses 1317 ("Run a
command") as its label and prompt, which reads naturally next to the choice lists. If a separate label is wanted,
it needs an id outside this block (1320 looks free).

The `;w` directive is written from `docs/settings-format.md` (label, then `;prompt`, `;default`, then the value
line); there is no other `;w` in the current `settings.reg` to copy from, so please check it renders.

```
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-empty-space-clicks]
;b %R:1310%
"Enabled"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-empty-space-clicks]
;c 7 %R:1311%
;x 0 %R:1313%
;x 1 %R:1314%
;x 2 %R:1315%
;x 3 %R:1316%
;x 4 %R:1317%
;x 5 %R:1318%
;x 6 %R:1319%
"DoubleClickAction"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-empty-space-clicks]
;c 7 %R:1312%
;x 0 %R:1313%
;x 1 %R:1314%
;x 2 %R:1315%
;x 3 %R:1316%
;x 4 %R:1317%
;x 5 %R:1318%
;x 6 %R:1319%
"MiddleClickAction"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-empty-space-clicks]
;w %R:1317%
;%R:1317%
;
"CustomCommand"=""
```

Suggested place: the Mods page, Taskbar heading (`%R:1103%`), after the `tray-icons-rebroadcast` block. No `*`
restart marker: enabling installs the hook in place, and every setting is live.

`strings.h` (Mods page block, 1101-1199 is the page; 1310-1319 were assigned for this mod):

```
#define IDS_MOD_TBCLICKS                1310
#define IDS_MOD_TBCLICKS_DOUBLE         1311
#define IDS_MOD_TBCLICKS_MIDDLE         1312
#define IDS_MOD_TBCLICKS_ACT_NOTHING    1313
#define IDS_MOD_TBCLICKS_ACT_DESKTOP    1314
#define IDS_MOD_TBCLICKS_ACT_START      1315
#define IDS_MOD_TBCLICKS_ACT_TASKMGR    1316
#define IDS_MOD_TBCLICKS_ACT_COMMAND    1317
#define IDS_MOD_TBCLICKS_ACT_MUTE       1318
#define IDS_MOD_TBCLICKS_ACT_AUTOHIDE   1319
```

`lang/gui.en-US.rc`:

```
    IDS_MOD_TBCLICKS                "Run an action when empty taskbar space is clicked"
    IDS_MOD_TBCLICKS_DOUBLE         "Double click"
    IDS_MOD_TBCLICKS_MIDDLE         "Middle click"
    IDS_MOD_TBCLICKS_ACT_NOTHING    "Do nothing"
    IDS_MOD_TBCLICKS_ACT_DESKTOP    "Show the desktop"
    IDS_MOD_TBCLICKS_ACT_START      "Open the Start menu"
    IDS_MOD_TBCLICKS_ACT_TASKMGR    "Open Task Manager"
    IDS_MOD_TBCLICKS_ACT_COMMAND    "Run a command"
    IDS_MOD_TBCLICKS_ACT_MUTE       "Mute or unmute the sound"
    IDS_MOD_TBCLICKS_ACT_AUTOHIDE   "Turn taskbar auto-hide on or off"
```

`lang/gui.tr-TR.rc`:

```
    IDS_MOD_TBCLICKS                "Görev çubuğunun boş alanına tıklanınca bir eylem çalıştır"
    IDS_MOD_TBCLICKS_DOUBLE         "Çift tıklama"
    IDS_MOD_TBCLICKS_MIDDLE         "Orta tıklama"
    IDS_MOD_TBCLICKS_ACT_NOTHING    "Hiçbir şey yapma"
    IDS_MOD_TBCLICKS_ACT_DESKTOP    "Masaüstünü göster"
    IDS_MOD_TBCLICKS_ACT_START      "Başlat menüsünü aç"
    IDS_MOD_TBCLICKS_ACT_TASKMGR    "Görev Yöneticisi'ni aç"
    IDS_MOD_TBCLICKS_ACT_COMMAND    "Bir komut çalıştır"
    IDS_MOD_TBCLICKS_ACT_MUTE       "Sesi kapat veya aç"
    IDS_MOD_TBCLICKS_ACT_AUTOHIDE   "Görev çubuğunu otomatik gizlemeyi aç veya kapat"
```

## 5. What changed versus the original, and what was left out

### How the clicks are caught (important for whoever touches this later)

The task brief suggested subclassing `Shell_TrayWnd` / `Shell_SecondaryTrayWnd`. That is done, but **it is not
where the clicks come from on the XAML taskbar**. The original mod's source is explicit about it: on Windows 11
the taskbar is a XAML island and every click arrives as `WM_POINTERDOWN` at a child window of class
`Windows.UI.Input.InputSite.WindowClass` (under `Windows.UI.Composition.DesktopWindowContentBridge`). The
top-level tray window sees nothing. That child window **must not be subclassed** ("the inputsite.dll code checks
that the value wasn't changed, and crashes otherwise" - m1lhaus). So, exactly like the original, the mod reads the
window's procedure with `GetWindowLongPtrW(GWLP_WNDPROC)` and puts an inline hook on it with
`SP_SetFunctionHookNow`.

Consequences:

- All InputSite windows share one procedure, so **one hook covers every taskbar**, including a secondary taskbar
  plugged in later. No `CreateWindowExW` / `CreateWindowInBand` hooks were ported. The hook filters by the class of
  `GetAncestor(hWnd, GA_ROOT)`; Start, search, widgets and other islands go straight through.
- The hook is installed from the `SP_WaitForModule(L"Taskbar.View.dll")` callback, which then polls (250 ms, up
  to a minute) for the tray window to have its island, because the DLL loads a moment before the island exists.
  The loop watches a `g_stopping` flag so `SP_CancelModuleWaits` never has to wait on it.
- The tray subclass (`SP_SetWindowSubclassFromAnyThread`, id `'SPEc'`) does three things: feeds legacy
  `WM_LBUTTONDOWN/DBLCLK` / `WM_MBUTTONDOWN` through the same pipeline for any strip the island does not cover,
  drops the window from the list on `WM_NCDESTROY`, and answers a registered message from `BeforeUninit` that
  releases the UI Automation object on the taskbar's own thread. Secondary taskbars seen later are subclassed
  lazily from inside the hook (same thread, plain `SetWindowSubclass`).

### Empty-space detection

Same rule as the original: `IUIAutomation::ElementFromPoint` at the click, then `CurrentClassName`. Empty space
is `Taskbar.TaskbarFrameAutomationPeer` (the frame), `Windows.UI.Input.InputSite.WindowClass` (21H2 builds
report the island), or the tray class names. Anything else (a button, a tray icon, the widgets button, the
clock, search) is not. The class seen is written to the log at debug level, so if a future build renames the
peer it shows up in one line. One `IUIAutomation` object is created lazily on the taskbar thread and reused.

### Trigger model: simplified

The original has 18 mouse/touch triggers x 7 keyboard modifiers x N rules, array settings, single/double/triple
click with a delayed timer, right-click context-menu suppression and synthetic right clicks. The port keeps the
two triggers the brief asked for:

- **Double click**: two left presses (mouse, or two touch/pen taps) on the same taskbar within
  `GetDoubleClickTime()` and the DPI-scaled `SM_CXDOUBLECLK`/`SM_CYDOUBLECLK` rectangle (15 px scaled for touch),
  both on empty space. No timer: since triple clicks are not a trigger the action fires on the second press.
- **Middle click**: fires on the press.

Modifiers are supported as an optional exact-match requirement (registry-only `DoubleClickModifier` /
`MiddleClickModifier`, see section 3) because the id budget had no room for a GUI label. Default 0 = ignored.

Single left click, right click, side buttons and triple clicks are not triggers (a single left click could not
be distinguished from a double click without the delay timer, and right click would need the context-menu
suppression machinery). Clicks are never swallowed; the message always reaches the original procedure.

### Actions kept (7, to fit ids 1313-1319)

| Action | How |
|--------|-----|
| Show desktop | `PostMessage(Shell_TrayWnd, WM_COMMAND, 407)` - the tray's own toggle-desktop command, same as the original. |
| Open Start | UI Automation: from the element under the click, `FindFirst(Descendants, AutomationId == "StartButton")` then `TogglePattern::Toggle()`; falls back to tapping the Win key if the button is hidden (e.g. by another mod). Same as the original. |
| Task Manager | `ShellExecuteEx("open", %SystemRoot%\System32\Taskmgr.exe)` on a worker thread. |
| Run a command | `ShellExecuteEx` on a worker thread; `uac;` prefix -> `runas`; env vars expanded; launched on the monitor under the cursor (`SEE_MASK_HMONITOR`). Splitting: quoted file, else the shortest space-delimited prefix that exists on disk, else first word. This is a little more forgiving than the original's "accumulate until an extension appears". |
| Mute | MMDevice: read the default render endpoint's mute, set `!muted` on every active render endpoint (as the original does), on a worker thread. |
| Auto-hide | `SHAppBarMessage(ABM_GETSTATE/ABM_SETSTATE)` on `Shell_TrayWnd`. |

Worker thread: Task Manager, Run a command and Mute run on a short-lived thread with its own STA so the taskbar's
UI thread never blocks on a UAC prompt. `Uninit` waits up to 5 s for such threads.

### Actions dropped, and why

- **Ctrl+Alt+Tab task switcher**: in the original this is a small subsystem (find the `XamlExplorerHostIslandWindow`
  on the `MultitaskingView` thread, check its z-band with the private `GetWindowBand`, subclass it, suppress its
  `WM_ACTIVATE`/`WM_NCACTIVATE`/`WM_SHOWWINDOW` so the next taskbar click can cycle, close it with a synthetic Enter).
  Without that it is a keypress that closes on the next click. Not reliable enough to ship.
- **Virtual desktop switch**: undocumented `IVirtualDesktopManagerInternal` with per-build IIDs and vtable
  indices (five variants in the original). Works on 26100+ today, breaks silently on the next interface change.
- **Everything else** the original offers (Win+Tab, media keys, hide desktop icons, combine buttons, taskbar
  alignment, arbitrary virtual key sequences with focus tracking, ExplorerPatcher / Windows 10 taskbar support,
  primary/secondary taskbar filter per rule, eager evaluation): outside the brief.

If the two dropped actions are wanted later they need two more string ids and about 80 lines each; the action
enum and `RunAction` switch are the only places to touch.

### Other differences

- No `CoInitializeEx` on the taskbar thread in normal operation (it already has an STA); it is only called as a
  recovery if `CoCreateInstance` reports `CO_E_NOTINITIALIZED`, and then never balanced on purpose.
- `GetCapture() != NULL` skips the click (a drag or menu in progress), as in the original.
- A click on a button forgets a half-finished double click.
- Unload hazard shared with every inline hook in the engine: if the mod is disabled at the exact moment a click is
  inside `ElementFromPoint`, that thread returns into unloaded code. Same as any other hook here; nothing extra
  was done about it.

## 6. How to verify on a live shell

1. Enable the mod, set **Double click** to "Show the desktop" and **Middle click** to "Open Task Manager"
   (or `DoubleClickAction=1`, `MiddleClickAction=3` under
   `HKCU\Software\ShadePatcher\Mods\taskbar-empty-space-clicks`).
2. Double click the empty part of the taskbar (between the centred icons and the tray, or left of the icons):
   all windows minimise; double click again: they come back. Nothing else should happen (no menu, no flash).
3. Middle click the same area: Task Manager opens. Middle click *on a taskbar button* must still do what Windows
   does there (nothing / close on some builds), i.e. the mod must not react.
4. Double click **on** a pinned icon, on the clock, on the tray chevron, on the Start button: the mod must not
   react (the log at `Logging=2` shows `Under the click: <class>` without "(empty space)").
5. Set Middle click to "Run a command" with `CustomCommand` = `notepad.exe` then
   `"C:\Program Files\...\something.exe" arg` then `https://example.com` then `shell:Downloads` then
   `uac;C:\Windows\System32\notepad.exe`: each opens as expected; the UAC one prompts.
6. Set an action to "Mute or unmute the sound": the volume tray icon shows muted / unmuted after each middle click.
7. Set an action to "Turn taskbar auto-hide on or off": the taskbar starts auto-hiding; again: it stays.
8. Set an action to "Open the Start menu": Start opens; middle click again closes it.
9. Secondary monitor: the same double / middle click on the second taskbar works too (its clicks go through the
   same InputSite procedure).
10. Optional modifier: set `DoubleClickModifier=1` in the registry. A plain double click does nothing; Ctrl +
    double click runs the action; Ctrl+Shift + double click does nothing (exact match).
11. Change a setting in the GUI while Explorer runs: takes effect on the next click, no restart.
12. Disable the mod: clicks do nothing; the log shows `Watching taskbar window` lines gone and no errors. With
    `Logging=1` a normal session shows `Hooked the taskbar's InputSite window procedure <addr>`,
    `Watching taskbar window <hwnd>` and one `<trigger> on empty taskbar space: <action>` line per action.
