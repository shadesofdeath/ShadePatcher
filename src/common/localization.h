#pragma once
//
// localization.h - UI language selection.
//
// The GUI strings are ordinary Win32 string tables compiled once per language (see gui/resources/lang). The user
// may pin a language in the registry ("Language" value under REGPATH, a LANGID); otherwise the system language is
// used. These helpers apply that choice to the calling thread and enumerate the languages a module contains.
//
#include <Windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct L10N_Language
{
    LANGID id;
    wchar_t wszId[LOCALE_NAME_MAX_LENGTH];           // e.g. "tr-TR"
    wchar_t wszDisplayName[LOCALE_NAME_MAX_LENGTH];  // e.g. "Türkçe (Türkiye)"
} L10N_Language;

typedef void (*L10N_EnumerateLanguagesProc_t)(const L10N_Language* language, void* data);

// Reads the "Language" registry value and applies it to the current thread (or resets to the system default).
BOOL L10N_ApplyPreferredLanguageForCurrentThread(void);

BOOL L10N_GetCurrentUserLanguage(wchar_t* wszLanguage, int cch);
BOOL L10N_GetCurrentThreadLanguage(wchar_t* wszLanguage, int cch);

// Calls pfnProc once per language that has the given resource: English (US) first, the rest sorted by display name.
void L10N_EnumerateLanguages(HMODULE hModule, LPCWSTR lpType, LPCWSTR lpName, L10N_EnumerateLanguagesProc_t pfnProc, void* data);

#ifdef __cplusplus
}
#endif
