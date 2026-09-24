#include "log.h"
#include "config.h"

#include <tchar.h>

#include <Shlobj.h>
#include <stdio.h>
#include <strsafe.h>

#pragma comment(lib, "Shell32.lib")

static SP_LogLevel   g_level = SP_LOG_ERROR;
static BOOL          g_initialized = FALSE;
static HANDLE        g_file = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_lock;
static BOOL          g_lockReady = FALSE;

// Lines written by the thread that holds a hook transaction (the other threads are suspended then); one
// transaction at a time, so a single buffer is enough.
#define SP_LOG_DEFER_MAX 32
static DWORD         g_deferThread = 0;
static char          g_deferred[SP_LOG_DEFER_MAX][2400];
static int           g_deferredCount = 0;
static int           g_deferredDropped = 0;

// %LOCALAPPDATA%\ShadePatcher\logs\<process>-<pid>.log
static void OpenLogFile(void)
{
    wchar_t wszDir[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, wszDir)))
    {
        return;
    }
    StringCchCatW(wszDir, MAX_PATH, L"\\" _T(PRODUCT_NAME));
    CreateDirectoryW(wszDir, NULL);
    StringCchCatW(wszDir, MAX_PATH, L"\\logs");
    CreateDirectoryW(wszDir, NULL);

    wchar_t wszExe[MAX_PATH];
    if (!GetModuleFileNameW(NULL, wszExe, MAX_PATH))
    {
        return;
    }
    const wchar_t* pName = wcsrchr(wszExe, L'\\');
    pName = pName ? pName + 1 : wszExe;

    wchar_t wszPath[MAX_PATH];
    StringCchPrintfW(wszPath, MAX_PATH, L"%s\\%s-%lu.log", wszDir, pName, GetCurrentProcessId());

    g_file = CreateFileW(wszPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

void SP_LogInitialize(void)
{
    if (g_initialized)
    {
        return;
    }
    if (!g_lockReady)
    {
        InitializeCriticalSection(&g_lock);
        g_lockReady = TRUE;
    }

    DWORD dwLevel = 0;
    DWORD dwSize = sizeof(dwLevel);
    RegGetValueW(HKEY_CURRENT_USER, _T(REGPATH), L"Logging", RRF_RT_REG_DWORD, NULL, &dwLevel, &dwSize);
    g_level = (dwLevel > SP_LOG_DEBUG) ? SP_LOG_DEBUG : (SP_LogLevel)dwLevel;

    DWORD dwToFile = 0;
    dwSize = sizeof(dwToFile);
    RegGetValueW(HKEY_CURRENT_USER, _T(REGPATH), L"LogToFile", RRF_RT_REG_DWORD, NULL, &dwToFile, &dwSize);
    if (dwToFile && g_level > SP_LOG_ERROR)
    {
        OpenLogFile();
    }

    g_initialized = TRUE;
}

void SP_LogShutdown(void)
{
    if (g_file != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    g_initialized = FALSE;
}

BOOL SP_LogEnabled(SP_LogLevel level)
{
    // Errors are always reported to the debugger, even before initialization.
    return (level == SP_LOG_ERROR) || (g_initialized && level <= g_level);
}

void SP_LogWrite(SP_LogLevel level, const char* tag, const wchar_t* format, ...)
{
    if (!SP_LogEnabled(level))
    {
        return;
    }

    wchar_t wszMessage[1024];
    va_list args;
    va_start(args, format);
    StringCchVPrintfW(wszMessage, ARRAYSIZE(wszMessage), format, args);
    va_end(args);

    SYSTEMTIME st;
    GetLocalTime(&st);

    static const wchar_t* const kLevelNames[] = { L"ERR", L"INF", L"DBG" };

    wchar_t wszLine[1200];
    StringCchPrintfW(wszLine, ARRAYSIZE(wszLine), L"[" _T(PRODUCT_NAME) L"][%02d:%02d:%02d.%03d][%S][%S] %s\n",
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                     kLevelNames[level], tag ? tag : "engine", wszMessage);

    // The file is UTF-8 so that it can be read anywhere; the conversion is done per line.
    char szUtf8[2400];
    int cb = WideCharToMultiByte(CP_UTF8, 0, wszLine, -1, szUtf8, sizeof(szUtf8), NULL, NULL);

    if (g_deferThread != 0 && g_deferThread == GetCurrentThreadId())
    {
        // Inside a hook transaction: every other thread is suspended, and one of them may own g_lock or the
        // process-wide lock that OutputDebugString takes (a thread suspended inside its own OutputDebugString
        // call deadlocked the shell this way). Nothing is written from here; SP_LogDeferEnd does it once the
        // threads run again.
        if (cb > 1 && g_deferredCount < SP_LOG_DEFER_MAX)
        {
            memcpy(g_deferred[g_deferredCount], szUtf8, (size_t)cb);
            g_deferredCount++;
        }
        else
        {
            g_deferredDropped++;
        }
        return;
    }

    OutputDebugStringW(wszLine);

    if (g_file != INVALID_HANDLE_VALUE && g_lockReady)
    {
        if (cb > 1)
        {
            EnterCriticalSection(&g_lock);
            DWORD dwWritten = 0;
            WriteFile(g_file, szUtf8, (DWORD)(cb - 1), &dwWritten, NULL);
            LeaveCriticalSection(&g_lock);
        }
    }
}

void SP_LogDeferBegin(void)
{
    g_deferThread = GetCurrentThreadId();
    g_deferredCount = 0;
    g_deferredDropped = 0;
}

void SP_LogDeferEnd(void)
{
    g_deferThread = 0;

    if (g_deferredCount == 0 && g_deferredDropped == 0)
    {
        return;
    }
    // The debugger output first, now that the lock it needs can be taken.
    for (int i = 0; i < g_deferredCount; ++i)
    {
        OutputDebugStringA(g_deferred[i]);
    }

    if (g_file != INVALID_HANDLE_VALUE && g_lockReady)
    {
        EnterCriticalSection(&g_lock);
        for (int i = 0; i < g_deferredCount; ++i)
        {
            DWORD dwWritten = 0;
            WriteFile(g_file, g_deferred[i], (DWORD)strlen(g_deferred[i]), &dwWritten, NULL);
        }
        if (g_deferredDropped > 0)
        {
            char szNote[128];
            StringCchPrintfA(szNote, ARRAYSIZE(szNote), "[" PRODUCT_NAME "][log] %d line(s) dropped inside a hook transaction\n",
                             g_deferredDropped);
            DWORD dwWritten = 0;
            WriteFile(g_file, szNote, (DWORD)strlen(szNote), &dwWritten, NULL);
        }
        LeaveCriticalSection(&g_lock);
    }
    g_deferredCount = 0;
    g_deferredDropped = 0;
}
