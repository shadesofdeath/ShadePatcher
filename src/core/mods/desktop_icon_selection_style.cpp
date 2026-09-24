//
// desktop-icon-selection-style - restyle the highlight behind selected desktop icons: a gap around it, rounded
// corners, a translucent fill in the accent colour (or one of the user's choosing), a soft glow and a solid,
// dashed or dotted border.
//
// Adapted from the idea behind the Windhawk mod "Desktop Icon Selection Style" (desktop-icon-selection-style)
// by RiteshK. The implementation here is written against this engine's API.
//
// How the desktop draws a selection
// ---------------------------------
// The desktop is a plain SysListView32 inside SHELLDLL_DefView. It is not owner drawn and the shell view does not
// custom draw the item background: comctl32 paints the plate behind a selected or hovered item itself, with the
// ListView theme class, part LVP_LISTITEM and one of the LISS_* states, through uxtheme's DrawThemeBackground.
// The original author measured exactly that on build 26200, and measured that the draw arrives on a memory DC
// (the desktop is double buffered), so WindowFromDC cannot tell whose draw it is. This mod therefore:
//
//   1. subclasses the desktop list view and counts, in a thread-local, how deep it is inside WM_PAINT,
//      WM_PRINTCLIENT or WM_ERASEBKGND (WM_NCPAINT is deliberately excluded: the scroll bar the list view grows
//      when icons fall outside the work area is themed with the ScrollBar class, whose part 1 / states 2..6 line
//      up with the list item states and would get a plate painted over its arrows);
//   2. hooks DrawThemeBackground and DrawThemeBackgroundEx in uxtheme. DrawThemeBackground is a wrapper over
//      the Ex function; both are hooked, with a re-entrancy flag so a call that passes through the first is not
//      examined twice, in case this build's comctl32 reaches the inner one directly;
//   3. while the desktop is painting, a LVP_LISTITEM draw in a selected or hot state is replaced by this mod's
//      own plate, drawn with GDI+ (antialiased rounded path, alpha fill, dashed pen). Anything else goes to the
//      original untouched.
//
// Because another port found that the desktop's labels did not reach the DrawTextW family on this build, the
// paint gate is instrumented: the subclass logs once when it sees the desktop paint, the hook logs once when it
// first replaces a selection plate, and if the desktop paints a selected item without a single LVP_LISTITEM draw
// reaching the hook, that is logged too, so a build that draws the selection some other way is visible in the
// log at info level rather than silently inert. As a backstop, while the subclass has never seen a paint message
// a selection draw arriving on the desktop's own thread is still taken; that fallback stands down the moment
// the subclass proves it is working.
//
// Colour, DPI
// -----------
// "Accent" is read through uxtheme's immersive colour API (the colour picked in Settings), with
// DwmGetColorizationColor as a fallback, and refreshed at most every few seconds because colour-change
// broadcasts do not reach a child window. Pixel settings are scaled by the list view's own DPI, which is also
// what the item rectangles are measured in, so the plate and the icon stay in proportion on any monitor.
//
#define SP_MOD_ID "desktop-icon-selection-style"
#include "engine/modapi.h"

#include <unknwn.h>
#include <objbase.h>
#include <objidl.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwctype>

// The GDI+ headers use unqualified min/max, which NOMINMAX (defined project-wide) takes away.
namespace Gdiplus
{
    using std::min;
    using std::max;
}
#include <gdiplus.h>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "Gdi32.lib")
#pragma comment(lib, "Gdiplus.lib")
#pragma comment(lib, "UxTheme.lib")

#ifndef WM_DPICHANGED_BEFOREPARENT
#define WM_DPICHANGED_BEFOREPARENT 0x02E2
#endif
#ifndef WM_DPICHANGED_AFTERPARENT
#define WM_DPICHANGED_AFTERPARENT 0x02E3
#endif

namespace {

// ListView theme part and states, spelled out so no vssym32.h dependency is needed.
constexpr int kLVP_LISTITEM = 1;
constexpr int kLISS_HOT = 2;
constexpr int kLISS_SELECTED = 3;
constexpr int kLISS_SELECTEDNOTFOCUS = 5;
constexpr int kLISS_HOTSELECTED = 6;

enum BorderStyle
{
    kBorderNone = 0,
    kBorderSolid = 1,
    kBorderDashed = 2,
    kBorderDotted = 3,
};

// Fixed parts of the look that the original exposed as settings and this port does not (see the notes).
constexpr int   kBorderOpacityPercent = 70;    // white border
constexpr int   kBorderThicknessPx = 1;        // logical pixels
constexpr int   kDashLengthPx = 2;
constexpr int   kDashGapPx = 2;
constexpr int   kGlowOpacityPercent = 40;      // reached at the plate edge
constexpr int   kGlowSizePx = 4;               // logical pixels beyond the plate
constexpr int   kMaxGlowDevicePixels = 48;     // one antialiased fill per unit, per item, per repaint
constexpr int   kMaxPixelSetting = 256;
constexpr int   kMinPlateSize = 4;             // padding never shrinks the plate below this
constexpr DWORD kAccentRefreshMs = 3000;
constexpr UINT_PTR kSubclassId = 1;

struct Style
{
    int      cornerRadius = 8;       // logical pixels, 0 = sharp
    int      opacity = 40;           // 0..100 fill opacity
    int      borderStyle = kBorderDashed;
    int      padding = 4;            // logical pixels trimmed from every side of the item rect
    bool     glow = true;
    bool     colorIsAccent = true;
    COLORREF color = RGB(0, 120, 215);
};

// Written on the engine thread, read on the desktop's thread inside the hooks.
SRWLOCK g_styleLock = SRWLOCK_INIT;
Style   g_style;

std::atomic<bool>  g_active{ false };          // FALSE once BeforeUninit ran: hooks and subclass stand down
std::atomic<HWND>  g_lv{ nullptr };            // the one desktop list view being watched
std::atomic<DWORD> g_lvThread{ 0 };            // its thread, for the paint-gate fallback
std::atomic<UINT>  g_dpi{ 96 };
std::atomic<COLORREF> g_accent{ RGB(0, 120, 215) };
std::atomic<DWORD> g_accentTick{ 0 };          // 0 forces the next read

// Diagnostics, all info-level and each logged once.
std::atomic<bool> g_paintSeen{ false };        // the subclass has seen the desktop paint
std::atomic<bool> g_hitLogged{ false };        // the hook replaced a selection plate at least once
std::atomic<bool> g_missLogged{ false };       // a selected item painted without reaching the hook
std::atomic<bool> g_fallbackLogged{ false };
std::atomic<unsigned> g_replacedCount{ 0 };
std::atomic<unsigned> g_outsidePaintCount{ 0 };

// GDI+ is started on the first attach, from whichever thread gets there first.
SRWLOCK   g_gdiplusLock = SRWLOCK_INIT;
ULONG_PTR g_gdiplusToken = 0;
std::atomic<bool> g_gdiplusReady{ false };

// Non-zero while the desktop list view is inside a paint message on this thread.
thread_local int  t_paintDepth = 0;
thread_local bool t_inHook = false;            // DrawThemeBackground calls DrawThemeBackgroundEx
thread_local int  t_listItemDraws = 0;         // LVP_LISTITEM draws seen during the current paint
thread_local int  t_replacedDraws = 0;         // of those, how many this mod painted

// ---------------------------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------------------------

int Scale(int logical)
{
    return MulDiv(logical, (int)g_dpi.load(std::memory_order_relaxed), 96);
}

Style CurrentStyle()
{
    AcquireSRWLockShared(&g_styleLock);
    Style copy = g_style;
    ReleaseSRWLockShared(&g_styleLock);
    return copy;
}

bool EnsureGdiplus()
{
    AcquireSRWLockExclusive(&g_gdiplusLock);
    bool ok = g_gdiplusReady.load(std::memory_order_relaxed);
    if (!ok)
    {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&g_gdiplusToken, &input, nullptr) == Gdiplus::Ok)
        {
            g_gdiplusReady.store(true, std::memory_order_release);
            ok = true;
        }
        else
        {
            SP_LogError(L"GDI+ could not be started; the stock highlight stays");
        }
    }
    ReleaseSRWLockExclusive(&g_gdiplusLock);
    return ok;
}

void ShutdownGdiplus()
{
    AcquireSRWLockExclusive(&g_gdiplusLock);
    if (g_gdiplusReady.load(std::memory_order_relaxed))
    {
        g_gdiplusReady.store(false, std::memory_order_release);
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }
    ReleaseSRWLockExclusive(&g_gdiplusLock);
}

// The colour picked in Settings > Personalization > Colors. DwmGetColorizationColor returns that colour blended
// with the colorization intensity, which can look visibly different, so the immersive API is tried first.
COLORREF ReadAccentColor()
{
    using GetImmersiveColorFromColorSetEx_t = DWORD(WINAPI*)(DWORD, DWORD, BOOL, DWORD);
    using GetImmersiveColorTypeFromName_t = DWORD(WINAPI*)(LPCWSTR);
    using GetImmersiveUserColorSetPreference_t = DWORD(WINAPI*)(BOOL, BOOL);

    static const HMODULE hUxtheme = GetModuleHandleW(L"uxtheme.dll");
    static const auto pFromColorSet = hUxtheme
        ? (GetImmersiveColorFromColorSetEx_t)GetProcAddress(hUxtheme, MAKEINTRESOURCEA(95)) : nullptr;
    static const auto pTypeFromName = hUxtheme
        ? (GetImmersiveColorTypeFromName_t)GetProcAddress(hUxtheme, MAKEINTRESOURCEA(96)) : nullptr;
    static const auto pUserColorSet = hUxtheme
        ? (GetImmersiveUserColorSetPreference_t)GetProcAddress(hUxtheme, MAKEINTRESOURCEA(98)) : nullptr;

    if (pFromColorSet && pTypeFromName && pUserColorSet)
    {
        DWORD abgr = pFromColorSet(pUserColorSet(FALSE, FALSE),
                                   pTypeFromName(L"ImmersiveStartHoverBackground"), TRUE, 0);
        return RGB(abgr & 0xFF, (abgr >> 8) & 0xFF, (abgr >> 16) & 0xFF);
    }

    DWORD argb = 0;
    BOOL opaque = FALSE;
    if (SUCCEEDED(DwmGetColorizationColor(&argb, &opaque)))
    {
        return RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
    }
    return RGB(0, 120, 215);
}

// Colour-change broadcasts only reach top-level windows and the list view is a child, so the accent is polled:
// at most once every few seconds, and only while something actually follows it.
void RefreshAccentIfStale(const Style& s)
{
    if (!s.colorIsAccent)
    {
        return;
    }
    const DWORD now = GetTickCount();
    const DWORD last = g_accentTick.load(std::memory_order_relaxed);
    if (last != 0 && now - last < kAccentRefreshMs)
    {
        return;
    }
    g_accentTick.store(now ? now : 1, std::memory_order_relaxed);
    g_accent.store(ReadAccentColor(), std::memory_order_relaxed);
}

UINT ReadWindowDpi(HWND hWnd)
{
    UINT dpi = hWnd ? GetDpiForWindow(hWnd) : 0;
    return dpi ? dpi : 96;
}

// "RRGGBB" or "#RRGGBB"; "accent" or an empty string means the accent colour. Anything else is refused.
bool ParseColor(const wchar_t* text, COLORREF* color, bool* isAccent)
{
    if (!text)
    {
        return false;
    }
    while (*text && iswspace(*text))
    {
        text++;
    }
    size_t length = wcslen(text);
    while (length > 0 && iswspace(text[length - 1]))
    {
        length--;
    }

    if (length == 0 || (length == 6 && _wcsnicmp(text, L"accent", 6) == 0))
    {
        *isAccent = true;
        return true;
    }

    if (length > 0 && text[0] == L'#')
    {
        text++;
        length--;
    }
    if (length != 6)
    {
        return false;
    }

    unsigned long rgb = 0;
    for (size_t i = 0; i < 6; ++i)
    {
        const wchar_t c = text[i];
        unsigned digit;
        if (c >= L'0' && c <= L'9')
        {
            digit = (unsigned)(c - L'0');
        }
        else if (c >= L'a' && c <= L'f')
        {
            digit = (unsigned)(c - L'a' + 10);
        }
        else if (c >= L'A' && c <= L'F')
        {
            digit = (unsigned)(c - L'A' + 10);
        }
        else
        {
            return false;
        }
        rgb = (rgb << 4) | digit;
    }

    *isAccent = false;
    *color = RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
    return true;
}

Gdiplus::Color MakeColor(COLORREF color, int opacityPercent)
{
    const int alpha = MulDiv(std::clamp(opacityPercent, 0, 100), 255, 100);
    return Gdiplus::Color((BYTE)alpha, GetRValue(color), GetGValue(color), GetBValue(color));
}

COLORREF PlateColor(const Style& s)
{
    return s.colorIsAccent ? g_accent.load(std::memory_order_relaxed) : s.color;
}

// ---------------------------------------------------------------------------------------------------------------
// Finding the desktop list view
// ---------------------------------------------------------------------------------------------------------------

bool ClassIs(HWND hWnd, const wchar_t* name)
{
    wchar_t wszClass[64];
    return hWnd && GetClassNameW(hWnd, wszClass, ARRAYSIZE(wszClass)) && _wcsicmp(wszClass, name) == 0;
}

// A SysListView32 inside SHELLDLL_DefView whose parent is the shell window, Progman or a WorkerW (a slideshow
// or a wallpaper tool moves the view there). A File Explorer view has a different chain and never matches.
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

HWND ListViewUnder(HWND hView)
{
    if (!hView)
    {
        return nullptr;
    }
    HWND hList = FindWindowExW(hView, nullptr, L"SysListView32", nullptr);
    if (!hList)
    {
        return nullptr;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hList, &pid);
    return pid == GetCurrentProcessId() ? hList : nullptr;
}

// Every host is tried and the first that actually holds a list view wins: Progman can own a shell view with no
// icons in it while the real one sits under a WorkerW, so stopping at the first SHELLDLL_DefView is not enough.
HWND FindDesktopListView()
{
    HWND hProgman = GetShellWindow();
    if (!hProgman)
    {
        hProgman = FindWindowW(L"Progman", nullptr);
    }
    if (HWND hList = ListViewUnder(FindWindowExW(hProgman, nullptr, L"SHELLDLL_DefView", nullptr)))
    {
        return hList;
    }

    HWND hWorker = nullptr;
    while ((hWorker = FindWindowExW(nullptr, hWorker, L"WorkerW", nullptr)) != nullptr)
    {
        if (HWND hList = ListViewUnder(FindWindowExW(hWorker, nullptr, L"SHELLDLL_DefView", nullptr)))
        {
            return hList;
        }
    }
    return nullptr;
}

// Invalidates only; a synchronous repaint from the engine thread would send into the desktop's thread.
void RepaintDesktop(HWND hList)
{
    if (hList && IsWindow(hList))
    {
        RedrawWindow(hList, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Plate geometry and drawing
// ---------------------------------------------------------------------------------------------------------------

// The item rect trimmed by the padding on every side. Always inside the item rect: that is all Windows
// invalidated, and painting outside it would leave stale plates behind when the selection moves.
RECT ComputePlateRect(const RECT& item, const Style& s)
{
    const int width = item.right - item.left;
    const int height = item.bottom - item.top;
    if (width <= 0 || height <= 0)
    {
        return RECT{ 0, 0, 0, 0 };
    }

    int pad = Scale(s.padding);
    pad = std::min(pad, (width - kMinPlateSize) / 2);
    pad = std::min(pad, (height - kMinPlateSize) / 2);
    pad = std::max(pad, 0);

    RECT rc = { item.left + pad, item.top + pad, item.right - pad, item.bottom - pad };
    RECT clamped;
    if (!IntersectRect(&clamped, &rc, &item))
    {
        return RECT{ 0, 0, 0, 0 };
    }
    return clamped;
}

void AddRoundedRect(Gdiplus::GraphicsPath* path, const Gdiplus::RectF& rect, Gdiplus::REAL radius)
{
    if (radius < 0.5f)
    {
        path->AddRectangle(rect);
        path->CloseFigure();
        return;
    }
    const Gdiplus::REAL d = radius * 2;
    const Gdiplus::REAL right = rect.X + rect.Width;
    const Gdiplus::REAL bottom = rect.Y + rect.Height;
    path->AddArc(rect.X, rect.Y, d, d, 180.0f, 90.0f);
    path->AddArc(right - d, rect.Y, d, d, 270.0f, 90.0f);
    path->AddArc(right - d, bottom - d, d, d, 0.0f, 90.0f);
    path->AddArc(rect.X, bottom - d, d, d, 90.0f, 90.0f);
    path->CloseFigure();
}

// Stacked translucent rings, not a blur: the wallpaper is not available when the plate is drawn, so a real blur
// is impossible. Nothing clamps the rings to the item rect on purpose; GDI+ honours the DC's clip region, which
// is exactly the area Windows invalidated, so an oversized ring is cut off rather than left on screen.
void DrawGlow(Gdiplus::Graphics* g, const Gdiplus::RectF& plate, Gdiplus::REAL radius, const Style& s)
{
    const int size = std::min(Scale(kGlowSizePx), kMaxGlowDevicePixels);
    if (size < 1)
    {
        return;
    }

    // N layers of alpha a compose to 1 - (1 - a)^N; solve for the per-layer alpha that lands on the target
    // opacity at the plate edge.
    const double target = kGlowOpacityPercent / 100.0;
    const double perLayer = 1.0 - std::pow(1.0 - target, 1.0 / size);
    const BYTE alpha = (BYTE)std::clamp((int)std::lround(perLayer * 255.0), 1, 255);

    const COLORREF base = PlateColor(s);
    Gdiplus::SolidBrush brush(Gdiplus::Color(alpha, GetRValue(base), GetGValue(base), GetBValue(base)));

    for (int i = size; i >= 1; --i)
    {
        Gdiplus::RectF ring = plate;
        ring.Inflate((Gdiplus::REAL)i, (Gdiplus::REAL)i);
        Gdiplus::GraphicsPath path;
        AddRoundedRect(&path, ring, radius + (Gdiplus::REAL)i);
        g->FillPath(&brush, &path);
    }
}

void ApplyDashPattern(Gdiplus::Pen* pen, Gdiplus::REAL thickness, int borderStyle)
{
    // GDI+ dash lengths are multiples of the pen width; the constants are in pixels.
    const Gdiplus::REAL dash = std::max(1.0f, (Gdiplus::REAL)Scale(kDashLengthPx)) / thickness;
    const Gdiplus::REAL gap = std::max(1.0f, (Gdiplus::REAL)Scale(kDashGapPx)) / thickness;

    if (borderStyle == kBorderDashed)
    {
        Gdiplus::REAL pattern[] = { dash, gap };
        pen->SetDashPattern(pattern, 2);
    }
    else if (borderStyle == kBorderDotted)
    {
        // One pen width long with a round cap, so it reads as a dot rather than a tiny square.
        Gdiplus::REAL pattern[] = { 1.0f, gap };
        pen->SetDashPattern(pattern, 2);
        pen->SetDashCap(Gdiplus::DashCapRound);
    }
}

// Paints this mod's plate for one item. Returns false when nothing could be drawn, in which case the caller
// lets the stock highlight through, so a failure here is visible rather than a vanished selection.
bool DrawPlate(HDC hdc, const RECT& item, const RECT* clip, bool hot, const Style& s)
{
    RefreshAccentIfStale(s);

    const RECT plate = ComputePlateRect(item, s);
    if (IsRectEmpty(&plate))
    {
        return false;
    }

    Gdiplus::Graphics g(hdc);
    if (g.GetLastStatus() != Gdiplus::Ok)
    {
        return false;
    }
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

    if (clip)
    {
        g.SetClip(Gdiplus::Rect(clip->left, clip->top, clip->right - clip->left, clip->bottom - clip->top));
    }

    Gdiplus::REAL thickness = s.borderStyle != kBorderNone
        ? (Gdiplus::REAL)std::max(1, Scale(kBorderThicknessPx)) : 0.0f;

    Gdiplus::RectF rect((Gdiplus::REAL)plate.left, (Gdiplus::REAL)plate.top,
                        (Gdiplus::REAL)(plate.right - plate.left), (Gdiplus::REAL)(plate.bottom - plate.top));

    // The stroke straddles the path, so the path is pulled in by half the pen width to keep the border inside
    // the plate. The inset is applied whether or not the border is drawn for this state, so a hovered plate and
    // a selected one are exactly the same size and nothing shifts between the two.
    if (thickness > 0.0f)
    {
        const Gdiplus::REAL maxThickness = std::min(rect.Width, rect.Height) - 1.0f;
        thickness = std::max(0.0f, std::min(thickness, maxThickness));
        rect.Inflate(-thickness / 2, -thickness / 2);
    }
    if (rect.Width <= 0.0f || rect.Height <= 0.0f)
    {
        return false;
    }

    const Gdiplus::REAL radius =
        std::min((Gdiplus::REAL)Scale(s.cornerRadius), std::min(rect.Width, rect.Height) / 2);

    Gdiplus::GraphicsPath path;
    AddRoundedRect(&path, rect, radius);

    // Under everything, with the plate's own area excluded so a translucent fill is not darkened by the glow
    // sitting behind it.
    if (s.glow)
    {
        Gdiplus::GraphicsState state = g.Save();
        g.SetClip(&path, Gdiplus::CombineModeExclude);
        DrawGlow(&g, rect, radius, s);
        g.Restore(state);
    }

    if (s.opacity > 0)
    {
        Gdiplus::SolidBrush brush(MakeColor(PlateColor(s), s.opacity));
        g.FillPath(&brush, &path);
    }

    // Hover gets the fill but not the border, so only a real selection is outlined.
    if (thickness > 0.0f && !hot)
    {
        Gdiplus::Pen pen(MakeColor(RGB(255, 255, 255), kBorderOpacityPercent), thickness);
        ApplyDashPattern(&pen, thickness, s.borderStyle);
        g.DrawPath(&pen, &path);
    }

    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// The theme hooks
// ---------------------------------------------------------------------------------------------------------------

using DrawThemeBackground_t = HRESULT(WINAPI*)(HTHEME, HDC, int, int, LPCRECT, LPCRECT);
using DrawThemeBackgroundEx_t = HRESULT(WINAPI*)(HTHEME, HDC, int, int, LPCRECT, const DTBGOPTS*);

DrawThemeBackground_t   g_origDrawThemeBackground = nullptr;
DrawThemeBackgroundEx_t g_origDrawThemeBackgroundEx = nullptr;

// Part 1 is LVP_LISTITEM only in the ListView class; other classes number from 1 too. Fail-open: only a theme
// positively identified as one of the classes whose part 1 / states 2..6 collide is refused. GetThemeClass is an
// undocumented ordinal, and matching the other way round would disable the whole mod if it ever changed.
bool IsForeignThemeClass(HTHEME hTheme)
{
    using GetThemeClass_t = HRESULT(WINAPI*)(HTHEME, LPWSTR, int);
    static const auto pGetThemeClass = []() -> GetThemeClass_t {
        HMODULE hUxtheme = GetModuleHandleW(L"uxtheme.dll");
        return hUxtheme ? (GetThemeClass_t)GetProcAddress(hUxtheme, MAKEINTRESOURCEA(74)) : nullptr;
    }();
    if (!pGetThemeClass)
    {
        return false;
    }

    wchar_t cls[64];
    if (FAILED(pGetThemeClass(hTheme, cls, ARRAYSIZE(cls))))
    {
        return false;
    }

    // A class list can be prefixed, as in "Explorer::ListView", so the tail is what identifies it.
    const size_t length = wcslen(cls);
    auto endsWith = [&](const wchar_t* suffix) {
        const size_t n = wcslen(suffix);
        return length >= n && _wcsicmp(cls + length - n, suffix) == 0;
    };
    // SBP_ARROWBTN, EP_EDITTEXT, HP_HEADERITEM and BP_PUSHBUTTON all sit at part 1.
    return endsWith(L"ScrollBar") || endsWith(L"Edit") || endsWith(L"Header") || endsWith(L"Button");
}

// The paint gate. True inside the desktop's paint; as a backstop, also true on the desktop's own thread while
// the subclass has never once seen it paint (so a build whose paint never reaches the subclass still works, and
// the log says so).
bool InsideDesktopPaint()
{
    if (t_paintDepth > 0)
    {
        return true;
    }
    const DWORD lvThread = g_lvThread.load(std::memory_order_relaxed);
    if (!lvThread || GetCurrentThreadId() != lvThread)
    {
        return false;
    }
    if (g_paintSeen.load(std::memory_order_relaxed))
    {
        // The subclass works, yet a selection draw arrived outside a paint message: counted, not taken.
        g_outsidePaintCount.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!g_fallbackLogged.exchange(true))
    {
        SP_Log(L"A list item draw arrived on the desktop thread before the subclass saw any paint message; "
               L"taking it by thread id until the subclass proves it works");
    }
    return true;
}

// Decides whether one theme draw is a desktop selection plate to replace; `hot` says which look it gets.
bool ShouldReplace(HTHEME hTheme, int partId, int stateId, bool* hot)
{
    if (partId != kLVP_LISTITEM || !g_active.load(std::memory_order_relaxed))
    {
        return false;
    }

    switch (stateId)
    {
    case kLISS_SELECTED:
    case kLISS_SELECTEDNOTFOCUS:
    case kLISS_HOTSELECTED:      // a selected icon under the mouse stays ours, or hovering would snap it back
        *hot = false;
        break;
    case kLISS_HOT:
        *hot = true;
        break;
    default:
        return false;
    }

    if (!InsideDesktopPaint())
    {
        return false;
    }
    t_listItemDraws++;

    if (!g_gdiplusReady.load(std::memory_order_acquire) || IsForeignThemeClass(hTheme))
    {
        return false;
    }
    return true;
}

void NoteReplaced(const wchar_t* entry, int stateId)
{
    t_replacedDraws++;
    g_replacedCount.fetch_add(1, std::memory_order_relaxed);
    if (!g_hitLogged.exchange(true))
    {
        SP_Log(L"Selection painting hook hit: %s LVP_LISTITEM state %d replaced on the desktop", entry, stateId);
    }
}

HRESULT WINAPI DrawThemeBackground_Hook(HTHEME hTheme, HDC hdc, int partId, int stateId,
                                        LPCRECT pRect, LPCRECT pClipRect)
{
    if (t_inHook)
    {
        return g_origDrawThemeBackground(hTheme, hdc, partId, stateId, pRect, pClipRect);
    }

    bool hot = false;
    if (pRect && ShouldReplace(hTheme, partId, stateId, &hot))
    {
        const Style s = CurrentStyle();
        if (DrawPlate(hdc, *pRect, pClipRect, hot, s))
        {
            NoteReplaced(L"DrawThemeBackground", stateId);
            return S_OK;
        }
    }

    t_inHook = true;
    HRESULT hr = g_origDrawThemeBackground(hTheme, hdc, partId, stateId, pRect, pClipRect);
    t_inHook = false;
    return hr;
}

HRESULT WINAPI DrawThemeBackgroundEx_Hook(HTHEME hTheme, HDC hdc, int partId, int stateId,
                                          LPCRECT pRect, const DTBGOPTS* pOptions)
{
    if (t_inHook)
    {
        return g_origDrawThemeBackgroundEx(hTheme, hdc, partId, stateId, pRect, pOptions);
    }

    bool hot = false;
    if (pRect && ShouldReplace(hTheme, partId, stateId, &hot))
    {
        const RECT* clip = nullptr;
        if (pOptions && pOptions->dwSize >= sizeof(DTBGOPTS) && (pOptions->dwFlags & DTBG_CLIPRECT))
        {
            clip = &pOptions->rcClip;
        }
        const Style s = CurrentStyle();
        if (DrawPlate(hdc, *pRect, clip, hot, s))
        {
            NoteReplaced(L"DrawThemeBackgroundEx", stateId);
            return S_OK;
        }
    }

    t_inHook = true;
    HRESULT hr = g_origDrawThemeBackgroundEx(hTheme, hdc, partId, stateId, pRect, pOptions);
    t_inHook = false;
    return hr;
}

// ---------------------------------------------------------------------------------------------------------------
// The desktop subclass
// ---------------------------------------------------------------------------------------------------------------

// TRUE when a selected item lies in the area about to be painted, so a paint that touches no selection is not
// mistaken for a build that draws the selection some other way. Only used while the diagnostics are pending.
bool SelectionInUpdateRect(HWND hWnd)
{
    RECT update;
    if (!GetUpdateRect(hWnd, &update, FALSE))
    {
        return false;
    }

    int index = (int)SendMessageW(hWnd, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    for (int guard = 0; index >= 0 && guard < 64; ++guard)
    {
        RECT rc = { LVIR_BOUNDS, 0, 0, 0 };
        RECT overlap;
        if (SendMessageW(hWnd, LVM_GETITEMRECT, (WPARAM)index, (LPARAM)&rc) && IntersectRect(&overlap, &rc, &update))
        {
            return true;
        }
        index = (int)SendMessageW(hWnd, LVM_GETNEXTITEM, (WPARAM)index, LVNI_SELECTED);
    }
    return false;
}

LRESULT CALLBACK DesktopListSubclass(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                     UINT_PTR idSubclass, DWORD_PTR refData)
{
    UNREFERENCED_PARAMETER(idSubclass);
    UNREFERENCED_PARAMETER(refData);

    switch (uMsg)
    {
    case WM_PAINT:
    case WM_PRINTCLIENT:
    case WM_ERASEBKGND:
    {
        if (!g_active.load(std::memory_order_relaxed))
        {
            break;
        }

        // The diagnostics cost a few messages per paint until both verdicts are in, then nothing.
        const bool diagnose = uMsg == WM_PAINT &&
            !(g_hitLogged.load(std::memory_order_relaxed) && g_missLogged.load(std::memory_order_relaxed));
        const bool expectSelection = diagnose && SelectionInUpdateRect(hWnd);

        if (!g_paintSeen.exchange(true))
        {
            SP_Log(L"The desktop list view %p is painting through the subclass", hWnd);
        }

        const int savedItems = t_listItemDraws;
        const int savedReplaced = t_replacedDraws;
        t_listItemDraws = 0;
        t_replacedDraws = 0;

        t_paintDepth++;
        LRESULT result = DefSubclassProc(hWnd, uMsg, wParam, lParam);
        t_paintDepth--;

        if (expectSelection && t_listItemDraws == 0 && !g_missLogged.exchange(true))
        {
            SP_Log(L"The desktop painted a selected icon but no LVP_LISTITEM draw reached DrawThemeBackground "
                   L"or DrawThemeBackgroundEx: this build draws the selection another way and the mod is inert");
        }

        t_listItemDraws = savedItems;
        t_replacedDraws = savedReplaced;
        return result;
    }

    case WM_THEMECHANGED:
    case WM_SETTINGCHANGE:
    case WM_DWMCOLORIZATIONCOLORCHANGED:
        // Rarely reach a child, but cost nothing to honour. The repaint matters: plates already on screen
        // would otherwise keep the old colour until they are next drawn.
        g_accentTick.store(0, std::memory_order_relaxed);
        g_dpi.store(ReadWindowDpi(hWnd), std::memory_order_relaxed);
        InvalidateRect(hWnd, nullptr, TRUE);
        break;

    case WM_DPICHANGED_BEFOREPARENT:
    case WM_DPICHANGED_AFTERPARENT:
        g_dpi.store(ReadWindowDpi(hWnd), std::memory_order_relaxed);
        break;

    case WM_NCDESTROY:
    {
        RemoveWindowSubclass(hWnd, DesktopListSubclass, kSubclassId);
        // Only forgotten if it is still the window being tracked: the shell can create and attach the
        // replacement before the old one is destroyed.
        HWND self = hWnd;
        if (g_lv.compare_exchange_strong(self, nullptr, std::memory_order_relaxed))
        {
            g_lvThread.store(0, std::memory_order_relaxed);
        }
        break;
    }
    }

    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

// Called from the engine thread and from the CreateWindowExW hook on the desktop's own thread; the engine's
// helper handles both. Exactly one list view stays subclassed: a displaced one is unsubclassed so it can never
// outlive the mod and call into freed code.
void Watch(HWND hList)
{
    if (!hList || !IsWindow(hList))
    {
        return;
    }

    EnsureGdiplus();

    DWORD_PTR existing = 0;
    if (!GetWindowSubclass(hList, DesktopListSubclass, kSubclassId, &existing))
    {
        if (!SP_SetWindowSubclassFromAnyThread(hList, DesktopListSubclass, kSubclassId, 0))
        {
            SP_LogError(L"The desktop list view %p could not be subclassed", hList);
            return;
        }
    }

    // g_paintSeen is deliberately not reset here: once the subclass has been seen to work on this build it
    // will work on the replacement too, and the thread-id fallback must not wake up while the displaced list
    // view is still painting its last frames.
    HWND previous = g_lv.exchange(hList, std::memory_order_relaxed);
    g_lvThread.store(GetWindowThreadProcessId(hList, nullptr), std::memory_order_relaxed);

    if (previous && previous != hList && IsWindow(previous))
    {
        SP_RemoveWindowSubclassFromAnyThread(previous, DesktopListSubclass, kSubclassId);
    }

    g_dpi.store(ReadWindowDpi(hList), std::memory_order_relaxed);
    g_accentTick.store(0, std::memory_order_relaxed);
    RepaintDesktop(hList);

    SP_Log(L"Watching the desktop list view %p on thread %lu", hList,
           g_lvThread.load(std::memory_order_relaxed));
}

void Unwatch()
{
    HWND hList = g_lv.exchange(nullptr, std::memory_order_relaxed);
    g_lvThread.store(0, std::memory_order_relaxed);
    if (hList && IsWindow(hList))
    {
        DWORD_PTR existing = 0;
        if (GetWindowSubclass(hList, DesktopListSubclass, kSubclassId, &existing))
        {
            SP_RemoveWindowSubclassFromAnyThread(hList, DesktopListSubclass, kSubclassId);
        }
        RepaintDesktop(hList);
    }
}

// The shell rebuilds the desktop on a theme change or when "Show desktop icons" is toggled; the new list view
// is caught as it is created.
using CreateWindowExW_t = decltype(&CreateWindowExW);
CreateWindowExW_t g_origCreateWindowExW = nullptr;

HWND WINAPI CreateWindowExW_Hook(DWORD exStyle, LPCWSTR className, LPCWSTR windowName, DWORD style,
                                 int x, int y, int width, int height, HWND hParent, HMENU hMenu,
                                 HINSTANCE hInstance, LPVOID param)
{
    HWND hWnd = g_origCreateWindowExW(exStyle, className, windowName, style, x, y, width, height,
                                      hParent, hMenu, hInstance, param);

    // Cheapest rejections first: the desktop list view always has a parent, and a class name that is a string
    // and not SysListView32 is out without walking anything. A class atom falls through to the full check.
    if (!hWnd || !hParent || !g_active.load(std::memory_order_relaxed))
    {
        return hWnd;
    }
    if (className && !IS_INTRESOURCE(className) && _wcsicmp(className, L"SysListView32") != 0)
    {
        return hWnd;
    }
    if (IsDesktopListView(hWnd))
    {
        Watch(hWnd);
    }
    return hWnd;
}

// ---------------------------------------------------------------------------------------------------------------
// Settings and lifecycle
// ---------------------------------------------------------------------------------------------------------------

void LoadSettings()
{
    Style s;
    s.cornerRadius = std::clamp(SP_GetIntSetting(L"CornerRadius", 8), 0, kMaxPixelSetting);
    s.opacity = std::clamp(SP_GetIntSetting(L"Opacity", 40), 0, 100);
    s.borderStyle = std::clamp(SP_GetIntSetting(L"BorderStyle", kBorderDashed), (int)kBorderNone, (int)kBorderDotted);
    s.padding = std::clamp(SP_GetIntSetting(L"Padding", 4), 0, kMaxPixelSetting);
    s.glow = SP_GetIntSetting(L"Glow", 1) != 0;

    wchar_t wszColor[64];
    SP_GetStringSetting(L"SelectionColor", wszColor, ARRAYSIZE(wszColor), L"");
    s.colorIsAccent = true;
    if (!ParseColor(wszColor, &s.color, &s.colorIsAccent))
    {
        SP_LogError(L"SelectionColor \"%s\" is not RRGGBB; following the accent colour instead", wszColor);
        s.colorIsAccent = true;
    }

    // Published in one assignment so a repaint never draws from a mix of old and new values.
    AcquireSRWLockExclusive(&g_styleLock);
    g_style = s;
    ReleaseSRWLockExclusive(&g_styleLock);

    SP_Log(L"CornerRadius=%d Opacity=%d BorderStyle=%d Padding=%d Glow=%d Color=%s",
           s.cornerRadius, s.opacity, s.borderStyle, s.padding, s.glow ? 1 : 0,
           s.colorIsAccent ? L"accent" : wszColor);
}

BOOL Init()
{
    LoadSettings();

    if (!SP_HookBegin())
    {
        return FALSE;
    }

    // uxtheme and user32 are always loaded in Explorer. DrawThemeBackground is what the desktop was measured
    // to use; the Ex entry is the function it wraps, hooked as well in case comctl32 reaches it directly.
    if (!SP_SetExportHook(L"uxtheme.dll", "DrawThemeBackground", DrawThemeBackground_Hook, &g_origDrawThemeBackground) ||
        !SP_SetExportHook(L"uxtheme.dll", "DrawThemeBackgroundEx", DrawThemeBackgroundEx_Hook, &g_origDrawThemeBackgroundEx) ||
        !SP_SetExportHook(L"user32.dll", "CreateWindowExW", CreateWindowExW_Hook, &g_origCreateWindowExW))
    {
        SP_HookAbort();
        SP_LogError(L"DrawThemeBackground / DrawThemeBackgroundEx / CreateWindowExW could not be hooked");
        return FALSE;
    }

    if (!SP_HookCommit())
    {
        return FALSE;
    }

    g_active.store(true, std::memory_order_relaxed);
    SP_Log(L"Hooked uxtheme DrawThemeBackground, DrawThemeBackgroundEx and user32 CreateWindowExW");
    return TRUE;
}

void AfterInit()
{
    // The CreateWindowExW hook may already have caught a newer list view; a search now would only displace it
    // with one on its way out.
    if (g_lv.load(std::memory_order_relaxed))
    {
        return;
    }

    HWND hList = FindDesktopListView();
    if (hList)
    {
        Watch(hList);
    }
    else
    {
        SP_Log(L"No desktop list view yet; waiting for the shell to create one");
    }
}

void SettingsChanged()
{
    LoadSettings();

    HWND hList = g_lv.load(std::memory_order_relaxed);
    if (!hList)
    {
        // A retry, so a lookup that missed at start-up is recoverable by touching a setting.
        hList = FindDesktopListView();
        if (hList)
        {
            Watch(hList);
            return;
        }
    }

    if (hList)
    {
        g_dpi.store(ReadWindowDpi(hList), std::memory_order_relaxed);
        g_accentTick.store(0, std::memory_order_relaxed);
        RepaintDesktop(hList);
    }
}

void BeforeUninit()
{
    // Still hooked, but every hook body reads g_active first and passes the call on from here.
    g_active.store(false, std::memory_order_relaxed);

    SP_Log(L"Standing down: %u selection plate(s) restyled, %u list item draw(s) seen outside a paint message, "
           L"subclass saw paint: %s",
           g_replacedCount.load(std::memory_order_relaxed),
           g_outsidePaintCount.load(std::memory_order_relaxed),
           g_paintSeen.load(std::memory_order_relaxed) ? L"yes" : L"no");

    // Synchronous, so the subclass proc is off the window's chain before the mod's code goes away.
    Unwatch();
}

void Uninit()
{
    ShutdownGdiplus();
}

}   // namespace

SP_MOD_DEFINE(g_modDesktopIconSelectionStyle) =
{
    /* id             */ SP_MOD_ID,
    /* name           */ L"Restyle the highlight behind selected desktop icons",
    /* basedOn        */ "desktop-icon-selection-style",
    /* originalAuthor */ "RiteshK",
    /* targets        */ SP_TARGET_EXPLORER,
    /* minOsBuild     */ 22000,
    /* flags          */ 0,
    /* Init           */ Init,
    /* AfterInit      */ AfterInit,
    /* SettingsChanged*/ SettingsChanged,
    /* BeforeUninit   */ BeforeUninit,
    /* Uninit         */ Uninit,
};
