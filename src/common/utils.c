#include "utils.h"
#include "config.h"
#include <Shlwapi.h>
#include <tchar.h>

#pragma comment(lib, "Version.lib")
#pragma comment(lib, "Shlwapi.lib")

BOOL IsHighContrast(void)
{
    HIGHCONTRASTW highContrast;
    ZeroMemory(&highContrast, sizeof(highContrast));
    highContrast.cbSize = sizeof(highContrast);
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(highContrast), &highContrast, FALSE))
    {
        return (highContrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
    }
    return FALSE;
}

long long milliseconds_now(void)
{
    LARGE_INTEGER frequency;
    if (QueryPerformanceFrequency(&frequency))
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return (1000LL * now.QuadPart) / frequency.QuadPart;
    }
    return (long long)GetTickCount64();
}

BOOL FileExistsW(const wchar_t* path)
{
    WIN32_FIND_DATAW findData;
    HANDLE handle = FindFirstFileW(path, &findData);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return FALSE;
    }
    FindClose(handle);
    return TRUE;
}

void QueryVersionInfo(HMODULE hModule, WORD resource, DWORD* major, DWORD* minor, DWORD* buildHi, DWORD* buildLo)
{
    *major = *minor = *buildHi = *buildLo = 0;

    HRSRC hResInfo = FindResourceW(hModule, MAKEINTRESOURCEW(resource), RT_VERSION);
    if (!hResInfo) return;
    DWORD dwSize = SizeofResource(hModule, hResInfo);
    HGLOBAL hResData = LoadResource(hModule, hResInfo);
    if (!hResData) return;
    LPVOID pRes = LockResource(hResData);
    if (!pRes) return;

    // VerQueryValue may write into the buffer, so work on a copy.
    LPVOID pResCopy = LocalAlloc(LMEM_FIXED, dwSize);
    if (!pResCopy) return;
    CopyMemory(pResCopy, pRes, dwSize);

    VS_FIXEDFILEINFO* pFfi = NULL;
    UINT uLen = 0;
    if (VerQueryValueW(pResCopy, L"\\", (LPVOID*)&pFfi, &uLen) && pFfi)
    {
        *major = HIWORD(pFfi->dwFileVersionMS);
        *minor = LOWORD(pFfi->dwFileVersionMS);
        *buildHi = HIWORD(pFfi->dwFileVersionLS);
        *buildLo = LOWORD(pFfi->dwFileVersionLS);
    }
    LocalFree(pResCopy);
}

BOOL GetModuleDirectoryW(HMODULE hModule, wchar_t* path, DWORD cch)
{
    if (!GetModuleFileNameW(hModule, path, cch))
    {
        return FALSE;
    }
    PathRemoveFileSpecW(path);
    return TRUE;
}

BOOL GetInstallDirectoryW(wchar_t* path, DWORD cch)
{
    // The 64-bit Program Files, which is where a 64-bit product belongs.
    if (!GetEnvironmentVariableW(L"ProgramW6432", path, cch) &&
        !GetEnvironmentVariableW(L"ProgramFiles", path, cch))
    {
        return FALSE;
    }
    return wcscat_s(path, cch, TEXT(APP_RELATIVE_PATH)) == 0;
}

BOOL FindProductFile(const wchar_t* name, wchar_t* path, DWORD cch)
{
    if (!name || !path || cch == 0)
    {
        return FALSE;
    }

    // First next to the module this code lives in. That module is ShadePatcher.dll or sp_gui.dll in the build
    // or install folder, or the proxy copy in the Windows folder, where nothing else of ours is.
    HMODULE self = NULL;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&FindProductFile, &self) && self)
    {
        if (GetModuleDirectoryW(self, path, cch) && PathAppendW(path, name) && FileExistsW(path))
        {
            return TRUE;
        }
    }

    // Then the install folder, which is where the installer puts every product file.
    if (GetInstallDirectoryW(path, cch) && PathAppendW(path, name) && FileExistsW(path))
    {
        return TRUE;
    }

    path[0] = 0;
    return FALSE;
}
