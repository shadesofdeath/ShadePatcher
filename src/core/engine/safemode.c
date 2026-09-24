#include "safemode.h"
#include "log.h"
#include "config.h"

#include <tchar.h>

#define TAG "safemode"

// Three starts inside the window means the shell is looping rather than being restarted by the user.
#define SP_SAFEMODE_TRIP_COUNT   3

// Two starts further apart than this are unrelated, so the count begins again.
#define SP_SAFEMODE_WINDOW_MS    60000

// Values under HKCU\Software\ShadePatcher.
#define SP_VALUE_LAST_START      L"ShellLastStart"      // QWORD, FILETIME
#define SP_VALUE_START_COUNT     L"ShellStartCount"     // DWORD
#define SP_VALUE_SAFE_MODE       L"SafeMode"            // DWORD, read by the settings window

static ULONGLONG NowAsFileTime(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER value;
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart;
}

static HKEY OpenProductKey(void)
{
    HKEY hKey = NULL;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, _T(REGPATH), 0, NULL, REG_OPTION_NON_VOLATILE,
                        KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &hKey, NULL) != ERROR_SUCCESS)
    {
        return NULL;
    }
    return hKey;
}

static void WriteDword(HKEY hKey, const wchar_t* name, DWORD value)
{
    RegSetValueExW(hKey, name, 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
}

BOOL SP_SafeModeCheckOnStart(void)
{
    HKEY hKey = OpenProductKey();
    if (!hKey)
    {
        // Without the key nothing can be recorded, so there is no way to detect a loop. Loading the mods is the
        // better failure: refusing to load them every time would disable the product outright.
        SP_LOG_ERR(TAG, L"The product key could not be opened; the crash guard is inactive");
        return FALSE;
    }

    ULONGLONG lastStart = 0;
    DWORD cb = sizeof(lastStart);
    DWORD type = 0;
    RegQueryValueExW(hKey, SP_VALUE_LAST_START, NULL, &type, (LPBYTE)&lastStart, &cb);
    if (type != REG_QWORD || cb != sizeof(lastStart))
    {
        lastStart = 0;
    }

    DWORD count = 0;
    cb = sizeof(count);
    type = 0;
    RegQueryValueExW(hKey, SP_VALUE_START_COUNT, NULL, &type, (LPBYTE)&count, &cb);
    if (type != REG_DWORD)
    {
        count = 0;
    }

    ULONGLONG now = NowAsFileTime();

    // FILETIME counts 100-nanosecond intervals, so a millisecond is 10000 of them.
    ULONGLONG elapsedMs = (lastStart != 0 && now > lastStart) ? (now - lastStart) / 10000ULL : MAXULONGLONG;

    if (elapsedMs < SP_SAFEMODE_WINDOW_MS)
    {
        count++;
    }
    else
    {
        count = 1;
    }

    ULARGE_INTEGER nowValue;
    nowValue.QuadPart = now;
    RegSetValueExW(hKey, SP_VALUE_LAST_START, 0, REG_QWORD, (const BYTE*)&nowValue.QuadPart, sizeof(nowValue.QuadPart));
    WriteDword(hKey, SP_VALUE_START_COUNT, count);

    BOOL safeMode = (count >= SP_SAFEMODE_TRIP_COUNT);
    WriteDword(hKey, SP_VALUE_SAFE_MODE, safeMode ? 1 : 0);

    RegCloseKey(hKey);

    if (safeMode)
    {
        SP_LOG_ERR(TAG, L"The shell has started %lu times in under %d seconds. No mods will be loaded this "
                        L"session so that the desktop stays usable.", count, SP_SAFEMODE_WINDOW_MS / 1000);
    }
    else if (count > 1)
    {
        SP_LOG_INF(TAG, L"Shell start %lu within the crash window", count);
    }

    return safeMode;
}

void SP_SafeModeMarkHealthy(void)
{
    HKEY hKey = OpenProductKey();
    if (!hKey)
    {
        return;
    }

    DWORD count = 0;
    DWORD cb = sizeof(count);
    RegQueryValueExW(hKey, SP_VALUE_START_COUNT, NULL, NULL, (LPBYTE)&count, &cb);

    if (count != 0)
    {
        WriteDword(hKey, SP_VALUE_START_COUNT, 0);
        SP_LOG_INF(TAG, L"The shell has been up for %d seconds; the crash counter is cleared",
                   SP_SAFEMODE_HEALTHY_AFTER_MS / 1000);
    }

    RegCloseKey(hKey);
}

BOOL SP_SafeModeIsActive(void)
{
    DWORD value = 0;
    DWORD cb = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, _T(REGPATH), SP_VALUE_SAFE_MODE, RRF_RT_REG_DWORD, NULL, &value, &cb) != ERROR_SUCCESS)
    {
        return FALSE;
    }
    return value != 0;
}
