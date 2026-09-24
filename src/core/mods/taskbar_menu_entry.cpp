//
// taskbar-menu-entry - puts a ShadePatcher entry in the Windows 11 taskbar context menu.
//
// Why this is not a menu hook
// ---------------------------
// On Windows 11 build 26200 the taskbar context menu is not a Win32 menu at all. Two things were tried and
// neither was ever called when the menu opened:
//
//     TrackPopupMenuEx                                  how a classic menu is shown
//     ImmersiveContextMenuHelper::ApplyOwnerDrawToMenu  how ExplorerPatcher reaches the older one
//
// The menu is built entirely in XAML, in Taskbar.View.dll; its window class, Xaml_WindowedPopupClass, says as
// much. So the entry is added as a XAML element, alongside the ones the shell creates.
//
// Where it hooks
// --------------
// The shell builds the menu's first item in
//     winrt::Taskbar::implementation::ContextMenus::CreateTaskManagerMenuFlyoutItem
// which returns the "Task Manager" item. That is the one moment when the menu is known to be under
// construction, and the returned item is the handle on it.
//
// The item has no parent when it is created, so the work waits for its Loaded event. By then XAML has put it
// inside the menu's presenter, and the presenter's item list is where the entry goes.
//
// About the signature
// -------------------
// That function is a public symbol: the PDB gives its address but not its parameters. The shape used here comes
// from the calling convention instead. On x64 a function returning a C++/WinRT object returns it through a
// hidden first argument, and the next three arrive in registers. None of them is interpreted, they are only
// passed back through, so the shape holds for any function taking up to three parameters. It was confirmed in a
// logging-only form first: the hook was entered once per menu and the shell stayed up.
//
// Exceptions
// ----------
// C++/WinRT reports failures by throwing, so this one file is compiled with exceptions on while the rest of the
// engine is not. Every path that XAML can call into is wrapped: an exception reaching the shell's UI thread
// would end the process.
//
#define SP_MOD_ID "taskbar-menu-entry"
#include "engine/modapi.h"

#include <atomic>
#include <string>

#include "menuitems.h"
#include "utils.h"

#include <winrt/Windows.Foundation.h>
// The menu's item list is an IObservableVector, whose methods only become callable with this header.
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Text.h>

namespace {

using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using winrt::Windows::Foundation::IInspectable;
using winrt::Windows::UI::Xaml::Media::VisualTreeHelper;

std::atomic<bool> g_enabled{ true };

// The text of the entry. It lives here rather than in the settings window's string table, because that table
// belongs to sp_gui.dll, which is not loaded in the shell.
constexpr wchar_t kEntryText[] = L"ShadePatcher";

// ---------------------------------------------------------------------------------------------------------------
// Opening the settings
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

// The settings window runs its own message loop, so it gets its own thread. Running it on the taskbar's UI
// thread would freeze the shell for as long as the window is open.
void OpenSettings()
{
    SP_Log(L"Opening the settings window");
    if (HANDLE thread = CreateThread(nullptr, 0, OpenSettingsThread, nullptr, 0, nullptr))
    {
        CloseHandle(thread);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Adding the entry
// ---------------------------------------------------------------------------------------------------------------

// The menu's own item list. The presenter that lays the items out is not it: the presenter is generated from
// the flyout and its Items collection refuses to be appended to, which is what E_UNEXPECTED meant on the first
// attempt. The list that can be added to belongs to the MenuFlyout, and a MenuFlyoutItem's logical parent is
// that flyout.
MenuFlyout FindOwningFlyout(FrameworkElement const& element)
{
    if (auto parent = element.Parent())
    {
        if (auto flyout = parent.try_as<MenuFlyout>())
        {
            return flyout;
        }
    }

    // Some builds parent the items one level further out, so the logical chain is followed a little way.
    DependencyObject current = element;
    for (int depth = 0; current && depth < 8; ++depth)
    {
        if (auto asElement = current.try_as<FrameworkElement>())
        {
            if (auto parent = asElement.Parent())
            {
                if (auto flyout = parent.try_as<MenuFlyout>())
                {
                    return flyout;
                }
                current = parent;
                continue;
            }
        }
        current = VisualTreeHelper::GetParent(current);
    }

    return nullptr;
}

bool AlreadyPresent(MenuFlyout const& flyout)
{
    auto items = flyout.Items();
    for (uint32_t i = 0; i < items.Size(); ++i)
    {
        if (auto existing = items.GetAt(i).try_as<MenuFlyoutItem>())
        {
            if (existing.Text() == kEntryText)
            {
                return true;
            }
        }
    }
    return false;
}

// The product icon as a file URI. XAML loads an image from a path rather than from this DLL's resources, so
// app.png is shipped next to the DLL, put there by the build and by the installer. When this DLL is the proxy
// copy in the Windows folder the file is looked for in the install folder instead.
winrt::Windows::Foundation::Uri IconUri()
{
    wchar_t wszPath[MAX_PATH];
    if (!FindProductFile(L"app.png", wszPath, MAX_PATH))
    {
        SP_LogDebug(L"No icon file next to the engine or in the install folder");
        return nullptr;
    }

    // A file URI wants forward slashes.
    std::wstring uri = L"file:///";
    for (const wchar_t* p = wszPath; *p; ++p)
    {
        uri += (*p == L'\\') ? L'/' : *p;
    }
    return winrt::Windows::Foundation::Uri(uri);
}

// Builds the entry itself, icon and all.
//
// The two items the shell puts in this menu each start with a symbol, so this one does too; an entry without an
// icon would sit text-first and look out of place. The product icon is used rather than a stock glyph, so the
// row is recognisably ours.
MenuFlyoutItem MakeEntry()
{
    MenuFlyoutItem entry;
    entry.Text(kEntryText);

    if (auto uri = IconUri())
    {
        BitmapIcon icon;
        icon.UriSource(uri);
        // A BitmapIcon paints a silhouette in the menu's foreground colour unless told otherwise, which would
        // throw away the artwork's own colours.
        icon.ShowAsMonochrome(false);
        entry.Icon(icon);
    }
    else
    {
        // The shell's own settings glyph keeps the row aligned with the others when the icon file is missing,
        // which is better than an entry that sits out of line.
        FontIcon icon;
        icon.Glyph(L"\uE713");
        icon.FontFamily(Media::FontFamily(L"Segoe Fluent Icons"));
        entry.Icon(icon);
    }

    entry.Click([](IInspectable const&, RoutedEventArgs const&) {
        try
        {
            OpenSettings();
        }
        catch (...)
        {
        }
    });
    return entry;
}

// Builds one of the user's own entries. Its command is captured by value, so the item stays valid after the
// list it came from has gone out of scope.
MenuFlyoutItem MakeCustomEntry(SP_MenuItem const& source)
{
    MenuFlyoutItem entry;
    entry.Text(source.name);

    // An entry names its icon either as a Segoe Fluent Icons glyph or as a path to an image. The glyph may be
    // the character itself, which is what the ready-made entries carry, or its code as the user typed it
    // (E713, 0xE713, \xE713); either way the row comes out looking like the ones Windows puts in this menu.
    const wchar_t glyphCode = SP_MenuItemIconGlyph(source.icon);

    if (glyphCode)
    {
        try
        {
            wchar_t text[2] = { glyphCode, 0 };
            FontIcon icon;
            icon.Glyph(text);
            icon.FontFamily(winrt::Windows::UI::Xaml::Media::FontFamily(L"Segoe Fluent Icons"));
            entry.Icon(icon);
        }
        catch (...)
        {
        }
    }
    else if (source.icon[0] && GetFileAttributesW(source.icon) != INVALID_FILE_ATTRIBUTES)
    {
        std::wstring uri = L"file:///";
        for (const wchar_t* p = source.icon; *p; ++p)
        {
            uri += (*p == L'\\') ? L'/' : *p;
        }
        try
        {
            BitmapIcon icon;
            icon.UriSource(winrt::Windows::Foundation::Uri(uri));
            icon.ShowAsMonochrome(false);
            entry.Icon(icon);
        }
        catch (...)
        {
            // A bad path costs the icon, not the entry.
        }
    }

    SP_MenuItem copy = source;
    entry.Click([copy](IInspectable const&, RoutedEventArgs const&) {
        try
        {
            SP_MenuItemExecute(&copy);
        }
        catch (...)
        {
        }
    });
    return entry;
}

// Writes the chain of ancestors to the log. Which types sit above a menu item changes between Windows builds,
// and this is what says where the entry can actually be attached on the build in front of us.
void LogAncestors(DependencyObject const& from)
{
    if (!SP_LogEnabled(SP_LOG_DEBUG))
    {
        return;
    }

    DependencyObject current = from;
    for (int depth = 0; current && depth < 10; ++depth)
    {
        SP_LogDebug(L"  ancestor %d: %s", depth, winrt::get_class_name(current).c_str());
        current = VisualTreeHelper::GetParent(current);
    }
}

void OnTaskManagerItemLoaded(IInspectable const& sender)
{
    if (!g_enabled.load(std::memory_order_relaxed))
    {
        return;
    }

    auto element = sender.try_as<FrameworkElement>();
    if (!element)
    {
        return;
    }

    LogAncestors(element);

    // First choice: the flyout's own item list, which is the collection the menu is built from.
    if (auto flyout = FindOwningFlyout(element))
    {
        if (AlreadyPresent(flyout))
        {
            return;
        }
        auto items = flyout.Items();
        items.Append(MenuFlyoutSeparator());
        items.Append(MakeEntry());
        SP_Log(L"Entry added to the flyout; %u item(s)", items.Size());
        return;
    }

    // The flyout object itself is not reachable from a loaded item, but its item list is. The presenter that
    // lays the menu out is an ItemsControl whose ItemsSource IS that list: reading Items on the presenter fails
    // with E_UNEXPECTED precisely because a source is bound to it. Appending to the source therefore appends to
    // the menu, and the new row is generated by the presenter like every other one, with its icon column and
    // its click handling.
    DependencyObject current = element;
    for (int depth = 0; current && depth < 16; ++depth)
    {
        if (auto presenter = current.try_as<ItemsControl>())
        {
            auto source = presenter.ItemsSource();
            auto items = source.try_as<winrt::Windows::Foundation::Collections::IVector<IInspectable>>();
            if (!items)
            {
                SP_LogDebug(L"The presenter's item source cannot be added to");
                return;
            }

            for (uint32_t i = 0; i < items.Size(); ++i)
            {
                if (auto existing = items.GetAt(i).try_as<MenuFlyoutItem>())
                {
                    if (existing.Text() == kEntryText)
                    {
                        return;     // already there
                    }
                }
            }

            items.Append(MenuFlyoutSeparator());
            items.Append(MakeEntry());

            // Then whatever the user added for themselves, in the order they arranged it.
            SP_MenuItem custom[SP_MENU_MAX_ITEMS];
            int customCount = SP_MenuItemsLoad(custom, SP_MENU_MAX_ITEMS);
            for (int c = 0; c < customCount; ++c)
            {
                // An entry still being filled in has no command yet; a row for it would do nothing.
                if (custom[c].command[0])
                {
                    items.Append(MakeCustomEntry(custom[c]));
                }
            }

            SP_Log(L"Entry added to the menu, with %d of the user's own; %u item(s)",
                   customCount, items.Size());
            return;
        }
        current = VisualTreeHelper::GetParent(current);
    }

    SP_LogDebug(L"Nothing above the Task Manager item could hold the entry");
}

// ---------------------------------------------------------------------------------------------------------------
// The hook
// ---------------------------------------------------------------------------------------------------------------

using CreateTaskManagerMenuFlyoutItem_t = void*(__fastcall*)(void* result, void* a2, void* a3, void* a4);
CreateTaskManagerMenuFlyoutItem_t g_origCreateTaskManagerItem = nullptr;

void* __fastcall CreateTaskManagerMenuFlyoutItem_Hook(void* result, void* a2, void* a3, void* a4)
{
    void* returned = g_origCreateTaskManagerItem(result, a2, a3, a4);

    if (!g_enabled.load(std::memory_order_relaxed) || !result)
    {
        return returned;
    }

    try
    {
        // The hidden return slot holds the projected object, which is one interface pointer wide.
        auto* abi = *reinterpret_cast<::IUnknown**>(result);
        if (!abi)
        {
            return returned;
        }

        // copy_from_abi takes its own reference, so the shell's own reference is left exactly as it was.
        MenuFlyoutItem item{ nullptr };
        winrt::copy_from_abi(item, abi);

        item.Loaded([](IInspectable const& sender, RoutedEventArgs const&) {
            // XAML calls this; nothing may escape back into it.
            try
            {
                OnTaskManagerItemLoaded(sender);
            }
            catch (winrt::hresult_error const& e)
            {
                SP_LogError(L"The entry could not be added: 0x%08X", (unsigned)e.code());
            }
            catch (...)
            {
                SP_LogError(L"The entry could not be added");
            }
        });
    }
    catch (...)
    {
        SP_LogError(L"The taskbar menu item could not be watched");
    }

    return returned;
}

// ---------------------------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------------------------

void LoadSettings()
{
    g_enabled.store(SP_GetIntSetting(L"OnTaskbar", 1) != 0, std::memory_order_relaxed);
}

// The taskbar is a separate package that the shell brings up a moment after the process starts, so on a cold
// sign-in this mod is initialized before Taskbar.View.dll exists. Failing there would mean the entry is missing
// until the next shell restart, which is exactly the case the user hits. So the wait is part of starting up: the
// hook goes in as soon as the library appears.
BOOL InstallHook(HMODULE hTaskbar)
{
    static const wchar_t* const kNames[] =
    {
        L"winrt::Taskbar::implementation::ContextMenus::CreateTaskManagerMenuFlyoutItem",
    };

    SP_SymbolHook hooks[1] = {};
    hooks[0].symbols = kNames;
    hooks[0].symbolCount = ARRAYSIZE(kNames);
    hooks[0].pOriginal = (void**)&g_origCreateTaskManagerItem;
    hooks[0].hookFunction = (void*)CreateTaskManagerMenuFlyoutItem_Hook;
    hooks[0].optional = FALSE;

    if (!SP_HookSymbols(hTaskbar, hooks, ARRAYSIZE(hooks)))
    {
        SP_LogError(L"The taskbar menu builder was not found in this build");
        return FALSE;
    }

    SP_Log(L"Watching the taskbar menu builder");
    return TRUE;
}

// Runs on the engine's worker thread once Taskbar.View.dll is in the process (at once if it already is). There is
// no deadline: the taskbar package comes up when it comes up, and the engine cancels the wait if the mod unloads.
void OnTaskbarLoaded(HMODULE hTaskbar, void* /*context*/)
{
    if (hTaskbar)
    {
        SP_Log(L"Taskbar.View.dll is in the process");
        InstallHook(hTaskbar);
    }
}

BOOL Init()
{
    LoadSettings();

    if (!SP_WaitForModuleOnWorker(L"Taskbar.View.dll", 0, OnTaskbarLoaded, nullptr))
    {
        SP_LogError(L"Could not wait for the taskbar library");
        return FALSE;
    }

    return TRUE;
}

}   // namespace

SP_MOD_DEFINE(g_modTaskbarMenuEntry) =
{
    /* id             */ SP_MOD_ID,
    /* name           */ L"Add a ShadePatcher entry to the taskbar context menu",
    /* basedOn        */ nullptr,
    /* originalAuthor */ nullptr,
    /* targets        */ SP_TARGET_EXPLORER,
    /* minOsBuild     */ 22000,
    /* flags          */ SP_MOD_ALWAYS_ON,
    /* Init           */ Init,
    /* AfterInit      */ nullptr,
    /* SettingsChanged*/ LoadSettings,
    /* BeforeUninit   */ nullptr,
    /* Uninit         */ nullptr,
};
