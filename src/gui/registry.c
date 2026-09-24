#include "registry.h"
#include "config.h"
#include "getline.h"
#include "utils.h"

#include <shellapi.h>
#include <stdlib.h>
#include <string.h>
#include <tchar.h>

FILE* AuditFile = NULL;

#define VIRTUAL_PREFIX  L"Virtualized_" _T(APP_CLSID) L"_"

// ---------------------------------------------------------------------------------------------------------------
// Virtual values
//
// Add an entry here for every setting that is not a plain registry value. The name is the part after the prefix,
// e.g. for ";"Virtualized_{...}_AutoHideTaskbar"=dword:0" the name is "AutoHideTaskbar".
// ---------------------------------------------------------------------------------------------------------------

typedef LSTATUS (*VirtualGet_t)(DWORD* pdwValue);
typedef LSTATUS (*VirtualSet_t)(DWORD dwValue);

typedef struct VirtualValue
{
    const wchar_t* name;
    VirtualGet_t get;
    VirtualSet_t set;
} VirtualValue;

// ---------------------------------------------------------------------------------------------------------------
// StartAtLogon: a Run entry that loads the engine into the shell after sign-in.
//
// With the installer's proxy in the Windows folder the engine is in the shell from the first moment and this is
// not needed; the entry is for a build that was injected instead, which a fresh shell would not have. The entry
// runs ZZStartup from the core DLL, which waits for the shell and injects only when the engine is not already
// there, so leaving it on with the proxy installed costs nothing.
// ---------------------------------------------------------------------------------------------------------------

#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

static LSTATUS StartAtLogon_Get(DWORD* pdwValue)
{
    wchar_t wszCommand[MAX_PATH * 2];
    DWORD cb = sizeof(wszCommand);
    *pdwValue = RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, _T(PRODUCT_NAME), RRF_RT_REG_SZ, NULL, wszCommand, &cb)
                    == ERROR_SUCCESS;
    return ERROR_SUCCESS;
}

static LSTATUS StartAtLogon_Set(DWORD dwValue)
{
    if (!dwValue)
    {
        LSTATUS lRes = RegDeleteKeyValueW(HKEY_CURRENT_USER, RUN_KEY, _T(PRODUCT_NAME));
        return (lRes == ERROR_FILE_NOT_FOUND) ? ERROR_SUCCESS : lRes;
    }

    wchar_t wszCore[MAX_PATH];
    if (!FindProductFile(_T(CORE_DLL_NAME), wszCore, ARRAYSIZE(wszCore)))
    {
        return ERROR_FILE_NOT_FOUND;
    }

    wchar_t wszCommand[MAX_PATH * 2];
    swprintf_s(wszCommand, ARRAYSIZE(wszCommand), L"rundll32.exe \"%s\",ZZStartup", wszCore);
    return RegSetKeyValueW(HKEY_CURRENT_USER, RUN_KEY, _T(PRODUCT_NAME), REG_SZ, wszCommand,
                           (DWORD)((wcslen(wszCommand) + 1) * sizeof(wchar_t)));
}

// ---------------------------------------------------------------------------------------------------------------
// NoShortcutSuffix: the "- Shortcut" suffix on new shortcuts is turned off by writing four zero bytes to the
// REG_BINARY value "link" under Explorer, which the page engine cannot express (it knows dword and sz), so it is
// a virtual value. Deleting the value restores the suffix.
// ---------------------------------------------------------------------------------------------------------------

#define EXPLORER_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer"

static LSTATUS NoShortcutSuffix_Get(DWORD* pdwValue)
{
    BYTE data[16] = { 0xFF, 0xFF, 0xFF, 0xFF };
    DWORD cb = sizeof(data);
    DWORD type = 0;
    LSTATUS lRes = RegGetValueW(HKEY_CURRENT_USER, EXPLORER_KEY, L"link", RRF_RT_REG_BINARY, &type, data, &cb);
    *pdwValue = (lRes == ERROR_SUCCESS && cb >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 0);
    return ERROR_SUCCESS;
}

static LSTATUS NoShortcutSuffix_Set(DWORD dwValue)
{
    if (!dwValue)
    {
        LSTATUS lRes = RegDeleteKeyValueW(HKEY_CURRENT_USER, EXPLORER_KEY, L"link");
        return (lRes == ERROR_FILE_NOT_FOUND) ? ERROR_SUCCESS : lRes;
    }
    static const BYTE zeros[4] = { 0, 0, 0, 0 };
    return RegSetKeyValueW(HKEY_CURRENT_USER, EXPLORER_KEY, L"link", REG_BINARY, zeros, sizeof(zeros));
}

static const VirtualValue g_virtualValues[] =
{
    { L"StartAtLogon", StartAtLogon_Get, StartAtLogon_Set },
    { L"NoShortcutSuffix", NoShortcutSuffix_Get, NoShortcutSuffix_Set },
    { NULL, NULL, NULL }
};

static const VirtualValue* FindVirtualValue(LPCWSTR lpValueName)
{
    size_t cchPrefix = wcslen(VIRTUAL_PREFIX);
    if (!lpValueName || wcsncmp(lpValueName, VIRTUAL_PREFIX, cchPrefix))
    {
        return NULL;
    }
    const wchar_t* name = lpValueName + cchPrefix;
    for (const VirtualValue* v = g_virtualValues; v->name; ++v)
    {
        if (!wcscmp(v->name, name))
        {
            return v;
        }
    }
    return NULL;
}

BOOL Registry_IsVirtualValue(LPCWSTR lpValueName)
{
    return lpValueName && !wcsncmp(lpValueName, VIRTUAL_PREFIX, wcslen(VIRTUAL_PREFIX));
}

// ---------------------------------------------------------------------------------------------------------------
// Wrappers used by GUI.c
// ---------------------------------------------------------------------------------------------------------------

static void AuditKey(HKEY hRoot, LPCWSTR lpSubKey, BOOL bMissing)
{
    if (!AuditFile) return;
    fwprintf(AuditFile, L"[%s%s\\%s]\n",
        bMissing ? L"-" : L"",
        hRoot == HKEY_CURRENT_USER ? L"HKEY_CURRENT_USER" : L"HKEY_LOCAL_MACHINE",
        lpSubKey);
}

LSTATUS GUI_RegCreateKeyExW(HKEY hKey, LPCWSTR lpSubKey, DWORD Reserved, LPWSTR lpClass, DWORD dwOptions,
                            REGSAM samDesired, const LPSECURITY_ATTRIBUTES lpSecurityAttributes, PHKEY phkResult,
                            LPDWORD lpdwDisposition)
{
    LSTATUS lRes = RegCreateKeyExW(hKey, lpSubKey, Reserved, lpClass, dwOptions, samDesired, lpSecurityAttributes, phkResult, lpdwDisposition);
    AuditKey(hKey, lpSubKey, FALSE);
    return lRes;
}

LSTATUS GUI_RegOpenKeyExW(HKEY hKey, LPCWSTR lpSubKey, DWORD ulOptions, REGSAM samDesired, PHKEY phkResult)
{
    LSTATUS lRes = RegOpenKeyExW(hKey, lpSubKey, ulOptions, samDesired, phkResult);
    if (AuditFile)
    {
        BOOL bMissing = (*phkResult == NULL || *phkResult == INVALID_HANDLE_VALUE);
        AuditKey(hKey, lpSubKey, bMissing);
        WCHAR wszDefVal[MAX_PATH];
        ZeroMemory(wszDefVal, sizeof(wszDefVal));
        DWORD dwLen = sizeof(wszDefVal);
        RegGetValueW(hKey, lpSubKey, NULL, RRF_RT_REG_SZ, NULL, wszDefVal, &dwLen);
        fwprintf(AuditFile, L"@=\"%s\"\n", wszDefVal);
    }
    return lRes;
}

LSTATUS GUI_RegQueryValueExW(HKEY hKey, LPCWSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData)
{
    DWORD dwSize = lpcbData ? *lpcbData : sizeof(DWORD);
    LSTATUS lRes;

    const VirtualValue* v = FindVirtualValue(lpValueName);
    if (v)
    {
        lRes = v->get ? v->get((DWORD*)lpData) : ERROR_NOT_SUPPORTED;
        if (lpcbData) *lpcbData = sizeof(DWORD);
    }
    else if (Registry_IsVirtualValue(lpValueName))
    {
        // Unknown virtual value: report 0 so the page still renders.
        *(DWORD*)lpData = 0;
        if (lpcbData) *lpcbData = sizeof(DWORD);
        lRes = ERROR_SUCCESS;
    }
    else
    {
        lRes = RegQueryValueExW(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);
    }

    if (AuditFile)
    {
        BOOL bVirtual = Registry_IsVirtualValue(lpValueName);
        if (dwSize != sizeof(DWORD))
        {
            fwprintf(AuditFile, L"%s\"%s\"=\"%s\"\n", bVirtual ? L";" : L"", lpValueName, (const wchar_t*)lpData);
        }
        else
        {
            fwprintf(AuditFile, L"%s\"%s\"=dword:%08x\n", bVirtual ? L";" : L"", lpValueName, *(DWORD*)lpData);
        }
    }
    return lRes;
}

LSTATUS GUI_RegSetValueExW(HKEY hKey, LPCWSTR lpValueName, DWORD Reserved, DWORD dwType, const BYTE* lpData, DWORD cbData)
{
    const VirtualValue* v = FindVirtualValue(lpValueName);
    if (v)
    {
        return v->set ? v->set(*(const DWORD*)lpData) : ERROR_NOT_SUPPORTED;
    }
    if (Registry_IsVirtualValue(lpValueName))
    {
        return ERROR_SUCCESS;
    }
    return RegSetValueExW(hKey, lpValueName, Reserved, dwType, lpData, cbData);
}

// ---------------------------------------------------------------------------------------------------------------
// Import helpers
// ---------------------------------------------------------------------------------------------------------------

BOOL Registry_WriteTempRegFile(const void* data, DWORD cbData, wchar_t* wszPath, DWORD cchPath)
{
    wchar_t wszDir[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, wszDir))
    {
        return FALSE;
    }
    wchar_t wszFile[MAX_PATH];
    if (!GetTempFileNameW(wszDir, L"sp_", 0, wszFile))
    {
        return FALSE;
    }
    // GetTempFileName creates a .tmp file; reg.exe insists on the .reg extension.
    DeleteFileW(wszFile);
    wchar_t* pDot = wcsrchr(wszFile, L'.');
    if (pDot) *pDot = 0;
    wcscat_s(wszFile, MAX_PATH, L".reg");

    HANDLE hFile = CreateFileW(wszFile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        return FALSE;
    }
    DWORD dwWritten = 0;
    BOOL bOk = WriteFile(hFile, data, cbData, &dwWritten, NULL) && dwWritten == cbData;
    CloseHandle(hFile);
    if (bOk)
    {
        wcscpy_s(wszPath, cchPath, wszFile);
    }
    else
    {
        DeleteFileW(wszFile);
    }
    return bOk;
}

static BOOL RunRegImport(const wchar_t* wszPath)
{
    wchar_t wszReg[MAX_PATH];
    GetSystemDirectoryW(wszReg, MAX_PATH);
    wcscat_s(wszReg, MAX_PATH, L"\\reg.exe");

    wchar_t wszArgs[MAX_PATH + 16];
    swprintf_s(wszArgs, ARRAYSIZE(wszArgs), L"import \"%s\"", wszPath);

    SHELLEXECUTEINFOW sei;
    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpFile = wszReg;
    sei.lpParameters = wszArgs;
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess)
    {
        return FALSE;
    }
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD dwExitCode = 1;
    GetExitCodeProcess(sei.hProcess, &dwExitCode);
    CloseHandle(sei.hProcess);
    return dwExitCode == 0;
}

// Lines of the form ;"Virtualized_{...}_Name"=dword:00000001 are comments to reg.exe; apply them by hand.
static void ReplayVirtualValues(const wchar_t* wszPath)
{
    FILE* f = NULL;
    if (_wfopen_s(&f, wszPath, L"r") || !f)
    {
        return;
    }
    char* line = NULL;
    size_t bufsiz = 0;
    while (getline(&line, &bufsiz, f) != -1)
    {
        if (strncmp(line, ";\"Virtualized_", 14))
        {
            continue;
        }
        wchar_t wszLine[1024];
        ZeroMemory(wszLine, sizeof(wszLine));
        MultiByteToWideChar(CP_UTF8, 0, line + 2, -1, wszLine, ARRAYSIZE(wszLine));

        wchar_t* pEquals = wcschr(wszLine, L'=');
        if (!pEquals) continue;
        *pEquals = 0;
        wchar_t* pQuote = wcschr(wszLine, L'"');
        if (pQuote) *pQuote = 0;
        if (wcsncmp(pEquals + 1, L"dword:", 6)) continue;

        DWORD dwValue = wcstoul(pEquals + 7, NULL, 16);
        GUI_RegSetValueExW(NULL, wszLine, 0, REG_DWORD, (const BYTE*)&dwValue, sizeof(dwValue));
    }
    free(line);
    fclose(f);
}

BOOL Registry_ImportFile(const wchar_t* wszPath)
{
    if (!RunRegImport(wszPath))
    {
        return FALSE;
    }
    ReplayVirtualValues(wszPath);
    return TRUE;
}
