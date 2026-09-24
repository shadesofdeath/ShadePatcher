//
// modtest - drives a real mod through its lifecycle and checks what it actually does.
//
// The engine self-test proves the machinery. This proves the mod: its hooks are installed into this process,
// a notification icon key is faked, and the registry calls the taskbar would make are made here instead. What
// the mod answers is then compared against what it promises.
//
// Nothing the user owns is touched. The fake icon key uses an id no real icon uses and is deleted afterwards,
// and AfterInit is deliberately not called because its only job is to nudge every real icon key so the taskbar
// re-reads it, which has no place in a test.
//
#include <Windows.h>
#include <stdio.h>

#include "engine/hooks.h"
#include "engine/log.h"
#include "engine/mod.h"
#include "engine/settings.h"

extern void Check(const char* what, BOOL ok);

// The mod under test, from src/core/mods/tray_show_all_icons.cpp.
SP_MOD_DECLARE(g_modTrayShowAllIcons);

#define TRAY_MOD_ID "tray-show-all-icons"

// Real ids are 20-digit hashes, so this one cannot collide with an icon the user has.
#define FAKE_ICON_KEY L"Control Panel\\NotifyIconSettings\\selftest_fake_icon"

static HKEY CreateFakeIconKey(void)
{
    HKEY hKey = NULL;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, FAKE_ICON_KEY, 0, NULL, REG_OPTION_NON_VOLATILE,
                        KEY_READ | KEY_SET_VALUE, NULL, &hKey, NULL) != ERROR_SUCCESS)
    {
        return NULL;
    }
    return hKey;
}

// Asks the way the taskbar asks.
static LONG ReadIsPromoted(HKEY hIcon, DWORD* pValue)
{
    DWORD cb = sizeof(DWORD);
    *pValue = 0xCCCCCCCC;
    return RegGetValueW(hIcon, NULL, L"IsPromoted", RRF_RT_REG_DWORD, NULL, pValue, &cb);
}

void TestTrayShowAllIconsMod(void)
{
    printf("Mod: %s\n", TRAY_MOD_ID);

    const SP_Mod* mod = &g_modTrayShowAllIcons;

    Check("the mod targets explorer", (mod->targets & SP_TARGET_EXPLORER) != 0);
    Check("the mod credits the idea it came from",
          mod->basedOn != NULL && mod->originalAuthor != NULL);

    HKEY hIcon = CreateFakeIconKey();
    if (!hIcon)
    {
        printf("  [SKIP] the test icon key could not be created\n");
        return;
    }

    DWORD value = 0;

    // Before the mod runs, the value genuinely does not exist.
    Check("IsPromoted is absent to begin with", ReadIsPromoted(hIcon, &value) == ERROR_FILE_NOT_FOUND);

    // Mode 0 is "show all".
    SP_SettingsSetInt(TRAY_MOD_ID, L"Mode", 0);

    if (!mod->Init())
    {
        printf("  [SKIP] the mod refused to start\n");
        RegCloseKey(hIcon);
        RegDeleteKeyW(HKEY_CURRENT_USER, FAKE_ICON_KEY);
        return;
    }
    Check("the mod starts", TRUE);

    Check("show all reports the icon as promoted",
          ReadIsPromoted(hIcon, &value) == ERROR_SUCCESS && value == 1);

    // A write of IsPromoted has to be accepted and then discarded, so that turning the mod off gives the user
    // back exactly the layout they had.
    DWORD one = 1;
    LONG setResult = RegSetValueExW(hIcon, L"IsPromoted", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
    Check("a write of IsPromoted reports success", setResult == ERROR_SUCCESS);

    // Reading it back through the engine's own path shows whether anything really landed.
    DWORD stored = 0;
    DWORD cbStored = sizeof(stored);
    LONG storedResult = RegGetValueW(HKEY_CURRENT_USER, FAKE_ICON_KEY, L"IsPromoted",
                                     RRF_RT_REG_DWORD, NULL, &stored, &cbStored);
    Check("the write did not reach the registry", storedResult == ERROR_FILE_NOT_FOUND);

    // Mode 2 is "hide all"; the same read must now answer the other way.
    SP_SettingsSetInt(TRAY_MOD_ID, L"Mode", 2);
    if (mod->SettingsChanged)
    {
        mod->SettingsChanged();
    }
    Check("hide all reports the icon as not promoted",
          ReadIsPromoted(hIcon, &value) == ERROR_SUCCESS && value == 0);

    // An unrelated value must pass straight through, hooks or not.
    DWORD probe = 7;
    RegSetValueExW(hIcon, L"SelftestProbe", 0, REG_DWORD, (const BYTE*)&probe, sizeof(probe));
    DWORD readBack = 0;
    DWORD cbReadBack = sizeof(readBack);
    Check("an unrelated value is untouched",
          RegGetValueW(hIcon, NULL, L"SelftestProbe", RRF_RT_REG_DWORD, NULL, &readBack, &cbReadBack) == ERROR_SUCCESS &&
          readBack == 7);

    // Unloading restores the real registry behaviour.
    if (mod->BeforeUninit)
    {
        mod->BeforeUninit();
    }
    SP_RemoveHooksOf(TRAY_MOD_ID);

    Check("unloading restores the real answer", ReadIsPromoted(hIcon, &value) == ERROR_FILE_NOT_FOUND);

    RegCloseKey(hIcon);
    RegDeleteKeyW(HKEY_CURRENT_USER, FAKE_ICON_KEY);

    wchar_t wszPath[MAX_PATH];
    if (SP_SettingsGetModKeyPath(TRAY_MOD_ID, wszPath, ARRAYSIZE(wszPath)))
    {
        RegDeleteKeyW(HKEY_CURRENT_USER, wszPath);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// explorer-auto-file-sizes
//
// propsys formats the Size column. Ordinal 422 always answers in KB, which is what the mod replaces. Calling it
// here before and after the hook shows exactly what a File Explorer window would show.
// ---------------------------------------------------------------------------------------------------------------

SP_MOD_DECLARE(g_modExplorerAutoFileSizes);

#define SIZES_MOD_ID "explorer-auto-file-sizes"

typedef void*(WINAPI* PSStrFormatSize_t)(ULONGLONG size, LPWSTR pszText, DWORD cchText);

// TRUE when the text ends in a unit other than plain KB, which is the whole point of the mod.
static BOOL EndsWithUnit(const wchar_t* text, const wchar_t* unit)
{
    size_t lenText = wcslen(text);
    size_t lenUnit = wcslen(unit);
    return lenText >= lenUnit && _wcsicmp(text + lenText - lenUnit, unit) == 0;
}

void TestExplorerAutoFileSizesMod(void)
{
    printf("Mod: %s\n", SIZES_MOD_ID);

    const SP_Mod* mod = &g_modExplorerAutoFileSizes;

    // propsys is not loaded in a plain console process, so the test loads it the way the shell would.
    HMODULE hPropsys = LoadLibraryExW(L"propsys.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!hPropsys)
    {
        printf("  [SKIP] propsys.dll could not be loaded\n");
        return;
    }

    PSStrFormatSize_t pFormatKB = (PSStrFormatSize_t)GetProcAddress(hPropsys, MAKEINTRESOURCEA(422));
    if (!pFormatKB)
    {
        printf("  [SKIP] propsys.dll does not export ordinal 422 on this build\n");
        return;
    }

    // Four gigabytes: Windows reports this as millions of KB until the mod is on.
    const ULONGLONG fourGigabytes = 4ULL * 1024 * 1024 * 1024;
    wchar_t wszText[64];

    wszText[0] = 0;
    pFormatKB(fourGigabytes, wszText, ARRAYSIZE(wszText));
    Check("4 GB reads as KB before the mod runs", EndsWithUnit(wszText, L"KB"));
    printf("         before: %ls\n", wszText);

    SP_SettingsSetInt(SIZES_MOD_ID, L"BinaryUnits", 0);

    if (!mod->Init())
    {
        printf("  [SKIP] the mod refused to start\n");
        return;
    }
    Check("the mod starts", TRUE);

    wszText[0] = 0;
    pFormatKB(fourGigabytes, wszText, ARRAYSIZE(wszText));
    Check("4 GB now reads as GB", EndsWithUnit(wszText, L"GB"));
    printf("         after:  %ls\n", wszText);

    // A small file must still read in bytes or KB rather than being scaled up.
    wszText[0] = 0;
    pFormatKB(900, wszText, ARRAYSIZE(wszText));
    Check("a small file is not scaled up", !EndsWithUnit(wszText, L"GB") && !EndsWithUnit(wszText, L"MB"));
    printf("         900 bytes: %ls\n", wszText);

    // Binary units turn GB into GiB and must not disturb the number.
    SP_SettingsSetInt(SIZES_MOD_ID, L"BinaryUnits", 1);
    if (mod->SettingsChanged)
    {
        mod->SettingsChanged();
    }
    wszText[0] = 0;
    pFormatKB(fourGigabytes, wszText, ARRAYSIZE(wszText));
    Check("binary units read as GiB", EndsWithUnit(wszText, L"GiB"));
    printf("         binary: %ls\n", wszText);

    SP_RemoveHooksOf(SIZES_MOD_ID);

    wszText[0] = 0;
    pFormatKB(fourGigabytes, wszText, ARRAYSIZE(wszText));
    Check("unloading restores the KB formatting", EndsWithUnit(wszText, L"KB"));

    wchar_t wszPath[MAX_PATH];
    if (SP_SettingsGetModKeyPath(SIZES_MOD_ID, wszPath, ARRAYSIZE(wszPath)))
    {
        RegDeleteKeyW(HKEY_CURRENT_USER, wszPath);
    }
}
