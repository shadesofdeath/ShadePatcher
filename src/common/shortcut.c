#define COBJMACROS
#include "shortcut.h"
#include "config.h"
#include "utils.h"

#include <Shlobj.h>
#include <Shlwapi.h>
#include <objbase.h>
#include <tchar.h>

#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Shell32.lib")

// The shortcut is what makes the settings show up when the user types the product name into Start search, the
// way ExplorerPatcher's "Properties" entry does. Its name follows the user's display language so that
// "ayarlar" finds it too.
static void ShortcutFileName(wchar_t* name, DWORD cch)
{
    LANGID lang = GetUserDefaultUILanguage();
    if (PRIMARYLANGID(lang) == LANG_TURKISH)
    {
        // "ShadePatcher Ayarlari" with a dotted capital I is not needed; the plain form is what people type.
        wcscpy_s(name, cch, _T(PRODUCT_NAME) L" Ayarlar\x0131.lnk");
    }
    else
    {
        wcscpy_s(name, cch, _T(PRODUCT_NAME) L" Settings.lnk");
    }
}

static BOOL ShortcutPath(int csidl, const wchar_t* fileName, wchar_t* path, DWORD cch)
{
    if (FAILED(SHGetFolderPathW(NULL, csidl, NULL, SHGFP_TYPE_CURRENT, path)))
    {
        return FALSE;
    }
    UNREFERENCED_PARAMETER(cch);
    return PathAppendW(path, fileName) != FALSE;
}

BOOL SettingsShortcutExists(BOOL allUsers)
{
    wchar_t name[MAX_PATH];
    ShortcutFileName(name, MAX_PATH);

    // Either language's name counts: the user may have switched display language since it was made.
    static const wchar_t* const kNames[] = { _T(PRODUCT_NAME) L" Ayarlar\x0131.lnk", _T(PRODUCT_NAME) L" Settings.lnk" };
    for (int i = 0; i < ARRAYSIZE(kNames); ++i)
    {
        wchar_t path[MAX_PATH];
        if (ShortcutPath(allUsers ? CSIDL_COMMON_PROGRAMS : CSIDL_PROGRAMS, kNames[i], path, MAX_PATH) &&
            FileExistsW(path))
        {
            return TRUE;
        }
    }
    return FALSE;
}

BOOL CreateSettingsShortcut(const wchar_t* coreDllPath, BOOL allUsers)
{
    if (!coreDllPath || !coreDllPath[0])
    {
        return FALSE;
    }

    wchar_t name[MAX_PATH];
    ShortcutFileName(name, MAX_PATH);

    wchar_t path[MAX_PATH];
    if (!ShortcutPath(allUsers ? CSIDL_COMMON_PROGRAMS : CSIDL_PROGRAMS, name, path, MAX_PATH))
    {
        return FALSE;
    }

    wchar_t rundll[MAX_PATH];
    GetSystemDirectoryW(rundll, MAX_PATH);
    PathAppendW(rundll, L"rundll32.exe");

    wchar_t args[MAX_PATH * 2];
    swprintf_s(args, ARRAYSIZE(args), L"\"%s\",ZZGUI", coreDllPath);

    // COM may or may not be up on this thread already (it is on the shell's threads); either answer is fine,
    // and the matching uninitialise is only done for an initialise that succeeded here.
    HRESULT hrInit = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    BOOL result = FALSE;

    IShellLinkW* link = NULL;
    if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void**)&link)))
    {
        IShellLinkW_SetPath(link, rundll);
        IShellLinkW_SetArguments(link, args);
        IShellLinkW_SetIconLocation(link, coreDllPath, 0);
        IShellLinkW_SetDescription(link, _T(PRODUCT_NAME));

        wchar_t workDir[MAX_PATH];
        wcscpy_s(workDir, MAX_PATH, coreDllPath);
        PathRemoveFileSpecW(workDir);
        IShellLinkW_SetWorkingDirectory(link, workDir);

        IPersistFile* file = NULL;
        if (SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void**)&file)))
        {
            result = SUCCEEDED(IPersistFile_Save(file, path, TRUE));
            IPersistFile_Release(file);
        }
        IShellLinkW_Release(link);
    }

    if (SUCCEEDED(hrInit))
    {
        CoUninitialize();
    }
    return result;
}

void RemoveSettingsShortcuts(void)
{
    static const wchar_t* const kNames[] = { _T(PRODUCT_NAME) L" Ayarlar\x0131.lnk", _T(PRODUCT_NAME) L" Settings.lnk" };
    static const int kFolders[] = { CSIDL_PROGRAMS, CSIDL_COMMON_PROGRAMS };
    for (int f = 0; f < ARRAYSIZE(kFolders); ++f)
    {
        for (int i = 0; i < ARRAYSIZE(kNames); ++i)
        {
            wchar_t path[MAX_PATH];
            if (ShortcutPath(kFolders[f], kNames[i], path, MAX_PATH))
            {
                DeleteFileW(path);
            }
        }
    }
}

BOOL EnsureSettingsShortcut(const wchar_t* coreDllPath)
{
    if (SettingsShortcutExists(TRUE) || SettingsShortcutExists(FALSE))
    {
        return TRUE;
    }
    return CreateSettingsShortcut(coreDllPath, FALSE);
}
