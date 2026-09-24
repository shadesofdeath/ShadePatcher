#pragma once
//
// settings.h - reading a mod's settings.
//
// Layout in the registry:
//
//   HKCU\Software\ShadePatcher
//       Logging, LogToFile              engine-wide
//   HKCU\Software\ShadePatcher\Mods\<mod id>
//       Enabled            dword        0 = the mod is not loaded
//       <setting name>     dword/sz     the mod's own settings
//
// The settings window (sp_gui.dll) writes the same values, so the two halves of the product share one store and
// no extra format is needed. A mod reads its settings through SP_GetIntSetting / SP_GetStringSetting, which bind
// the mod id automatically.
//
#include <Windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// Full registry path of a mod's key, e.g. "Software\ShadePatcher\Mods\tray-show-all-icons".
BOOL SP_SettingsGetModKeyPath(const char* modId, wchar_t* wszPath, DWORD cchPath);

// Reads one value. Missing values return the default, so a fresh install needs no seeding.
int  SP_SettingsGetInt(const char* modId, const wchar_t* name, int defaultValue);
BOOL SP_SettingsGetString(const char* modId, const wchar_t* name, wchar_t* buffer, DWORD cchBuffer, const wchar_t* defaultValue);
BOOL SP_SettingsSetInt(const char* modId, const wchar_t* name, int value);

// TRUE when the user turned the mod on.
BOOL SP_SettingsIsModEnabled(const char* modId);

// Blocks until any value under HKCU\Software\ShadePatcher changes, the timeout expires, or hStopEvent is
// signalled. Returns TRUE when a change happened.
BOOL SP_SettingsWaitForChange(HANDLE hStopEvent, DWORD dwTimeoutMs);

// Releases the registry watch that SP_SettingsWaitForChange keeps between calls.
void SP_SettingsStopWatching(void);

#ifdef __cplusplus
}
#endif
