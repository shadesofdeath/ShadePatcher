#pragma once
//
// catalog.h - what the Start menu shows: installed apps, pinned apps, recent files and the signed-in user.
//
// Everything that touches the disk or the shell namespace runs on the Loader's own thread and arrives on the UI
// thread as an immutable snapshot, so opening the menu never waits for I/O (handoff section 9, "Performans").
//
#include <Windows.h>
#include <ShlObj.h>

#include <memory>
#include <unordered_map>
#include <string>
#include <vector>

namespace sm {

// A decoded image: premultiplied BGRA, top-down, tightly packed. Device-independent, so a lost Direct3D device
// only costs re-uploading these, never re-reading them from the shell.
struct Pixels
{
    UINT width = 0;
    UINT height = 0;
    std::vector<BYTE> bgra;
};
using PixelsPtr = std::shared_ptr<const Pixels>;

// A 32-bit DIB (as IShellItemImageFactory returns) as premultiplied pixels.
PixelsPtr PixelsFromHBitmap(HBITMAP bitmap);

// An icon drawn at `px` x `px`.
PixelsPtr PixelsFromIcon(HICON icon, UINT px);

// An absolute item id list, freed with CoTaskMemFree. Shared between list snapshots.
struct IdList
{
    explicit IdList(PIDLIST_ABSOLUTE p) : value(p) {}
    ~IdList() { CoTaskMemFree(value); }
    IdList(const IdList&) = delete;
    IdList& operator=(const IdList&) = delete;
    PIDLIST_ABSOLUTE value;
};
using IdListPtr = std::shared_ptr<const IdList>;

struct App
{
    std::wstring name;      // display name
    std::wstring id;        // parsing name inside shell:AppsFolder: an AppUserModelID or a path-like id
    std::wstring heading;   // the A-Z heading this app is listed under ("#" for digits and symbols)
    IdListPtr pidl;         // absolute id list, what launching and the context menu use
    PixelsPtr icon;         // null until the icon pass has reached this app
};

enum class FileKind { Document, Image, Code, Media, Archive, Other };

struct RecentFile
{
    std::wstring name;      // file name as shown on the chip
    std::wstring path;
    FileKind kind = FileKind::Other;
};

struct UserInfo
{
    std::wstring name;
    PixelsPtr picture;      // the account picture, or null for the initial on a gradient
};

// All installed apps, sorted the way the user's locale sorts (Turkish rules for a Turkish user).
struct AppList
{
    std::vector<App> apps;
    UINT iconPx = 0;        // pixel size the icons were rendered at
    bool iconsComplete = false;
};

// ---------------------------------------------------------------------------------------------------------------
// The loader thread
// ---------------------------------------------------------------------------------------------------------------

// Results are posted to the window given to Start() with the message given there:
//   wParam = LoadResult, lParam = a heap object the receiver takes ownership of (see TakeResult).
enum class LoadResult : WPARAM
{
    Apps = 1,       // AppList*: posted once with names only, then again when icons are ready
    Recent = 2,     // std::vector<RecentFile>*
    User = 3,       // UserInfo*
    Shortcuts = 4,  // std::vector<ShortcutItem>*
    Files = 5,      // FileResults* (filesearch.h)
    Quick = 6,      // QuickState* (quick.h)
};

class Loader
{
public:
    bool Start(HWND notify, UINT message);
    void Stop();

    // Queue work. Requests coalesce: asking twice before the thread gets to it does the work once.
    // `firstIds` are loaded first in the icon pass (the pinned apps), so the default view fills in soonest.
    void RequestApps(UINT iconPx, std::vector<std::wstring> firstIds);
    void RequestRecent(int maxCount, bool ignoreTrackingSetting = false);
    void RequestUser();
    void RequestShortcuts(unsigned which, UINT iconPx);

private:
    static DWORD WINAPI ThreadProc(void* param);
    void Run();

    HANDLE m_thread = nullptr;
    HANDLE m_wake = nullptr;
    HWND m_notify = nullptr;
    UINT m_message = 0;
    volatile LONG m_stop = 0;

    SRWLOCK m_lock = SRWLOCK_INIT;
    bool m_wantApps = false;
    bool m_wantRecent = false;
    bool m_wantUser = false;
    bool m_wantShortcuts = false;
    unsigned m_shortcuts = 0;
    UINT m_shortcutPx = 0;
    UINT m_iconPx = 0;
    int m_recentMax = 6;
    bool m_recentForce = false;
    std::vector<std::wstring> m_firstIds;
};

// Deletes the object behind a Loader message. The receiver calls this for results it does not keep.
void FreeLoadResult(WPARAM kind, LPARAM object);

// ---------------------------------------------------------------------------------------------------------------
// Pinned apps
// ---------------------------------------------------------------------------------------------------------------

// The ordered list of pinned app ids, kept in %LOCALAPPDATA%\ShadePatcher\pinned.json. UI thread only.
class PinStore
{
public:
    // False when the file does not exist yet; the caller then seeds defaults once the app list is known.
    bool Load();
    bool Save() const;
    static void Delete();   // removes pinned.json, so the next Load starts from the defaults

    const std::vector<std::wstring>& Ids() const { return m_ids; }
    bool IsPinned(const std::wstring& id) const;
    void Pin(const std::wstring& id);
    void Unpin(const std::wstring& id);
    void MoveToFront(const std::wstring& id);
    // Moves `id` to position `index` of the list (drag and drop in the pinned grid).
    void MoveTo(const std::wstring& id, size_t index);

    // Fifteen well-known apps that are installed, in the order the handoff's default grid suggests.
    void SeedDefaults(const AppList& list);

private:
    std::vector<std::wstring> m_ids;
};

// ---------------------------------------------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------------------------------------------

// How often each app was started from the menu, in %LOCALAPPDATA%\ShadePatcher\usage.txt ("count<TAB>id" lines).
// Feeds "Most used". UI thread only.
class UsageStore
{
public:
    void Load();
    void Record(const std::wstring& id);
    // The most started ids, most first, skipping `exclude` (the pinned ones), at most `count`.
    std::vector<std::wstring> Top(size_t count, const PinStore& exclude) const;

private:
    void Save() const;
    std::vector<std::pair<std::wstring, unsigned>> m_counts;
    bool m_loaded = false;
};

// ---------------------------------------------------------------------------------------------------------------
// A list of strings in a text file under %LOCALAPPDATA%\ShadePatcher, one per line. Hidden apps (by id) and
// the search history use it. UI thread only.
// ---------------------------------------------------------------------------------------------------------------

class LineStore
{
public:
    explicit LineStore(const wchar_t* fileName) : m_fileName(fileName) {}
    const std::vector<std::wstring>& Lines();
    bool Contains(const std::wstring& line);
    void Add(const std::wstring& line, size_t keep = 500);   // moved to the front if already there
    void Remove(const std::wstring& line);
    void Clear();
    static void Delete(const wchar_t* fileName);

private:
    void Load();
    void Save() const;
    const wchar_t* m_fileName;
    std::vector<std::wstring> m_lines;
    bool m_loaded = false;
};

// ---------------------------------------------------------------------------------------------------------------
// New apps
// ---------------------------------------------------------------------------------------------------------------

// Apps installed since the menu first saw the list, in %LOCALAPPDATA%\ShadePatcher\known.txt. An app counts as
// new for a week, or until it is started. The first list ever seen is the baseline: nothing in it is new.
class NewApps
{
public:
    void Update(const AppList& list);
    bool IsNew(const std::wstring& id) const;
    void Clear(const std::wstring& id);
    bool Any() const;

private:
    void Save() const;
    std::unordered_map<std::wstring, ULONGLONG> m_seen;   // lower-case id -> first seen (FILETIME), 0 = baseline
    bool m_loaded = false;
};

// ---------------------------------------------------------------------------------------------------------------
// Footer shortcuts
// ---------------------------------------------------------------------------------------------------------------

// In the order they appear; the setting picks a subset (one bit each).
enum class Shortcut { Explorer, Documents, Downloads, Pictures, Music, Videos, UserFolder, Settings, Count };
constexpr unsigned ShortcutBit(Shortcut s) { return 1u << (unsigned)s; }
constexpr unsigned kDefaultShortcuts = ShortcutBit(Shortcut::Explorer) | ShortcutBit(Shortcut::Documents) |
                                       ShortcutBit(Shortcut::Downloads) | ShortcutBit(Shortcut::Settings);

struct ShortcutItem
{
    Shortcut which;
    std::wstring name;   // the shell's display name, for the tooltip
    IdListPtr pidl;
    PixelsPtr icon;
};

// ---------------------------------------------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------------------------------------------

// Indices of the apps whose name contains `query`, case-insensitively under the user's locale (so "i" matches
// "\x0130" for a Turkish user). Names that start with the query come first, then names with a word that starts
// with it, then the rest; each group keeps the list's order.
std::vector<int> SearchApps(const AppList& list, const std::wstring& query);

// ---------------------------------------------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------------------------------------------

bool LaunchApp(const App& app, bool asAdmin);
bool OpenIdList(PCIDLIST_ABSOLUTE pidl);
bool OpenPath(const std::wstring& path);
bool OpenAccountSettings();

// In the order the power menu lists them. The settings pick a subset (ViewOptions::powerItems, one bit each).
// UpdateRestart and UpdateShutDown are only offered while Windows has updates waiting for a restart.
enum class PowerAction
{
    Lock, SignOut, Sleep, Hibernate, UpdateRestart, Restart, UpdateShutDown, ShutDown, AdvancedStartup, Firmware,
    Count
};
constexpr unsigned PowerBit(PowerAction action) { return 1u << (unsigned)action; }
constexpr unsigned kDefaultPowerItems = PowerBit(PowerAction::Lock) | PowerBit(PowerAction::Sleep) |
                                        PowerBit(PowerAction::Restart) | PowerBit(PowerAction::ShutDown);
void RunPowerAction(PowerAction action);

// True when Windows has installed updates that wait for a restart.
bool UpdatesPending();

// Extra entries shown above the shell's own context menu entries.
struct MenuExtra
{
    UINT id;                // returned when chosen; must be below kShellFirstId
    const wchar_t* text;    // null for a separator
    UINT flags = 0;         // MF_GRAYED for a heading
};

// An app's recent documents, as its jump list shows them.
struct JumpItem
{
    std::wstring name;
    IdListPtr pidl;
};
std::vector<JumpItem> RecentItemsOf(const std::wstring& appId, size_t max);

// An image file decoded to premultiplied pixels, scaled down so the longer side is at most `maxSide`.
PixelsPtr LoadImagePixels(const std::wstring& path, UINT maxSide);
constexpr UINT kShellFirstId = 0x100;

// Shows the shell's context menu for an item with `extras` on top and runs the shell command that is picked.
// Returns the id of the extra that was picked, 0 when the shell ran a command, or -1 when nothing was picked.
// The shell's own "Pin to Start" entries are removed: they refer to the Windows Start menu.
int ShowItemContextMenu(HWND owner, POINT screen, PCIDLIST_ABSOLUTE pidl, const std::vector<MenuExtra>& extras);

// For WM_INITMENUPOPUP, WM_DRAWITEM, WM_MEASUREITEM and WM_MENUCHAR while a context menu is up: submenus of the
// shell's menu are drawn by the shell. Returns true when the message was handled.
bool HandleContextMenuMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT* result);

} // namespace sm
