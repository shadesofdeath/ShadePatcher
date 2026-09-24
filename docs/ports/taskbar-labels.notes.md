# taskbar-labels - integration notes

Source: `src/core/mods/taskbar_labels.cpp`
Based on the Windhawk mod `taskbar-labels` v1.4.5 by m417z.

## 1. SP_MOD_DECLARE symbol

`g_modTaskbarLabels`

Suggested placement in `mod_table.c`: the "Taskbar" group, next to `g_modTaskbarButtonClick`. Order does not
matter; it has no shared gestures. It hooks kernelbase `RegGetValueW` like `tray-show-all-icons` does; the two
hooks chain through the engine's ownership and do not care about each other (this one only touches
`TaskbarGlomLevel` / `MMTaskbarGlomLevel`, and only during its own refresh).

## 2. ExceptionHandling

**Yes.** The layout pass uses C++/WinRT (`Windows.UI.Xaml` / `Windows.UI.Xaml.Controls`), which throws. Add it
to `core.vcxproj` like `taskbar_menu_entry.cpp`:

```
    <ClCompile Include="mods\taskbar_labels.cpp">
      <ExceptionHandling>Sync</ExceptionHandling>
    </ClCompile>
```

The file also contains one `__try/__except` (calling the wil feature gate, in a helper with no C++ objects) so it
does not hit C2712. Only `Windows.UI.Xaml` headers are used, no WinUI 2 (`Microsoft.UI.Xaml`) projection needed:
the original's ItemsRepeater walk belonged to the pre-native-labels path, which is not ported.

## 3. Settings read

All under `HKCU\Software\ShadePatcher\Mods\taskbar-labels`, all `dword`, all re-read on `SettingsChanged`:

| Name | Default | Meaning |
|------|---------|---------|
| `Mode` | 0 | 0 = labels on every button, combined or not (Windows' own "combine taskbar buttons" choice is left alone; combined groups get a label too). 1 = Windows decides, as if the mod were not there (only the width limits apply). 2 = no labels on any button. Any other value is treated as 0. |
| `MinimumTaskbarItemWidth` | 50 | The narrowest a labelled button may get before the taskbar overflows, in DIPs. Only ever *lowers* Windows' own floor (a value above it has no effect, as in the original). 0 = leave Windows' value. Values outside 0..1000 fall back to 50. |
| `MaximumTaskbarItemWidth` | 176 | The widest a labelled button may grow, in DIPs (label capped, ellipsis on overflow). 176 is what Windows itself uses. 0 = leave Windows' value (label only gains the ellipsis). Values outside 0..2000 fall back to 176. |

Mapping from the original's four modes: `labelsWithCombining` ~ Mode 0, `labelsWithoutCombining` ~ Mode 1 with
Windows set to "Never combine" (the port does not force the combining choice; see section 5),
`noLabelsWithCombining` ~ Mode 1 with Windows set to "Always", `noLabelsWithoutCombining` ~ Mode 2.

## 4. Proposed settings.reg lines and strings

Ids 1370-1377 used; 1378-1379 free. No `*` restart marker: every change is applied live by rebuilding the
taskbar buttons (see section 5). The settings window has no numeric edit control, so both widths are offered as
drop-downs of sensible values; the item labels are literal `NN px` (language-neutral) except the "Windows
default" entry (1377), which is shared by both lists. If literal text after `;x value` is not accepted by the
parser, replace them with string ids from 1378+.

```
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-labels]
;b %R:1370%
"Enabled"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-labels]
;c 3 %R:1371%
;x 0 %R:1372%
;x 1 %R:1373%
;x 2 %R:1374%
"Mode"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-labels]
;c 5 %R:1375%
;x 0 %R:1377%
;x 30 30 px
;x 50 50 px
;x 80 80 px
;x 100 100 px
"MinimumTaskbarItemWidth"=dword:00000032
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-labels]
;c 7 %R:1376%
;x 0 %R:1377%
;x 120 120 px
;x 140 140 px
;x 160 160 px
;x 176 176 px
;x 200 200 px
;x 240 240 px
"MaximumTaskbarItemWidth"=dword:000000b0
```

Suggested place: the Taskbar section, after the `taskbar-button-click` block.

`strings.h`:

```
#define IDS_MOD_TASKBARLABELS           1370
#define IDS_MOD_TASKBARLABELS_MODE      1371
#define IDS_MOD_TASKBARLABELS_MODE_0    1372
#define IDS_MOD_TASKBARLABELS_MODE_1    1373
#define IDS_MOD_TASKBARLABELS_MODE_2    1374
#define IDS_MOD_TASKBARLABELS_MINWIDTH  1375
#define IDS_MOD_TASKBARLABELS_MAXWIDTH  1376
#define IDS_MOD_TASKBARLABELS_WINDEFAULT 1377
```

`lang/gui.en-US.rc`:

```
    IDS_MOD_TASKBARLABELS           "Show labels on taskbar buttons"
    IDS_MOD_TASKBARLABELS_MODE      "When to show labels"
    IDS_MOD_TASKBARLABELS_MODE_0    "Always, even on combined buttons (default)"
    IDS_MOD_TASKBARLABELS_MODE_1    "Only when buttons are not combined (Windows setting)"
    IDS_MOD_TASKBARLABELS_MODE_2    "Never"
    IDS_MOD_TASKBARLABELS_MINWIDTH  "Narrowest a button may get before the taskbar overflows"
    IDS_MOD_TASKBARLABELS_MAXWIDTH  "Widest a button may grow"
    IDS_MOD_TASKBARLABELS_WINDEFAULT "Windows default"
```

`lang/gui.tr-TR.rc`:

```
    IDS_MOD_TASKBARLABELS           "Görev çubuğu düğmelerinde etiketleri göster"
    IDS_MOD_TASKBARLABELS_MODE      "Etiketler ne zaman gösterilsin"
    IDS_MOD_TASKBARLABELS_MODE_0    "Her zaman, birleştirilmiş düğmelerde de (varsayılan)"
    IDS_MOD_TASKBARLABELS_MODE_1    "Yalnızca düğmeler birleştirilmediğinde (Windows ayarı)"
    IDS_MOD_TASKBARLABELS_MODE_2    "Hiçbir zaman"
    IDS_MOD_TASKBARLABELS_MINWIDTH  "Görev çubuğu taşmadan önce bir düğme en fazla ne kadar daralsın"
    IDS_MOD_TASKBARLABELS_MAXWIDTH  "Bir düğme en fazla ne kadar genişlesin"
    IDS_MOD_TASKBARLABELS_WINDEFAULT "Windows varsayılanı"
```

## 5. What changed versus the original, and what was left out

Kept (the core of the mod on a taskbar with Windows' own labels implementation, i.e. 22621.2361+ / 26200):

- **Label on/off by mode** through the three `Taskbar.View.dll` hooks the original uses:
  `consume_Taskbar_ITaskbarAppItemViewModel<...>::HasLabel` (the wrapper; only calls that pass through it are
  answered differently, exactly like the original's `g_inITaskbarAppItemViewModel_HasLabels` gate, here a
  `thread_local` counter), `produce<TaskListWindowViewModel, ITaskbarAppItemViewModel>::get_HasLabel` and
  `produce<TaskListGroupViewModel, ITaskbarAppItemViewModel>::get_HasLabel`.
- **Minimum width** via `produce<TaskListButton, ITaskbarButton>::get_MinScalableWidth`, same rule as the
  original (only lowered, never raised).
- **Maximum width and ellipsis** in the `TaskListButton::UpdateVisualStates` hook: `LabelControl.MaxWidth` =
  max - icon column width - label margins (the original's adaptive-width branch, `taskbarItemWidth == 0`),
  `TextTrimming = CharacterEllipsis`, `InvalidateMeasure` only when the value actually changed (the original's
  loop guard). Only for buttons under `TaskbarFrameRepeater` and only for the two-column `IconPanel`.
- **Pinned programs in Mode 0** get their (forced) label collapsed when `get_IsRunning` says not running,
  the original's `labelsWithCombining` rule. `get_IsRunning` is resolved, not hooked, and optional.
- **Live apply** the original's way: `WM_SETTINGCHANGE` to `Shell_TrayWnd` while `RegGetValueW`
  (kernelbase) answers the opposite `TaskbarGlomLevel`, 400 ms, `WM_SETTINGCHANGE` again. Used from the
  `SP_WaitForModule` callback (for buttons that already existed), from `SettingsChanged` and from
  `BeforeUninit` (with `g_unloading` set so every hook answers as Windows would and the layout pass clears
  `MaxWidth` / `TextTrimming` with `ClearValue`, followed by the original's 400 ms grace for Store-app buttons).
  `SendMessageTimeoutW` (5 s, abort-if-hung) instead of `SendMessage`, so a stuck shell thread cannot hold the
  engine thread.
- The wil feature gate check (`Feature_29785186`, both pre- and post-KB5036980 spellings, optional): if both
  symbols exist and the gate is shut, the mod logs and stays inert. Missing symbols count as "enabled", which
  is right for 26200 where the implementation is no longer gated.

Left out, and why:

- **The pre-native-labels implementation** (custom `WindhawkText` TextBlock, `taskbar.dll` hooks
  `CTaskListWnd::GroupChanged` / `TaskDestroyed` / `CTaskGroup::GetTitleText` / `IconContainer`,
  `TaskListButton::Icon`, `UpdateButtonPadding`, `UpdateBadgeSize`, `TaskbarFrame::OnTaskbarLayoutChildBoundsChanged`,
  `CalculateTaskbarItemWidth`, the `LoadLibraryExW` hook, the `%name%`/`%amount%` label formats). Scope is
  22621.2361+ only; `minOsBuild` is 22621 and on a 22621 without the labels update the mod does nothing
  (logged).
- **Forcing the combining choice** (the original's `RegGetValueW` rewrite of `TaskbarGlomLevel` in its
  `labelsWithoutCombining` / `*WithCombining` modes). The requested Mode semantics leave combining to Windows;
  the registry hook is kept only for the refresh trick and passes everything through otherwise. Users who want
  "labels, never combine" set Windows' "Combine taskbar buttons" to Never and use Mode 0 or 1.
- **Fixed item width** (`taskbarItemWidth`, the `WindhawkLabelSpacer` Border) - only the limits were asked for.
- **Running / progress indicator styles**, `runningIndicatorHeight`, `runningIndicatorVerticalOffset`: about 120
  lines of margin arithmetic against the icon column; not cheap, and the native look (indicator under the icon)
  is what the original restores on unload anyway.
- **Font size, font family, text trimming choice, paddings** (`fontSize`, `fontFamily`, `textTrimming`,
  `leftAndRightPaddingSize`, `spaceBetweenIconAndLabel`): with the original's defaults they reproduce Windows'
  own layout; the port keeps Windows' margins/icon alignment and hardcodes `CharacterEllipsis`.
- **Excluded programs** (`excludedPrograms[]`, `GetWindowAppId`, the `ITaskListWindowViewModel` vftable walk):
  an array setting with no equivalent here, and a per-program table is exactly the kind of extra the brief says
  to drop.
- **`alwaysShowThumbnailLabels`** (`CTaskListThumbnailWnd::DisplayUI` in taskbar.dll): the original marks it as
  removed around 26100.8491; not present on this machine's builds.

Other notes for the integrator:

- Every symbol hook is `optional`; the callback returns FALSE (mod inert, logged) only when neither
  `UpdateVisualStates` nor `get_MinScalableWidth` nor a usable HasLabel pair resolved. Each missing one is logged
  at error level with what it costs.
- The `(void**)pThis + 3` idiom for reaching the XAML object from a `TaskListButton` implementation pointer is
  the original's (and every m417z Taskbar.View.dll mod's); a null slot is checked before it is used.
- No `AfterInit`: the `SP_WaitForModule` callback does the first rebuild itself once the hooks are in. On a
  cold sign-in there may be no `Shell_TrayWnd` yet at that point, which is fine: every button is still to be
  created and goes through the hooks.
- `SettingsChanged` blocks the engine thread for ~400 ms plus two cross-thread sends; same as the original's
  `Wh_ModSettingsChanged`.

## 6. How to verify on a live shell

Precondition: Windows Settings > Personalization > Taskbar > Taskbar behaviors > "Combine taskbar buttons and
hide labels". Note its current value; try the steps with "Always" and again with "Never".

1. Enable the mod (settings window, or `Enabled=1` under `HKCU\Software\ShadePatcher\Mods\taskbar-labels`).
   Within ~1 s the taskbar buttons rebuild (they flicker once).
   - Windows set to "Always": running programs now show a title next to the icon (Mode 0 default). Pinned
     programs that are not running keep icon only.
   - Windows set to "Never": labels were already there; long titles now end in "..." instead of being cut
     mid-letter.
2. Open a window with a long title (a browser tab with a long page name). The button grows to at most
   176 DIPs and the title is trimmed with an ellipsis. Set `MaximumTaskbarItemWidth` to 120: after the rebuild
   the same button is narrower. Set it to 0: Windows' own clipping at ~176 returns (no ellipsis limit change).
3. Open ~12-15 windows. With `MinimumTaskbarItemWidth=50` the labelled buttons shrink well below Windows' floor
   before the overflow chevron appears; set it to 0 and the overflow appears much earlier (Windows' own floor).
4. Set `Mode=2`: after the rebuild every button is icon-only, whatever the Windows setting is.
   Set `Mode=1`: buttons follow the Windows setting exactly (labels only with "Never"/"When taskbar is full").
   Set `Mode=0` with Windows on "Always": combined groups (e.g. two Explorer windows) show one labelled button.
5. Disable the mod: the buttons rebuild once more and look exactly like stock Windows for the current
   combining setting (no ellipsis, Windows widths). No Explorer restart at any step.
6. With `Logging=2` under `HKCU\Software\ShadePatcher`: `Taskbar label hooks are in`, `Taskbar buttons
   rebuilt`, and during each rebuild two `Answering TaskbarGlomLevel with N instead of M` lines, all tagged
   `taskbar-labels`. A missing symbol shows as an error line naming the feature it costs.
