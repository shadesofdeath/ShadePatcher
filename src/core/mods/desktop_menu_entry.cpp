//
// desktop-menu-entry - put the product's entry, and the user's own entries, in the Windows 11 desktop and File
// Explorer context menu.
//
// Why this is a separate mod from taskbar-menu-entry
// --------------------------------------------------
// Both menus are XAML, but not the same XAML. The taskbar is drawn by the XAML that ships in Windows, under the
// Windows.UI.Xaml namespace, and its popup window class is Xaml_WindowedPopupClass. The desktop menu is drawn by
// WinUI 3 from the Windows App SDK, under Microsoft.UI.Xaml, and its window classes are
// Microsoft.UI.Content.PopupWindowSiteBridge and XamlExplorerHostIslandWindow_WASDK.
//
// MenuFlyoutItem, ItemsControl and VisualTreeHelper exist in both, as different types in different libraries.
// So the idea carries over from taskbar_menu_entry.cpp but not a line of the code does.
//
// Where the entry goes
// --------------------
// The menu is built in Windows.UI.FileExplorer.dll by a class called ContextMenuPresenter. Two of its members
// are handed the finished menu:
//
//     ContextMenuPresenter::RegisterTappedOnShowMoreOptions(CommandBarFlyout)
//     ContextMenuPresenter::HandleDuplicateAccessKeys(CommandBarFlyout)
//
// The second is the one hooked here. It exists to check whether two entries claim the same access key, which it
// can only do once every entry is in place, so by the time it runs the menu is complete and appending to it is
// safe.
//
// A CommandBarFlyout keeps its rows in two collections. PrimaryCommands is the strip of icon-only buttons along
// the top (cut, copy, rename, delete); SecondaryCommands is the labelled list below it. The entry belongs in the
// second one, which is where "Open in Terminal" and "Show more options" also live.
//
#define SP_MOD_ID "desktop-menu-entry"
#include "engine/modapi.h"

#include <shlwapi.h>
#include <strsafe.h>
#include <tchar.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <atomic>
#include <vector>

#include "config.h"
#include "menuitems.h"
#include "utils.h"

using namespace winrt::Microsoft::UI::Xaml::Controls;
using winrt::Microsoft::UI::Xaml::RoutedEventArgs;
using winrt::Windows::Foundation::IInspectable;

namespace {

std::atomic<bool> g_enabled{ true };

// ---------------------------------------------------------------------------------------------------------------
// Opening the settings window
// ---------------------------------------------------------------------------------------------------------------

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
    if (auto pfn = (ZZGUI_t)GetProcAddress(self, "ZZGUI"))
    {
        pfn(nullptr, nullptr, nullptr, SW_SHOWNORMAL);
    }
    return 0;
}

// The settings window runs its own message loop, so it gets its own thread. Running it on the menu's UI thread
// would freeze the shell for as long as the window is open.
void OpenSettings()
{
    SP_Log(L"Opening the settings window");
    if (HANDLE thread = CreateThread(nullptr, 0, OpenSettingsThread, nullptr, 0, nullptr))
    {
        CloseHandle(thread);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Building the rows
// ---------------------------------------------------------------------------------------------------------------

// An icon for a row. The text is either one Segoe Fluent Icons character or a path to an image file, the same
// two spellings the taskbar entry understands, so one entry configured once shows up correctly in both menus.
IconElement MakeIcon(const wchar_t* icon)
{
    if (!icon || !icon[0])
    {
        return nullptr;
    }

    // The glyph may be typed as the character or as its code (E713, 0xE713, \xE713); both come back as one
    // character.
    if (wchar_t code = SP_MenuItemIconGlyph(icon))
    {
        wchar_t text[2] = { code, 0 };
        FontIcon glyph;
        glyph.Glyph(text);
        glyph.FontFamily(winrt::Microsoft::UI::Xaml::Media::FontFamily(L"Segoe Fluent Icons"));
        return glyph;
    }

    if (GetFileAttributesW(icon) == INVALID_FILE_ATTRIBUTES)
    {
        return nullptr;
    }

    std::wstring uri = L"file:///";
    for (const wchar_t* p = icon; *p; ++p)
    {
        uri += (*p == L'\\') ? L'/' : *p;
    }

    BitmapIcon bitmap;
    bitmap.UriSource(winrt::Windows::Foundation::Uri(uri));
    bitmap.ShowAsMonochrome(false);
    return bitmap;
}

// The product's own row, which opens the settings window.
AppBarButton MakeProductEntry()
{
    AppBarButton button;
    button.Label(_T(PRODUCT_NAME));

    // The product icon sits next to the DLL, or in the install folder when the DLL is the proxy copy in the
    // Windows folder; without it the row still works, just with a stock glyph.
    wchar_t wszIcon[MAX_PATH];
    IconElement icon = nullptr;
    if (FindProductFile(L"app.png", wszIcon, ARRAYSIZE(wszIcon)))
    {
        icon = MakeIcon(wszIcon);
    }
    button.Icon(icon ? icon : MakeIcon(L"\uE713"));

    button.Click([](IInspectable const&, RoutedEventArgs const&) {
        try
        {
            OpenSettings();
        }
        catch (...)
        {
        }
    });
    return button;
}

// One of the user's own rows.
AppBarButton MakeCustomEntry(SP_MenuItem const& source)
{
    AppBarButton button;
    button.Label(source.name);

    if (auto icon = MakeIcon(source.icon))
    {
        button.Icon(icon);
    }

    SP_MenuItem copy = source;
    button.Click([copy](IInspectable const&, RoutedEventArgs const&) {
        try
        {
            SP_MenuItemExecute(&copy);
        }
        catch (...)
        {
        }
    });
    return button;
}

// ---------------------------------------------------------------------------------------------------------------
// The hook
// ---------------------------------------------------------------------------------------------------------------

using MenuHandler_t = void(__fastcall*)(void* self, void* flyout);

// A C++/WinRT object is one pointer wide but has a destructor, so the x64 ABI hands it over as a pointer to a
// copy the caller made rather than in a register. Which of the two the argument actually is cannot be told by
// looking, so both readings are tried and the one that answers like a menu wins.
bool TryReadFlyout(void* candidate, CommandBarFlyout& out)
{
    if (!candidate)
    {
        return false;
    }

    try
    {
        CommandBarFlyout flyout{ nullptr };
        winrt::copy_from_abi(flyout, candidate);
        if (!flyout)
        {
            return false;
        }
        // Asking for the collection proves this really is the menu and not some other object.
        if (!flyout.SecondaryCommands())
        {
            return false;
        }
        out = flyout;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void AddEntries(CommandBarFlyout const& flyout)
{
    auto commands = flyout.SecondaryCommands();
    if (!commands)
    {
        return;
    }

    // Appending twice would show the entry twice if the menu object is ever reused, so an entry that is already
    // there is left alone.
    for (uint32_t i = 0; i < commands.Size(); ++i)
    {
        if (auto existing = commands.GetAt(i).try_as<AppBarButton>())
        {
            if (existing.Label() == _T(PRODUCT_NAME))
            {
                SP_LogDebug(L"The entry is already in this menu");
                return;
            }
        }
    }

    commands.Append(AppBarSeparator());
    commands.Append(MakeProductEntry());

    SP_MenuItem custom[SP_MENU_MAX_ITEMS];
    int customCount = SP_MenuItemsLoadFor(L"" SP_MOD_ID, custom, SP_MENU_MAX_ITEMS);
    for (int c = 0; c < customCount; ++c)
    {
        // An entry still being filled in has no command yet; a row for it would do nothing.
        if (custom[c].command[0])
        {
            commands.Append(MakeCustomEntry(custom[c]));
        }
    }

    SP_Log(L"Entry added to the desktop menu, with %d of the user's own; %u row(s)",
           customCount, commands.Size());
}

// Both hooked functions are handed the same thing, so the work is shared.
void OnMenuReady(void* flyout)
{
    SP_LogDebug(L"A menu was handed over");

    if (g_enabled.load(std::memory_order_relaxed))
    {
        CommandBarFlyout menu{ nullptr };

        // The pointer-to-a-copy reading is the one the ABI calls for, so it is tried first.
        if (TryReadFlyout(flyout ? *(void**)flyout : nullptr, menu) || TryReadFlyout(flyout, menu))
        {
            try
            {
                AddEntries(menu);
            }
            catch (const winrt::hresult_error& error)
            {
                SP_LogError(L"The entry could not be added: %s", error.message().c_str());
            }
            catch (...)
            {
                SP_LogError(L"The entry could not be added");
            }
        }
        else
        {
            SP_LogDebug(L"The argument was not a menu this build understands");
        }
    }

}

// HandleDuplicateAccessKeys only runs when two rows claim the same access key, so on its own it is not enough.
// RegisterTappedOnShowMoreOptions runs whenever the menu has a "Show more options" row, which the desktop menu
// always does. Hooking both means the entry is added whichever path this build takes; the duplicate check in
// AddEntries is what keeps it from being added twice when both fire.
// This build ships two implementations of the presenter side by side, ContextMenuPresenter and
// ContextMenuPresenter_Old, and which one serves a given menu is decided at run time. Each has three members
// that are handed the finished menu. Rather than guess, all six are hooked, every one of them optional, and the
// duplicate check in AddEntries keeps the entry from being added twice when more than one fires.
#define SP_MENU_HOOKS(X)                                                                       X(0, "ContextMenuPresenter::RegisterTappedOnShowMoreOptions")                              X(1, "ContextMenuPresenter::HandleDuplicateAccessKeys")                                    X(2, "ContextMenuPresenter::SetAccessKeyScope")                                            X(3, "ContextMenuPresenter_Old::RegisterTappedOnShowMoreOptions")                          X(4, "ContextMenuPresenter_Old::HandleDuplicateAccessKeys")                                X(5, "ContextMenuPresenter_Old::SetAccessKeyScope")

constexpr int kHookCount = 6;
MenuHandler_t g_originals[kHookCount] = {};

#define SP_DEFINE_HOOK(index, name)                                                            void __fastcall Hook##index(void* self, void* flyout)                                      {                                                                                              OnMenuReady(flyout);                                                                       if (g_originals[index]) { g_originals[index](self, flyout); }                          }
SP_MENU_HOOKS(SP_DEFINE_HOOK)
#undef SP_DEFINE_HOOK

// ---------------------------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------------------------

void LoadSettings()
{
    g_enabled.store(SP_GetIntSetting(L"OnDesktop", 1) != 0, std::memory_order_relaxed);
}

BOOL InstallHook(HMODULE hFileExplorer)
{
    // The decorated form is what the symbol file stores, so the names are spelled out in full.
    static const wchar_t* const kNames[kHookCount][1] =
    {
        { L"private: void __cdecl ContextMenuPresenter::RegisterTappedOnShowMoreOptions(struct winrt::Microsoft::UI::Xaml::Controls::CommandBarFlyout)" },
        { L"private: void __cdecl ContextMenuPresenter::HandleDuplicateAccessKeys(struct winrt::Microsoft::UI::Xaml::Controls::CommandBarFlyout)" },
        { L"private: void __cdecl ContextMenuPresenter::SetAccessKeyScope(struct winrt::Microsoft::UI::Xaml::Controls::CommandBarFlyout)" },
        { L"private: void __cdecl ContextMenuPresenter_Old::RegisterTappedOnShowMoreOptions(struct winrt::Microsoft::UI::Xaml::Controls::CommandBarFlyout)" },
        { L"private: void __cdecl ContextMenuPresenter_Old::HandleDuplicateAccessKeys(struct winrt::Microsoft::UI::Xaml::Controls::CommandBarFlyout)" },
        { L"private: void __cdecl ContextMenuPresenter_Old::SetAccessKeyScope(struct winrt::Microsoft::UI::Xaml::Controls::CommandBarFlyout)" },
    };

    static void* const kHookFunctions[kHookCount] =
    {
        (void*)Hook0, (void*)Hook1, (void*)Hook2, (void*)Hook3, (void*)Hook4, (void*)Hook5,
    };

    SP_SymbolHook hooks[kHookCount] = {};
    for (int i = 0; i < kHookCount; ++i)
    {
        hooks[i].symbols = kNames[i];
        hooks[i].symbolCount = 1;
        hooks[i].pOriginal = (void**)&g_originals[i];
        hooks[i].hookFunction = kHookFunctions[i];
        hooks[i].optional = TRUE;
    }

    if (!SP_HookSymbols(hFileExplorer, hooks, ARRAYSIZE(hooks)))
    {
        SP_LogError(L"The desktop menu builder was not found in this build");
        return FALSE;
    }

    SP_Log(L"Watching the desktop menu builder");
    return TRUE;
}

// Windows.UI.FileExplorer.dll is loaded the first time a shell view needs it, which on a cold sign-in is after the
// engine has started, and possibly minutes later: nothing loads it until the first context menu opens. So there
// is no deadline on the wait, and the engine cancels it if the mod unloads first. The callback is asked for on a
// worker thread because resolving the symbols reads the symbol cache, which is not something to do on the thread
// that is loading the library.
void OnFileExplorerLoaded(HMODULE hModule, void* /*context*/)
{
    if (hModule)
    {
        SP_Log(L"Windows.UI.FileExplorer.dll is in the process");
        InstallHook(hModule);
    }
}

BOOL Init()
{
    LoadSettings();

    if (!SP_WaitForModuleOnWorker(L"Windows.UI.FileExplorer.dll", 0, OnFileExplorerLoaded, nullptr))
    {
        SP_LogError(L"Could not wait for the shell view library");
        return FALSE;
    }

    return TRUE;
}

}   // namespace

SP_MOD_DEFINE(g_modDesktopMenuEntry) =
{
    /* id             */ SP_MOD_ID,
    /* name           */ L"Add a ShadePatcher entry to the desktop context menu",
    /* basedOn        */ nullptr,
    /* originalAuthor */ nullptr,
    /* targets        */ SP_TARGET_EXPLORER,
    /* minOsBuild     */ 22621,     // the WinUI context menu
    /* flags          */ 0,
    /* Init           */ Init,
    /* AfterInit      */ nullptr,
    /* SettingsChanged*/ LoadSettings,
    /* BeforeUninit   */ nullptr,
    /* Uninit         */ nullptr,
};
