//
// taskbar-clock-customization - custom text and text style for the Windows 11 taskbar clock.
//
// Adapted from the idea behind the Windhawk mod "Taskbar Clock Customization" (taskbar-clock-customization) by
// m417z. Only its core is ported: the time and date format pictures, the top and bottom line templates with
// their date/time placeholders, and the font size and colour of the two clock lines. The web feeds, weather,
// performance counters, media player info, tooltip text, time zones and layout tweaks of the original are left
// out; the implementation here is written against this engine's API.
//
// How the clock gets its text
// ---------------------------
// The XAML clock is refreshed by
//     winrt::SystemTray::implementation::ClockSystemTrayIconDataModel::RefreshIcon
// which takes the local time and turns it into two strings with the ordinary kernelbase exports
// GetTimeFormatEx (the top line) and GetDateFormatEx (the bottom line). Those two exports are hooked for the
// whole process, but the hooks only act while the calling thread is inside RefreshIcon; every other caller in
// explorer.exe gets the untouched function. RefreshIcon itself is hooked for nothing more than knowing that.
//
// Inside RefreshIcon the shell also builds the clock's tooltip through GetTimeToolTipString, and that text is
// meant to stay the system's own. Those functions are hooked only to raise a flag that makes the format hooks
// step aside while they run.
//
// Where the types live
// --------------------
// Up to Taskbar.View.dll 2603 the SystemTray types are in Taskbar.View.dll; from 2604 on they moved into
// SystemTray.dll (build 26200 has 2607). The mod waits for Taskbar.View.dll and then hooks whichever of the two
// actually holds the clock: SystemTray.dll when it is loaded, Taskbar.View.dll when the symbol resolves there,
// and otherwise it waits for SystemTray.dll to appear.
//
// Text style
// ----------
// The clock control, SystemTray.DateTimeIconContent, holds two TextBlocks named TimeInnerTextBlock and
// DateInnerTextBlock under a StackPanel in its ContainerGrid. Its OnApplyTemplate is the moment the tree exists,
// and get_ViewModel on the same control is called on every refresh, which is how a changed style reaches a
// clock that is already on screen. Both are applied with C++/WinRT, so this file is compiled with exceptions on
// and every path XAML can call into is wrapped.
//
// Making the clock refresh
// ------------------------
// The shell watches HKCU\Control Panel\TimeDate\AdditionalClocks and refreshes the clock when anything under it
// changes. Writing and deleting a temporary value there is how a settings change shows up at once.
//
#define SP_MOD_ID "taskbar-clock-customization"
#include "engine/modapi.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <unknwn.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>

namespace {

using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using winrt::Windows::Foundation::IInspectable;
using winrt::Windows::UI::Xaml::Media::VisualTreeHelper;

// ---------------------------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------------------------

constexpr size_t kFormatMax = 256;   // one format picture
constexpr size_t kLineMax = 512;     // one line template

// What the shell is told when it asks how large a buffer it needs (older builds did that with cch = 0).
constexpr int kFormattedBufferSize = 256;

struct Settings
{
    wchar_t timeFormat[kFormatMax];     // GetTimeFormatEx picture, empty = what the shell asked for
    wchar_t dateFormat[kFormatMax];     // GetDateFormatEx picture, empty = what the shell asked for
    wchar_t weekdayFormat[kFormatMax];  // "dddd", "ddd", any date picture, or 7 comma-separated names
    wchar_t topLine[kLineMax];          // template for the time line
    wchar_t bottomLine[kLineMax];       // template for the date line (with the middle line folded in)
    bool topCustom;                     // false when the user wrote "-": leave the shell's text alone
    bool bottomCustom;
    int fontSize;                       // 0 = default
    bool hasTextColor;
    winrt::Windows::UI::Color textColor;
};

// Written on the engine thread, copied by the hooks on the taskbar's thread.
std::mutex g_settingsLock;
Settings g_settings{};

void CopySettings(Settings& out)
{
    std::lock_guard<std::mutex> guard(g_settingsLock);
    out = g_settings;
}

// "RRGGBB", "#RRGGBB" or "AARRGGBB". Anything else means no colour.
bool ParseColor(const wchar_t* text, winrt::Windows::UI::Color& color)
{
    if (!text)
    {
        return false;
    }
    if (*text == L'#')
    {
        ++text;
    }

    size_t len = wcslen(text);
    if (len != 6 && len != 8)
    {
        return false;
    }

    unsigned value = 0;
    for (size_t i = 0; i < len; ++i)
    {
        wchar_t c = text[i];
        unsigned digit;
        if (c >= L'0' && c <= L'9')
        {
            digit = c - L'0';
        }
        else if (c >= L'a' && c <= L'f')
        {
            digit = 10 + (c - L'a');
        }
        else if (c >= L'A' && c <= L'F')
        {
            digit = 10 + (c - L'A');
        }
        else
        {
            return false;
        }
        value = (value << 4) | digit;
    }

    color.A = (len == 8) ? (uint8_t)(value >> 24) : 255;
    color.R = (uint8_t)(value >> 16);
    color.G = (uint8_t)(value >> 8);
    color.B = (uint8_t)value;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// The refresh in progress
//
// Everything here is only touched on the thread that is inside RefreshIcon, so it needs no lock: the thread id
// is the guard, and it is the only thing read from elsewhere.
// ---------------------------------------------------------------------------------------------------------------

std::atomic<DWORD> g_refreshThreadId{ 0 };
std::atomic<bool> g_inTooltip{ false };

// The time the shell is formatting, and how it asked for the two strings. An empty format picture in the
// settings reuses the shell's own request, so "leave empty" gives exactly what the shell would have shown.
SYSTEMTIME g_refreshTime{};
DWORD g_shellTimeFlags = TIME_NOSECONDS;
wchar_t g_shellTimeFormat[kFormatMax] = {};
bool g_shellTimeFormatSet = false;
DWORD g_shellDateFlags = DATE_AUTOLAYOUT;
wchar_t g_shellDateFormat[kFormatMax] = {};
bool g_shellDateFormatSet = false;

bool IsRefreshCall()
{
    return g_refreshThreadId.load(std::memory_order_relaxed) == GetCurrentThreadId() &&
           !g_inTooltip.load(std::memory_order_relaxed);
}

void RememberRefreshTime(const SYSTEMTIME* time)
{
    if (time && time->wYear != 0)
    {
        g_refreshTime = *time;
    }
}

using GetTimeFormatEx_t = decltype(&GetTimeFormatEx);
using GetDateFormatEx_t = decltype(&GetDateFormatEx);
GetTimeFormatEx_t g_origGetTimeFormatEx = nullptr;
GetDateFormatEx_t g_origGetDateFormatEx = nullptr;

// ---------------------------------------------------------------------------------------------------------------
// Calendar arithmetic for the placeholders
// ---------------------------------------------------------------------------------------------------------------

bool IsLeapYear(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int DayOfYear(const SYSTEMTIME& t)
{
    static const int kBefore[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    int month = (t.wMonth >= 1 && t.wMonth <= 12) ? t.wMonth : 1;
    int day = t.wDay + kBefore[month - 1];
    if (month > 2 && IsLeapYear(t.wYear))
    {
        ++day;
    }
    return day;
}

// Sunday = 0, as SYSTEMTIME counts.
int DayOfWeekOfJan1(int year)
{
    SYSTEMTIME jan1{};
    jan1.wYear = (WORD)year;
    jan1.wMonth = 1;
    jan1.wDay = 1;
    FILETIME ft{};
    if (SystemTimeToFileTime(&jan1, &ft) && FileTimeToSystemTime(&ft, &jan1))
    {
        return jan1.wDayOfWeek;
    }
    return 0;
}

// The user's first day of week, Sunday = 0.
int FirstDayOfWeek()
{
    DWORD value = 0;
    if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK | LOCALE_RETURN_NUMBER,
                        (LPWSTR)&value, sizeof(value) / sizeof(wchar_t)) == 0)
    {
        return 0;
    }
    // The locale counts from Monday; SYSTEMTIME from Sunday.
    return (int)((value + 1) % 7);
}

// Week 1 is the week that holds 1 January; later weeks start on the user's first day of week.
int WeekNumber(const SYSTEMTIME& t)
{
    int start = FirstDayOfWeek();
    int jan1 = DayOfWeekOfJan1(t.wYear);
    int offset = (start - jan1 + 7) % 7;
    if (offset == 0)
    {
        offset = 7;
    }
    int secondWeekDay = 1 + offset;   // day of year on which week 2 starts
    int doy = DayOfYear(t);
    if (doy < secondWeekDay)
    {
        return 1;
    }
    return 2 + (doy - secondWeekDay) / 7;
}

int IsoWeeksInYear(int year)
{
    auto p = [](int y) { return (y + y / 4 - y / 100 + y / 400) % 7; };
    return (p(year) == 4 || p(year - 1) == 3) ? 53 : 52;
}

int IsoWeekNumber(const SYSTEMTIME& t)
{
    int isoDow = (t.wDayOfWeek + 6) % 7 + 1;   // Monday = 1 .. Sunday = 7
    int week = (DayOfYear(t) - isoDow + 10) / 7;
    if (week < 1)
    {
        return IsoWeeksInYear(t.wYear - 1);
    }
    if (week > IsoWeeksInYear(t.wYear))
    {
        return 1;
    }
    return week;
}

std::wstring TimeZoneOffset()
{
    TIME_ZONE_INFORMATION tz{};
    DWORD kind = GetTimeZoneInformation(&tz);
    long bias = tz.Bias + ((kind == TIME_ZONE_ID_DAYLIGHT) ? tz.DaylightBias : tz.StandardBias);
    long absBias = bias < 0 ? -bias : bias;
    wchar_t buffer[16];
    // UTC = local + bias, so the sign shown is the opposite of the bias.
    swprintf_s(buffer, L"%c%02ld:%02ld", bias <= 0 ? L'+' : L'-', absBias / 60, absBias % 60);
    return buffer;
}

// ---------------------------------------------------------------------------------------------------------------
// Placeholders
//
// A template is copied through with every %token% replaced. Tokens the mod does not know are copied as they
// are, so a stray percent sign costs nothing.
// ---------------------------------------------------------------------------------------------------------------

// Splits "fmt1;fmt2" on semicolons that are outside single quotes (a quoted ';' is part of a picture).
std::vector<std::wstring> SplitFormats(std::wstring_view s)
{
    std::vector<std::wstring> parts;
    size_t start = 0;
    bool quoted = false;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == L'\'')
        {
            quoted = !quoted;
        }
        else if (s[i] == L';' && !quoted)
        {
            parts.emplace_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    parts.emplace_back(s.substr(start));
    return parts;
}

std::wstring_view Trim(std::wstring_view s)
{
    while (!s.empty() && (s.front() == L' ' || s.front() == L'\t'))
    {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t'))
    {
        s.remove_suffix(1);
    }
    return s;
}

bool StartsWith(std::wstring_view s, std::wstring_view prefix)
{
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// "%time3%" against prefix "%time" gives 3; anything that is not prefix + one digit 1-9 + '%' gives 0.
int DigitToken(std::wstring_view s, std::wstring_view prefix)
{
    if (s.size() < prefix.size() + 2 || !StartsWith(s, prefix))
    {
        return 0;
    }
    wchar_t digit = s[prefix.size()];
    if (digit < L'1' || digit > L'9' || s[prefix.size() + 1] != L'%')
    {
        return 0;
    }
    return digit - L'0';
}

class LineFormatter
{
public:
    LineFormatter(const Settings& settings, const SYSTEMTIME& time) : m_settings(settings), m_time(time) {}

    // Writes the expanded template into out (cch characters, null included) and returns the length written.
    // A template that does not fit ends in "...", as the original does.
    int Format(std::wstring_view tpl, wchar_t* out, size_t cch)
    {
        if (!out || cch == 0)
        {
            return 0;
        }

        wchar_t* p = out;
        wchar_t* end = out + cch;
        std::wstring value;

        while (!tpl.empty() && end - p > 1)
        {
            if (tpl[0] == L'%')
            {
                size_t consumed = Resolve(tpl, value);
                if (consumed)
                {
                    size_t room = (size_t)(end - p) - 1;
                    size_t take = std::min(room, value.size());
                    wmemcpy(p, value.data(), take);
                    p += take;
                    if (take < value.size())
                    {
                        break;      // the rest cannot fit; tpl stays non-empty and marks the overflow
                    }
                    tpl.remove_prefix(consumed);
                    continue;
                }
            }
            *p++ = tpl[0];
            tpl.remove_prefix(1);
        }

        if (!tpl.empty() && cch >= 4)
        {
            // p sits at the last slot; the three characters before the terminator become the ellipsis.
            p = end - 1;
            p[-1] = L'.';
            p[-2] = L'.';
            p[-3] = L'.';
        }

        *p = L'\0';
        return (int)(p - out);
    }

private:
    // Returns the number of template characters the token took, 0 if s does not start with a known token.
    size_t Resolve(std::wstring_view s, std::wstring& value)
    {
        struct Simple
        {
            std::wstring_view token;
            std::wstring (LineFormatter::*get)();
        };
        static const Simple kSimple[] =
        {
            { L"%time%",        &LineFormatter::Time0 },
            { L"%date%",        &LineFormatter::Date0 },
            { L"%weekday%",     &LineFormatter::Weekday },
            { L"%weekday_num%", &LineFormatter::WeekdayNum },
            { L"%weeknum%",     &LineFormatter::WeekNum },
            { L"%weeknum_iso%", &LineFormatter::WeekNumIso },
            { L"%dayofyear%",   &LineFormatter::DayOfYearText },
            { L"%timezone%",    &LineFormatter::TimeZone },
            { L"%newline%",     &LineFormatter::Newline },
            { L"%n%",           &LineFormatter::Newline },
        };

        for (const auto& entry : kSimple)
        {
            if (StartsWith(s, entry.token))
            {
                value = (this->*entry.get)();
                return entry.token.size();
            }
        }

        // %time2% .. %time9% and %date2% .. %date9%: the extra pictures after ';' in the format setting.
        if (int digit = DigitToken(s, L"%time"))
        {
            value = digit >= 2 ? Time(digit - 1) : L"-";
            return 5 + 2;
        }
        if (int digit = DigitToken(s, L"%date"))
        {
            value = digit >= 2 ? Date(digit - 1) : L"-";
            return 5 + 2;
        }

        return 0;
    }

    std::wstring Time0() { return Time(0); }
    std::wstring Date0() { return Date(0); }
    std::wstring Newline() { return L"\n"; }

    std::wstring Time(size_t part)
    {
        auto parts = SplitFormats(m_settings.timeFormat);
        if (part >= parts.size())
        {
            return L"-";
        }

        const wchar_t* picture = parts[part].empty() ? nullptr : parts[part].c_str();
        DWORD flags = 0;
        if (!picture)
        {
            // No picture of the user's own: ask for what the shell asked for.
            flags = g_shellTimeFlags;
            picture = g_shellTimeFormatSet ? g_shellTimeFormat : nullptr;
        }

        wchar_t buffer[kFormattedBufferSize];
        if (!g_origGetTimeFormatEx ||
            g_origGetTimeFormatEx(nullptr, flags, &m_time, picture, buffer, ARRAYSIZE(buffer)) == 0)
        {
            return L"-";
        }
        return buffer;
    }

    std::wstring Date(size_t part)
    {
        auto parts = SplitFormats(m_settings.dateFormat);
        if (part >= parts.size())
        {
            return L"-";
        }

        const wchar_t* picture = parts[part].empty() ? nullptr : parts[part].c_str();
        DWORD flags = DATE_AUTOLAYOUT;
        if (!picture)
        {
            flags = g_shellDateFlags;
            picture = g_shellDateFormatSet ? g_shellDateFormat : nullptr;
        }

        wchar_t buffer[kFormattedBufferSize];
        if (!g_origGetDateFormatEx ||
            g_origGetDateFormatEx(nullptr, flags, &m_time, picture, buffer, ARRAYSIZE(buffer), nullptr) == 0)
        {
            return L"-";
        }
        return buffer;
    }

    std::wstring Weekday()
    {
        std::wstring_view format = m_settings.weekdayFormat;

        // Seven names of the user's own, Sunday first.
        if (format.find(L',') != std::wstring_view::npos)
        {
            std::vector<std::wstring_view> names;
            size_t start = 0;
            while (true)
            {
                size_t comma = format.find(L',', start);
                names.push_back(Trim(format.substr(start, comma == std::wstring_view::npos ? std::wstring_view::npos : comma - start)));
                if (comma == std::wstring_view::npos)
                {
                    break;
                }
                start = comma + 1;
            }
            if (m_time.wDayOfWeek < names.size())
            {
                return std::wstring(names[m_time.wDayOfWeek]);
            }
            return L"-";
        }

        wchar_t buffer[kFormattedBufferSize];
        const wchar_t* picture = format.empty() ? L"dddd" : (const wchar_t*)m_settings.weekdayFormat;
        if (!g_origGetDateFormatEx ||
            g_origGetDateFormatEx(nullptr, DATE_AUTOLAYOUT, &m_time, picture, buffer, ARRAYSIZE(buffer), nullptr) == 0)
        {
            return L"-";
        }
        return buffer;
    }

    std::wstring WeekdayNum()
    {
        return std::to_wstring(1 + (7 + m_time.wDayOfWeek - FirstDayOfWeek()) % 7);
    }

    std::wstring TwoDigits(int n)
    {
        wchar_t buffer[16];
        swprintf_s(buffer, L"%02d", n);
        return buffer;
    }

    std::wstring WeekNum() { return TwoDigits(WeekNumber(m_time)); }
    std::wstring WeekNumIso() { return TwoDigits(IsoWeekNumber(m_time)); }
    std::wstring DayOfYearText() { return std::to_wstring(DayOfYear(m_time)); }
    std::wstring TimeZone() { return TimeZoneOffset(); }

    const Settings& m_settings;
    const SYSTEMTIME& m_time;
};

// ---------------------------------------------------------------------------------------------------------------
// The format hooks
// ---------------------------------------------------------------------------------------------------------------

int WINAPI GetTimeFormatEx_Hook(LPCWSTR lpLocaleName, DWORD dwFlags, const SYSTEMTIME* lpTime, LPCWSTR lpFormat,
                                LPWSTR lpTimeStr, int cchTime)
{
    if (!IsRefreshCall())
    {
        return g_origGetTimeFormatEx(lpLocaleName, dwFlags, lpTime, lpFormat, lpTimeStr, cchTime);
    }

    // Keep the shell's request so an empty picture in the settings reproduces it.
    g_shellTimeFlags = dwFlags;
    g_shellTimeFormatSet = lpFormat != nullptr;
    if (lpFormat)
    {
        wcsncpy_s(g_shellTimeFormat, lpFormat, _TRUNCATE);
    }
    RememberRefreshTime(lpTime);

    Settings settings;
    CopySettings(settings);
    if (!settings.topCustom)
    {
        return g_origGetTimeFormatEx(lpLocaleName, dwFlags, lpTime, lpFormat, lpTimeStr, cchTime);
    }

    if (cchTime <= 0 || !lpTimeStr)
    {
        return kFormattedBufferSize;
    }

    LineFormatter formatter(settings, g_refreshTime);
    return formatter.Format(settings.topLine, lpTimeStr, (size_t)cchTime) + 1;
}

int WINAPI GetDateFormatEx_Hook(LPCWSTR lpLocaleName, DWORD dwFlags, const SYSTEMTIME* lpDate, LPCWSTR lpFormat,
                                LPWSTR lpDateStr, int cchDate, LPCWSTR lpCalendar)
{
    // The long date is not one of the two clock lines (the flyout and the tooltip ask for it), so it passes.
    if (!IsRefreshCall() || (dwFlags & DATE_LONGDATE))
    {
        return g_origGetDateFormatEx(lpLocaleName, dwFlags, lpDate, lpFormat, lpDateStr, cchDate, lpCalendar);
    }

    g_shellDateFlags = dwFlags;
    g_shellDateFormatSet = lpFormat != nullptr;
    if (lpFormat)
    {
        wcsncpy_s(g_shellDateFormat, lpFormat, _TRUNCATE);
    }
    RememberRefreshTime(lpDate);

    Settings settings;
    CopySettings(settings);
    if (!settings.bottomCustom)
    {
        return g_origGetDateFormatEx(lpLocaleName, dwFlags, lpDate, lpFormat, lpDateStr, cchDate, lpCalendar);
    }

    if (cchDate <= 0 || !lpDateStr)
    {
        return kFormattedBufferSize;
    }

    LineFormatter formatter(settings, g_refreshTime);
    return formatter.Format(settings.bottomLine, lpDateStr, (size_t)cchDate) + 1;
}

// ---------------------------------------------------------------------------------------------------------------
// The clock's own functions
// ---------------------------------------------------------------------------------------------------------------

using RefreshIcon_t = void(WINAPI*)(void* pThis, void* clockUpdate);
RefreshIcon_t g_origRefreshIcon = nullptr;
RefreshIcon_t g_origRefreshIcon2 = nullptr;

void RefreshIconCommon(void* pThis, void* clockUpdate, RefreshIcon_t original)
{
    DWORD previous = g_refreshThreadId.exchange(GetCurrentThreadId(), std::memory_order_relaxed);
    original(pThis, clockUpdate);
    g_refreshThreadId.store(previous, std::memory_order_relaxed);
}

void WINAPI RefreshIcon_Hook(void* pThis, void* clockUpdate)
{
    RefreshIconCommon(pThis, clockUpdate, g_origRefreshIcon);
}

void WINAPI RefreshIcon2_Hook(void* pThis, void* clockUpdate)
{
    RefreshIconCommon(pThis, clockUpdate, g_origRefreshIcon2);
}

// The tooltip builders return an hstring through a hidden slot, so on x64 they carry five pointer-sized
// arguments in total. None is looked at: the hook only marks the tooltip as being built.
using ToolTip_t = void*(WINAPI*)(void* a1, void* a2, void* a3, void* a4, void* a5);
ToolTip_t g_origToolTip = nullptr;
ToolTip_t g_origToolTip2 = nullptr;
ToolTip_t g_origToolTipB = nullptr;
ToolTip_t g_origToolTipB2 = nullptr;
ToolTip_t g_origToolTipOld = nullptr;

void* ToolTipCommon(void* a1, void* a2, void* a3, void* a4, void* a5, ToolTip_t original)
{
    bool previous = g_inTooltip.exchange(true, std::memory_order_relaxed);
    void* result = original(a1, a2, a3, a4, a5);
    g_inTooltip.store(previous, std::memory_order_relaxed);
    return result;
}

void* WINAPI ToolTip_Hook(void* a1, void* a2, void* a3, void* a4, void* a5)
{
    return ToolTipCommon(a1, a2, a3, a4, a5, g_origToolTip);
}

void* WINAPI ToolTip2_Hook(void* a1, void* a2, void* a3, void* a4, void* a5)
{
    return ToolTipCommon(a1, a2, a3, a4, a5, g_origToolTip2);
}

void* WINAPI ToolTipB_Hook(void* a1, void* a2, void* a3, void* a4, void* a5)
{
    return ToolTipCommon(a1, a2, a3, a4, a5, g_origToolTipB);
}

void* WINAPI ToolTipB2_Hook(void* a1, void* a2, void* a3, void* a4, void* a5)
{
    return ToolTipCommon(a1, a2, a3, a4, a5, g_origToolTipB2);
}

void* WINAPI ToolTipOld_Hook(void* a1, void* a2, void* a3, void* a4, void* a5)
{
    return ToolTipCommon(a1, a2, a3, a4, a5, g_origToolTipOld);
}

// ---------------------------------------------------------------------------------------------------------------
// Text style
// ---------------------------------------------------------------------------------------------------------------

// Whether the settings ask for any style at all, and a counter bumped whenever they change. A clock element
// remembers the counter it was last styled with, so a refresh that changed nothing costs nothing.
std::atomic<bool> g_styleEnabled{ false };
std::atomic<DWORD> g_styleIndex{ 0 };

struct StyledClock
{
    winrt::weak_ref<FrameworkElement> element;
    DWORD styleIndex;
};

std::mutex g_styleLock;
std::vector<StyledClock> g_styledClocks;

FrameworkElement FindChildByName(FrameworkElement const& parent, const wchar_t* name)
{
    int count = VisualTreeHelper::GetChildrenCount(parent);
    for (int i = 0; i < count; ++i)
    {
        auto child = VisualTreeHelper::GetChild(parent, i).try_as<FrameworkElement>();
        if (child && child.Name() == name)
        {
            return child;
        }
    }
    return nullptr;
}

void ApplyTextBlockStyle(TextBlock const& block, bool enabled, const Settings& settings)
{
    if (enabled && settings.fontSize > 0)
    {
        block.FontSize((double)settings.fontSize);
    }
    else
    {
        block.ClearValue(TextBlock::FontSizeProperty());
    }

    if (enabled && settings.hasTextColor)
    {
        block.Foreground(Media::SolidColorBrush(settings.textColor));
    }
    else
    {
        block.ClearValue(TextBlock::ForegroundProperty());
    }
}

// Styles (or restores) one clock control. Runs on the taskbar's UI thread, inside a hooked call.
void ApplyClockStyle(FrameworkElement const& clock)
{
    bool enabled = g_styleEnabled.load(std::memory_order_relaxed);
    DWORD index = g_styleIndex.load(std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> guard(g_styleLock);

        bool known = false;
        for (auto it = g_styledClocks.begin(); it != g_styledClocks.end();)
        {
            auto element = it->element.get();
            if (!element)
            {
                it = g_styledClocks.erase(it);   // the control went away
                continue;
            }
            if (element == clock)
            {
                known = true;
                if (it->styleIndex == index)
                {
                    return;         // already at the current style
                }
            }
            ++it;
        }

        // A clock that was never styled and does not need to be is left untouched.
        if (!known && !enabled)
        {
            return;
        }
    }

    auto grid = FindChildByName(clock, L"ContainerGrid").try_as<Grid>();
    if (!grid)
    {
        SP_LogDebug(L"No ContainerGrid under the clock");
        return;
    }

    auto gridChildren = grid.Children();
    if (gridChildren.Size() == 0)
    {
        return;
    }
    auto panel = gridChildren.GetAt(0).try_as<StackPanel>();
    if (!panel)
    {
        SP_LogDebug(L"The clock's first child is not a StackPanel");
        return;
    }

    TextBlock dateBlock{ nullptr };
    TextBlock timeBlock{ nullptr };
    auto panelChildren = panel.Children();
    for (uint32_t i = 0; i < panelChildren.Size(); ++i)
    {
        auto block = panelChildren.GetAt(i).try_as<TextBlock>();
        if (!block)
        {
            continue;
        }
        if (block.Name() == L"DateInnerTextBlock")
        {
            dateBlock = block;
        }
        else if (block.Name() == L"TimeInnerTextBlock")
        {
            timeBlock = block;
        }
    }

    if (!dateBlock || !timeBlock)
    {
        SP_LogDebug(L"The clock's text blocks were not found");
        return;
    }

    Settings settings;
    CopySettings(settings);

    ApplyTextBlockStyle(timeBlock, enabled, settings);
    ApplyTextBlockStyle(dateBlock, enabled, settings);

    {
        std::lock_guard<std::mutex> guard(g_styleLock);
        bool found = false;
        for (auto& entry : g_styledClocks)
        {
            if (entry.element.get() == clock)
            {
                entry.styleIndex = index;
                found = true;
                break;
            }
        }
        if (!found)
        {
            g_styledClocks.push_back({ winrt::make_weak(clock), index });
        }
    }

    SP_LogDebug(L"Clock style %u applied (%s)", index, enabled ? L"custom" : L"default");
}

// TRUE when every clock the mod has touched carries the style counter `index` or newer.
bool AllClocksAt(DWORD index)
{
    std::lock_guard<std::mutex> guard(g_styleLock);
    for (const auto& entry : g_styledClocks)
    {
        if (entry.element.get() && entry.styleIndex < index)
        {
            return false;
        }
    }
    return true;
}

using OnApplyTemplate_t = HRESULT(WINAPI*)(void* pThis);
OnApplyTemplate_t g_origOnApplyTemplate = nullptr;

HRESULT WINAPI OnApplyTemplate_Hook(void* pThis)
{
    HRESULT hr = g_origOnApplyTemplate(pThis);

    try
    {
        FrameworkElement clock{ nullptr };
        if (pThis)
        {
            ((::IUnknown*)pThis)->QueryInterface(winrt::guid_of<FrameworkElement>(), winrt::put_abi(clock));
        }
        if (clock)
        {
            ApplyClockStyle(clock);
        }
    }
    catch (winrt::hresult_error const& e)
    {
        SP_LogError(L"Styling the clock failed: 0x%08X", (unsigned)e.code());
    }
    catch (...)
    {
        SP_LogError(L"Styling the clock failed");
    }

    return hr;
}

using GetViewModel_t = HRESULT(WINAPI*)(void* pThis, void** ppValue);
GetViewModel_t g_origGetViewModel = nullptr;

// Called on every refresh of every tray icon, so it does nothing unless a style is or was in force.
HRESULT WINAPI GetViewModel_Hook(void* pThis, void** ppValue)
{
    HRESULT hr = g_origGetViewModel(pThis, ppValue);

    bool interesting = g_styleEnabled.load(std::memory_order_relaxed);
    if (!interesting)
    {
        std::lock_guard<std::mutex> guard(g_styleLock);
        interesting = !g_styledClocks.empty();
    }
    if (!interesting || !pThis)
    {
        return hr;
    }

    try
    {
        IInspectable object{ nullptr };
        ((::IUnknown*)pThis)->QueryInterface(winrt::guid_of<IInspectable>(), winrt::put_abi(object));
        if (object && winrt::get_class_name(object) == L"SystemTray.DateTimeIconContent")
        {
            auto clock = object.try_as<FrameworkElement>();
            if (clock && clock.IsLoaded())
            {
                ApplyClockStyle(clock);
            }
        }
    }
    catch (winrt::hresult_error const& e)
    {
        SP_LogError(L"Restyling the clock failed: 0x%08X", (unsigned)e.code());
    }
    catch (...)
    {
        SP_LogError(L"Restyling the clock failed");
    }

    return hr;
}

// ---------------------------------------------------------------------------------------------------------------
// Making the clock refresh
// ---------------------------------------------------------------------------------------------------------------

bool OwnsTaskbar()
{
    HWND found = nullptr;
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        DWORD pid = 0;
        wchar_t className[32];
        if (GetWindowThreadProcessId(hwnd, &pid) && pid == GetCurrentProcessId() &&
            GetClassNameW(hwnd, className, ARRAYSIZE(className)) && _wcsicmp(className, L"Shell_TrayWnd") == 0)
        {
            *reinterpret_cast<HWND*>(lParam) = hwnd;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&found));
    return found != nullptr;
}

void NudgeClock()
{
    if (!OwnsTaskbar())
    {
        return;
    }

    constexpr wchar_t kTempValue[] = L"_temp_shadepatcher_taskbar-clock-customization";
    HKEY key = nullptr;
    LONG result = RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\TimeDate\\AdditionalClocks", 0, KEY_WRITE, &key);
    if (result != ERROR_SUCCESS)
    {
        DWORD disposition = 0;
        result = RegCreateKeyExW(HKEY_CURRENT_USER, L"Control Panel\\TimeDate\\AdditionalClocks", 0, nullptr,
                                 REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &key, &disposition);
    }
    if (result != ERROR_SUCCESS)
    {
        SP_LogDebug(L"The clock could not be nudged: %d", result);
        return;
    }

    if (RegSetValueExW(key, kTempValue, 0, REG_SZ, (const BYTE*)L"", sizeof(wchar_t)) == ERROR_SUCCESS)
    {
        RegDeleteValueW(key, kTempValue);
    }
    RegCloseKey(key);
}

// ---------------------------------------------------------------------------------------------------------------
// Installing the clock hooks
// ---------------------------------------------------------------------------------------------------------------

std::atomic<bool> g_clockHooked{ false };

const wchar_t* const kRefreshIconNames[] =
{
    L"private: void __cdecl winrt::SystemTray::implementation::ClockSystemTrayIconDataModel::RefreshIcon(class SystemTrayTelemetry::ClockUpdate &)",
};
const wchar_t* const kRefreshIcon2Names[] =
{
    L"private: void __cdecl winrt::SystemTray::implementation::ClockSystemTrayIconDataModel2::RefreshIcon(class SystemTrayTelemetry::ClockUpdate &)",
};
const wchar_t* const kToolTipNames[] =
{
    L"private: struct winrt::hstring __cdecl winrt::SystemTray::implementation::ClockSystemTrayIconDataModel::GetTimeToolTipString(struct _SYSTEMTIME const &,struct _SYSTEMTIME const &,class SystemTrayTelemetry::ClockUpdate &)",
};
const wchar_t* const kToolTip2Names[] =
{
    L"private: struct winrt::hstring __cdecl winrt::SystemTray::implementation::ClockSystemTrayIconDataModel2::GetTimeToolTipString(struct _SYSTEMTIME const &,struct _SYSTEMTIME const &,class SystemTrayTelemetry::ClockUpdate &)",
};
const wchar_t* const kToolTipBNames[] =
{
    L"private: struct winrt::hstring __cdecl winrt::SystemTray::implementation::ClockSystemTrayIconDataModel::GetTimeToolTipString2(struct _SYSTEMTIME const &,struct _SYSTEMTIME const &,class SystemTrayTelemetry::ClockUpdate &)",
};
const wchar_t* const kToolTipB2Names[] =
{
    L"private: struct winrt::hstring __cdecl winrt::SystemTray::implementation::ClockSystemTrayIconDataModel2::GetTimeToolTipString2(struct _SYSTEMTIME const &,struct _SYSTEMTIME const &,class SystemTrayTelemetry::ClockUpdate &)",
};
const wchar_t* const kToolTipOldNames[] =
{
    // Windows 11 21H2 only.
    L"private: struct winrt::hstring __cdecl winrt::SystemTray::implementation::ClockSystemTrayIconDataModel::GetTimeToolTipString(struct _SYSTEMTIME *,struct _TIME_DYNAMIC_ZONE_INFORMATION *,class SystemTrayTelemetry::ClockUpdate &)",
};
const wchar_t* const kOnApplyTemplateNames[] =
{
    L"public: virtual int __cdecl winrt::impl::produce<struct winrt::SystemTray::implementation::DateTimeIconContent,struct winrt::Windows::UI::Xaml::IFrameworkElementOverrides>::OnApplyTemplate(void)",
};
const wchar_t* const kGetViewModelNames[] =
{
    L"public: virtual int __cdecl winrt::impl::produce<struct winrt::SystemTray::implementation::BadgeIconContent,struct winrt::SystemTray::IBadgeIconContent>::get_ViewModel(void * *)",
};

// TRUE when the clock's refresh function is in this module, without patching anything.
bool ModuleHasClock(HMODULE module)
{
    void* address = nullptr;
    SP_SymbolHook probe[1] = {};
    probe[0].symbols = kRefreshIconNames;
    probe[0].symbolCount = ARRAYSIZE(kRefreshIconNames);
    probe[0].pOriginal = &address;
    probe[0].hookFunction = nullptr;
    probe[0].optional = TRUE;
    return SP_ResolveSymbols(module, probe, ARRAYSIZE(probe)) && address != nullptr;
}

BOOL InstallClockHooks(HMODULE module, const wchar_t* moduleName)
{
    if (g_clockHooked.exchange(true))
    {
        return TRUE;
    }

    SP_SymbolHook hooks[9] = {};

    hooks[0].symbols = kRefreshIconNames;
    hooks[0].symbolCount = ARRAYSIZE(kRefreshIconNames);
    hooks[0].pOriginal = (void**)&g_origRefreshIcon;
    hooks[0].hookFunction = (void*)RefreshIcon_Hook;
    hooks[0].optional = FALSE;

    hooks[1].symbols = kRefreshIcon2Names;
    hooks[1].symbolCount = ARRAYSIZE(kRefreshIcon2Names);
    hooks[1].pOriginal = (void**)&g_origRefreshIcon2;
    hooks[1].hookFunction = (void*)RefreshIcon2_Hook;
    hooks[1].optional = TRUE;

    hooks[2].symbols = kToolTipNames;
    hooks[2].symbolCount = ARRAYSIZE(kToolTipNames);
    hooks[2].pOriginal = (void**)&g_origToolTip;
    hooks[2].hookFunction = (void*)ToolTip_Hook;
    hooks[2].optional = TRUE;

    hooks[3].symbols = kToolTip2Names;
    hooks[3].symbolCount = ARRAYSIZE(kToolTip2Names);
    hooks[3].pOriginal = (void**)&g_origToolTip2;
    hooks[3].hookFunction = (void*)ToolTip2_Hook;
    hooks[3].optional = TRUE;

    hooks[4].symbols = kToolTipBNames;
    hooks[4].symbolCount = ARRAYSIZE(kToolTipBNames);
    hooks[4].pOriginal = (void**)&g_origToolTipB;
    hooks[4].hookFunction = (void*)ToolTipB_Hook;
    hooks[4].optional = TRUE;

    hooks[5].symbols = kToolTipB2Names;
    hooks[5].symbolCount = ARRAYSIZE(kToolTipB2Names);
    hooks[5].pOriginal = (void**)&g_origToolTipB2;
    hooks[5].hookFunction = (void*)ToolTipB2_Hook;
    hooks[5].optional = TRUE;

    hooks[6].symbols = kToolTipOldNames;
    hooks[6].symbolCount = ARRAYSIZE(kToolTipOldNames);
    hooks[6].pOriginal = (void**)&g_origToolTipOld;
    hooks[6].hookFunction = (void*)ToolTipOld_Hook;
    hooks[6].optional = TRUE;

    hooks[7].symbols = kOnApplyTemplateNames;
    hooks[7].symbolCount = ARRAYSIZE(kOnApplyTemplateNames);
    hooks[7].pOriginal = (void**)&g_origOnApplyTemplate;
    hooks[7].hookFunction = (void*)OnApplyTemplate_Hook;
    hooks[7].optional = TRUE;

    hooks[8].symbols = kGetViewModelNames;
    hooks[8].symbolCount = ARRAYSIZE(kGetViewModelNames);
    hooks[8].pOriginal = (void**)&g_origGetViewModel;
    hooks[8].hookFunction = (void*)GetViewModel_Hook;
    hooks[8].optional = TRUE;

    if (!SP_HookSymbols(module, hooks, ARRAYSIZE(hooks)))
    {
        SP_LogError(L"The clock's refresh function was not found in %s", moduleName);
        g_clockHooked.store(false);
        return FALSE;
    }

    SP_Log(L"Clock hooks installed in %s (style hooks: %s)", moduleName,
           (g_origOnApplyTemplate && g_origGetViewModel) ? L"yes" : L"no");

    // The clock on screen still shows the shell's text; ask for a refresh so the settings show at once.
    NudgeClock();
    return TRUE;
}

void OnSystemTrayLoaded(HMODULE module, void*)
{
    InstallClockHooks(module, L"SystemTray.dll");
}

void OnTaskbarViewLoaded(HMODULE taskbarView, void*)
{
    if (HMODULE systemTray = GetModuleHandleW(L"SystemTray.dll"))
    {
        InstallClockHooks(systemTray, L"SystemTray.dll");
        return;
    }

    // Builds before Taskbar.View.dll 2604 keep the tray types inside it.
    if (ModuleHasClock(taskbarView))
    {
        InstallClockHooks(taskbarView, L"Taskbar.View.dll");
        return;
    }

    SP_Log(L"The clock is not in Taskbar.View.dll; waiting for SystemTray.dll");
    if (!SP_WaitForModule(L"SystemTray.dll", 60000, OnSystemTrayLoaded, nullptr))
    {
        SP_LogError(L"Could not wait for SystemTray.dll");
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------------------------

void LoadSettings()
{
    Settings s{};

    SP_GetStringSetting(L"TimeFormat", s.timeFormat, ARRAYSIZE(s.timeFormat), L"");
    SP_GetStringSetting(L"DateFormat", s.dateFormat, ARRAYSIZE(s.dateFormat), L"");
    SP_GetStringSetting(L"WeekdayFormat", s.weekdayFormat, ARRAYSIZE(s.weekdayFormat), L"dddd");

    wchar_t top[kLineMax];
    wchar_t bottom[kLineMax];
    wchar_t middle[kLineMax];
    SP_GetStringSetting(L"TopLine", top, ARRAYSIZE(top), L"");
    SP_GetStringSetting(L"BottomLine", bottom, ARRAYSIZE(bottom), L"");
    SP_GetStringSetting(L"MiddleLine", middle, ARRAYSIZE(middle), L"");

    // "-" keeps the shell's own text for that line. Empty means the line's natural content, which is what the
    // shell shows too, so a fresh install changes nothing until a format is typed.
    s.topCustom = wcscmp(top, L"-") != 0;
    wcsncpy_s(s.topLine, top[0] ? top : L"%time%", _TRUNCATE);

    s.bottomCustom = wcscmp(bottom, L"-") != 0;
    std::wstring effectiveBottom = bottom[0] ? bottom : L"%date%";
    if (middle[0] && wcscmp(middle, L"-") != 0)
    {
        // The XAML clock has only two text blocks; the middle line becomes the first line of the lower one.
        effectiveBottom = std::wstring(middle) + L"%n%" + effectiveBottom;
    }
    wcsncpy_s(s.bottomLine, effectiveBottom.c_str(), _TRUNCATE);

    s.fontSize = SP_GetIntSetting(L"FontSize", 0);
    if (s.fontSize < 0 || s.fontSize > 200)
    {
        s.fontSize = 0;
    }

    wchar_t color[32];
    SP_GetStringSetting(L"TextColor", color, ARRAYSIZE(color), L"");
    s.hasTextColor = ParseColor(color, s.textColor);
    if (color[0] && !s.hasTextColor)
    {
        SP_LogError(L"TextColor \"%s\" is not RRGGBB; ignored", color);
    }

    {
        std::lock_guard<std::mutex> guard(g_settingsLock);
        g_settings = s;
    }

    g_styleEnabled.store(s.fontSize > 0 || s.hasTextColor, std::memory_order_relaxed);
    g_styleIndex.fetch_add(1, std::memory_order_relaxed);

    SP_Log(L"Top \"%s\" bottom \"%s\" time \"%s\" date \"%s\" font %d colour %s",
           s.topCustom ? s.topLine : L"-", s.bottomCustom ? s.bottomLine : L"-",
           s.timeFormat, s.dateFormat, s.fontSize, s.hasTextColor ? color : L"default");
}

BOOL Init()
{
    LoadSettings();

    // The two exports are hooked by module so the kernelbase functions are patched, not the kernel32 stubs.
    if (!SP_HookBegin())
    {
        return FALSE;
    }
    if (!SP_SetExportHook(L"kernelbase.dll", "GetTimeFormatEx", GetTimeFormatEx_Hook, &g_origGetTimeFormatEx) ||
        !SP_SetExportHook(L"kernelbase.dll", "GetDateFormatEx", GetDateFormatEx_Hook, &g_origGetDateFormatEx))
    {
        SP_HookAbort();
        SP_LogError(L"The date and time formatting exports could not be hooked");
        return FALSE;
    }
    if (!SP_HookCommit())
    {
        return FALSE;
    }

    // The taskbar comes up after the engine on a cold sign-in; the clock hooks go in when it is there.
    if (!SP_WaitForModule(L"Taskbar.View.dll", 60000, OnTaskbarViewLoaded, nullptr))
    {
        SP_LogError(L"Could not wait for Taskbar.View.dll");
        return FALSE;
    }

    return TRUE;
}

void AfterInit()
{
    // Already hooked on a warm start: make the clock pick the settings up now rather than at the next minute.
    if (g_clockHooked.load())
    {
        NudgeClock();
    }
}

void SettingsChanged()
{
    LoadSettings();
    NudgeClock();
}

void BeforeUninit()
{
    // Hooks are still live, so the restyle can go through the same path the styling did.
    g_styleEnabled.store(false, std::memory_order_relaxed);
    DWORD index = g_styleIndex.fetch_add(1, std::memory_order_relaxed) + 1;

    if (AllClocksAt(index))
    {
        return;
    }

    NudgeClock();
    for (int i = 0; i < 20 && !AllClocksAt(index); ++i)
    {
        Sleep(100);
    }
}

void Uninit()
{
    // The hooks are gone; one more refresh puts the shell's own text back.
    NudgeClock();

    std::lock_guard<std::mutex> guard(g_styleLock);
    g_styledClocks.clear();

    // The DLL stays resident: without this the next load's InstallClockHooks thinks the hooks are still in and
    // installs nothing (the clock then stays stock until the shell restarts).
    g_clockHooked.store(false);
}

}   // namespace

SP_MOD_DEFINE(g_modTaskbarClockCustomization) =
{
    /* id             */ SP_MOD_ID,
    /* name           */ L"Customize the taskbar clock text and style",
    /* basedOn        */ "taskbar-clock-customization",
    /* originalAuthor */ "m417z",
    /* targets        */ SP_TARGET_EXPLORER,
    /* minOsBuild     */ 22000,
    /* flags          */ 0,
    /* Init           */ Init,
    /* AfterInit      */ AfterInit,
    /* SettingsChanged*/ SettingsChanged,
    /* BeforeUninit   */ BeforeUninit,
    /* Uninit         */ Uninit,
};
