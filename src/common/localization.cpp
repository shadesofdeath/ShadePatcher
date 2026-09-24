#include "localization.h"
#include "config.h"

#include <algorithm>
#include <vector>

extern "C"
{

static L10N_Language LangIDToLanguage(LANGID wLanguage)
{
    L10N_Language language = {};
    language.id = wLanguage;
    GetLocaleInfoW(wLanguage, LOCALE_SNAME, language.wszId, ARRAYSIZE(language.wszId));
    GetLocaleInfoW(wLanguage, LOCALE_SLOCALIZEDDISPLAYNAME, language.wszDisplayName, ARRAYSIZE(language.wszDisplayName));
    return language;
}

BOOL L10N_ApplyPreferredLanguageForCurrentThread(void)
{
    DWORD dwPreferredLanguage = 0;
    DWORD dwSize = sizeof(dwPreferredLanguage);
    LSTATUS lres = RegGetValueW(
        HKEY_CURRENT_USER,
        TEXT(REGPATH),
        L"Language",
        RRF_RT_REG_DWORD,
        nullptr,
        &dwPreferredLanguage,
        &dwSize
    );
    if (lres == ERROR_SUCCESS && dwPreferredLanguage != 0)
    {
        L10N_Language language = LangIDToLanguage((LANGID)dwPreferredLanguage);
        return SetThreadPreferredUILanguages(MUI_LANGUAGE_NAME, language.wszId, nullptr);
    }
    return SetThreadPreferredUILanguages(MUI_LANGUAGE_NAME, nullptr, nullptr);
}

static BOOL GetPreferredUILanguage(BOOL bThread, wchar_t* wszLanguage, int cch)
{
    BOOL bOk = FALSE;
    ULONG ulNumLanguages = 0;
    ULONG cchLanguagesBuffer = 0;
    BOOL bSized = bThread
        ? GetThreadPreferredUILanguages(MUI_LANGUAGE_NAME, &ulNumLanguages, nullptr, &cchLanguagesBuffer)
        : GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &ulNumLanguages, nullptr, &cchLanguagesBuffer);
    if (bSized)
    {
        std::vector<wchar_t> buffer(cchLanguagesBuffer);
        BOOL bGot = bThread
            ? GetThreadPreferredUILanguages(MUI_LANGUAGE_NAME, &ulNumLanguages, buffer.data(), &cchLanguagesBuffer)
            : GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &ulNumLanguages, buffer.data(), &cchLanguagesBuffer);
        if (bGot)
        {
            // The buffer holds a double-NUL terminated list; the first entry is the preferred one.
            wcscpy_s(wszLanguage, cch, buffer.data());
            bOk = TRUE;
        }
    }
    if (!bOk)
    {
        wcscpy_s(wszLanguage, cch, L"en-US");
    }
    return TRUE;
}

BOOL L10N_GetCurrentUserLanguage(wchar_t* wszLanguage, int cch)
{
    return GetPreferredUILanguage(FALSE, wszLanguage, cch);
}

BOOL L10N_GetCurrentThreadLanguage(wchar_t* wszLanguage, int cch)
{
    return GetPreferredUILanguage(TRUE, wszLanguage, cch);
}

void L10N_EnumerateLanguages(HMODULE hModule, LPCWSTR lpType, LPCWSTR lpName, L10N_EnumerateLanguagesProc_t pfnProc, void* data)
{
    std::vector<L10N_Language> languages;

    // English (US) is the primary language and always comes first.
    languages.push_back(LangIDToLanguage(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US)));

    EnumResourceLanguagesW(hModule, lpType, lpName, [](HMODULE, LPCWSTR, LPCWSTR, WORD wLanguage, LONG_PTR lParam) -> BOOL
    {
        if (wLanguage != MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US))
        {
            ((std::vector<L10N_Language>*)lParam)->push_back(LangIDToLanguage(wLanguage));
        }
        return TRUE;
    }, (LONG_PTR)&languages);

    std::sort(languages.begin() + 1, languages.end(), [](const L10N_Language& a, const L10N_Language& b)
    {
        return wcscmp(a.wszDisplayName, b.wszDisplayName) < 0;
    });

    for (const L10N_Language& language : languages)
    {
        pfnProc(&language, data);
    }
}

}
