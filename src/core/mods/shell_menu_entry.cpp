//
// shell-menu-entry - puts a ShadePatcher entry in the taskbar and desktop context menus.
//
// This is how the settings are reached without hunting for a shortcut, the way ExplorerPatcher puts
// "Properties" on the taskbar menu.
//
// Why there are two hooks
// -----------------------
// The Windows 11 taskbar menu looks like a XAML flyout, and it is: its window class is
// Xaml_WindowedPopupClass. Hooking TrackPopupMenuEx, which is how a classic menu is shown, therefore does
// nothing for it; that was tried and the menu came up unchanged.
//
// What the flyout is built from is still an ordinary HMENU. Just before the menu is shown, the shell hands that
// HMENU to
//     ImmersiveContextMenuHelper::ApplyOwnerDrawToMenu(HMENU, HWND, POINT*, ...)
// in twinui.pcshell.dll, so appending an item there puts it in the flyout. That function is not exported, which
// is what the engine's symbol layer is for.
//
// Classic menus, including the desktop's, still go through TrackPopupMenuEx, so both hooks are kept and each
// covers the menus the other cannot.
//
#define SP_MOD_ID "shell-menu-entry"
#include "engine/modapi.h"

#include <atomic>

namespace {

// Well above anything the shell assigns to a context menu item, and below the 0xF000 range the system reserves.
constexpr UINT kCommandId = 0xEFED;

std::atomic<bool> g_onTaskbar{ true };
std::atomic<bool> g_onDesktop{ true };

// Set while a menu this mod has added its item to is on screen, so the command can be recognised when it comes
// back through whichever path the shell uses.
std::atomic<HMENU> g_ourMenu{ nullptr };

// ---------------------------------------------------------------------------------------------------------------
// Which menu is this
// ---------------------------------------------------------------------------------------------------------------

bool ClassIsTaskbar(const wchar_t* name)
{
    return _wcsicmp(name, L"Shell_TrayWnd") == 0 ||
           _wcsicmp(name, L"Shell_SecondaryTrayWnd") == 0 ||
           _wcsicmp(name, L"TrayNotifyWnd") == 0 ||
           _wcsicmp(name, L"MSTaskListWClass") == 0 ||
           _wcsicmp(name, L"MSTaskSwWClass") == 0 ||
           _wcsicmp(name, L"Windows.UI.Input.InputSite.WindowClass") == 0;
}

bool ClassIsDesktop(const wchar_t* name)
{
    return _wcsicmp(name, L"SHELLDLL_DefView") == 0 ||
           _wcsicmp(name, L"SysListView32") == 0 ||
           _wcsicmp(name, L"Progman") == 0 ||
           _wcsicmp(name, L"WorkerW") == 0;
}

// The window a menu is shown for is not always the one that carries the recognisable class, so its ancestors
// are checked too.
bool ShouldAddEntry(HWND hWnd)
{
    for (HWND w = hWnd; w != nullptr; w = GetParent(w))
    {
        wchar_t wszClass[96];
        if (!GetClassNameW(w, wszClass, ARRAYSIZE(wszClass)))
        {
            break;
        }
        if (g_onTaskbar.load(std::memory_order_relaxed) && ClassIsTaskbar(wszClass))
        {
            return true;
        }
        if (g_onDesktop.load(std::memory_order_relaxed) && ClassIsDesktop(wszClass))
        {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------
// Opening the settings
// ---------------------------------------------------------------------------------------------------------------

// The settings window runs its own message loop, so it gets its own thread; blocking the thread that is showing
// a menu would freeze the shell.
DWORD WINAPI OpenSettingsThread(LPVOID)
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&OpenSettingsThread, &self);
    if (!self)
    {
        return 1;
    }

    typedef int (*ZZGUI_t)(HWND, HINSTANCE, LPSTR, int);
    ZZGUI_t pfn = (ZZGUI_t)GetProcAddress(self, "ZZGUI");
    if (pfn)
    {
        pfn(nullptr, nullptr, nullptr, SW_SHOWNORMAL);
    }
    return 0;
}

void OpenSettings()
{
    SP_Log(L"Opening the settings window");
    HANDLE thread = CreateThread(nullptr, 0, OpenSettingsThread, nullptr, 0, nullptr);
    if (thread)
    {
        CloseHandle(thread);
    }
}

void AppendOurItem(HMENU hMenu)
{
    if (GetMenuItemCount(hMenu) > 0)
    {
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(hMenu, MF_STRING, kCommandId, L"ShadePatcher");
    g_ourMenu.store(hMenu, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------------------------------
// The Windows 11 taskbar flyout
// ---------------------------------------------------------------------------------------------------------------

// long __cdecl ImmersiveContextMenuHelper::ApplyOwnerDrawToMenu(
//     struct HMENU__ *, struct HWND__ *, struct tagPOINT *, enum ImmersiveContextMenuOptions,
//     class CSimplePointerArrayNewMem<struct ContextMenuRenderingData, ...> &)
using ApplyOwnerDrawToMenu_t = HRESULT(__cdecl*)(HMENU, HWND, POINT*, int, void*);
ApplyOwnerDrawToMenu_t g_origApplyOwnerDraw = nullptr;

HRESULT __cdecl ApplyOwnerDrawToMenu_Hook(HMENU hMenu, HWND hWnd, POINT* pt, int options, void* renderingData)
{
    // Logged before the decision, so "the shell never called this" can be told apart from "it did, but the
    // window was not one the entry belongs on".
    wchar_t wszClass[96] = L"?";
    GetClassNameW(hWnd, wszClass, ARRAYSIZE(wszClass));
    SP_LogDebug(L"immersive menu on %p (%s)", hWnd, wszClass);

    if (hMenu && ShouldAddEntry(hWnd))
    {
        SP_LogDebug(L"Adding the entry to an immersive menu on %p", hWnd);
        // Appended before the owner-draw pass runs, so the new item is measured and drawn like the rest.
        AppendOurItem(hMenu);
    }

    return g_origApplyOwnerDraw(hMenu, hWnd, pt, options, renderingData);
}

// ---------------------------------------------------------------------------------------------------------------
// Classic menus
// ---------------------------------------------------------------------------------------------------------------

// TrackPopupMenu and TrackPopupMenuEx are two entry points into the same code in user32, and on build 26200 the
// older one does not pass through the exported body of the newer one: the desktop's classic menu (the one behind
// "Show more options") is shown with TrackPopupMenu and never reached a hook on TrackPopupMenuEx alone. So both
// are hooked and share the decision. Should a build route one through the other, the second hook sees the menu
// already carrying the entry and adds nothing.
using TrackPopupMenuEx_t = decltype(&TrackPopupMenuEx);
using TrackPopupMenu_t = decltype(&TrackPopupMenu);
TrackPopupMenuEx_t g_origTrackPopupMenuEx = nullptr;
TrackPopupMenu_t   g_origTrackPopupMenu = nullptr;

// Before the menu is shown. Returns TRUE when the entry was appended by this call; `alreadyOurs` is set when
// the item is already in the menu (the immersive hook or the outer of two nested tracking calls put it there).
bool PrepareMenu(HMENU hMenu, UINT uFlags, HWND hWnd, const wchar_t* via, bool* alreadyOurs)
{
    *alreadyOurs = (g_ourMenu.load(std::memory_order_relaxed) == hMenu);

    // Only a caller that asked for the command to be returned can carry this entry. Otherwise the command would
    // be posted to a window that knows nothing about it.
    wchar_t wszTrackClass[96] = L"?";
    GetClassNameW(hWnd, wszTrackClass, ARRAYSIZE(wszTrackClass));
    SP_LogDebug(L"classic menu (%s) on %p (%s) flags 0x%X", via, hWnd, wszTrackClass, uFlags);

    const bool added = !*alreadyOurs && (uFlags & TPM_RETURNCMD) && hMenu && ShouldAddEntry(hWnd);
    if (added)
    {
        SP_LogDebug(L"Adding the entry to a classic menu on %p", hWnd);
        AppendOurItem(hMenu);
    }
    return added;
}

// After the menu closed. The menu handle is forgotten by the call that added the entry, so a later menu that
// happens to get the same HMENU value is not mistaken for one that already carries it.
BOOL FinishMenu(HMENU hMenu, bool added, bool alreadyOurs, BOOL result)
{
    if (added)
    {
        HMENU expected = hMenu;
        g_ourMenu.compare_exchange_strong(expected, nullptr, std::memory_order_relaxed);
    }

    if ((added || alreadyOurs) && (UINT)result == kCommandId)
    {
        OpenSettings();
        // Zero means "nothing was chosen", which stops the shell from acting on an id it does not know.
        return 0;
    }

    return result;
}

BOOL WINAPI TrackPopupMenuEx_Hook(HMENU hMenu, UINT uFlags, int x, int y, HWND hWnd, LPTPMPARAMS lptpm)
{
    bool alreadyOurs = false;
    const bool added = PrepareMenu(hMenu, uFlags, hWnd, L"TrackPopupMenuEx", &alreadyOurs);

    BOOL result = g_origTrackPopupMenuEx(hMenu, uFlags, x, y, hWnd, lptpm);

    return FinishMenu(hMenu, added, alreadyOurs, result);
}

BOOL WINAPI TrackPopupMenu_Hook(HMENU hMenu, UINT uFlags, int x, int y, int nReserved, HWND hWnd,
                                const RECT* prcRect)
{
    bool alreadyOurs = false;
    const bool added = PrepareMenu(hMenu, uFlags, hWnd, L"TrackPopupMenu", &alreadyOurs);

    BOOL result = g_origTrackPopupMenu(hMenu, uFlags, x, y, nReserved, hWnd, prcRect);

    return FinishMenu(hMenu, added, alreadyOurs, result);
}

// ---------------------------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------------------------

void LoadSettings()
{
    g_onTaskbar.store(SP_GetIntSetting(L"OnTaskbar", 1) != 0, std::memory_order_relaxed);
    g_onDesktop.store(SP_GetIntSetting(L"OnDesktop", 1) != 0, std::memory_order_relaxed);
}

BOOL Init()
{
    LoadSettings();

    if (!SP_HookBegin())
    {
        return FALSE;
    }
    if (!SP_SetFunctionHook(TrackPopupMenuEx, TrackPopupMenuEx_Hook, &g_origTrackPopupMenuEx) ||
        !SP_SetFunctionHook(TrackPopupMenu, TrackPopupMenu_Hook, &g_origTrackPopupMenu))
    {
        SP_HookAbort();
        return FALSE;
    }
    if (!SP_HookCommit())
    {
        return FALSE;
    }

    // The immersive menu is optional: on a build where that function is gone the classic hook still works, and
    // the mod should stay loaded rather than disappear entirely.
    HMODULE hTwinui = LoadLibraryExW(L"twinui.pcshell.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!hTwinui)
    {
        SP_LogError(L"twinui.pcshell.dll could not be loaded; only classic menus will carry the entry");
        return TRUE;
    }

    static const wchar_t* const kNames[] =
    {
        L"long __cdecl ImmersiveContextMenuHelper::ApplyOwnerDrawToMenu(struct HMENU__ *,struct HWND__ *,"
        L"struct tagPOINT *,enum ImmersiveContextMenuOptions,class CSimplePointerArrayNewMem<struct "
        L"ContextMenuRenderingData,class CSimpleArrayStandardCompareHelper<struct ContextMenuRenderingData *> > &)",
    };

    SP_SymbolHook hooks[1] = {};
    hooks[0].symbols = kNames;
    hooks[0].symbolCount = ARRAYSIZE(kNames);
    hooks[0].pOriginal = (void**)&g_origApplyOwnerDraw;
    hooks[0].hookFunction = (void*)ApplyOwnerDrawToMenu_Hook;
    hooks[0].optional = TRUE;

    if (!SP_HookSymbols(hTwinui, hooks, ARRAYSIZE(hooks)) || !g_origApplyOwnerDraw)
    {
        SP_LogError(L"The immersive menu function was not found; only classic menus will carry the entry");
    }
    else
    {
        SP_Log(L"The immersive menu hook is in place");
    }

    return TRUE;
}

}   // namespace

SP_MOD_DEFINE(g_modShellMenuEntry) =
{
    /* id             */ SP_MOD_ID,
    /* name           */ L"Add a ShadePatcher entry to the taskbar and desktop menus",
    /* basedOn        */ nullptr,
    /* originalAuthor */ nullptr,
    /* targets        */ SP_TARGET_EXPLORER,
    /* minOsBuild     */ 0,
    /* flags          */ 0,
    /* Init           */ Init,
    /* AfterInit      */ nullptr,
    /* SettingsChanged*/ LoadSettings,
    /* BeforeUninit   */ nullptr,
    /* Uninit         */ nullptr,
};
