# taskbar-styler - integration notes

Sources written (nothing else was touched):

- `src/core/mods/taskbar_styler.cpp`         the engine: diagnostics connection, element bookkeeping, matching,
                                             applying/restoring, style variables, the WindhawkBlur brush, lifecycle
- `src/core/mods/taskbar_styler_rules.h`     the rule language as pure string code (targets, styles, constants,
                                             WindhawkBlur parameters, `{{...}}` expression evaluator, the CustomRules
                                             setting format)
- `src/core/mods/taskbar_styler_themes.h`    the seven themes, copied word for word, with author credits

Based on the Windhawk mod `windows-11-taskbar-styler` v1.10 by m417z. The theme data is the guide's; the code is new.

## 1. SP_MOD_DECLARE symbol

`g_modTaskbarStyler`

Suggested placement in `mod_table.c`: the "Taskbar" group. Order matters in one respect only: it should come
*after* the other taskbar mods that change layout (icon size, labels, clock) so that on a live "enable" the
styler's sweep sees their final tree; on a cold start order is irrelevant (every element is reported when it is
created).

## 2. ExceptionHandling

**Yes.** C++/WinRT throughout (Windows.UI.Xaml, Windows.UI.Composition, Windows.Storage.Streams,
Windows.System). Add to `core.vcxproj`:

```
    <ClCompile Include="mods\taskbar_styler.cpp">
      <ExceptionHandling>Sync</ExceptionHandling>
    </ClCompile>
    <ClInclude Include="mods\taskbar_styler_rules.h" />
    <ClInclude Include="mods\taskbar_styler_themes.h" />
```

Libraries: the file pulls `comctl32.lib` (DefSubclassProc), `dxguid.lib` (the D2D effect CLSIDs used by the blur
brush) and `ole32.lib` (CreateStreamOnHGlobal) with `#pragma comment(lib, ...)`; `shcore.dll`'s
`CreateRandomAccessStreamOverStream` is reached through GetProcAddress, so no import library. No WinUI 2 or
WinUI 3 projection is used (see section 5 on ItemsRepeater).

**Exports.** The file adds two `extern "C" __declspec(dllexport)` functions to ShadePatcher.dll:
`DllGetClassObject` and `DllCanUnloadNow`. They are how `InitializeXamlDiagnosticsEx` obtains the TAP object
from this DLL (it loads the DLL by the path the mod passes, which is the running module's own path, so it also
works for the `C:\Windows\dxgi.dll` copy). Make sure nothing else in core defines them and that the dxgi proxy's
export list does not forward them. `DllCanUnloadNow` always answers S_FALSE: the engine DLL never unloads.

## 3. Settings read

All under `HKCU\Software\ShadePatcher\Mods\taskbar-styler`, re-read on `SettingsChanged` and applied live
(every XAML thread is restored and re-styled, then the diagnostics are reconnected):

| Name | Type | Default | Meaning |
|------|------|---------|---------|
| `Theme` | dword | 1 | 0 = no theme (custom rules only), 1 TranslucentTaskbar, 2 DockLike, 3 SimplyTransparent, 4 Squircle, 5 Matter, 6 Surface, 7 Luminosity (Dock), 8 Luminosity (Classic), 9 Luminosity (Compact). Anything else = 0. |
| `CustomRules` | sz | "" | Extra rules, applied after the theme (so they override it). Format below. Read into a 32K-character buffer. |

### CustomRules format (document this in the help text)

One string holds every rule. Rules are separated by `|`. Inside a rule the **target comes first** and the styles
follow, separated by `;`:

```
Taskbar.TaskListButton;CornerRadius=0|Rectangle#BackgroundStroke;Visibility=Collapsed|Taskbar.TaskListButtonPanel@CommonStates > Border#BackgroundElement;Background@ActiveNormal:=<SolidColorBrush Color="Red"/>;CornerRadius=4
```

Target and style syntax are exactly the original's (`Type#Name[2][Prop=Value]@Group > ...`, alternatives with
`,`, `Prop=Value`, `Prop:=<Xaml/>`, `Prop:=` to clear, `Prop@State=Value`, `Prop=>Var`, `{{expr}}`, `$constant`
from the active theme's constants). A rule or style beginning with `//` is ignored. Bad rules are logged
(`SP_LogError`) and skipped; nothing crashes.

The English string 1662 currently reads "(target=... | property=value; ...)", which suggests a different shape.
Suggested replacement (see section 4).

## 4. settings.reg lines and strings

The blocks and strings already exist in the repo (settings.reg lines ~277-297, strings.h 1650-1663, both
language files). They match what the mod reads, with two suggestions:

- `"Theme"=dword:00000001` as the default is fine (TranslucentTaskbar is the guide's first theme).
- Reword 1662 so the format is unambiguous.

For reference, the intended block:

```
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-styler]
;b %R:1650%
"Enabled"=dword:00000000
;e %R:1663%
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-styler]
;c 10 %R:1651%
;x 0 %R:1652%
;x 1 %R:1653%
;x 2 %R:1654%
;x 3 %R:1655%
;x 4 %R:1656%
;x 5 %R:1657%
;x 6 %R:1658%
;x 7 %R:1659%
;x 8 %R:1660%
;x 9 %R:1661%
"Theme"=dword:00000001
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-styler]
;w %R:1662%
;%R:1662%
;
"CustomRules"=""
```

Strings (ids 1650-1663; 1664-1669 free):

```
IDS_MOD_STYLER              1650  "Style the taskbar with a theme (Windows 11 Taskbar Styler)"
IDS_MOD_STYLER_THEME        1651  "Theme"
IDS_MOD_STYLER_THEME_NONE   1652  "None (custom rules only)"
IDS_MOD_STYLER_THEME_1..9   1653-1661  TranslucentTaskbar, DockLike, SimplyTransparent, Squircle, Matter, Surface,
                                       Luminosity (Dock), Luminosity (Classic), Luminosity (Compact)
IDS_MOD_STYLER_CUSTOM       1662  en: "Extra rules: target;style;style|target;style - may be left empty"
                                  tr: "Ek kurallar: hedef;stil;stil|hedef;stil - bos birakilabilir"
IDS_MOD_STYLER_HELP         1663  en: "Themes come from the windows-11-taskbar-styling-guide repository and credit
                                       their authors. Some are designed together with the labels, clock and icon
                                       size mods."
                                  tr: (as already in gui.tr-TR.rc)
```

Turkish for 1662 with proper characters: "Ek kurallar: hedef;stil;stil|hedef;stil - boş bırakılabilir".

## 5. What was left out, and what was done differently

Left out (not used by the seven themes, or not wanted here):

- **Resource variables** (`themeResourceVariables`, `Key@Dark=...`, the merged theme dictionary and the
  UISettings colour-change handler). None of the seven themes has any. `{ThemeResource X}` *inside* a style's
  XAML is fully supported (XamlReader resolves it) and the blur brush's `TintColor="{ThemeResource X}"` is too.
- **Click-through taskbar** and the per-monitor region code.
- **Remote image tracking/caching/retry** (ImageBrush with an https source). None of the seven themes uses an
  image; a style with one still works, it just is not retried when the network comes up.
- **XAML diagnostics consumer handling** (the `InitializeXamlDiagnosticsEx` hook with the Alert/Block/Allow
  prompt). Only one consumer can exist per process; if TranslucentTB or ExplorerBlurMica is also running, the
  later one wins and the other stops seeing elements. Logged, not prompted.
- **The stats timer, the restart-explorer prompt** (a failed AdviseVisualTreeChange is logged instead), and the
  `Squircle_variant_WeatherOnTheRight` selection (the original checks an OS feature flag; only plain Squircle is
  shipped, as extracted).
- Windhawk's array settings (`controlStyles[n]`, `styleConstants[n]`): flattened to the single `CustomRules`
  string. User-defined style constants are not offered; the active theme's constants are applied to custom rules.

Done differently:

- **Recycled taskbar buttons.** The original subscribes to WinUI 2's `ItemsRepeater.ElementPrepared/Clearing`
  to re-match a button that is reused for another window. This project has no WinUI 2 projection (vendor/winui3
  is WinUI 3, whose interface IDs differ), so instead every element on which a `[Property=Value]` condition is
  tested gets a property-changed watch; when the outcome of a condition flips (e.g. `AutomationProperties.Name`
  becomes or stops being "Copilot"), that element's subtree is torn down and matched again from the dispatcher.
  This also covers the case where the name is bound after the element was reported. Title changes that do not
  flip a condition do nothing.
- **Style variable ranking** (which capture a `{{Var}}` reads when several elements capture the same name) is
  computed by walking the two ancestor chains on demand, not with the original's interned tree-spine pool. With a
  single capture, which is what DockLike/Surface/Luminosity Dock have (`Tag=>taskbarDock`), nothing is walked.
- The noise texture for `NoiseOpacity` is written to a COM stream wrapped by shcore instead of an
  `InMemoryRandomAccessStream` + `StoreAsync().get()` (a blocking wait on the UI thread).
- Hooks use the engine: `CreateWindowExW`, `CreateWindowInBand`, `CreateWindowInBandEx` (user32 exports; the
  in-band ones are optional and the load line says whether they were found) and `RegOpenKeyExW` /
  `RegQueryValueExW` (kernelbase). The registry hooks only act while the advise thread is inside
  `AdviseVisualTreeChange` (a thread_local flag), where Windows.UI.Xaml.dll reads
  `HKLM\Software\Microsoft\XAML\Debug\DisableCompositionDiag` once; answering "1" keeps the composition
  diagnostics (not thread-safe, heap corruption seen upstream) from starting. `tray-show-all-icons` hooks
  `RegGetValueW`, a different export, so the two do not interact.
- Waiting for the taskbar: `SP_WaitForModule(L"Taskbar.View.dll")` triggers a sweep of the shell's XAML host
  windows; the CreateWindow hooks initialize threads whose host window is created later (cold sign-in, Task
  View, input switcher). The diagnostics are connected once at least one thread has its rules loaded, so the
  burst of existing elements lands on prepared threads. Rules are per thread (thread_local), as in the original.

Kept as in the original: the per-element id scheme (handles are addresses and get reused), the queued
`IXamlDiagnosticsTestHooks::UnregisterInstance` releases so the diagnostics do not pin every element forever,
the `FreeLibrary` in `SetSite` that balances the reference `InitializeXamlDiagnosticsEx` took on this DLL, the
deferred first write to `Rectangle#BackgroundFill.Fill`, the FontWeight boxing and Grid definition cloning
workarounds, the visual-state-group handling (apply on `CurrentStateChanged`, default value re-pushed only when a
state-specific one is left), re-pushing a style when something else writes the property, and the full
`{{...}}` grammar (numbers, backtick strings, variables, arithmetic, comparisons, `?:`, `min`, `max`, `skip()`)
with lazy evaluation of the untaken conditional branch.

Clean unload: `BeforeUninit` disconnects the watcher (a stopped flag also makes late callbacks no-ops), then runs
the per-thread teardown on every initialized XAML thread through a WH_CALLWNDPROC hook + SendMessage, which
unregisters every callback, puts every touched property back to the value read before it was styled (or clears
it), removes the taskbar surface subclass and the blur brushes' proxy resources. A thread that has no window left
cannot be reached and is logged as such.

## 6. How to verify on a live shell

Turn logging on (`Logging`=1) and look for:

```
taskbar-styler: Loaded: theme TranslucentTaskbar; hooked CreateWindowExW, RegOpenKeyExW, RegQueryValueExW; CreateWindowInBand yes, CreateWindowInBandEx yes
taskbar-styler: Thread 1234: theme TranslucentTaskbar, 9 rule(s) (0 custom)
taskbar-styler: XAML diagnostics connected
taskbar-styler: Watching the XAML visual trees
```

"Rules for '...' could not be read by XAML" lines name a rule the current build's XAML rejects (the rest of the
theme still applies). At level 2 each styled element is logged.

Then per theme, comparing with
`https://raw.githubusercontent.com/ramensoftware/windows-11-taskbar-styling-guide/main/Themes/<Theme>/screenshot.png`
(Luminosity variants share `Themes/Luminosity/`):

| Theme | What to look for | Companion mods per the guide |
|-------|------------------|------------------------------|
| TranslucentTaskbar | Taskbar background and stroke gone, replaced by a blurred translucent dark tint (WindhawkBlur 18, #25323232); the same blur on the hover flyout background, the tray overflow flyout, context menus (rounded 14) and the volume/brightness confirmator; input switcher blurred. | none |
| DockLike | Taskbar becomes a centred dock: `TaskbarFrame` width Auto with 250 px margins, acrylic rounded (8,8,0,0) root grid with a border; tray area acrylic rounded 10 with negative margins; tray icons without padding. Dock left/right keeps full width (the `taskbarDock` capture). | `taskbar-icon-size`: raise the taskbar height by 4 (`TaskbarHeight` 52 instead of 48). |
| SimplyTransparent | Background fill and stroke fully transparent; nothing else changes. Needs a non-black wallpaper to see. | none |
| Squircle | Transparent bar; every button gets a black acrylic rounded (5) square in every state; tray area acrylic rounded (5) with 5 px vertical margins; running indicator hidden (transparent, 38x40); white labels/clock text; `RequestedTheme=2` (dark) on every Grid; the Copilot button red. | none |
| Matter | Transparent bar; buttons and tray on a `SystemAltLowColor` acrylic with rounded 8 corners; running indicator as a 12x4 pill in `SystemBaseHighColor`, accent-coloured and 21 wide when active; multi-window element accent; flyouts rounded without shadow; volume slider track 8 px. | `taskbar-clock-customization`: show seconds, time format `hh':'mm':'ss tt`, font Tektur (this port's clock mod: `TimeFormat`; there is no seconds/font-family setting, so the seconds come from the format and the Tektur font only through the theme's `FontFamily=Tektur` rule on `volumeLevelText`). `taskbar-icon-size`: `TaskbarButtonWidth` 45, `IconSize` 23, `TaskbarHeight` 48. |
| Surface | Bar becomes a floating rounded (20) pill with a 1 px #40FFFFFF border and blur 5, lifted 10 px from the bottom; tray on the right as its own rounded (20,0,0,20) blurred panel with a #66FFFFFF border; task buttons acrylic rounded 12 with a gradient stroke; running indicator raised 8 px; multi-window element hidden. | `taskbar-icon-size`: `TaskbarHeight` 70, `IconSize` 24, `TaskbarButtonWidth` 47 (the guide also lists IconSizeSmall 16 / TaskbarButtonWidthSmall 32, which this port's icon-size mod does not have). |
| Luminosity (Dock) | Centred dock 250 px from each edge, height 58, 5 px top/bottom gap, blur 30 with luminosity 1.0 and 10 % noise, 1 px #20FFFFFF border, radius 15; tray frame centred with the same margins; every button/tray icon rounded 10; flyouts, tooltips, Alt-Tab, Task View, snap layouts and the input switcher restyled with the same blur (`mbg`), radius 20/15 and no shadow; entrance animations on menu items. | none stated for Dock. |
| Luminosity (Classic) | Same as Dock but the bar stays full width at stock height (no `TaskbarFrame` rules), so only surfaces, radii and the flyouts change. | none stated for Classic. |
| Luminosity (Compact) | Classic plus a 30 px taskbar and tray, 16 px icons, 14 px tray glyphs, label text shifted up 1 px, search button pulled in, widget button shifted 57 px. | `taskbar-labels`: labels without combining, centred running indicator, font size 12, max width 176 (this port: `Mode` 1 with Windows set to "Never combine", `MaximumTaskbarItemWidth` 176; no running-indicator style or font-size setting exists here). `taskbar-clock-customization`: show seconds, width 180, height 60 (this port: put seconds in `TimeFormat`; there is no width/height setting). |

Generic checks for every theme:

1. Hover, press and click a taskbar button: `@CommonStates`/`@RunningIndicatorStates` values follow the state
   and come back when it returns to Normal.
2. Close a window whose button was styled, open another app: the reused button restyles for the new item
   (Squircle/Matter: a button that stops being Copilot loses the red acrylic, one that becomes Copilot gains it).
3. Turn transparency effects off in Settings > Personalization > Colors: `WindhawkBlur`/`AcrylicBrush` rules
   with a `FallbackColor` switch to the flat colour; back on, the blur returns.
4. Change `Theme` in the settings window: the old theme disappears completely (values restored) and the new one
   appears without a shell restart; `Thread N: M element(s) restored` is logged per thread.
5. Turn the mod off: everything returns to stock. Open Task View and Alt-Tab afterwards to confirm those trees
   were restored too.
6. Type a bad custom rule (`Foo#;Bar=`): one `Rule ... skipped` log line, the shell unaffected.

## 7. Things the integrator should know

- `taskbar_styler_themes.h` is generated from the extracted theme data; each theme is a function-local static so
  no `std::vector` is constructed during DLL load. Credits in the header come from the theme pages in the guide.
- The rule strings are the guide's. Some target the volume/brightness confirmator, Alt-Tab and Task View trees
  (`WindowsInternal.ComposableShell...`), which live on other XAML threads in explorer; those threads are
  initialized when their host window (`XamlExplorerHostIslandWindow`, `Shell_InputSwitchTopLevelWindow`) is
  created, so the styles appear the first time those surfaces open after the mod started.
- One element of the taskbar (`Taskbar.TaskListButtonPanel` under `SearchBoxLaunchListButton`, and
  `SearchUx.SearchUI.SearchButtonRootGrid` under `SearchPillButton`) crashes when its visual state groups are
  read; the port skips the group lookup for those two, as the original does.
- Per-thread state is `thread_local`; the engine thread never touches XAML. `SettingsChanged`/`BeforeUninit`
  reach the UI threads with a WH_CALLWNDPROC hook and a registered message
  (`ShadePatcher_RunFromWindowThread_taskbar-styler`), synchronously.
- The CLSID of the TAP object is `{7D2E9F41-3C6B-4A8E-9B1D-5F0C2A7E8D31}`; nothing registers it in the registry
  (the diagnostics load the DLL by path), so nothing needs to be cleaned up by the installer.
- Build risks to look at first if the compiler complains: `winrt::implements` with the two classic COM
  interfaces declared at the top of the file (`IXamlDiagnosticsTestHooks`, `IGraphicsEffectD2D1Interop`, both
  `__declspec(uuid)`); `XamlCompositionBrushBaseT<XamlBlurBrush>` (the WindhawkBlur brush); the
  `std::unordered_map` keys of projected types (`DependencyProperty`, `VisualStateGroup`), which rely on
  C++/WinRT's `std::hash` specializations. All three patterns are the same ones the original and TranslucentTB
  compile with.
