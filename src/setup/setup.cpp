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
// explorer.exe holds C:\Windows\dxgi.dll open, so it has to go away before that file can be written or deleted.
// The shell is terminated rather than asked to exit: Windows restarts a terminated shell by itself, in the
// user's own session and without administrator rights, which is exactly what is wanted. Asking it to exit
// politely suppresses that restart and would leave the user with no desktop.
//
// The restart takes a moment, so the file operations retry for a few seconds to catch the window while nothing
// holds the file. If that still fails, the change is queued for the next reboot and the user is told.
//
#include <Windows.h>
#include <Shlwapi.h>
#include <TlHelp32.h>
#include <shellapi.h>
#include <tchar.h>

#include <string>

#include "config.h"
#include "version.h"
#include "shortcut.h"

#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "Shell32.lib")

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

// Terminates every explorer.exe in this session. Windows brings the shell back on its own, unelevated.
void StopShell()
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
            if (_wcsicmp(entry.szExeFile, L"explorer.exe") != 0)
            {
                continue;
            }

            // Only this user's shell. Terminating another session's would be someone else's desktop.
            DWORD session = 0;
            if (!ProcessIdToSessionId(entry.th32ProcessID, &session) || session != thisSession)
            {
                continue;
            }

            HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, entry.th32ProcessID);
            if (process)
            {
                TerminateProcess(process, 1);
                WaitForSingleObject(process, 5000);
                CloseHandle(process);
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
}

// Waits for Windows to bring the shell back. If it does not, the user is told how to start it themselves:
// launching explorer from here would inherit this program's administrator rights and leave them with an
// elevated desktop, which is worse than a missing one.
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

// ---------------------------------------------------------------------------------------------------------------
// File operations that have to win a race against the shell restarting
// ---------------------------------------------------------------------------------------------------------------

constexpr DWORD kRetryForMs = 8000;
constexpr DWORD kRetryEveryMs = 150;

bool CopyWithRetry(const std::wstring& from, const std::wstring& to, DWORD* lastError)
{
    ULONGLONG deadline = GetTickCount64() + kRetryForMs;
    for (;;)
    {
        if (CopyFileW(from.c_str(), to.c_str(), FALSE))
        {
            return true;
        }
        *lastError = GetLastError();

        // Anything other than the file being held is not going to improve by waiting.
        if (*lastError != ERROR_SHARING_VIOLATION && *lastError != ERROR_ACCESS_DENIED &&
            *lastError != ERROR_USER_MAPPED_FILE)
        {
            return false;
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
        if (*lastError != ERROR_SHARING_VIOLATION && *lastError != ERROR_ACCESS_DENIED &&
            *lastError != ERROR_USER_MAPPED_FILE)
        {
            return false;
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

    // The shell is what holds the proxy open, so it goes first. On a first install nothing is held and this only
    // costs the user a shell restart they would need anyway for the engine to start.
    bool shellWasRunning = IsShellRunning();
    if (shellWasRunning)
    {
        StopShell();
    }

    if (!CreateDirectoryW(target.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
    {
        DWORD error = GetLastError();
        Say(L"The install folder could not be created:\n" + target + L"\n\n" + DescribeError(error), MB_ICONERROR);
        WaitForShell(15000);
        return 1;
    }

    for (const Payload& item : kPayload)
    {
        DWORD error = 0;
        if (!PlacePayload(item, source, target, &error))
        {
            Say(std::wstring(L"Could not write ") + item.name + L":\n" + DescribeError(error), MB_ICONERROR);
            WaitForShell(15000);
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
        WaitForShell(15000);
        return 1;
    }

    WriteUninstallEntry(target);

    // Start search should find the settings. The all-users entry is made here (this program is elevated); a
    // per-user one a previous build-folder session may have left is removed so there is exactly one.
    RemoveSettingsShortcuts();
    CreateSettingsShortcut((target + L"\\" _T(CORE_DLL_NAME)).c_str(), TRUE);

    // A shell started before the proxy landed is running unpatched, so it is sent round once more.
    if (IsShellRunning())
    {
        StopShell();
    }
    WaitForShell(20000);

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

    if (IsShellRunning())
    {
        StopShell();
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

    WaitForShell(20000);

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
