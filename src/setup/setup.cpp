//
// sp_setup - installs and removes ShadePatcher.
//
// What installing means
// ---------------------
// Windows has no supported way to say "load my code into the shell". The one this product uses is the loader's
// own search order: an executable's directory is searched before System32, explorer.exe lives in C:\Windows, and
// so a file called C:\Windows\dxgi.dll is loaded by explorer in place of the real one. That file is
// ShadePatcher.dll, which starts the mod engine and forwards every DXGI call to System32\dxgi.dll.
//
// The blast radius is small and worth stating plainly: C:\Windows is also on the default search path for other
// programs, but System32 comes before it, so every other process still finds the genuine dxgi.dll. Only a
// program whose own directory is C:\Windows picks up this one, and in practice that is explorer.exe.
//
// So an install is four things:
//     %ProgramFiles%\ShadePatcher\        the product files, including this installer for later removal
//     C:\Windows\dxgi.dll                 a copy of ShadePatcher.dll, which is what explorer loads
//     an entry under Uninstall            so it appears in Settings and can be removed from there
//     a Start menu shortcut               so that typing the product name into Start finds the settings
//
// The product files are carried inside this executable (RCDATA resources, see setup.rc), so a single
// sp_setup.exe is the whole download. A copy sitting next to the DLLs (the build folder) prefers those, which
// is what makes a freshly built DLL installable without rebuilding the installer.
//
// Restarting the shell
// --------------------
// explorer.exe holds C:\Windows\dxgi.dll open, so the file cannot be overwritten or deleted while the shell runs.
// It can be renamed, though: a held file is moved aside (and deleted at the next restart), which frees its name
// without stopping anything. All files are put in place first and the shell is restarted exactly once, at the
// end, so that it comes back with the new state.
//
// The shell is terminated rather than asked to exit. Winlogon restarts a terminated shell only sometimes (not at
// all when the running explorer was not the one Winlogon started), so this program does not rely on it: when no
// explorer has appeared after a few seconds, it starts one itself as the signed-in user, with the token of that
// user's sihost.exe. Starting it with this program's own token would give the user an elevated desktop.
//
#include <Windows.h>
#include <Shlwapi.h>
#include <TlHelp32.h>
#include <UserEnv.h>
#include <shellapi.h>
#include <tchar.h>

#include <string>

#include "config.h"
#include "version.h"
#include "shortcut.h"

#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Userenv.lib")

namespace {

bool g_quiet = false;

const wchar_t* const kProductTitle = _T(PRODUCT_NAME);

// Where the loader will find the proxy: the Windows directory, next to explorer.exe.
const wchar_t* const kProxyName = L"dxgi.dll";

const wchar_t* const kUninstallKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" _T(PRODUCT_NAME);

// The files placed in the install directory. The installer copies itself too, so an uninstall is possible
// after the build folder is gone. The two DLLs come out of this executable's resources when they are not
// next to it.
constexpr int SP_PAYLOAD_CORE = 101;
constexpr int SP_PAYLOAD_GUI = 102;

struct Payload
{
    const wchar_t* name;
    int resourceId;     // 0: no embedded copy, the file must be next to the installer (the installer itself)
};

const Payload kPayload[] =
{
    { _T(CORE_DLL_NAME), SP_PAYLOAD_CORE },
    { _T(GUI_DLL_NAME), SP_PAYLOAD_GUI },
    { _T(SETUP_UTILITY_NAME), 0 },
};

// ---------------------------------------------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------------------------------------------

void Say(const std::wstring& text, UINT icon = MB_ICONINFORMATION)
{
    if (!g_quiet)
    {
        MessageBoxW(nullptr, text.c_str(), kProductTitle, MB_OK | icon);
    }
    OutputDebugStringW((L"[" + std::wstring(kProductTitle) + L" setup] " + text + L"\n").c_str());
}

std::wstring DescribeError(DWORD error)
{
    wchar_t* buffer = nullptr;
    DWORD cch = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                   FORMAT_MESSAGE_IGNORE_INSERTS,
                               nullptr, error, 0, (LPWSTR)&buffer, 0, nullptr);
    std::wstring text = (cch && buffer) ? std::wstring(buffer, cch) : L"";
    if (buffer)
    {
        LocalFree(buffer);
    }
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n'))
    {
        text.pop_back();
    }
    return text.empty() ? (L"error " + std::to_wstring(error)) : text;
}

// ---------------------------------------------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------------------------------------------

std::wstring DirectoryOfThisProgram()
{
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH))
    {
        return {};
    }
    PathRemoveFileSpecW(path);
    return path;
}

std::wstring InstallDirectory()
{
    wchar_t path[MAX_PATH];
    // The 64-bit Program Files, which is where a 64-bit product belongs.
    if (!GetEnvironmentVariableW(L"ProgramW6432", path, MAX_PATH) &&
        !GetEnvironmentVariableW(L"ProgramFiles", path, MAX_PATH))
    {
        return {};
    }
    return std::wstring(path) + _T(APP_RELATIVE_PATH);
}

std::wstring ProxyPath()
{
    wchar_t path[MAX_PATH];
    UINT cch = GetWindowsDirectoryW(path, MAX_PATH);
    if (cch == 0 || cch >= MAX_PATH)
    {
        return {};
    }
    return std::wstring(path) + L"\\" + kProxyName;
}

bool FileExists(const std::wstring& path)
{
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// The embedded copy of a product file, or an empty view when this build carries none.
struct ResourceView
{
    const BYTE* data = nullptr;
    DWORD size = 0;
};

ResourceView FindPayloadResource(int id)
{
    ResourceView view;
    HRSRC info = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL handle = info ? LoadResource(nullptr, info) : nullptr;
    if (handle)
    {
        view.data = (const BYTE*)LockResource(handle);
        view.size = SizeofResource(nullptr, info);
    }
    return view;
}

// Where a payload entry is copied from. The installer entry is this very program, whatever it has been renamed
// to (a downloaded copy is often called setup.exe); it is still installed as SETUP_UTILITY_NAME.
std::wstring PayloadSource(const Payload& item, const std::wstring& source)
{
    if (item.resourceId == 0)
    {
        wchar_t self[MAX_PATH];
        DWORD cch = GetModuleFileNameW(nullptr, self, MAX_PATH);
        if (cch && cch < MAX_PATH)
        {
            return self;
        }
    }
    return source + L"\\" + item.name;
}

// Whether a payload entry can be produced at all: from the folder this program runs from, or from inside it.
bool PayloadAvailable(const Payload& item, const std::wstring& source)
{
    if (FileExists(PayloadSource(item, source)))
    {
        return true;
    }
    return item.resourceId != 0 && FindPayloadResource(item.resourceId).data != nullptr;
}

// ---------------------------------------------------------------------------------------------------------------
// The shell
// ---------------------------------------------------------------------------------------------------------------

bool IsShellRunning()
{
    return FindWindowW(L"Shell_TrayWnd", nullptr) != nullptr;
}

// Calls `visit` for every process of the given name in this session, until it returns false.
template <typename Visit>
void ForEachProcessInSession(const wchar_t* name, Visit visit)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return;
    }

    DWORD thisSession = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &thisSession);

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (_wcsicmp(entry.szExeFile, name) != 0)
            {
                continue;
            }

            // Only this user's processes. Another session's shell would be someone else's desktop.
            DWORD session = 0;
            if (!ProcessIdToSessionId(entry.th32ProcessID, &session) || session != thisSession)
            {
                continue;
            }

            if (!visit(entry.th32ProcessID))
            {
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
}

// Whether an explorer.exe runs in this session. The process is up well before its taskbar window is.
bool IsShellProcessRunning()
{
    bool found = false;
    ForEachProcessInSession(L"explorer.exe", [&](DWORD) { found = true; return false; });
    return found;
}

// Terminates every explorer.exe in this session.
void StopShell()
{
    ForEachProcessInSession(L"explorer.exe", [](DWORD pid)
    {
        HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
        if (process)
        {
            TerminateProcess(process, 1);
            WaitForSingleObject(process, 5000);
            CloseHandle(process);
        }
        return true;
    });
}

// A primary token of the signed-in user without administrator rights, borrowed from a process that always runs
// that way in an interactive session. Null when none is found.
HANDLE UnelevatedUserToken()
{
    HANDLE result = nullptr;
    const wchar_t* const donors[] = { L"sihost.exe", L"ctfmon.exe", L"taskhostw.exe" };

    for (const wchar_t* donor : donors)
    {
        ForEachProcessInSession(donor, [&](DWORD pid)
        {
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!process)
            {
                return true;
            }

            HANDLE token = nullptr;
            if (OpenProcessToken(process, TOKEN_DUPLICATE | TOKEN_QUERY, &token))
            {
                TOKEN_ELEVATION elevation{};
                DWORD size = 0;
                if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) &&
                    !elevation.TokenIsElevated)
                {
                    DuplicateTokenEx(token, TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY |
                                                TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID,
                                     nullptr, SecurityImpersonation, TokenPrimary, &result);
                }
                CloseHandle(token);
            }
            CloseHandle(process);
            return result == nullptr;
        });

        if (result)
        {
            break;
        }
    }
    return result;
}

// Starts the shell as the signed-in user, unelevated.
bool StartShellAsUser()
{
    HANDLE token = UnelevatedUserToken();
    if (!token)
    {
        OutputDebugStringW(L"[ShadePatcher setup] no unelevated user token to start the shell with\n");
        return false;
    }

    wchar_t windows[MAX_PATH];
    UINT cch = GetWindowsDirectoryW(windows, MAX_PATH);
    if (cch == 0 || cch >= MAX_PATH)
    {
        CloseHandle(token);
        return false;
    }
    std::wstring explorer = std::wstring(windows) + L"\\explorer.exe";

    void* environment = nullptr;
    CreateEnvironmentBlock(&environment, token, FALSE);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.lpDesktop = const_cast<wchar_t*>(L"winsta0\\default");
    PROCESS_INFORMATION pi{};

    BOOL ok = CreateProcessWithTokenW(token, 0, explorer.c_str(), nullptr,
                                      environment ? CREATE_UNICODE_ENVIRONMENT : 0, environment, windows, &si, &pi);
    DWORD error = ok ? 0 : GetLastError();

    if (environment)
    {
        DestroyEnvironmentBlock(environment);
    }
    CloseHandle(token);

    if (!ok)
    {
        OutputDebugStringW((L"[ShadePatcher setup] starting the shell failed: " + DescribeError(error) + L"\n").c_str());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// Waits for the taskbar. Only when even starting the shell ourselves did not bring it back is the user asked to.
void WaitForShell(DWORD timeoutMs)
{
    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (!IsShellRunning() && GetTickCount64() < deadline)
    {
        Sleep(200);
    }

    if (!IsShellRunning())
    {
        Say(L"The desktop has not come back by itself.\n\nPress Ctrl+Shift+Esc to open Task Manager, then "
            L"choose Run new task and enter:\n\nexplorer.exe",
            MB_ICONWARNING);
    }
}

// Stops the shell and brings it back, so that it loads whatever is in the Windows folder now.
void RestartShell()
{
    StopShell();

    // Winlogon may restart it on its own; a second explorer started meanwhile would only find the shell taken and
    // exit, but waiting a moment avoids the extra process.
    ULONGLONG deadline = GetTickCount64() + 3000;
    while (!IsShellProcessRunning() && GetTickCount64() < deadline)
    {
        Sleep(100);
    }
    if (!IsShellProcessRunning())
    {
        StartShellAsUser();
    }

    WaitForShell(20000);
}

// ---------------------------------------------------------------------------------------------------------------
// File operations that have to win a race against the shell restarting
// ---------------------------------------------------------------------------------------------------------------

constexpr DWORD kRetryForMs = 8000;
constexpr DWORD kRetryEveryMs = 150;

bool IsHeldError(DWORD error)
{
    return error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED || error == ERROR_USER_MAPPED_FILE;
}

// A file a process has loaded cannot be overwritten or deleted, but it can be renamed. The held copy is moved to a
// name of its own and deleted at the next restart, which frees the original name at once.
bool MoveAside(const std::wstring& path)
{
    std::wstring aside = path + L".old-" + std::to_wstring(GetTickCount64());
    if (!MoveFileExW(path.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        return false;
    }
    if (!DeleteFileW(aside.c_str()))
    {
        MoveFileExW(aside.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
    return true;
}

bool CopyWithRetry(const std::wstring& from, const std::wstring& to, DWORD* lastError)
{
    bool movedAside = false;
    ULONGLONG deadline = GetTickCount64() + kRetryForMs;
    for (;;)
    {
        if (CopyFileW(from.c_str(), to.c_str(), FALSE))
        {
            return true;
        }
        *lastError = GetLastError();

        // Anything other than the file being held is not going to improve by waiting.
        if (!IsHeldError(*lastError))
        {
            return false;
        }
        if (!movedAside)
        {
            movedAside = true;
            if (MoveAside(to))
            {
                continue;
            }
        }
        if (GetTickCount64() >= deadline)
        {
            return false;
        }
        Sleep(kRetryEveryMs);
    }
}

// Writes the bytes to `to`, retrying while the shell still holds the old file, like CopyWithRetry.
bool WriteWithRetry(const ResourceView& view, const std::wstring& to, DWORD* lastError)
{
    bool movedAside = false;
    ULONGLONG deadline = GetTickCount64() + kRetryForMs;
    for (;;)
    {
        HANDLE file = CreateFileW(to.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            bool ok = WriteFile(file, view.data, view.size, &written, nullptr) && written == view.size;
            *lastError = ok ? 0 : GetLastError();
            CloseHandle(file);
            if (ok)
            {
                return true;
            }
            DeleteFileW(to.c_str());
            return false;
        }
        *lastError = GetLastError();
        if (!IsHeldError(*lastError))
        {
            return false;
        }
        if (!movedAside)
        {
            movedAside = true;
            if (MoveAside(to))
            {
                continue;
            }
        }
        if (GetTickCount64() >= deadline)
        {
            return false;
        }
        Sleep(kRetryEveryMs);
    }
}

// Puts one payload file in place: the copy next to the installer when there is one, else the embedded one.
bool PlacePayload(const Payload& item, const std::wstring& source, const std::wstring& target, DWORD* lastError)
{
    std::wstring from = PayloadSource(item, source);
    std::wstring to = target + L"\\" + item.name;
    if (_wcsicmp(from.c_str(), to.c_str()) == 0)
    {
        return true;    // run from the install folder: already in place
    }
    if (FileExists(from))
    {
        return CopyWithRetry(from, to, lastError);
    }
    ResourceView view = FindPayloadResource(item.resourceId);
    if (!view.data)
    {
        *lastError = ERROR_FILE_NOT_FOUND;
        return false;
    }
    return WriteWithRetry(view, to, lastError);
}

// Returns true when the file is gone, or scheduled to go at the next restart.
bool DeleteWithRetry(const std::wstring& path, bool* needsReboot)
{
    *needsReboot = false;

    ULONGLONG deadline = GetTickCount64() + kRetryForMs;
    for (;;)
    {
        if (DeleteFileW(path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND)
        {
            return true;
        }
        // Held: renaming frees the name now, and the renamed file goes at the next restart.
        if (IsHeldError(GetLastError()) && MoveAside(path))
        {
            *needsReboot = true;
            return true;
        }
        if (GetTickCount64() >= deadline)
        {
            break;
        }
        Sleep(kRetryEveryMs);
    }

    // Still held. Queue it so the machine comes back clean rather than half installed.
    if (MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT))
    {
        *needsReboot = true;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------
// The Uninstall entry
// ---------------------------------------------------------------------------------------------------------------

void WriteUninstallEntry(const std::wstring& installDirectory)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
    {
        return;
    }

    auto setString = [key](const wchar_t* name, const std::wstring& value) {
        RegSetValueExW(key, name, 0, REG_SZ, (const BYTE*)value.c_str(),
                       (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    };
    auto setDword = [key](const wchar_t* name, DWORD value) {
        RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
    };

    std::wstring setupPath = installDirectory + L"\\" _T(SETUP_UTILITY_NAME);

    setString(L"DisplayName", kProductTitle);
    setString(L"DisplayVersion", _T(VER_WITH_DOTS));
    setString(L"Publisher", _T(PRODUCT_PUBLISHER));
    setString(L"InstallLocation", installDirectory);
    setString(L"DisplayIcon", setupPath);
    setString(L"UninstallString", L"\"" + setupPath + L"\" /uninstall");
    setString(L"URLInfoAbout", _T(PRODUCT_URL));
    setDword(L"NoModify", 1);
    setDword(L"NoRepair", 1);

    RegCloseKey(key);
}

// ---------------------------------------------------------------------------------------------------------------
// Install and uninstall
// ---------------------------------------------------------------------------------------------------------------

int Install()
{
    std::wstring source = DirectoryOfThisProgram();
    std::wstring target = InstallDirectory();
    std::wstring proxy = ProxyPath();

    if (source.empty() || target.empty() || proxy.empty())
    {
        Say(L"The install locations could not be determined.", MB_ICONERROR);
        return 1;
    }

    // Everything that will be placed has to be available before anything is changed, so a partial install is
    // not possible from a missing file. The installer itself is the one file that is never embedded.
    for (const Payload& item : kPayload)
    {
        if (!PayloadAvailable(item, source))
        {
            Say(std::wstring(L"This installer needs ") + item.name +
                    L" and neither carries it nor has it next to it.\n\nRun the installer from the build output "
                    L"folder, where all of the product files are.",
                MB_ICONERROR);
            return 1;
        }
    }

    // Nothing is stopped yet: files the shell holds are moved aside, and the shell is restarted once at the end.
    if (!CreateDirectoryW(target.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
    {
        DWORD error = GetLastError();
        Say(L"The install folder could not be created:\n" + target + L"\n\n" + DescribeError(error), MB_ICONERROR);
        return 1;
    }

    for (const Payload& item : kPayload)
    {
        DWORD error = 0;
        if (!PlacePayload(item, source, target, &error))
        {
            Say(std::wstring(L"Could not write ") + item.name + L":\n" + DescribeError(error), MB_ICONERROR);
            return 1;
        }
    }

    // The proxy itself. This is the file that makes explorer load the engine.
    DWORD error = 0;
    if (!CopyWithRetry(target + L"\\" _T(CORE_DLL_NAME), proxy, &error))
    {
        Say(L"Could not write:\n" + proxy + L"\n\n" + DescribeError(error) +
                L"\n\nNothing has been left behind in the Windows folder.",
            MB_ICONERROR);
        return 1;
    }

    WriteUninstallEntry(target);

    // Start search should find the settings. The all-users entry is made here (this program is elevated); a
    // per-user one a previous build-folder session may have left is removed so there is exactly one.
    RemoveSettingsShortcuts();
    CreateSettingsShortcut((target + L"\\" _T(CORE_DLL_NAME)).c_str(), TRUE);

    // The one restart: the shell comes back and loads the proxy.
    RestartShell();

    Say(std::wstring(kProductTitle) + L" is installed.\n\nFile Explorer has been restarted. Right-click the "
        L"taskbar and choose Properties, or run the settings from:\n" + target);
    return 0;
}

int Uninstall()
{
    std::wstring target = InstallDirectory();
    std::wstring proxy = ProxyPath();

    if (target.empty() || proxy.empty())
    {
        Say(L"The install locations could not be determined.", MB_ICONERROR);
        return 1;
    }

    bool needsReboot = false;
    bool anyFailed = false;

    // The proxy first: once it is gone the shell comes back clean even if the rest fails.
    bool queued = false;
    if (!DeleteWithRetry(proxy, &queued))
    {
        anyFailed = true;
    }
    needsReboot = needsReboot || queued;

    RemoveSettingsShortcuts();

    for (const Payload& item : kPayload)
    {
        const wchar_t* name = item.name;
        std::wstring path = target + L"\\" + name;

        // This program is one of the files being removed, so it cannot delete itself while it is running; that
        // one is queued for the next restart.
        if (_wcsicmp(name, _T(SETUP_UTILITY_NAME)) == 0)
        {
            wchar_t self[MAX_PATH];
            if (GetModuleFileNameW(nullptr, self, MAX_PATH) && _wcsicmp(self, path.c_str()) == 0)
            {
                if (MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT))
                {
                    needsReboot = true;
                }
                continue;
            }
        }

        queued = false;
        if (!DeleteWithRetry(path, &queued))
        {
            anyFailed = true;
        }
        needsReboot = needsReboot || queued;
    }

    // Removed only when empty, so anything the user put there is left alone.
    RemoveDirectoryW(target.c_str());

    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, KEY_WOW64_64KEY, 0);

    // The shell still has the engine loaded from the renamed proxy; a restart brings it back without it.
    RestartShell();

    if (anyFailed)
    {
        Say(std::wstring(kProductTitle) + L" was partly removed. Some files were in use and could not be "
            L"deleted.", MB_ICONWARNING);
        return 1;
    }

    // The user's own settings under HKEY_CURRENT_USER are deliberately kept, so reinstalling restores them.
    Say(std::wstring(kProductTitle) + L" has been removed." +
        (needsReboot ? std::wstring(L"\n\nA few files are in use and will disappear at the next restart.")
                     : std::wstring(L"")));
    return needsReboot ? 0 : 0;
}

}   // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    bool uninstall = false;

    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc))
    {
        for (int i = 1; i < argc; ++i)
        {
            if (_wcsicmp(argv[i], L"/uninstall") == 0 || _wcsicmp(argv[i], L"-uninstall") == 0)
            {
                uninstall = true;
            }
            else if (_wcsicmp(argv[i], L"/quiet") == 0 || _wcsicmp(argv[i], L"-quiet") == 0)
            {
                g_quiet = true;
            }
        }
        LocalFree(argv);
    }

    return uninstall ? Uninstall() : Install();
}
