//
// hide-home-gallery-explorer - hide Home, Gallery, OneDrive and any other named entry from the navigation pane.
//
// Adapted from the idea behind the Windhawk mod "Hide Home, Gallery & OneDrive in Explorer"
// (hide-home-gallery-explorer) by rinosaur681. The implementation here is written against this engine's API.
//
// What the original does
// ----------------------
// A worker thread wakes up every 300 ms, finds the visible Explorer windows, locates the navigation pane's tree
// control in each and deletes every item whose text is "Home" or "Gallery" or contains "OneDrive". The three
// labels are settings, so a user with a non-English Windows has to type the translated names in by hand.
//
// What this does instead
// ----------------------
// The navigation pane is still a plain Win32 tree (class SysTreeView32) on Windows 11, one per tab, created by
// the shell's namespace tree control (class NamespaceTreeControl) inside the CabinetWClass frame. This mod runs
// inside explorer.exe, so it does not need to look for it:
//
//   * CreateWindowExW in user32 is hooked. When a SysTreeView32 is born inside a NamespaceTreeControl that
//     belongs to an Explorer frame, it is subclassed right there, on its own thread.
//   * The subclass watches TVM_INSERTITEM. Items arrive in bursts (the pane is filled, refreshed after F5,
//     re-populated when a folder is expanded), so each insert restarts a short timer and the pruning happens
//     once, on the tree's own thread, when the burst has settled. No polling, and nothing is done on a pane that
//     has nothing to hide.
//   * The names of Home and Gallery are read from the shell namespace itself (their ::{CLSID} items), so the mod
//     matches "Giris" on a Turkish system as well as "Home" on an English one. The English names stay in the
//     list as a fallback. OneDrive is a brand and is not translated, so any entry containing "OneDrive" is
//     matched, which also covers "OneDrive - Personal" and a second business account.
//   * Custom labels are one string setting, separated by semicolons; a leading or trailing * makes a label a
//     "contains" or "starts with" match instead of an exact one. Matching is never case sensitive.
//
// Deleting a tree item is exactly what the original does; nothing is written to the registry and nothing in the
// shell is patched. A deleted entry comes back when the window is reopened, which is also how turning the mod
// off is undone.
//
#define SP_MOD_ID "hide-home-gallery-explorer"
#include "engine/modapi.h"

#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <atomic>
#include <cwctype>
#include <string>
#include <vector>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")

namespace {

constexpr wchar_t kFrameClass[] = L"CabinetWClass";           // a File Explorer top-level window
constexpr wchar_t kNscClass[]   = L"NamespaceTreeControl";    // the shell's tree host, parent of the tree
constexpr wchar_t kTreeClass[]  = L"SysTreeView32";           // the navigation pane itself

// The two entries whose display name is localized. Their names are read from the shell at load time.
constexpr wchar_t kHomeParsingName[]    = L"::{f874310e-b6b7-47dc-bc84-b9e6b38f5903}";
constexpr wchar_t kGalleryParsingName[] = L"::{e88865ea-0e1c-4e20-9aa6-edcd0212c87c}";

constexpr UINT_PTR kSubclassId = 1;
constexpr UINT_PTR kTimerId    = 0x53504E56;    // 'SPNV': far from the ids the tree control uses itself
constexpr UINT     kSettleMs   = 60;            // how long after the last insert the pane is pruned
constexpr int      kMaxItems   = 4000;          // items examined per prune, so a huge tree can never stall the pane
constexpr DWORD    kDetachMs   = 2000;          // how long the engine thread waits for a pane to let go

// Private messages the engine thread sends to a subclassed tree. Registered rather than WM_APP + n so they can
// never collide with anything the tree control or the shell uses.
UINT g_msgPrune  = 0;
UINT g_msgDetach = 0;

// Whether panes are pruned. Follows the Enabled setting, so turning the mod off stops it at once; cleared for good
// in BeforeUninit. Read on the tree's thread.
std::atomic<bool> g_enabled{ true };

// Whether new panes are still subclassed. A pane is watched even while pruning is off, so that turning the mod
// back on needs no restart; only the teardown in BeforeUninit stops it, so no pane is ever left subclassed.
std::atomic<bool> g_watching{ false };

// ---------------------------------------------------------------------------------------------------------------
// Rules: what to hide
//
// Built on the engine thread from the settings, read on whatever thread owns a pane, so the list is swapped
// under a lock and copied out before use.
// ---------------------------------------------------------------------------------------------------------------

enum class Match
{
    Exact,      // the whole label
    Prefix,     // "label*"
    Contains,   // "*label*"
};

struct Rule
{
    std::wstring text;
    Match        match;
};

SRWLOCK           g_rulesLock = SRWLOCK_INIT;
std::vector<Rule> g_rules;                      // guarded by g_rulesLock
std::atomic<int>  g_ruleCount{ 0 };             // mirror of g_rules.size(), so a pane can skip the lock

std::vector<Rule> SnapshotRules()
{
    AcquireSRWLockShared(&g_rulesLock);
    std::vector<Rule> copy = g_rules;
    ReleaseSRWLockShared(&g_rulesLock);
    return copy;
}

void PublishRules(std::vector<Rule>& rules)
{
    AcquireSRWLockExclusive(&g_rulesLock);
    g_rules.swap(rules);
    g_ruleCount.store((int)g_rules.size(), std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&g_rulesLock);
}

bool EqualsNoCase(const wchar_t* a, int lenA, const wchar_t* b, int lenB)
{
    return CompareStringOrdinal(a, lenA, b, lenB, TRUE) == CSTR_EQUAL;
}

bool RuleMatches(const Rule& rule, const std::wstring& text)
{
    const int needle = (int)rule.text.size();
    const int hay = (int)text.size();
    if (needle == 0 || hay < needle)
    {
        return false;
    }

    switch (rule.match)
    {
    case Match::Exact:
        return hay == needle && EqualsNoCase(text.c_str(), hay, rule.text.c_str(), needle);

    case Match::Prefix:
        return EqualsNoCase(text.c_str(), needle, rule.text.c_str(), needle);

    case Match::Contains:
        for (int at = 0; at + needle <= hay; ++at)
        {
            if (EqualsNoCase(text.c_str() + at, needle, rule.text.c_str(), needle))
            {
                return true;
            }
        }
        return false;
    }
    return false;
}

bool AnyRuleMatches(const std::vector<Rule>& rules, const std::wstring& text)
{
    if (text.empty())
    {
        return false;
    }
    for (const Rule& rule : rules)
    {
        if (RuleMatches(rule, text))
        {
            return true;
        }
    }
    return false;
}

void AddRule(std::vector<Rule>& rules, const std::wstring& text, Match match)
{
    if (text.empty())
    {
        return;
    }
    for (const Rule& existing : rules)
    {
        if (existing.match == match && existing.text.size() == text.size() &&
            EqualsNoCase(existing.text.c_str(), (int)existing.text.size(), text.c_str(), (int)text.size()))
        {
            return;     // already there, in some spelling
        }
    }
    rules.push_back({ text, match });
}

std::wstring Trim(const std::wstring& s)
{
    size_t first = 0;
    size_t last = s.size();
    while (first < last && iswspace(s[first]))
    {
        ++first;
    }
    while (last > first && iswspace(s[last - 1]))
    {
        --last;
    }
    return s.substr(first, last - first);
}

// "Network;Linux;*Drive*;Music*" -> one rule per label. A bare "*" is ignored rather than hiding everything.
void AddCustomLabels(std::vector<Rule>& rules, const wchar_t* list)
{
    std::wstring token;
    const wchar_t* p = list ? list : L"";

    for (;; ++p)
    {
        const wchar_t ch = *p;
        if (ch != L';' && ch != L'\0')
        {
            token.push_back(ch);
            continue;
        }

        std::wstring label = Trim(token);
        token.clear();

        if (!label.empty())
        {
            Match match = Match::Exact;
            const bool leading = label.front() == L'*';
            const bool trailing = label.size() > 1 && label.back() == L'*';

            if (leading && trailing)
            {
                match = Match::Contains;
                label = Trim(label.substr(1, label.size() - 2));
            }
            else if (trailing)
            {
                match = Match::Prefix;
                label = Trim(label.substr(0, label.size() - 1));
            }
            else if (leading)
            {
                // "*Drive" means the same as "*Drive*" to anyone typing it; treated as contains.
                match = Match::Contains;
                label = Trim(label.substr(1));
            }

            AddRule(rules, label, match);
        }

        if (ch == L'\0')
        {
            break;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Localized names from the shell namespace
// ---------------------------------------------------------------------------------------------------------------

// COM may or may not be initialized on the engine thread. Initializing it here is harmless when it already is
// (S_FALSE), and when it is initialized in the other apartment model the shell item calls below still work.
struct ComScope
{
    HRESULT hr;
    ComScope() : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}
    ~ComScope()
    {
        if (SUCCEEDED(hr))
        {
            CoUninitialize();
        }
    }
};

// The display name the navigation pane shows for a namespace item, or empty when the item does not exist on this
// system (Gallery is not registered everywhere, OneDrive only when it is set up).
std::wstring NamespaceItemName(const wchar_t* parsingName)
{
    std::wstring name;

    IShellItem* item = nullptr;
    HRESULT hr = SHCreateItemFromParsingName(parsingName, nullptr, IID_PPV_ARGS(&item));
    if (FAILED(hr) || !item)
    {
        SP_LogDebug(L"%s is not present on this system (0x%08X)", parsingName, hr);
        return name;
    }

    PWSTR display = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &display)) && display)
    {
        name = display;
        CoTaskMemFree(display);
    }
    item->Release();
    return name;
}

// ---------------------------------------------------------------------------------------------------------------
// Panes being watched
// ---------------------------------------------------------------------------------------------------------------

SRWLOCK           g_treesLock = SRWLOCK_INIT;
std::vector<HWND> g_trees;                      // guarded by g_treesLock

void RememberTree(HWND hTree)
{
    AcquireSRWLockExclusive(&g_treesLock);
    bool known = false;
    for (HWND h : g_trees)
    {
        if (h == hTree)
        {
            known = true;
            break;
        }
    }
    if (!known)
    {
        g_trees.push_back(hTree);
    }
    ReleaseSRWLockExclusive(&g_treesLock);
}

void ForgetTree(HWND hTree)
{
    AcquireSRWLockExclusive(&g_treesLock);
    for (size_t i = 0; i < g_trees.size(); ++i)
    {
        if (g_trees[i] == hTree)
        {
            g_trees.erase(g_trees.begin() + i);
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_treesLock);
}

std::vector<HWND> SnapshotTrees()
{
    AcquireSRWLockShared(&g_treesLock);
    std::vector<HWND> copy = g_trees;
    ReleaseSRWLockShared(&g_treesLock);
    return copy;
}

bool HasClass(HWND hWnd, const wchar_t* className)
{
    wchar_t buffer[64] = {};
    return hWnd && GetClassNameW(hWnd, buffer, ARRAYSIZE(buffer)) > 0 && _wcsicmp(buffer, className) == 0;
}

// A SysTreeView32 whose parent is the shell's namespace tree host, inside an Explorer frame. The same tree control
// is used by the file dialogs, whose root is a plain dialog, so those are left alone as the original does.
bool IsNavigationPane(HWND hWnd)
{
    if (!HasClass(hWnd, kTreeClass))
    {
        return false;
    }
    if (!HasClass(GetParent(hWnd), kNscClass))
    {
        return false;
    }
    return HasClass(GetAncestor(hWnd, GA_ROOT), kFrameClass);
}

// ---------------------------------------------------------------------------------------------------------------
// Pruning, always on the tree's own thread
// ---------------------------------------------------------------------------------------------------------------

HTREEITEM NextItem(HWND hTree, UINT relation, HTREEITEM from)
{
    return (HTREEITEM)SendMessageW(hTree, TVM_GETNEXTITEM, relation, (LPARAM)from);
}

// The pane fills its labels on demand (LPSTR_TEXTCALLBACK), so asking for the text makes the tree ask the shell
// for it, which is what happens when the item is painted.
std::wstring ItemText(HWND hTree, HTREEITEM item)
{
    wchar_t buffer[MAX_PATH] = {};
    TVITEMW tvi = {};
    tvi.mask = TVIF_TEXT;
    tvi.hItem = item;
    tvi.pszText = buffer;
    tvi.cchTextMax = ARRAYSIZE(buffer);
    if (!SendMessageW(hTree, TVM_GETITEMW, 0, (LPARAM)&tvi))
    {
        return {};
    }
    return std::wstring(buffer);
}

// Walks the tree from `first` across its siblings and down into children, collecting the items to delete. A
// matching item is recorded and its subtree is not entered: deleting it takes the children with it, and no
// recorded item is ever a descendant of another, so they can be deleted in any order.
void CollectMatches(HWND hTree, HTREEITEM first, const std::vector<Rule>& rules,
                    std::vector<HTREEITEM>& doomed, int& budget)
{
    for (HTREEITEM item = first; item && budget > 0; item = NextItem(hTree, TVGN_NEXT, item))
    {
        --budget;

        if (AnyRuleMatches(rules, ItemText(hTree, item)))
        {
            doomed.push_back(item);
            continue;
        }

        HTREEITEM child = NextItem(hTree, TVGN_CHILD, item);
        if (child)
        {
            CollectMatches(hTree, child, rules, doomed, budget);
        }
    }
}

// TRUE when `item` or one of its ancestors is about to be deleted.
bool IsDoomed(HWND hTree, HTREEITEM item, const std::vector<HTREEITEM>& doomed)
{
    for (HTREEITEM at = item; at; at = NextItem(hTree, TVGN_PARENT, at))
    {
        for (HTREEITEM d : doomed)
        {
            if (d == at)
            {
                return true;
            }
        }
    }
    return false;
}

thread_local bool t_pruning = false;

void Prune(HWND hTree)
{
    if (!g_enabled.load(std::memory_order_relaxed) || g_ruleCount.load(std::memory_order_relaxed) == 0)
    {
        return;
    }
    if (t_pruning)
    {
        return;     // a delete made the shell insert something; the timer will come back to it
    }

    struct Guard
    {
        Guard() { t_pruning = true; }
        ~Guard() { t_pruning = false; }
    } guard;

    const std::vector<Rule> rules = SnapshotRules();

    HTREEITEM root = NextItem(hTree, TVGN_ROOT, nullptr);
    if (!root)
    {
        return;
    }

    // Dry run first: a pane with nothing to hide is not touched, so it never flickers.
    std::vector<HTREEITEM> doomed;
    int budget = kMaxItems;
    CollectMatches(hTree, root, rules, doomed, budget);
    if (doomed.empty())
    {
        return;
    }

    // Deleting items makes the tree scroll to keep its caret in view, which is the "pane jumps to the bottom"
    // the original fought. The item at the top of the view is remembered and put back afterwards; when it is
    // one of the deleted ones, the pane is scrolled to its first item instead.
    HTREEITEM topBefore = NextItem(hTree, TVGN_FIRSTVISIBLE, nullptr);
    const bool keepTop = topBefore && !IsDoomed(hTree, topBefore, doomed);

    SendMessageW(hTree, WM_SETREDRAW, FALSE, 0);
    int deleted = 0;
    for (HTREEITEM item : doomed)
    {
        if (SendMessageW(hTree, TVM_DELETEITEM, 0, (LPARAM)item))
        {
            ++deleted;
        }
    }
    SendMessageW(hTree, WM_SETREDRAW, TRUE, 0);

    HTREEITEM anchor = keepTop ? topBefore : NextItem(hTree, TVGN_ROOT, nullptr);
    if (anchor)
    {
        // TVGN_FIRSTVISIBLE scrolls without moving the selection, so the shell sees no selection change and does
        // not navigate anywhere.
        SendMessageW(hTree, TVM_SELECTITEM, TVGN_FIRSTVISIBLE, (LPARAM)anchor);
    }
    RedrawWindow(hTree, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);

    SP_LogDebug(L"Removed %d item(s) from navigation pane %p", deleted, hTree);
}

// ---------------------------------------------------------------------------------------------------------------
// The subclass
// ---------------------------------------------------------------------------------------------------------------

LRESULT CALLBACK TreeSubclass(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                              UINT_PTR idSubclass, DWORD_PTR refData)
{
    UNREFERENCED_PARAMETER(idSubclass);
    UNREFERENCED_PARAMETER(refData);

    if (uMsg == WM_TIMER && wParam == kTimerId)
    {
        KillTimer(hWnd, kTimerId);
        Prune(hWnd);
        return 0;
    }

    if (g_msgPrune && uMsg == g_msgPrune)
    {
        KillTimer(hWnd, kTimerId);
        Prune(hWnd);
        return 0;
    }

    if (g_msgDetach && uMsg == g_msgDetach)
    {
        KillTimer(hWnd, kTimerId);
        RemoveWindowSubclass(hWnd, TreeSubclass, kSubclassId);
        ForgetTree(hWnd);
        return 0;
    }

    switch (uMsg)
    {
    case TVM_INSERTITEMA:
    case TVM_INSERTITEMW:
    {
        LRESULT inserted = DefSubclassProc(hWnd, uMsg, wParam, lParam);
        if (inserted && g_enabled.load(std::memory_order_relaxed) &&
            g_ruleCount.load(std::memory_order_relaxed) > 0)
        {
            // Restarted on every insert, so a burst of them ends in one prune.
            SetTimer(hWnd, kTimerId, kSettleMs, nullptr);
        }
        return inserted;
    }

    case WM_NCDESTROY:
        KillTimer(hWnd, kTimerId);
        RemoveWindowSubclass(hWnd, TreeSubclass, kSubclassId);
        ForgetTree(hWnd);
        break;
    }

    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

// Subclasses a pane. `ownThread` is TRUE when called on the thread that owns the window (from the hook); from the
// engine thread the call is carried over by the engine.
bool Watch(HWND hTree, bool ownThread)
{
    if (!hTree || !IsWindow(hTree))
    {
        return false;
    }

    DWORD_PTR existing = 0;
    if (GetWindowSubclass(hTree, TreeSubclass, kSubclassId, &existing))
    {
        return true;
    }

    BOOL ok = ownThread ? SetWindowSubclass(hTree, TreeSubclass, kSubclassId, 0)
                        : SP_SetWindowSubclassFromAnyThread(hTree, TreeSubclass, kSubclassId, 0);
    if (!ok)
    {
        SP_LogError(L"Navigation pane %p could not be watched: %lu", hTree, GetLastError());
        return false;
    }

    RememberTree(hTree);
    SP_LogDebug(L"Watching navigation pane %p", hTree);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// The hook: a pane is caught the moment it is created
// ---------------------------------------------------------------------------------------------------------------

using CreateWindowExW_t = decltype(&CreateWindowExW);
CreateWindowExW_t g_origCreateWindowExW = nullptr;

HWND WINAPI CreateWindowExW_Hook(DWORD dwExStyle, LPCWSTR lpClassName, LPCWSTR lpWindowName, DWORD dwStyle,
                                 int X, int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu,
                                 HINSTANCE hInstance, LPVOID lpParam)
{
    HWND hWnd = g_origCreateWindowExW(dwExStyle, lpClassName, lpWindowName, dwStyle, X, Y, nWidth, nHeight,
                                      hWndParent, hMenu, hInstance, lpParam);

    // Cheap early exit for everything that is not a tree control created as a child. The class may be an atom,
    // which is never the tree class the shell asks for by name.
    if (hWnd && hWndParent && lpClassName && !IS_INTRESOURCE(lpClassName) &&
        g_watching.load(std::memory_order_relaxed) && _wcsicmp(lpClassName, kTreeClass) == 0 &&
        IsNavigationPane(hWnd))
    {
        Watch(hWnd, true);
    }

    return hWnd;
}

// ---------------------------------------------------------------------------------------------------------------
// Panes that already exist when the mod starts
// ---------------------------------------------------------------------------------------------------------------

BOOL CALLBACK OnChildWindow(HWND hWnd, LPARAM lParam)
{
    if (IsNavigationPane(hWnd) && Watch(hWnd, false))
    {
        PostMessageW(hWnd, g_msgPrune, 0, 0);
        ++*(int*)lParam;
    }
    return TRUE;
}

BOOL CALLBACK OnTopLevelWindow(HWND hWnd, LPARAM lParam)
{
    if (!HasClass(hWnd, kFrameClass))
    {
        return TRUE;
    }

    // Only frames of this process: a folder window opened in a separate explorer.exe cannot be subclassed from
    // here, and the hook does not see it either.
    DWORD pid = 0;
    GetWindowThreadProcessId(hWnd, &pid);
    if (pid != GetCurrentProcessId())
    {
        return TRUE;
    }

    EnumChildWindows(hWnd, OnChildWindow, lParam);
    return TRUE;
}

int WatchExistingPanes()
{
    int found = 0;
    EnumWindows(OnTopLevelWindow, (LPARAM)&found);
    return found;
}

// ---------------------------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------------------------

void LoadSettings()
{
    g_enabled.store(SP_GetIntSetting(L"Enabled", 1) != 0, std::memory_order_relaxed);

    const bool hideHome     = SP_GetIntSetting(L"HideHome", 1) != 0;
    const bool hideGallery  = SP_GetIntSetting(L"HideGallery", 1) != 0;
    const bool hideOneDrive = SP_GetIntSetting(L"HideOneDrive", 1) != 0;

    wchar_t custom[1024] = {};
    SP_GetStringSetting(L"CustomLabels", custom, ARRAYSIZE(custom), L"");

    std::vector<Rule> rules;

    if (hideHome || hideGallery)
    {
        ComScope com;
        if (hideHome)
        {
            AddRule(rules, NamespaceItemName(kHomeParsingName), Match::Exact);
            AddRule(rules, L"Home", Match::Exact);
        }
        if (hideGallery)
        {
            AddRule(rules, NamespaceItemName(kGalleryParsingName), Match::Exact);
            AddRule(rules, L"Gallery", Match::Exact);
        }
    }

    if (hideOneDrive)
    {
        // "OneDrive", "OneDrive - Personal", "OneDrive - Contoso": the brand is never translated.
        AddRule(rules, L"OneDrive", Match::Contains);
    }

    AddCustomLabels(rules, custom);

    const int count = (int)rules.size();
    PublishRules(rules);

    SP_Log(L"Hiding home=%d gallery=%d onedrive=%d, custom labels \"%s\": %d rule(s)",
           hideHome, hideGallery, hideOneDrive, custom, count);
}

BOOL Init()
{
    g_msgPrune  = RegisterWindowMessageW(L"ShadePatcher." SP_MOD_ID ".Prune");
    g_msgDetach = RegisterWindowMessageW(L"ShadePatcher." SP_MOD_ID ".Detach");
    if (!g_msgPrune || !g_msgDetach)
    {
        SP_LogError(L"The private messages could not be registered: %lu", GetLastError());
        return FALSE;
    }

    LoadSettings();

    if (!SP_HookBegin())
    {
        return FALSE;
    }
    if (!SP_SetExportHook(L"user32.dll", "CreateWindowExW", CreateWindowExW_Hook, &g_origCreateWindowExW))
    {
        SP_HookAbort();
        SP_LogError(L"CreateWindowExW could not be hooked");
        return FALSE;
    }
    if (!SP_HookCommit())
    {
        SP_LogError(L"The hook transaction failed");
        return FALSE;
    }

    g_watching.store(true, std::memory_order_relaxed);
    SP_Log(L"CreateWindowExW hooked; navigation panes are pruned as they are filled");
    return TRUE;
}

void AfterInit()
{
    // Windows already open never went through the hook.
    int found = WatchExistingPanes();
    SP_Log(L"%d existing navigation pane(s) watched", found);
}

void SettingsChanged()
{
    LoadSettings();

    // A pane the hook missed (none expected, but cheap to check) is picked up here, then the new rules apply to
    // every open pane at once. An entry that is no longer hidden comes back when its window is reopened, exactly
    // as with the original.
    WatchExistingPanes();

    for (HWND hTree : SnapshotTrees())
    {
        if (IsWindow(hTree))
        {
            PostMessageW(hTree, g_msgPrune, 0, 0);
        }
    }
}

void BeforeUninit()
{
    g_watching.store(false, std::memory_order_relaxed);
    g_enabled.store(false, std::memory_order_relaxed);

    // Each pane lets go on its own thread, so its timer is killed where it was set. A pane whose thread does not
    // answer is unhooked by the engine's carrier instead; a stray WM_TIMER with an unknown id is ignored by the
    // tree control.
    for (HWND hTree : SnapshotTrees())
    {
        if (!IsWindow(hTree))
        {
            ForgetTree(hTree);
            continue;
        }

        DWORD_PTR result = 0;
        if (!SendMessageTimeoutW(hTree, g_msgDetach, 0, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, kDetachMs, &result))
        {
            SP_LogDebug(L"Navigation pane %p did not answer; detaching from the engine thread", hTree);
            SP_RemoveWindowSubclassFromAnyThread(hTree, TreeSubclass, kSubclassId);
            ForgetTree(hTree);
        }
    }
}

void Uninit()
{
    std::vector<Rule> none;
    PublishRules(none);

    AcquireSRWLockExclusive(&g_treesLock);
    g_trees.clear();
    ReleaseSRWLockExclusive(&g_treesLock);
}

}   // namespace

SP_MOD_DEFINE(g_modExplorerHideNavItems) =
{
    /* id             */ SP_MOD_ID,
    /* name           */ L"Hide Home, Gallery and OneDrive from the File Explorer navigation pane",
    /* basedOn        */ "hide-home-gallery-explorer",
    /* originalAuthor */ "rinosaur681",
    /* targets        */ SP_TARGET_EXPLORER,
    /* minOsBuild     */ 22000,     // Home and Gallery are Windows 11 navigation pane entries
    /* flags          */ 0,
    /* Init           */ Init,
    /* AfterInit      */ AfterInit,
    /* SettingsChanged*/ SettingsChanged,
    /* BeforeUninit   */ BeforeUninit,
    /* Uninit         */ Uninit,
};
