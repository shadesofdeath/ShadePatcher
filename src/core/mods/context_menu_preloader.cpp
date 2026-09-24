//
// context-menu-preloader - make the first right click as fast as the rest.
//
// Adapted from the idea behind the Windhawk mod "Context Menu Preloader" (context-menu-preloader) by Lockframe.
// The implementation here is written against this engine's API.
//
// The first right click in a session is slow because the shell only loads the context menu handler DLLs at that
// moment: archivers, editors, cloud storage clients and so on, each one a separate disk read. Afterwards they
// are in memory and the menu appears instantly.
//
// This mod does that loading once, on a background thread, shortly after the shell starts. The handlers are the
// same ones the shell would have loaded anyway; only the timing changes, from "while the user is waiting" to
// "while the machine is idle".
//
// The library handles are deliberately never released: keeping them loaded is the entire point.
//
#define SP_MOD_ID "context-menu-preloader"
#include "engine/modapi.h"

#include <atomic>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

std::atomic<int> g_maxHandlers{ 64 };
std::atomic<int> g_delaySeconds{ 10 };

HANDLE g_thread = nullptr;
HANDLE g_stopEvent = nullptr;

// The shell looks for context menu handlers under these classes. Together they cover the handlers that appear on
// a file, a folder, the folder background and a drive.
const wchar_t* const kHandlerRoots[] =
{
    L"*\\shellex\\ContextMenuHandlers",
    L"AllFilesystemObjects\\shellex\\ContextMenuHandlers",
    L"Directory\\shellex\\ContextMenuHandlers",
    L"Directory\\Background\\shellex\\ContextMenuHandlers",
    L"Folder\\shellex\\ContextMenuHandlers",
    L"Drive\\shellex\\ContextMenuHandlers",
};

// Reads a key's default value.
std::wstring ReadDefaultValue(HKEY hRoot, const std::wstring& subKey)
{
    DWORD cb = 0;
    if (RegGetValueW(hRoot, subKey.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, nullptr, &cb) != ERROR_SUCCESS ||
        cb <= sizeof(wchar_t))
    {
        return {};
    }

    std::wstring value(cb / sizeof(wchar_t), L'\0');
    if (RegGetValueW(hRoot, subKey.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, value.data(), &cb) != ERROR_SUCCESS)
    {
        return {};
    }
    value.resize(wcslen(value.c_str()));
    return value;
}

// A handler key is named either after its CLSID or after the product, in which case the CLSID is its default
// value. Both spellings occur in practice.
std::wstring ClsidOf(const std::wstring& rootPath, const std::wstring& keyName)
{
    if (!keyName.empty() && keyName.front() == L'{')
    {
        return keyName;
    }

    std::wstring value = ReadDefaultValue(HKEY_CLASSES_ROOT, rootPath + L"\\" + keyName);
    if (!value.empty() && value.front() == L'{')
    {
        return value;
    }
    return {};
}

// Collects the DLL path behind a CLSID, with environment variables expanded.
std::wstring DllPathOf(const std::wstring& clsid)
{
    std::wstring path = ReadDefaultValue(HKEY_CLASSES_ROOT, L"CLSID\\" + clsid + L"\\InprocServer32");
    if (path.empty())
    {
        return {};
    }

    // Many entries are written as %SystemRoot%\system32\something.dll.
    wchar_t wszExpanded[MAX_PATH * 2];
    DWORD cch = ExpandEnvironmentStringsW(path.c_str(), wszExpanded, ARRAYSIZE(wszExpanded));
    if (cch == 0 || cch > ARRAYSIZE(wszExpanded))
    {
        return path;
    }
    return wszExpanded;
}

std::vector<std::wstring> CollectHandlerDlls()
{
    std::unordered_set<std::wstring> seenClsids;
    std::unordered_set<std::wstring> seenPaths;
    std::vector<std::wstring> result;

    for (const wchar_t* root : kHandlerRoots)
    {
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(HKEY_CLASSES_ROOT, root, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        {
            continue;
        }

        wchar_t wszName[256];
        DWORD cchName = ARRAYSIZE(wszName);

        for (DWORD index = 0;
             RegEnumKeyExW(hKey, index, wszName, &cchName, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
             ++index, cchName = ARRAYSIZE(wszName))
        {
            std::wstring clsid = ClsidOf(root, wszName);
            if (clsid.empty() || !seenClsids.insert(clsid).second)
            {
                continue;
            }

            std::wstring dll = DllPathOf(clsid);
            if (dll.empty() || !seenPaths.insert(dll).second)
            {
                continue;
            }

            result.push_back(std::move(dll));
        }

        RegCloseKey(hKey);
    }

    return result;
}

DWORD WINAPI PreloadThread(LPVOID)
{
    // Wait first. Loading a dozen DLLs while the shell is still drawing the desktop would compete with exactly
    // the work this mod is meant to make faster.
    if (WaitForSingleObject(g_stopEvent, (DWORD)g_delaySeconds.load(std::memory_order_relaxed) * 1000) != WAIT_TIMEOUT)
    {
        return 0;
    }

    std::vector<std::wstring> dlls = CollectHandlerDlls();
    const int maxHandlers = g_maxHandlers.load(std::memory_order_relaxed);

    int loaded = 0;
    int failed = 0;

    for (const std::wstring& dll : dlls)
    {
        if (loaded >= maxHandlers)
        {
            break;
        }
        if (WaitForSingleObject(g_stopEvent, 0) != WAIT_TIMEOUT)
        {
            break;
        }

        // Already loaded by the shell: nothing to do and nothing to count.
        if (GetModuleHandleW(dll.c_str()))
        {
            continue;
        }

        // The handle is kept by the process on purpose and never freed; that is what keeps the menu fast.
        if (LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH))
        {
            loaded++;
            SP_LogDebug(L"Preloaded %s", dll.c_str());
        }
        else
        {
            failed++;
            SP_LogDebug(L"Could not preload %s: %lu", dll.c_str(), GetLastError());
        }
    }

    SP_Log(L"Preloaded %d handler(s) out of %zu found, %d could not be loaded", loaded, dlls.size(), failed);
    return 0;
}

void LoadSettings()
{
    int maxHandlers = SP_GetIntSetting(L"MaxHandlers", 64);
    if (maxHandlers < 1)
    {
        maxHandlers = 1;
    }
    else if (maxHandlers > 256)
    {
        maxHandlers = 256;
    }
    g_maxHandlers.store(maxHandlers, std::memory_order_relaxed);

    int delay = SP_GetIntSetting(L"DelaySeconds", 10);
    if (delay < 1)
    {
        delay = 1;
    }
    else if (delay > 120)
    {
        delay = 120;
    }
    g_delaySeconds.store(delay, std::memory_order_relaxed);
}

BOOL Init()
{
    LoadSettings();

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stopEvent)
    {
        SP_LogError(L"The stop event could not be created: %lu", GetLastError());
        return FALSE;
    }
    return TRUE;
}

void AfterInit()
{
    g_thread = CreateThread(nullptr, 0, PreloadThread, nullptr, 0, nullptr);
    if (!g_thread)
    {
        SP_LogError(L"The preload thread could not be started: %lu", GetLastError());
    }
}

void Uninit()
{
    if (g_stopEvent)
    {
        SetEvent(g_stopEvent);
    }
    if (g_thread)
    {
        // A LoadLibrary in progress has to finish before the thread can see the stop event, so the wait is
        // generous; it still cannot hang the shell.
        WaitForSingleObject(g_thread, 10000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_stopEvent)
    {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }

    // The preloaded libraries stay: unloading a handler the shell has since started using would be worse than
    // leaving it in memory, and the memory is returned when the shell restarts anyway.
}

}   // namespace

SP_MOD_DEFINE(g_modContextMenuPreloader) =
{
    /* id             */ SP_MOD_ID,
    /* name           */ L"Preload context menu handlers for a faster first right click",
    /* basedOn        */ "context-menu-preloader",
    /* originalAuthor */ "Lockframe",
    /* targets        */ SP_TARGET_EXPLORER,
    /* minOsBuild     */ 0,
    /* flags          */ 0,
    /* Init           */ Init,
    /* AfterInit      */ AfterInit,
    /* SettingsChanged*/ LoadSettings,
    /* BeforeUninit   */ nullptr,
    /* Uninit         */ Uninit,
};
