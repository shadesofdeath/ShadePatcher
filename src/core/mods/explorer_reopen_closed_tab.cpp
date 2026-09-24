//
// explorer-reopen-closed-tab - Ctrl+Shift+T reopens the last closed File Explorer tab, the way a browser does.
//
// Adapted from the idea behind the Windhawk mod "File Explorer Reopen Closed Tab"
// (file-explorer-reopen-closed-tab) by Armaninyow. The implementation here is written against this engine's API.
//
// What the original does
// ----------------------
// It runs in a process of its own, polls IShellWindows every 750 ms to notice which tabs have disappeared,
// remembers their paths on a stack, and watches the keyboard system-wide with a low-level hook. On Ctrl+Shift+T
// it sends the "New tab" command into the foreground Explorer window, waits for the tab to show up in
// IShellWindows, and steers it to the remembered path with IWebBrowser2::Navigate2.
//
// What this does instead
// ----------------------
// This mod lives inside explorer.exe, so none of the polling is needed: the shell tells us directly.
//
//   * FileCabinet_CreateViewWindow2 in ExplorerFrame.dll is called every time a tab shows a folder. Its first
//     argument is the tab's IShellBrowser, whose window is the tab (class ShellTabWindowClass, a child of the
//     CabinetWClass frame), and its third argument is the view being created, from which the folder's item id
//     list is read. So every navigation updates "tab window -> folder" for free.
//   * Each tab window is subclassed; WM_NCDESTROY is the tab closing, and that is when its folder goes on the
//     closed stack. No polling, no missed tabs, no 750 ms lag.
//   * Ctrl+Shift+T is caught with a WH_KEYBOARD hook scoped to the Explorer window's own thread, not a
//     system-wide low-level hook. The shell's input is not delayed and nothing outside Explorer sees the hook.
//   * Reopening sends the same "New tab" command the original uses (WM_COMMAND 0xA21B, the command behind
//     Ctrl+T) to the tab window, then waits for the new tab's first view to come through the same
//     FileCabinet_CreateViewWindow2 hook. The tab's own IShellBrowser::BrowseObject takes it to the folder,
//     on its own thread, with no IShellWindows enumeration and no cross-apartment COM.
//
// Folders are remembered as item id lists rather than file system paths, so This PC, Control Panel, a library
// or a search result reopen just as a plain folder does.
//
// Closing a whole window records its tabs too, one per tab, so a window shut by accident can be brought back a
// tab at a time. The original clears its history at that point instead; see the porting notes.
//
#define SP_MOD_ID "file-explorer-reopen-closed-tab"
#include "engine/modapi.h"

#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")

namespace {

constexpr size_t   kMaxHistory      = 50;       // closed tabs remembered, most recent first
constexpr DWORD    kRestoreWindowMs = 3000;     // how long a requested new tab may take to appear
constexpr UINT     kNewTabCommand   = 0xA21B;   // WM_COMMAND id of "New tab" in the tab's browser (Ctrl+T)
constexpr UINT_PTR kSubclassId      = 1;

constexpr wchar_t kFrameClass[] = L"CabinetWClass";
constexpr wchar_t kTabClass[]   = L"ShellTabWindowClass";

std::atomic<bool> g_enabled{ true };

// ---------------------------------------------------------------------------------------------------------------
// Folders
//
// An absolute item id list is a self-contained run of bytes, so a copy of those bytes is a copy of the folder.
// Keeping the bytes in a vector means no ILFree bookkeeping and no ownership questions between threads.
// ---------------------------------------------------------------------------------------------------------------

using Pidl = std::vector<BYTE>;

PCIDLIST_ABSOLUTE AsPidl(const Pidl& pidl)
{
    return pidl.empty() ? nullptr : reinterpret_cast<PCIDLIST_ABSOLUTE>(pidl.data());
}

Pidl CopyPidl(PCIDLIST_ABSOLUTE pidl)
{
    Pidl copy;
    if (pidl)
    {
        UINT size = ILGetSize(pidl);
        if (size)
        {
            const BYTE* bytes = reinterpret_cast<const BYTE*>(pidl);
            copy.assign(bytes, bytes + size);
        }
    }
    return copy;
}

// The folder's parsing name, for the log. Never fails: an unnamed folder is written as "?".
std::wstring Describe(const Pidl& pidl)
{
    PWSTR name = nullptr;
    if (!pidl.empty() && SUCCEEDED(SHGetNameFromIDList(AsPidl(pidl), SIGDN_DESKTOPABSOLUTEPARSING, &name)) && name)
    {
        std::wstring text(name);
        CoTaskMemFree(name);
        return text;
    }
    return L"?";
}

// The folder a view is about to show. The view's folder object implements IPersistFolder2, which hands back
// the absolute item id list it was initialized with.
Pidl FolderOf(IShellView* view)
{
    Pidl folder;
    if (!view)
    {
        return folder;
    }

    IFolderView* folderView = nullptr;
    if (SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&folderView))) && folderView)
    {
        IPersistFolder2* persist = nullptr;
        if (SUCCEEDED(folderView->GetFolder(IID_PPV_ARGS(&persist))) && persist)
        {
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(persist->GetCurFolder(&pidl)) && pidl)
            {
                folder = CopyPidl(pidl);
                CoTaskMemFree(pidl);
            }
            persist->Release();
        }
        folderView->Release();
    }
    return folder;
}

// ---------------------------------------------------------------------------------------------------------------
// State
//
// Everything below is shared between the tab threads (hook bodies, subclass procs, keyboard hooks) and the
// engine thread (lifecycle), so it all sits behind one lock. Nothing slow is ever done while it is held.
// ---------------------------------------------------------------------------------------------------------------

struct TabState
{
    HWND           frame = nullptr;     // the CabinetWClass window the tab belongs to
    DWORD          thread = 0;          // the UI thread of that window
    IShellBrowser* browser = nullptr;   // unowned; the browser owns the tab window, so it outlives the window
    Pidl           folder;              // what the tab shows now; empty until its first navigation is seen
    Pidl           navigateTo;          // a restore waiting for this (new) tab's first view
};

struct PendingRestore
{
    HWND  frame = nullptr;              // the window a new tab was asked for; NULL when nothing is pending
    Pidl  folder;                       // where that tab should go
    DWORD requested = 0;                // GetTickCount when the request was made
};

std::mutex               g_lock;
std::map<HWND, TabState> g_tabs;            // by tab window
std::vector<Pidl>        g_closed;          // back = most recently closed
PendingRestore           g_pending;
std::map<DWORD, HHOOK>   g_keyboardHooks;   // one WH_KEYBOARD hook per Explorer window thread

// Registered messages cannot collide with anything the shell uses on its own windows, unlike WM_APP + n.
UINT g_msgRestore = 0;      // to a tab window: reopen the last closed tab in this window
UINT g_msgNavigate = 0;     // to a new tab window: go to the folder waiting for it

bool ClassIs(HWND hWnd, const wchar_t* className)
{
    wchar_t buffer[64] = {};
    return hWnd && GetClassNameW(hWnd, buffer, ARRAYSIZE(buffer)) && _wcsicmp(buffer, className) == 0;
}

void PushClosedLocked(Pidl&& folder)
{
    if (folder.empty())
    {
        return;
    }
    g_closed.push_back(std::move(folder));
    if (g_closed.size() > kMaxHistory)
    {
        g_closed.erase(g_closed.begin());
    }
}

// A restore whose new tab never showed up gives its folder back, so the next Ctrl+Shift+T tries again rather
// than losing it.
void ExpirePendingLocked()
{
    if (!g_pending.frame || GetTickCount() - g_pending.requested <= kRestoreWindowMs)
    {
        return;
    }
    SP_LogDebug(L"No new tab appeared for the last restore; the folder is kept for next time");
    PushClosedLocked(std::move(g_pending.folder));
    g_pending = PendingRestore{};
}

// ---------------------------------------------------------------------------------------------------------------
// The keyboard
//
// A WH_KEYBOARD hook sees every key the thread's message loop retrieves, before Explorer's accelerator table
// does, and returning non-zero discards it. It is per thread and in process: nothing else on the machine is
// touched, and a slow hook cannot cost the system its input the way a low-level hook can.
// ---------------------------------------------------------------------------------------------------------------

bool IsDown(int vk)
{
    return (GetKeyState(vk) & 0x8000) != 0;
}

LRESULT CALLBACK KeyboardProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code != HC_ACTION || !g_enabled.load(std::memory_order_relaxed))
    {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    // Bit 31 of lParam is the transition (0 = pressed) and bit 30 the previous state (1 = auto-repeat). One
    // reopened tab per press: holding the key down must not empty the whole history in a burst.
    const bool pressed = (lParam & 0x80000000u) == 0;
    const bool repeat = (lParam & 0x40000000u) != 0;
    if (wParam != 'T' || !pressed || repeat || !IsDown(VK_CONTROL) || !IsDown(VK_SHIFT) || IsDown(VK_MENU))
    {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    // Only when an Explorer window of this thread is in front. A dialog or another window in front passes
    // the key on untouched.
    HWND frame = GetAncestor(GetForegroundWindow(), GA_ROOT);
    if (!frame || !ClassIs(frame, kFrameClass) || GetWindowThreadProcessId(frame, nullptr) != GetCurrentThreadId())
    {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    // The work is handed to a tab window of that frame as a posted message: nothing that changes the shell's
    // windows runs from inside a hook called by GetMessage. The visible tab is the active one and gets it.
    HWND target = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        ExpirePendingLocked();
        if (g_closed.empty())
        {
            // Nothing to reopen: the key is left to Explorer, in case a build ever handles it itself.
            return CallNextHookEx(nullptr, code, wParam, lParam);
        }
        for (const auto& entry : g_tabs)
        {
            if (entry.second.frame == frame && (!target || IsWindowVisible(entry.first)))
            {
                target = entry.first;
            }
        }
    }

    if (!target)
    {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    PostMessageW(target, g_msgRestore, 0, 0);
    return 1;   // consumed
}

// Called on the thread to hook, or from the engine thread with the id of a thread in this process; both are
// allowed for a thread-scoped hook with no module.
void EnsureKeyboardHookLocked(DWORD thread)
{
    if (g_keyboardHooks.count(thread))
    {
        return;
    }
    HHOOK hook = SetWindowsHookExW(WH_KEYBOARD, KeyboardProc, nullptr, thread);
    if (!hook)
    {
        SP_LogError(L"The keyboard of thread %lu could not be watched: %lu", thread, GetLastError());
        return;
    }
    g_keyboardHooks[thread] = hook;
    SP_LogDebug(L"Watching the keyboard of thread %lu", thread);
}

// A thread without tabs is a window that has closed; its hook goes, so that a later thread reusing the id is
// hooked afresh rather than mistaken for this one.
void DropKeyboardHookIfUnusedLocked(DWORD thread)
{
    for (const auto& entry : g_tabs)
    {
        if (entry.second.thread == thread)
        {
            return;
        }
    }
    auto it = g_keyboardHooks.find(thread);
    if (it != g_keyboardHooks.end())
    {
        UnhookWindowsHookEx(it->second);
        g_keyboardHooks.erase(it);
        SP_LogDebug(L"Thread %lu has no tabs left; its keyboard is no longer watched", thread);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// The tab window
// ---------------------------------------------------------------------------------------------------------------

// The tab is gone: its folder joins the history.
void OnTabDestroyed(HWND hTab)
{
    Pidl folder;
    size_t remembered = 0;
    bool known = false;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        auto it = g_tabs.find(hTab);
        if (it != g_tabs.end())
        {
            known = true;
            folder = std::move(it->second.folder);
            const DWORD thread = it->second.thread;
            g_tabs.erase(it);
            if (g_enabled.load(std::memory_order_relaxed))
            {
                PushClosedLocked(Pidl(folder));
            }
            remembered = g_closed.size();
            DropKeyboardHookIfUnusedLocked(thread);
        }
    }

    if (known && SP_LogEnabled(SP_LOG_DEBUG))
    {
        SP_LogDebug(L"Tab %p closed at %s; %u remembered", hTab, Describe(folder).c_str(), (unsigned)remembered);
    }
}

// Ctrl+Shift+T landed on this window: take the most recent folder off the stack, note that a new tab is
// expected here, and ask the browser for one the same way Ctrl+T does. The new tab's first view arrives
// through the FileCabinet_CreateViewWindow2 hook, which is where the folder is handed to it.
void RestoreInto(HWND hTab)
{
    if (!g_enabled.load(std::memory_order_relaxed) || !IsWindow(hTab))
    {
        return;
    }

    HWND frame = GetAncestor(hTab, GA_ROOT);
    Pidl folder;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        ExpirePendingLocked();
        if (g_pending.frame)
        {
            SP_LogDebug(L"A restore is still in progress; this press is ignored");
            return;
        }
        if (g_closed.empty())
        {
            return;
        }
        folder = std::move(g_closed.back());
        g_closed.pop_back();

        g_pending.frame = frame;
        g_pending.folder = folder;
        g_pending.requested = GetTickCount();
    }

    SP_Log(L"Reopening %s in window %p", Describe(folder).c_str(), frame);
    SendMessageW(hTab, WM_COMMAND, MAKEWPARAM(kNewTabCommand, 0), 0);
}

// The new tab has shown its first view; now it goes where the closed one was. This runs as a posted message,
// so the browser has finished creating the tab before it is asked to navigate.
void NavigatePending(HWND hTab)
{
    IShellBrowser* browser = nullptr;
    Pidl folder;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        auto it = g_tabs.find(hTab);
        if (it == g_tabs.end() || it->second.navigateTo.empty())
        {
            return;
        }
        browser = it->second.browser;
        folder = std::move(it->second.navigateTo);
        it->second.navigateTo.clear();
    }

    if (!browser || !IsWindow(hTab))
    {
        return;
    }

    HRESULT hr = browser->BrowseObject(AsPidl(folder), SBSP_SAMEBROWSER | SBSP_ABSOLUTE);
    if (FAILED(hr))
    {
        SP_LogError(L"The reopened tab could not be taken to %s: 0x%08X", Describe(folder).c_str(), (unsigned)hr);
    }
    else
    {
        SP_LogDebug(L"Tab %p taken to %s", hTab, Describe(folder).c_str());
    }
}

LRESULT CALLBACK TabSubclass(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                             UINT_PTR idSubclass, DWORD_PTR refData)
{
    UNREFERENCED_PARAMETER(idSubclass);
    UNREFERENCED_PARAMETER(refData);

    if (uMsg == WM_NCDESTROY)
    {
        OnTabDestroyed(hWnd);
        RemoveWindowSubclass(hWnd, TabSubclass, kSubclassId);
        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }
    if (g_msgRestore && uMsg == g_msgRestore)
    {
        RestoreInto(hWnd);
        return 0;
    }
    if (g_msgNavigate && uMsg == g_msgNavigate)
    {
        NavigatePending(hWnd);
        return 0;
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

// Starts following a tab window. `browser` and `folder` may be empty for a tab adopted at start-up; they are
// filled in by its next navigation. Returns true when the tab is new to the mod.
bool Track(HWND hTab, HWND hFrame, DWORD thread, IShellBrowser* browser, Pidl&& folder, bool onOwnThread)
{
    DWORD_PTR unused = 0;
    if (!GetWindowSubclass(hTab, TabSubclass, kSubclassId, &unused))
    {
        BOOL ok = onOwnThread ? SetWindowSubclass(hTab, TabSubclass, kSubclassId, 0)
                              : SP_SetWindowSubclassFromAnyThread(hTab, TabSubclass, kSubclassId, 0);
        if (!ok)
        {
            SP_LogError(L"Tab window %p could not be watched: %lu", hTab, GetLastError());
            return false;
        }
    }

    bool isNew = false;
    bool navigate = false;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        auto it = g_tabs.find(hTab);
        if (it == g_tabs.end())
        {
            isNew = true;
            it = g_tabs.emplace(hTab, TabState{}).first;
            it->second.frame = hFrame;
            it->second.thread = thread;
        }
        if (browser)
        {
            it->second.browser = browser;
        }
        if (!folder.empty())
        {
            it->second.folder = std::move(folder);
        }
        EnsureKeyboardHookLocked(thread);

        // A tab that appears in the window a restore was asked for, while the request is fresh, is the one the
        // restore made.
        ExpirePendingLocked();
        if (isNew && browser && g_pending.frame == hFrame)
        {
            it->second.navigateTo = std::move(g_pending.folder);
            g_pending = PendingRestore{};
            navigate = true;
        }
    }

    if (navigate)
    {
        PostMessageW(hTab, g_msgNavigate, 0, 0);
    }
    return isNew;
}

// ---------------------------------------------------------------------------------------------------------------
// The hook
// ---------------------------------------------------------------------------------------------------------------

using CreateViewWindow2_t = HRESULT(__cdecl*)(IShellBrowser* browser, void* folderSettings, IShellView* newView,
                                              IShellView* previousView, RECT* bounds, HWND* result);
CreateViewWindow2_t g_origCreateViewWindow2 = nullptr;

HRESULT __cdecl CreateViewWindow2_Hook(IShellBrowser* browser, void* folderSettings, IShellView* newView,
                                       IShellView* previousView, RECT* bounds, HWND* result)
{
    HRESULT hr = g_origCreateViewWindow2(browser, folderSettings, newView, previousView, bounds, result);

    if (FAILED(hr) || !browser || !g_enabled.load(std::memory_order_relaxed))
    {
        return hr;
    }

    // The desktop and the panes of dialogs come through here too; only a tab inside an Explorer window is
    // of interest, and that is told by the two window classes.
    HWND hTab = nullptr;
    if (FAILED(browser->GetWindow(&hTab)) || !hTab || !ClassIs(hTab, kTabClass))
    {
        return hr;
    }
    HWND hFrame = GetAncestor(hTab, GA_ROOT);
    if (!hFrame || !ClassIs(hFrame, kFrameClass))
    {
        return hr;
    }

    Pidl folder = FolderOf(newView);
    if (folder.empty())
    {
        SP_LogDebug(L"The folder of the new view in tab %p is not known; the last one is kept", hTab);
    }
    else if (SP_LogEnabled(SP_LOG_DEBUG))
    {
        SP_LogDebug(L"Tab %p shows %s", hTab, Describe(folder).c_str());
    }

    if (Track(hTab, hFrame, GetCurrentThreadId(), browser, std::move(folder), true))
    {
        SP_LogDebug(L"Watching tab %p of window %p", hTab, hFrame);
    }
    return hr;
}

// ---------------------------------------------------------------------------------------------------------------
// Windows already open
//
// A window opened before the mod loaded has not been through the hook. Its tabs are adopted so that Ctrl+Shift+T
// works in it at once; their folders are learned on their next navigation, so a tab that is closed before that
// is not remembered. That is the only gap, and it closes by itself.
// ---------------------------------------------------------------------------------------------------------------

BOOL CALLBACK AdoptTab(HWND hChild, LPARAM lParam)
{
    if (ClassIs(hChild, kTabClass))
    {
        HWND hFrame = reinterpret_cast<HWND>(lParam);
        DWORD thread = GetWindowThreadProcessId(hChild, nullptr);
        if (Track(hChild, hFrame, thread, nullptr, Pidl{}, false))
        {
            SP_LogDebug(L"Adopted tab %p of window %p", hChild, hFrame);
        }
    }
    return TRUE;
}

BOOL CALLBACK AdoptWindow(HWND hWnd, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    DWORD process = 0;
    GetWindowThreadProcessId(hWnd, &process);
    if (process == GetCurrentProcessId() && ClassIs(hWnd, kFrameClass))
    {
        EnumChildWindows(hWnd, AdoptTab, reinterpret_cast<LPARAM>(hWnd));
    }
    return TRUE;
}

// ---------------------------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------------------------

void LoadSettings()
{
    g_enabled.store(SP_GetIntSetting(L"Enabled", 1) != 0, std::memory_order_relaxed);
}

BOOL Init()
{
    LoadSettings();

    g_msgRestore = RegisterWindowMessageW(L"ShadePatcher." SP_MOD_ID ".Restore");
    g_msgNavigate = RegisterWindowMessageW(L"ShadePatcher." SP_MOD_ID ".Navigate");
    if (!g_msgRestore || !g_msgNavigate)
    {
        SP_LogError(L"The window messages could not be registered");
        return FALSE;
    }

    HMODULE hFrame = GetModuleHandleW(L"ExplorerFrame.dll");
    if (!hFrame)
    {
        hFrame = LoadLibraryW(L"ExplorerFrame.dll");
    }
    if (!hFrame)
    {
        SP_LogError(L"ExplorerFrame.dll could not be loaded");
        return FALSE;
    }

    static const wchar_t* const kNames[] =
    {
        L"long __cdecl FileCabinet_CreateViewWindow2(struct IShellBrowser *,struct tagFolderSetDataBase *,struct IShellView *,struct IShellView *,struct tagRECT *,struct HWND__ * *)",
    };

    SP_SymbolHook hooks[1] = {};
    hooks[0].symbols = kNames;
    hooks[0].symbolCount = ARRAYSIZE(kNames);
    hooks[0].pOriginal = (void**)&g_origCreateViewWindow2;
    hooks[0].hookFunction = (void*)CreateViewWindow2_Hook;
    hooks[0].optional = FALSE;

    if (!SP_HookSymbols(hFrame, hooks, ARRAYSIZE(hooks)))
    {
        SP_LogError(L"The view window builder was not found in this build");
        return FALSE;
    }

    SP_Log(L"Watching for tabs; Ctrl+Shift+T reopens the last closed one");
    return TRUE;
}

void AfterInit()
{
    EnumWindows(AdoptWindow, 0);
}

void BeforeUninit()
{
    // From here on nothing is recorded and no key is taken.
    g_enabled.store(false, std::memory_order_relaxed);

    // The subclasses come off first, on each window's own thread, and without the lock held: a tab thread may
    // be waiting for the lock inside the hook, and it has to get it to reach the message this sends.
    std::vector<HWND> tabs;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        for (const auto& entry : g_tabs)
        {
            tabs.push_back(entry.first);
        }
    }
    for (HWND hTab : tabs)
    {
        if (IsWindow(hTab) && !SP_RemoveWindowSubclassFromAnyThread(hTab, TabSubclass, kSubclassId))
        {
            SP_LogError(L"Tab window %p kept its subclass", hTab);
        }
    }

    std::lock_guard<std::mutex> guard(g_lock);
    for (const auto& entry : g_keyboardHooks)
    {
        UnhookWindowsHookEx(entry.second);
    }
    g_keyboardHooks.clear();
    g_tabs.clear();
    g_closed.clear();
    g_pending = PendingRestore{};
}

}   // namespace

SP_MOD_DEFINE(g_modExplorerReopenClosedTab) =
{
    /* id             */ SP_MOD_ID,
    /* name           */ L"Reopen the last closed File Explorer tab with Ctrl+Shift+T",
    /* basedOn        */ "file-explorer-reopen-closed-tab",
    /* originalAuthor */ "Armaninyow",
    /* targets        */ SP_TARGET_EXPLORER,
    /* minOsBuild     */ 22621,     // File Explorer tabs arrived with 22H2
    /* flags          */ 0,
    /* Init           */ Init,
    /* AfterInit      */ AfterInit,
    /* SettingsChanged*/ LoadSettings,
    /* BeforeUninit   */ BeforeUninit,
    /* Uninit         */ nullptr,
};
