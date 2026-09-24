# Port notes: taskbar-clock-customization

Source file: `src/core/mods/taskbar_clock_customization.cpp`
Original: Windhawk mod `taskbar-clock-customization` v1.8 by m417z (credited in `basedOn` / `originalAuthor`).

## 1. Declaration

```c
SP_MOD_DECLARE(g_modTaskbarClockCustomization);
```

Mod id: `taskbar-clock-customization`. `minOsBuild` 22000, `targets` SP_TARGET_EXPLORER, `flags` 0.

## 2. Exception handling

**Yes**, the file uses C++/WinRT (Windows.UI.Xaml) to style the clock's TextBlocks, so its `ClCompile` entry in
`core.vcxproj` needs `<ExceptionHandling>Sync</ExceptionHandling>`, exactly like `taskbar_menu_entry.cpp`.
No extra libraries: the WinRT calls resolve through `runtimeobject.lib`, which the project already links;
`GetTimeFormatEx` / `GetDateFormatEx` / registry / `GetLocaleInfoEx` are in kernel32 / advapi32.

## 3. Settings (HKCU\Software\ShadePatcher\Mods\taskbar-clock-customization)

| Name | Type | Default | Meaning |
|------|------|---------|---------|
| `Enabled` | dword | 0 | Engine toggle. |
| `TimeFormat` | sz | "" | GetTimeFormatEx picture for `%time%`. Empty = exactly what the shell asked for (respects the Windows "show seconds" option). Several pictures may be separated with `;` and reached as `%time2%`, `%time3%` ... A `;` inside single quotes is part of the picture. |
| `DateFormat` | sz | "" | GetDateFormatEx picture for `%date%`. Empty = what the shell asked for. `;` separates extra pictures reached as `%date2%` ... |
| `WeekdayFormat` | sz | "dddd" | Picture for `%weekday%`: `dddd` (full name), `ddd` (short name) or any date picture. If it contains a comma it is read as seven names of the user's own, Sunday first (this replaces the original's `WeekdayFormat=custom` + `WeekdayFormatCustom` pair). |
| `TopLine` | sz | "" | Template for the upper clock line. Empty = `%time%` (the shell's own content); `-` = do not touch this line at all. |
| `BottomLine` | sz | "" | Template for the lower clock line. Empty = `%date%`; `-` = do not touch. |
| `MiddleLine` | sz | "" | Extra line. The Windows 11 clock only has two text blocks, so when set (and BottomLine is not `-`) it is rendered as the first line of the lower block: `MiddleLine` + newline + `BottomLine`. Needs a smaller FontSize to fit the 48 px taskbar. Empty or `-` = none. |
| `FontSize` | dword | 0 | Font size in XAML pixels for both lines; 0 = default. Values outside 1..200 are treated as 0. |
| `TextColor` | sz | "" | `RRGGBB` (also accepts `#RRGGBB` and `AARRGGBB`) for both lines; empty = default. Anything unparsable is ignored and logged. |

Placeholders accepted in TopLine / BottomLine / MiddleLine (same spelling as the original):
`%time%`, `%time2%`..`%time9%`, `%date%`, `%date2%`..`%date9%`, `%weekday%`, `%weekday_num%`, `%weeknum%`,
`%weeknum_iso%`, `%dayofyear%`, `%timezone%`, `%newline%`, `%n%`. Unknown `%...%` text is copied literally.

### Example values a user could type

TimeFormat (GetTimeFormatEx pictures, https://learn.microsoft.com/windows/win32/api/datetimeapi/nf-datetimeapi-gettimeformatex):
- `HH:mm` -> 14:05
- `HH:mm:ss` -> 14:05:09 (ticks only if Windows' "Show seconds in system tray clock" is on)
- `h:mm tt` -> 2:05 PM
- `hh':'mm':'ss tt` -> 02:05:09 PM (the original's default)
- `HH'h'mm` -> 14h05
- `HH:mm;h tt` -> `%time%` = 14:05, `%time2%` = 2 PM

DateFormat (day/month/year pictures, https://learn.microsoft.com/windows/win32/intl/day--month--year--and-era-format-pictures):
- `dd.MM.yyyy` -> 19.09.2026
- `ddd, MMM dd yyyy` -> Sat, Sep 19 2026
- `ddd',' MMM dd yyyy` -> Sat, Sep 19 2026 (the original's default; quoted comma)
- `dddd d MMMM` -> Saturday 19 September
- `yyyy-MM-dd` -> 2026-09-19
- `d MMM;yyyy` -> `%date%` = 19 Sep, `%date2%` = 2026

WeekdayFormat:
- `dddd` -> Saturday
- `ddd` -> Sat
- `Sun, Mon, Tue, Wed, Thu, Fri, Sat` -> the name for today from this list
- `Paz, Pzt, Sal, Car, Per, Cum, Cmt` -> Turkish short names of the user's own

TopLine:
- `%time%` (same as empty)
- `%date% | %time%` (the original's default look, one line)
- `%weekday% %time%`
- `%time% (UTC%timezone%)`
- `W%weeknum_iso% %time%`

BottomLine:
- `%date%` (same as empty)
- `%weekday%, %date%`
- `%date% - day %dayofyear%`
- `%date%%n%%weekday%` (two lines in the lower block)
- `-` (keep the shell's own date text)

MiddleLine:
- `%weekday%` (the original's Windows 10 default; needs FontSize around 9 to fit)

FontSize: `10`, `12`, `14`, `0` (default).

TextColor: `FF8000`, `#00C8FF`, `80FFFFFF` (half transparent white), empty (default).

## 4. Proposed settings.reg block

Goes under the taskbar heading of the Mods page. `%R:135x%` are the string ids assigned to this mod.

```
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-clock-customization]
;b %R:1350%
"Enabled"=dword:00000000
;e %R:1359%
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-clock-customization]
;w %R:1351%
;%R:1351%
;
"TimeFormat"=""
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-clock-customization]
;w %R:1352%
;%R:1352%
;
"DateFormat"=""
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-clock-customization]
;w %R:1353%
;%R:1353%
;dddd
"WeekdayFormat"=""
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-clock-customization]
;w %R:1354%
;%R:1354%
;%time%
"TopLine"=""
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-clock-customization]
;w %R:1355%
;%R:1355%
;%date%
"BottomLine"=""
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-clock-customization]
;w %R:1356%
;%R:1356%
;
"MiddleLine"=""
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-clock-customization]
;c 6 %R:1357%
;x 0 %R:1357%
;x 10 10
;x 11 11
;x 12 12
;x 14 14
;x 16 16
"FontSize"=dword:00000000
[HKEY_CURRENT_USER\Software\ShadePatcher\Mods\taskbar-clock-customization]
;w %R:1358%
;%R:1358%
;
"TextColor"=""
```

Notes for the integrator:
- `;w` is the text-input directive (`;prompt` line, then `;default` line, then the value line); the default
  line is only what the input box is pre-filled with, the mod itself treats a missing/empty value as default.
  The `;` prompt lines reuse the label id; use dedicated prompt ids if the GUI wants longer prompts.
- FontSize is offered as a `;c` list so no numeric input control is needed; if the GUI grows one, a free
  number is fine (the mod clamps to 0..200). The `;x 0` entry's label should read "Default".
- 1359 is a plain `;e` help line under the toggle listing the placeholders; drop it if the page gets crowded.

### String ids and texts

| Id | English | Turkish |
|----|---------|---------|
| 1350 | Customize the taskbar clock text and style | Görev çubuğu saatinin metnini ve görünümünü özelleştir |
| 1351 | Time format (empty = Windows default, e.g. HH:mm or h:mm tt) | Saat biçimi (boş = Windows varsayılanı, örn. HH:mm veya h:mm tt) |
| 1352 | Date format (empty = Windows default, e.g. dd.MM.yyyy or ddd, MMM dd) | Tarih biçimi (boş = Windows varsayılanı, örn. dd.MM.yyyy veya ddd, MMM dd) |
| 1353 | Weekday format (dddd, ddd, or seven names separated by commas, Sunday first) | Gün adı biçimi (dddd, ddd ya da virgülle ayrılmış yedi ad, Pazar ilk) |
| 1354 | Top line (empty = %time%, "-" = leave unchanged) | Üst satır (boş = %time%, "-" = değiştirme) |
| 1355 | Bottom line (empty = %date%, "-" = leave unchanged) | Alt satır (boş = %date%, "-" = değiştirme) |
| 1356 | Extra line above the bottom line (empty = none) | Alt satırın üstüne ek satır (boş = yok) |
| 1357 | Font size (Default) | Yazı boyutu (Varsayılan) |
| 1358 | Text color as RRGGBB (empty = default) | Metin rengi, RRGGBB olarak (boş = varsayılan) |
| 1359 | Placeholders: %time% %date% %weekday% %weekday_num% %weeknum% %weeknum_iso% %dayofyear% %timezone% %n% | Yer tutucular: %time% %date% %weekday% %weekday_num% %weeknum% %weeknum_iso% %dayofyear% %timezone% %n% |

Suggested `strings.h` names: `IDS_MOD_CLOCK`, `IDS_MOD_CLOCK_TIMEFORMAT`, `IDS_MOD_CLOCK_DATEFORMAT`,
`IDS_MOD_CLOCK_WEEKDAYFORMAT`, `IDS_MOD_CLOCK_TOPLINE`, `IDS_MOD_CLOCK_BOTTOMLINE`, `IDS_MOD_CLOCK_MIDDLELINE`,
`IDS_MOD_CLOCK_FONTSIZE`, `IDS_MOD_CLOCK_TEXTCOLOR`, `IDS_MOD_CLOCK_HELP`.

Note: 1357 is used both as the `;c` label and the `;x 0` label above; if that reads oddly, put "Default" in
the first `;x` line literally (it is not localized then) or spend 1359 on it and drop the help line.

## 5. What was left out and why

- **Web feeds, weather (`%web*%`, `%weather%`)**: network thread, HTML/XML scraping, regex search/replace. Out of scope.
- **Performance metrics (`%cpu%`, `%ram%`, `%gpu%`, `%upload_speed%`, `%battery%` ...)**: PDH sessions, DXGI, power APIs. Out of scope.
- **Media player info (`%media_*%`)**: GlobalSystemMediaTransportControls session. Out of scope.
- **Tooltip customization (`TooltipLine`, `TooltipLineMode`)**: the tooltip builders are still hooked, but only to
  step aside while the shell formats the tooltip, so it stays the system's own.
- **`ShowSeconds` and the per-second refresh machinery** (GetLocalTime sentinel hook, `ICalendar::Second`,
  `ThreadPoolTimer::CreateTimer` and its lambda). Windows 11 has its own "Show seconds in system tray clock"
  option (Settings > Personalization > Taskbar > Taskbar behaviors) that makes RefreshIcon run every second;
  the mod honours whatever the shell asks. A `ss` in TimeFormat only ticks when that option is on; documented
  in the label text.
- **Time zones (`TimeZones` list, `%time_tz<n>%`, `%date_tz<n>%`, `%weekday_tz<n>%`)**: the engine has no list
  settings; not core.
- **`DateLocale`**: the user's default locale is always used (the original's default). Could be added later
  as one more sz setting.
- **Layout (`MaxWidth`, `TextSpacing`, `Width`, `Height`)**, **per-line style extras** (hidden, alignment,
  font family/weight/style/stretch, character spacing, line height): only FontSize and TextColor are ported,
  applied to both lines.
- **Windows 10 taskbar and ExplorerPatcher (`oldTaskbarOnWin11`)**: all ClockButton hooks, GetDateFormatW,
  the LoadLibraryExW hook. Windows 11 XAML taskbar only.
- **`SendMessageW` hook** (refreshed web content after resume): no web content.
- **Array settings**: `WeekdayFormatCustom` became "commas inside WeekdayFormat"; extra time/date pictures are
  `;`-separated inside the single format string exactly as in the original.

Behavioural differences worth knowing:
- Empty TopLine/BottomLine mean `%time%` / `%date%` (the shell's own content) rather than the original's
  `%date% | %time%` / `%web1%` defaults, so enabling the mod with nothing typed changes nothing visible.
- Empty TimeFormat/DateFormat reproduce the flags and picture the shell itself passed (captured in the hook),
  so "default" is pixel-identical to stock, including the seconds option.
- MiddleLine is folded into the lower text block (see table) because the XAML clock has no third block.

## 6. Engine-facing notes

- **Module choice**: on this machine Taskbar.View.dll is 2607.28001 and the `winrt::SystemTray::*` symbols
  live in `SystemTray.dll` (2607.28000), as the Windhawk source notes for 2604+. The mod does
  `SP_WaitForModule(L"Taskbar.View.dll")` as required; inside that callback it hooks `SystemTray.dll` if it is
  loaded, otherwise probes Taskbar.View.dll for the RefreshIcon symbol (older builds) with a resolve-only,
  optional entry, and otherwise issues a second `SP_WaitForModule(L"SystemTray.dll")`. Nested registration from
  a callback is fine: `modules.c` runs callbacks outside its critical section. The symbol engine will need to
  download the PDB for SystemTray.dll (new module for the cache).
- **Hooks**: two export hooks on kernelbase `GetTimeFormatEx` / `GetDateFormatEx` in one transaction at Init
  (active only on the thread inside RefreshIcon); nine symbol hooks in the tray module: `RefreshIcon`
  (required), `ClockSystemTrayIconDataModel2::RefreshIcon`, four `GetTimeToolTipString[2]` spellings plus the
  21H2 one, `DateTimeIconContent::OnApplyTemplate`, `BadgeIconContent::get_ViewModel` (all optional).
- **Refresh trigger**: `NudgeClock` writes and deletes a temp REG_SZ value under
  `HKCU\Control Panel\TimeDate\AdditionalClocks` (the shell watches it), only when this explorer owns
  `Shell_TrayWnd`. Called after the hooks land, in AfterInit, SettingsChanged, BeforeUninit and Uninit.
- **Unload**: BeforeUninit disables the style, bumps the style counter, nudges and waits up to 2 s for every
  styled clock to be restored through the still-live `get_ViewModel` hook; Uninit nudges once more so the
  shell re-formats its own text after the format hooks are gone.
- If `Init` cannot hook the two exports or register the module wait it returns FALSE.

## 7. Verifying on a live shell

1. Enable the mod, restart explorer (or let the engine load it). With no settings typed the clock must look
   exactly as before.
2. Set `TopLine` to `%weekday% %time%` and `BottomLine` to `%date% W%weeknum_iso%`: within a second (the nudge)
   the clock should read e.g. `Saturday 14:05` over `19.09.2026 W38`. Hover the clock: the tooltip must still
   show the normal full date (tooltip untouched).
3. Set `TimeFormat` to `h:mm tt` and `DateFormat` to `ddd, MMM dd`: the same lines now read `Saturday 2:05 PM`
   / `Sat, Sep 19 W38`. Set `TimeFormat` to `HH:mm:ss`, turn on Windows' "Show seconds in system tray clock":
   seconds tick each second.
4. Set `TopLine` to `-`: the top line goes back to the stock time text while the bottom stays custom.
5. Set `FontSize` to 10 and `TextColor` to `FF8000`: both lines shrink and turn orange without a restart.
   Set `MiddleLine` to `%weekday%`: a third line appears between the two (fits with FontSize 9-10).
6. Clear FontSize/TextColor: the clock returns to the default size and colour on the next refresh.
7. Disable the mod: the clock text and style return to stock within a second; open the clock flyout and the
   calendar to confirm they were never affected.
8. With `Logging`=2 the log shows "Clock hooks installed in SystemTray.dll (style hooks: yes)" and one
   "Clock style N applied" line per style change.
