//
// transparent-desktop-icons-spotlight - dim the desktop icons while nobody is using them.
//
// Adapted from the idea behind the Windhawk mod "Transparent Desktop Icons with Spotlight"
// (transparent-desktop-icons-spotlight) by drgutman. The implementation here is written against this engine's API.
//
// What this does
// --------------
// The desktop icons are drawn at a reduced opacity while the desktop is idle. They fade back to full opacity as
// soon as the mouse is over the desktop or an icon is selected (or renamed), and fade out again when the mouse
// leaves and the selection has been left alone for a moment. With a spotlight radius set, only a soft circle
// around the cursor and the selected icons are revealed; the rest of the desktop stays dim.
//
// How the desktop is drawn on Windows 11, and why this is small
// -------------------------------------------------------------
// Progman has WS_EX_NOREDIRECTIONBITMAP: the wallpaper is composed by DWM, not painted with GDI. The icon view
// (SHELLDLL_DefView) is a WS_EX_LAYERED child of Progman on which the shell itself calls
// SetLayeredWindowAttributes(0, 255, LWA_ALPHA). For such a window DWM multiplies that constant alpha into the
// per-pixel alpha of the view's own surface, and the surface is fully transparent between the icons. Lowering
// the constant alpha therefore dims icons, labels and the selection highlight and nothing else; the wallpaper is
// never touched. The original mod re-drew the wallpaper over the icons from a Direct2D/DirectComposition overlay,
// which needs the wallpaper file, its fit mode and a list of shell quirks. None of that is needed here: the alpha
// the shell already set is the whole lever, and restoring it is one call.
//
// The spotlight
// -------------
// With a radius set, the view stays at the idle alpha and a second layered child of Progman, placed just above
// the view, shows the revealed parts. Its content is a print of the icon list (PrintWindow into a 32-bit DIB;
// the shell draws the icons and composited labels with a real alpha channel, so the print carries it too)
// multiplied by a soft mask: a feathered circle around the cursor and the padded rectangles of the selected
// items. UpdateLayeredWindowIndirect with a dirty rectangle keeps every frame down to the area that changed.
// If that window cannot be created or updated, the mod falls back to dimming the whole view.
//
// Threads
// -------
// Everything that touches the view runs on the desktop's own thread, inside the subclass procedures: the fade
// is driven by a timer on the view, and the engine thread only installs the subclasses
// (SP_SetWindowSubclassFromAnyThread) and sends short requests (apply the settings, tear down) to the view. A
// small watcher thread re-attaches when the shell rebuilds the desktop (theme change, "Show desktop icons").
//
// The view is shared with the engine's own desktop surface, which subclasses the same windows with its own id;
// this mod uses a different id and never subclasses from the engine thread directly.
//
#define SP_MOD_ID "transparent-desktop-icons-spotlight"
#include "engine/modapi.h"

#include <commctrl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <new>
#include <vector>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Gdi32.lib")

namespace {

// ---------------------------------------------------------------------------------------------------------------
// Settings and shared state
// ---------------------------------------------------------------------------------------------------------------

// Written on the engine thread, read on the desktop's thread.
std::atomic<int>  g_idleOpacity{ 35 };      // percent of full opacity while idle (0 = hidden, 100 = no dimming)
std::atomic<int>  g_fadeMs{ 300 };          // length of the fade in either direction; 0 = instant
std::atomic<int>  g_spotlightRadius{ 0 };   // radius in pixels at 96 dpi; 0 = the whole desktop is revealed
std::atomic<bool> g_active{ false };        // FALSE once BeforeUninit ran

// The windows this mod is attached to. Written by the watcher thread and, when they die, by the desktop thread.
std::atomic<HWND> g_hView{ nullptr };       // SHELLDLL_DefView
std::atomic<HWND> g_hList{ nullptr };       // its SysListView32

HANDLE g_watchThread = nullptr;
HANDLE g_watchStop = nullptr;

// A registered message carrying requests from the engine thread to the view. wParam must hold the magic so a
// stray message with the same id can never be mistaken for one of ours.
UINT g_msg = 0;
constexpr WPARAM kMagic = 0x53504953;   // 'SPIS'
enum Op
{
    OP_APPLY = 1,       // settings changed or first attach: apply the idle alpha and start animating
    OP_TEARDOWN = 2,    // restore the view exactly as the shell had it
    OP_HEARTBEAT = 3,   // once a second: make sure the shell did not reset the alpha
};

// A subclass id unique to this mod. The engine's desktop surface uses 0x53506431 ('SPd1') on the same windows.
constexpr UINT_PTR kSubclassId = 0x53506453;   // 'SPdS'
constexpr UINT_PTR kTimerId = 0x53506453;
constexpr UINT     kTimerMs = 16;
constexpr DWORD    kHeartbeatMs = 1000;

// After the selection stops changing, this is how long the selected icons stay revealed with the mouse away.
// The original had this as a setting (2000 ms); a fixed value keeps the settings page to what matters.
constexpr DWORD kSelectionHoldMs = 2500;

// Spotlight overlay: padding and feathering around a selected item, and the cap on separate item rectangles
// (a larger selection is revealed as one bounding rectangle).
constexpr int kSelectionPadDip = 8;
constexpr int kMaxSelectionRegions = 48;

const wchar_t* const kOverlayClass = L"ShadePatcher.DesktopIconsSpotlight.Overlay";

// Everything below is only touched on the desktop's thread.
struct State
{
    bool attached = false;

    // The layered attributes the shell had set on the view, restored on teardown.
    COLORREF origKey = 0;
    BYTE     origAlpha = 255;
    DWORD    origFlags = LWA_ALPHA;

    bool  mouseOver = false;
    bool  cursorValid = false;
    POINT cursor = { 0, 0 };            // in view client coordinates
    bool  editing = false;              // a label is being renamed
    DWORD lastSelectionChange = 0;

    float revealWhole = 0.0f;           // 0 = idle alpha, 1 = full (whole-desktop mode)
    float fadeSpot = 0.0f;              // spotlight mode: the circle
    float fadeSel = 0.0f;               // spotlight mode: the selected items
    DWORD lastTick = 0;
    bool  timerOn = false;
    bool  inTick = false;
    int   appliedAlpha = -1;

    // The spotlight overlay and its surface.
    HWND    hOverlay = nullptr;
    bool    overlayShown = false;
    bool    overlayFailed = false;      // creation or update failed: whole-desktop mode until settings change
    HDC     hdc = nullptr;
    HBITMAP hBitmap = nullptr;
    HGDIOBJ hOldBitmap = nullptr;
    BYTE*   bits = nullptr;
    BYTE*   mask = nullptr;
    int     width = 0;
    int     height = 0;
    bool    fullUpdate = true;          // the next update must push the whole surface
    RECT    drawn = { 0, 0, 0, 0 };     // the part of the overlay that currently holds pixels
};
State g_s;

// ---------------------------------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------------------------------

bool ClassIs(HWND hWnd, const wchar_t* name)
{
    wchar_t wszClass[64];
    return hWnd && GetClassNameW(hWnd, wszClass, ARRAYSIZE(wszClass)) && _wcsicmp(wszClass, name) == 0;
}

HMODULE ThisModule()
{
    HMODULE hModule = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&ThisModule, &hModule);
    return hModule;
}

int Clamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

float Clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

// The desktop icon view: SHELLDLL_DefView under Progman, or under a WorkerW (child of Progman on newer builds,
// top-level on older ones). Only a window of this process can be subclassed.
HWND FindDesktopView()
{
    HWND hProgman = GetShellWindow();
    if (!hProgman)
    {
        hProgman = FindWindowW(L"Progman", nullptr);
    }

    HWND hView = hProgman ? FindWindowExW(hProgman, nullptr, L"SHELLDLL_DefView", nullptr) : nullptr;

    if (!hView && hProgman)
    {
        HWND hWorker = nullptr;
        while (!hView && (hWorker = FindWindowExW(hProgman, hWorker, L"WorkerW", nullptr)) != nullptr)
        {
            hView = FindWindowExW(hWorker, nullptr, L"SHELLDLL_DefView", nullptr);
        }
    }
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

    DWORD pid = 0;
    GetWindowThreadProcessId(hView, &pid);
    return pid == GetCurrentProcessId() ? hView : nullptr;
}

HWND ListOf(HWND hView)
{
    return hView ? FindWindowExW(hView, nullptr, L"SysListView32", nullptr) : nullptr;
}

int IdleAlpha()
{
    return MulDiv(Clamp(g_idleOpacity.load(std::memory_order_relaxed), 0, 100), 255, 100);
}

float DpiScale(HWND hWnd)
{
    UINT dpi = GetDpiForWindow(hWnd);
    return dpi ? (float)dpi / 96.0f : 1.0f;
}

float SpotlightRadiusPx(HWND hView)
{
    int radius = g_spotlightRadius.load(std::memory_order_relaxed);
    if (radius <= 0)
    {
        return 0.0f;
    }
    return (float)Clamp(radius, 20, 4000) * DpiScale(hView);
}

// ---------------------------------------------------------------------------------------------------------------
// The view's alpha
// ---------------------------------------------------------------------------------------------------------------

void ApplyViewAlpha(HWND hView, int alpha)
{
    alpha = Clamp(alpha, 0, 255);
    if (g_s.appliedAlpha == alpha)
    {
        return;
    }
    if (SetLayeredWindowAttributes(hView, g_s.origKey, (BYTE)alpha, g_s.origFlags | LWA_ALPHA))
    {
        g_s.appliedAlpha = alpha;
    }
}

void EnsureTimer(HWND hView)
{
    if (g_s.timerOn)
    {
        return;
    }
    if (SetTimer(hView, kTimerId, kTimerMs, nullptr))
    {
        g_s.timerOn = true;
        g_s.lastTick = GetTickCount();
    }
}

void StopTimer(HWND hView)
{
    if (g_s.timerOn)
    {
        KillTimer(hView, kTimerId);
        g_s.timerOn = false;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// The spotlight overlay
// ---------------------------------------------------------------------------------------------------------------

LRESULT CALLBACK OverlayWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_NCHITTEST:
        // Never in the way of the mouse: clicks and hovers go to the icons underneath.
        return HTTRANSPARENT;

    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

bool RegisterOverlayClass()
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = ThisModule();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kOverlayClass;
    if (RegisterClassExW(&wc))
    {
        return true;
    }
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

void FreeSurface()
{
    if (g_s.hdc)
    {
        if (g_s.hOldBitmap)
        {
            SelectObject(g_s.hdc, g_s.hOldBitmap);
        }
        DeleteDC(g_s.hdc);
    }
    if (g_s.hBitmap)
    {
        DeleteObject(g_s.hBitmap);
    }
    delete[] g_s.mask;
    g_s.hdc = nullptr;
    g_s.hBitmap = nullptr;
    g_s.hOldBitmap = nullptr;
    g_s.bits = nullptr;
    g_s.mask = nullptr;
    g_s.width = 0;
    g_s.height = 0;
    g_s.fullUpdate = true;
    SetRectEmpty(&g_s.drawn);
}

// A top-down 32-bit DIB the size of the view, plus one byte of mask per pixel.
bool EnsureSurface(int width, int height)
{
    if (g_s.bits && g_s.width == width && g_s.height == height)
    {
        return true;
    }
    FreeSurface();

    if (width <= 0 || height <= 0 || width > 16384 || height > 16384)
    {
        return false;
    }

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC hScreen = GetDC(nullptr);
    g_s.hdc = CreateCompatibleDC(hScreen);
    if (hScreen)
    {
        ReleaseDC(nullptr, hScreen);
    }
    if (!g_s.hdc)
    {
        return false;
    }

    void* bits = nullptr;
    g_s.hBitmap = CreateDIBSection(g_s.hdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!g_s.hBitmap || !bits)
    {
        FreeSurface();
        return false;
    }
    g_s.bits = (BYTE*)bits;
    g_s.hOldBitmap = SelectObject(g_s.hdc, g_s.hBitmap);

    const size_t pixels = (size_t)width * (size_t)height;
    g_s.mask = new (std::nothrow) BYTE[pixels];
    if (!g_s.mask)
    {
        FreeSurface();
        return false;
    }
    memset(g_s.bits, 0, pixels * 4);
    memset(g_s.mask, 0, pixels);
    g_s.width = width;
    g_s.height = height;
    g_s.fullUpdate = true;
    SetRectEmpty(&g_s.drawn);
    return true;
}

// The view's rectangle in its parent's client coordinates, which is where the overlay lives.
bool ViewRectInParent(HWND hView, HWND hParent, RECT* rc)
{
    if (!GetWindowRect(hView, rc))
    {
        return false;
    }
    MapWindowPoints(nullptr, hParent, (POINT*)rc, 2);
    return rc->right > rc->left && rc->bottom > rc->top;
}

void DestroyOverlay()
{
    if (g_s.hOverlay)
    {
        DestroyWindow(g_s.hOverlay);
        g_s.hOverlay = nullptr;
    }
    g_s.overlayShown = false;
    FreeSurface();
}

// Keeps the overlay over the view and on top of its siblings, and resizes the surface with it.
void PlaceOverlay(HWND hView)
{
    if (!g_s.hOverlay)
    {
        return;
    }
    HWND hParent = GetAncestor(hView, GA_PARENT);
    RECT rc;
    if (!hParent || !ViewRectInParent(hView, hParent, &rc))
    {
        return;
    }

    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;

    RECT now;
    GetWindowRect(g_s.hOverlay, &now);
    MapWindowPoints(nullptr, hParent, (POINT*)&now, 2);

    const bool moved = !EqualRect(&now, &rc);
    const bool notOnTop = GetWindow(g_s.hOverlay, GW_HWNDPREV) != nullptr;
    if (moved || notOnTop)
    {
        SetWindowPos(g_s.hOverlay, HWND_TOP, rc.left, rc.top, width, height,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER | (g_s.overlayShown ? SWP_SHOWWINDOW : 0));
    }
    if (width != g_s.width || height != g_s.height)
    {
        if (!EnsureSurface(width, height))
        {
            SP_LogError(L"The spotlight surface (%d x %d) could not be created", width, height);
            g_s.overlayFailed = true;
            DestroyOverlay();
        }
    }
}

bool CreateOverlay(HWND hView)
{
    if (g_s.hOverlay)
    {
        return true;
    }
    if (g_s.overlayFailed)
    {
        return false;
    }

    HWND hParent = GetAncestor(hView, GA_PARENT);
    if (!hParent)
    {
        return false;
    }
    if (GetWindowThreadProcessId(hParent, nullptr) != GetCurrentThreadId())
    {
        SP_LogError(L"The desktop's parent belongs to another thread; the spotlight is not available");
        g_s.overlayFailed = true;
        return false;
    }

    RECT rc;
    if (!ViewRectInParent(hView, hParent, &rc))
    {
        return false;   // not sized yet; tried again on the next tick
    }
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;

    // Hidden until it has something to show. WS_EX_TRANSPARENT and HTTRANSPARENT together keep it out of the
    // mouse's way; WS_EX_NOACTIVATE keeps it out of the focus order.
    HWND hOverlay = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_NOPARENTNOTIFY,
        kOverlayClass, L"", WS_CHILD | WS_CLIPSIBLINGS,
        rc.left, rc.top, width, height, hParent, nullptr, ThisModule(), nullptr);
    if (!hOverlay)
    {
        SP_LogError(L"The spotlight overlay could not be created: %lu", GetLastError());
        g_s.overlayFailed = true;
        return false;
    }

    g_s.hOverlay = hOverlay;
    g_s.overlayShown = false;
    if (!EnsureSurface(width, height))
    {
        SP_LogError(L"The spotlight surface (%d x %d) could not be created", width, height);
        g_s.overlayFailed = true;
        DestroyOverlay();
        return false;
    }

    SetWindowPos(hOverlay, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    SP_Log(L"Spotlight overlay %p created over the desktop (%d x %d)", hOverlay, width, height);
    return true;
}

// One revealed area: a feathered circle, or a rectangle with feathered edges.
struct Region
{
    RECT  box;          // bounding box, view coordinates
    bool  circle;
    float cx, cy, radius;
    float feather;      // width of the soft edge in pixels
    float strength;     // 0..1, the fade
};

void CollectRegions(HWND hView, HWND hList, float radiusPx, std::vector<Region>& out)
{
    const float scale = DpiScale(hView);

    if (g_s.fadeSpot > 0.001f && g_s.cursorValid && radiusPx > 0.0f)
    {
        Region r = {};
        r.circle = true;
        r.cx = (float)g_s.cursor.x + 0.5f;
        r.cy = (float)g_s.cursor.y + 0.5f;
        r.radius = radiusPx;
        r.feather = (std::max)(8.0f * scale, radiusPx * 0.3f);
        r.strength = g_s.fadeSpot;
        r.box.left = (int)floorf(r.cx - r.radius) - 1;
        r.box.top = (int)floorf(r.cy - r.radius) - 1;
        r.box.right = (int)ceilf(r.cx + r.radius) + 1;
        r.box.bottom = (int)ceilf(r.cy + r.radius) + 1;
        out.push_back(r);
    }

    if (g_s.fadeSel > 0.001f && hList)
    {
        const int pad = (int)(kSelectionPadDip * scale + 0.5f);
        const float feather = (float)kSelectionPadDip * scale;

        POINT offset = { 0, 0 };
        MapWindowPoints(hList, hView, &offset, 1);

        RECT extra = { 0, 0, 0, 0 };
        bool haveExtra = false;
        int  count = 0;
        int  guard = 0;
        int  index = -1;
        while ((index = (int)SendMessageW(hList, LVM_GETNEXTITEM, (WPARAM)index, LVNI_SELECTED)) != -1)
        {
            if (++guard > 8192)
            {
                break;
            }
            RECT rc = {};
            rc.left = LVIR_BOUNDS;
            if (!SendMessageW(hList, LVM_GETITEMRECT, (WPARAM)index, (LPARAM)&rc))
            {
                continue;
            }
            OffsetRect(&rc, offset.x, offset.y);
            InflateRect(&rc, pad, pad);

            if (count < kMaxSelectionRegions)
            {
                Region r = {};
                r.box = rc;
                r.feather = feather;
                r.strength = g_s.fadeSel;
                out.push_back(r);
                ++count;
            }
            else if (!haveExtra)
            {
                extra = rc;
                haveExtra = true;
            }
            else
            {
                UnionRect(&extra, &extra, &rc);
            }
        }
        if (haveExtra)
        {
            Region r = {};
            r.box = extra;
            r.feather = feather;
            r.strength = g_s.fadeSel;
            out.push_back(r);
        }
    }
}

// Writes a region's coverage into the mask (keeping the larger value where regions overlap).
void PaintMask(const Region& region, const RECT& clip)
{
    RECT box;
    if (!IntersectRect(&box, &region.box, &clip))
    {
        return;
    }
    const float feather = region.feather > 0.5f ? region.feather : 0.5f;

    for (int y = box.top; y < box.bottom; ++y)
    {
        BYTE* row = g_s.mask + (size_t)y * (size_t)g_s.width;
        for (int x = box.left; x < box.right; ++x)
        {
            float coverage;
            if (region.circle)
            {
                const float dx = (float)x + 0.5f - region.cx;
                const float dy = (float)y + 0.5f - region.cy;
                coverage = (region.radius - sqrtf(dx * dx + dy * dy)) / feather;
            }
            else
            {
                const int dxi = (std::min)(x - region.box.left, region.box.right - 1 - x);
                const int dyi = (std::min)(y - region.box.top, region.box.bottom - 1 - y);
                coverage = ((float)(std::min)(dxi, dyi) + 1.0f) / feather;
            }
            coverage = Clamp01(coverage) * region.strength;
            const BYTE m = (BYTE)(coverage * 255.0f + 0.5f);
            if (row[x] < m)
            {
                row[x] = m;
            }
        }
    }
}

// Multiplies the printed pixels by the mask (premultiplied BGRA, so every channel is scaled) and clears the
// mask behind it. A pixel outside every region ends up fully transparent.
void ApplyMask(const RECT& rc)
{
    for (int y = rc.top; y < rc.bottom; ++y)
    {
        BYTE* mask = g_s.mask + (size_t)y * (size_t)g_s.width;
        BYTE* px = g_s.bits + ((size_t)y * (size_t)g_s.width + (size_t)rc.left) * 4;
        for (int x = rc.left; x < rc.right; ++x, px += 4)
        {
            const BYTE m = mask[x];
            mask[x] = 0;
            if (m == 0)
            {
                *(DWORD*)px = 0;
            }
            else if (m != 255)
            {
                px[0] = (BYTE)((px[0] * m + 127) / 255);
                px[1] = (BYTE)((px[1] * m + 127) / 255);
                px[2] = (BYTE)((px[2] * m + 127) / 255);
                px[3] = (BYTE)((px[3] * m + 127) / 255);
            }
        }
    }
}

void ZeroRect(const RECT& rc)
{
    for (int y = rc.top; y < rc.bottom; ++y)
    {
        memset(g_s.bits + ((size_t)y * (size_t)g_s.width + (size_t)rc.left) * 4, 0,
               (size_t)(rc.right - rc.left) * 4);
    }
}

void ShowOverlay(bool show)
{
    if (!g_s.hOverlay || g_s.overlayShown == show)
    {
        return;
    }
    if (show)
    {
        SetWindowPos(g_s.hOverlay, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    }
    else
    {
        ShowWindow(g_s.hOverlay, SW_HIDE);
    }
    g_s.overlayShown = show;
}

// One frame of the spotlight: print the icons where something is revealed, mask them, push the changed area.
void RenderOverlay(HWND hView, HWND hList, float radiusPx)
{
    if (!g_s.hOverlay || !g_s.bits)
    {
        return;
    }

    std::vector<Region> regions;
    if (hList && IsWindow(hList) && IsWindowVisible(hList))
    {
        CollectRegions(hView, hList, radiusPx, regions);
    }

    const RECT surface = { 0, 0, g_s.width, g_s.height };

    // The area that will hold pixels after this frame, and the area that has to be pushed: that plus whatever
    // was drawn last time, so old pixels are cleared.
    RECT area;
    SetRectEmpty(&area);
    for (const Region& r : regions)
    {
        RECT clipped;
        if (IntersectRect(&clipped, &r.box, &surface))
        {
            if (IsRectEmpty(&area))
            {
                area = clipped;
            }
            else
            {
                UnionRect(&area, &area, &clipped);
            }
        }
    }

    RECT dirty;
    if (g_s.fullUpdate)
    {
        dirty = surface;
    }
    else if (IsRectEmpty(&g_s.drawn))
    {
        dirty = area;
    }
    else if (IsRectEmpty(&area))
    {
        dirty = g_s.drawn;
    }
    else
    {
        UnionRect(&dirty, &g_s.drawn, &area);
    }
    IntersectRect(&dirty, &dirty, &surface);

    if (IsRectEmpty(&dirty))
    {
        ShowOverlay(false);
        return;
    }

    ZeroRect(dirty);

    if (!IsRectEmpty(&area))
    {
        // The list's client origin in view coordinates (0,0 in practice, but not assumed).
        POINT offset = { 0, 0 };
        MapWindowPoints(hList, hView, &offset, 1);

        HRGN hClip = CreateRectRgnIndirect(&dirty);
        if (hClip)
        {
            SelectClipRgn(g_s.hdc, hClip);
            DeleteObject(hClip);
        }
        SetViewportOrgEx(g_s.hdc, offset.x, offset.y, nullptr);
        PrintWindow(hList, g_s.hdc, PW_CLIENTONLY);
        SetViewportOrgEx(g_s.hdc, 0, 0, nullptr);
        SelectClipRgn(g_s.hdc, nullptr);

        for (const Region& r : regions)
        {
            PaintMask(r, dirty);
        }
    }
    ApplyMask(dirty);

    POINT ptSrc = { 0, 0 };
    SIZE size = { g_s.width, g_s.height };
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    UPDATELAYEREDWINDOWINFO info = {};
    info.cbSize = sizeof(info);
    info.hdcSrc = g_s.hdc;
    info.pptSrc = &ptSrc;
    info.psize = &size;
    info.pblend = &blend;
    info.dwFlags = ULW_ALPHA;
    info.prcDirty = g_s.fullUpdate ? nullptr : &dirty;

    if (!UpdateLayeredWindowIndirect(g_s.hOverlay, &info))
    {
        SP_LogError(L"The spotlight overlay could not be updated: %lu; dimming the whole desktop instead",
                    GetLastError());
        g_s.overlayFailed = true;
        DestroyOverlay();
        return;
    }
    g_s.fullUpdate = false;
    g_s.drawn = area;

    ShowOverlay(!IsRectEmpty(&area));
}

// ---------------------------------------------------------------------------------------------------------------
// The animation
// ---------------------------------------------------------------------------------------------------------------

float Approach(float value, float target, float step)
{
    if (value < target)
    {
        value += step;
        return value > target ? target : value;
    }
    if (value > target)
    {
        value -= step;
        return value < target ? target : value;
    }
    return value;
}

bool CursorOverDesktop(HWND hView, HWND hList)
{
    POINT pt;
    if (!GetCursorPos(&pt))
    {
        return false;
    }
    HWND hUnder = WindowFromPoint(pt);
    if (!hUnder)
    {
        return false;
    }
    if (hUnder == hView || (hList && hUnder == hList) || (g_s.hOverlay && hUnder == g_s.hOverlay))
    {
        return true;
    }
    // The rename box is a child of the list.
    HWND hParent = GetAncestor(hUnder, GA_PARENT);
    return (hList && hParent == hList) || hParent == hView;
}

void Tick(HWND hView)
{
    if (g_s.inTick)
    {
        return;
    }
    g_s.inTick = true;

    const DWORD now = GetTickCount();
    float dt = (float)(now - g_s.lastTick);
    if (dt > 100.0f)
    {
        dt = 100.0f;
    }
    g_s.lastTick = now;

    HWND hList = g_hList.load(std::memory_order_relaxed);
    if (!hList || !IsWindow(hList))
    {
        hList = ListOf(hView);
    }

    if (g_s.mouseOver)
    {
        POINT pt;
        if (GetCursorPos(&pt) && ScreenToClient(hView, &pt))
        {
            g_s.cursor = pt;
            g_s.cursorValid = true;
        }
    }

    const int selected = hList ? (int)SendMessageW(hList, LVM_GETSELECTEDCOUNT, 0, 0) : 0;
    const bool holding = selected > 0 && !g_s.editing && (now - g_s.lastSelectionChange) <= kSelectionHoldMs;
    const bool selTarget = g_s.editing || holding;

    const int fadeMs = g_fadeMs.load(std::memory_order_relaxed);
    const float step = fadeMs <= 0 ? 1.0f : dt / (float)fadeMs;

    const float radiusPx = SpotlightRadiusPx(hView);
    const bool spotlight = radiusPx > 0.0f && !g_s.overlayFailed && (g_s.hOverlay || CreateOverlay(hView));

    bool settled;
    if (!spotlight)
    {
        if (g_s.hOverlay)
        {
            DestroyOverlay();
        }
        g_s.fadeSpot = 0.0f;
        g_s.fadeSel = 0.0f;

        const float target = (g_s.mouseOver || selTarget) ? 1.0f : 0.0f;
        g_s.revealWhole = Approach(g_s.revealWhole, target, step);

        const int idle = IdleAlpha();
        ApplyViewAlpha(hView, idle + (int)((float)(255 - idle) * g_s.revealWhole + 0.5f));
        settled = g_s.revealWhole == target;
    }
    else
    {
        g_s.revealWhole = 0.0f;
        ApplyViewAlpha(hView, IdleAlpha());

        const float spotTarget = g_s.mouseOver ? 1.0f : 0.0f;
        const float selFadeTarget = selTarget ? 1.0f : 0.0f;
        g_s.fadeSpot = Approach(g_s.fadeSpot, spotTarget, step);
        g_s.fadeSel = Approach(g_s.fadeSel, selFadeTarget, step);

        RenderOverlay(hView, hList, radiusPx);

        // While the cursor is over the desktop the circle follows it, so the timer keeps running.
        settled = g_s.fadeSpot == spotTarget && g_s.fadeSel == selFadeTarget && !g_s.mouseOver;
    }

    // A selection that is only held for a while needs a tick when the hold runs out.
    if (holding)
    {
        settled = false;
    }
    if (settled)
    {
        StopTimer(hView);
    }
    g_s.inTick = false;
}

void OnMouseActivity(HWND hView, HWND hReceiver)
{
    if (!g_s.attached)
    {
        return;
    }
    g_s.mouseOver = true;

    TRACKMOUSEEVENT tme = {};
    tme.cbSize = sizeof(tme);
    tme.dwFlags = TME_LEAVE;
    tme.hwndTrack = hReceiver;
    TrackMouseEvent(&tme);

    EnsureTimer(hView);
}

void OnMouseLeave(HWND hView)
{
    if (!g_s.attached)
    {
        return;
    }
    if (!CursorOverDesktop(hView, g_hList.load(std::memory_order_relaxed)))
    {
        g_s.mouseOver = false;
        EnsureTimer(hView);
    }
}

void OnSelectionChanged(HWND hView)
{
    if (!g_s.attached)
    {
        return;
    }
    g_s.lastSelectionChange = GetTickCount();
    EnsureTimer(hView);
}

// ---------------------------------------------------------------------------------------------------------------
// Attach, apply, tear down (desktop thread)
// ---------------------------------------------------------------------------------------------------------------

void InitState(HWND hView)
{
    g_s = State();

    COLORREF key = 0;
    BYTE alpha = 255;
    DWORD flags = 0;
    if (GetLayeredWindowAttributes(hView, &key, &alpha, &flags) && flags)
    {
        g_s.origKey = key;
        g_s.origAlpha = alpha;
        g_s.origFlags = flags;
    }
    g_s.attached = true;
    g_s.lastTick = GetTickCount();
    g_s.mouseOver = CursorOverDesktop(hView, g_hList.load(std::memory_order_relaxed));
    SP_LogDebug(L"The view had alpha %u, flags 0x%X", (unsigned)g_s.origAlpha, (unsigned)g_s.origFlags);
}

void Teardown(HWND hView)
{
    StopTimer(hView);
    DestroyOverlay();
    if (g_s.attached)
    {
        SetLayeredWindowAttributes(hView, g_s.origKey, g_s.origAlpha, g_s.origFlags ? g_s.origFlags : LWA_ALPHA);
        SP_Log(L"The desktop icons are back at their original opacity");
    }
    g_s = State();
}

void HandleOp(HWND hView, int op)
{
    switch (op)
    {
    case OP_APPLY:
        if (!g_active.load(std::memory_order_relaxed))
        {
            return;
        }
        if (!g_s.attached)
        {
            InitState(hView);
        }
        // Settings may have changed the mode or the idle alpha: the next tick re-applies both, and a failed
        // overlay gets another chance.
        g_s.appliedAlpha = -1;
        g_s.overlayFailed = false;
        EnsureTimer(hView);
        break;

    case OP_HEARTBEAT:
        if (!g_active.load(std::memory_order_relaxed) || !g_s.attached)
        {
            return;
        }
        {
            // If the shell set the alpha again behind our back, it is re-applied.
            COLORREF key = 0;
            BYTE alpha = 255;
            DWORD flags = 0;
            if (GetLayeredWindowAttributes(hView, &key, &alpha, &flags) && (int)alpha != g_s.appliedAlpha)
            {
                g_s.appliedAlpha = -1;
                EnsureTimer(hView);
            }
            if (g_s.hOverlay)
            {
                PlaceOverlay(hView);
            }
        }
        break;

    case OP_TEARDOWN:
        Teardown(hView);
        break;
    }
}

void OnViewDestroyed(HWND hView)
{
    StopTimer(hView);
    DestroyOverlay();
    g_s = State();

    HWND expected = hView;
    if (g_hView.compare_exchange_strong(expected, nullptr))
    {
        g_hList.store(nullptr, std::memory_order_relaxed);
        SP_Log(L"The desktop view went away; watching for the next one");
    }
}

// ---------------------------------------------------------------------------------------------------------------
// The subclasses
// ---------------------------------------------------------------------------------------------------------------

LRESULT CALLBACK ViewSubclass(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                              UINT_PTR idSubclass, DWORD_PTR refData)
{
    UNREFERENCED_PARAMETER(idSubclass);
    UNREFERENCED_PARAMETER(refData);

    if (g_msg && uMsg == g_msg && wParam == kMagic)
    {
        HandleOp(hWnd, (int)lParam);
        return 0;
    }

    switch (uMsg)
    {
    case WM_TIMER:
        if (wParam == kTimerId)
        {
            if (g_s.attached && g_active.load(std::memory_order_relaxed))
            {
                Tick(hWnd);
            }
            else
            {
                StopTimer(hWnd);
            }
            return 0;
        }
        break;

    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        // The view only sees the mouse when the icon list is hidden or does not cover it.
        OnMouseActivity(hWnd, hWnd);
        break;

    case WM_MOUSELEAVE:
        OnMouseLeave(hWnd);
        break;

    case WM_NOTIFY:
    {
        const NMHDR* hdr = (const NMHDR*)lParam;
        if (!hdr || !g_s.attached)
        {
            break;
        }
        HWND hList = g_hList.load(std::memory_order_relaxed);
        if (hdr->hwndFrom != hList && !ClassIs(hdr->hwndFrom, L"SysListView32"))
        {
            break;
        }
        switch (hdr->code)
        {
        case LVN_ITEMCHANGED:
        {
            const NMLISTVIEW* nm = (const NMLISTVIEW*)lParam;
            if ((nm->uNewState ^ nm->uOldState) & LVIS_SELECTED)
            {
                OnSelectionChanged(hWnd);
            }
            break;
        }
        case LVN_ODSTATECHANGED:
        {
            const NMLVODSTATECHANGE* nm = (const NMLVODSTATECHANGE*)lParam;
            if ((nm->uNewState ^ nm->uOldState) & LVIS_SELECTED)
            {
                OnSelectionChanged(hWnd);
            }
            break;
        }
        case LVN_DELETEALLITEMS:
        case LVN_DELETEITEM:
        case LVN_INSERTITEM:
            OnSelectionChanged(hWnd);
            break;

        case LVN_BEGINLABELEDITW:
        case LVN_BEGINLABELEDITA:
            g_s.editing = true;
            OnSelectionChanged(hWnd);
            break;

        case LVN_ENDLABELEDITW:
        case LVN_ENDLABELEDITA:
            g_s.editing = false;
            OnSelectionChanged(hWnd);
            break;
        }
        break;
    }

    case WM_WINDOWPOSCHANGED:
        if (g_s.attached && g_s.hOverlay)
        {
            PlaceOverlay(hWnd);
            EnsureTimer(hWnd);
        }
        break;

    case WM_NCDESTROY:
        OnViewDestroyed(hWnd);
        RemoveWindowSubclass(hWnd, ViewSubclass, kSubclassId);
        break;
    }

    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK ListSubclass(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                              UINT_PTR idSubclass, DWORD_PTR refData)
{
    UNREFERENCED_PARAMETER(idSubclass);
    UNREFERENCED_PARAMETER(refData);

    switch (uMsg)
    {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    {
        HWND hView = g_hView.load(std::memory_order_relaxed);
        if (hView)
        {
            OnMouseActivity(hView, hWnd);
        }
        break;
    }

    case WM_MOUSELEAVE:
    {
        HWND hView = g_hView.load(std::memory_order_relaxed);
        if (hView)
        {
            OnMouseLeave(hView);
        }
        break;
    }

    case WM_NCDESTROY:
    {
        HWND expected = hWnd;
        g_hList.compare_exchange_strong(expected, nullptr);
        RemoveWindowSubclass(hWnd, ListSubclass, kSubclassId);
        break;
    }
    }

    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

// ---------------------------------------------------------------------------------------------------------------
// Attaching (watcher thread) and the lifecycle (engine thread)
// ---------------------------------------------------------------------------------------------------------------

void SendOp(HWND hView, Op op)
{
    if (!hView || !IsWindow(hView) || !g_msg)
    {
        return;
    }
    // Sent with a timeout so a hung desktop thread cannot take the engine down with it.
    DWORD_PTR ignored = 0;
    SendMessageTimeoutW(hView, g_msg, kMagic, (LPARAM)op, SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &ignored);
}

const wchar_t* ModeText()
{
    return g_spotlightRadius.load(std::memory_order_relaxed) > 0 ? L"spotlight" : L"whole desktop";
}

// One attempt to get hold of the desktop. Returns TRUE when attached after the call.
bool Attach()
{
    HWND hView = g_hView.load(std::memory_order_relaxed);
    if (hView && IsWindow(hView))
    {
        // The list can be rebuilt on its own; pick it up late if needed.
        HWND hList = g_hList.load(std::memory_order_relaxed);
        if (!hList || !IsWindow(hList))
        {
            HWND hNew = ListOf(hView);
            if (hNew && SP_SetWindowSubclassFromAnyThread(hNew, ListSubclass, kSubclassId, 0))
            {
                g_hList.store(hNew, std::memory_order_relaxed);
                SP_Log(L"Watching the desktop icon list %p", hNew);
            }
        }
        return true;
    }

    g_hView.store(nullptr, std::memory_order_relaxed);
    g_hList.store(nullptr, std::memory_order_relaxed);

    hView = FindDesktopView();
    if (!hView)
    {
        return false;
    }

    // The whole approach rests on the view being a layered window (it is on every Windows 11 build). Adding the
    // style to a view that does not have it could turn the desktop black, so that is refused rather than tried.
    if (!(GetWindowLongPtrW(hView, GWL_EXSTYLE) & WS_EX_LAYERED))
    {
        static bool s_said = false;
        if (!s_said)
        {
            s_said = true;
            SP_LogError(L"The desktop view %p is not a layered window on this build; the mod does nothing", hView);
        }
        return false;
    }

    if (!SP_SetWindowSubclassFromAnyThread(hView, ViewSubclass, kSubclassId, 0))
    {
        SP_LogError(L"The desktop view %p could not be subclassed", hView);
        return false;
    }
    g_hView.store(hView, std::memory_order_relaxed);

    HWND hList = ListOf(hView);
    if (hList && SP_SetWindowSubclassFromAnyThread(hList, ListSubclass, kSubclassId, 0))
    {
        g_hList.store(hList, std::memory_order_relaxed);
    }

    SendOp(hView, OP_APPLY);

    SP_Log(L"Watching the desktop (view %p, list %p): icons at %d%% when idle, fade %d ms, %s",
           hView, hList, g_idleOpacity.load(std::memory_order_relaxed), g_fadeMs.load(std::memory_order_relaxed),
           ModeText());
    return true;
}

// Finds the desktop when it appears and again after the shell rebuilds it, and nudges the view once a second so
// an alpha the shell reset is put back.
DWORD WINAPI WatchThread(LPVOID parameter)
{
    UNREFERENCED_PARAMETER(parameter);

    int missed = 0;
    for (;;)
    {
        if (g_active.load(std::memory_order_relaxed))
        {
            if (Attach())
            {
                missed = 0;
                HWND hView = g_hView.load(std::memory_order_relaxed);
                if (hView)
                {
                    PostMessageW(hView, g_msg, kMagic, (LPARAM)OP_HEARTBEAT);
                }
            }
            else if (++missed == 60)
            {
                SP_LogDebug(L"Still waiting for the desktop view");
                missed = 0;
            }
        }

        if (WaitForSingleObject(g_watchStop, kHeartbeatMs) != WAIT_TIMEOUT)
        {
            return 0;
        }
    }
}

void LoadSettings()
{
    g_idleOpacity.store(Clamp(SP_GetIntSetting(L"IdleOpacity", 35), 0, 100), std::memory_order_relaxed);
    g_fadeMs.store(Clamp(SP_GetIntSetting(L"FadeMs", 300), 0, 10000), std::memory_order_relaxed);
    g_spotlightRadius.store(Clamp(SP_GetIntSetting(L"SpotlightRadius", 0), 0, 4000), std::memory_order_relaxed);
    SP_Log(L"IdleOpacity=%d%% FadeMs=%d SpotlightRadius=%d (%s)",
           g_idleOpacity.load(std::memory_order_relaxed), g_fadeMs.load(std::memory_order_relaxed),
           g_spotlightRadius.load(std::memory_order_relaxed), ModeText());
}

BOOL Init()
{
    LoadSettings();

    g_msg = RegisterWindowMessageW(L"ShadePatcher.DesktopIconsSpotlight.Op");
    if (!g_msg)
    {
        SP_LogError(L"The private message could not be registered");
        return FALSE;
    }
    if (!RegisterOverlayClass())
    {
        // Only the spotlight needs the class; dimming the whole desktop still works.
        SP_LogError(L"The overlay window class could not be registered; the spotlight is not available");
    }

    g_active.store(true, std::memory_order_relaxed);
    return TRUE;
}

void AfterInit()
{
    g_watchStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_watchStop)
    {
        SP_LogError(L"The watcher could not be started: %lu", GetLastError());
        return;
    }
    // The thread makes the first attempt itself, so an existing desktop is attached within a moment and a
    // desktop that is not up yet is picked up when it appears.
    g_watchThread = CreateThread(nullptr, 0, WatchThread, nullptr, 0, nullptr);
    if (!g_watchThread)
    {
        SP_LogError(L"The watcher could not be started: %lu", GetLastError());
        CloseHandle(g_watchStop);
        g_watchStop = nullptr;
    }
}

void SettingsChanged()
{
    LoadSettings();
    SendOp(g_hView.load(std::memory_order_relaxed), OP_APPLY);
}

void BeforeUninit()
{
    g_active.store(false, std::memory_order_relaxed);

    if (g_watchStop)
    {
        SetEvent(g_watchStop);
    }
    if (g_watchThread)
    {
        WaitForSingleObject(g_watchThread, 5000);
        CloseHandle(g_watchThread);
        g_watchThread = nullptr;
    }
    if (g_watchStop)
    {
        CloseHandle(g_watchStop);
        g_watchStop = nullptr;
    }

    HWND hView = g_hView.exchange(nullptr);
    HWND hList = g_hList.exchange(nullptr);

    // The view is put back the way the shell had it on its own thread, then both subclasses are removed.
    if (hView && IsWindow(hView))
    {
        SendOp(hView, OP_TEARDOWN);
        SP_RemoveWindowSubclassFromAnyThread(hView, ViewSubclass, kSubclassId);
    }
    if (hList && IsWindow(hList))
    {
        SP_RemoveWindowSubclassFromAnyThread(hList, ListSubclass, kSubclassId);
    }
}

void Uninit()
{
    UnregisterClassW(kOverlayClass, ThisModule());
}

}   // namespace

SP_MOD_DEFINE(g_modDesktopIconsSpotlight) =
{
    /* id             */ SP_MOD_ID,
    /* name           */ L"Dim the desktop icons when idle, with a spotlight",
    /* basedOn        */ "transparent-desktop-icons-spotlight",
    /* originalAuthor */ "drgutman",
    /* targets        */ SP_TARGET_EXPLORER,
    /* minOsBuild     */ 22000,
    /* flags          */ 0,
    /* Init           */ Init,
    /* AfterInit      */ AfterInit,
    /* SettingsChanged*/ SettingsChanged,
    /* BeforeUninit   */ BeforeUninit,
    /* Uninit         */ Uninit,
};
