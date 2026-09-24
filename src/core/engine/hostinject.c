#include "hostinject.h"
#include "log.h"
#include "config.h"

#include <TlHelp32.h>
#include <tchar.h>
#include <Shlwapi.h>

#define TAG "hostinject"

// How often a new Start menu process is looked for. The host is started once per session and again after it
// crashes or is restarted, so a few seconds of delay only matter on the very first open after that.
#define SP_HOSTINJECT_INTERVAL_MS 3000

extern HMODULE SP_GetCoreModule(void);

static HANDLE g_thread = NULL;
static HANDLE g_stopEvent = NULL;

// Processes already dealt with, identified by id and start time so a reused id is not mistaken for an old one.
typedef struct HostProcess
{
    DWORD    pid;
    FILETIME created;
} HostProcess;

#define SP_HOSTINJECT_MAX_KNOWN 16
static HostProcess g_known[SP_HOSTINJECT_MAX_KNOWN];
static int         g_knownCount = 0;

static BOOL IsKnown(DWORD pid, const FILETIME* created)
{
    for (int i = 0; i < g_knownCount; ++i)
    {
        if (g_known[i].pid == pid && CompareFileTime(&g_known[i].created, created) == 0)
        {
            return TRUE;
        }
    }
    return FALSE;
}

static void Remember(DWORD pid, const FILETIME* created)
{
    if (g_knownCount == SP_HOSTINJECT_MAX_KNOWN)
    {
        // The oldest entries belong to processes long gone.
        MoveMemory(g_known, g_known + 1, sizeof(g_known[0]) * (SP_HOSTINJECT_MAX_KNOWN - 1));
        g_knownCount--;
    }
    g_known[g_knownCount].pid = pid;
    g_known[g_knownCount].created = *created;
    g_knownCount++;
}

// Whether an engine is already in the process: this very file, another copy of the core DLL, or the proxy copy
// (a dxgi.dll that does not come from the system folder). -1 when the modules cannot be listed, which happens
// for a moment while the process is starting; the next pass asks again.
static int HasEngine(DWORD pid, const wchar_t* wszSelf)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return -1;
    }

    wchar_t wszSystem[MAX_PATH];
    UINT cchSystem = GetSystemDirectoryW(wszSystem, MAX_PATH);

    BOOL found = FALSE;
    MODULEENTRY32W entry;
    entry.dwSize = sizeof(entry);
    for (BOOL more = Module32FirstW(snapshot, &entry); more && !found; more = Module32NextW(snapshot, &entry))
    {
        if (_wcsicmp(entry.szExePath, wszSelf) == 0 || _wcsicmp(entry.szModule, _T(CORE_DLL_NAME)) == 0)
        {
            found = TRUE;
        }
        else if (_wcsicmp(entry.szModule, L"dxgi.dll") == 0 && cchSystem &&
                 _wcsnicmp(entry.szExePath, wszSystem, cchSystem) != 0)
        {
            found = TRUE;
        }
    }
    CloseHandle(snapshot);
    return found;
}

// Loads the core DLL into the process with a remote LoadLibraryW. kernel32 sits at the same address in every
// process of the session, so our own LoadLibraryW address is valid there too.
static void LoadInto(HANDLE hProcess, DWORD pid, const wchar_t* wszSelf)
{
    const SIZE_T cb = (wcslen(wszSelf) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(hProcess, NULL, cb, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote)
    {
        SP_LOG_ERR(TAG, L"No memory in the Start menu process %lu: %lu", pid, GetLastError());
        return;
    }
    if (!WriteProcessMemory(hProcess, remote, wszSelf, cb, NULL))
    {
        SP_LOG_ERR(TAG, L"Writing to the Start menu process %lu failed: %lu", pid, GetLastError());
        VirtualFreeEx(hProcess, remote, 0, MEM_RELEASE);
        return;
    }

    LPTHREAD_START_ROUTINE pfnLoadLibraryW =
        (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE hThread = CreateRemoteThread(hProcess, NULL, 0, pfnLoadLibraryW, remote, 0, NULL);
    if (!hThread)
    {
        SP_LOG_ERR(TAG, L"Starting a thread in the Start menu process %lu failed: %lu", pid, GetLastError());
        VirtualFreeEx(hProcess, remote, 0, MEM_RELEASE);
        return;
    }

    // While the Start menu is closed Windows may keep its process frozen; the thread then runs the next time the
    // menu opens. Waiting for that is pointless, and the few bytes of the path are left behind in that case.
    if (WaitForSingleObject(hThread, 5000) == WAIT_OBJECT_0)
    {
        DWORD exitCode = 0;
        GetExitCodeThread(hThread, &exitCode);
        if (exitCode)
        {
            SP_LOG_INF(TAG, L"Engine loaded into the Start menu process %lu", pid);
        }
        else
        {
            SP_LOG_ERR(TAG, L"The Start menu process %lu could not load %s", pid, wszSelf);
        }
        VirtualFreeEx(hProcess, remote, 0, MEM_RELEASE);
    }
    else
    {
        SP_LOG_INF(TAG, L"The Start menu process %lu is frozen; the engine goes in when it wakes up", pid);
    }
    CloseHandle(hThread);
}

static void ScanOnce(const wchar_t* wszSelf)
{
    DWORD thisSession = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &thisSession);

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return;
    }

    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
    {
        if (_wcsicmp(entry.szExeFile, L"StartMenuExperienceHost.exe") != 0)
        {
            continue;
        }

        DWORD session = 0;
        if (!ProcessIdToSessionId(entry.th32ProcessID, &session) || session != thisSession)
        {
            continue;
        }

        HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_CREATE_THREAD |
                                      PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE,
                                      FALSE, entry.th32ProcessID);
        if (!hProcess)
        {
            continue;
        }

        FILETIME created, exited, kernel, user;
        if (GetProcessTimes(hProcess, &created, &exited, &kernel, &user) && !IsKnown(entry.th32ProcessID, &created))
        {
            const int present = HasEngine(entry.th32ProcessID, wszSelf);
            if (present == 0)
            {
                LoadInto(hProcess, entry.th32ProcessID, wszSelf);
            }
            if (present >= 0)
            {
                Remember(entry.th32ProcessID, &created);
            }
        }
        CloseHandle(hProcess);
    }
    CloseHandle(snapshot);
}

static DWORD WINAPI WatchThread(LPVOID lpParameter)
{
    UNREFERENCED_PARAMETER(lpParameter);

    wchar_t wszSelf[MAX_PATH];
    if (!GetModuleFileNameW(SP_GetCoreModule(), wszSelf, MAX_PATH))
    {
        return 0;
    }

    SP_LOG_DBG(TAG, L"Watching for the Start menu process");
    do
    {
        ScanOnce(wszSelf);
    } while (WaitForSingleObject(g_stopEvent, SP_HOSTINJECT_INTERVAL_MS) == WAIT_TIMEOUT);
    return 0;
}

void SP_HostInjectUpdate(BOOL wanted)
{
    if (wanted && !g_thread)
    {
        g_stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!g_stopEvent)
        {
            return;
        }
        g_thread = CreateThread(NULL, 0, WatchThread, NULL, 0, NULL);
        if (!g_thread)
        {
            CloseHandle(g_stopEvent);
            g_stopEvent = NULL;
        }
    }
    else if (!wanted && g_thread)
    {
        SP_HostInjectStop();
    }
}

void SP_HostInjectStop(void)
{
    if (!g_thread)
    {
        return;
    }
    SetEvent(g_stopEvent);
    WaitForSingleObject(g_thread, 10000);
    CloseHandle(g_thread);
    CloseHandle(g_stopEvent);
    g_thread = NULL;
    g_stopEvent = NULL;
}
