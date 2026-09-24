//
// sp_inject - loads the engine into the running shell for testing, without installing anything.
//
// Why this exists
// ---------------
// Installing puts a copy of the engine at C:\Windows\dxgi.dll. That needs administrator rights, it changes a
// system folder, and undoing it means running the uninstaller. None of that belongs in a development loop where
// the DLL is rebuilt every few minutes.
//
// This tool asks the running explorer.exe to load the DLL directly instead. explorer.exe belongs to the same
// user and runs at the same integrity level as this program, so no elevation is involved.
//
// What it means for safety: nothing on disk changes. If the engine misbehaves, restarting the shell is enough to
// be rid of it, because there is no copy in C:\Windows for the next explorer to pick up. That makes this the
// right way to try a build, and installing the right way to keep one.
//
//     sp_inject            loads ShadePatcher.dll from this folder into the shell
//     sp_inject /eject     unloads it again
//     sp_inject /status    reports whether the shell currently has it loaded
//
#include <Windows.h>
#include <TlHelp32.h>
#include <Shlwapi.h>
#include <stdio.h>

#pragma comment(lib, "Shlwapi.lib")

#define CORE_DLL L"ShadePatcher.dll"

// The shell of this session. Another session's explorer belongs to someone else's desktop.
static DWORD FindShellProcessId(void)
{
    DWORD thisSession = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &thisSession);

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return 0;
    }

    DWORD found = 0;
    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (_wcsicmp(entry.szExeFile, L"explorer.exe") != 0)
            {
                continue;
            }
            DWORD session = 0;
            if (ProcessIdToSessionId(entry.th32ProcessID, &session) && session == thisSession)
            {
                found = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return found;
}

// The full path of the DLL sitting next to this program.
static BOOL GetPayloadPath(wchar_t* path, DWORD cch)
{
    if (!GetModuleFileNameW(NULL, path, cch))
    {
        return FALSE;
    }
    PathRemoveFileSpecW(path);
    PathAppendW(path, CORE_DLL);
    return PathFileExistsW(path);
}

// The base address the DLL is loaded at inside the shell, or NULL when it is not loaded.
static HMODULE FindLoadedModule(DWORD processId, const wchar_t* dllPath)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return NULL;
    }

    HMODULE found = NULL;
    MODULEENTRY32W entry;
    entry.dwSize = sizeof(entry);

    if (Module32FirstW(snapshot, &entry))
    {
        do
        {
            // Compared by full path, so a different build in another folder is not mistaken for this one.
            if (_wcsicmp(entry.szExePath, dllPath) == 0)
            {
                found = entry.hModule;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return found;
}

// Runs one function inside the shell with a single pointer-sized argument, and returns what it returned.
// LoadLibraryW and FreeLibrary both have that shape, which is why this works with no code injected at all:
// only a string is written into the other process.
static BOOL CallInShell(HANDLE process, LPTHREAD_START_ROUTINE function, void* argument, DWORD* exitCode)
{
    HANDLE thread = CreateRemoteThread(process, NULL, 0, function, argument, 0, NULL);
    if (!thread)
    {
        printf("The shell would not start the call: %lu\n", GetLastError());
        return FALSE;
    }

    DWORD wait = WaitForSingleObject(thread, 15000);
    if (wait != WAIT_OBJECT_0)
    {
        printf("The call did not finish in time.\n");
        CloseHandle(thread);
        return FALSE;
    }

    GetExitCodeThread(thread, exitCode);
    CloseHandle(thread);
    return TRUE;
}

static int Inject(void)
{
    wchar_t dllPath[MAX_PATH];
    if (!GetPayloadPath(dllPath, MAX_PATH))
    {
        wprintf(L"%s is not next to this program.\n", CORE_DLL);
        return 1;
    }

    DWORD shellPid = FindShellProcessId();
    if (!shellPid)
    {
        printf("No shell is running in this session.\n");
        return 1;
    }

    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                                     PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
                                 FALSE, shellPid);
    if (!process)
    {
        printf("The shell (pid %lu) could not be opened: %lu\n", shellPid, GetLastError());
        return 1;
    }

    if (FindLoadedModule(shellPid, dllPath))
    {
        wprintf(L"The shell already has this build loaded. Eject it first.\n");
        CloseHandle(process);
        return 1;
    }

    // Only the path string is written; the code that runs is the real LoadLibraryW.
    SIZE_T bytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote)
    {
        printf("No room in the shell for the path: %lu\n", GetLastError());
        CloseHandle(process);
        return 1;
    }

    int result = 1;
    if (WriteProcessMemory(process, remote, dllPath, bytes, NULL))
    {
        // kernel32 sits at the same address in every process of a session, so this program's own LoadLibraryW
        // is the shell's LoadLibraryW.
        LPTHREAD_START_ROUTINE loadLibrary =
            (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");

        DWORD exitCode = 0;
        if (loadLibrary && CallInShell(process, loadLibrary, remote, &exitCode))
        {
            // The exit code is the low half of the HMODULE, so it only says whether the load succeeded.
            if (exitCode != 0 && FindLoadedModule(shellPid, dllPath))
            {
                wprintf(L"Loaded into the shell (pid %lu).\n", shellPid);
                printf("Turn logging on to watch it: set Logging to 1 under HKCU\\Software\\ShadePatcher.\n");
                result = 0;
            }
            else
            {
                printf("The shell rejected the DLL.\n");
            }
        }
    }
    else
    {
        printf("The path could not be written into the shell: %lu\n", GetLastError());
    }

    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);
    return result;
}

static int Eject(void)
{
    wchar_t dllPath[MAX_PATH];
    if (!GetPayloadPath(dllPath, MAX_PATH))
    {
        wprintf(L"%s is not next to this program.\n", CORE_DLL);
        return 1;
    }

    DWORD shellPid = FindShellProcessId();
    if (!shellPid)
    {
        printf("No shell is running in this session.\n");
        return 1;
    }

    HMODULE module = FindLoadedModule(shellPid, dllPath);
    if (!module)
    {
        printf("The shell does not have this build loaded.\n");
        return 0;
    }

    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_READ |
                                     PROCESS_QUERY_INFORMATION,
                                 FALSE, shellPid);
    if (!process)
    {
        printf("The shell could not be opened: %lu\n", GetLastError());
        return 1;
    }

    LPTHREAD_START_ROUTINE freeLibrary =
        (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "FreeLibrary");

    DWORD exitCode = 0;
    int result = 1;
    if (freeLibrary && CallInShell(process, freeLibrary, module, &exitCode))
    {
        // The engine removes its hooks on DLL_PROCESS_DETACH, so this is an orderly unload rather than a
        // yank: it asks the engine to stop first and the module goes when its last reference does.
        Sleep(500);
        if (!FindLoadedModule(shellPid, dllPath))
        {
            printf("Unloaded from the shell.\n");
            result = 0;
        }
        else
        {
            printf("The shell still holds the DLL; restart the shell to be rid of it.\n");
        }
    }

    CloseHandle(process);
    return result;
}

static int Status(void)
{
    wchar_t dllPath[MAX_PATH];
    if (!GetPayloadPath(dllPath, MAX_PATH))
    {
        wprintf(L"%s is not next to this program.\n", CORE_DLL);
        return 1;
    }

    DWORD shellPid = FindShellProcessId();
    if (!shellPid)
    {
        printf("No shell is running in this session.\n");
        return 1;
    }

    HMODULE module = FindLoadedModule(shellPid, dllPath);
    wprintf(L"shell pid %lu: %s\n", shellPid, module ? L"loaded" : L"not loaded");
    return module ? 0 : 2;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc >= 2)
    {
        if (_wcsicmp(argv[1], L"/eject") == 0 || _wcsicmp(argv[1], L"-eject") == 0)
        {
            return Eject();
        }
        if (_wcsicmp(argv[1], L"/status") == 0 || _wcsicmp(argv[1], L"-status") == 0)
        {
            return Status();
        }
        printf("Usage: sp_inject [/eject | /status]\n");
        return 1;
    }

    return Inject();
}
