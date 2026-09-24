# taskbar-count-badges - integration notes

Source: `src/core/mods/taskbar_count_badges.cpp`
Based on the Windhawk mod `taskbar-count-badges` v1.1.0 by digART (GPL-3.0).

## 1. SP_MOD_DECLARE symbol

`g_modTaskbarCountBadges`

Suggested placement in `mod_table.c`: the "Taskbar" group, next to `g_modTaskbarLabels`. Order does not matter;
it uses no shared gestures and no export hooks. It hooks the same `TaskListButton::UpdateVisualStates` as
`taskbar-labels`; the engine chains the two detours and neither cares about the other (this one only remembers
the button and queues work to the button's dispatcher, it changes nothing inside the call).

## 2. ExceptionHandling

**Yes.** The badge is built with C++/WinRT (`Windows.UI.Xaml`, `Windows.UI.Xaml.Controls`, `Windows.UI.Core`
for `CoreDispatcher`), which throws. Add to `core.vcxproj` like `taskbar_labels.cpp`:

```
    <ClCompile Include="mods\taskbar_count_badges.cpp">
      <ExceptionHandling>Sync</ExceptionHandling>
    </ClCompile>
```

No `__try/__except`, no WinUI 2 (`Microsoft.UI.Xaml`), no `Windows.UI.Composition` (the original's composition
expression animations for the dots are not ported; see section 5). `<unknwn.h>` is included before the first
winrt header, per the brief.

## 3. Settings read

All under `HKCU\Software\ShadePatcher\Mods\taskbar-count-badges`, all re-read on `SettingsChanged` and applied
live (every remembered button redraws its badge on its own taskbar thread; no rebuild, no flicker).

| Name | Type | Default | Meaning |
|------|------|---------|---------|
| `Style` | dword | 0 | 0 = number badge (a filled circle with the count, a pill for two digits, `99+` beyond). 1 = dots (one per window, up to five; five means five or more). Other values = 0. |
| `Position` | dword | 0 | Which side of the icon. 0 = right, 1 = left, 2 = top, 3 = bottom. Number badge: 0/1 are the top-right / top-left corner, 2/3 the middle of the top / bottom edge. Dots: 0/1 a vertical stack beside the icon, 2/3 a horizontal row above / below it. Other values = 0. The GUI list below offers 0..2 only (ten ids); 3 is accepted from the registry and needs one more id if it should be listed. |
| `MinimumCount` | dword | 2 | The badge is shown from this many windows. 1 = every running button gets one; 2 = a single window has no badge (the original's default). Values outside 1..99 fall back to 2. |
| `BadgeColor` | sz | "" | `RRGGBB` (also accepts `#RRGGBB` and `AARRGGBB`). Number badge: the circle's fill; the digits are black or white by the fill's brightness. Dots: the dot colour. Empty = default: the original's red `D90000` for the number, the theme's text colour (`TextFillColorPrimaryBrush`, white if unreadable) for the dots. Unparsable = default, logged at error level. |

Mapping from the original: `Display.style` number/dots -> `Style` 0/1; `Badge.position` topRight/topLeft ->
`Position` 0/1 (topCenter -> 2, the three bottom corners are not offered, bottom centre is 3);
`VerticalDots.position` right/left/top/bottom -> `Position` 0/1/2/3; `Behavior.minimumCount` -> `MinimumCount`;
`Badge.backgroundColor` and `VerticalDots.color` -> the one `BadgeColor`.

## 4. Proposed settings.reg lines and strings

Ids 1630-1639, all ten used. No `*` restart marker: everything applies live. The minimum-count list uses
literal digits as labels (language-neutral, like the `NN px` entries of taskbar-labels). The colour is a `;w`
text input whose prompt reuses the label id; its default line is empty because the mod treats empty as
"default colour".

```
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-count-badges]
;b %R:1630%
"Enabled"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-count-badges]
;c 2 %R:1631%
;x 0 %R:1632%
;x 1 %R:1633%
"Style"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-count-badges]
;c 3 %R:1634%
;x 0 %R:1635%
;x 1 %R:1636%
;x 2 %R:1637%
"Position"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-count-badges]
;c 5 %R:1638%
;x 1 1
;x 2 2
;x 3 3
;x 4 4
;x 5 5
"MinimumCount"=dword:00000002
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-count-badges]
;w %R:1639%
;%R:1639%
;
"BadgeColor"=""
```

Suggested place: the Taskbar section, after the `taskbar-labels` block.

`strings.h`:

```
#define IDS_MOD_TASKBARCOUNTBADGES              1630
#define IDS_MOD_TASKBARCOUNTBADGES_STYLE        1631
#define IDS_MOD_TASKBARCOUNTBADGES_STYLE_0      1632
#define IDS_MOD_TASKBARCOUNTBADGES_STYLE_1      1633
#define IDS_MOD_TASKBARCOUNTBADGES_POSITION     1634
#define IDS_MOD_TASKBARCOUNTBADGES_POSITION_0   1635
#define IDS_MOD_TASKBARCOUNTBADGES_POSITION_1   1636
#define IDS_MOD_TASKBARCOUNTBADGES_POSITION_2   1637
#define IDS_MOD_TASKBARCOUNTBADGES_MINCOUNT     1638
#define IDS_MOD_TASKBARCOUNTBADGES_COLOR        1639
```

`lang/gui.en-US.rc`:

```
    IDS_MOD_TASKBARCOUNTBADGES              "Show the window count on taskbar buttons"
    IDS_MOD_TASKBARCOUNTBADGES_STYLE        "Show the count as"
    IDS_MOD_TASKBARCOUNTBADGES_STYLE_0      "A number badge (default)"
    IDS_MOD_TASKBARCOUNTBADGES_STYLE_1      "Dots, one per window (up to five)"
    IDS_MOD_TASKBARCOUNTBADGES_POSITION     "Where on the icon"
    IDS_MOD_TASKBARCOUNTBADGES_POSITION_0   "Right (default)"
    IDS_MOD_TASKBARCOUNTBADGES_POSITION_1   "Left"
    IDS_MOD_TASKBARCOUNTBADGES_POSITION_2   "Top"
    IDS_MOD_TASKBARCOUNTBADGES_MINCOUNT     "Show from this many windows"
    IDS_MOD_TASKBARCOUNTBADGES_COLOR        "Badge colour as RRGGBB (empty for the default)"
```

`lang/gui.tr-TR.rc`:

```
    IDS_MOD_TASKBARCOUNTBADGES              "Görev çubuğu düğmelerinde pencere sayısını göster"
    IDS_MOD_TASKBARCOUNTBADGES_STYLE        "Sayı nasıl gösterilsin"
    IDS_MOD_TASKBARCOUNTBADGES_STYLE_0      "Sayı rozeti (varsayılan)"
    IDS_MOD_TASKBARCOUNTBADGES_STYLE_1      "Noktalar, pencere başına bir (en fazla beş)"
    IDS_MOD_TASKBARCOUNTBADGES_POSITION     "Simgenin neresinde"
    IDS_MOD_TASKBARCOUNTBADGES_POSITION_0   "Sağ (varsayılan)"
    IDS_MOD_TASKBARCOUNTBADGES_POSITION_1   "Sol"
    IDS_MOD_TASKBARCOUNTBADGES_POSITION_2   "Üst"
    IDS_MOD_TASKBARCOUNTBADGES_MINCOUNT     "Kaç pencereden itibaren gösterilsin"
    IDS_MOD_TASKBARCOUNTBADGES_COLOR        "Rozet rengi, RRGGBB olarak (varsayılan için boş bırakın)"
```

## 5. What changed versus the original, and what was left out

Kept (the core):

- **The count per button**, read exactly the way the original reads it: `TryGetItemFromContainer<TaskListGroupViewModel>`
  (resolved, not hooked) maps the button element to its group view model, and
  `produce<TaskListGroupViewModel, ITaskListGroupViewModel>::get_ViewModelCount` gives the count. The reported
  value is one more than the number of windows (the original verified that on 24H2; kept as is: `count - 1`).
  **Verify on 26200** (section 6, step 2); if the badge is off by one, change `WindowCountOf` in the source.
- **Two styles**: number in a circle (pill for two digits, `99+` cap) and up to five dots; **four sides**;
  **minimum count**; **one colour**. Live apply of every setting.
- **Where the badge is triggered**: `TaskListButton::UpdateVisualStates` (hooked) remembers the button;
  `get_ViewModelCount` (hooked, notification only) queues a refresh when a count changes. Both from the
  original. The refresh is coalesced per taskbar thread and stays marked as queued until it has run, so the
  count reads caused by its own XAML work collapse into it (the original's `g_deferredCountRefreshQueued`).
- **The badge element** is named (`ShadePatcherCountBadge`) so a leftover from an earlier instance is taken
  over or removed rather than duplicated (the original's recovery of `WindhawkCountBadge`).
- **Recycled containers**: an entry whose element died at the same address is started over (the original's
  `TrackTaskbarButton` rule).
- **Unload** removes every badge on each taskbar's own thread, waits up to 3 s per taskbar, and `Uninit` waits
  (up to 2 s) for queued dispatcher callbacks to drain before the code goes away.

Changed:

- **Every XAML change is deferred** to the button's `CoreDispatcher` (Normal priority) instead of being done
  inside `UpdateVisualStates`. The original mutated the tree inside the call and only deferred the
  `get_ViewModelCount` path; deferring both is the pattern `taskbar_start_button_position.cpp` already uses
  ("changing a margin during arrange would re-enter the layout"). Cost: the badge appears one dispatcher tick
  after the button's own update.
- **Existing buttons** (mod enabled while the taskbar is up): the original walked taskbar.dll's
  `CTaskBand::GetTaskbarHost` / `TaskbarHost::FrameHeight` (with byte-pattern sniffing of the function
  prologue to find the XamlRoot offset) and `std::_Ref_count_base::_Decref`. Not ported: six taskbar.dll
  symbols and an instruction-pattern dependency for a one-time sweep. Instead, the first `UpdateVisualStates`
  on any button of a taskbar sweeps its sibling `TaskListButton`s in the same repeater, and a plain
  `WM_SETTINGCHANGE` is sent to `Shell_TrayWnd` / `Shell_SecondaryTrayWnd` once the hooks are in. If the nudge
  does not make the buttons update on this build, the badges appear on the first hover / window change
  (step 1 in section 6 says what to expect).
- **Dots live inside `IconPanel`** like the number badge (column 0 of the panel's grid, so they stay on the
  icon column when labels are on), aligned to the panel's edge, 1 px inset. The original put them in the
  taskbar's `RootGrid` with two compositor expression animations (Translation over the layout chain, and a
  copy of the button's `TransformMatrix`) to escape `IconPanel` clipping and follow the button's animations.
  Inside the panel there is nothing to clip and nothing to follow, so ~200 lines of `Windows.UI.Composition`
  went away. The dots sit 3-4 px from the icon rather than the original's 2 px gap outside it.
- **Dot colour default** follows the theme (`TextFillColorPrimaryBrush`) instead of hard white, so dots are
  visible on a light taskbar; a set `BadgeColor` overrides it. The number's text is black or white by the
  fill's brightness instead of a separate text-colour setting.

Left out, and why:

- **Badge shape (circle / rounded / square), size, X/Y offsets, border colour and thickness, font family,
  font size, font weight, text colour, maximum number** (`Badge.*`, `Text.*`, `Behavior.maximumNumber`):
  cosmetic knobs; fixed at the original's defaults (16 px circle, 10 px semi-bold, white/black text, 99+).
- **Dot size** (`VerticalDots.size`): fixed at 4 px, 2 px apart (the original's defaults).
- **Bottom dots replacing the running indicator** (the 1.1.0 feature: `RunningIndicator` hidden at the
  composition layer, dots anchored to its bounds, minimum count forced to 1, restore on unload): it needs
  `Windows.UI.Composition` and the RootGrid placement above, and it leaves a hidden indicator behind if the
  unload cannot reach the taskbar thread (the original documents that). `Position=3` here puts the dots at the
  bottom of the icon panel and leaves the indicator alone.
- **Bottom corner positions for the number badge** (`bottomLeft`, `bottomRight`): they sit on the running
  indicator; bottom centre (3) is kept for completeness but not in the GUI list.
- **The `LoadLibraryExW` hook and `ExplorerExtensions.dll` fallback** for late Taskbar.View loading:
  `SP_WaitForModule` does that.
- **The `WH_CALLWNDPROC` + `SendMessage` "run on taskbar thread" machinery and the `Shell_TrayWnd` subclass
  with a registered message**: replaced by the elements' own `CoreDispatcher`, which also handles secondary
  taskbars (each has its own thread) without a per-window subclass.

Other notes for the integrator:

- All three symbol hooks are in one `SP_HookSymbols` batch, all `optional`; the mod stays inert (logged at
  error level, one line per missing symbol) unless all three resolved, because each is needed for anything to
  show. The success line names what was hooked and what was only resolved.
- The `(void**)pThis + 3` idiom for reaching the XAML object from a `TaskListButton` implementation pointer is
  the one in `taskbar_labels.cpp`; a null slot is checked.
- `TryGetItemFromContainer` returns a C++/WinRT object, so it is called with a hidden result slot first and a
  pointer to the ABI pointer of the `UIElement` (what `UIElement const&` is on the wire); the result is held
  in a `winrt::com_ptr<::IUnknown>` and released after the count is read.
- The remembered-button list holds only weak references and each button's `CoreDispatcher`; entries are
  selected per thread with `HasThreadAccess`, and no XAML object is resolved from a foreign thread.
- `SettingsChanged` returns at once (the redraw is queued); `BeforeUninit` blocks up to 3 s per taskbar
  thread; `Uninit` up to 2 s. No `AfterInit`.
- `minOsBuild` 22000 (any XAML taskbar). On this machine (26200, Taskbar.View.dll 2607) the three symbol
  spellings are the original's; none of them is a `consume_...` wrapper, so the lesson about those does not
  apply here.

## 6. How to verify on a live shell

1. Enable the mod (settings window, or `Enabled=1` under `HKCU\Software\ShadePatcher\Mods\taskbar-count-badges`).
   Have two or more windows of one program open (two Explorer windows, two Notepad windows) with Windows'
   "Combine taskbar buttons" on Always. Within a second the combined button shows a small red circle with "2"
   at the top-right of its icon. If nothing appears at once, hover any taskbar button: the first hover sweeps
   the taskbar and every qualifying button gets its badge.
2. Count check: open a third window of that program: the badge reads "3". Close one: "2". Close another: the
   badge disappears (one window, default `MinimumCount` 2). A pinned program that is not running never gets a
   badge. If the numbers are one too high or low, `WindowCountOf` in the source is where the `- 1` lives.
3. Set `MinimumCount=1`: every running button gets a "1" badge without a restart; back to 2 removes them.
4. Set `Style=1`: the badge becomes a vertical stack of dots to the right of the icon, one per window (open
   six windows: five dots). Set `Position=2`: a horizontal row above the icon. `Position=1`: stack on the
   left. Back to `Style=0`, `Position=1`: number at the top-left.
5. Set `BadgeColor=0078D4`: blue badge, white digits; `FFD700`: yellow badge with black digits; with `Style=1`
   the dots take the colour. Empty: red badge, theme-coloured dots. `BadgeColor=xyz`: default colour and an
   error line in the log.
6. With labels on (Windows "Combine" set to Never, or the taskbar-labels mod): the badge stays on the icon,
   not on the label. Open the overflow flyout (many windows): its buttons are TaskListButtons too and get
   badges as well.
7. Secondary monitor with its own taskbar: its buttons get badges on the same terms, refreshed on their own
   thread.
8. Disable the mod: every badge is gone within a second, no Explorer restart, the buttons look stock.
9. With `Logging=2` under `HKCU\Software\ShadePatcher`: `Count badge hooks are in: ...`, `Swept N button(s)
   on this taskbar`, `Nudged N taskbar window(s)`, on unload `Removed N badge(s) from M button(s) on this
   taskbar`, all tagged `taskbar-count-badges`. A missing symbol shows as an error line naming it.
