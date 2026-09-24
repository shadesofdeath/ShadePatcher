# taskbar-button-click port notes

Source file written: `src/core/mods/taskbar_button_click.cpp`
Based on the Windhawk mod `taskbar-button-click` v1.0.9 by m417z (credited in `basedOn` / `originalAuthor`).

## 1. Declaration

```c
SP_MOD_DECLARE(g_modTaskbarButtonClick);
```

`SP_MOD_ID` is `"taskbar-button-click"`. `minOsBuild` is 22000, `flags` 0, target `SP_TARGET_EXPLORER`.

## 2. Exception handling

**Not needed.** The file uses no C++/WinRT; it is plain Win32 plus `<atomic>` and `<unordered_map>`. Add it to
`core.vcxproj` as a plain `<ClCompile Include="mods\taskbar_button_click.cpp" />`, without
`<ExceptionHandling>Sync</ExceptionHandling>`.

## 3. Settings read

All under `HKCU\Software\ShadePatcher\Mods\taskbar-button-click`. Both are re-read in `SettingsChanged` and
take effect on the next click; no restart.

| Name | Type | Default | Meaning | Allowed values |
|------|------|---------|---------|----------------|
| `MultipleItemsBehavior` | dword | 0 | What a middle click does on a button that holds several combined windows | 0 = close all of the group's windows; 1 = close only the group's active (foreground) window; 2 = do nothing (the click is swallowed, no new instance either). Out-of-range falls back to 0. |
| `EndTaskKeys` | dword | 1 | Modifier(s) that must be held with the middle click to **end the task** (`CTaskBand::_EndTask`, same as the taskbar's "End task") instead of closing the window. Exactly these keys and no others must be down. Never applies to Store/immersive apps, and never to a combined group (only to a single button / uncombined button). | Bitmask: 0 = off; 1 = Ctrl; 2 = Alt; 3 = Ctrl + Alt. Other bits are masked off. |

The Windhawk originals were `multipleItemsBehavior` (string enum closeAll/closeForeground/none), the two-bool
array `keysToEndTask.Ctrl` / `keysToEndTask.Alt`, and `oldTaskbarOnWin11`. The two bools are flattened into the
single `EndTaskKeys` bitmask (the original semantics "the held keys must equal the configured set" are kept
exactly); the string enum became a dword; `oldTaskbarOnWin11` is dropped (see section 5).

## 4. Proposed settings.reg block and strings (ids 1340-1349)

Put the block in the taskbar section of `settings.reg` (next to `tray-show-all-icons` /
`tray-icons-rebroadcast`, whichever `;a` heading the integrator prefers). No `*` restart marker is needed: the
hooks are installed and removed live by the engine.

```
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-button-click]
;b %R:1340%
"Enabled"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-button-click]
;c 3 %R:1341%
;x 0 %R:1342%
;x 1 %R:1343%
;x 2 %R:1344%
"MultipleItemsBehavior"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-button-click]
;c 4 %R:1345%
;x 0 %R:1346%
;x 1 %R:1347%
;x 2 %R:1348%
;x 3 %R:1349%
"EndTaskKeys"=dword:00000001
```

Suggested `strings.h` names (the integrator may rename to taste):

```c
// Mod: taskbar-button-click (1340-1349)
#define IDS_MOD_TBCLICK                 1340
#define IDS_MOD_TBCLICK_GROUP           1341
#define IDS_MOD_TBCLICK_GROUP_0         1342
#define IDS_MOD_TBCLICK_GROUP_1         1343
#define IDS_MOD_TBCLICK_GROUP_2         1344
#define IDS_MOD_TBCLICK_ENDTASK         1345
#define IDS_MOD_TBCLICK_ENDTASK_0       1346
#define IDS_MOD_TBCLICK_ENDTASK_1       1347
#define IDS_MOD_TBCLICK_ENDTASK_2       1348
#define IDS_MOD_TBCLICK_ENDTASK_3       1349
```

`gui.en-US.rc`:

```
    IDS_MOD_TBCLICK                 "Middle click on a taskbar button closes the window"
    IDS_MOD_TBCLICK_GROUP           "When a button with several windows is middle-clicked"
    IDS_MOD_TBCLICK_GROUP_0         "Close all of its windows (default)"
    IDS_MOD_TBCLICK_GROUP_1         "Close only the window in front"
    IDS_MOD_TBCLICK_GROUP_2         "Do nothing"
    IDS_MOD_TBCLICK_ENDTASK         "Keys held with the middle click to end the task instead of closing"
    IDS_MOD_TBCLICK_ENDTASK_0       "Never end the task"
    IDS_MOD_TBCLICK_ENDTASK_1       "Ctrl (default)"
    IDS_MOD_TBCLICK_ENDTASK_2       "Alt"
    IDS_MOD_TBCLICK_ENDTASK_3       "Ctrl + Alt"
```

`gui.tr-TR.rc`:

```
    IDS_MOD_TBCLICK                 "Görev çubuğu düğmesine orta tıklama pencereyi kapatır"
    IDS_MOD_TBCLICK_GROUP           "Birden çok pencereli bir düğmeye orta tıklanınca"
    IDS_MOD_TBCLICK_GROUP_0         "Tüm pencerelerini kapat (varsayılan)"
    IDS_MOD_TBCLICK_GROUP_1         "Yalnızca öndeki pencereyi kapat"
    IDS_MOD_TBCLICK_GROUP_2         "Hiçbir şey yapma"
    IDS_MOD_TBCLICK_ENDTASK         "Kapatmak yerine görevi sonlandırmak için orta tıklarken basılı tutulacak tuşlar"
    IDS_MOD_TBCLICK_ENDTASK_0       "Görevi hiç sonlandırma"
    IDS_MOD_TBCLICK_ENDTASK_1       "Ctrl (varsayılan)"
    IDS_MOD_TBCLICK_ENDTASK_2       "Alt"
    IDS_MOD_TBCLICK_ENDTASK_3       "Ctrl + Alt"
```

## 5. Deliberately left out

- **Windows 10 path** (`explorer.exe` symbols, the `__thiscall` `_EndTask` hook target, the `+0x28` this-adjust
  magic in `_HandleClick`): the port is Windows 11 only. The `__thiscall` spelling of `_EndTask` is still
  listed as an alternate name for that one resolve-only symbol, as the brief asks for all Windhawk spellings to
  be kept; it simply never matches on Windows 11.
- **ExplorerPatcher support** (`oldTaskbarOnWin11` setting, `ep_taskbar.*` module detection, the
  `LoadLibraryExW` hook in kernelbase, `EnumProcessModules` scan, the `Wh_ModAfterInit` retry): all tied to the
  Win10-style taskbar, which is out of scope.
- **Explorer version sniffing** (`GetModuleVersionInfo`, `-lversion`): the engine gates on `minOsBuild`.
- **Explicit `LoadLibraryEx("taskbar.dll")`**: replaced by `SP_WaitForModule(L"Taskbar.dll", 60000, ...)`.
  On Windows 11 the shell loads Taskbar.dll itself with the taskbar, so the mod waits for it rather than
  forcing it in; hooks are installed inside the callback, as the brief requires for late modules.
- **Per-thread nesting state via globals**: the original keeps the in-flight click in globals cleared after
  each call. The port uses a `thread_local` POD for the same data (the three calls nest on the taskbar UI
  thread) and an SRWLOCK-guarded map for the per-task-list active item, so no hook body races another thread.

Everything the user sees is kept: middle click closes, Shift+left click still opens a new instance, the three
group behaviours, the end-task modifier combination, and the Store-app exception for end-task.

## Things the integrator should know

- Register in `mod_table.c` (`SP_MOD_DECLARE(g_modTaskbarButtonClick);` + table entry) and add the plain
  `ClCompile` line to `core.vcxproj`. Nothing else in the engine is touched.
- One `SP_HookSymbols` call installs 4 hooks and 8 resolve-only entries in `Taskbar.dll`. Entry 11 is a **data
  symbol**, `const CImmersiveTaskItem::`vftable'{for `ITaskItem'}`, resolved with `hookFunction = nullptr`. The
  engine's PDB matcher must return public/data symbols for that name, not only functions, exactly as Windhawk's
  `HookSymbols` does; if it only walks function symbols the whole call fails (all entries are required) and the
  log says so. In that case either teach the matcher about data symbols or make entry 11 optional and fall back
  to treating every item as a `CWindowTaskItem` (that loses only the "never end a Store app" guard).
- All hooks live in `Taskbar.dll`, not `Taskbar.View.dll`. The wait is `SP_WaitForModule(L"Taskbar.dll", 60000, ...)`.
- The mod has no `AfterInit` / `BeforeUninit`: it changes nothing on live windows or XAML. `Uninit` only clears
  the active-item map after the engine has removed the hooks.

## 6. Verifying on a live shell

Enable the mod, then with logging on (`Logging`=2) watch for `Middle click on taskbar buttons now closes` from
tag `taskbar-button-click`. If instead you see `The taskbar click functions were not all found in this build`,
a symbol name changed on this build; run the symbol enumerator on Taskbar.dll to find the new spelling.

1. Open Notepad. **Middle-click** its taskbar button: the Notepad window closes (it goes through the same path
   as the jump list's "Close window", so an unsaved document still prompts). Without the mod a second Notepad
   would have opened instead.
2. **Shift + left-click** the button of a running app: a new instance still opens (unchanged behaviour).
3. Middle-click a **pinned app that is not running**: it launches as usual (nothing to close).
4. Hold **Ctrl** and middle-click a running Win32 app (e.g. Notepad with unsaved text): the process is killed
   without a save prompt (End task). Set `EndTaskKeys` to 0 and repeat: it now just closes with the prompt.
   Set it to 3: only Ctrl+Alt together end the task; Ctrl alone closes normally.
5. Hold Ctrl and middle-click a Store app (e.g. Calculator): it closes normally, never ended.
6. Open three Explorer windows so they combine into one button (taskbar setting "Combine taskbar buttons" =
   Always). Middle-click the button:
   - `MultipleItemsBehavior` 0: all three windows close.
   - `MultipleItemsBehavior` 1: only the Explorer window that was last active closes; if another program was
     the last active window in that group's task list you may get nothing (matches the original's behaviour).
   - `MultipleItemsBehavior` 2: nothing happens, and no new Explorer window opens.
   Change the value in the settings window and repeat without restarting the shell; `SettingsChanged` picks
   it up (log line `Group behavior is N, end-task keys are M`).
7. Disable the mod in the settings window: middle click immediately goes back to opening a new instance.
