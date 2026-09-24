# transparent-desktop-icons-spotlight - integration notes

Source: `src/core/mods/desktop_icons_spotlight.cpp`
Original: Windhawk `transparent-desktop-icons-spotlight` v1.1.0 by drgutman

## 1. Declaration

```c
SP_MOD_DECLARE(g_modDesktopIconsSpotlight);
```

`SP_MOD_ID` is `"transparent-desktop-icons-spotlight"`. `minOsBuild` is 22000 (the approach relies on the
Windows 11 desktop composition, see section 5), `targets` is `SP_TARGET_EXPLORER`, `flags` is 0.

## 2. Exception handling

Not needed. No C++/WinRT, no `throw`, no `try`. Win32 + comctl32 + GDI only; the STL use is `std::atomic`,
`std::vector` and `<algorithm>`/`<cmath>`.

Link pragmas inside the file: `Comctl32.lib`, `Gdi32.lib`. Nothing to add to core.vcxproj beyond the
`ClCompile` line (plain, no `<ExceptionHandling>` child).

No hooks at all (no `SP_HookSymbols`, no export hooks): the mod only subclasses the desktop windows and drives
a timer on the desktop's thread. Nothing to download, nothing build-specific to resolve.

## 3. Settings read (HKCU\Software\ShadePatcher\Mods\transparent-desktop-icons-spotlight)

| Name              | Type  | Default | Meaning |
|-------------------|-------|---------|---------|
| `Enabled`         | dword | 0       | Engine toggle (read by the engine, not the mod). |
| `IdleOpacity`     | dword | 35      | Opacity of the desktop icons while idle, percent of full. 0 = icons invisible when idle (they still take clicks and reappear on hover), 100 = never dimmed. Clamped to 0..100. |
| `FadeMs`          | dword | 300     | Length of the fade, in ms, in both directions. 0 = instant. Clamped to 0..10000. |
| `SpotlightRadius` | dword | 0       | 0 = the whole desktop is revealed while the mouse is over it or an icon is selected. Any other value = spotlight mode: only a soft circle of this radius (pixels at 96 dpi, scaled with the monitor DPI) around the cursor and the selected icons are revealed; the rest stays at `IdleOpacity`. Clamped to 20..4000 when non-zero. |

All three are applied live from `SettingsChanged` (the view is told to re-apply on its own thread; switching
between whole-desktop and spotlight mode creates or destroys the overlay on the next frame). No restart needed.

The GUI only knows on/off (`;b`) and choice (`;c`/`;x`) controls, so the three numbers are offered as choice
lists below; the mod itself accepts any value in range if the user writes one to the registry.

## 4. Proposed settings.reg lines and strings (ids 1570-1579)

Place it under the existing Desktop heading (`;a %R:1503%` group, next to `hide-desktop-icon-text` /
`desktop-icons-view`).

```
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\transparent-desktop-icons-spotlight]
;b %R:1570%
"Enabled"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\transparent-desktop-icons-spotlight]
;c 7 %R:1571%
;x 0 %R:1574%
;x 10 10 %
;x 20 20 %
;x 35 35 %
;x 50 50 %
;x 65 65 %
;x 80 80 %
"IdleOpacity"=dword:00000023
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\transparent-desktop-icons-spotlight]
;c 7 %R:1572%
;x 0 %R:1576%
;x 150 150 ms
;x 300 300 ms
;x 500 500 ms
;x 800 800 ms
;x 1200 1200 ms
;x 2000 2000 ms
"FadeMs"=dword:0000012c
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\transparent-desktop-icons-spotlight]
;c 6 %R:1573%
;x 0 %R:1575%
;x 120 120 px
;x 180 180 px
;x 250 250 px
;x 350 350 px
;x 500 500 px
"SpotlightRadius"=dword:00000000
```

(The literal `%` in "35 %" is safe: `GUI_SubstituteLocalizedString` only reacts to `%R:`.)

| Id   | Suggested define                     | English | Turkish (ASCII) |
|------|--------------------------------------|---------|-----------------|
| 1570 | `IDS_MOD_ICONSPOTLIGHT`              | Dim the desktop icons when idle, with a spotlight | Bostayken masaustu simgelerini soluklastir (spot isigi ile) |
| 1571 | `IDS_MOD_ICONSPOTLIGHT_OPACITY`      | Icon opacity when idle | Bostayken simge gorunurlugu |
| 1572 | `IDS_MOD_ICONSPOTLIGHT_FADE`         | Fade duration | Gecis suresi |
| 1573 | `IDS_MOD_ICONSPOTLIGHT_RADIUS`       | Spotlight radius | Spot isigi yaricapi |
| 1574 | `IDS_MOD_ICONSPOTLIGHT_HIDDEN`       | Hidden | Gizli |
| 1575 | `IDS_MOD_ICONSPOTLIGHT_WHOLE`        | Whole desktop | Tum masaustu |
| 1576 | `IDS_MOD_ICONSPOTLIGHT_INSTANT`      | Instant | Aninda |

Turkish with proper characters for the .rc file (the ASCII rows above are only so this file stays ASCII):

- 1570: `Boştayken masaüstü simgelerini soluklaştır (spot ışığı ile)`
- 1571: `Boştayken simge görünürlüğü`
- 1572: `Geçiş süresi`
- 1573: `Spot ışığı yarıçapı`
- 1574: `Gizli`
- 1575: `Tüm masaüstü`
- 1576: `Anında`

Ids 1577-1579 are unused.

## 5. What the port does, what was changed, and what was left out

### How it works (this is not how the original works)

The original creates a full-screen Direct3D 11 / Direct2D / DirectComposition overlay that re-draws the
wallpaper on top of the icons at the idle opacity and cuts blurred holes for the cursor spotlight and the
selected icons. That needs the wallpaper image file, the fit mode (fill/fit/stretch/tile/span with a hand-coded
"/3 crop quirk"), the solid colour, slideshow cache files, a registry watcher for wallpaper changes, a render
thread and ~1900 lines.

Probing the live shell on this machine (build 26200) showed a much shorter lever:

- `Progman` has `WS_EX_NOREDIRECTIONBITMAP`: the wallpaper is composed by DWM, not painted with GDI.
- `SHELLDLL_DefView` is a `WS_EX_LAYERED` child of Progman on which the shell itself has called
  `SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA)` (`GetLayeredWindowAttributes` returns flags=0x2,
  alpha=255). DWM multiplies that constant alpha into the per-pixel alpha of the view's own surface, and that
  surface is fully transparent between the icons.
- Setting alpha 100 on the view from outside dimmed the icons, their labels and the selection highlight, and
  changed nothing else on screen (before/after screen diff: only the icon column changed, wallpaper pixels
  identical).

So the port dims the desktop by calling `SetLayeredWindowAttributes` on the view with an animated alpha, and
restores the values the shell had (`GetLayeredWindowAttributes` at attach) on unload. No wallpaper reading, no
DirectX, no render thread.

Kept (core behaviour):

- Idle dimming with a fade; reveal while the mouse is over the desktop (WM_MOUSEMOVE / TrackMouseEvent /
  WM_MOUSELEAVE on both the view and the list, with a `WindowFromPoint` check on leave so moving onto the
  rename box does not count as leaving).
- Reveal while icons are selected or a label is being edited (LVN_ITEMCHANGED / LVN_ODSTATECHANGED with the
  LVIS_SELECTED bit, LVN_BEGIN/ENDLABELEDIT via the view's WM_NOTIFY; the desktop list is owner-data so both
  notifications matter).
- Spotlight mode (`SpotlightRadius` > 0): the view stays at the idle alpha and a second `WS_EX_LAYERED |
  WS_EX_TRANSPARENT` child of Progman, placed above the view, shows a `PrintWindow(PW_CLIENTONLY)` of the icon
  list multiplied by a soft mask (feathered circle around the cursor + padded, feathered rectangles of the
  selected items) via `UpdateLayeredWindowIndirect` with a dirty rectangle. The print carries the real
  per-pixel alpha (verified: background A=0, icons A=255, text shadow A=142), so the overlay is pixel-identical
  to the icons underneath. The overlay is hidden whenever nothing is revealed. If it cannot be created or
  updated, the mod logs it and falls back to whole-desktop dimming until the next settings change.
- Re-attaching when the shell rebuilds the desktop (theme change, "Show desktop icons"): a watcher thread polls
  once a second, like the engine's own surface watcher, and also posts a heartbeat that re-applies the alpha
  if the shell reset it.

Changed:

- Settings reduced from 18 to 3 as requested. `IdleOpacity` is "how visible" in percent (the original's
  `idleOpacity` was "how much covered", 0..255). Selection reveal has a fixed hold of 2.5 s after the
  selection stops changing (original `selectionTimeout`, default 2 s); label editing keeps the reveal.
- The original's idle timeout (spotlight fades after 2 s without mouse movement even while over the desktop)
  was dropped: over the desktop = visible. Simpler and what users expect from "hover".
- The original's double-click-on-empty-desktop toggle of the overlay was dropped: that gesture belongs to the
  engine's shared desktop surface (`desktop-toggle-icons` already owns it) and this mod must not take it.
- Mouse smoothing, spotlight roundness/blur amounts, per-feature fade in/out durations, selection padding and
  roundness: replaced by fixed values (feather = max(8 px, 30% of the radius), selection padding 8 px, one
  `FadeMs` for everything).
- The mod uses a private subclass id `0x53506453` ('SPdS') on the same two windows the engine's desktop
  surface subclasses with `0x53506431`; both are installed with `SP_SetWindowSubclassFromAnyThread` and coexist.

Left out:

- Wallpaper rendering, fit modes, slideshow tracking, solid-colour reading, the registry watcher and the
  DirectX device-lost recovery: not needed with this approach.
- Overlay toggle by double click (see above).

Known limitations:

- The shell must keep the view a layered window with `LWA_ALPHA` (true on every Windows 11 build seen, and on
  this one). If a build ever does not, the mod logs "is not a layered window on this build" once and does
  nothing rather than add the style itself, which could black out the desktop.
- Spotlight mode keeps a 32-bit DIB the size of the desktop view (about 33 MB on a 4K monitor) plus 1 byte per
  pixel of mask while the mode is on; freed when switched to whole-desktop mode or on unload. Each frame only
  processes the changed rectangle.
- Multi-monitor: the icon view spans the primary monitor only (that is how the shell lays it out); other
  monitors have no icons and are unaffected.
- With `IdleOpacity` = 0 the icons are invisible when idle but still clickable (LWA_ALPHA does not affect hit
  testing); moving the mouse over the desktop brings them back.

## 6. Verifying on a live shell

1. Enable the mod (`Enabled=1`) with the defaults (35 %, 300 ms, whole desktop). Within about a second, with
   the mouse away from the desktop and nothing selected, the icons and labels fade to roughly a third of their
   opacity; the wallpaper between them does not change at all (compare a wallpaper detail next to an icon).
2. Move the mouse over the desktop: everything fades to full in ~300 ms. Move it onto a window: fades back.
3. With the mouse away, click an icon (or press F2 on one): the icons come back while the selection is fresh
   and while renaming; about 2.5 s after the last selection change (and after the rename ends) they dim again.
4. Set `FadeMs` to 0: the change is instant. Set `IdleOpacity` to 0: icons vanish when idle, reappear on hover.
5. Set `SpotlightRadius` to 250: with the mouse over the desktop only a soft circle around the cursor is at
   full opacity and follows the cursor; the rest of the icons stay dim. Select an icon and move the mouse away:
   that icon (with a small halo) stays revealed for ~2.5 s, then fades. Set it back to 0: the overlay goes
   away and the whole desktop reveals again.
6. Right-click the desktop > View > untick and re-tick "Show desktop icons", or change the theme: the desktop
   is rebuilt; within a second the new view is dimmed again (log: "The desktop view went away", then
   "Watching the desktop").
7. Disable the mod: the icons return to full opacity at once ("The desktop icons are back at their original
   opacity" in the log), the overlay is gone, no crash. `GetLayeredWindowAttributes` on the view reads
   alpha 255 / flags 0x2 again.
8. With `Logging=2` the log shows "Watching the desktop (view ..., list ...): icons at 35% when idle, fade
   300 ms, whole desktop" and, in spotlight mode, "Spotlight overlay ... created over the desktop".
