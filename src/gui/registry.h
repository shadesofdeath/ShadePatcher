#pragma once
//
// registry.h - the registry layer of the settings window.
//
// GUI.c never calls the Win32 registry functions directly; it goes through the GUI_Reg* wrappers below. That gives
// two things:
//   1. Auditing: while AuditFile is open, every key and value the page engine touches is written to it as a .reg
//      file. That is how "Export settings" works.
//   2. Virtual values: a settings.reg line whose value name starts with "Virtualized_<APP_CLSID>_" is not stored in
//      the registry at all; reading and writing it is routed to a handler in registry.c (for example a setting
//      that is really a Windows API call). Such lines are written commented out (";"Virtualized_..."=dword:...")
//      so that importing the file with regedit ignores them.
//
#include <Windows.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

extern FILE* AuditFile;

LSTATUS GUI_RegCreateKeyExW(HKEY hKey, LPCWSTR lpSubKey, DWORD Reserved, LPWSTR lpClass, DWORD dwOptions,
                            REGSAM samDesired, const LPSECURITY_ATTRIBUTES lpSecurityAttributes, PHKEY phkResult,
                            LPDWORD lpdwDisposition);
LSTATUS GUI_RegOpenKeyExW(HKEY hKey, LPCWSTR lpSubKey, DWORD ulOptions, REGSAM samDesired, PHKEY phkResult);
LSTATUS GUI_RegQueryValueExW(HKEY hKey, LPCWSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData);
LSTATUS GUI_RegSetValueExW(HKEY hKey, LPCWSTR lpValueName, DWORD Reserved, DWORD dwType, const BYTE* lpData, DWORD cbData);

// TRUE when the name uses the virtual value prefix.
BOOL Registry_IsVirtualValue(LPCWSTR lpValueName);

// Writes a buffer to a new temporary .reg file; returns the path in wszPath.
BOOL Registry_WriteTempRegFile(const void* data, DWORD cbData, wchar_t* wszPath, DWORD cchPath);

// Imports a .reg file with reg.exe, then replays its commented-out virtual values through GUI_RegSetValueExW.
BOOL Registry_ImportFile(const wchar_t* wszPath);

#ifdef __cplusplus
}
#endif
