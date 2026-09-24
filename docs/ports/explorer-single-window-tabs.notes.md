# explorer-single-window-tabs - port notes

Source: `src/core/mods/explorer_single_window_tabs.cpp`
Original: Windhawk mod `explorer-single-window-tabs` v0.8 by ALMAS CP.

## 1. Declaration

```c
SP_MOD_DECLARE(g_modExplorerSingleWindowTabs);
```

Suggested place in `g_modTable`: the "File Explorer" group, after `g_modExplorerDoubleClickUp`.
`minOsBuild` is 22621 (tabs arrived in 22H2).

## 2. Exception handling

No. The file uses plain COM (`IShellWindows` / `IWebBrowser2` / `IShellBrowser`), no C++/WinRT, and
nothing in it throws. No `<ExceptionHandling>Sync</ExceptionHandling>` entry is needed in core.vcxproj; a
plain `<ClCompile Include="mods\explorer_single_window_tabs.cpp" />` is enough.

Libraries: the file pulls in `Shlwapi.lib`, `Shell32.lib`, `Ole32.lib` and `OleAut32.lib` with
`#pragma comment(lib, ...)`, the same way `explorer_double_click_up.cpp` pulls in Comctl32. `uuid.lib`
(for `CLSID_ShellWindows` / `SID_STopLevelBrowser`) is in the default link set.

## 3. Settings read

Key: `HKCU\Software\ShadePatcher\Mods\explorer-single-window-tabs`

| Name           | Type  | Default | Meaning |
|----------------|-------|---------|---------|
| `Enabled`      | dword | 0       | Read by the engine only; the mod does not read it itself (the engine loads/unloads the mod on a change). |
| `BringToFront` | dword | 1       | 1 = after a folder has been put into a tab, the window that received it is restored (if minimized) and brought to the foreground. 0 = the tab is added quietly and focus stays where it was. Applied live through `SettingsChanged`. |

Allowed values for `BringToFront`: 0 or 1.

The original mod has no settings at all. `BringToFront` is the one behaviour of the original (unconditional
`SetForegroundWindow`) that a user may reasonably want off, e.g. when folders are opened from a script or
from another application while they keep typing there.

The Shift bypass (hold Shift while opening a folder to get a separate window) is fixed behaviour, exactly as
in the original, and is not a setting.

## 4. Proposed settings.reg lines and strings

Place in the File Explorer group (after the `explorer-auto-file-sizes` block, or after
`explorer-double-click-up` if that one has been added by then). No restart marker (`*`): the hooks are
installed and removed live, and turning the mod off shows any window that was mid-redirect.

```
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\explorer-single-window-tabs]
;b %R:1410%
"Enabled"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\explorer-single-window-tabs]
;b %R:1411%
"BringToFront"=dword:00000001
```

strings.h:

```c
#define IDS_MOD_SINGLEWINDOWTABS        1410
#define IDS_MOD_SINGLEWINDOWTABS_FRONT  1411
```

Ids 1412-1419 are unused.

| Id   | English | Turkish |
|------|---------|---------|
| 1410 | `Open new folders as tabs in the File Explorer window that is already open` | `Yeni klasörleri zaten açık olan Dosya Gezgini penceresinde sekme olarak aç` |
| 1411 | `Bring that window to the front when a folder is added to it` | `Klasör eklendiğinde o pencereyi öne getir` |

(The Turkish text above contains non-ASCII characters; that is fine in the .rc language files, which already
hold Turkish. The .cpp file itself is ASCII only.)

The `name` field in the SP_Mod is the same text as 1410.

## 5. What was left out or changed, and why

Kept: everything the original does that the user can see. New Explorer window -> invisible -> its folder read
-> a new tab in the existing window -> navigated -> hidden window closed -> existing window brought to the
front. Shift bypass. Control Panel (and `shell:::{...}` locations) left in their own window and never chosen
as a target. Same undocumented pieces: the `ShellTabWindowClass` child, the `WM_COMMAND 0xA21B` "new tab"
command, layered alpha 0 plus a refused `ShowWindow` to keep the window from flashing.

Changed on purpose:

- **Target window choice.** The original takes the first redirectable entry in `IShellWindows`, which is the
  oldest window. This port enumerates visible `CabinetWClass` windows in Z order and takes the first that shows
  an ordinary folder, so the folder lands in the Explorer window the user worked in last. Enumerating first also
  means the very first Explorer window (the common case) is created without any COM call inside the hook.
- **Order of close and navigate.** The original posts `WM_CLOSE` to the hidden window *before* asking for the
  new tab and, if the tab never shows up, falls back to navigating the *last existing tab* of the target,
  which clobbers whatever the user had there. This port asks for the tab first, navigates it, and only then
  closes the hidden window; if the tab does not appear or the navigation fails, the hidden window is shown
  instead, so a folder is never lost and no existing tab is ever overwritten. The one cosmetic cost of a
  failure is a spare "Home" tab in the target window.
- **New-tab detection.** The original compares the global `IShellWindows` count (minus one for the hidden
  window). This port counts only the entries whose HWND is the target window, which is unaffected by the
  hidden window's own entry, and adds a short "settle" wait (up to 1 s, normally 100-200 ms) until the new
  tab reports a location, so the navigation to the requested folder cannot be overtaken by the tab's own
  initial navigation to Home.
- **Hidden-window bookkeeping.** The original keeps a single "last created window" HWND forever. This port
  keeps a small list, removes a window from it when its redirect ends, and clears it (showing whatever is
  left) in `BeforeUninit`, so a reused HWND can never be blocked from showing later.
- **Unload.** `BeforeUninit` sets a stop flag, waits (up to 5 s, in practice well under 1 s) for redirect
  threads to finish, and shows any window still hidden. The original has an empty `Wh_ModUninit`.
- **Navigation form.** Every location is navigated as a PIDL (via `SHParseDisplayName`), with the BSTR form
  as a fallback, instead of choosing by looking at the first characters of the path.

Left out: nothing functional. There were no settings, no arrays, no per-monitor tables.

Known limitations carried over from the original (documented for the README):

- Control Panel always opens in its own window.
- Dragging a tab out to detach it creates a new window, which is merged back. Hold Shift instead.
- Ctrl+N / "Open in new window" from inside Explorer also becomes a tab, by design; Shift bypasses it.
- "Launch folder windows in a separate process" is untested: the hooks live in whichever explorer.exe the
  engine is injected into, while `IShellWindows` is process-wide, so it should still work, but it has not
  been tried.

## 6. How to verify on a live shell

1. Enable the mod. Open one File Explorer window (Win+E). It opens normally; that is the primary window.
2. From the desktop, double-click any folder (or open one from Start, or run `explorer.exe C:\Windows` from
   Win+R). Expected: no second window appears; the primary window comes to the front with a new tab showing
   that folder. There may be a brief blink of a taskbar button, no window flash.
3. Do the same for a virtual folder: Win+R `shell:RecycleBinFolder`, or right-click This PC on the desktop
   and choose Open. Expected: a Recycle Bin / This PC tab in the primary window.
4. Hold Shift and double-click a folder. Expected: a separate, normal window.
5. Open Control Panel (Win+R `control`). Expected: it opens in its own window, and with the primary window
   closed it is never used as a tab target (a folder opened next gets a fresh window of its own).
6. Minimize the primary window and open a folder. Expected: the window is restored and comes to the front
   with the new tab. Set `BringToFront` to 0 and repeat: the tab is added but the window stays where it was.
7. With several folders opening at once (e.g. select three folders and press Enter): each becomes a tab.
8. Turn the mod off in the settings window while nothing is in flight: nothing changes on screen. Turn it off
   while a redirect is mid-way (hard to time by hand); the hidden window is shown instead of being lost.
9. With `Logging` = 2 under `HKCU\Software\ShadePatcher`, DbgView shows lines tagged
   `explorer-single-window-tabs`: "Explorer window ... will be redirected into ...", "Showing ... is held
   back", and "<path> opened as a tab in window ...".

The single most likely point of failure on a future Windows build is the `0xA21B` command id. If step 2 shows
the folder in a normal window after a ~3 s delay and the log says "did not open a new tab", that id has
changed.
