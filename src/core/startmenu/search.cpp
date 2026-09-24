//
// search.cpp - see search.h.
//
#include "search.h"

#include <Shlwapi.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cwctype>

#include "text.h"

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "dwmapi.lib")

namespace sm {

namespace {

struct SettingPage
{
    const wchar_t* uri;
    const wchar_t* english;
    const wchar_t* turkish;
    const wchar_t* keywords;   // both languages
};

const SettingPage kSettings[] =
{
#include "settings_table.inc"
};

int Find(const std::wstring& text, const std::wstring& query)
{
    return FindNLSStringEx(LOCALE_NAME_USER_DEFAULT, FIND_FROMSTART | LINGUISTIC_IGNORECASE | LINGUISTIC_IGNOREDIACRITIC,
                           text.c_str(), (int)text.size(), query.c_str(), (int)query.size(), nullptr, nullptr, nullptr,
                           0);
}

std::wstring Trim(const std::wstring& text)
{
    size_t start = 0, end = text.size();
    while (start < end && iswspace(text[start])) ++start;
    while (end > start && iswspace(text[end - 1])) --end;
    return text.substr(start, end - start);
}

bool StartsWithNoCase(const std::wstring& text, const wchar_t* prefix)
{
    size_t length = wcslen(prefix);
    return text.size() >= length && _wcsnicmp(text.c_str(), prefix, length) == 0;
}

std::wstring Expand(const std::wstring& text)
{
    std::wstring result = text;
    if (text.find(L'%') != std::wstring::npos)
    {
        DWORD needed = ExpandEnvironmentStringsW(text.c_str(), nullptr, 0);
        if (needed)
        {
            std::wstring buffer(needed, L'\0');
            ExpandEnvironmentStringsW(text.c_str(), buffer.data(), needed);
            buffer.resize(wcslen(buffer.c_str()));
            result = buffer;
        }
    }
    if (result == L"~" || StartsWithNoCase(result, L"~\\") || StartsWithNoCase(result, L"~/"))
    {
        wchar_t profile[MAX_PATH] = L"";
        if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH))
        {
            result = profile + result.substr(1);
        }
    }
    return result;
}

bool LooksLikePath(const std::wstring& text)
{
    return text.find(L'\\') != std::wstring::npos || text.find(L'/') != std::wstring::npos ||
           (text.size() >= 2 && text[1] == L':');
}

// A local path that exists. Network paths are not probed: a missing server would stall the menu.
bool LocalPathExists(const std::wstring& path)
{
    if (path.empty() || PathIsNetworkPathW(path.c_str()))
    {
        return false;
    }
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// A program name as the Run dialog understands it: on the PATH (with its own extension or .exe), or registered
// under App Paths ("chrome", "winword").
bool ResolveProgram(const std::wstring& name, std::wstring* path)
{
    if (name.empty() || name.size() > MAX_PATH || name.find_first_of(L"*?<>|\"") != std::wstring::npos)
    {
        return false;
    }
    if (LooksLikePath(name))
    {
        if (LocalPathExists(name))
        {
            *path = name;
            return true;
        }
        return false;
    }
    wchar_t found[MAX_PATH] = L"";
    if (SearchPathW(nullptr, name.c_str(), L".exe", MAX_PATH, found, nullptr))
    {
        *path = found;
        return true;
    }
    std::wstring key = L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" + name;
    if (!PathFindExtensionW(name.c_str())[0])
    {
        key += L".exe";
    }
    for (HKEY root : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE })
    {
        wchar_t value[MAX_PATH] = L"";
        DWORD bytes = sizeof(value);
        if (RegGetValueW(root, key.c_str(), nullptr, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, value, &bytes) ==
                ERROR_SUCCESS &&
            value[0])
        {
            std::wstring resolved = Expand(value);
            if (resolved.size() > 1 && resolved.front() == L'"')
            {
                resolved = resolved.substr(1, resolved.find(L'"', 1) - 1);
            }
            *path = resolved;
            return true;
        }
    }
    return false;
}

// --- calculator ------------------------------------------------------------------------------------------------

class Parser
{
public:
    explicit Parser(const std::wstring& text) : m_text(text) {}

    bool Parse(double* value)
    {
        double result = 0;
        if (!Expression(&result))
        {
            return false;
        }
        Skip();
        if (m_pos != m_text.size() || !std::isfinite(result))
        {
            return false;
        }
        *value = result;
        return true;
    }

    bool sawOperator = false;

private:
    void Skip()
    {
        while (m_pos < m_text.size() && iswspace(m_text[m_pos])) ++m_pos;
    }

    bool Eat(wchar_t c)
    {
        Skip();
        if (m_pos < m_text.size() && m_text[m_pos] == c)
        {
            ++m_pos;
            return true;
        }
        return false;
    }

    bool Expression(double* out)
    {
        if (!Term(out))
        {
            return false;
        }
        for (;;)
        {
            if (Eat(L'+') || Eat(L'-'))
            {
                bool minus = m_text[m_pos - 1] == L'-';
                double right = 0;
                if (!Term(&right))
                {
                    return false;
                }
                *out = minus ? *out - right : *out + right;
                sawOperator = true;
            }
            else
            {
                return true;
            }
        }
    }

    bool Term(double* out)
    {
        if (!Power(out))
        {
            return false;
        }
        for (;;)
        {
            wchar_t op = 0;
            if (Eat(L'*')) op = L'*';
            else if (Eat(L'/')) op = L'/';
            else if (Eat(L'%')) op = L'%';
            else return true;
            double right = 0;
            if (!Power(&right))
            {
                return false;
            }
            if ((op == L'/' || op == L'%') && right == 0)
            {
                return false;
            }
            *out = op == L'*' ? *out * right : op == L'/' ? *out / right : fmod(*out, right);
            sawOperator = true;
        }
    }

    bool Power(double* out)
    {
        if (!Unary(out))
        {
            return false;
        }
        if (Eat(L'^'))
        {
            double exponent = 0;
            if (!Power(&exponent))   // right-associative
            {
                return false;
            }
            *out = pow(*out, exponent);
            sawOperator = true;
        }
        return true;
    }

    bool Unary(double* out)
    {
        if (Eat(L'-'))
        {
            if (!Unary(out))
            {
                return false;
            }
            *out = -*out;
            return true;
        }
        if (Eat(L'+'))
        {
            return Unary(out);
        }
        if (Eat(L'('))
        {
            if (!Expression(out) || !Eat(L')'))
            {
                return false;
            }
            return true;
        }
        Skip();
        size_t start = m_pos;
        while (m_pos < m_text.size() && (iswdigit(m_text[m_pos]) || m_text[m_pos] == L'.'))
        {
            ++m_pos;
        }
        if (m_pos == start)
        {
            return false;
        }
        std::wstring number = m_text.substr(start, m_pos - start);
        if (std::count(number.begin(), number.end(), L'.') > 1)
        {
            return false;
        }
        *out = _wtof(number.c_str());
        return true;
    }

    const std::wstring& m_text;
    size_t m_pos = 0;
};

} // namespace

// ---------------------------------------------------------------------------------------------------------------

std::vector<Result> SearchSettings(const std::wstring& rawQuery, size_t limit)
{
    std::vector<Result> result;
    std::wstring query = Trim(rawQuery);
    if (query.size() < 2)
    {
        return result;
    }
    bool turkish = IsTurkish();
    struct Match { int rank; size_t index; };
    std::vector<Match> matches;
    for (size_t i = 0; i < ARRAYSIZE(kSettings); ++i)
    {
        const SettingPage& page = kSettings[i];
        std::wstring name = turkish ? page.turkish : page.english;
        std::wstring other = turkish ? page.english : page.turkish;
        int at = Find(name, query);
        int rank = at == 0 ? 0 : at > 0 ? 1 : Find(page.keywords, query) >= 0 ? 2 : Find(other, query) >= 0 ? 3 : -1;
        if (rank >= 0)
        {
            matches.push_back({ rank, i });
        }
    }
    std::stable_sort(matches.begin(), matches.end(), [](const Match& a, const Match& b) { return a.rank < b.rank; });
    for (const Match& match : matches)
    {
        if (result.size() >= limit)
        {
            break;
        }
        const SettingPage& page = kSettings[match.index];
        Result r;
        r.kind = ResultKind::Setting;
        r.title = turkish ? page.turkish : page.english;
        r.subtitle = Text(Str::HintSettings);
        r.target = page.uri;
        result.push_back(std::move(r));
    }
    return result;
}

bool Calculate(const std::wstring& rawQuery, double* value)
{
    std::wstring query = Trim(rawQuery);
    if (!query.empty() && query[0] == L'=')
    {
        query = Trim(query.substr(1));
    }
    if (query.empty() || query.size() > 200)
    {
        return false;
    }
    // Turkish writes decimals with a comma; x, \x00D7 and \x00F7 are common ways to type * and /.
    for (wchar_t& c : query)
    {
        if (c == L',') c = L'.';
        else if (c == L'x' || c == L'X' || c == 0x00D7) c = L'*';
        else if (c == 0x00F7 || c == L':') c = L'/';
        else if (!(iswdigit(c) || c == L'.' || c == L'+' || c == L'-' || c == L'*' || c == L'/' || c == L'^' ||
                   c == L'%' || c == L'(' || c == L')' || iswspace(c)))
        {
            return false;
        }
    }
    Parser parser(query);
    return parser.Parse(value) && parser.sawOperator;
}

Result CalcResult(double value)
{
    wchar_t text[64];
    if (fabs(value) < 1e15 && fabs(value - llround(value)) < 1e-9)
    {
        swprintf_s(text, L"%lld", llround(value));
    }
    else
    {
        swprintf_s(text, L"%.10g", value);
    }
    std::wstring formatted = text;
    wchar_t decimal[8] = L".";
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SDECIMAL, decimal, ARRAYSIZE(decimal));
    size_t dot = formatted.find(L'.');
    if (dot != std::wstring::npos)
    {
        formatted.replace(dot, 1, decimal);
    }
    Result r;
    r.kind = ResultKind::Calc;
    r.title = L"= " + formatted;
    r.subtitle = Text(Str::HintCopy);
    r.target = formatted;
    r.strong = true;
    return r;
}

bool ResolveCommand(const std::wstring& rawQuery, Result* result)
{
    std::wstring query = Trim(rawQuery);
    if (query.empty())
    {
        return false;
    }

    // Locations the shell opens as they are.
    static const wchar_t* const kSchemes[] = { L"http://", L"https://", L"www.", L"ms-settings:", L"shell:",
                                               L"mailto:", L"file:" };
    for (const wchar_t* scheme : kSchemes)
    {
        if (StartsWithNoCase(query, scheme) && query.size() > wcslen(scheme))
        {
            result->kind = ResultKind::Open;
            result->target = StartsWithNoCase(query, L"www.") ? L"https://" + query : query;
            result->title = query;
            result->subtitle = Text(Str::HintOpen);
            result->strong = true;
            return true;
        }
    }

    // A path, with %VARIABLES% and ~ expanded: %appdata%, %temp%\foo, C:\Windows, ~\Downloads.
    std::wstring expanded = Expand(query);
    bool variable = query.find(L'%') != std::wstring::npos || query[0] == L'~';
    if ((variable || LooksLikePath(expanded)) && LocalPathExists(expanded))
    {
        result->kind = ResultKind::Open;
        result->target = expanded;
        result->title = query;
        result->subtitle = expanded != query ? expanded : Text(Str::HintOpen);
        result->strong = true;
        return true;
    }
    if (PathIsNetworkPathW(expanded.c_str()) && StartsWithNoCase(expanded, L"\\\\") && expanded.size() > 2)
    {
        result->kind = ResultKind::Open;
        result->target = expanded;
        result->title = query;
        result->subtitle = Text(Str::HintOpen);
        result->strong = true;
        return true;
    }

    // A program with arguments: "cmd", "notepad C:\a.txt", "\"C:\Program Files\x\x.exe\" -y", "devmgmt.msc".
    std::wstring program, arguments;
    if (query[0] == L'"')
    {
        size_t close = query.find(L'"', 1);
        if (close == std::wstring::npos)
        {
            return false;
        }
        program = query.substr(1, close - 1);
        arguments = Trim(query.substr(close + 1));
    }
    else
    {
        size_t space = query.find(L' ');
        program = query.substr(0, space);
        arguments = space == std::wstring::npos ? L"" : Trim(query.substr(space + 1));
    }
    program = Expand(program);
    std::wstring path;
    if (!ResolveProgram(program, &path))
    {
        return false;
    }
    result->kind = ResultKind::Run;
    result->target = path;
    result->arguments = Expand(arguments);
    result->title = query;
    result->subtitle = path;
    result->strong = !arguments.empty() || variable || LooksLikePath(program) || PathFindExtensionW(program.c_str())[0];
    return true;
}

namespace {

// The windows Alt+Tab would list: visible, unowned, not tool windows, not cloaked (other desktops, suspended
// apps), with a title, and not part of the shell.
bool IsSwitchable(HWND hwnd)
{
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) || GetWindowTextLengthW(hwnd) == 0)
    {
        return false;
    }
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW))
    {
        return false;
    }
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
    {
        return false;
    }
    wchar_t name[64] = L"";
    GetClassNameW(hwnd, name, ARRAYSIZE(name));
    return wcscmp(name, L"Progman") != 0 && wcscmp(name, L"Shell_TrayWnd") != 0 &&
           wcscmp(name, L"Shell_SecondaryTrayWnd") != 0 && wcsncmp(name, L"ShadePatcher.StartMenu", 22) != 0;
}

} // namespace

std::vector<Result> SearchWindows(const std::wstring& rawQuery, size_t limit)
{
    struct Context
    {
        std::wstring query;
        size_t limit;
        std::vector<Result> results;
    } context{ Trim(rawQuery), limit, {} };
    if (context.query.size() < 2)
    {
        return {};
    }
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            Context* c = reinterpret_cast<Context*>(param);
            if (c->results.size() >= c->limit || !IsSwitchable(hwnd))
            {
                return c->results.size() < c->limit;
            }
            wchar_t title[512] = L"";
            GetWindowTextW(hwnd, title, ARRAYSIZE(title));
            if (Find(title, c->query) < 0)
            {
                return TRUE;
            }
            Result r;
            r.kind = ResultKind::Window;
            r.title = title;
            r.subtitle = Text(Str::HintSwitch);
            r.window = hwnd;
            wchar_t key[32];
            swprintf_s(key, L"window:%p", hwnd);
            r.target = key;
            c->results.push_back(std::move(r));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&context));
    return context.results;
}

bool ExecuteResult(const Result& result, bool asAdmin)
{
    AllowSetForegroundWindow(ASFW_ANY);
    if (result.kind == ResultKind::Window)
    {
        if (!IsWindow(result.window))
        {
            return false;
        }
        if (IsIconic(result.window))
        {
            ShowWindowAsync(result.window, SW_RESTORE);
        }
        SwitchToThisWindow(result.window, TRUE);
        return SetForegroundWindow(result.window) != FALSE;
    }
    SHELLEXECUTEINFOW info = { sizeof(info) };
    info.fMask = SEE_MASK_ASYNCOK;
    info.nShow = SW_SHOWNORMAL;
    info.lpFile = result.target.c_str();
    wchar_t profile[MAX_PATH] = L"";
    if (result.kind == ResultKind::Run)
    {
        info.lpParameters = result.arguments.empty() ? nullptr : result.arguments.c_str();
        info.lpVerb = asAdmin ? L"runas" : nullptr;
        // The Run dialog starts programs in the user's profile folder.
        if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH))
        {
            info.lpDirectory = profile;
        }
    }
    return ShellExecuteExW(&info) != FALSE;
}

} // namespace sm
