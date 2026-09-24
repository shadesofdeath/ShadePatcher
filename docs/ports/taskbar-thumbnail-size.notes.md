# Port notes: taskbar-thumbnail-size

Source: Windhawk mod `taskbar-thumbnail-size` v1.2.1 by m417z ("Taskbar Thumbnail Size").
Written: `src/core/mods/taskbar_thumbnail_size.cpp`. Nothing else was touched.

## 1. Declaration

```c
SP_MOD_DECLARE(g_modTaskbarThumbnailSize);
```

Suggested place in `mod_table.c`: the "Taskbar" group, next to `g_modTaskbarIconSize`.
`minOsBuild` is **26100**, not 22000: the XAML previews and `ThumbnailHelpers` only exist from 24H2 on
(the original's readme says the same and points older builds at the registry). On an older build the two
symbols would simply be missing and the mod would log that and do nothing, so 22000 would also be safe, but
26100 keeps the mod out of the settings window where it cannot work.

## 2. Exceptions

**Yes**, the file uses C++/WinRT (`Windows.UI.Xaml`) and needs in `core.vcxproj`:

```xml
<ClCompile Include="mods\taskbar_thumbnail_size.cpp">
  <ExceptionHandling>Sync</ExceptionHandling>
</ClCompile>
```

Same reason as `taskbar_menu_entry.cpp`. The file also has one `__try/__except` in a helper with no C++
objects (`QueryElementAbi`), like `taskbar_labels.cpp`, so it does not hit C2712. No new link dependencies
(`runtimeobject.lib` is already linked). `<unknwn.h>` is included before the first winrt header and
`GetCurrentTime` is undefined before it, per the brief's lessons.

## 3. Settings (HKCU\Software\ShadePatcher\Mods\taskbar-thumbnail-size)

| Name              | Type  | Default | Meaning                                                                   |
|-------------------|-------|---------|---------------------------------------------------------------------------|
| `Enabled`         | dword | 0       | engine toggle                                                             |
| `ThumbnailWidth`  | dword | 0       | widest a preview may be, logical pixels (0..4000); **0 = Windows default** |
| `ThumbnailHeight` | dword | 0       | tallest a preview may be, logical pixels (0..4000); **0 = Windows default** |

Semantics: the preview keeps its window's shape and is scaled to fit inside Width x Height (the smaller of
the two ratios wins). With only one side set, the preview grows or shrinks until that side is reached. With
both at 0 the mod is a no-op (enabling it with defaults changes nothing visible). Negative values read as 0;
values above 4000 are clamped. Both are re-read on `SettingsChanged`; the next hover shows the new size.

The values are logical (DPI-independent) pixels: the size function's `scale` argument is the monitor's DPI
factor and the hook only multiplies it, so 300 means 300 DIPs, i.e. 450 physical pixels at 150 %.

Choice lists offered in the settings window (the GUI has no numeric edit control; same approach as
`taskbar-icon-size`). The item labels are literal `NN px` except the shared "Windows default" entry:

- ThumbnailWidth: **0 (Windows default)**, 160, 200, 240, 280, 320, 400, 480, 560, 640
- ThumbnailHeight: **0 (Windows default)**, 100, 120, 150, 180, 210, 240, 300, 360, 420

If the integrator prefers fewer entries, drop from the top; the code accepts any value. The stock box on
this build is printed at debug level on the first hover (`stock WxH` in the log line), which is the best
guide to which entries are worth offering.

## 4. settings.reg lines and strings (ids 1640-1649)

```
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-thumbnail-size]
;b %R:1640%
"Enabled"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-thumbnail-size]
;c 10 %R:1641%
;x 0 %R:1643%
;x 160 160 px
;x 200 200 px
;x 240 240 px
;x 280 280 px
;x 320 320 px
;x 400 400 px
;x 480 480 px
;x 560 560 px
;x 640 640 px
"ThumbnailWidth"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-thumbnail-size]
;c 10 %R:1642%
;x 0 %R:1643%
;x 100 100 px
;x 120 120 px
;x 150 150 px
;x 180 180 px
;x 210 210 px
;x 240 240 px
;x 300 300 px
;x 360 360 px
;x 420 420 px
"ThumbnailHeight"=dword:00000000
```

Put the block under the Taskbar heading, after the `taskbar-icon-size` block. No `*` restart marker: every
change is applied on the next hover.

strings.h:

```c
#define IDS_MOD_THUMBSIZE               1640
#define IDS_MOD_THUMBSIZE_WIDTH         1641
#define IDS_MOD_THUMBSIZE_HEIGHT        1642
#define IDS_MOD_THUMBSIZE_WINDEFAULT    1643
```

gui.en-US.rc:

```
    IDS_MOD_THUMBSIZE               "Taskbar thumbnail size"
    IDS_MOD_THUMBSIZE_WIDTH         "Maximum thumbnail width"
    IDS_MOD_THUMBSIZE_HEIGHT        "Maximum thumbnail height"
    IDS_MOD_THUMBSIZE_WINDEFAULT    "Windows default"
```

gui.tr-TR.rc:

```
    IDS_MOD_THUMBSIZE               "Görev çubuğu küçük resim boyutu"
    IDS_MOD_THUMBSIZE_WIDTH         "En fazla küçük resim genişliği"
    IDS_MOD_THUMBSIZE_HEIGHT        "En fazla küçük resim yüksekliği"
    IDS_MOD_THUMBSIZE_WINDEFAULT    "Windows varsayılanı"
```

1644-1649 are unused.

## 5. What was left out, and why

- **Percentage mode** (`size`, the original's default: multiply the DPI scale by N/100). The requested scope
  is an absolute width/height; a percentage is a third setting nobody asked for. Note that the port's
  mechanism is the same one the original uses for both of its modes (call the original with a multiplied
  scale), so the rounding/minimums the shell applies are kept.
- **Separate min/max constraints** (`minWidth`, `minHeight`, `maxWidth`, `maxHeight`) and the
  `useAbsoluteSize` switch: collapsed into one box. The port behaves like the original's absolute mode with
  min = max = the box and `preserveAspectRatio` on, in which the original's "constraints conflict, prioritise
  max" branch is what runs: the result is fit-inside. That is the one sensible outcome for window previews.
- **`preserveAspectRatio = false`** (stretch the preview to exactly W x H): deliberately not offered;
  stretched window previews are unreadable and the original marks it as the non-default.
- **ExplorerExtensions.dll** (the ExplorerPatcher host) as an alternative home for the symbols: not on this
  machine; only Taskbar.View.dll is waited for.
- **LoadLibraryExW hook** for the late module: replaced by `SP_WaitForModule(L"Taskbar.View.dll", 60000, ...)`.
  On a cold start the first previews can be built before the hooks land (the engine polls every 500 ms);
  those keep the stock size until the flyout is rebuilt, which happens on the next hover.
- **Restoring `MaxWidth` on unload**: the original does not either. The cap is only lifted; with the hooks
  gone the size function answers the stock box, and a control is never wider than that, so a lifted cap has
  no visible effect after disabling. Tracking every templated view to put a number back would be more code
  than the effect is worth.
- Both symbols are **optional** (the original refuses to load on a miss). A missing `GetScaledThumbnailSize`
  is logged as an error ("the thumbnail size cannot be changed"), a missing `OnApplyTemplate` as an error
  ("previews wider than the stock width will be clipped"); the "hooks are in" line says `size yes/no,
  template yes/no` so a build difference is visible at a glance.

Kept: the full undecorated symbol strings from the original (one spelling each, that is all the original
has); the "second pointer slot of the implementation object is the XAML object" idiom, here checked with
`QueryInterface` for `IFrameworkElement` inside a `__try` guard so a different object layout on some build
costs the cap-lifting, not the shell.

## 6. Threading and unload

- Settings are `std::atomic<int>`; hook bodies read them relaxed.
- The size hook calls the original at most twice per call and touches no XAML.
- The template hook runs on the taskbar's UI thread inside the shell's own `OnApplyTemplate`; the XAML call
  (`MaxWidth`) happens right there, so no dispatcher is needed. Everything is inside try/catch.
- `BeforeUninit` sets `g_unloading`, after which both hooks pass through; the engine then removes the hooks.
  No `Uninit`, no `AfterInit`.
- First run downloads the PDB for Taskbar.View.dll (already cached if any other taskbar mod ran).

## 7. How to verify on a live shell

1. Enable the mod with defaults (0 / 0): hover a taskbar button; the preview looks exactly as before. With
   `Logging=2` under `HKCU\Software\ShadePatcher` the log shows `Taskbar thumbnail hooks are in: size yes,
   template yes`, and on every hover a `Thumbnail view width cap of N lifted` line (once per preview control)
   but no `Thumbnail ... now ...` line (nothing to scale).
2. Set `ThumbnailWidth=400`, `ThumbnailHeight=0`: move the pointer off the taskbar and hover the button
   again. The preview is now 400 DIPs wide (at 100 % scaling, 400 px on screen; measure with a screenshot),
   proportionally taller, and not clipped on the right. The log shows `stock WxH, now 400xH2 (xF)`.
3. Set `ThumbnailWidth=0`, `ThumbnailHeight=300`: the preview is 300 DIPs tall and proportionally wider.
4. Set both, e.g. 480 x 120: the preview fits inside that box, i.e. for a normal landscape window it is
   120 tall and narrower than 480 (height wins), with its shape unchanged. A tall/narrow window (a phone-like
   app, or a window resized to a portrait shape) is bounded by the width or by the height, whichever is hit
   first.
5. Set `ThumbnailWidth=160`: previews shrink below the stock size (proves shrinking works too).
6. Hover a button with several windows (a browser with three windows): every preview in the row takes the
   new size and the flyout grows with them; nothing overlaps and the shell does not restart.
7. Change the value while a preview flyout is open: the open one keeps its size; the next hover shows the
   new one (the "applied live" contract: no restart, next flyout).
8. Disable the mod: the next hover shows stock-sized previews again, without an Explorer restart.
9. Multi-monitor at different scaling: the same setting gives the same physical proportion on each screen
   (the value is in DIPs).
