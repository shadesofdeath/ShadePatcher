# desktop-icon-selection-style - integration notes

Source: `src/core/mods/desktop_icon_selection_style.cpp`
Original: Windhawk `desktop-icon-selection-style` v1.0.0 by RiteshK

## 1. Declaration

```c
SP_MOD_DECLARE(g_modDesktopIconSelectionStyle);
```

`SP_MOD_ID` is `"desktop-icon-selection-style"`. `minOsBuild` 22000, `targets` `SP_TARGET_EXPLORER`, `flags` 0.

## 2. Exception handling

Not needed. No C++/WinRT, nothing throws. GDI+ (the C++ wrapper classes in `<gdiplus.h>`) reports failures
through status codes, not exceptions. No STL containers are used, only `<algorithm>`, `<atomic>`, `<cmath>`.

Libraries are pulled in with `#pragma comment(lib, ...)` inside the file: `Comctl32.lib`, `Dwmapi.lib`,
`Gdi32.lib`, `Gdiplus.lib`, `UxTheme.lib`. Nothing to add to `core.vcxproj` beyond the `ClCompile` line.

One thing to know about the build: the project defines `NOMINMAX`, and the GDI+ headers use unqualified
`min`/`max`. The file works around that with

```cpp
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
```

which is the standard fix. If the compiler still complains inside `gdiplustypes.h`, that is the place to look.

## 3. How the selection is drawn on this build, and which path the port takes

Investigated from the original and from the Windows ListView behaviour:

- The desktop list view is not owner drawn (no `LVS_OWNERDRAWFIXED`), and `SHELLDLL_DefView` does not custom
  draw the item background (it only uses `NM_CUSTOMDRAW` for text colours). comctl32 paints the plate behind a
  selected/hovered item itself, with the **ListView theme class, part `LVP_LISTITEM` (1), states
  `LISS_HOT` (2) / `LISS_SELECTED` (3) / `LISS_SELECTEDNOTFOCUS` (5) / `LISS_HOTSELECTED` (6)**, through
  uxtheme's `DrawThemeBackground`. The original author measured exactly this on **build 26200** (the same build
  as this machine) and also measured that the draws arrive on a memory DC (double buffered), so `WindowFromDC`
  is null and cannot be used to identify the desktop.
- So the port hooks **`uxtheme!DrawThemeBackground` and `uxtheme!DrawThemeBackgroundEx`** (exports, one
  transaction together with `user32!CreateWindowExW`). `DrawThemeBackground` is a thin wrapper over the Ex
  function; the original hooked only the wrapper and documents that a build calling the inner function
  directly would leave it inert. Both are hooked here, with a thread-local re-entrancy flag so a call that
  passes through the outer hook is not examined a second time in the inner one.
- The paint gate is the same as the original's: the desktop `SysListView32` is subclassed and a thread-local
  depth counter is incremented around `WM_PAINT`, `WM_PRINTCLIENT` and `WM_ERASEBKGND` (not `WM_NCPAINT`: the
  scroll bar the list view grows when icons fall outside the work area uses the ScrollBar class, whose part 1
  / states 2..6 collide with the list item states). A `GetThemeClass` (uxtheme ordinal 74) check additionally
  refuses draws whose class ends in ScrollBar/Edit/Header/Button, fail-open.
- **Backstop for the concern from the other port** (labels not reaching `DrawTextW` on 26200): while the
  subclass has *never* seen the desktop paint in this session, a matching `LVP_LISTITEM` draw arriving on the
  desktop list view's own thread is still taken (thread-id match). The moment the subclass sees its first
  paint message that fallback is off for good. In the normal case it never fires.

### Info-level log lines to look for (`Logging=1`)

| Line | Meaning |
|------|---------|
| `Hooked uxtheme DrawThemeBackground, DrawThemeBackgroundEx and user32 CreateWindowExW` | Init succeeded. |
| `Watching the desktop list view 0x... on thread N` | Subclass installed (AfterInit or CreateWindowExW). |
| `The desktop list view 0x... is painting through the subclass` | The paint gate works (first paint seen). |
| `Selection painting hook hit: DrawThemeBackground LVP_LISTITEM state 3 replaced on the desktop` | **The hook is being hit and the plate was replaced.** Says which of the two entry points was used. Logged once. |
| `The desktop painted a selected icon but no LVP_LISTITEM draw reached DrawThemeBackground or DrawThemeBackgroundEx: this build draws the selection another way and the mod is inert` | Diagnosis for the bad case: the subclass saw a paint whose update rect covers a selected item, yet neither hook got a list item draw during it. Logged once. |
| `A list item draw arrived on the desktop thread before the subclass saw any paint message; taking it by thread id ...` | The fallback gate kicked in: hooks work but the subclass does not see the paint (should not happen; tells you where to look). |
| `Standing down: N selection plate(s) restyled, M list item draw(s) seen outside a paint message, subclass saw paint: yes/no` | Totals at BeforeUninit. `M > 0` with `subclass saw paint: yes` means some selection draws happen outside WM_PAINT on this build (they were left stock). |

If the "hook hit" line never appears and the "inert" line does, the selection on this build is drawn by
something other than the uxtheme background functions (candidates to try next: `DrawThemeBackground` reached
through a non-exported internal entry, a DirectComposition/Direct2D path, or the DefView custom drawing the
item). That is the same failure mode the original documents ("inert, not broken").

## 4. Settings read (HKCU\Software\ShadePatcher\Mods\desktop-icon-selection-style)

| Name             | Type  | Default | Meaning / allowed values |
|------------------|-------|---------|--------------------------|
| `Enabled`        | dword | 0       | Engine toggle (read by the engine). |
| `CornerRadius`   | dword | 8       | Corner rounding in logical pixels, DPI scaled. 0 = sharp. Clamped 0..256; capped at half the plate's smaller side. |
| `Opacity`        | dword | 40      | Fill opacity 0..100 (0 = no fill, 100 = solid). Also used for the hover plate. |
| `BorderStyle`    | dword | 2       | 0 = none, 1 = solid, 2 = dashed, 3 = dotted. Out of range falls back to 2. The border is white at 70 %, 1 px (scaled), dash 2 px / gap 2 px. Hover never gets a border, only a selection does. |
| `Padding`        | dword | 4       | Logical pixels trimmed from **every** side of the icon cell, DPI scaled. 0 = the stock size (adjacent selections touch). Clamped 0..256 and never shrinks the plate below 4 px. |
| `Glow`           | dword | 1       | 0/1. Soft glow (stacked translucent rings, not a real blur) around the plate in the plate colour, 4 px (scaled) at 40 %. Clipped to what Windows invalidates around an icon (~10 px). |
| `SelectionColor` | sz    | ""      | `RRGGBB` or `#RRGGBB`. Empty (or `accent`) = the system accent colour, read through the immersive colour API (the colour picked in Settings), DWM colorization as fallback, re-read at most every 3 s. Anything else is logged at error level and treated as empty. Used for the fill and the glow. |

All values are applied live from `SettingsChanged` (the desktop is invalidated); no restart.

## 5. Proposed settings.reg lines and strings (ids 1550-1569)

Goes in the desktop group (`;a %R:1503%` heading), e.g. after `hide-desktop-icon-text`. The GUI has no numeric
field, so the three pixel/percent values are offered as `;c` presets (the registry value accepts any number in
range either way; a user can type others with regedit).

```
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\desktop-icon-selection-style]
;b %R:1550%
"Enabled"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\desktop-icon-selection-style]
;c 5 %R:1551%
;x 0 %R:1552%
;x 4 4 px
;x 8 8 px
;x 12 12 px
;x 16 16 px
"CornerRadius"=dword:00000008
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\desktop-icon-selection-style]
;c 5 %R:1553%
;x 20 20 %
;x 40 40 %
;x 60 60 %
;x 80 80 %
;x 100 100 %
"Opacity"=dword:00000028
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\desktop-icon-selection-style]
;c 4 %R:1554%
;x 0 %R:1555%
;x 1 %R:1556%
;x 2 %R:1557%
;x 3 %R:1558%
"BorderStyle"=dword:00000002
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\desktop-icon-selection-style]
;c 5 %R:1559%
;x 0 %R:1560%
;x 2 2 px
;x 4 4 px
;x 8 8 px
;x 12 12 px
"Padding"=dword:00000004
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\desktop-icon-selection-style]
;b %R:1561%
"Glow"=dword:00000001
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\desktop-icon-selection-style]
;w %R:1562%
;%R:1563%
;
"SelectionColor"=""
```

(The `;w` block follows the `TextColor` row of taskbar-clock-customization: label id, hint id, default line.
If the GUI cannot render `%` inside a `;x` label, drop the `%` sign from the opacity presets.)

| Id   | Suggested define                        | English | Turkish |
|------|-----------------------------------------|---------|---------|
| 1550 | `IDS_MOD_ICONSELSTYLE`                  | Restyle the highlight behind selected desktop icons | Seçili masaüstü simgelerinin arkasındaki vurguyu yeniden biçimlendir |
| 1551 | `IDS_MOD_ICONSELSTYLE_RADIUS`           | Corner radius | Köşe yuvarlaklığı |
| 1552 | `IDS_MOD_ICONSELSTYLE_RADIUS_NONE`      | Sharp corners | Keskin köşeler |
| 1553 | `IDS_MOD_ICONSELSTYLE_OPACITY`          | Fill opacity | Dolgu saydamlığı |
| 1554 | `IDS_MOD_ICONSELSTYLE_BORDER`           | Border | Kenarlık |
| 1555 | `IDS_MOD_ICONSELSTYLE_BORDER_NONE`      | No border | Kenarlık yok |
| 1556 | `IDS_MOD_ICONSELSTYLE_BORDER_SOLID`     | Solid | Düz |
| 1557 | `IDS_MOD_ICONSELSTYLE_BORDER_DASHED`    | Dashed | Kesik çizgili |
| 1558 | `IDS_MOD_ICONSELSTYLE_BORDER_DOTTED`    | Dotted | Noktalı |
| 1559 | `IDS_MOD_ICONSELSTYLE_PADDING`          | Gap around the highlight | Vurgunun çevresindeki boşluk |
| 1560 | `IDS_MOD_ICONSELSTYLE_PADDING_NONE`     | None (stock size) | Yok (varsayılan boyut) |
| 1561 | `IDS_MOD_ICONSELSTYLE_GLOW`             | Soft glow around the highlight | Vurgunun çevresinde yumuşak parıltı |
| 1562 | `IDS_MOD_ICONSELSTYLE_COLOR`            | Highlight colour (RRGGBB, empty = accent colour) | Vurgu rengi (RRGGBB, boş = vurgu rengi) |
| 1563 | `IDS_MOD_ICONSELSTYLE_COLOR_HINT`       | e.g. 3399FF; leave empty to follow the Windows accent colour | örn. 3399FF; Windows vurgu rengini izlemek için boş bırakın |

Ids 1564-1569 are unused.

## 6. What was deliberately left out or changed

Kept (core behaviour): the `DrawThemeBackground` interception of `LVP_LISTITEM` selected/hot draws during the
desktop's paint, the GDI+ plate (rounded path, alpha fill, fake glow of stacked rings behind an excluded
path, dashed/dotted pen inset by half its width), the accent colour via the immersive colour API with DWM
fallback and a 3 s poll, DPI scaling from the list view's DPI (refreshed on `WM_DPICHANGED_BEFOREPARENT` /
`AFTERPARENT`, `WM_THEMECHANGED`, `WM_SETTINGCHANGE`, `WM_DWMCOLORIZATIONCOLORCHANGED`), the `CreateWindowExW`
hook to re-attach when the shell rebuilds the desktop, the "exactly one list view subclassed" rule, the
`GetThemeClass` foreign-class guard, hover styled with fill but no border, hovered+selected always ours.

Flattened to the six requested settings (the original had 22):

- `widthPercent` + `topInset` + `bottomInset` -> one `Padding` in pixels on every side. The original narrowed
  only the width (75 % of the cell) and kept the full height. Padding trims the bottom too, by the same few
  pixels; the item rect has slack below the last label line so this is not visible in practice.
- `shape: square` + `squareSize` -> dropped (exotic).
- `fillColor` + `glowColor` -> one `SelectionColor` used by both fill and glow; `borderColor` fixed to white.
- `fillOpacity` -> `Opacity`; `hoverOpacity` uses the same value; `styleHover` fixed on (the original default).
- `borderStyle` -> dword 0..3; `dashDot` dropped. `borderOnSelectionOnly` fixed on, `borderOpacity` 70,
  `borderThickness` 1, `dashLength` 2, `dashGap` 2 (all the original defaults, as constants at the top of the
  file).
- `glow` kept; `glowOpacity` 40, `glowSize` 4, `glowOffsetY` 0 fixed (original defaults).

Other changes:

- An empty plate (padding larger than the item) falls back to the stock highlight instead of hiding the
  selection; the original treated a collapsed plate as "no highlight", which this port's clamp makes
  unreachable anyway.
- `DrawThemeBackgroundEx` is hooked in addition to `DrawThemeBackground` (see section 3).
- Diagnostics (section 3) were added; the original has none.
- `IsDesktopListView` does not require the "FolderView" caption (same reasoning as hide-desktop-icon-text: the
  caption can be set after creation, and class + parent chain is specific enough).
- Repaints from the engine thread are `RedrawWindow(RDW_INVALIDATE | RDW_ERASE)` only, never synchronous.
- GDI+ is started on the first attach (not in Init) under an SRW lock and shut down in `Uninit`, after the
  engine removed the hooks and `BeforeUninit` removed the subclass synchronously.

Interaction with other mods: `hide-desktop-icon-text` also subclasses the desktop list view and hooks
`CreateWindowExW`; the subclass procs are different and hooks stack, so they coexist. The engine's own
desktop input surface subclass is unaffected.

Known limitations (inherited): the plate is clipped to the item rect / the invalidated area, so a glow larger
than ~10 px would be cut off (hence the fixed 4 px). A long file name wraps to the full cell width and can
overhang a padded plate at the sides. Explorer file windows are untouched (desktop only).

## 7. Verifying on a live shell

1. Set `Logging=1`, enable the mod. Log should show "Hooked uxtheme DrawThemeBackground ...", then "Watching
   the desktop list view ...", then on the first repaint "... is painting through the subclass".
2. Click one desktop icon. Expected: the highlight is narrower than the cell (gap around it), rounded corners,
   translucent accent fill, a white dashed border, and a soft accent-coloured halo. The log shows once
   "Selection painting hook hit: DrawThemeBackground LVP_LISTITEM state 3 replaced ..." (state 5 if the desktop
   is not focused). If instead "... no LVP_LISTITEM draw reached ..." appears, see section 3.
3. Ctrl-click a second icon in the neighbouring column: two separate plates with a gutter between them (stock:
   one merged slab). Click empty desktop: both plates disappear cleanly, nothing left behind.
4. Hover an unselected icon: a translucent fill without border. Hover the selected icon: the border stays.
5. In the settings window change Corner radius to "Sharp corners", Border to "Dotted", Padding to "None",
   Opacity to 100 %: each applies on the next repaint (the desktop is invalidated on every change). Turn Glow
   off: the halo goes.
6. Type `FF4080` into the colour field: the fill and the halo turn pink; the border stays white. Type `xyz`:
   the log shows `SelectionColor "xyz" is not RRGGBB ...` and the accent colour is used.
7. Change the Windows accent colour in Settings > Personalization: within ~3 s (on the next repaint, e.g.
   select a different icon) the plate follows.
8. Right-click the desktop > View > untick and re-tick "Show desktop icons": the log shows a new "Watching the
   desktop list view ..." (the `CreateWindowExW` path) and selection still restyled.
9. Disable the mod: the stock highlight comes back at once; log shows "Standing down: N selection plate(s)
   restyled ...". No crash, no leftover plate.
