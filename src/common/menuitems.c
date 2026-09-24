#include "menuitems.h"
#include "config.h"

#include <tchar.h>
#include <shellapi.h>
#include <strsafe.h>

#pragma comment(lib, "Shell32.lib")

// Each menu keeps its own list, under the key of the mod that draws it. The caller names the mod, so adding
// another menu later needs no change here.
static const wchar_t* ListKey(const wchar_t* modId, wchar_t* buffer, size_t cch)
{
    if (!modId || FAILED(StringCchPrintfW(buffer, cch, _T(REGPATH) L"\\Mods\\%s\\Items", modId)))
    {
        return NULL;
    }
    return buffer;
}

static void ReadString(HKEY hKey, const wchar_t* name, wchar_t* buffer, DWORD cchBuffer)
{
    buffer[0] = 0;
    DWORD cb = cchBuffer * sizeof(wchar_t);
    if (RegGetValueW(hKey, NULL, name, RRF_RT_REG_SZ, NULL, buffer, &cb) != ERROR_SUCCESS)
    {
        buffer[0] = 0;
    }
}

static void WriteString(HKEY hKey, const wchar_t* name, const wchar_t* value)
{
    RegSetValueExW(hKey, name, 0, REG_SZ, (const BYTE*)value,
                   (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
}

int SP_MenuItemsLoadFor(const wchar_t* modId, SP_MenuItem* items, int maxItems)
{
    wchar_t wszKey[MAX_PATH];
    const wchar_t* SP_MENU_KEY = ListKey(modId, wszKey, ARRAYSIZE(wszKey));
    if (!SP_MENU_KEY)
    {
        return 0;
    }

    if (!items || maxItems <= 0)
    {
        return 0;
    }

    DWORD count = 0;
    DWORD cb = sizeof(count);
    if (RegGetValueW(HKEY_CURRENT_USER, SP_MENU_KEY, L"Count", RRF_RT_REG_DWORD, NULL, &count, &cb) != ERROR_SUCCESS)
    {
        return 0;
    }
    if (count > (DWORD)maxItems)
    {
        count = (DWORD)maxItems;
    }

    int loaded = 0;
    for (DWORD i = 0; i < count; ++i)
    {
        wchar_t wszPath[MAX_PATH];
        if (FAILED(StringCchPrintfW(wszPath, MAX_PATH, L"%s\\%lu", SP_MENU_KEY, i)))
        {
            break;
        }

        HKEY hKey = NULL;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, wszPath, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        {
            continue;
        }

        SP_MenuItem* item = &items[loaded];
        ZeroMemory(item, sizeof(*item));
        ReadString(hKey, L"Name", item->name, SP_MENU_NAME_LENGTH);
        ReadString(hKey, L"Command", item->command, SP_MENU_COMMAND_LENGTH);
        ReadString(hKey, L"Arguments", item->arguments, SP_MENU_ARGS_LENGTH);
        ReadString(hKey, L"Icon", item->icon, SP_MENU_ICON_LENGTH);
        RegCloseKey(hKey);

        // A wholly empty entry is a leftover and is dropped. One with a name but no command yet is kept: it is
        // the blank entry the user has just added and is filling in. The menus themselves skip entries without
        // a command, so an unfinished one never shows up as a row that does nothing.
        if (item->name[0] || item->command[0])
        {
            loaded++;
        }
    }

    return loaded;
}

BOOL SP_MenuItemsSaveFor(const wchar_t* modId, const SP_MenuItem* items, int count)
{
    wchar_t wszKey[MAX_PATH];
    const wchar_t* SP_MENU_KEY = ListKey(modId, wszKey, ARRAYSIZE(wszKey));
    if (!SP_MENU_KEY)
    {
        return FALSE;
    }

    if (count < 0 || count > SP_MENU_MAX_ITEMS)
    {
        return FALSE;
    }

    // RegDeleteTree below needs to enumerate and delete under this key, not only read and write values; with
    // less than this the removed entries' keys were left behind.
    HKEY hRoot = NULL;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SP_MENU_KEY, 0, NULL, REG_OPTION_NON_VOLATILE,
                        KEY_READ | KEY_WRITE | DELETE, NULL, &hRoot, NULL) != ERROR_SUCCESS)
    {
        return FALSE;
    }

    // How many were there before, so the ones now removed can be deleted rather than left behind.
    DWORD previous = 0;
    DWORD cb = sizeof(previous);
    RegQueryValueExW(hRoot, L"Count", NULL, NULL, (LPBYTE)&previous, &cb);

    for (int i = 0; i < count; ++i)
    {
        wchar_t wszName[16];
        StringCchPrintfW(wszName, ARRAYSIZE(wszName), L"%d", i);

        HKEY hItem = NULL;
        if (RegCreateKeyExW(hRoot, wszName, 0, NULL, REG_OPTION_NON_VOLATILE,
                            KEY_SET_VALUE, NULL, &hItem, NULL) != ERROR_SUCCESS)
        {
            continue;
        }
        WriteString(hItem, L"Name", items[i].name);
        WriteString(hItem, L"Command", items[i].command);
        WriteString(hItem, L"Arguments", items[i].arguments);
        WriteString(hItem, L"Icon", items[i].icon);
        RegCloseKey(hItem);
    }

    for (DWORD i = (DWORD)count; i < previous && i < SP_MENU_MAX_ITEMS; ++i)
    {
        wchar_t wszName[16];
        StringCchPrintfW(wszName, ARRAYSIZE(wszName), L"%lu", i);
        RegDeleteTreeW(hRoot, wszName);
    }

    DWORD newCount = (DWORD)count;
    RegSetValueExW(hRoot, L"Count", 0, REG_DWORD, (const BYTE*)&newCount, sizeof(newCount));

    RegCloseKey(hRoot);
    return TRUE;
}

BOOL SP_MenuItemExecute(const SP_MenuItem* item)
{
    if (!item || !item->command[0])
    {
        return FALSE;
    }

    SHELLEXECUTEINFOW sei;
    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_ASYNCOK;
    sei.lpVerb = NULL;      // the file's default action, so a folder opens and a program runs
    sei.lpFile = item->command;
    sei.lpParameters = item->arguments[0] ? item->arguments : NULL;
    sei.nShow = SW_SHOWNORMAL;

    return ShellExecuteExW(&sei);
}

wchar_t SP_MenuItemIconGlyph(const wchar_t* icon)
{
    if (!icon || !icon[0])
    {
        return 0;
    }

    // The character itself. Segoe Fluent Icons lives in the private use area, so a lone character there can
    // only be a glyph.
    if (icon[1] == 0)
    {
        return (icon[0] >= 0xE000 && icon[0] <= 0xF8FF) ? icon[0] : 0;
    }

    // A code. Whatever prefix the user copied it with is skipped, then exactly four hex digits are expected.
    const wchar_t* p = icon;
    while (*p == L' ')
    {
        p++;
    }
    if (p[0] == L'&' && p[1] == L'#' && (p[2] == L'x' || p[2] == L'X'))
    {
        p += 3;
    }
    else if ((p[0] == L'0' && (p[1] == L'x' || p[1] == L'X')) ||
             (p[0] == L'\\' && (p[1] == L'u' || p[1] == L'U')) ||
             ((p[0] == L'U' || p[0] == L'u') && p[1] == L'+'))
    {
        p += 2;
    }

    wchar_t code = 0;
    int digits = 0;
    for (; *p && digits < 4; ++p, ++digits)
    {
        wchar_t c = *p;
        int v;
        if (c >= L'0' && c <= L'9')      v = c - L'0';
        else if (c >= L'a' && c <= L'f') v = c - L'a' + 10;
        else if (c >= L'A' && c <= L'F') v = c - L'A' + 10;
        else return 0;
        code = (wchar_t)((code << 4) | v);
    }
    if (digits != 4)
    {
        return 0;
    }

    // Trailing junk (a ';' from an HTML entity, spaces) is tolerated; anything else means it was not a code.
    while (*p == L';' || *p == L' ')
    {
        p++;
    }
    if (*p)
    {
        return 0;
    }

    return (code >= 0xE000 && code <= 0xF8FF) ? code : 0;
}

// The taskbar list is the one most callers mean, so it keeps the short names.
int SP_MenuItemsLoad(SP_MenuItem* items, int maxItems)
{
    return SP_MenuItemsLoadFor(L"taskbar-menu-entry", items, maxItems);
}

BOOL SP_MenuItemsSave(const SP_MenuItem* items, int count)
{
    return SP_MenuItemsSaveFor(L"taskbar-menu-entry", items, count);
}
