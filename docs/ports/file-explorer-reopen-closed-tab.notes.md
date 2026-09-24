# file-explorer-reopen-closed-tab - porting notes

Source written: `src/core/mods/explorer_reopen_closed_tab.cpp`
Original: Windhawk mod `file-explorer-reopen-closed-tab` v1.0.0 by Armaninyow.

## 1. Declaration

```c
SP_MOD_DECLARE(g_modExplorerReopenClosedTab);
```

Add it to the "File Explorer" group of `g_modTable[]` in `mod_table.c`, and
`<ClCompile Include="mods\explorer_reopen_closed_tab.cpp" />` to the `Mods` group of `core.vcxproj`.

## 2. Exception handling

Not needed. The file uses no C++/WinRT, only plain COM (`IShellBrowser`, `IShellView`, `IFolderView`,
`IPersistFolder2`) and Win32. Compile it like `explorer_double_click_up.cpp`, without `<ExceptionHandling>`.

Libraries: it pulls `Comctl32.lib`, `Shell32.lib` and `Ole32.lib` through `#pragma comment(lib, ...)`
(`ILGetSize`, `SHGetNameFromIDList`, `CoTaskMemFree`, `SetWindowSubclass`). If the project already links them,
the pragmas are harmless.

## 3. Settings read

| Name      | Type  | Default | Meaning |
|-----------|-------|---------|---------|
| `Enabled` | dword | 1       | The mod's on/off switch, read into `g_enabled` at Init and on SettingsChanged. When off, no tab close is recorded and Ctrl+Shift+T is passed through to Explorer untouched. |

There are no other settings. The original has none either (its history size of 50 and the 3 s wait for the
new tab are constants there and stay constants here: `kMaxHistory`, `kRestoreWindowMs`).

## 4. Proposed settings.reg lines and strings

Ids 1420-1429 are assigned; only 1420 is used.

```
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\file-explorer-reopen-closed-tab]
;b %R:1420% *
"Enabled"=dword:00000000
```

Put it under the File Explorer heading (`;a %R:1105%`), after `explorer-double-click-up`.

strings.h:

```c
#define IDS_MOD_REOPENTAB               1420
```

gui.en-US.rc:

```
    IDS_MOD_REOPENTAB               "Reopen the last closed File Explorer tab with Ctrl+Shift+T"
```

gui.tr-TR.rc:

```
    IDS_MOD_REOPENTAB               "Ctrl+Shift+T ile son kapatılan Dosya Gezgini sekmesini yeniden aç"
```

## 5. What was left out or changed, and why

The original is a Windhawk "tool mod": it runs in a dedicated explorer.exe process, polls `IShellWindows`
every 750 ms to diff the set of open tabs, installs a system-wide `WH_KEYBOARD_LL` hook from a third thread,
and reopens a tab by sending WM_COMMAND 0xA21B cross-process, then polling `IShellWindows` for the new tab
and calling `IWebBrowser2::Navigate2` on it. None of that shape fits an in-process engine, so the behaviour
was rebuilt around what the shell already tells us:

- **No polling threads, no `IShellWindows`, no COM marshaling.** `FileCabinet_CreateViewWindow2`
  (ExplorerFrame.dll, the same symbol `explorer_double_click_up.cpp` hooks; the engine chains both hooks)
  fires on every navigation with the tab's `IShellBrowser*` and the new `IShellView*`. The tab window is
  `IShellBrowser::GetWindow` (class `ShellTabWindowClass` under `CabinetWClass`); the folder is
  `IFolderView::GetFolder(IPersistFolder2)::GetCurFolder`. Each tab window is subclassed and `WM_NCDESTROY`
  is the close event. The close is seen the instant it happens instead of up to 750 ms later.
- **No low-level keyboard hook.** A `WH_KEYBOARD` hook scoped to each Explorer window's own thread (one per
  thread, dropped when the thread's last tab goes) sees Ctrl+Shift+T before Explorer's accelerators and
  consumes it. Nothing outside explorer.exe is affected and the system's input queue is never delayed.
  Auto-repeat is ignored (one tab per press), Alt must be up, and the foreground window must be a
  `CabinetWClass` of the same thread.
- **Pass-through when there is nothing to reopen.** The original always swallows Ctrl+Shift+T in Explorer;
  here the key is left to Explorer when the history is empty, so a future build that handles it natively is
  not blocked.
- **Reopening.** Same undocumented "New tab" command (WM_COMMAND 0xA21B, Ctrl+T's command) sent to the
  active tab window, from that window's own thread. The new tab's first view comes back through the same
  `FileCabinet_CreateViewWindow2` hook; a `PendingRestore` record (frame, folder, tick) matches it, and a
  posted message then calls the new tab's own `IShellBrowser::BrowseObject(pidl, SBSP_SAMEBROWSER |
  SBSP_ABSOLUTE)`. If no tab shows up within 3 s the folder is pushed back so the next press retries.
- **Item id lists instead of paths.** The original drops anything `SHGetPathFromIDListW` cannot name
  (This PC, Control Panel, libraries, search results, Home). Here the absolute pidl is copied, so every
  kind of folder reopens.
- **Closed windows are remembered too.** In the original, when the last Explorer window closes the whole
  history is wiped (a side effect of its "no tabs at all = new session" diffing). Here closing a window
  records each of its tabs, one per tab, so a window shut by mistake comes back a tab at a time, as in a
  browser. History is still capped at 50.
- **Windows open before the mod loaded** are adopted in `AfterInit` (`EnumWindows` over `CabinetWClass`,
  subclass via `SP_SetWindowSubclassFromAnyThread`, keyboard hook for their thread) so Ctrl+Shift+T works in
  them at once. Their folders are only learned on their next navigation, so a tab that is closed before it
  ever navigates again is not recorded. The original seeds from `IShellWindows`; the cost of that in
  process (CoCreateInstance + cross-apartment calls from the engine thread) was not worth this one gap.
- **Dropped the tool-mod launcher boilerplate** entirely (mutex, `-tool-mod` argv parsing,
  `CreateProcessInternalW`, entry-point hook): irrelevant in process.

Windows.UI.FileExplorer.dll / Taskbar.View.dll are not touched, so nothing goes through `SP_WaitForModule`;
ExplorerFrame.dll is always present in explorer.exe, and `LoadLibraryW` is the fallback as in
`explorer_double_click_up.cpp`.

Cleanup: `BeforeUninit` sets `g_enabled` false, removes every tab subclass through
`SP_RemoveWindowSubclassFromAnyThread` (lock not held while doing so), then unhooks the per-thread keyboard
hooks and clears all state. The engine removes the `FileCabinet_CreateViewWindow2` hook afterwards. `Uninit`
is null.

## 6. Verifying on a live shell

Turn on debug logging (`Logging` = 2) for the first run; every step writes a line under the tag
`file-explorer-reopen-closed-tab`.

1. Enable the mod, then open a **new** File Explorer window (a window that was already open is adopted but
   its tabs are unknown until they navigate; navigating once, e.g. to Documents, fixes that).
   Log: `Watching tab ... of window ...`, `Watching the keyboard of thread ...`, `Tab ... shows C:\...`.
2. Open two or three tabs (Ctrl+T) and navigate each to a different folder, e.g. `C:\Windows`,
   `C:\Users`, This PC.
3. Close one tab with Ctrl+W or its X. Log: `Tab ... closed at C:\Windows; 1 remembered`.
4. Press **Ctrl+Shift+T** with the Explorer window in front. A new tab should open (it shows Home for a
   moment) and land on the folder you just closed, in the same window, as the active tab.
   Log: `Reopening C:\Windows in window ...` then `Tab ... taken to C:\Windows`.
5. Close two tabs in a row, press Ctrl+Shift+T twice: they come back most-recent first.
6. Close a tab that shows This PC and reopen it: it must come back as This PC (the original could not do
   this).
7. Close an entire window that has several tabs, open a fresh window, press Ctrl+Shift+T repeatedly: the
   closed window's tabs come back one by one.
8. Negative checks: with nothing closed, Ctrl+Shift+T does nothing and is passed on; with a dialog
   (e.g. Properties) or another app in front, Ctrl+Shift+T is not intercepted; holding the key down
   reopens one tab, not many.
9. Turn the mod off in the settings window: tabs closed afterwards are not recorded and Ctrl+Shift+T is
   left to Explorer. Turn it back on: works again without a shell restart.

Things worth watching in the first live run (the two assumptions this port rests on):

- `IShellBrowser::GetWindow` on the browser handed to `FileCabinet_CreateViewWindow2` must be the
  `ShellTabWindowClass` window. If the hook is entered but no `Watching tab` line ever appears, that is
  where to look (add a debug line printing the class name of `hTab`).
- The new tab's Home page must come through `FileCabinet_CreateViewWindow2`. If `Reopening ...` is logged,
  a new tab opens, but it stays on Home and 3 s later a press logs `No new tab appeared for the last
  restore`, then Home on this build is not a DefView view and the pending record never matched; the
  fallback would be to find the new `ShellTabWindowClass` child of the frame directly after the command
  and reach its browser another way.
