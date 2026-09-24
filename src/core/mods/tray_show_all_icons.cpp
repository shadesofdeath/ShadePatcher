//
// tray_show_all_icons - always show every notification area icon on the Windows 11 taskbar.
//
// Adapted from the idea behind the Windhawk mod "Always show all taskbar tray icons"
// (taskbar-notification-icons-show-all) by m417z. The implementation here is written against this engine's API.
//
// How it works
// ------------
// Windows 11 keeps one registry key per tray icon under
//     HKCU\Control Panel\NotifyIconSettings\<icon id>
// and decides whether an icon is shown from the "IsPromoted" value in it. The taskbar reads that value through
// RegGetValueW and writes it through RegSetValueExW when the user drags an icon in or out of the overflow.
//
// So two hooks are enough, and neither of them changes anything on disk:
//   * reading IsPromoted returns what this mod's mode says, not what is stored;
//   * writing IsPromoted is swallowed, so the taskbar cannot undo the mod.
//
// Because nothing is written, turning the mod off restores exactly the layout the user had before.
//
// The taskbar caches the value, so after changing the answer the mod nudges every icon key to make the taskbar
// re-read it. That is what MakeTaskbarReread does.
//
#define SP_MOD_ID "tray-show-all-icons"
#include "engine/modapi.h"

#include <winternl.h>

#include <atomic>
#include <string>
#include <vector>

namespace {

// What the mod reports for IsPromoted.
enum class Mode
{
    ShowAll = 0,   // every icon is shown
    ShowNew = 1,   // icons the user has never arranged are shown; existing choices are kept
    HideAll = 2,   // every icon is hidden in the overflow
};

// Read on the engine thread, used on whatever thread the taskbar calls the registry from.
std::atomic<Mode> g_mode{ Mode::ShowAll };

constexpr wchar_t kValueName[] = L"IsPromoted";
constexpr wchar_t kSettingsKey[] = L"Control Panel\\NotifyIconSettings";

// ---------------------------------------------------------------------------------------------------------------
// Recognising the key a call is about
//
// The hooks are handed an open HKEY, not a path, so the path has to be recovered from the handle. NtQueryKey
// with KeyNameInformation gives the full object path:
//     \REGISTRY\USER\<sid>\Control Panel\NotifyIconSettings\<icon id>
// ---------------------------------------------------------------------------------------------------------------

using NtQueryKey_t = NTSTATUS(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);

NtQueryKey_t GetNtQueryKey()
{
    static NtQueryKey_t pfn = []() -> NtQueryKey_t {
        HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
        return hNtdll ? (NtQueryKey_t)GetProcAddress(hNtdll, "NtQueryKey") : nullptr;
    }();
    return pfn;
}

std::wstring GetKeyPath(HKEY key)
{
    // A predefined handle such as HKEY_CURRENT_USER has its high bit set and is not a real object.
    if (!key || ((ULONG_PTR)key & 0x80000000) == 0x80000000)
    {
        return {};
    }

    NtQueryKey_t pNtQueryKey = GetNtQueryKey();
    if (!pNtQueryKey)
    {
        return {};
    }

    constexpr int kKeyNameInformation = 3;
    constexpr NTSTATUS kBufferTooSmall = (NTSTATUS)0xC0000023L;    // STATUS_BUFFER_TOO_SMALL

    ULONG size = 0;
    if (pNtQueryKey(key, kKeyNameInformation, nullptr, 0, &size) != kBufferTooSmall || size < sizeof(ULONG))
    {
        return {};
    }

    std::vector<BYTE> buffer(size);
    if (pNtQueryKey(key, kKeyNameInformation, buffer.data(), size, &size) != 0 || size < sizeof(ULONG))
    {
        return {};
    }

    // KEY_NAME_INFORMATION: ULONG NameLength followed by the characters.
    ULONG nameBytes = *(const ULONG*)buffer.data();
    if (nameBytes == 0 || size - sizeof(ULONG) < nameBytes)
    {
        return {};
    }

    return std::wstring((const wchar_t*)(buffer.data() + sizeof(ULONG)), nameBytes / sizeof(wchar_t));
}

// TRUE when the handle is one of the per-icon keys. The icon id itself is not needed for the decision, only for
// the log line, so it is returned through `iconId`.
bool IsNotifyIconKey(HKEY key, std::wstring* iconId)
{
    std::wstring path = GetKeyPath(key);
    if (path.empty())
    {
        return false;
    }

    // Match on the tail so the user's SID never has to be worked out.
    //   ...\Control Panel\NotifyIconSettings\<icon id>
    static const std::wstring needle = std::wstring(L"\\") + kSettingsKey + L"\\";

    size_t at = path.rfind(needle);
    if (at == std::wstring::npos)
    {
        return false;
    }

    std::wstring tail = path.substr(at + needle.size());
    // The tail must be the icon id alone; a deeper key is something else.
    if (tail.empty() || tail.find(L'\\') != std::wstring::npos)
    {
        return false;
    }

    if (iconId)
    {
        *iconId = tail;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// The hooks
// ---------------------------------------------------------------------------------------------------------------

using RegGetValueW_t = decltype(&RegGetValueW);
using RegSetValueExW_t = decltype(&RegSetValueExW);

RegGetValueW_t   g_origRegGetValueW = nullptr;
RegSetValueExW_t g_origRegSetValueExW = nullptr;

LONG WINAPI RegGetValueW_Hook(HKEY hkey, LPCWSTR lpSubKey, LPCWSTR lpValue, DWORD dwFlags,
                              LPDWORD pdwType, PVOID pvData, LPDWORD pcbData)
{
    // Only the taskbar's own read of IsPromoted directly on an icon key is answered; anything else, including a
    // read that only asks for the size, goes straight through.
    const bool isOurRead =
        !lpSubKey && lpValue && pvData && pcbData && *pcbData >= sizeof(DWORD) &&
        (dwFlags & RRF_RT_REG_DWORD) && _wcsicmp(lpValue, kValueName) == 0;

    if (!isOurRead)
    {
        return g_origRegGetValueW(hkey, lpSubKey, lpValue, dwFlags, pdwType, pvData, pcbData);
    }

    std::wstring iconId;
    if (!IsNotifyIconKey(hkey, &iconId))
    {
        return g_origRegGetValueW(hkey, lpSubKey, lpValue, dwFlags, pdwType, pvData, pcbData);
    }

    const Mode mode = g_mode.load(std::memory_order_relaxed);

    if (mode == Mode::ShowNew)
    {
        // Respect a choice the user has already made: only answer when nothing is stored yet.
        LONG result = g_origRegGetValueW(hkey, lpSubKey, lpValue, dwFlags, pdwType, pvData, pcbData);
        if (result != ERROR_FILE_NOT_FOUND)
        {
            return result;
        }
    }

    if (pdwType)
    {
        *pdwType = REG_DWORD;
    }
    *(DWORD*)pvData = (mode == Mode::HideAll) ? 0u : 1u;
    *pcbData = sizeof(DWORD);

    SP_LogDebug(L"Answering IsPromoted for %s", iconId.c_str());
    return ERROR_SUCCESS;
}

LONG WINAPI RegSetValueExW_Hook(HKEY hKey, LPCWSTR lpValueName, DWORD Reserved, DWORD dwType,
                                const BYTE* lpData, DWORD cbData)
{
    // In ShowNew mode the taskbar is allowed to record the user's arrangement as usual.
    if (g_mode.load(std::memory_order_relaxed) != Mode::ShowNew &&
        lpValueName && _wcsicmp(lpValueName, kValueName) == 0)
    {
        std::wstring iconId;
        if (IsNotifyIconKey(hKey, &iconId))
        {
            // Reporting success while writing nothing keeps the stored layout untouched, so turning the mod off
            // gives the user back exactly what they had.
            SP_LogDebug(L"Discarding a write to IsPromoted for %s", iconId.c_str());
            return ERROR_SUCCESS;
        }
    }

    return g_origRegSetValueExW(hKey, lpValueName, Reserved, dwType, lpData, cbData);
}

// ---------------------------------------------------------------------------------------------------------------
// Making the taskbar re-read
//
// Each icon key has a registry watcher behind it. Writing a value and deleting it again trips the watcher
// without changing anything that survives the call.
// ---------------------------------------------------------------------------------------------------------------

void MakeTaskbarReread()
{
    constexpr wchar_t kTempValue[] = L"_" SP_MOD_ID "_refresh";

    HKEY hRoot = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_READ, &hRoot) != ERROR_SUCCESS)
    {
        SP_LogDebug(L"There are no notification icon settings to refresh");
        return;
    }

    int touched = 0;
    wchar_t wszName[MAX_PATH];
    DWORD cchName = ARRAYSIZE(wszName);

    for (DWORD index = 0;
         RegEnumKeyExW(hRoot, index, wszName, &cchName, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
         ++index, cchName = ARRAYSIZE(wszName))
    {
        HKEY hIcon = nullptr;
        if (RegOpenKeyExW(hRoot, wszName, 0, KEY_SET_VALUE, &hIcon) != ERROR_SUCCESS)
        {
            continue;
        }

        // The write goes through the original function: the hook only guards IsPromoted, but calling the
        // original keeps this independent of what the hook happens to do.
        static const BYTE empty[sizeof(wchar_t)] = { 0 };
        if (g_origRegSetValueExW(hIcon, kTempValue, 0, REG_SZ, empty, sizeof(empty)) == ERROR_SUCCESS)
        {
            RegDeleteValueW(hIcon, kTempValue);
            touched++;
        }
        RegCloseKey(hIcon);
    }

    RegCloseKey(hRoot);
    SP_Log(L"Refreshed %d notification icon(s)", touched);
}

void LoadSettings()
{
    int mode = SP_GetIntSetting(L"Mode", (int)Mode::ShowAll);
    if (mode < (int)Mode::ShowAll || mode > (int)Mode::HideAll)
    {
        mode = (int)Mode::ShowAll;
    }
    g_mode.store((Mode)mode, std::memory_order_relaxed);
    SP_Log(L"Mode is %d", mode);
}

// ---------------------------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------------------------

BOOL Init()
{
    LoadSettings();

    // The taskbar reaches the registry through kernelbase, so that is where the hooks go. Hooking the advapi32
    // forwarders instead would miss every call that binds to kernelbase directly, which is most of them.
    if (!SP_HookBegin())
    {
        return FALSE;
    }

    if (!SP_SetExportHook(L"kernelbase.dll", "RegGetValueW", RegGetValueW_Hook, &g_origRegGetValueW) ||
        !SP_SetExportHook(L"kernelbase.dll", "RegSetValueExW", RegSetValueExW_Hook, &g_origRegSetValueExW))
    {
        SP_HookAbort();
        SP_LogError(L"The registry functions could not be hooked");
        return FALSE;
    }

    return SP_HookCommit();
}

void AfterInit()
{
    MakeTaskbarReread();
}

void SettingsChanged()
{
    LoadSettings();
    MakeTaskbarReread();
}

void BeforeUninit()
{
    // Still hooked, but the answers stop here: put the mode back to whatever is actually stored by letting the
    // real values through, then make the taskbar read them.
    g_mode.store(Mode::ShowNew, std::memory_order_relaxed);
    MakeTaskbarReread();
}

}   // namespace

SP_MOD_DEFINE(g_modTrayShowAllIcons) =
{
    /* id             */ SP_MOD_ID,
    /* name           */ L"Show all notification area icons",
    /* basedOn        */ "taskbar-notification-icons-show-all",
    /* originalAuthor */ "m417z",
    /* targets        */ SP_TARGET_EXPLORER,
    /* minOsBuild     */ 22000,     // the NotifyIconSettings key is a Windows 11 thing
    /* flags          */ 0,
    /* Init           */ Init,
    /* AfterInit      */ AfterInit,
    /* SettingsChanged*/ SettingsChanged,
    /* BeforeUninit   */ BeforeUninit,
    /* Uninit         */ nullptr,
};
