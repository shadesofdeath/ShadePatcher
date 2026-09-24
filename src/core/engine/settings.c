#include "settings.h"
#include "log.h"
#include "config.h"

#include <tchar.h>

#include <strsafe.h>

#define TAG "settings"

BOOL SP_SettingsGetModKeyPath(const char* modId, wchar_t* wszPath, DWORD cchPath)
{
    if (!modId || !wszPath)
    {
        return FALSE;
    }
    return SUCCEEDED(StringCchPrintfW(wszPath, cchPath, L"%s\\Mods\\%S", _T(REGPATH), modId));
}

int SP_SettingsGetInt(const char* modId, const wchar_t* name, int defaultValue)
{
    wchar_t wszPath[MAX_PATH];
    if (!SP_SettingsGetModKeyPath(modId, wszPath, ARRAYSIZE(wszPath)))
    {
        return defaultValue;
    }

    DWORD dwValue = 0;
    DWORD dwSize = sizeof(dwValue);
    if (RegGetValueW(HKEY_CURRENT_USER, wszPath, name, RRF_RT_REG_DWORD, NULL, &dwValue, &dwSize) != ERROR_SUCCESS)
    {
        return defaultValue;
    }
    return (int)dwValue;
}

BOOL SP_SettingsGetString(const char* modId, const wchar_t* name, wchar_t* buffer, DWORD cchBuffer,
                          const wchar_t* defaultValue)
{
    if (!buffer || cchBuffer == 0)
    {
        return FALSE;
    }
    buffer[0] = 0;

    wchar_t wszPath[MAX_PATH];
    if (SP_SettingsGetModKeyPath(modId, wszPath, ARRAYSIZE(wszPath)))
    {
        DWORD dwSize = cchBuffer * sizeof(wchar_t);
        if (RegGetValueW(HKEY_CURRENT_USER, wszPath, name, RRF_RT_REG_SZ, NULL, buffer, &dwSize) == ERROR_SUCCESS)
        {
            return TRUE;
        }
    }

    if (defaultValue)
    {
        StringCchCopyW(buffer, cchBuffer, defaultValue);
    }
    return FALSE;
}

BOOL SP_SettingsSetInt(const char* modId, const wchar_t* name, int value)
{
    wchar_t wszPath[MAX_PATH];
    if (!SP_SettingsGetModKeyPath(modId, wszPath, ARRAYSIZE(wszPath)))
    {
        return FALSE;
    }

    HKEY hKey = NULL;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, wszPath, 0, NULL, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE, NULL, &hKey, NULL) != ERROR_SUCCESS)
    {
        return FALSE;
    }

    DWORD dwValue = (DWORD)value;
    LSTATUS lRes = RegSetValueExW(hKey, name, 0, REG_DWORD, (const BYTE*)&dwValue, sizeof(dwValue));
    RegCloseKey(hKey);
    return lRes == ERROR_SUCCESS;
}

BOOL SP_SettingsIsModEnabled(const char* modId)
{
    return SP_SettingsGetInt(modId, L"Enabled", 0) != 0;
}

// The watch is kept between calls. RegNotifyChangeKeyValue fires once and then has to be asked again, and a
// change made in the gap between one notification and the next request would be lost: the manager acts on a
// notification by loading, unloading and re-reading mods, which can take seconds, and a value written during that
// (the settings window writes an option and "Enabled" back to back) went unseen until something else changed.
// So the request is renewed right after the notification is consumed, before the caller acts on it: anything
// written meanwhile signals the event and the next call returns at once.
static HKEY   g_hWatchKey = NULL;
static HANDLE g_hWatchEvent = NULL;
static BOOL   g_watchArmed = FALSE;

static BOOL ArmWatch(void)
{
    if (g_watchArmed)
    {
        return TRUE;
    }
    // REG_NOTIFY_THREAD_AGNOSTIC keeps the registration alive even if this thread later exits.
    LSTATUS lRes = RegNotifyChangeKeyValue(
        g_hWatchKey, TRUE,
        REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_THREAD_AGNOSTIC,
        g_hWatchEvent, TRUE);
    if (lRes != ERROR_SUCCESS)
    {
        SP_LOG_ERR(TAG, L"RegNotifyChangeKeyValue failed: %d", lRes);
        return FALSE;
    }
    g_watchArmed = TRUE;
    return TRUE;
}

BOOL SP_SettingsWaitForChange(HANDLE hStopEvent, DWORD dwTimeoutMs)
{
    if (!g_hWatchKey &&
        RegCreateKeyExW(HKEY_CURRENT_USER, _T(REGPATH), 0, NULL, REG_OPTION_NON_VOLATILE,
                        KEY_NOTIFY, NULL, &g_hWatchKey, NULL) != ERROR_SUCCESS)
    {
        // Without the key there is nothing to watch; fall back to a plain sleep so the caller still polls.
        g_hWatchKey = NULL;
        WaitForSingleObject(hStopEvent, dwTimeoutMs);
        return FALSE;
    }

    if (!g_hWatchEvent)
    {
        g_hWatchEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!g_hWatchEvent)
        {
            WaitForSingleObject(hStopEvent, dwTimeoutMs);
            return FALSE;
        }
    }

    if (!ArmWatch())
    {
        WaitForSingleObject(hStopEvent, dwTimeoutMs);
        return FALSE;
    }

    HANDLE handles[2] = { hStopEvent, g_hWatchEvent };
    DWORD dwWait = WaitForMultipleObjects(2, handles, FALSE, dwTimeoutMs);
    if (dwWait != WAIT_OBJECT_0 + 1)
    {
        return FALSE;
    }

    // Consumed: renew the request before the caller acts, so nothing written meanwhile is missed.
    ResetEvent(g_hWatchEvent);
    g_watchArmed = FALSE;
    ArmWatch();
    return TRUE;
}

void SP_SettingsStopWatching(void)
{
    if (g_hWatchEvent)
    {
        CloseHandle(g_hWatchEvent);
        g_hWatchEvent = NULL;
    }
    if (g_hWatchKey)
    {
        RegCloseKey(g_hWatchKey);
        g_hWatchKey = NULL;
    }
    g_watchArmed = FALSE;
}
