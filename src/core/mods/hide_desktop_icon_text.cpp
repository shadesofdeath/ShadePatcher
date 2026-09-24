//
// hide-desktop-icon-text - hide the labels under desktop icons (folders keep their names) and remove the
// shortcut arrow overlay.
//
// Adapted from the idea behind the Windhawk mod "Hide Desktop Icon Text and Shortcut Arrows"
// (hide-desktop-icon-text) by kivsak. The implementation here is written against this engine's API.
//
// Hiding the labels
// -----------------
// The desktop is an ordinary SysListView32 inside SHELLDLL_DefView, and it draws its labels the way every list
// view does: through DrawTextW / DrawTextExW (user32) and DrawThemeTextEx (uxtheme). Those three are hooked.
// While the desktop list view is inside WM_PAINT, which a subclass records in a thread-local flag, any label
// that is not the name of a folder on the desktop is drawn as an empty string. The list view measures labels
// with the same calls (DT_CALCRECT), so the highlight collapses along with the text. Outside that paint, and on
// every other thread, the hooks only read the flag and pass the call on, so the rest of the shell never pays
// for them.
//
// Folder names come from the desktop namespace (SHGetDesktopFolder), refreshed at most once a second and again
// after the view inserts, removes or renames an item. Only the text is known at draw time, so the match is by
// name: a file that has exactly the same name as a folder keeps its label too, as in the original mod.
//
// The subclass is installed on the existing desktop in AfterInit, and CreateWindowExW is hooked to catch the
// shell recreating the view (a theme change or toggling "Show desktop icons" does that).
//
// Hiding the shortcut arrows
// --------------------------
// The arrow is an overlay in the shared system image lists (SHGetImageList). A fully transparent icon is added
// to each list once and the link overlay slot (SHGetIconOverlayIndexW with IDO_SHGIOI_LINK) is pointed at it.
// The lists are process-wide, so this affects every Explorer window and not only the desktop. When the option
// is turned off the stock link overlay (SIID_LINK) is drawn back into the same image; that is best effort, and
// restarting Explorer always brings the original back.
//
#define SP_MOD_ID "hide-desktop-icon-text"
#include "engine/modapi.h"

#include <shellapi.h>
#include <commctrl.h>
#include <commoncontrols.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <uxtheme.h>

#include <atomic>
#include <string>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "Gdi32.lib")

#ifndef IDO_SHGIOI_LINK
#define IDO_SHGIOI_LINK 0x0FFFFFFE
#endif

namespace {

// Written on the engine thread, read on the desktop's thread and inside the hooks.
std::atomic<bool> g_active{ false };        // FALSE once BeforeUninit ran: the subclass and the hooks stand down
std::atomic<bool> g_hideText{ true };
std::atomic<bool> g_hideArrows{ true };

// TRUE only while the desktop list view is painting, on its own thread. Every draw hook checks this first.
thread_local bool t_paintingDesktop = false;

// Display names of the folders on the desktop. Refreshed and read on the desktop's thread inside WM_PAINT; the
// lock keeps that from being an assumption the hooks depend on.
SRWLOCK g_namesLock = SRWLOCK_INIT;
std::unordered_set<std::wstring> g_folderNames;
std::atomic<DWORD> g_namesTick{ 0 };        // GetTickCount of the last refresh; 0 forces the next one

constexpr DWORD kRefreshIntervalMs = 1000;
constexpr UINT_PTR kSubclassId = 1;

// ---------------------------------------------------------------------------------------------------------------
// Finding the desktop list view
// ---------------------------------------------------------------------------------------------------------------

bool ClassIs(HWND hWnd, const wchar_t* name)
{
    wchar_t wszClass[64];
    return hWnd && GetClassNameW(hWnd, wszClass, ARRAYSIZE(wszClass)) && _wcsicmp(wszClass, name) == 0;
}

// The icon list of the desktop: a SysListView32 inside SHELLDLL_DefView whose parent is Progman, a WorkerW
// (the shell moves the view there in some configurations) or whatever GetShellWindow reports. A File Explorer
// window has a different chain of parents, so its views never match.
bool IsDesktopListView(HWND hWnd)
{
    if (!ClassIs(hWnd, L"SysListView32"))
    {
        return false;
    }

    HWND hView = GetAncestor(hWnd, GA_PARENT);
    if (!ClassIs(hView, L"SHELLDLL_DefView"))
    {
        return false;
    }

    HWND hTop = GetAncestor(hView, GA_PARENT);
    if (!hTop)
    {
        return false;
    }
    return hTop == GetShellWindow() || ClassIs(hTop, L"Progman") || ClassIs(hTop, L"WorkerW");
}

HWND FindDesktopListView()
{
    HWND hProgman = GetShellWindow();
    if (!hProgman)
    {
        hProgman = FindWindowW(L"Progman", nullptr);
    }

    HWND hView = hProgman ? FindWindowExW(hProgman, nullptr, L"SHELLDLL_DefView", nullptr) : nullptr;
    if (!hView)
    {
        HWND hWorker = nullptr;
        while (!hView && (hWorker = FindWindowExW(nullptr, hWorker, L"WorkerW", nullptr)) != nullptr)
        {
            hView = FindWindowExW(hWorker, nullptr, L"SHELLDLL_DefView", nullptr);
        }
    }
    if (!hView)
    {
        return nullptr;
    }

    HWND hList = FindWindowExW(hView, nullptr, L"SysListView32", nullptr);
    if (!hList)
    {
        return nullptr;
    }

    // A subclass can only go on a window of this process.
    DWORD pid = 0;
    GetWindowThreadProcessId(hList, &pid);
    return pid == GetCurrentProcessId() ? hList : nullptr;
}

// Invalidates only; a synchronous UpdateWindow from the engine thread would send into the desktop's thread.
void RepaintDesktop(HWND hList)
{
    if (!hList)
    {
        hList = FindDesktopListView();
    }
    if (hList)
    {
        RedrawWindow(hList, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// The folder name cache
// ---------------------------------------------------------------------------------------------------------------

void RefreshFolderNames()
{
    const DWORD now = GetTickCount();
    const DWORD last = g_namesTick.load(std::memory_order_relaxed);
    if (last != 0 && now - last < kRefreshIntervalMs)
    {
        return;
    }
    g_namesTick.store(now ? now : 1, std::memory_order_relaxed);

    std::unordered_set<std::wstring> fresh;

    IShellFolder* pDesktop = nullptr;
    if (SUCCEEDED(SHGetDesktopFolder(&pDesktop)) && pDesktop)
    {
        IEnumIDList* pEnum = nullptr;
        const SHCONTF what = SHCONTF_FOLDERS | SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN;
        if (SUCCEEDED(pDesktop->EnumObjects(nullptr, what, &pEnum)) && pEnum)
        {
            LPITEMIDLIST pidl = nullptr;
            while (pEnum->Next(1, &pidl, nullptr) == S_OK && pidl)
            {
                // A real folder, not a file the shell can browse into such as a .zip (SFGAO_STREAM).
                SFGAOF attributes = SFGAO_FOLDER | SFGAO_STREAM;
                LPCITEMIDLIST child = pidl;
                if (SUCCEEDED(pDesktop->GetAttributesOf(1, &child, &attributes)) &&
                    (attributes & SFGAO_FOLDER) && !(attributes & SFGAO_STREAM))
                {
                    STRRET name = {};
                    if (SUCCEEDED(pDesktop->GetDisplayNameOf(pidl, SHGDN_NORMAL, &name)))
                    {
                        wchar_t wszName[MAX_PATH];
                        if (SUCCEEDED(StrRetToBufW(&name, pidl, wszName, ARRAYSIZE(wszName))) && wszName[0])
                        {
                            fresh.insert(wszName);
                        }
                    }
                }
                CoTaskMemFree(pidl);
                pidl = nullptr;
            }
            pEnum->Release();
        }
        pDesktop->Release();
    }

    AcquireSRWLockExclusive(&g_namesLock);
    g_folderNames.swap(fresh);
    const size_t count = g_folderNames.size();
    ReleaseSRWLockExclusive(&g_namesLock);

    SP_LogDebug(L"%Iu folder name(s) on the desktop", count);
}

// TRUE for a label the desktop is painting that is not a folder name.
bool ShouldHideLabel(const wchar_t* text, int cch)
{
    if (!text)
    {
        return false;
    }
    const size_t length = cch < 0 ? wcslen(text) : (size_t)cch;
    if (length == 0)
    {
        return false;
    }

    std::wstring name(text, length);

    AcquireSRWLockShared(&g_namesLock);
    const bool isFolder = g_folderNames.count(name) != 0;
    ReleaseSRWLockShared(&g_namesLock);

    return !isFolder;
}

// ---------------------------------------------------------------------------------------------------------------
// The text hooks
//
// Each one replaces the text with an empty string and lets the original do the rest, so the format flags, the
// rectangle and the return value stay exactly what the list view expects. The empty string is a writable local
// because DT_MODIFYSTRING may write a terminator into it.
// ---------------------------------------------------------------------------------------------------------------

using DrawTextW_t = int (WINAPI*)(HDC, LPCWSTR, int, LPRECT, UINT);
using DrawTextExW_t = int (WINAPI*)(HDC, LPWSTR, int, LPRECT, UINT, LPDRAWTEXTPARAMS);
using DrawThemeTextEx_t = HRESULT (WINAPI*)(HTHEME, HDC, int, int, LPCWSTR, int, DWORD, LPRECT, const DTTOPTS*);

DrawTextW_t        g_origDrawTextW = nullptr;
DrawTextExW_t      g_origDrawTextExW = nullptr;
DrawThemeTextEx_t  g_origDrawThemeTextEx = nullptr;

int WINAPI DrawTextW_Hook(HDC hdc, LPCWSTR text, int cch, LPRECT rect, UINT format)
{
    if (t_paintingDesktop && ShouldHideLabel(text, cch))
    {
        wchar_t empty[2] = { 0, 0 };
        return g_origDrawTextW(hdc, empty, 0, rect, format);
    }
    return g_origDrawTextW(hdc, text, cch, rect, format);
}

int WINAPI DrawTextExW_Hook(HDC hdc, LPWSTR text, int cch, LPRECT rect, UINT format, LPDRAWTEXTPARAMS params)
{
    if (t_paintingDesktop && ShouldHideLabel(text, cch))
    {
        wchar_t empty[2] = { 0, 0 };
        return g_origDrawTextExW(hdc, empty, 0, rect, format, params);
    }
    return g_origDrawTextExW(hdc, text, cch, rect, format, params);
}

HRESULT WINAPI DrawThemeTextEx_Hook(HTHEME theme, HDC hdc, int part, int state, LPCWSTR text, int cch,
                                    DWORD flags, LPRECT rect, const DTTOPTS* options)
{
    if (t_paintingDesktop && ShouldHideLabel(text, cch))
    {
        wchar_t empty[2] = { 0, 0 };
        return g_origDrawThemeTextEx(theme, hdc, part, state, empty, 0, flags, rect, options);
    }
    return g_origDrawThemeTextEx(theme, hdc, part, state, text, cch, flags, rect, options);
}

// ---------------------------------------------------------------------------------------------------------------
// The desktop subclass
// ---------------------------------------------------------------------------------------------------------------

LRESULT CALLBACK DesktopListSubclass(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                     UINT_PTR idSubclass, DWORD_PTR refData)
{
    UNREFERENCED_PARAMETER(idSubclass);
    UNREFERENCED_PARAMETER(refData);

    switch (uMsg)
    {
    case WM_PAINT:
    case WM_PRINTCLIENT:
        if (g_active.load(std::memory_order_relaxed) && g_hideText.load(std::memory_order_relaxed))
        {
            RefreshFolderNames();

            // Saved and restored rather than set and cleared, in case the paint is nested.
            const bool was = t_paintingDesktop;
            t_paintingDesktop = true;
            LRESULT result = DefSubclassProc(hWnd, uMsg, wParam, lParam);
            t_paintingDesktop = was;
            return result;
        }
        break;

    case LVM_INSERTITEMA:
    case LVM_INSERTITEMW:
    case LVM_DELETEITEM:
    case LVM_DELETEALLITEMS:
    case LVM_SETITEMA:
    case LVM_SETITEMW:
    case LVM_SETITEMTEXTA:
    case LVM_SETITEMTEXTW:
        // The desktop changed; the next paint re-reads the folder names.
        g_namesTick.store(0, std::memory_order_relaxed);
        break;

    case WM_NCDESTROY:
        RemoveWindowSubclass(hWnd, DesktopListSubclass, kSubclassId);
        break;
    }

    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

// `crossThread` is TRUE from the engine thread; from inside the CreateWindowExW hook the caller already is the
// window's thread and SetWindowSubclass can be called directly.
void Watch(HWND hList, bool crossThread)
{
    if (!hList || !IsWindow(hList))
    {
        return;
    }

    DWORD_PTR existing = 0;
    if (GetWindowSubclass(hList, DesktopListSubclass, kSubclassId, &existing))
    {
        return;
    }

    const BOOL ok = crossThread
        ? SP_SetWindowSubclassFromAnyThread(hList, DesktopListSubclass, kSubclassId, 0)
        : SetWindowSubclass(hList, DesktopListSubclass, kSubclassId, 0);

    if (!ok)
    {
        SP_LogError(L"The desktop list view %p could not be subclassed", hList);
        return;
    }
    SP_Log(L"Watching the desktop list view %p", hList);
}

void Unwatch(HWND hList)
{
    if (!hList || !IsWindow(hList))
    {
        return;
    }

    DWORD_PTR existing = 0;
    if (GetWindowSubclass(hList, DesktopListSubclass, kSubclassId, &existing))
    {
        SP_RemoveWindowSubclassFromAnyThread(hList, DesktopListSubclass, kSubclassId);
    }
}

using CreateWindowExW_t = decltype(&CreateWindowExW);
CreateWindowExW_t g_origCreateWindowExW = nullptr;

HWND WINAPI CreateWindowExW_Hook(DWORD exStyle, LPCWSTR className, LPCWSTR windowName, DWORD style,
                                 int x, int y, int width, int height, HWND hParent, HMENU hMenu,
                                 HINSTANCE hInstance, LPVOID param)
{
    HWND hWnd = g_origCreateWindowExW(exStyle, className, windowName, style, x, y, width, height,
                                      hParent, hMenu, hInstance, param);

    // Cheapest rejection first: the desktop list view always has a parent.
    if (hWnd && hParent && g_active.load(std::memory_order_relaxed) && IsDesktopListView(hWnd))
    {
        Watch(hWnd, false);
    }
    return hWnd;
}

// ---------------------------------------------------------------------------------------------------------------
// The shortcut arrow overlay
// ---------------------------------------------------------------------------------------------------------------

// IID_IImageList, spelled out so no uuid library is needed.
const GUID kIID_IImageList =
    { 0x46eb5926, 0x582e, 0x4017, { 0x9f, 0xdf, 0xe8, 0x99, 0x8d, 0xaa, 0x09, 0x50 } };

constexpr int kImageLists[] = { SHIL_LARGE, SHIL_SMALL, SHIL_EXTRALARGE, SHIL_SYSSMALL, SHIL_JUMBO };

// The image this mod added to each list, or -1 while it has not. Engine thread only.
int  g_overlayImage[ARRAYSIZE(kImageLists)] = { -1, -1, -1, -1, -1 };
bool g_arrowsHidden = false;

// A 32-bit icon whose every pixel has zero alpha and a mask that is all "transparent".
HICON CreateBlankIcon(int cx, int cy)
{
    if (cx <= 0 || cy <= 0 || cx > 1024 || cy > 1024)
    {
        return nullptr;
    }

    BITMAPV5HEADER header = {};
    header.bV5Size = sizeof(header);
    header.bV5Width = cx;
    header.bV5Height = cy;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC hdc = GetDC(nullptr);
    HBITMAP hColor = CreateDIBSection(hdc, (const BITMAPINFO*)&header, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (hdc)
    {
        ReleaseDC(nullptr, hdc);
    }
    if (!hColor)
    {
        return nullptr;
    }
    if (bits)
    {
        memset(bits, 0, (size_t)cx * (size_t)cy * 4);
    }

    // Monochrome mask rows are padded to 16 bits; all ones means "do not draw".
    const size_t rowBytes = (((size_t)cx + 15) / 16) * 2;
    std::vector<BYTE> mask(rowBytes * (size_t)cy, 0xFF);
    HBITMAP hMask = CreateBitmap(cx, cy, 1, 1, mask.data());

    HICON hIcon = nullptr;
    if (hMask)
    {
        ICONINFO info = {};
        info.fIcon = TRUE;
        info.hbmColor = hColor;
        info.hbmMask = hMask;
        hIcon = CreateIconIndirect(&info);
        DeleteObject(hMask);
    }
    DeleteObject(hColor);
    return hIcon;
}

// The stock link overlay at the size a list wants, honouring a user's Shell Icons override the same way the
// shell does. Used to put the arrow back.
HICON LoadLinkOverlayIcon(int cx)
{
    SHSTOCKICONINFO info = {};
    info.cbSize = sizeof(info);
    if (SUCCEEDED(SHGetStockIconInfo(SIID_LINK, SHGSI_ICONLOCATION, &info)) && info.szPath[0])
    {
        HICON hIcon = nullptr;
        HRESULT hr = SHDefExtractIconW(info.szPath, info.iIcon, 0, &hIcon, nullptr, MAKELONG(cx, cx));
        if (hr == S_OK && hIcon)
        {
            return hIcon;
        }
    }

    // The list scales whatever it gets, so a stock size is an acceptable fallback.
    info = {};
    info.cbSize = sizeof(info);
    const UINT size = cx > 16 ? SHGSI_LARGEICON : SHGSI_SMALLICON;
    if (SUCCEEDED(SHGetStockIconInfo(SIID_LINK, SHGSI_ICON | size, &info)))
    {
        return info.hIcon;
    }
    return nullptr;
}

// Points the link overlay of every system image list at a blank image (hide) or at a copy of the stock arrow
// (restore). The image is added once per list and replaced in place after that, so toggling never grows the
// lists.
void SetLinkOverlay(bool blank)
{
    const int linkOverlay = SHGetIconOverlayIndexW(nullptr, IDO_SHGIOI_LINK);
    if (linkOverlay <= 0)
    {
        SP_LogError(L"The link overlay slot is unknown (%d)", linkOverlay);
        return;
    }

    int done = 0;
    for (size_t i = 0; i < ARRAYSIZE(kImageLists); ++i)
    {
        // Nothing to put back in a list this mod never touched.
        if (!blank && g_overlayImage[i] < 0)
        {
            continue;
        }

        IImageList* pList = nullptr;
        if (FAILED(SHGetImageList(kImageLists[i], kIID_IImageList, (void**)&pList)) || !pList)
        {
            continue;
        }

        int cx = 0, cy = 0;
        if (SUCCEEDED(pList->GetIconSize(&cx, &cy)) && cx > 0 && cy > 0)
        {
            HICON hIcon = blank ? CreateBlankIcon(cx, cy) : LoadLinkOverlayIcon(cx);
            if (hIcon)
            {
                int index = -1;
                if (SUCCEEDED(pList->ReplaceIcon(g_overlayImage[i], hIcon, &index)) && index >= 0)
                {
                    g_overlayImage[i] = index;
                    if (SUCCEEDED(pList->SetOverlayImage(index, linkOverlay)))
                    {
                        done++;
                    }
                }
                DestroyIcon(hIcon);
            }
        }
        pList->Release();
    }

    g_arrowsHidden = blank && done > 0;
    SP_Log(L"Link overlay %s in %d image list(s)", blank ? L"hidden" : L"restored", done);

    // Makes every open view fetch its icons again; the desktop is invalidated on top so the change shows at
    // once even if the notification is slow.
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    RepaintDesktop(nullptr);
}

// ---------------------------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------------------------

void LoadSettings()
{
    g_hideText.store(SP_GetIntSetting(L"HideText", 1) != 0, std::memory_order_relaxed);
    g_hideArrows.store(SP_GetIntSetting(L"HideArrows", 1) != 0, std::memory_order_relaxed);
    SP_Log(L"HideText=%d HideArrows=%d",
           g_hideText.load(std::memory_order_relaxed) ? 1 : 0,
           g_hideArrows.load(std::memory_order_relaxed) ? 1 : 0);
}

BOOL Init()
{
    LoadSettings();

    if (!SP_HookBegin())
    {
        return FALSE;
    }

    if (!SP_SetExportHook(L"user32.dll", "CreateWindowExW", CreateWindowExW_Hook, &g_origCreateWindowExW) ||
        !SP_SetExportHook(L"user32.dll", "DrawTextW", DrawTextW_Hook, &g_origDrawTextW) ||
        !SP_SetExportHook(L"user32.dll", "DrawTextExW", DrawTextExW_Hook, &g_origDrawTextExW) ||
        !SP_SetExportHook(L"uxtheme.dll", "DrawThemeTextEx", DrawThemeTextEx_Hook, &g_origDrawThemeTextEx))
    {
        SP_HookAbort();
        SP_LogError(L"The text drawing functions could not be hooked");
        return FALSE;
    }

    if (!SP_HookCommit())
    {
        return FALSE;
    }

    g_active.store(true, std::memory_order_relaxed);
    return TRUE;
}

void AfterInit()
{
    HWND hList = FindDesktopListView();
    if (hList)
    {
        Watch(hList, true);
        RepaintDesktop(hList);
    }
    else
    {
        // Not an error: on a cold sign-in the desktop may not exist yet, and the CreateWindowExW hook picks it
        // up when it does.
        SP_Log(L"No desktop list view yet; waiting for the shell to create one");
    }

    if (g_hideArrows.load(std::memory_order_relaxed))
    {
        SetLinkOverlay(true);
    }
}

void SettingsChanged()
{
    const bool hadText = g_hideText.load(std::memory_order_relaxed);
    const bool hadArrows = g_hideArrows.load(std::memory_order_relaxed);

    LoadSettings();

    if (g_hideText.load(std::memory_order_relaxed) != hadText)
    {
        RepaintDesktop(nullptr);
    }

    const bool wantArrowsHidden = g_hideArrows.load(std::memory_order_relaxed);
    if (wantArrowsHidden != hadArrows || wantArrowsHidden != g_arrowsHidden)
    {
        SetLinkOverlay(wantArrowsHidden);
    }
}

void BeforeUninit()
{
    // Still hooked, but from here on the subclass paints normally and no new desktop is watched.
    g_active.store(false, std::memory_order_relaxed);
    g_hideText.store(false, std::memory_order_relaxed);

    HWND hList = FindDesktopListView();
    if (hList)
    {
        Unwatch(hList);
        RepaintDesktop(hList);
    }

    if (g_arrowsHidden)
    {
        SetLinkOverlay(false);
    }
}

}   // namespace

SP_MOD_DEFINE(g_modHideDesktopIconText) =
{
    /* id             */ SP_MOD_ID,
    /* name           */ L"Hide desktop icon text and shortcut arrows",
    /* basedOn        */ "hide-desktop-icon-text",
    /* originalAuthor */ "kivsak",
    /* targets        */ SP_TARGET_EXPLORER,
    /* minOsBuild     */ 0,
    /* flags          */ 0,
    /* Init           */ Init,
    /* AfterInit      */ AfterInit,
    /* SettingsChanged*/ SettingsChanged,
    /* BeforeUninit   */ BeforeUninit,
    /* Uninit         */ nullptr,
};
