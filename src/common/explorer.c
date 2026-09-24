#include "explorer.h"
#include "config.h"
#include "utils.h"

#include <shellapi.h>
#include <Shlwapi.h>
#include <TlHelp32.h>
#include <tchar.h>

#pragma comment(lib, "Shlwapi.lib")

// The undocumented "exit explorer" message, the one the Ctrl+Shift+right click "Exit Explorer" item sends. The
// shell shuts down cleanly on it, saving its state, which a plain termination would not give it.
#define WM_SHELL_EXIT 0x5B4

// One restart at a time, across processes: the settings window in the shell, the rundll32 helper and a settings
// window living in that helper can all ask, and a second request while one is under way would pull the rug
// from under the first.
#define SP_RESTART_MUTEX L"Local\\" _T(PRODUCT_NAME) L".RestartExplorer"

// ---------------------------------------------------------------------------------------------------------------
// Finding things
// ---------------------------------------------------------------------------------------------------------------

BOOL IsThisProcessTheShell(void)
{
    wchar_t wszPath[MAX_PATH];
    if (!GetModuleFileNameW(NULL, wszPath, MAX_PATH))
    {
        return FALSE;
    }
    PathStripPathW(wszPath);
    return _wcsicmp(wszPath, L"explorer.exe") == 0;
}

// The explorer.exe processes of this session, in snapshot order. Returns how many were written to `pids`.
static int ListSessionExplorers(DWORD* pids, int max)
{
    DWORD thisSession = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &thisSession);

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return 0;
    }

    int n = 0;
    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (_wcsicmp(entry.szExeFile, L"explorer.exe") != 0 || entry.th32ProcessID == GetCurrentProcessId())
            {
                continue;
            }
            DWORD session = 0;
            if (ProcessIdToSessionId(entry.th32ProcessID, &session) && session == thisSession && n < max)
            {
                pids[n++] = entry.th32ProcessID;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return n;
}

static BOOL PidInList(DWORD pid, const DWORD* pids, int n)
{
    for (int i = 0; i < n; ++i)
    {
        if (pids[i] == pid)
        {
            return TRUE;
        }
    }
    return FALSE;
}

// An explorer.exe of this session that is not in `known`: one that has appeared since that list was taken.
static DWORD FindNewExplorer(const DWORD* known, int nKnown)
{
    DWORD now[64];
    int n = ListSessionExplorers(now, ARRAYSIZE(now));
    for (int i = 0; i < n; ++i)
    {
        if (!PidInList(now[i], known, nKnown))
        {
            return now[i];
        }
    }
    return 0;
}

DWORD FindShellProcessId(void)
{
    // The taskbar belongs to the shell, so its owner is the answer when there is one.
    HWND hTray = FindWindowW(L"Shell_TrayWnd", NULL);
    if (hTray)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(hTray, &pid);
        if (pid)
        {
            return pid;
        }
    }

    // No taskbar yet (the shell is still coming up) or none at all: fall back to the first explorer.exe of this
    // session. Another session's explorer is someone else's desktop.
    DWORD pids[1];
    return ListSessionExplorers(pids, 1) ? pids[0] : 0;
}

int FindEngineInShell(wchar_t* path, DWORD cch)
{
    if (path && cch)
    {
        path[0] = 0;
    }

    DWORD pid = FindShellProcessId();
    if (!pid)
    {
        return SP_ENGINE_MISSING;
    }

    wchar_t wszProxy[MAX_PATH];
    wszProxy[0] = 0;
    if (GetWindowsDirectoryW(wszProxy, MAX_PATH))
    {
        PathAppendW(wszProxy, L"dxgi.dll");
    }

    // The snapshot can fail while the shell is still starting; asking again a moment later usually works.
    HANDLE snapshot = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 5 && snapshot == INVALID_HANDLE_VALUE; ++attempt)
    {
        snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            Sleep(100);
        }
    }
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return SP_ENGINE_MISSING;
    }

    int result = SP_ENGINE_MISSING;
    MODULEENTRY32W entry;
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry))
    {
        do
        {
            if (_wcsicmp(entry.szModule, _T(CORE_DLL_NAME)) == 0)
            {
                result = SP_ENGINE_INJECTED;
                if (path && cch)
                {
                    wcscpy_s(path, cch, entry.szExePath);
                }
                break;
            }
            if (wszProxy[0] && _wcsicmp(entry.szExePath, wszProxy) == 0)
            {
                result = SP_ENGINE_PROXY;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

BOOL IsEngineInSafeMode(void)
{
    DWORD value = 0;
    DWORD cb = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, _T(REGPATH), L"SafeMode", RRF_RT_REG_DWORD, NULL, &value, &cb) != ERROR_SUCCESS)
    {
        return FALSE;
    }
    return value != 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Injection
// ---------------------------------------------------------------------------------------------------------------

static BOOL ShellHasModule(DWORD pid, const wchar_t* dllPath)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return FALSE;
    }

    BOOL found = FALSE;
    MODULEENTRY32W entry;
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry))
    {
        do
        {
            if (_wcsicmp(entry.szExePath, dllPath) == 0)
            {
                found = TRUE;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

BOOL InjectIntoShell(const wchar_t* dllPath)
{
    if (!dllPath || !dllPath[0] || !FileExistsW(dllPath))
    {
        return FALSE;
    }

    DWORD pid = FindShellProcessId();
    if (!pid)
    {
        return FALSE;
    }

    if (ShellHasModule(pid, dllPath))
    {
        return TRUE;    // already there
    }

    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                                     PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
                                 FALSE, pid);
    if (!process)
    {
        return FALSE;
    }

    BOOL result = FALSE;

    // Only the path string is written into the shell; the code that runs is its own LoadLibraryW, which sits at
    // the same address in every process of the session.
    SIZE_T bytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote && WriteProcessMemory(process, remote, dllPath, bytes, NULL))
    {
        LPTHREAD_START_ROUTINE loadLibrary =
            (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        HANDLE thread = loadLibrary ? CreateRemoteThread(process, NULL, 0, loadLibrary, remote, 0, NULL) : NULL;
        if (thread)
        {
            if (WaitForSingleObject(thread, 15000) == WAIT_OBJECT_0)
            {
                result = ShellHasModule(pid, dllPath);
            }
            CloseHandle(thread);
        }
    }

    if (remote)
    {
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    }
    CloseHandle(process);
    return result;
}

// ---------------------------------------------------------------------------------------------------------------
// Restarting
//
// The shape of a restart, and why each step is there:
//
//   1. The shell is asked to exit with WM_SHELL_EXIT and given a generous while to do so. A shell that is busy
//      (a mod holding its taskbar thread, a folder operation) takes longer than the second or two it usually
//      needs, and terminating it instead has a cost: Winlogon sees a shell that died and starts another one
//      by itself, without the engine.
//   2. Only if it will not leave is it terminated. Then Winlogon's own restart is waited for rather than raced:
//      two explorer.exe starting at once leave one of them without a taskbar. Whatever shell appears gets the
//      engine injected late.
//   3. When nothing brought a shell back, one is started here, with the engine loaded before its first
//      instruction so that it is in place before the taskbar is built.
//   4. If the taskbar is still missing after all that, whatever explorer.exe is there is not going to become
//      the shell; it is terminated and one more start is made.
// ---------------------------------------------------------------------------------------------------------------

BOOL WaitForShell(DWORD timeoutMs)
{
    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (!FindWindowW(L"Shell_TrayWnd", NULL))
    {
        if (GetTickCount64() >= deadline)
        {
            return FALSE;
        }
        Sleep(200);
    }
    return TRUE;
}

// Winlogon brings back a shell that died or was terminated, unless the machine has been told not to.
static BOOL WinlogonRestartsShell(void)
{
    DWORD value = 1;
    DWORD cb = sizeof(value);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
                 L"AutoRestartShell", RRF_RT_REG_DWORD, NULL, &value, &cb);
    return value != 0;
}

// Starts explorer.exe as the shell. When `dllToLoad` is given the process is created suspended and the DLL is
// loaded into it before its first instruction runs, so the engine is in place before the taskbar, the tray
// and the desktop are built: the same moment the installed proxy gets, which is what mods that style tray
// icons or taskbar buttons as they are created rely on. Returns FALSE if nothing was started.
static BOOL StartExplorerWith(const wchar_t* dllToLoad)
{
    if (FindWindowW(L"Shell_TrayWnd", NULL))
    {
        // A shell is already up; a second explorer.exe would only open a folder window.
        return FALSE;
    }

    wchar_t wszPath[MAX_PATH];
    GetWindowsDirectoryW(wszPath, MAX_PATH);
    PathAppendW(wszPath, L"explorer.exe");

    // CreateProcess rather than ShellExecute: with no shell running there is nothing for ShellExecute to ask,
    // and explorer.exe started this way with no arguments becomes the shell.
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);

    const BOOL early = (dllToLoad && dllToLoad[0] && FileExistsW(dllToLoad));
    if (!CreateProcessW(wszPath, NULL, NULL, NULL, FALSE, early ? CREATE_SUSPENDED : 0, NULL, NULL, &si, &pi))
    {
        ShellExecuteW(NULL, L"open", wszPath, NULL, NULL, SW_SHOWNORMAL);
        return TRUE;
    }

    if (early)
    {
        // A remote thread in a suspended process initialises the process on its way in, then runs
        // LoadLibraryW; explorer's own main thread is released afterwards.
        SIZE_T bytes = (wcslen(dllToLoad) + 1) * sizeof(wchar_t);
        void* remote = VirtualAllocEx(pi.hProcess, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (remote && WriteProcessMemory(pi.hProcess, remote, dllToLoad, bytes, NULL))
        {
            LPTHREAD_START_ROUTINE loadLibrary =
                (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
            HANDLE thread = loadLibrary ? CreateRemoteThread(pi.hProcess, NULL, 0, loadLibrary, remote, 0, NULL) : NULL;
            if (thread)
            {
                WaitForSingleObject(thread, 15000);
                CloseHandle(thread);
            }
        }
        if (remote)
        {
            VirtualFreeEx(pi.hProcess, remote, 0, MEM_RELEASE);
        }
        ResumeThread(pi.hThread);
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return TRUE;
}

void StartExplorer(void)
{
    StartExplorerWith(NULL);
}

static void TerminateExplorer(DWORD pid)
{
    HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (process)
    {
        TerminateProcess(process, 1);
        WaitForSingleObject(process, 5000);
        CloseHandle(process);
    }
}

// A restart the user asked for is not a crash. The engine counts shell starts to catch a crash loop, and three
// restarts inside a minute would otherwise put it into safe mode with every mod off.
static void ForgiveShellStarts(void)
{
    DWORD zero = 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, _T(REGPATH), L"ShellStartCount", REG_DWORD, &zero, sizeof(zero));
}

// Puts the engine into a shell that came up without it. A few tries, because the shell is still building its
// windows for the first second or two and a snapshot of it can fail meanwhile.
static void InjectLate(const wchar_t* dll)
{
    Sleep(1000);
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        if (InjectIntoShell(dll))
        {
            return;
        }
        Sleep(1000);
    }
}

void RestartExplorerNow(const wchar_t* reinjectDll)
{
    HANDLE hGuard = CreateMutexW(NULL, TRUE, SP_RESTART_MUTEX);
    if (hGuard && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        // Another restart is under way (a second click, or the helper and the window it came from); this one
        // would only get in its way.
        CloseHandle(hGuard);
        return;
    }

    ForgiveShellStarts();

    // Who is there before anything is asked to leave. Explorer processes that appear afterwards are new shells,
    // Winlogon's or ours, and must be left alone.
    DWORD before[64];
    int nBefore = ListSessionExplorers(before, ARRAYSIZE(before));

    DWORD shellPid = FindShellProcessId();
    HANDLE hShell = shellPid ? OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, shellPid) : NULL;

    BOOL terminated = FALSE;
    HWND hTray = FindWindowW(L"Shell_TrayWnd", NULL);
    if (hTray)
    {
        PostMessageW(hTray, WM_SHELL_EXIT, 0, 0);
    }

    if (hShell)
    {
        // The process, not just the taskbar window: the window goes first and the shell saves its state after.
        if (WaitForSingleObject(hShell, hTray ? 20000 : 3000) != WAIT_OBJECT_0)
        {
            TerminateProcess(hShell, 1);
            WaitForSingleObject(hShell, 5000);
            terminated = TRUE;
        }
        CloseHandle(hShell);
    }

    // Folder windows that run in a process of their own, and a shell that could not be opened above. Only the
    // ones that were there at the start.
    for (int i = 0; i < nBefore; ++i)
    {
        if (before[i] != shellPid)
        {
            TerminateExplorer(before[i]);
        }
    }

    // A terminated shell is one Winlogon brings back on its own. That one gets a moment to appear; starting
    // another here at the same time would leave two explorers fighting over the taskbar.
    if (terminated && WinlogonRestartsShell())
    {
        ULONGLONG deadline = GetTickCount64() + 10000;
        while (!FindNewExplorer(before, nBefore) && GetTickCount64() < deadline)
        {
            Sleep(200);
        }
    }
    else
    {
        // After a clean exit nothing brings the shell back but us; still, a moment for the process to be gone
        // from the process list so that it is not mistaken for a new one.
        Sleep(500);
    }

    BOOL startedHere = FALSE;
    if (!FindNewExplorer(before, nBefore))
    {
        startedHere = StartExplorerWith(reinjectDll);
    }

    if (!WaitForShell(30000))
    {
        // Half a minute and no taskbar. An explorer.exe that is there is not becoming the shell, or it is
        // stuck; it goes, and one more start is made. If that fails too there is nothing more to be done from
        // here, and the user still has Task Manager.
        DWORD stuck;
        while ((stuck = FindNewExplorer(before, nBefore)) != 0)
        {
            TerminateExplorer(stuck);
            // Whatever we terminated is now "known", so the loop ends.
            if (nBefore < ARRAYSIZE(before))
            {
                before[nBefore++] = stuck;
            }
            else
            {
                break;
            }
        }
        Sleep(1500);
        startedHere = StartExplorerWith(reinjectDll);
        if (!WaitForShell(30000))
        {
            if (hGuard)
            {
                ReleaseMutex(hGuard);
                CloseHandle(hGuard);
            }
            return;
        }
    }

    if (reinjectDll && reinjectDll[0] && !startedHere)
    {
        // The shell came back on its own, so the engine could not be loaded ahead of it; it goes in late,
        // which is enough for every mod that hooks rather than styles at creation.
        InjectLate(reinjectDll);
    }

    if (hGuard)
    {
        ReleaseMutex(hGuard);
        CloseHandle(hGuard);
    }
}

BOOL IsExplorerRestartInProgress(void)
{
    HANDLE hGuard = OpenMutexW(SYNCHRONIZE, FALSE, SP_RESTART_MUTEX);
    if (!hGuard)
    {
        return FALSE;
    }
    CloseHandle(hGuard);
    return TRUE;
}

// The path of the core DLL as loaded in this process: ShadePatcher.dll from a folder, or the proxy copy named
// dxgi.dll in the Windows folder. Both export the helper entry points.
static BOOL FindCoreModulePath(wchar_t* path, DWORD cch)
{
    static const wchar_t* const kNames[] = { _T(CORE_DLL_NAME), L"dxgi.dll" };
    for (int i = 0; i < ARRAYSIZE(kNames); ++i)
    {
        HMODULE hModule = GetModuleHandleW(kNames[i]);
        if (hModule && GetProcAddress(hModule, "ZZRestartExplorer") && GetModuleFileNameW(hModule, path, cch))
        {
            return TRUE;
        }
    }
    // Not loaded here (the settings window can be opened from sp_gui.dll alone): the copy next to us or in the
    // install folder.
    return FindProductFile(_T(CORE_DLL_NAME), path, cch);
}

// What goes back into the shell after a restart: an injected build is injected again, the proxy needs nothing,
// and a shell that had no engine gets the build next to us unless the proxy is installed (in which case the
// new shell loads it by itself). Decided before the shell goes away, while the answer can still be read off it.
static void DecideReinject(const wchar_t* coreDll, wchar_t* reinject, DWORD cch)
{
    reinject[0] = 0;

    wchar_t wszEngine[MAX_PATH];
    int mode = FindEngineInShell(wszEngine, ARRAYSIZE(wszEngine));

    if (mode == SP_ENGINE_INJECTED)
    {
        wcscpy_s(reinject, cch, wszEngine);
    }
    else if (mode == SP_ENGINE_MISSING && coreDll && coreDll[0])
    {
        wchar_t wszProxy[MAX_PATH];
        GetWindowsDirectoryW(wszProxy, MAX_PATH);
        PathAppendW(wszProxy, L"dxgi.dll");
        if (!FileExistsW(wszProxy) && _wcsicmp(coreDll, wszProxy) != 0)
        {
            wcscpy_s(reinject, cch, coreDll);
        }
    }
}

static DWORD WINAPI RestartThread(LPVOID lpParameter)
{
    wchar_t* reinject = (wchar_t*)lpParameter;
    RestartExplorerNow(reinject);
    HeapFree(GetProcessHeap(), 0, reinject);
    return 0;
}

void RestartExplorer(void)
{
    if (IsExplorerRestartInProgress())
    {
        return;
    }

    wchar_t wszCore[MAX_PATH];
    BOOL haveCore = FindCoreModulePath(wszCore, ARRAYSIZE(wszCore));

    if (IsThisProcessTheShell())
    {
        // Everything below would die with the shell, so a helper does it. rundll32 runs as the user, not
        // elevated, so the new shell comes up with ordinary rights.
        if (haveCore)
        {
            wchar_t wszCommand[MAX_PATH * 2];
            swprintf_s(wszCommand, ARRAYSIZE(wszCommand), L"rundll32.exe \"%s\",ZZRestartExplorer reopen", wszCore);

            STARTUPINFOW si;
            PROCESS_INFORMATION pi;
            ZeroMemory(&si, sizeof(si));
            ZeroMemory(&pi, sizeof(pi));
            si.cb = sizeof(si);
            if (CreateProcessW(NULL, wszCommand, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
            {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                return;
            }
        }

        // No helper available. The shell is still asked to leave; the user gets it back with Task Manager. This
        // is the pre-existing behaviour and only reachable when the product files are missing.
        HWND hShellTrayWnd = FindWindowW(L"Shell_TrayWnd", NULL);
        if (hShellTrayWnd)
        {
            PostMessageW(hShellTrayWnd, WM_SHELL_EXIT, 0, 0);
        }
        return;
    }

    // Outside the shell the restart can be done right here, on a thread of its own so that a settings window
    // calling this keeps painting (and keeps showing the engine's status) while the shell is away.
    wchar_t* reinject = (wchar_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, MAX_PATH * sizeof(wchar_t));
    if (!reinject)
    {
        return;
    }
    DecideReinject(haveCore ? wszCore : NULL, reinject, MAX_PATH);

    HANDLE thread = CreateThread(NULL, 0, RestartThread, reinject, 0, NULL);
    if (thread)
    {
        CloseHandle(thread);
    }
    else
    {
        RestartThread(reinject);
    }
}
