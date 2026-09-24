# start-menu-all-apps - port notes

Source file: `src/core/mods/start_menu_all_apps.cpp`
Original: Windhawk mod `start-menu-all-apps` v1.0.5 by m417z (`@include StartMenuExperienceHost.exe`).

## 1. SP_MOD_DECLARE symbol

`g_modStartMenuAllApps`

Suggested place in `mod_table.c`: a new "Start menu" group, or under "Taskbar" if no new group is wanted.

## 2. Exception handling

Not needed. The file uses no C++/WinRT: every hook only passes opaque pointers back to the shell's own
`consume_` functions. Compile it like the plain C++ mods (no `<ExceptionHandling>Sync</ExceptionHandling>`).

## 3. Settings read by the mod

None besides the engine's own `Enabled`. The Windhawk mod has no settings either, and there is no useful knob to
add: the behaviour is binary (all apps on open, or not).

| name    | type  | default | meaning                                              |
|---------|-------|---------|------------------------------------------------------|
| Enabled | dword | 0       | engine toggle; the mod is not loaded when it is 0    |

`SettingsChanged` is therefore `nullptr`.

## 4. Proposed settings.reg lines (ids 1530-1539; only 1530-1532 used)

```
;a %R:1531%
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\start-menu-all-apps]
;b %R:1530%
"Enabled"=dword:00000000
;e %R:1532%
```

If the integrator prefers not to open a new heading, drop the `;a %R:1531%` line and put the block under the
existing Taskbar heading (1103).

Strings:

| id   | English                                                                                                                                                 | Turkish                                                                                                                                                                  |
|------|---------------------------------------------------------------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| 1530 | Show all apps when the Start menu opens                                                                                                                 | Başlat menüsü açıldığında tüm uygulamaları göster                                                                                                                        |
| 1531 | Start menu                                                                                                                                              | Başlat menüsü                                                                                                                                                            |
| 1532 | Only works on builds where the Start menu runs inside explorer.exe. On most builds it runs in StartMenuExperienceHost.exe, which ShadePatcher does not patch. | Yalnızca Başlat menüsünün explorer.exe içinde çalıştığı yapılarda çalışır. Çoğu yapıda Başlat menüsü, ShadePatcher'ın yamalamadığı StartMenuExperienceHost.exe içinde çalışır. |

Suggested `strings.h` names: `IDS_MOD_STARTALLAPPS` (1530), `IDS_MODS_HEADING_STARTMENU` (1531),
`IDS_MOD_STARTALLAPPS_NOTE` (1532).

## IMPORTANT: which process hosts the Start menu on build 26200

The Windhawk mod runs inside **StartMenuExperienceHost.exe**, not explorer.exe. All four of its hooks are on
functions inside **StartMenu.dll** (package `MicrosoftWindows.Client.Core`), which is the Start menu's XAML
library. The controller the frame is built on (`DockedStartController`) is implemented in **StartDocked.dll**.

What this machine (26200, StartMenu.dll 2607.28004.100.0) shows right now:

| process                     | StartMenu.dll | StartDocked.dll | StartMenu.NET.dll |
|-----------------------------|---------------|-----------------|-------------------|
| explorer.exe                | loaded        | not loaded      | not loaded        |
| StartMenuExperienceHost.exe | loaded        | loaded          | loaded            |

StartMenuExperienceHost.exe was started in the same second as explorer.exe and holds the controller DLL, so on
this build the Start menu is almost certainly still hosted **outside** explorer.exe. That means:

- The mod's hooks will be installed in explorer.exe's copy of StartMenu.dll (it is loaded there, so
  `SP_WaitForModule` fires quickly and symbol resolution runs).
- `StartInnerFrame` will most likely never be constructed inside explorer.exe, so the hooks will never be
  reached and the user will see **no change**. This is a limitation of the engine's process scope, not a bug in
  the mod, and cannot be worked around without an engine target for StartMenuExperienceHost.exe
  (`SP_ModTarget` currently only has `SP_TARGET_EXPLORER`).
- The mod is still correct for any build where the Start menu is hosted in explorer.exe (Microsoft has been
  moving Start into explorer.exe in Insider builds; the presence of StartMenu.dll in explorer.exe here is a hint
  that this is in progress). It logs `StartInnerFrame is being constructed in this process` when that is the
  case, which is the definitive test.

Recommendation for the integrator: ship it with the 1532 note under the toggle, or hold it back until the engine
gains a StartMenuExperienceHost.exe target. If a target is added, the mod body needs no change; only `targets`
in `SP_MOD_DEFINE` and the module wait (StartMenu.dll is also the right module in that process).

## Port design (what the code does)

- `Init`: `SP_WaitForModule(L"StartMenu.dll", 0 /* no deadline */, ...)`. No deadline because on some builds the
  DLL only appears when the menu is first opened; the engine's poll is a `GetModuleHandleW` every 500 ms and is
  cancelled on unload.
- In the callback, one `SP_HookSymbols` transaction with four entries (same spellings as the Windhawk mod):
  1. `...IDockedStartControllerOverrides::ShowAllApps` - resolve only (`hookFunction = nullptr`).
  2. `...IDockedStartControllerOverrides::HideAllApps` - hooked; when enabled it calls `ShowAllApps` on the same
     object instead (the shell calls HideAllApps on menu dismissal to reset to the pinned page).
  3. `StartInnerFrame::StartInnerFrame(DockedStartController const&, IInspectable const&)` - hooked; after the
     original runs, `ShowAllApps` is called on the frame's controller so the very first opening shows all apps.
  4. `winrt::impl::as<IDockedStartControllerOverrides,...>` - hooked, **optional** (added in KB5055627). While
     the constructor is running on the same thread, the first call's result slot is captured as the controller
     object for step 3. Thread identity is tracked with `std::atomic<DWORD>`; the captured pointer is reset at
     the start of every construction (the Windhawk mod captures once for the process lifetime; resetting is
     strictly safer if the frame is ever rebuilt).
- Fallback: if the `as<>` symbol is absent (older builds) the constructor's `dockedStartController` parameter is
  used, as in the original. If the symbol is present but nothing was captured, the first opening is left alone
  and an error is logged, rather than calling through a possibly wrong interface (the original would call it
  anyway).
- `BeforeUninit` clears `g_enabled` so `HideAllApps` passes through while hooks are still live; the engine then
  removes the hooks. No `Uninit` needed.
- No exceptions, no XAML objects touched, every hook body is a few pointer checks and a call to the shell's own
  function.

## 5. Deliberately left out

- The `LoadLibraryExW` hook in kernelbase the original uses to notice StartMenu.dll loading: replaced by
  `SP_WaitForModule`, as the brief requires.
- The `Wh_ModAfterInit` re-check for a module that appeared between Init and AfterInit: the module wait covers
  that window.
- Nothing else: the original has no settings and no other features.

## 6. How to verify on a live shell

Precondition check first (this decides whether the mod can work at all on the machine):

1. Turn on engine logging (`HKCU\Software\ShadePatcher\Logging` = 2) and watch DbgView or the log file.
2. Enable the mod, restart the shell (or load the engine).
3. Expect `[start-menu-all-apps] Waiting for StartMenu.dll` then `Hooked StartMenu.dll` (possibly with the
   `older build` suffix). If instead `The Start menu functions were not found in this build of StartMenu.dll`
   appears, the PDB for StartMenu.dll 2607.x did not resolve the names; run the engine's symbol enumeration
   tooling on `C:\Windows\SystemApps\MicrosoftWindows.Client.Core_cw5n1h2txyewy\StartMenu.dll` and look for the
   current spellings of `ShowAllApps` / `HideAllApps` / `StartInnerFrame::StartInnerFrame`.
4. Press the Windows key. If `StartInnerFrame is being constructed in this process` (or the debug line
   `HideAllApps: showing all apps instead` after closing the menu) never appears, the Start menu is hosted in
   StartMenuExperienceHost.exe on this build and the mod cannot take effect from explorer.exe. Confirm with
   Task Manager / Process Explorer: StartDocked.dll loaded in StartMenuExperienceHost.exe, not in explorer.exe.

Visual check when the menu does run in explorer.exe:

1. With the mod off: press Win. The menu opens on the pinned page with the "All" button top-right.
2. Enable the mod, restart the shell. Press Win: the menu should open directly on the "All apps" list.
   - If the mod was enabled while the shell was already running (frame already built), the first opening still
     shows pinned; close it and open again, and it should be on "All apps" from then on. This matches the
     original mod.
3. Click "Back" in the app list, close the menu, open it again: it should be on "All apps" again.
4. Disable the mod (or unload): close and reopen the menu; it should be back on the pinned page.
