//
// symbolprobe - asks the engine's symbol layer to find real shell functions on this machine.
//
// Why this exists
// ---------------
// A mod that hooks an undocumented function is only worth writing if the function still has that name in the
// current Windows build. Microsoft renames, inlines and moves these things between releases, and the only way to
// know is to look in the PDB.
//
// Running this before writing a mod answers that in a few seconds, and it exercises the symbol engine against
// the real shell modules, which are far larger than anything the self-test covers.
//
// Nothing is hooked and nothing is installed: each module is mapped read-only as an image, so none of its
// initialisation runs. A module mapped that way is not in the process module list, which is why the path is
// handed to the symbol engine rather than recovered from the handle.
//
#include <Windows.h>
#include <stdio.h>

#include "engine/log.h"
#include "engine/symbols.h"

#include <Shlwapi.h>
#pragma comment(lib, "Shlwapi.lib")

// The shell is spread across three places on Windows 11, and which one holds a given DLL has changed between
// releases, so all of them are searched.
static const wchar_t* const kSearchFolders[] =
{
    L"%SystemRoot%\\System32",
    L"%SystemRoot%\\SystemApps\\MicrosoftWindows.Client.Core_cw5n1h2txyewy",
    L"%SystemRoot%",
};

// One function a queued mod needs, and the modules it might live in.
typedef struct Probe
{
    const wchar_t* mod;
    const wchar_t* const* modules;
    size_t moduleCount;
    const wchar_t* symbol;
} Probe;

static const wchar_t* const kExplorerFrame[] = { L"ExplorerFrame.dll" };
static const wchar_t* const kTaskbar[] = { L"Taskbar.View.dll", L"explorer.exe" };
static const wchar_t* const kSystemTray[] = { L"SystemTray.dll", L"Taskbar.View.dll" };
static const wchar_t* const kTwinui[] = { L"twinui.pcshell.dll" };

static const Probe kProbes[] =
{
    {
        L"explorer-double-click-up", kExplorerFrame, ARRAYSIZE(kExplorerFrame),
        L"long __cdecl FileCabinet_CreateViewWindow2(struct IShellBrowser *,struct tagFolderSetDataBase *,"
        L"struct IShellView *,struct IShellView *,struct tagRECT *,struct HWND__ * *)"
    },
    {
        L"taskbar-wheel-cycle", kTaskbar, ARRAYSIZE(kTaskbar),
        L"public: virtual int __cdecl CTaskListWnd::GetButtonGroupCount(void)"
    },
    {
        L"taskbar-wheel-cycle", kTaskbar, ARRAYSIZE(kTaskbar),
        L"protected: struct ITaskBtnGroup * __cdecl CTaskListWnd::_GetTBGroupFromGroup(struct ITaskGroup *,int *)"
    },
    {
        L"taskbar-volume-control", kSystemTray, ARRAYSIZE(kSystemTray),
        L"public: void __cdecl winrt::SystemTray::implementation::VolumeSystemTrayIconDataModel::OnIconClicked("
        L"struct winrt::SystemTray::IconClickedEventArgs const &)"
    },
    {
        L"taskbar-volume-control", kSystemTray, ARRAYSIZE(kSystemTray),
        L"public: virtual int __cdecl winrt::impl::produce<struct winrt::SystemTray::implementation::"
        L"VolumeSystemTrayIconDataModel,struct winrt::SystemTray::IIconDataModel>::OnIconClicked(void *)"
    },
    {
        L"taskbar context menu", kTwinui, ARRAYSIZE(kTwinui),
        L"private: void __cdecl CLauncherTipContextMenu::_ExecuteCommand(int)"
    },
};

// Finds the module and maps it as an image without running any of its code.
static HMODULE MapImage(const wchar_t* name, wchar_t* pathOut, DWORD cchPathOut)
{
    for (size_t i = 0; i < ARRAYSIZE(kSearchFolders); ++i)
    {
        wchar_t wszFolder[MAX_PATH];
        if (!ExpandEnvironmentStringsW(kSearchFolders[i], wszFolder, MAX_PATH))
        {
            continue;
        }

        wchar_t wszPath[MAX_PATH];
        if (swprintf_s(wszPath, MAX_PATH, L"%s\\%s", wszFolder, name) < 0)
        {
            continue;
        }
        if (GetFileAttributesW(wszPath) == INVALID_FILE_ATTRIBUTES)
        {
            continue;
        }

        // LOAD_LIBRARY_AS_IMAGE_RESOURCE lays the file out the way the loader would, so relative addresses in
        // the PDB land where they should, but no entry point runs. The handle it returns is tagged in its low
        // bits to say it is not a real module, and those have to be cleared before it is used as a base.
        HMODULE handle = LoadLibraryExW(wszPath, NULL, LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE);
        if (!handle)
        {
            continue;
        }

        wcsncpy_s(pathOut, cchPathOut, wszPath, _TRUNCATE);
        return (HMODULE)((ULONG_PTR)handle & ~(ULONG_PTR)3);
    }

    pathOut[0] = 0;
    return NULL;
}

// ---------------------------------------------------------------------------------------------------------------
// Search mode
//
//     sp_symbolprobe <module> <text>
//
// Prints every symbol in the module whose undecorated name contains the text. This is how a renamed function is
// found again: search for the class or the method and read the current spelling out of the result.
// ---------------------------------------------------------------------------------------------------------------

typedef struct SearchContext
{
    const wchar_t* needle;
    int matches;
    int scanned;
} SearchContext;

static BOOL SearchCallback(const wchar_t* undecorated, const wchar_t* decorated, ULONGLONG offset, void* context)
{
    SearchContext* ctx = (SearchContext*)context;
    ctx->scanned++;

    const wchar_t* name = (undecorated && undecorated[0]) ? undecorated : decorated;
    if (!StrStrIW(name, ctx->needle))
    {
        return TRUE;
    }

    // The compiler emits a public symbol for every funclet a function is split into: exception handlers,
    // destructor thunks, coroutine bodies. They carry the enclosing function's name, so a search for a class
    // fills up with them and the real entry points never get printed. None of them can be hooked, so they are
    // skipped here rather than filtered by eye afterwards.
    if (wcschr(name, L'`') ||
        StrStrIW(name, L"winrt::impl::") ||
        StrStrIW(name, L"$_ResumeCoro") ||
        StrStrIW(name, L"$_InitCoro") ||
        StrStrIW(name, L"std::"))
    {
        return TRUE;
    }

    wprintf(L"+0x%08llX  %s\n", offset, name);
    ctx->matches++;

    // Enough to read; a broad search would otherwise print thousands of lines.
    return ctx->matches < 200;
}

static int Search(const wchar_t* moduleName, const wchar_t* needle)
{
    wchar_t wszPath[MAX_PATH];
    HMODULE module = MapImage(moduleName, wszPath, ARRAYSIZE(wszPath));
    if (!module)
    {
        wprintf(L"%s was not found in any shell folder\n", moduleName);
        return 1;
    }

    wprintf(L"Searching %s\n", wszPath);
    wprintf(L"for symbols containing: %s\n\n", needle);

    SearchContext ctx = { needle, 0, 0 };
    ULONGLONG start = GetTickCount64();
    BOOL ok = SP_EnumSymbolsAtOwned("symbolprobe", module, wszPath, SearchCallback, &ctx);
    ULONGLONG elapsed = GetTickCount64() - start;

    if (!ok)
    {
        wprintf(L"\nThe symbols for this module could not be read.\n");
        return 1;
    }

    wprintf(L"\n%d match(es) out of %d symbol(s), %llu ms\n", ctx.matches, ctx.scanned, elapsed);
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc >= 3)
    {
        SP_LogInitialize();
        int result = Search(argv[1], argv[2]);
        SP_SymbolsShutdown();
        SP_LogShutdown();
        return result;
    }

    printf("ShadePatcher symbol probe\n");
    printf("Checking whether the functions the queued mods need still exist on this build.\n");
    printf("To look a name up instead:  sp_symbolprobe <module> <text>\n\n");

    SP_LogInitialize();

    int found = 0;
    int missing = 0;

    for (size_t i = 0; i < ARRAYSIZE(kProbes); ++i)
    {
        const Probe* probe = &kProbes[i];

        wprintf(L"%s\n", probe->mod);

        // Shortened for the report; the full text is what is looked up.
        wchar_t shortName[110];
        wcsncpy_s(shortName, ARRAYSIZE(shortName), probe->symbol, 96);
        if (wcslen(probe->symbol) > 96)
        {
            wcscat_s(shortName, ARRAYSIZE(shortName), L"...");
        }
        wprintf(L"  %s\n", shortName);

        BOOL resolved = FALSE;

        for (size_t m = 0; m < probe->moduleCount && !resolved; ++m)
        {
            wchar_t wszPath[MAX_PATH];
            HMODULE module = MapImage(probe->modules[m], wszPath, ARRAYSIZE(wszPath));
            if (!module)
            {
                wprintf(L"    %-22s not found in any shell folder\n", probe->modules[m]);
                continue;
            }

            void* address = NULL;
            const wchar_t* names[1] = { probe->symbol };

            SP_SymbolHook hook;
            ZeroMemory(&hook, sizeof(hook));
            hook.symbols = names;
            hook.symbolCount = 1;
            hook.pOriginal = &address;
            hook.hookFunction = NULL;      // resolve only; nothing is patched
            hook.optional = TRUE;

            ULONGLONG start = GetTickCount64();
            SP_ResolveSymbolsAtOwned("symbolprobe", module, wszPath, &hook, 1);
            ULONGLONG elapsed = GetTickCount64() - start;

            if (address)
            {
                wprintf(L"    %-22s FOUND at +0x%llX  (%llu ms)\n", probe->modules[m],
                        (ULONGLONG)((BYTE*)address - (BYTE*)module), elapsed);
                resolved = TRUE;
            }
            else
            {
                wprintf(L"    %-22s not in this module  (%llu ms)\n", probe->modules[m], elapsed);
            }
        }

        if (resolved)
        {
            found++;
        }
        else
        {
            missing++;
            wprintf(L"    -> cannot be ported with this symbol name\n");
        }
        wprintf(L"\n");
    }

    SP_SymbolsShutdown();
    SP_LogShutdown();

    printf("%d symbol(s) found, %d missing\n", found, missing);
    return 0;
}
