#pragma once
//
// utils.h - small helpers shared by the GUI and the core DLL.
//
#include <Windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// TRUE when a high contrast theme is active.
BOOL IsHighContrast(void);

// Monotonic millisecond counter (QueryPerformanceCounter based).
long long milliseconds_now(void);

// TRUE when the file (or directory) exists.
BOOL FileExistsW(const wchar_t* path);

// Reads the four version numbers from a module's VERSIONINFO resource.
void QueryVersionInfo(HMODULE hModule, WORD resource, DWORD* major, DWORD* minor, DWORD* buildHi, DWORD* buildLo);

// Path of the directory that contains the given module, without a trailing backslash.
BOOL GetModuleDirectoryW(HMODULE hModule, wchar_t* path, DWORD cch);

// %ProgramFiles%\<product>, whether or not anything is installed there.
BOOL GetInstallDirectoryW(wchar_t* path, DWORD cch);

// Finds one of the product's own files (sp_gui.dll, app.png, ShadePatcher.dll) by name: first next to the
// module this code is linked into, then in the install folder. The first matters in the build folder and for
// the injected build; the second matters when the engine runs as the proxy copy in the Windows folder, where
// nothing else of ours sits. Returns FALSE and an empty path when the file is in neither place.
BOOL FindProductFile(const wchar_t* name, wchar_t* path, DWORD cch);

#ifdef __cplusplus
}
#endif
