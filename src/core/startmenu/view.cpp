//
// view.cpp - see view.h. The section numbers in comments refer to design/startmenu/README.md.
//
#include "view.h"

#include <d2d1_1helper.h>
#include <d2d1effects.h>
#include <dxgi.h>
#include <ShellScalingApi.h>
#include <CommCtrl.h>
#include <Shlwapi.h>
#include <dwmapi.h>
#include <imm.h>
#include <windowsx.h>

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "engine/log.h"
#include "host.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace sm {

namespace {

constexpr char kTag[] = "custom-start-menu";
constexpr wchar_t kInputClass[] = L"ShadePatcher.StartMenu";
constexpr wchar_t kVisualClass[] = L"ShadePatcher.StartMenu.Visual";

enum : UINT_PTR
{
    kTimerHide = 1,      // the close animation has finished
    kTimerCaret,         // caret blink
    kTimerField,         // search border colour transition, ~60 Hz for 120 ms
    kTimerPress,         // a released cell has finished growing back
};

constexpr float B = metric::PanelBorder;
constexpr size_t kMaxQuery = 200;
constexpr float kShortcutSize = 36, kShortcutGap = 2, kShortcutIcon = 20;   // footer shortcut buttons (DIPs)
constexpr float kLetterTile = 52;                                           // a tile of the A-Z index (DIPs)   // characters; typing and pasting both stop here

// Flyout surface margins: enough for its shadow (0 12 32, std 16, ~3 std of blur).
constexpr float kFlyoutMarginX = 48, kFlyoutMarginTop = 36, kFlyoutMarginBottom = 64;

// ---------------------------------------------------------------------------------------------------------------
// Bezier curves (section 8.1)
// ---------------------------------------------------------------------------------------------------------------

double Bez(double p1, double p2, double u)
{
    double v = 1 - u;
    return 3 * v * v * u * p1 + 3 * v * u * u * p2 + u * u * u;
}

double BezSlope(double p1, double p2, double u)
{
    double v = 1 - u;
    return 3 * v * v * p1 + 6 * v * u * (p2 - p1) + 3 * u * u * (1 - p2);
}

// y of a CSS cubic-bezier at progress x (0..1).
double BezierY(const motion::Bezier& b, double x)
{
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    double u = x;
    for (int i = 0; i < 8; ++i)
    {
        double d = BezSlope(b.x1, b.x2, u);
        if (fabs(d) < 1e-6)
        {
            break;
        }
        u -= (Bez(b.x1, b.x2, u) - x) / d;
        u = std::clamp(u, 0.0, 1.0);
    }
    // A few bisection steps settle the cases Newton's method does not (flat ends).
    double lo = 0, hi = 1;
    for (int i = 0; i < 20 && fabs(Bez(b.x1, b.x2, u) - x) > 1e-5; ++i)
    {
        u = (lo + hi) / 2;
        (Bez(b.x1, b.x2, u) < x ? lo : hi) = u;
    }
    return Bez(b.y1, b.y2, u);
}

// Adds a cubic-bezier curve to a DirectComposition animation as eight Hermite segments. The slope at each knot is
// taken numerically, which also handles curves that start flat in x (ease-out's first control point is 0,0).
void AddBezier(IDCompositionAnimation* anim, float from, float to, double duration, const motion::Bezier& curve)
{
    constexpr int N = 8;
    auto value = [&](double t) { return from + (to - from) * BezierY(curve, t); };
    auto slope = [&](double t) {   // d value / d second
        constexpr double e = 1e-4;
        double a = std::max(0.0, t - e), b = std::min(1.0, t + e);
        return (value(b) - value(a)) / (b - a) / duration;
    };
    double h = duration / N;
    for (int i = 0; i < N; ++i)
    {
        double t0 = double(i) / N, t1 = double(i + 1) / N;
        double p0 = value(t0), p1 = value(t1), m0 = slope(t0) * h, m1 = slope(t1) * h;
        double c = 3 * (p1 - p0) - 2 * m0 - m1, d = 2 * (p0 - p1) + m0 + m1;
        anim->AddCubic(i * h, (float)p0, (float)(m0 / h), (float)(c / (h * h)), (float)(d / (h * h * h)));
    }
    anim->End(duration, to);
}

D2D1_COLOR_F Lerp(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t)
{
    return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t };
}

bool Contains(const D2D1_RECT_F& r, float x, float y)
{
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

D2D1_RECT_F Inset(const D2D1_RECT_F& r, float d)
{
    return { r.left + d, r.top + d, r.right - d, r.bottom - d };
}

ComPtr<IDCompositionVisual3> V3(const ComPtr<IDCompositionVisual2>& visual)
{
    ComPtr<IDCompositionVisual3> v3;
    if (visual)
    {
        visual.As(&v3);
    }
    return v3;
}

void SetVisible(const ComPtr<IDCompositionVisual2>& visual, bool visible)
{
    if (auto v3 = V3(visual))
    {
        v3->SetVisible(visible);
    }
}

double Seconds(const LARGE_INTEGER& since)
{
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return double(now.QuadPart - since.QuadPart) / frequency.QuadPart;
}

bool IsTaskbarAlignedLeft()
{
    DWORD value = 1, size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                 L"TaskbarAl", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value == 0;
}

HWND TaskbarOnMonitor(HMONITOR monitor)
{
    HWND primary = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (primary && MonitorFromWindow(primary, MONITOR_DEFAULTTONULL) == monitor)
    {
        return primary;
    }
    for (HWND secondary = FindWindowExW(nullptr, nullptr, L"Shell_SecondaryTrayWnd", nullptr); secondary;
         secondary = FindWindowExW(nullptr, secondary, L"Shell_SecondaryTrayWnd", nullptr))
    {
        if (MonitorFromWindow(secondary, MONITOR_DEFAULTTONULL) == monitor)
        {
            return secondary;
        }
    }
    return nullptr;
}

// SetForegroundWindow, and when the foreground lock refuses it, the same with this thread's input attached to the
// foreground thread's for the moment of the call (the shell is allowed to do this; the Start menu must be able to
// take the keyboard however it was opened).
bool BringToForeground(HWND hwnd)
{
    if (SetForegroundWindow(hwnd) && GetForegroundWindow() == hwnd)
    {
        return true;
    }
    HWND foreground = GetForegroundWindow();
    DWORD foregroundThread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    DWORD self = GetCurrentThreadId();
    bool attached = foregroundThread && foregroundThread != self && AttachThreadInput(self, foregroundThread, TRUE);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    if (attached)
    {
        AttachThreadInput(self, foregroundThread, FALSE);
    }
    return GetForegroundWindow() == hwnd;
}

// The window to give the focus back to when the menu closes without launching anything: the one that had it,
// unless that was the taskbar (the click on Start activated it), in which case the topmost ordinary window.
HWND WindowToRestore(HWND previous, HWND input, HWND visual)
{
    auto isShell = [](HWND hwnd) {
        wchar_t name[64] = L"";
        GetClassNameW(hwnd, name, ARRAYSIZE(name));
        return wcscmp(name, L"Shell_TrayWnd") == 0 || wcscmp(name, L"Shell_SecondaryTrayWnd") == 0 ||
               wcscmp(name, L"Progman") == 0 || wcscmp(name, L"WorkerW") == 0;
    };
    if (previous && IsWindow(previous) && IsWindowVisible(previous) && !isShell(previous))
    {
        return previous;
    }
    for (HWND hwnd = GetTopWindow(nullptr); hwnd; hwnd = GetWindow(hwnd, GW_HWNDNEXT))
    {
        if (hwnd == input || hwnd == visual || !IsWindowVisible(hwnd) || IsIconic(hwnd) || isShell(hwnd))
        {
            continue;
        }
        LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        RECT rect;
        if ((ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) || !GetWindowRect(hwnd, &rect) || IsRectEmpty(&rect))
        {
            continue;
        }
        BOOL cloaked = FALSE;
        if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
        {
            continue;
        }
        return hwnd;
    }
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
// Drawing scope: BeginDraw on a surface (or a part of a virtual surface), a DIP coordinate system whose origin is
// the surface's (0, 0), and a clip to the updated pixels.
// ---------------------------------------------------------------------------------------------------------------

struct MenuView::Draw
{
    MenuView* view;
    IDCompositionSurface* surface;
    ComPtr<ID2D1DeviceContext> dc;
    HRESULT hr = E_FAIL;

    Draw(MenuView* v, IDCompositionSurface* s, const RECT* update = nullptr) : view(v), surface(s)
    {
        if (!surface)
        {
            return;
        }
        POINT offset = {};
        hr = surface->BeginDraw(update, IID_PPV_ARGS(&dc), &offset);
        if (FAILED(hr))
        {
            dc.Reset();
            view->DeviceLost(hr);
            return;
        }
        float sc = view->m_scale;
        float left = update ? (float)update->left : 0, top = update ? (float)update->top : 0;
        dc->SetDpi((float)view->m_dpi, (float)view->m_dpi);
        dc->SetTransform(D2D1::Matrix3x2F::Translation((offset.x - left) / sc, (offset.y - top) / sc));
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        if (update)
        {
            dc->PushAxisAlignedClip(D2D1::RectF(update->left / sc, update->top / sc, update->right / sc,
                                                update->bottom / sc),
                                    D2D1_ANTIALIAS_MODE_ALIASED);
        }
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        clipped = update != nullptr;
    }

    ~Draw()
    {
        if (!dc)
        {
            return;
        }
        if (clipped)
        {
            dc->PopAxisAlignedClip();
        }
        dc.Reset();
        HRESULT end = surface->EndDraw();
        if (FAILED(end))
        {
            view->DeviceLost(end);
        }
    }

    explicit operator bool() const { return dc != nullptr; }
    ID2D1DeviceContext* operator->() const { return dc.Get(); }

private:
    bool clipped = false;
};

// ---------------------------------------------------------------------------------------------------------------
// Creation
// ---------------------------------------------------------------------------------------------------------------

MenuView::MenuView() = default;

MenuView::~MenuView()
{
    Destroy();
}

bool MenuView::RegisterClasses()
{
    HINSTANCE instance = reinterpret_cast<HINSTANCE>(&__ImageBase);
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = InputProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kInputClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return false;
    }
    WNDCLASSEXW visual = { sizeof(visual) };
    visual.lpfnWndProc = DefWindowProcW;
    visual.hInstance = instance;
    visual.lpszClassName = kVisualClass;
    return RegisterClassExW(&visual) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool MenuView::Create(const ViewOptions& options)
{
    m_options = options;
    if (!RegisterClasses())
    {
        SP_LOG_ERR(kTag, L"Window classes could not be registered (%lu)", GetLastError());
        return false;
    }
    HINSTANCE instance = reinterpret_cast<HINSTANCE>(&__ImageBase);

    // The visual window: click-through (layered + transparent), never activated.
    m_visual = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW |
                                   WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                               kVisualClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
    // The input window: no content, so it is invisible, but it is hit-tested and takes the focus.
    m_input = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kInputClass, L"Start",
                              WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, this);
    if (!m_visual || !m_input)
    {
        SP_LOG_ERR(kTag, L"Menu windows could not be created (%lu)", GetLastError());
        Destroy();
        return false;
    }
    SetLayeredWindowAttributes(m_visual, 0, 255, LWA_ALPHA);

    ApplyStyle();
    CreateFormats();
    if (!m_fmtTitle)
    {
        SP_LOG_ERR(kTag, L"DirectWrite is not available");
        Destroy();
        return false;
    }

    if (!CreateDevice())
    {
        Destroy();
        return false;
    }

    m_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_WIN95_CLASSES };
    InitCommonControlsEx(&controls);
    m_tooltip = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, TOOLTIPS_CLASSW, nullptr,
                                WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0, m_input, nullptr,
                                reinterpret_cast<HINSTANCE>(&__ImageBase), nullptr);
    m_usage.Load();
    m_files.Start(m_input, kMsgLoaded, (WPARAM)LoadResult::Files);
    m_quick.Start(m_input, kMsgLoaded, (WPARAM)LoadResult::Quick);
    m_pinsLoaded = false;
    if (!m_loader.Start(m_input, kMsgLoaded))
    {
        SP_LOG_ERR(kTag, L"The loader thread could not start");
    }
    
    // Load for the primary monitor's DPI now, so the first opening has icons.
    HMONITOR primary = MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    UINT dpiX = 96, dpiY = 96;
    GetDpiForMonitor(primary, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    m_dpi = (UINT)lroundf(dpiX * m_options.scale / 100.f);
    m_scale = m_dpi / 96.f;
    m_loader.RequestUser();
    RequestData(true);
    return true;
}

void MenuView::Destroy()
{
    m_loader.Stop();
    m_files.Stop();
    m_quick.Stop();
    if (m_input)
    {
        MSG msg;
        while (PeekMessageW(&msg, m_input, kMsgLoaded, kMsgLoaded, PM_REMOVE))
        {
            FreeLoadResult(msg.wParam, msg.lParam);
        }
    }
    if (m_input)
    {
        KillTimer(m_input, kTimerHide);
        KillTimer(m_input, kTimerCaret);
        KillTimer(m_input, kTimerField);
        KillTimer(m_input, kTimerPress);
    }
    ReleaseDevice();
    m_fmtTitle.Reset(); m_fmtSearch.Reset(); m_fmtToggle.Reset(); m_fmtGridLabel.Reset(); m_fmtListItem.Reset();
    m_fmtLetter.Reset(); m_fmtChip.Reset(); m_fmtUser.Reset(); m_fmtInitial.Reset(); m_fmtButton.Reset();
    m_fmtClear.Reset(); m_fmtEmpty.Reset();
    m_fonts.Shutdown();
    if (m_input)
    {
        SetWindowLongPtrW(m_input, GWLP_USERDATA, 0);
        DestroyWindow(m_input);
        m_input = nullptr;
    }
    if (m_visual)
    {
        DestroyWindow(m_visual);
        m_visual = nullptr;
    }
    m_open = m_visible = false;
    m_apps.reset();
}

void MenuView::CreateFormats()
{
    int fontKey = m_options.font * 1000 + m_options.fontScale;
    if (m_fontBuilt == fontKey && m_fmtTitle)
    {
        return;
    }
    m_fonts.Shutdown();
    if (!m_fonts.Initialize(m_options.font))
    {
        return;
    }
    m_fonts.SetSizeScale(std::clamp(m_options.fontScale, 80, 130) / 100.f);
    m_fontBuilt = fontKey;
    SP_LOG_INF(kTag, L"Text uses %s", m_fonts.UsingFigtree() ? L"Figtree" : L"the system font");

    auto format = [&](const type::Style& style, DWRITE_TEXT_ALIGNMENT align, DWRITE_PARAGRAPH_ALIGNMENT paragraph,
                      bool ellipsis) { return m_fonts.CreateFormat({ style, align, paragraph, ellipsis }); };
    m_fmtTitle = format(type::Title, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
    m_fmtSearch = format(type::Search, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    m_fmtToggle = format(type::Button, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    m_fmtGridLabel = format(type::GridLabel, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
    m_fmtListItem = format(type::ListItem, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
    m_fmtLetter = format(type::Letter, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_FAR, false);
    m_fmtChip = format(type::Chip, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
    m_fmtUser = format(type::UserName, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
    m_fmtInitial = format(type::Initial, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    m_fmtButton = format(type::Button, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
    m_fmtClear = format(type::Clear, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    m_fmtEmpty = format(type::Empty, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true);

    // Icon glyphs come from the system's symbol font: Segoe Fluent Icons on Windows 11, MDL2 Assets before.
    m_fmtIcon.Reset();
    ComPtr<IDWriteFontCollection> system;
    const wchar_t* iconFamily = L"Segoe MDL2 Assets";
    if (m_fonts.Factory() && SUCCEEDED(m_fonts.Factory()->GetSystemFontCollection(&system, FALSE)))
    {
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (SUCCEEDED(system->FindFamilyName(L"Segoe Fluent Icons", &index, &exists)) && exists)
        {
            iconFamily = L"Segoe Fluent Icons";
        }
    }
    if (m_fonts.Factory() &&
        SUCCEEDED(m_fonts.Factory()->CreateTextFormat(iconFamily, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.f, L"",
                                                      &m_fmtIcon)))
    {
        m_fmtIcon->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        m_fmtIcon->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
}

bool MenuView::CreateDevice()
{
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                         D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3 };
    // WARP, the software rasterizer, on purpose. The menu draws rarely and little (a surface when its state
    // changes); the animations run in DWM on the GPU whatever device drew the surfaces. A hardware device costs
    // 20-35 MB of private memory in the shell (measured on an NVIDIA driver), WARP about 1.5 MB, and a GPU reset
    // cannot take it away. The hardware device is only the fallback.
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, ARRAYSIZE(levels),
                                   D3D11_SDK_VERSION, &m_d3d, nullptr, nullptr);
    if (FAILED(hr))
    {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, ARRAYSIZE(levels),
                               D3D11_SDK_VERSION, &m_d3d, nullptr, nullptr);
    }
    ComPtr<IDXGIDevice> dxgi;
    if (FAILED(hr) || FAILED(m_d3d.As(&dxgi)))
    {
        SP_LOG_ERR(kTag, L"Direct3D 11 device could not be created (0x%08X)", hr);
        return false;
    }
    if (!m_d2dFactory)
    {
        D2D1_FACTORY_OPTIONS options = {};
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
                               reinterpret_cast<void**>(m_d2dFactory.GetAddressOf()));
        if (FAILED(hr))
        {
            SP_LOG_ERR(kTag, L"Direct2D factory could not be created (0x%08X)", hr);
            return false;
        }
        D2D1_STROKE_STYLE_PROPERTIES round = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                         D2D1_CAP_STYLE_ROUND);
        m_d2dFactory->CreateStrokeStyle(round, nullptr, 0, &m_roundStroke);
    }
    if (FAILED(hr = m_d2dFactory->CreateDevice(dxgi.Get(), &m_d2d)) ||
        FAILED(hr = m_d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_resources)) ||
        FAILED(hr = DCompositionCreateDevice3(m_d2d.Get(), IID_PPV_ARGS(&m_dcomp))) ||
        FAILED(hr = m_dcomp->CreateTargetForHwnd(m_visual, TRUE, &m_target)))
    {
        SP_LOG_ERR(kTag, L"Composition device could not be created (0x%08X)", hr);
        ReleaseDevice();
        return false;
    }
    if (!BuildVisualTree())
    {
        ReleaseDevice();
        return false;
    }
    m_builtDpi = 0;
    return true;
}

void MenuView::ReleaseDevice()
{
    m_bitmaps.clear();
    m_brushes.clear();
    m_shadowSurface.Reset(); m_backgroundSurface.Reset(); m_chromeSurface.Reset(); m_hlGrid.Reset();
    m_hlList.Reset(); m_hlLetter.Reset(); m_pressedSurface.Reset(); m_caretSurface.Reset(); m_flyoutSurface.Reset();
    m_contentSurface.Reset();
    m_openScale.Reset(); m_pressScale.Reset(); m_openTranslate.Reset(); m_openGroup.Reset();
    m_containerEffect.Reset(); m_panelClip.Reset(); m_scrollClip.Reset();
    m_root.Reset(); m_container.Reset(); m_shadow.Reset(); m_panel.Reset(); m_background.Reset();
    m_scrollHost.Reset(); m_scrollLayer.Reset(); m_highlight.Reset(); m_content.Reset(); m_pressed.Reset();
    m_chrome.Reset(); m_caret.Reset(); m_flyout.Reset();
    m_target.Reset();
    m_dcomp.Reset();
    m_resources.Reset();
    m_d2d.Reset();
    m_d3d.Reset();
    m_bandValid = false;
    m_builtDpi = 0;
}

bool MenuView::DeviceLost(HRESULT hr)
{
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == D2DERR_RECREATE_TARGET ||
        hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR)
    {
        if (!m_deviceLost)
        {
            SP_LOG_ERR(kTag, L"Graphics device lost (0x%08X); rebuilding", hr);
            m_deviceLost = true;
            PostMessageW(m_input, WM_NULL, 0, 0);   // RecoverDevice runs after the current message
        }
        return true;
    }
    return false;
}

void MenuView::RecoverDevice()
{
    m_deviceLost = false;
    ReleaseDevice();
    if (!CreateDevice())
    {
        // Nothing can be drawn: take the menu down so no input path reaches the released visuals.
        m_open = m_visible = false;
        KillTimer(m_input, kTimerCaret);
        KillTimer(m_input, kTimerField);
        KillTimer(m_input, kTimerPress);
        KillTimer(m_input, kTimerHide);
        ShowWindow(m_input, SW_HIDE);
        ShowWindow(m_visual, SW_HIDE);
        return;
    }
    if (m_visible)
    {
        BuildSurfaces();
        RenderChrome();
        RenderContent(true);
        UpdateHighlight(false);
        UpdateCaret();
        if (m_powerOpen)
        {
            RenderFlyout();
        }
        m_containerEffect->SetOpacity(m_open ? 1.f : 0.f);
        Commit();
    }
}

bool MenuView::BuildVisualTree()
{
    IDCompositionDesktopDevice* d = m_dcomp.Get();
    ComPtr<IDCompositionVisual2>* visuals[] = { &m_root, &m_container, &m_shadow, &m_panel, &m_background,
                                                &m_scrollHost, &m_scrollLayer, &m_highlight, &m_content,
                                                &m_pressed, &m_chrome, &m_caret, &m_flyout };
    for (auto* visual : visuals)
    {
        if (FAILED(d->CreateVisual(visual->GetAddressOf())))
        {
            return false;
        }
    }
    if (FAILED(d->CreateEffectGroup(&m_containerEffect)) || FAILED(d->CreateScaleTransform(&m_openScale)) ||
        FAILED(d->CreateTranslateTransform(&m_openTranslate)) || FAILED(d->CreateScaleTransform(&m_pressScale)) ||
        FAILED(d->CreateRectangleClip(&m_panelClip)) || FAILED(d->CreateRectangleClip(&m_scrollClip)))
    {
        return false;
    }
    IDCompositionTransform* parts[] = { m_openScale.Get(), m_openTranslate.Get() };   // scale first, then move
    if (FAILED(d->CreateTransformGroup(parts, 2, &m_openGroup)))
    {
        return false;
    }

    // Children in back-to-front order: each one is added in front of the one before it.
    auto children = [](IDCompositionVisual2* parent, std::initializer_list<IDCompositionVisual2*> list) {
        IDCompositionVisual2* previous = nullptr;
        for (IDCompositionVisual2* child : list)
        {
            parent->AddVisual(child, previous != nullptr, previous);
            previous = child;
        }
    };
    children(m_root.Get(), { m_container.Get() });
    children(m_container.Get(), { m_shadow.Get(), m_panel.Get(), m_flyout.Get() });
    children(m_panel.Get(), { m_background.Get(), m_scrollHost.Get(), m_chrome.Get(), m_caret.Get() });
    children(m_scrollHost.Get(), { m_scrollLayer.Get() });
    children(m_scrollLayer.Get(), { m_highlight.Get(), m_content.Get(), m_pressed.Get() });

    m_container->SetEffect(m_containerEffect.Get());
    m_container->SetTransform(m_openGroup.Get());
    m_panel->SetClip(m_panelClip.Get());
    m_scrollHost->SetClip(m_scrollClip.Get());
    m_pressed->SetTransform(m_pressScale.Get());
    m_containerEffect->SetOpacity(0.f);
    SetVisible(m_pressed, false);
    SetVisible(m_flyout, false);
    SetVisible(m_highlight, false);
    SetVisible(m_caret, false);
    return SUCCEEDED(m_target->SetRoot(m_root.Get()));
}

// ---------------------------------------------------------------------------------------------------------------
// Style: the options turned into a palette and a layout
// ---------------------------------------------------------------------------------------------------------------

static bool SystemUsesDarkTheme()
{
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light == 0;
}

// The Windows accent colour (Settings > Personalisation > Colours), 0xRRGGBB.
static unsigned WindowsAccent()
{
    DWORD abgr = 0, size = sizeof(abgr);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD,
                     nullptr, &abgr, &size) != ERROR_SUCCESS)
    {
        return 0x0078D4;
    }
    return ((abgr & 0xFF) << 16) | (abgr & 0xFF00) | ((abgr >> 16) & 0xFF);
}

size_t MenuView::ApplyStyle()
{
    const ViewOptions& o = m_options;

    bool dark = o.theme == ThemeMode::Dark || (o.theme == ThemeMode::System && SystemUsesDarkTheme());
    switch (o.theme)
    {
    case ThemeMode::Midnight: m_pal = MidnightPalette(); break;
    case ThemeMode::Graphite: m_pal = GraphitePalette(); break;
    case ThemeMode::Sand:     m_pal = SandPalette(); break;
    case ThemeMode::Scheduled:
    {
        SYSTEMTIME now;
        GetLocalTime(&now);
        int h = now.wHour, darkFrom = std::clamp(o.darkFrom, 0, 23), lightFrom = std::clamp(o.lightFrom, 0, 23);
        bool night = darkFrom > lightFrom ? (h >= darkFrom || h < lightFrom) : (h >= darkFrom && h < lightFrom);
        m_pal = night ? DarkPalette() : LightPalette();
        break;
    }
    default:                  m_pal = dark ? DarkPalette() : LightPalette(); break;
    }
    dark = m_pal.dark;
    int accentIndex = std::clamp(o.accent, 0, accent::Count - 1);
    if (accentIndex != 0)   // 0 keeps the theme's own accent
    {
        unsigned rgb = accentIndex == 1 ? WindowsAccent() : accent::Presets[accentIndex];
        m_pal.Accent = Hex(rgb);
        m_pal.Selection = Hex(rgb, dark ? 0.30f : 0.22f);
    }
    m_acrylic = o.background == Background::Acrylic && TransparencyEnabled();
    m_translucent = m_acrylic || o.background == Background::Glass;
    // A picture replaces the other backgrounds. Decoded once per path, at most 1600 pixels on its longer side.
    if (o.backgroundImage != m_imagePath)
    {
        m_imagePath = o.backgroundImage;
        m_image = LoadImagePixels(m_imagePath, 1600);
        if (!m_imagePath.empty() && !m_image)
        {
            SP_LOG_ERR(kTag, L"The background picture could not be read: %s", m_imagePath.c_str());
        }
    }
    if (m_image)
    {
        m_acrylic = m_translucent = false;
    }

    // Layout. The handoff's cell (107.6 x 96 around a 44 DIP icon) grows with the icon; without labels the cell
    // hugs the icon. The panel is as wide as its columns, but never narrower than the header and footer need.
    Layout& l = m_lay;
    l.GridCols = std::clamp(o.columns, 3, 8);
    l.Icon = std::clamp(o.iconSize, 24.f, 72.f);
    l.labels = o.labels;
    l.IconTop = metric::IconTop;
    if (l.labels)
    {
        l.CellW = std::max(107.6f, l.Icon + 48);
        l.CellH = l.Icon + (metric::CellH - metric::Icon);
    }
    else
    {
        l.CellW = l.Icon + 40;
        l.CellH = l.Icon + 32;
        l.IconTop = 16;
    }
    float chrome = 2 * B + 2 * metric::ContentPadX;
    float gridW = l.GridCols * l.CellW + (l.GridCols - 1) * metric::GridGap;
    const float minW = 420;
    if (chrome + gridW < minW)
    {
        l.CellW = (minW - chrome - (l.GridCols - 1) * metric::GridGap) / l.GridCols;
        gridW = l.GridCols * l.CellW + (l.GridCols - 1) * metric::GridGap;
    }
    l.PanelW = chrome + gridW;
    l.SearchW = l.PanelW - 2 * B - 2 * metric::SearchX;
    l.CellRadius = metric::CellRadius;
    l.IconRadius = l.Icon * metric::IconRadius / metric::Icon;
    l.LabelMaxW = l.CellW - (107.6f - metric::LabelMaxW);
    l.ListIcon = std::clamp(o.listIconSize, 16.f, 48.f);
    l.ListIconRadius = l.ListIcon * metric::ListIconRadius / metric::ListIcon;
    l.ListRowH = std::max(metric::ListRowH, l.ListIcon + 16);
    l.PanelRadius = std::clamp(o.cornerRadius, 0.f, 32.f);
    l.PanelMaxH = std::clamp(o.maxHeight, 400.f, 1200.f);
    l.EdgeGap = std::clamp(o.edgeGap, 0.f, 64.f);

    m_powerItems.clear();
    bool updates = o.powerUpdates && UpdatesPending();
    for (int i = 0; i < (int)PowerAction::Count; ++i)
    {
        PowerAction action = (PowerAction)i;
        bool wanted = action == PowerAction::UpdateRestart || action == PowerAction::UpdateShutDown
                          ? updates
                          : (o.powerItems & PowerBit(action)) != 0;
        if (wanted)
        {
            m_powerItems.push_back(action);
        }
    }
    if (m_powerItems.empty())
    {
        m_powerItems.push_back(PowerAction::ShutDown);
    }

    // A fingerprint of everything the surfaces are drawn from, to know when they must be rebuilt (FNV-1a).
    unsigned long long hash = 14695981039346656037ull;
    auto mix = [&hash](unsigned long long value) { hash = (hash ^ value) * 1099511628211ull; };
    mix(dark);
    mix(accentIndex == 1 ? WindowsAccent() : accentIndex);
    mix(m_acrylic);
    mix(m_translucent);
    mix(std::hash<std::wstring>()(m_image ? m_imagePath : std::wstring()));
    mix((unsigned)o.imageBlur);
    mix((unsigned)o.imageTint);
    mix((unsigned)o.theme);
    mix(o.showFooter);
    mix((unsigned)o.opacity);
    mix(o.shadow);
    mix((unsigned)l.GridCols);
    mix((unsigned)(l.Icon * 16));
    mix((unsigned)(l.ListIcon * 16));
    mix(l.labels);
    mix((unsigned)(l.PanelRadius * 16));
    mix((unsigned)(l.PanelMaxH * 16));
    mix(o.powerItems);
    return (size_t)(hash | 1);
}

// ---------------------------------------------------------------------------------------------------------------
// Geometry (section 5). Panel coordinates are DIPs from the panel's outer top-left corner; the handoff's numbers
// are measured inside the 1 DIP border, hence the B offsets.
// ---------------------------------------------------------------------------------------------------------------

float MenuView::Snap(float dip) const
{
    return roundf(dip * m_scale) / m_scale;
}

float MenuView::ContentTop() const
{
    return B + metric::ContentY - TopShift();
}

bool MenuView::RecentVisible() const
{
    return m_options.showRecent && m_query.empty() && !m_showAll && !m_recent.empty();
}

float MenuView::ContentBottom() const
{
    return m_panelH - B - FooterH() - (RecentVisible() ? metric::RecentH : 0) - QuickH();
}

D2D1_RECT_F MenuView::SearchRect() const
{
    float x = B + metric::SearchX, y = B + metric::SearchY;
    return { x, y, x + m_lay.SearchW, y + metric::SearchH };
}

D2D1_RECT_F MenuView::ClearRect() const
{
    D2D1_RECT_F box = SearchRect();
    float right = box.right - metric::SearchIconInset, cy = (box.top + box.bottom) / 2;
    return { right - metric::ClearBtn, cy - metric::ClearBtn / 2, right, cy + metric::ClearBtn / 2 };
}

D2D1_RECT_F MenuView::ToggleRect() const
{
    float right = m_lay.PanelW - B - metric::HeaderPadX;
    float top = B + HeaderY() + metric::HeaderPadT;
    return { right - m_toggleW, top, right, top + metric::ToggleH };
}

D2D1_RECT_F MenuView::AccountRect() const
{
    float footerTop = m_panelH - B - FooterH();
    float cy = footerTop + 1 + (FooterH() - 1) / 2;   // below the 1 DIP hairline
    float left = B + metric::FooterPadL;
    float width = metric::AccountPadL + metric::Avatar + metric::AvatarGap + m_nameW + metric::AccountPadR;
    return { left, cy - metric::AccountH / 2, left + width, cy + metric::AccountH / 2 };
}

D2D1_RECT_F MenuView::PowerRect() const
{
    float footerTop = m_panelH - B - FooterH();
    float cy = footerTop + 1 + (FooterH() - 1) / 2;
    float right = m_lay.PanelW - B - metric::FooterPadR;
    return { right - metric::PowerBtn, cy - metric::PowerBtn / 2, right, cy + metric::PowerBtn / 2 };
}

D2D1_RECT_F MenuView::ShortcutRect(int i) const
{
    float footerTop = m_panelH - B - FooterH();
    float cy = footerTop + 1 + (FooterH() - 1) / 2;
    float right = PowerShown() ? PowerRect().left - 6 : m_lay.PanelW - B - metric::FooterPadR;
    int n = (int)m_shortcuts.size();
    float left = right - n * kShortcutSize - (n - 1) * kShortcutGap + i * (kShortcutSize + kShortcutGap);
    return { left, cy - kShortcutSize / 2, left + kShortcutSize, cy + kShortcutSize / 2 };
}

void MenuView::UpdateTooltips()
{
    if (!m_tooltip)
    {
        return;
    }
    // One tool per shortcut button; the rectangles follow the layout.
    int wanted = ShortcutsShown() ? (int)m_shortcuts.size() : 0;
    for (int i = 0; i < std::max(wanted, m_tooltipCount); ++i)
    {
        TOOLINFOW tool = { sizeof(tool) };
        tool.hwnd = m_input;
        tool.uId = i + 1;
        if (i >= wanted)
        {
            SendMessageW(m_tooltip, TTM_DELTOOLW, 0, (LPARAM)&tool);
            continue;
        }
        D2D1_RECT_F r = ShortcutRect(i);
        tool.uFlags = TTF_SUBCLASS;
        tool.rect = { (LONG)(r.left * m_scale), (LONG)(r.top * m_scale), (LONG)(r.right * m_scale),
                      (LONG)(r.bottom * m_scale) };
        tool.lpszText = const_cast<wchar_t*>(m_shortcuts[i].name.c_str());
        if (i >= m_tooltipCount)
        {
            SendMessageW(m_tooltip, TTM_ADDTOOLW, 0, (LPARAM)&tool);
        }
        else
        {
            SendMessageW(m_tooltip, TTM_NEWTOOLRECTW, 0, (LPARAM)&tool);
            SendMessageW(m_tooltip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&tool);
        }
    }
    m_tooltipCount = wanted;
}

D2D1_RECT_F MenuView::FlyoutRect() const
{
    D2D1_RECT_F power = PowerRect();
    float height = 2 * (metric::FlyoutPad + 1) + (float)m_powerItems.size() * metric::FlyoutItemH;
    float bottom = power.top - metric::FlyoutGapAbove;
    return { power.right - metric::FlyoutW, bottom - height, power.right, bottom };
}

D2D1_RECT_F MenuView::FlyoutItemRect(int i) const
{
    D2D1_RECT_F box = FlyoutRect();
    float top = box.top + 1 + metric::FlyoutPad + i * metric::FlyoutItemH;
    return { box.left + 1 + metric::FlyoutPad, top, box.right - 1 - metric::FlyoutPad, top + metric::FlyoutItemH };
}

bool MenuView::PlaceOnMonitor(HMONITOR monitor)
{
    if (!monitor)
    {
        HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
        monitor = taskbar ? MonitorFromWindow(taskbar, MONITOR_DEFAULTTOPRIMARY)
                          : MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    }
    MONITORINFO info = { sizeof(info) };
    if (!GetMonitorInfoW(monitor, &info))
    {
        return false;
    }
    UINT dpiX = 96, dpiY = 96;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
    {
        dpiX = 96;
    }
    m_monitor = monitor;
    m_dpi = (UINT)lroundf(dpiX * m_options.scale / 100.f);
    m_scale = m_dpi / 96.f;

    // Anchor to the taskbar on this monitor: above it when it is at the bottom, below it when it is at the top.
    RECT work = info.rcWork;
    LONG anchorTop = work.top, anchorBottom = work.bottom;
    m_fromTop = false;
    if (HWND taskbar = TaskbarOnMonitor(monitor))
    {
        RECT bar;
        if (GetWindowRect(taskbar, &bar) && (bar.right - bar.left) > (bar.bottom - bar.top))
        {
            LONG monitorMid = (info.rcMonitor.top + info.rcMonitor.bottom) / 2;
            if ((bar.top + bar.bottom) / 2 < monitorMid)
            {
                m_fromTop = true;
                anchorTop = std::max(work.top, bar.bottom);
            }
            else
            {
                anchorBottom = std::min(work.bottom, bar.top);
            }
        }
    }

    float gap = roundf(m_lay.EdgeGap * m_scale);
    float availablePx = float(anchorBottom - anchorTop) - 2 * gap;
    m_panelH = std::min(m_lay.PanelMaxH, floorf(availablePx) / m_scale);
    m_panelH = std::max(m_panelH, 320.f);
    LONG widthPx = (LONG)lroundf(m_lay.PanelW * m_scale), heightPx = (LONG)lroundf(m_panelH * m_scale);
    m_panelH = heightPx / m_scale;

    bool left = m_options.placement == Placement::Left ||
                (m_options.placement == Placement::Auto && IsTaskbarAlignedLeft());
    m_panelPx.x = left ? work.left + (LONG)gap : work.left + (work.right - work.left - widthPx) / 2;
    m_panelPx.y = m_fromTop ? anchorTop + (LONG)gap : anchorBottom - (LONG)gap - heightPx;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Surfaces
// ---------------------------------------------------------------------------------------------------------------

bool MenuView::BuildSurfaces()
{
    if (!m_dcomp)
    {
        return false;
    }
    float s = m_scale;
    UINT panelW = (UINT)lroundf(m_lay.PanelW * s), panelH = (UINT)lroundf(m_panelH * s);
    UINT winW = (UINT)lroundf((m_lay.PanelW + metric::ShadowPadL + metric::ShadowPadR) * s);
    UINT winH = (UINT)lroundf((m_panelH + metric::ShadowPadT + metric::ShadowPadB) * s);
    float cellW = m_lay.CellW;
    float rowW = m_lay.PanelW - 2 * B - 2 * metric::ContentPadX;
    auto px = [s](float dip) { return (UINT)ceilf(dip * s); };

    auto surface = [&](UINT w, UINT h, ComPtr<IDCompositionSurface>* out) {
        out->Reset();
        return SUCCEEDED(m_dcomp->CreateSurface(std::max(1u, w), std::max(1u, h), DXGI_FORMAT_B8G8R8A8_UNORM,
                                                DXGI_ALPHA_MODE_PREMULTIPLIED, out->GetAddressOf()));
    };
    float flyW = metric::FlyoutW + 2 * kFlyoutMarginX;
    float flyH = 2 * (metric::FlyoutPad + 1) + (float)m_powerItems.size() * metric::FlyoutItemH + kFlyoutMarginTop +
                 kFlyoutMarginBottom;
    if (!surface(winW, winH, &m_shadowSurface) || !surface(panelW, panelH, &m_backgroundSurface) ||
        !surface(panelW, panelH, &m_chromeSurface) || !surface(px(cellW), px(m_lay.CellH), &m_hlGrid) ||
        !surface(px(rowW), px(m_lay.ListRowH), &m_hlList) ||
        !surface(px(kLetterTile), px(kLetterTile), &m_hlLetter) ||
        !surface(px(cellW), px(m_lay.CellH), &m_pressedSurface) ||
        !surface(px(metric::CaretW), px(metric::CaretH), &m_caretSurface) ||
        !surface(px(flyW), px(flyH), &m_flyoutSurface))
    {
        SP_LOG_ERR(kTag, L"Composition surfaces could not be created");
        return false;
    }
    m_contentSurface.Reset();
    m_contentSurfaceH = panelH;
    if (FAILED(m_dcomp->CreateVirtualSurface(panelW, m_contentSurfaceH, DXGI_FORMAT_B8G8R8A8_UNORM,
                                             DXGI_ALPHA_MODE_PREMULTIPLIED, &m_contentSurface)))
    {
        return false;
    }
    m_bandValid = false;

    m_shadow->SetContent(m_shadowSurface.Get());
    m_background->SetContent(m_backgroundSurface.Get());
    m_chrome->SetContent(m_chromeSurface.Get());
    m_content->SetContent(m_contentSurface.Get());
    m_pressed->SetContent(m_pressedSurface.Get());
    m_caret->SetContent(m_caretSurface.Get());
    m_flyout->SetContent(m_flyoutSurface.Get());

    m_panel->SetOffsetX(roundf(metric::ShadowPadL * s));
    m_panel->SetOffsetY(roundf(metric::ShadowPadT * s));
    m_backdrop.Layout(roundf(metric::ShadowPadL * s), roundf(metric::ShadowPadT * s), (float)panelW, (float)panelH,
                      m_lay.PanelRadius * s);
    m_backdrop.SetVisible(m_acrylic);
    float radius = m_lay.PanelRadius * s;
    m_panelClip->SetLeft(0.f);
    m_panelClip->SetTop(0.f);
    m_panelClip->SetRight((float)panelW);
    m_panelClip->SetBottom((float)panelH);
    m_panelClip->SetTopLeftRadiusX(radius);    m_panelClip->SetTopLeftRadiusY(radius);
    m_panelClip->SetTopRightRadiusX(radius);   m_panelClip->SetTopRightRadiusY(radius);
    m_panelClip->SetBottomLeftRadiusX(radius); m_panelClip->SetBottomLeftRadiusY(radius);
    m_panelClip->SetBottomRightRadiusX(radius); m_panelClip->SetBottomRightRadiusY(radius);
    m_scrollHost->SetOffsetY(roundf(ContentTop() * s));
    m_scrollClip->SetLeft(0.f);
    m_scrollClip->SetTop(0.f);
    m_scrollClip->SetRight((float)panelW);
    UpdateScrollClip();

    RenderShadow();
    RenderBackground();
    RenderHighlightSurfaces();
    RenderCaret();

    m_builtDpi = m_dpi;
    m_builtH = m_panelH;
    return true;
}

void MenuView::UpdateScrollClip()
{
    if (m_scrollClip)
    {
        m_scrollClip->SetBottom(roundf(ContentHeight() * m_scale));
        m_scrollHost->SetOffsetY(roundf(ContentTop() * m_scale));
    }
}

ID2D1SolidColorBrush* MenuView::Brush(ID2D1DeviceContext* dc, const D2D1_COLOR_F& c)
{
    UINT32 key = ((UINT32)lroundf(c.r * 255) << 24) | ((UINT32)lroundf(c.g * 255) << 16) |
                 ((UINT32)lroundf(c.b * 255) << 8) | (UINT32)lroundf(c.a * 255);
    auto it = m_brushes.find(key);
    if (it != m_brushes.end())
    {
        return it->second.Get();
    }
    ComPtr<ID2D1SolidColorBrush> brush;
    (m_resources ? m_resources.Get() : dc)->CreateSolidColorBrush(c, &brush);
    return (m_brushes[key] = brush).Get();
}

ID2D1Bitmap* MenuView::BitmapFor(const PixelsPtr& pixels)
{
    if (!pixels || !m_resources || pixels->bgra.empty())
    {
        return nullptr;
    }
    auto it = m_bitmaps.find(pixels.get());
    if (it != m_bitmaps.end())
    {
        return it->second.second.Get();
    }
    ComPtr<ID2D1Bitmap> bitmap;
    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
    if (FAILED(m_resources->CreateBitmap(D2D1::SizeU(pixels->width, pixels->height), pixels->bgra.data(),
                                         pixels->width * 4, props, &bitmap)))
    {
        return nullptr;
    }
    m_bitmaps[pixels.get()] = { pixels, bitmap };
    return bitmap.Get();
}

void MenuView::Commit()
{
    if (m_dcomp)
    {
        m_dcomp->Commit();
    }
}

// A rounded-rectangle alpha mask for the shadow effect, `w` x `h` DIPs at the current DPI.
static ComPtr<ID2D1Bitmap1> CreateMask(ID2D1DeviceContext* resources, UINT dpi, float w, float h, float radius)
{
    ComPtr<ID2D1Bitmap1> mask;
    float s = dpi / 96.f;
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        (float)dpi, (float)dpi);
    if (FAILED(resources->CreateBitmap(D2D1::SizeU((UINT)ceilf(w * s), (UINT)ceilf(h * s)), nullptr, 0, props,
                                       &mask)))
    {
        return nullptr;
    }
    ComPtr<ID2D1SolidColorBrush> black;
    resources->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &black);
    resources->SetTarget(mask.Get());
    resources->SetDpi((float)dpi, (float)dpi);
    resources->BeginDraw();
    resources->Clear(D2D1::ColorF(0, 0, 0, 0));
    resources->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, w, h), radius, radius), black.Get());
    HRESULT hr = resources->EndDraw();
    resources->SetTarget(nullptr);
    return SUCCEEDED(hr) ? mask : nullptr;
}

static void DrawShadow(ID2D1DeviceContext* dc, ID2D1Bitmap1* mask, float x, float y, float blur,
                       const D2D1_COLOR_F& color)
{
    ComPtr<ID2D1Effect> shadow;
    if (!mask || FAILED(dc->CreateEffect(CLSID_D2D1Shadow, &shadow)))
    {
        return;
    }
    shadow->SetInput(0, mask);
    shadow->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, blur / 2);   // CSS blur radius = 2 x std dev
    shadow->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(color.r, color.g, color.b, color.a));
    dc->DrawImage(shadow.Get(), D2D1::Point2F(x, y));
}

void MenuView::RenderShadow()
{
    ComPtr<ID2D1Bitmap1> mask = CreateMask(m_resources.Get(), m_dpi, m_lay.PanelW, m_panelH, m_lay.PanelRadius);
    Draw dc(this, m_shadowSurface.Get());
    if (!dc)
    {
        return;
    }
    if (!m_options.shadow)
    {
        return;
    }
    // Section 6.1: 0 30 80 at 20 % and 0 4 14 at 8 %.
    DrawShadow(dc.dc.Get(), mask.Get(), metric::ShadowPadL, metric::ShadowPadT + 30, 80, m_pal.Shadow1);
    DrawShadow(dc.dc.Get(), mask.Get(), metric::ShadowPadL, metric::ShadowPadT + 4, 14, m_pal.Shadow2);
    if (m_translucent)
    {
        // The panel is see-through over the acrylic: the shadow must not be under it.
        dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);
        dc->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(metric::ShadowPadL, metric::ShadowPadT, metric::ShadowPadL + m_lay.PanelW,
                                          metric::ShadowPadT + m_panelH),
                              m_lay.PanelRadius, m_lay.PanelRadius),
            Brush(dc.dc.Get(), D2D1::ColorF(0, 0, 0, 0)));
        dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
    }
}

void MenuView::RenderBackground()
{
    Draw dc(this, m_backgroundSurface.Get());
    if (!dc)
    {
        return;
    }
    float footerTop = m_panelH - B - FooterH();
    if (m_image && m_resources)
    {
        // The picture, cover-fitted to the panel and blurred, with the panel colour over it.
        UINT w = (UINT)lroundf(m_lay.PanelW * m_scale), h = (UINT)lroundf(m_panelH * m_scale);
        ComPtr<ID2D1Bitmap1> layer;
        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            (float)m_dpi, (float)m_dpi);
        ID2D1Bitmap* picture = BitmapFor(m_image);
        if (picture && SUCCEEDED(m_resources->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, props, &layer)))
        {
            float iw = (float)m_image->width, ih = (float)m_image->height;
            float k = std::max(m_lay.PanelW / iw, m_panelH / ih);
            float dw = iw * k, dh = ih * k;
            m_resources->SetTarget(layer.Get());
            m_resources->SetDpi((float)m_dpi, (float)m_dpi);
            m_resources->BeginDraw();
            m_resources->Clear(D2D1::ColorF(0, 0, 0, 0));
            m_resources->DrawBitmap(picture,
                                    D2D1::RectF((m_lay.PanelW - dw) / 2, (m_panelH - dh) / 2, (m_lay.PanelW + dw) / 2,
                                                (m_panelH + dh) / 2),
                                    1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
            m_resources->EndDraw();
            m_resources->SetTarget(nullptr);
            ComPtr<ID2D1Effect> blur;
            if (m_options.imageBlur > 0 && SUCCEEDED(dc->CreateEffect(CLSID_D2D1GaussianBlur, &blur)))
            {
                blur->SetInput(0, layer.Get());
                blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, (float)m_options.imageBlur / 2);
                blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
                dc->DrawImage(blur.Get());
            }
            else
            {
                dc->DrawBitmap(layer.Get());
            }
        }
        D2D1_COLOR_F panel = m_pal.PanelOpaque, footer = m_pal.FooterTint;
        panel.a = std::clamp(m_options.imageTint, 0, 100) / 100.f;
        footer.a = m_pal.FooterTintAlpha;
        dc->FillRectangle(D2D1::RectF(0, 0, m_lay.PanelW, m_panelH), Brush(dc.dc.Get(), panel));
        if (FooterH() > 0)
        {
            dc->FillRectangle(D2D1::RectF(0, footerTop, m_lay.PanelW, m_panelH), Brush(dc.dc.Get(), footer));
        }
    }
    else if (m_translucent)
    {
        // Section 4.4: the tint over the blurred backdrop, and the footer's own layer over that.
        float alpha = std::clamp(m_options.opacity, 30, 100) / 100.f;
        D2D1_COLOR_F panel = m_pal.PanelTint, footer = m_pal.FooterTint;
        panel.a = alpha;
        footer.a = m_pal.FooterTintAlpha;
        dc->FillRectangle(D2D1::RectF(0, 0, m_lay.PanelW, m_panelH), Brush(dc.dc.Get(), panel));
        if (FooterH() > 0)
        {
            dc->FillRectangle(D2D1::RectF(0, footerTop, m_lay.PanelW, m_panelH), Brush(dc.dc.Get(), footer));
        }
    }
    else
    {
        dc->FillRectangle(D2D1::RectF(0, 0, m_lay.PanelW, m_panelH), Brush(dc.dc.Get(), m_pal.PanelOpaque));
        if (FooterH() > 0)
        {
            dc->FillRectangle(D2D1::RectF(0, footerTop, m_lay.PanelW, m_panelH), Brush(dc.dc.Get(), m_pal.FooterOpaque));
        }
    }
    if (FooterH() <= 0)
    {
        return;
    }
    dc->FillRectangle(D2D1::RectF(0, footerTop, m_lay.PanelW, footerTop + 1), Brush(dc.dc.Get(), m_pal.Ink06));
}

void MenuView::RenderHighlightSurfaces()
{
    float cellW = m_lay.CellW;
    float rowW = m_lay.PanelW - 2 * B - 2 * metric::ContentPadX;
    {
        Draw dc(this, m_hlGrid.Get());
        if (dc)
        {
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, cellW, m_lay.CellH), m_lay.CellRadius,
                                                       m_lay.CellRadius),
                                     Brush(dc.dc.Get(), m_pal.Ink06));
        }
    }
    {
        Draw dc(this, m_hlLetter.Get());
        if (dc)
        {
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, kLetterTile, kLetterTile), 10, 10),
                                     Brush(dc.dc.Get(), m_pal.Ink08));
        }
    }
    {
        Draw dc(this, m_hlList.Get());
        if (dc)
        {
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, rowW, m_lay.ListRowH),
                                                       metric::ListRowRadius, metric::ListRowRadius),
                                     Brush(dc.dc.Get(), m_pal.Ink06));
        }
    }
}

void MenuView::RenderCaret()
{
    Draw dc(this, m_caretSurface.Get());
    if (dc)
    {
        dc->FillRectangle(D2D1::RectF(0, 0, metric::CaretW, metric::CaretH), Brush(dc.dc.Get(), m_pal.Accent));
    }
}

void MenuView::RenderChrome()
{
    Draw dc(this, m_chromeSurface.Get());
    if (!dc)
    {
        return;
    }
    ID2D1DeviceContext* d = dc.dc.Get();

    // Search box (5.1)
    if (SearchShown())
    {
    D2D1_RECT_F box = SearchRect();
    d->FillRoundedRectangle(D2D1::RoundedRect(box, metric::SearchRadius, metric::SearchRadius),
                            Brush(d, m_pal.Field));
    d->DrawRoundedRectangle(D2D1::RoundedRect(Inset(box, 0.5f), metric::SearchRadius - 0.5f,
                                              metric::SearchRadius - 0.5f),
                            Brush(d, Lerp(m_pal.Ink08, m_pal.Accent, m_fieldT)), 1.f);
    {
        float ox = box.left + metric::SearchIconInset, oy = box.top + (metric::SearchH - metric::SearchIcon) / 2;
        ID2D1SolidColorBrush* ink = Brush(d, m_pal.TextMuted);
        d->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(ox + 7, oy + 7), 4.6f, 4.6f), ink, 1.6f, m_roundStroke.Get());
        d->DrawLine(D2D1::Point2F(ox + 10.5f, oy + 10.5f), D2D1::Point2F(ox + 14, oy + 14), ink, 1.6f,
                    m_roundStroke.Get());
    }
    float textLeft = box.left + metric::SearchTextX;
    float textRight = box.right - metric::SearchIconInset - (m_query.empty() ? 0 : metric::ClearBtn + 10);
    d->PushAxisAlignedClip(D2D1::RectF(textLeft, box.top, textRight, box.bottom), D2D1_ANTIALIAS_MODE_ALIASED);
    if (m_query.empty())
    {
        const wchar_t* placeholder = Text(Str::SearchPlaceholder);
        d->DrawText(placeholder, (UINT32)wcslen(placeholder), m_fmtSearch.Get(),
                    D2D1::RectF(textLeft, box.top, textRight + 200, box.bottom), Brush(d, m_pal.TextPlaceholder));
    }
    else
    {
        float x = textLeft - std::max(0.f, m_queryWidth - (textRight - textLeft));
        if (m_selectAll)
        {
            float cy = (box.top + box.bottom) / 2;
            d->FillRectangle(D2D1::RectF(x, cy - 10, x + m_queryWidth, cy + 10), Brush(d, m_pal.Selection));
        }
        d->DrawText(m_query.c_str(), (UINT32)m_query.size(), m_fmtSearch.Get(),
                    D2D1::RectF(x, box.top, x + m_queryWidth + 100, box.bottom), Brush(d, m_pal.TextPrimary));
    }
    d->PopAxisAlignedClip();
    if (!m_query.empty())
    {
        D2D1_RECT_F clear = ClearRect();
        D2D1_POINT_2F c = D2D1::Point2F((clear.left + clear.right) / 2, (clear.top + clear.bottom) / 2);
        d->FillEllipse(D2D1::Ellipse(c, metric::ClearBtn / 2, metric::ClearBtn / 2), Brush(d, m_pal.ClearBtn));
        d->DrawText(L"\x00D7", 1, m_fmtClear.Get(), clear, Brush(d, m_pal.TextMuted));
    }

    }

    // Header (5.2)
    const wchar_t* title = !m_query.empty() ? Text(Str::TitleBestMatches)
                          : m_historyMode    ? Text(Str::SectionHistory)
                          : m_showAll        ? Text(Str::TitleAllApps)
                                             : Text(Str::TitlePinned);
    float rowTop = B + HeaderY() + metric::HeaderPadT;
    float rowBottom = B + HeaderY() + metric::HeaderH - metric::HeaderPadB;
    const wchar_t* toggle = m_showAll ? Text(Str::ToggleBack) : Text(Str::ToggleAll);
    m_toggleW = m_fonts.Measure(m_fmtToggle.Get(), toggle, (UINT32)wcslen(toggle)) + 2 * metric::TogglePadX;
    if (m_options.showTitle || !m_query.empty())
    d->DrawText(title, (UINT32)wcslen(title), m_fmtTitle.Get(),
                D2D1::RectF(B + metric::HeaderPadX, rowTop, m_lay.PanelW - B - metric::HeaderPadX - m_toggleW - 8, rowBottom),
                Brush(d, m_pal.TextPrimary));
    if (m_query.empty())
    {
        D2D1_RECT_F t = ToggleRect();
        bool hot = m_hot.target == Target::Toggle;
        d->FillRoundedRectangle(D2D1::RoundedRect(t, metric::ToggleH / 2, metric::ToggleH / 2),
                                Brush(d, hot ? m_pal.Ink09 : m_pal.Ink05));
        d->DrawText(toggle, (UINT32)wcslen(toggle), m_fmtToggle.Get(), t, Brush(d, m_pal.TextSecondary));
    }

    // Recent files (5.5)
    m_chipRects.clear();
    if (RecentVisible())
    {
        float x = B + metric::RecentPadX, top = ContentBottom() + QuickH(), limit = m_lay.PanelW - B - metric::RecentPadX;
        for (size_t i = 0; i < m_recent.size(); ++i)
        {
            const RecentFile& file = m_recent[i];
            // Long names end in an ellipsis so one file cannot push every other chip off the strip.
            float textW = std::min(metric::ChipTextMax,
                                   m_fonts.Measure(m_fmtChip.Get(), file.name.c_str(), (UINT32)file.name.size()));
            float w = metric::ChipPadX + metric::ChipDot + metric::ChipDotGap + textW + metric::ChipPadX;
            if (x + w > limit)
            {
                break;
            }
            D2D1_RECT_F chip = { x, top, x + w, top + metric::ChipH };
            bool hot = m_hot.target == Target::Chip && m_hot.index == (int)i;
            d->FillRoundedRectangle(D2D1::RoundedRect(chip, metric::ChipH / 2, metric::ChipH / 2),
                                    Brush(d, m_pal.Field));
            d->DrawRoundedRectangle(D2D1::RoundedRect(Inset(chip, 0.5f), metric::ChipH / 2 - 0.5f,
                                                      metric::ChipH / 2 - 0.5f),
                                    Brush(d, hot ? m_pal.Ink18 : m_pal.Ink07), 1.f);
            static const D2D1_COLOR_F kKindColors[] = { color::KindDocument, color::KindImage, color::KindCode,
                                                        color::KindMedia, color::KindArchive, color::KindOther };
            float cy = top + metric::ChipH / 2;
            d->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + metric::ChipPadX + metric::ChipDot / 2, cy),
                                         metric::ChipDot / 2, metric::ChipDot / 2),
                           Brush(d, kKindColors[(int)file.kind]));
            float tx = x + metric::ChipPadX + metric::ChipDot + metric::ChipDotGap;
            d->DrawText(file.name.c_str(), (UINT32)file.name.size(), m_fmtChip.Get(),
                        D2D1::RectF(tx, top, tx + textW + 1, top + metric::ChipH), Brush(d, m_pal.TextChip));
            m_chipRects.push_back(chip);
            x += w + metric::ChipGap;
        }
    }

    // Quick settings (not in the handoff): chips like the recent files, filled with the accent when on.
    m_quickRects.clear();
    m_quickKinds.clear();
    if (QuickVisible() && m_quickKnown && m_fmtIcon)
    {
        struct Chip { int kind; bool present, on; const wchar_t* glyph; std::wstring text; };
        wchar_t volume[16];
        swprintf_s(volume, L"%d%%", m_quickState.volume);
        const Chip chips[] = {
            { 0, m_quickState.wifiPresent, m_quickState.wifiOn, L"\xE701", L"Wi-Fi" },
            { 1, m_quickState.bluetoothPresent, m_quickState.bluetoothOn, L"\xE702", L"Bluetooth" },
            { 2, m_quickState.volumePresent, !m_quickState.muted, m_quickState.muted ? L"\xE74F" : L"\xE767", volume },
            { 3, true, m_quickState.dark, L"\xE708", Text(Str::QuickDark) },
        };
        float x = B + metric::RecentPadX, top = ContentBottom() + 6, limit = m_lay.PanelW - B - metric::RecentPadX;
        const float h = 32;
        for (const Chip& chip : chips)
        {
            if (!chip.present)
            {
                continue;
            }
            float textW = m_fonts.Measure(m_fmtChip.Get(), chip.text.c_str(), (UINT32)chip.text.size());
            float w = 12 + 16 + 8 + textW + 14;
            if (x + w > limit)
            {
                break;
            }
            D2D1_RECT_F r = { x, top, x + w, top + h };
            bool hot = m_hot.target == Target::Quick && m_hot.index == (int)m_quickRects.size();
            if (chip.on)
            {
                D2D1_COLOR_F fill = m_pal.Accent;
                if (hot) fill.a = 0.85f;
                d->FillRoundedRectangle(D2D1::RoundedRect(r, h / 2, h / 2), Brush(d, fill));
            }
            else
            {
                d->FillRoundedRectangle(D2D1::RoundedRect(r, h / 2, h / 2), Brush(d, m_pal.Field));
                d->DrawRoundedRectangle(D2D1::RoundedRect(Inset(r, 0.5f), h / 2 - 0.5f, h / 2 - 0.5f),
                                        Brush(d, hot ? m_pal.Ink18 : m_pal.Ink07), 1.f);
            }
            D2D1_COLOR_F ink = chip.on ? D2D1::ColorF(1, 1, 1, 1) : m_pal.TextChip;
            d->DrawText(chip.glyph, 1, m_fmtIcon.Get(), D2D1::RectF(x + 12, top, x + 12 + 16, top + h), Brush(d, ink));
            d->DrawText(chip.text.c_str(), (UINT32)chip.text.size(), m_fmtChip.Get(),
                        D2D1::RectF(x + 12 + 16 + 8, top, x + w, top + h), Brush(d, ink));
            m_quickRects.push_back(r);
            m_quickKinds.push_back(chip.kind);
            x += w + metric::ChipGap;
        }
    }

    // Footer (5.6): account button
    if (AccountShown())
    {
    float maxName = m_lay.PanelW - 2 * B - metric::FooterPadL - metric::FooterPadR - metric::PowerBtn - 24 -
                    (ShortcutsShown() ? m_shortcuts.size() * (kShortcutSize + kShortcutGap) : 0.f) -
                    (metric::AccountPadL + metric::Avatar + metric::AvatarGap + metric::AccountPadR);
    m_nameW = m_options.showUserName
                  ? std::min(maxName, m_fonts.Measure(m_fmtUser.Get(), m_user.name.c_str(), (UINT32)m_user.name.size()))
                  : -metric::AvatarGap - metric::AccountPadR + metric::AccountPadL;
    D2D1_RECT_F account = AccountRect();
    if (m_hot.target == Target::Account)
    {
        d->FillRoundedRectangle(D2D1::RoundedRect(account, metric::AccountH / 2, metric::AccountH / 2),
                                Brush(d, m_pal.Ink05));
    }
    float cy = (account.top + account.bottom) / 2;
    D2D1_RECT_F avatar = { account.left + metric::AccountPadL, cy - metric::Avatar / 2,
                           account.left + metric::AccountPadL + metric::Avatar, cy + metric::Avatar / 2 };
    D2D1_ELLIPSE circle = D2D1::Ellipse(D2D1::Point2F((avatar.left + avatar.right) / 2, cy), metric::Avatar / 2,
                                        metric::Avatar / 2);
    ID2D1Bitmap* picture = BitmapFor(m_user.picture);
    ComPtr<ID2D1EllipseGeometry> circleGeometry;
    if (picture && SUCCEEDED(m_d2dFactory->CreateEllipseGeometry(circle, &circleGeometry)))
    {
        d->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), circleGeometry.Get()), nullptr);
        D2D1_SIZE_U size = picture->GetPixelSize();
        // Cover: crop the longer side.
        float side = (float)std::min(size.width, size.height);
        D2D1_RECT_F source = { (size.width - side) / 2, (size.height - side) / 2, (size.width + side) / 2,
                               (size.height + side) / 2 };
        d->DrawBitmap(picture, avatar, 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, &source);
        d->PopLayer();
    }
    else
    {
        // 135 degrees: from the top-left corner towards the bottom-right one.
        D2D1_GRADIENT_STOP stops[] = { { 0.f, color::AvatarA }, { 1.f, color::AvatarB } };
        ComPtr<ID2D1GradientStopCollection> collection;
        ComPtr<ID2D1LinearGradientBrush> gradient;
        if (SUCCEEDED(d->CreateGradientStopCollection(stops, 2, &collection)) &&
            SUCCEEDED(d->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(avatar.left, avatar.top),
                                                    D2D1::Point2F(avatar.right, avatar.bottom)),
                collection.Get(), &gradient)))
        {
            d->FillEllipse(circle, gradient.Get());
        }
        if (!m_user.name.empty())
        {
            wchar_t initial[2] = { m_user.name[0], 0 };
            LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_UPPERCASE | LCMAP_LINGUISTIC_CASING, initial, 1, initial,
                          1, nullptr, nullptr, 0);
            d->DrawText(initial, 1, m_fmtInitial.Get(), avatar, Brush(d, D2D1::ColorF(1, 1, 1, 1)));
        }
    }
    float nameLeft = avatar.right + metric::AvatarGap;
    if (m_options.showUserName)
    {
        d->DrawText(m_user.name.c_str(), (UINT32)m_user.name.size(), m_fmtUser.Get(),
                    D2D1::RectF(nameLeft, account.top, nameLeft + m_nameW + 1, account.bottom),
                    Brush(d, m_pal.TextPrimary));
    }
    }

    // Shortcut buttons, right-aligned before the power button
    if (ShortcutsShown())
    {
        for (int i = 0; i < (int)m_shortcuts.size(); ++i)
        {
            D2D1_RECT_F r = ShortcutRect(i);
            D2D1_POINT_2F c = D2D1::Point2F((r.left + r.right) / 2, (r.top + r.bottom) / 2);
            if (m_hot.target == Target::Shortcut && m_hot.index == i)
            {
                d->FillEllipse(D2D1::Ellipse(c, kShortcutSize / 2, kShortcutSize / 2), Brush(d, m_pal.Ink06));
            }
            if (ID2D1Bitmap* icon = BitmapFor(m_shortcuts[i].icon))
            {
                float half = kShortcutIcon / 2;
                float ix = Snap(c.x - half), iy = Snap(c.y - half);
                d->DrawBitmap(icon, D2D1::RectF(ix, iy, ix + kShortcutIcon, iy + kShortcutIcon), 1.f,
                              D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
            }
        }
    }

    // Power button
    if (PowerShown())
    {
    D2D1_RECT_F power = PowerRect();
    D2D1_POINT_2F pc = D2D1::Point2F((power.left + power.right) / 2, (power.top + power.bottom) / 2);
    if (m_powerOpen || m_hot.target == Target::Power)
    {
        d->FillEllipse(D2D1::Ellipse(pc, metric::PowerBtn / 2, metric::PowerBtn / 2),
                       Brush(d, m_powerOpen ? m_pal.Ink08 : m_pal.Ink06));
    }
    {
        float ox = pc.x - metric::PowerIcon / 2, oy = pc.y - metric::PowerIcon / 2;
        ID2D1SolidColorBrush* ink = Brush(d, m_pal.TextLabel);
        d->DrawLine(D2D1::Point2F(ox + 8, oy + 1.8f), D2D1::Point2F(ox + 8, oy + 7.4f), ink, 1.6f,
                    m_roundStroke.Get());
        ComPtr<ID2D1PathGeometry> arc;
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(m_d2dFactory->CreatePathGeometry(&arc)) && SUCCEEDED(arc->Open(&sink)))
        {
            sink->BeginFigure(D2D1::Point2F(ox + 4.4f, oy + 3.9f), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(ox + 11.6f, oy + 3.9f), D2D1::SizeF(5.4f, 5.4f), 0,
                                          D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            d->DrawGeometry(arc.Get(), ink, 1.6f, m_roundStroke.Get());
        }
    }
    }

    // Panel border (6.2), drawn last so it sits on top of the footer.
    d->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, m_lay.PanelW - 0.5f, m_panelH - 0.5f),
                                              std::max(0.f, m_lay.PanelRadius - 0.5f), std::max(0.f, m_lay.PanelRadius - 0.5f)),
                            Brush(d, m_pal.PanelBorder), 1.f);
    UpdateTooltips();
}

void MenuView::RenderFlyout()
{
    D2D1_RECT_F box = FlyoutRect();
    float w = box.right - box.left, h = box.bottom - box.top;
    ComPtr<ID2D1Bitmap1> mask = CreateMask(m_resources.Get(), m_dpi, w, h, metric::FlyoutRadius);
    Draw dc(this, m_flyoutSurface.Get());
    if (!dc)
    {
        return;
    }
    ID2D1DeviceContext* d = dc.dc.Get();
    DrawShadow(d, mask.Get(), kFlyoutMarginX, kFlyoutMarginTop + metric::FlyoutShadowY, metric::FlyoutShadowBlur,
               m_pal.ShadowFl);
    D2D1_RECT_F local = { kFlyoutMarginX, kFlyoutMarginTop, kFlyoutMarginX + w, kFlyoutMarginTop + h };
    d->FillRoundedRectangle(D2D1::RoundedRect(local, metric::FlyoutRadius, metric::FlyoutRadius),
                            Brush(d, m_pal.Field));
    d->DrawRoundedRectangle(D2D1::RoundedRect(Inset(local, 0.5f), metric::FlyoutRadius - 0.5f,
                                              metric::FlyoutRadius - 0.5f),
                            Brush(d, m_pal.Ink06), 1.f);
    static const Str kLabels[] = { Str::Lock, Str::SignOut, Str::Sleep, Str::Hibernate, Str::UpdateRestart,
                                   Str::Restart, Str::UpdateShutDown, Str::ShutDown, Str::AdvancedStartup,
                                   Str::Firmware };
    static_assert(ARRAYSIZE(kLabels) == (size_t)PowerAction::Count, "one label per power action");
    for (int i = 0; i < (int)m_powerItems.size(); ++i)
    {
        D2D1_RECT_F item = FlyoutItemRect(i);
        item = { item.left - box.left + local.left, item.top - box.top + local.top, item.right - box.left + local.left,
                 item.bottom - box.top + local.top };
        if (i == m_flyoutHot)
        {
            d->FillRoundedRectangle(D2D1::RoundedRect(item, metric::FlyoutItemRadius, metric::FlyoutItemRadius),
                                    Brush(d, m_pal.Ink05));
        }
        const wchar_t* text = Text(kLabels[(int)m_powerItems[i]]);
        d->DrawText(text, (UINT32)wcslen(text), m_fmtButton.Get(),
                    D2D1::RectF(item.left + metric::FlyoutItemPadX, item.top, item.right - metric::FlyoutItemPadX,
                                item.bottom),
                    Brush(d, m_pal.TextPrimary));
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Content (5.3, 5.4)
// ---------------------------------------------------------------------------------------------------------------

void MenuView::BuildModel()
{
    m_model = Model{};
    const float left = B + metric::ContentPadX;
    const float right = m_lay.PanelW - B - metric::ContentPadX;
    float y = metric::ContentPadT;

    auto heading = [&](const std::wstring& text) {
        if (!m_model.items.empty())
        {
            y += 6;   // a little air above a new section
        }
        m_model.headings.push_back({ text, { left, y, right, y + metric::ListHeadH } });
        y += metric::ListHeadH;
    };
    auto grid = [&](const std::vector<int>& apps) {
        for (size_t i = 0; i < apps.size(); ++i)
        {
            int col = (int)i % m_lay.GridCols, row = (int)i / m_lay.GridCols;
            float x = left + col * (m_lay.CellW + metric::GridGap);
            float top = y + row * (m_lay.CellH + metric::GridGap);
            m_model.items.push_back({ apps[i], { x, top, x + m_lay.CellW, top + m_lay.CellH }, ItemKind::Cell });
        }
        int rows = ((int)apps.size() + m_lay.GridCols - 1) / m_lay.GridCols;
        y += rows * m_lay.CellH + std::max(0, rows - 1) * metric::GridGap;
    };
    auto row = [&](int app, int result) {
        m_model.items.push_back({ app, { left, y, right, y + m_lay.ListRowH },
                                  result >= 0 ? ItemKind::Result : ItemKind::Row, result });
        y += m_lay.ListRowH;
    };
    auto results = [&](std::vector<Result>& list) {
        for (Result& r : list)
        {
            m_model.results.push_back(std::move(r));
            row(-1, (int)m_model.results.size() - 1);
        }
    };

    m_model.grid = true;
    if (!m_query.empty())
    {
        // Search: a calculation or an unmistakable command first, then the apps, then Settings, then a program
        // that only might be what was meant.
        std::vector<int> apps = m_apps ? SearchApps(*m_apps, m_query) : std::vector<int>();
        apps.erase(std::remove_if(apps.begin(), apps.end(), [this](int i) { return IsHidden(i); }), apps.end());
        std::vector<Result> windows =
            m_options.searchWindows ? SearchWindows(m_query, 5) : std::vector<Result>();
        std::vector<Result> strong, weak, settings;
        double value = 0;
        if (m_options.searchCalculator && Calculate(m_query, &value))
        {
            strong.push_back(CalcResult(value));
        }
        Result command;
        if (m_options.searchCommands && ResolveCommand(m_query, &command))
        {
            (command.strong ? strong : weak).push_back(command);
        }
        if (m_options.searchSettings)
        {
            settings = SearchSettings(m_query, 4);
        }
        bool sections = !strong.empty() || !weak.empty() || !settings.empty() || !windows.empty();
        results(strong);
        if (!apps.empty())
        {
            if (sections)
            {
                heading(Text(Str::SectionApps));
            }
            grid(apps);
        }
        if (!windows.empty())
        {
            heading(Text(Str::SectionWindows));
            results(windows);
        }
        if (!settings.empty())
        {
            heading(Text(Str::SectionSettings));
            results(settings);
        }
        if (!m_fileResults.empty())
        {
            heading(Text(Str::SectionFiles));
            std::vector<Result> files = m_fileResults;
            results(files);
        }
        if (!weak.empty())
        {
            heading(Text(Str::SectionCommands));
            results(weak);
        }
        m_model.empty = m_model.items.empty();
    }
    else if (m_historyMode)
    {
        std::vector<Result> past;
        for (const std::wstring& line : m_history.Lines())
        {
            if (past.size() >= 10)
            {
                break;
            }
            Result r;
            r.kind = ResultKind::History;
            r.title = line;
            r.target = line;
            past.push_back(std::move(r));
        }
        results(past);
    }
    else if (m_showAll && m_apps && m_letters)
    {
        // The A-Z index: one tile per heading of the list.
        m_letterList.clear();
        for (const App& app : m_apps->apps)
        {
            if (m_letterList.empty() || m_letterList.back() != app.heading)
            {
                m_letterList.push_back(app.heading);
            }
        }
        const float tile = kLetterTile, gap = 6;
        int cols = std::max(1, (int)((right - left + gap) / (tile + gap)));
        float x0 = left + ((right - left) - (cols * tile + (cols - 1) * gap)) / 2;
        for (size_t i = 0; i < m_letterList.size(); ++i)
        {
            float x = x0 + (i % cols) * (tile + gap), top = y + (i / cols) * (tile + gap);
            m_model.items.push_back({ -1, { x, top, x + tile, top + tile }, ItemKind::Letter, (int)i });
        }
        int rows = ((int)m_letterList.size() + cols - 1) / cols;
        y += rows * tile + std::max(0, rows - 1) * gap;
    }
    else if (m_showAll && m_apps)
    {
        m_model.grid = false;
        if (m_options.showNewApps && m_newApps.Any())
        {
            bool any = false;
            for (int index = 0; index < (int)m_apps->apps.size(); ++index)
            {
                if (m_newApps.IsNew(m_apps->apps[index].id) && !IsHidden(index))
                {
                    if (!any)
                    {
                        heading(Text(Str::SectionNew));
                        any = true;
                    }
                    row(index, -1);
                }
            }
        }
        const std::wstring* previous = nullptr;
        for (int index = 0; index < (int)m_apps->apps.size(); ++index)
        {
            const App& app = m_apps->apps[index];
            if (IsHidden(index))
            {
                continue;
            }
            if (!previous || *previous != app.heading)
            {
                m_model.headings.push_back({ app.heading, { left, y, right, y + metric::ListHeadH } });
                y += metric::ListHeadH;
                previous = &app.heading;
            }
            row(index, -1);
        }
    }
    else if (m_apps)
    {
        std::vector<int> pinned;
        for (const std::wstring& id : m_pins.Ids())
        {
            int index = FindApp(id);
            if (index >= 0)
            {
                pinned.push_back(index);
            }
        }
        grid(pinned);
        if (m_options.showMostUsed)
        {
            std::vector<int> used;
            for (const std::wstring& id : m_usage.Top(std::clamp(m_options.mostUsedCount, 1, 10), m_pins))
            {
                int index = FindApp(id);
                if (index >= 0 && !IsHidden(index))
                {
                    used.push_back(index);
                }
            }
            if (!used.empty())
            {
                heading(Text(Str::SectionMostUsed));
                for (int index : used)
                {
                    row(index, -1);
                }
            }
        }
    }
    m_model.height = y + metric::ContentPadB;

    // Keep the content surface tall enough for the whole list (a virtual surface only holds what is drawn).
    if (m_contentSurface)
    {
        UINT height = (UINT)ceilf(std::max(m_model.height, ContentHeight()) * m_scale);
        if (height != m_contentSurfaceH)
        {
            m_contentSurfaceH = height;
            m_contentSurface->Resize((UINT)lroundf(m_lay.PanelW * m_scale), height);
        }
    }
    m_bandValid = false;
}

void MenuView::RenderContent(bool force)
{
    if (!m_contentSurface)
    {
        return;
    }
    float viewport = ContentHeight();
    float surfaceH = m_contentSurfaceH / m_scale;
    if (!force && m_bandValid && m_scrollY >= m_bandTop && m_scrollY + viewport <= m_bandBottom)
    {
        return;
    }
    // One viewport above and below: wheel steps and keyboard moves stay inside it and cost no drawing.
    float top = std::max(0.f, m_scrollY - viewport);
    float bottom = std::min(surfaceH, m_scrollY + 2 * viewport);
    RenderContentBand(top, bottom);
    m_bandTop = top;
    m_bandBottom = bottom;
    m_bandValid = true;
    RECT keep = { 0, (LONG)floorf(top * m_scale), (LONG)lroundf(m_lay.PanelW * m_scale), (LONG)ceilf(bottom * m_scale) };
    m_contentSurface->Trim(&keep, 1);
}

void MenuView::RenderContentBand(float top, float bottom)
{
    RECT update = { 0, (LONG)floorf(top * m_scale), (LONG)lroundf(m_lay.PanelW * m_scale),
                    (LONG)std::min<float>(ceilf(bottom * m_scale), (float)m_contentSurfaceH) };
    if (update.bottom <= update.top)
    {
        return;
    }
    Draw dc(this, m_contentSurface.Get(), &update);
    if (!dc)
    {
        return;
    }
    ID2D1DeviceContext* d = dc.dc.Get();
    float bandTop = update.top / m_scale, bandBottom = update.bottom / m_scale;
    for (const Heading& heading : m_model.headings)
    {
        if (heading.rect.bottom < bandTop || heading.rect.top > bandBottom)
        {
            continue;
        }
        D2D1_RECT_F r = { heading.rect.left + metric::ListPadX, heading.rect.top, heading.rect.right,
                          heading.rect.bottom - metric::ListHeadPadB };
        d->DrawText(heading.text.c_str(), (UINT32)heading.text.size(), m_fmtLetter.Get(), r,
                    Brush(d, m_pal.Accent));
    }
    for (size_t i = 0; i < m_model.items.size(); ++i)
    {
        const Item& item = m_model.items[i];
        if (item.rect.bottom < bandTop || item.rect.top > bandBottom)
        {
            continue;
        }
        if ((int)i == m_pressedItem)
        {
            continue;   // drawn by the pressed visual while it is scaled
        }
        DrawItem(d, item, false);
    }
    if (m_model.empty)
    {
        wchar_t text[64 + kMaxQuery * 2];
        _snwprintf_s(text, _TRUNCATE, Text(Str::NoResults), m_query.c_str());
        float y = metric::ContentPadT + metric::EmptyPadT;
        d->DrawText(text, (UINT32)wcslen(text), m_fmtEmpty.Get(),
                    D2D1::RectF(B + metric::ContentPadX, y, m_lay.PanelW - B - metric::ContentPadX, y + 40),
                    Brush(d, m_pal.TextPlaceholder));
    }
}

void MenuView::DrawIcon(ID2D1DeviceContext* d, const App& app, D2D1_RECT_F rect, float radius)
{
    if (ID2D1Bitmap* bitmap = BitmapFor(app.icon))
    {
        d->DrawBitmap(bitmap, rect, 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
    }
    else
    {
        // Still loading: a quiet tile in its place, so the grid does not jump when the icon arrives.
        d->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), Brush(d, m_pal.Ink06));
    }
}

PixelsPtr MenuView::ResultIcon(const Result& result)
{
    if (result.kind != ResultKind::Run && result.kind != ResultKind::Open && result.kind != ResultKind::Window)
    {
        return nullptr;
    }
    auto it = m_resultIcons.find(result.target);
    if (it != m_resultIcons.end())
    {
        return it->second;
    }
    if (result.kind == ResultKind::Window)
    {
        // The window's own icon, else its class icon, else its program's.
        UINT px = (UINT)ceilf(m_lay.ListIcon * m_scale);
        DWORD_PTR icon = 0;
        if (!SendMessageTimeoutW(result.window, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 50, &icon) || !icon)
        {
            icon = GetClassLongPtrW(result.window, GCLP_HICON);
        }
        PixelsPtr pixels = PixelsFromIcon(reinterpret_cast<HICON>(icon), px);
        if (!pixels)
        {
            DWORD pid = 0;
            GetWindowThreadProcessId(result.window, &pid);
            if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))
            {
                wchar_t path[MAX_PATH];
                DWORD size = MAX_PATH;
                if (QueryFullProcessImageNameW(process, 0, path, &size))
                {
                    Result program;
                    program.kind = ResultKind::Run;
                    program.target = path;
                    pixels = ResultIcon(program);
                }
                CloseHandle(process);
            }
        }
        m_resultIcons[result.target] = pixels;
        return pixels;
    }
    // Local files, folders and shell: locations only; a URL or a share gets the glyph.
    PixelsPtr pixels;
    if (!PathIsURLW(result.target.c_str()) && !PathIsNetworkPathW(result.target.c_str()))
    {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(result.target.c_str(), nullptr, &pidl, 0, nullptr)))
        {
            ComPtr<IShellItemImageFactory> factory;
            HBITMAP bitmap = nullptr;
            UINT px = (UINT)ceilf(m_lay.ListIcon * m_scale);
            if (SUCCEEDED(SHCreateItemFromIDList(pidl, IID_PPV_ARGS(&factory))) &&
                SUCCEEDED(factory->GetImage(SIZE{ (LONG)px, (LONG)px }, SIIGBF_ICONONLY, &bitmap)) && bitmap)
            {
                pixels = PixelsFromHBitmap(bitmap);
                DeleteObject(bitmap);
            }
            CoTaskMemFree(pidl);
        }
    }
    if (m_resultIcons.size() > 64)
    {
        m_resultIcons.clear();
    }
    m_resultIcons[result.target] = pixels;
    return pixels;
}

void MenuView::DrawResult(ID2D1DeviceContext* d, const Item& item)
{
    const Result& result = m_model.results[item.result];
    const D2D1_RECT_F& r = item.rect;
    float x = Snap(r.left + metric::ListPadX), y = Snap(r.top + (m_lay.ListRowH - m_lay.ListIcon) / 2);
    D2D1_RECT_F icon = D2D1::RectF(x, y, x + m_lay.ListIcon, y + m_lay.ListIcon);

    ID2D1Bitmap* bitmap = nullptr;
    if (result.kind == ResultKind::Setting)
    {
        int settingsApp = FindApp(L"windows.immersivecontrolpanel_cw5n1h2txyewy!microsoft.windows.immersivecontrolpanel");
        if (settingsApp >= 0)
        {
            bitmap = BitmapFor(m_apps->apps[settingsApp].icon);
        }
    }
    else
    {
        bitmap = BitmapFor(ResultIcon(result));
    }
    if (bitmap)
    {
        d->DrawBitmap(bitmap, icon, 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
    }
    else
    {
        // A glyph on a tile: "=" in the accent for a calculation, an arrow for a location, ">" for a program.
        bool calc = result.kind == ResultKind::Calc;
        d->FillRoundedRectangle(D2D1::RoundedRect(icon, m_lay.ListIconRadius, m_lay.ListIconRadius),
                                Brush(d, calc ? m_pal.Accent : m_pal.Ink08));
        const wchar_t* glyph = calc ? L"=" : result.kind == ResultKind::Open ? L"\x2197" : L"\x203A";
        IDWriteTextFormat* format = m_fmtInitial.Get();
        if (result.kind == ResultKind::History && m_fmtIcon)
        {
            glyph = L"\xE81C";   // History
            format = m_fmtIcon.Get();
        }
        d->DrawText(glyph, (UINT32)wcslen(glyph), format, icon,
                    Brush(d, calc ? D2D1::ColorF(1, 1, 1, 1) : m_pal.TextPrimary));
    }

    float textLeft = r.left + metric::ListPadX + m_lay.ListIcon + metric::ListIconGap;
    float textRight = r.right - metric::ListPadX;
    float titleW = std::min(m_fonts.Measure(m_fmtListItem.Get(), result.title.c_str(), (UINT32)result.title.size()),
                            (textRight - textLeft) * 0.62f);
    d->DrawText(result.title.c_str(), (UINT32)result.title.size(), m_fmtListItem.Get(),
                D2D1::RectF(textLeft, r.top, textLeft + titleW + 1, r.bottom), Brush(d, m_pal.TextPrimary));
    if (!result.subtitle.empty())
    {
        float subLeft = textLeft + titleW + 10;
        d->DrawText(result.subtitle.c_str(), (UINT32)result.subtitle.size(), m_fmtChip.Get(),
                    D2D1::RectF(subLeft, r.top, textRight, r.bottom), Brush(d, m_pal.TextMuted));
    }
}

void MenuView::DrawItem(ID2D1DeviceContext* d, const Item& item, bool)
{
    if (item.kind == ItemKind::Result)
    {
        DrawResult(d, item);
        return;
    }
    if (item.kind == ItemKind::Letter)
    {
        const std::wstring& letter = m_letterList[item.result];
        d->FillRoundedRectangle(D2D1::RoundedRect(item.rect, 10, 10), Brush(d, m_pal.Ink05));
        d->DrawText(letter.c_str(), (UINT32)letter.size(), m_fmtTitle.Get(),
                    D2D1::RectF(item.rect.left + (kLetterTile - m_fonts.Measure(m_fmtTitle.Get(), letter.c_str(),
                                                                               (UINT32)letter.size())) / 2,
                                item.rect.top, item.rect.right, item.rect.bottom),
                    Brush(d, m_pal.Accent));
        return;
    }
    bool fresh = m_options.showNewApps && m_newApps.IsNew(m_apps->apps[item.app].id);
    const App& app = m_apps->apps[item.app];
    const D2D1_RECT_F& r = item.rect;
    if (item.kind == ItemKind::Cell)
    {
        float cellW = r.right - r.left;
        float x = Snap(r.left + (cellW - m_lay.Icon) / 2), y = Snap(r.top + m_lay.IconTop);
        DrawIcon(d, app, D2D1::RectF(x, y, x + m_lay.Icon, y + m_lay.Icon), m_lay.IconRadius);
        if (fresh)
        {
            d->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + m_lay.Icon, y + 2), 4, 4), Brush(d, m_pal.Accent));
        }
        if (!m_lay.labels)
        {
            return;
        }
        float labelTop = r.top + m_lay.IconTop + m_lay.Icon + metric::IconLabelGap;
        float labelLeft = r.left + (cellW - m_lay.LabelMaxW) / 2;
        d->DrawText(app.name.c_str(), (UINT32)app.name.size(), m_fmtGridLabel.Get(),
                    D2D1::RectF(labelLeft, labelTop, labelLeft + m_lay.LabelMaxW,
                                labelTop + type::GridLabel.size * type::LineHeight),
                    Brush(d, m_pal.TextLabel));
    }
    else
    {
        float x = Snap(r.left + metric::ListPadX), y = Snap(r.top + (m_lay.ListRowH - m_lay.ListIcon) / 2);
        DrawIcon(d, app, D2D1::RectF(x, y, x + m_lay.ListIcon, y + m_lay.ListIcon), m_lay.ListIconRadius);
        float textLeft = r.left + metric::ListPadX + m_lay.ListIcon + metric::ListIconGap;
        float textRight = r.right - metric::ListPadX;
        if (fresh)
        {
            const wchar_t* badge = Text(Str::BadgeNew);
            float w = m_fonts.Measure(m_fmtChip.Get(), badge, (UINT32)wcslen(badge));
            d->DrawText(badge, (UINT32)wcslen(badge), m_fmtChip.Get(),
                        D2D1::RectF(textRight - w, r.top, textRight + 1, r.bottom), Brush(d, m_pal.Accent));
            textRight -= w + 10;
        }
        d->DrawText(app.name.c_str(), (UINT32)app.name.size(), m_fmtListItem.Get(),
                    D2D1::RectF(textLeft, r.top, textRight, r.bottom), Brush(d, m_pal.TextPrimary));
    }
}

void MenuView::UpdateHighlight(bool animate)
{
    if (!m_highlight)
    {
        return;
    }
    if (m_model.items.empty() || m_sel < 0 || m_sel >= (int)m_model.items.size() || (m_pressedItem >= 0 && !m_dragging))
    {
        SetVisible(m_highlight, false);
        return;
    }
    const D2D1_RECT_F& r = m_model.items[m_sel].rect;
    ItemKind kind = m_model.items[m_sel].kind;
    m_highlight->SetContent(kind == ItemKind::Cell ? m_hlGrid.Get() : kind == ItemKind::Letter ? m_hlLetter.Get()
                                                                                              : m_hlList.Get());
    m_highlight->SetOffsetX(roundf(r.left * m_scale));
    m_highlight->SetOffsetY(roundf(r.top * m_scale));
    SetVisible(m_highlight, true);
    auto v3 = V3(m_highlight);
    if (!v3)
    {
        return;
    }
    ComPtr<IDCompositionAnimation> fade;
    if (animate && m_animations && SUCCEEDED(m_dcomp->CreateAnimation(&fade)))
    {
        AddBezier(fade.Get(), 0.f, 1.f, motion::Highlight, motion::Linear);
        v3->SetOpacity(fade.Get());
    }
    else
    {
        v3->SetOpacity(1.f);
    }
}

void MenuView::UpdateCaret()
{
    if (!m_caret)
    {
        return;
    }
    D2D1_RECT_F box = SearchRect();
    float textLeft = box.left + metric::SearchTextX;
    float textRight = box.right - metric::SearchIconInset - (m_query.empty() ? 0 : metric::ClearBtn + 10);
    float x = textLeft + std::min(m_queryWidth, textRight - textLeft);
    float y = box.top + (metric::SearchH - metric::CaretH) / 2;
    m_caret->SetOffsetX(roundf(x * m_scale));
    m_caret->SetOffsetY(roundf(y * m_scale));
    SetVisible(m_caret, m_open && m_caretOn && SearchShown());
}

// ---------------------------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------------------------

int MenuView::FindApp(const std::wstring& id) const
{
    std::wstring key = id;
    CharLowerBuffW(key.data(), (DWORD)key.size());
    auto it = m_appIndex.find(key);
    return it == m_appIndex.end() ? -1 : it->second;
}

void MenuView::ResetState()
{
    m_query.clear();
    m_queryWidth = 0;
    m_selectAll = false;
    m_showAll = m_options.startWithAllApps;
    m_letters = false;
    m_historyMode = false;
    m_fileResults.clear();
    m_sel = 0;
    m_powerOpen = false;
    m_flyoutHot = -1;
    m_scrollY = 0;
    m_hot = {};
    m_down = {};
    m_pressedItem = -1;
    m_fieldT = m_fieldFrom = m_fieldTo = 0;
    m_caretOn = true;
}

void MenuView::SetQuery(std::wstring query)
{
    bool wasEmpty = m_query.empty();
    m_query = std::move(query);
    // Files come later, from the index; whatever was found for the previous text no longer applies.
    m_fileResults.clear();
    m_letters = false;
    m_historyMode = false;
    if (m_query.size() > kMaxQuery)
    {
        m_query.resize(kMaxQuery);
    }
    m_files.Request(++m_fileQuery, m_options.searchFiles ? m_query : std::wstring(),
                    (size_t)std::clamp(m_options.fileCount, 1, 12));
    if (m_query.size() > kMaxQuery)
    {
        m_query.resize(kMaxQuery);
    }
    m_selectAll = false;
    m_queryWidth = m_query.empty() ? 0 : m_fonts.Measure(m_fmtSearch.Get(), m_query.c_str(), (UINT32)m_query.size());
    m_sel = 0;
    m_scrollY = 0;
    if (m_scrollLayer)
    {
        m_scrollLayer->SetOffsetY(0.f);
    }
    BuildModel();
    UpdateScrollClip();
    RenderChrome();
    RenderContent(true);
    UpdateHighlight(false);
    m_caretOn = true;
    if (m_input && m_open)
    {
        UINT blink = GetCaretBlinkTime();
        if (blink != INFINITE)
        {
            SetTimer(m_input, kTimerCaret, blink, nullptr);
        }
    }
    UpdateCaret();
    if (wasEmpty != m_query.empty())
    {
        StartFieldTransition();
    }
    PositionIme();
    Commit();
}

void MenuView::SetSelection(int sel, bool fromKeyboard)
{
    if (m_model.items.empty())
    {
        return;
    }
    sel = std::clamp(sel, 0, (int)m_model.items.size() - 1);
    if (fromKeyboard)
    {
        EnsureVisible(sel);
    }
    if (sel != m_sel)
    {
        m_sel = sel;
        UpdateHighlight(true);
    }
    Commit();
}

void MenuView::MoveSelection(int delta)
{
    if (m_model.items.empty())
    {
        return;
    }
    // Section 8.3: stop at the ends, no wrapping.
    SetSelection(std::clamp(m_sel + delta, 0, (int)m_model.items.size() - 1), true);
}

void MenuView::MoveSpatial(int dx, int dy)
{
    if (m_model.items.empty())
    {
        return;
    }
    if (m_sel < 0 || m_sel >= (int)m_model.items.size())
    {
        SetSelection(0, true);
        return;
    }
    const D2D1_RECT_F cur = m_model.items[m_sel].rect;
    float cx = (cur.left + cur.right) / 2;
    int best = -1;
    float bestScore = FLT_MAX;
    for (int i = 0; i < (int)m_model.items.size(); ++i)
    {
        if (i == m_sel)
        {
            continue;
        }
        const D2D1_RECT_F& r = m_model.items[i].rect;
        float score;
        if (dy != 0)
        {
            // The nearest row in that direction first, then the nearest column in it.
            float gap = dy > 0 ? r.top - cur.bottom : cur.top - r.bottom;
            if (gap < -1)
            {
                continue;
            }
            score = gap * 1000 + fabsf((r.left + r.right) / 2 - cx);
        }
        else
        {
            if (r.bottom <= cur.top + 1 || r.top >= cur.bottom - 1)
            {
                continue;   // not on the same row
            }
            float gap = dx > 0 ? r.left - cur.right : cur.left - r.right;
            if (gap < -1)
            {
                continue;
            }
            score = gap;
        }
        if (score < bestScore)
        {
            bestScore = score;
            best = i;
        }
    }
    if (best >= 0)
    {
        SetSelection(best, true);
    }
}

void MenuView::EnsureVisible(int sel)
{
    const D2D1_RECT_F& r = m_model.items[sel].rect;
    float viewport = ContentHeight();
    if (r.top < m_scrollY)
    {
        ScrollTo(r.top - metric::ScrollMargin);
    }
    else if (r.bottom > m_scrollY + viewport)
    {
        ScrollTo(r.bottom + metric::ScrollMargin - viewport);
    }
}

void MenuView::ScrollTo(float y)
{
    float limit = std::max(0.f, m_model.height - ContentHeight());
    y = std::clamp(y, 0.f, limit);
    if (y == m_scrollY)
    {
        return;
    }
    m_scrollY = y;
    if (!m_scrollLayer)
    {
        return;
    }
    m_scrollLayer->SetOffsetY(-roundf(y * m_scale));
    RenderContent(false);
}

const App* MenuView::SelectedApp() const
{
    if (!m_apps || m_sel < 0 || m_sel >= (int)m_model.items.size() || m_model.items[m_sel].app < 0)
    {
        return nullptr;
    }
    return &m_apps->apps[m_model.items[m_sel].app];
}

void MenuView::SetHot(Hit hit)
{
    // Only the chrome elements have a hover look of their own; items follow the selection.
    bool chrome = hit.target == Target::Toggle || hit.target == Target::Chip || hit.target == Target::Account ||
                  hit.target == Target::Power || hit.target == Target::Shortcut || hit.target == Target::Quick;
    Hit hot = chrome ? hit : Hit{};
    int flyoutHot = hit.target == Target::FlyoutItem ? hit.index : -1;
    bool chromeChanged = hot != m_hot;
    bool flyoutChanged = m_powerOpen && flyoutHot != m_flyoutHot && (flyoutHot >= 0 || hit.target != Target::Flyout);
    m_hot = hot;
    if (chromeChanged)
    {
        RenderChrome();
    }
    if (flyoutChanged)
    {
        m_flyoutHot = flyoutHot;
        RenderFlyout();
    }
    if (chromeChanged || flyoutChanged)
    {
        Commit();
    }
}

void MenuView::SetFlyout(bool open)
{
    if (open == m_powerOpen || !m_flyout)
    {
        return;
    }
    m_powerOpen = open;
    m_flyoutHot = -1;
    RenderChrome();
    if (open)
    {
        D2D1_RECT_F box = FlyoutRect();
        RenderFlyout();
        m_flyout->SetOffsetX(roundf((metric::ShadowPadL + box.left - kFlyoutMarginX) * m_scale));
        m_flyout->SetOffsetY(roundf((metric::ShadowPadT + box.top - kFlyoutMarginTop) * m_scale));
    }
    SetVisible(m_flyout, open);
    Commit();
}

// ---------------------------------------------------------------------------------------------------------------
// Animation (8.1, 8.2)
// ---------------------------------------------------------------------------------------------------------------

void MenuView::AnimateOpen(bool opening)
{
    float s = m_scale;
    float closedY = m_options.slide ? (m_fromTop ? -1.f : 1.f) * motion::ClosedOffsetY : 0.f;
    const float openValues[3] = { 1.f, 0.f, 1.f };
    const float closedValues[3] = { 0.f, closedY, m_options.slide ? motion::ClosedScale : 1.f };
    double speed = m_options.animation == Animation::Fast ? 0.6 : 1.0;
    const double durations[3] = { motion::OpenOpacity * speed, motion::OpenTransform * speed,
                                  motion::OpenTransform * speed };
    const motion::Bezier curves[3] = { motion::EaseOut, motion::Settle, motion::Settle };

    // Where the running animation is right now, so an interrupted close turns around without a jump.
    float from[3];
    if (!m_visible)
    {
        std::copy(closedValues, closedValues + 3, from);
    }
    else
    {
        double elapsed = Seconds(m_animStart);
        const float* target = m_animOpening ? openValues : closedValues;
        for (int i = 0; i < 3; ++i)
        {
            double t = std::min(1.0, elapsed / durations[i]);
            from[i] = m_animFrom[i] + (target[i] - m_animFrom[i]) * (float)BezierY(curves[i], t);
        }
    }
    const float* to = opening ? openValues : closedValues;

    // Scale about the panel's centre, in window coordinates (the shadow margins included).
    m_openScale->SetCenterX(roundf((metric::ShadowPadL + m_lay.PanelW / 2) * s));
    m_openScale->SetCenterY(roundf((metric::ShadowPadT + m_panelH / 2) * s));

    ComPtr<IDCompositionAnimation> anims[3];
    bool animate = m_animations;
    for (int i = 0; i < 3 && animate; ++i)
    {
        float scale = i == 1 ? s : 1.f;   // translation is in pixels
        if (FAILED(m_dcomp->CreateAnimation(&anims[i])))
        {
            animate = false;
            break;
        }
        AddBezier(anims[i].Get(), from[i] * scale, to[i] * scale, durations[i], curves[i]);
    }
    // The acrylic layer lives in another composition tree: give it the same motion (its offset is in pixels too).
    float backdropFrom[3] = { from[0], from[1] * s, from[2] };
    float backdropTo[3] = { to[0], to[1] * s, to[2] };
    m_backdrop.Animate(backdropFrom, backdropTo, durations, curves, roundf((metric::ShadowPadL + m_lay.PanelW / 2) * s),
                       roundf((metric::ShadowPadT + m_panelH / 2) * s), animate);
    if (animate)
    {
        m_containerEffect->SetOpacity(anims[0].Get());
        m_openTranslate->SetOffsetY(anims[1].Get());
        m_openScale->SetScaleX(anims[2].Get());
        m_openScale->SetScaleY(anims[2].Get());
    }
    else
    {
        m_containerEffect->SetOpacity(to[0]);
        m_openTranslate->SetOffsetY(to[1] * s);
        m_openScale->SetScaleX(to[2]);
        m_openScale->SetScaleY(to[2]);
    }
    QueryPerformanceCounter(&m_animStart);
    m_animOpening = opening;
    std::copy(from, from + 3, m_animFrom);
}

void MenuView::StartPress(int item)
{
    if (item < 0 || item >= (int)m_model.items.size() || m_model.items[item].kind != ItemKind::Cell || !m_pressedSurface)
    {
        return;
    }
    m_pressedItem = item;
    const D2D1_RECT_F r = m_model.items[item].rect;
    float cellW = r.right - r.left;
    {
        Draw dc(this, m_pressedSurface.Get());
        if (dc)
        {
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, cellW, m_lay.CellH), m_lay.CellRadius,
                                                       m_lay.CellRadius),
                                     Brush(dc.dc.Get(), m_pal.Ink06));
            D2D1_MATRIX_3X2_F base;
            dc->GetTransform(&base);
            dc->SetTransform(D2D1::Matrix3x2F::Translation(-r.left, -r.top) *
                             *D2D1::Matrix3x2F::ReinterpretBaseType(&base));
            DrawItem(dc.dc.Get(), m_model.items[item], false);
        }
    }
    m_pressed->SetOffsetX(roundf(r.left * m_scale));
    m_pressed->SetOffsetY(roundf(r.top * m_scale));
    m_pressScale->SetCenterX(cellW * m_scale / 2);
    m_pressScale->SetCenterY(m_lay.CellH * m_scale / 2);
    ComPtr<IDCompositionAnimation> anim;
    if (m_animations && SUCCEEDED(m_dcomp->CreateAnimation(&anim)))
    {
        AddBezier(anim.Get(), 1.f, motion::PressScale, motion::Press, motion::Ease);
        m_pressScale->SetScaleX(anim.Get());
        m_pressScale->SetScaleY(anim.Get());
    }
    else
    {
        m_pressScale->SetScaleX(motion::PressScale);
        m_pressScale->SetScaleY(motion::PressScale);
    }
    SetVisible(m_pressed, true);
    SetVisible(m_highlight, false);
    RenderContentBand(r.top, r.bottom);   // the cell leaves the content surface while it is scaled
    KillTimer(m_input, kTimerPress);
    Commit();
}

void MenuView::EndPress(bool restoreNow)
{
    if (m_pressedItem < 0)
    {
        return;
    }
    ComPtr<IDCompositionAnimation> anim;
    if (!restoreNow && m_animations && SUCCEEDED(m_dcomp->CreateAnimation(&anim)))
    {
        AddBezier(anim.Get(), motion::PressScale, 1.f, motion::Press, motion::Ease);
        m_pressScale->SetScaleX(anim.Get());
        m_pressScale->SetScaleY(anim.Get());
        SetTimer(m_input, kTimerPress, (UINT)(motion::Press * 1000) + 10, nullptr);
        Commit();
        return;
    }
    const D2D1_RECT_F r = m_pressedItem < (int)m_model.items.size() ? m_model.items[m_pressedItem].rect
                                                                      : D2D1::RectF();
    m_pressedItem = -1;
    SetVisible(m_pressed, false);
    m_pressScale->SetScaleX(1.f);
    m_pressScale->SetScaleY(1.f);
    if (m_bandValid)
    {
        RenderContentBand(std::max(r.top, m_bandTop), std::min(r.bottom, m_bandBottom));
    }
    UpdateHighlight(false);
    Commit();
}

void MenuView::StartFieldTransition()
{
    m_fieldFrom = m_fieldT;
    m_fieldTo = m_query.empty() ? 0.f : 1.f;
    if (!m_animations)
    {
        m_fieldT = m_fieldTo;
        RenderChrome();
        return;
    }
    m_fieldStart = GetTickCount64();
    SetTimer(m_input, kTimerField, 15, nullptr);
}

// ---------------------------------------------------------------------------------------------------------------
// Open and close
// ---------------------------------------------------------------------------------------------------------------

void MenuView::Toggle(OpenSource source, HMONITOR monitor)
{
    if (source == OpenSource::MiddleClick)
    {
        static const wchar_t* const kTargets[] = { nullptr, nullptr, L"explorer.exe", L"taskmgr.exe", L"ms-settings:" };
        int action = std::clamp(m_options.middleClick, 0, 4);
        if (action == 1)
        {
            if (m_open)
            {
                Close(true);
                return;
            }
            Open(monitor);
            if (m_open && !m_showAll)
            {
                Activate({ Target::Toggle, -1 });
            }
        }
        else if (kTargets[action])
        {
            if (m_open)
            {
                Close(true, false);
            }
            AllowSetForegroundWindow(ASFW_ANY);
            OpenPath(kTargets[action]);
        }
        return;
    }
    if (m_open)
    {
        Close(true);
        return;
    }
    // The click that opens the menu again right after it closed is the click that closed it: the taskbar took
    // the activation away on mouse-down, before the toggle arrived.
    if (source == OpenSource::StartButton && GetTickCount64() - m_closedAt < 400)
    {
        return;
    }
    Open(monitor);
}

void MenuView::Open(HMONITOR monitor)
{
    LARGE_INTEGER started;
    QueryPerformanceCounter(&started);
    if (m_deviceLost || (m_d3d && FAILED(m_d3d->GetDeviceRemovedReason())))
    {
        RecoverDevice();
    }
    if (!m_dcomp && !CreateDevice())
    {
        return;
    }
    BOOL animations = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    m_animations = m_options.animation == Animation::Off      ? false
                   : m_options.animation == Animation::System ? animations != FALSE
                                                              : true;

    // The style first: the placement depends on the panel's size.
    size_t style = ApplyStyle();
    if (m_acrylic && !m_backdrop.Ready() && !m_backdrop.Create(m_visual))
    {
        m_acrylic = false;   // not available: draw the opaque background
        style ^= 0x9E3779B9u;
    }
    if (!PlaceOnMonitor(monitor))
    {
        return;
    }
    bool rebuilt = m_builtDpi != m_dpi || m_builtH != m_panelH || m_builtStyle != style;
    m_builtStyle = style;
    if (rebuilt && !BuildSurfaces())
    {
        return;
    }
    KillTimer(m_input, kTimerHide);
    bool wasVisible = m_visible;
    if (!wasVisible)
    {
        HWND foreground = GetForegroundWindow();
        m_prevForeground = foreground != m_input ? foreground : nullptr;
    }

    ResetState();
    m_pressScale->SetScaleX(1.f);
    m_pressScale->SetScaleY(1.f);
    SetVisible(m_pressed, false);
    SetVisible(m_flyout, false);
    BuildModel();
    UpdateScrollClip();
    m_scrollLayer->SetOffsetY(0.f);
    RenderChrome();
    RenderContent(true);
    UpdateHighlight(false);
    m_open = true;
    UpdateCaret();

    float s = m_scale;
    LONG winX = m_panelPx.x - (LONG)roundf(metric::ShadowPadL * s);
    LONG winY = m_panelPx.y - (LONG)roundf(metric::ShadowPadT * s);
    LONG winW = (LONG)lroundf((m_lay.PanelW + metric::ShadowPadL + metric::ShadowPadR) * s);
    LONG winH = (LONG)lroundf((m_panelH + metric::ShadowPadT + metric::ShadowPadB) * s);
    LONG panelW = (LONG)lroundf(m_lay.PanelW * s), panelH = (LONG)lroundf(m_panelH * s);

    if (!wasVisible)
    {
        // Section 8.1: closed values first, then show, then animate.
        m_containerEffect->SetOpacity(0.f);
        m_openTranslate->SetOffsetY((m_fromTop ? -1.f : 1.f) * motion::ClosedOffsetY * s);
        m_openScale->SetScaleX(motion::ClosedScale);
        m_openScale->SetScaleY(motion::ClosedScale);
        Commit();
    }
    SetWindowPos(m_visual, HWND_TOPMOST, winX, winY, winW, winH, SWP_NOACTIVATE);
    SetWindowPos(m_input, HWND_TOPMOST, m_panelPx.x, m_panelPx.y, panelW, panelH, 0);
    ShowWindow(m_visual, SW_SHOWNOACTIVATE);
    ShowWindow(m_input, SW_SHOW);
    if (!BringToForeground(m_input))
    {
        SP_LOG_INF(kTag, L"The menu could not take the foreground; it will not get the keyboard");
    }
    SetFocus(m_input);

    AnimateOpen(true);   // from the closed values, or from wherever an interrupted close has got to
    m_visible = true;
    Commit();
    lastOpenMs = Seconds(started) * 1000;
    SP_LOG_DBG(kTag, L"Opened in %.1f ms%s", lastOpenMs, rebuilt ? L" (surfaces rebuilt)" : L"");

    UINT blink = GetCaretBlinkTime();
    if (blink != INFINITE)
    {
        SetTimer(m_input, kTimerCaret, blink, nullptr);
    }
    PositionIme();
    RequestData(false);
    if (m_options.showQuick)
    {
        m_quick.Post(QuickAction::Refresh);
    }
}

void MenuView::Close(bool animate, bool restoreFocus)
{
    if (!m_open)
    {
        return;
    }
    m_open = false;
    KillTimer(m_input, kTimerCaret);
    KillTimer(m_input, kTimerField);
    SetVisible(m_caret, false);
    if (m_powerOpen)
    {
        m_powerOpen = false;
        SetVisible(m_flyout, false);
    }
    if (GetCapture() == m_input)
    {
        ReleaseCapture();
    }

    // The input window goes at once so it never catches a click meant for what is underneath.
    if (restoreFocus && GetForegroundWindow() == m_input)
    {
        if (HWND next = WindowToRestore(m_prevForeground, m_input, m_visual))
        {
            SetForegroundWindow(next);
        }
    }
    ShowWindow(m_input, SW_HIDE);

    if (animate && m_animations)
    {
        AnimateOpen(false);
        Commit();
        SetTimer(m_input, kTimerHide, (UINT)(motion::OpenTransform * 1000) + 10, nullptr);
    }
    else
    {
        Hide();
    }
}

void MenuView::Hide()
{
    KillTimer(m_input, kTimerHide);
    KillTimer(m_input, kTimerPress);
    ShowWindow(m_visual, SW_HIDE);
    m_visible = false;
    if (m_containerEffect)
    {
        m_containerEffect->SetOpacity(0.f);
    }
    m_pressedItem = -1;
    SetVisible(m_pressed, false);
    SetVisible(m_highlight, false);
    // Give back the content's video memory while the menu is closed; it is redrawn on the next opening.
    if (m_contentSurface)
    {
        m_contentSurface->Trim(nullptr, 0);
    }
    m_bandValid = false;
    Commit();
    RequestData(false);
    if (onHidden)
    {
        onHidden();
    }
}

void MenuView::SetOptions(const ViewOptions& options)
{
    if (options == m_options)
    {
        return;
    }
    bool iconsChanged = options.iconSize != m_options.iconSize || options.listIconSize != m_options.listIconSize ||
                        options.scale != m_options.scale;
    m_options = options;
    // Everything else is picked up by the next opening: ApplyStyle and, when the fingerprint moved, BuildSurfaces.
    Close(false);
    CreateFormats();
    m_builtStyle = 0;
    if (iconsChanged)
    {
        RequestData(true);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------------------------------------------

void MenuView::RequestData(bool force)
{
    UINT iconPx = (UINT)ceilf(m_lay.Icon * m_scale);
    ULONGLONG now = GetTickCount64();
    bool stale = now - m_appsRequestedAt > 60 * 1000;
    if (force || !m_apps || stale || m_apps->iconPx < iconPx)
    {
        m_appsRequestedAt = now;
        m_loader.RequestApps(std::max(iconPx, m_apps ? m_apps->iconPx : 0u), m_pins.Ids());
        if (stale && m_apps)
        {
            m_loader.RequestUser();
        }
    }
    m_loader.RequestRecent(8, m_options.forceRecent);
    UINT shortcutPx = (UINT)ceilf(kShortcutIcon * m_scale);
    if (m_options.shortcuts != m_shortcutsRequested || shortcutPx > m_shortcutsPx)
    {
        m_shortcutsRequested = m_options.shortcuts;
        m_shortcutsPx = std::max(shortcutPx, m_shortcutsPx);
        m_loader.RequestShortcuts(m_options.shortcuts, m_shortcutsPx);
    }
}

void MenuView::OnLoaded(WPARAM kind, LPARAM object)
{
    switch ((LoadResult)kind)
    {
    case LoadResult::Apps:
    {
        std::shared_ptr<AppList> list(reinterpret_cast<AppList*>(object));
        m_apps = list;
        m_newApps.Update(*list);
        m_appIndex.clear();
        for (size_t i = 0; i < list->apps.size(); ++i)
        {
            std::wstring key = list->apps[i].id;
            CharLowerBuffW(key.data(), (DWORD)key.size());
            m_appIndex.emplace(std::move(key), (int)i);
        }
        if (!m_pinsLoaded)
        {
            m_pinsLoaded = true;
            if (!m_pins.Load())
            {
                m_pins.SeedDefaults(*list);
                m_pins.Save();
                SP_LOG_INF(kTag, L"Seeded %u default pins", (unsigned)m_pins.Ids().size());
            }
        }
        if (list->iconsComplete)
        {
            // Forget the bitmaps of icons that are no longer in the list.
            std::unordered_map<const Pixels*, bool> alive;
            for (const App& app : list->apps)
            {
                alive[app.icon.get()] = true;
            }
            for (auto it = m_bitmaps.begin(); it != m_bitmaps.end();)
            {
                if (!alive.count(it->first) && it->first != m_user.picture.get())
                {
                    it = m_bitmaps.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }
        if (m_open)
        {
            int sel = m_sel;
            float scroll = m_scrollY;
            BuildModel();
            m_sel = m_model.items.empty() ? 0 : std::min(sel, (int)m_model.items.size() - 1);
            m_scrollY = -1;   // force ScrollTo to apply
            ScrollTo(scroll);
            RenderContent(true);
            UpdateHighlight(false);
            Commit();
        }
        break;
    }
    case LoadResult::Recent:
    {
        std::unique_ptr<std::vector<RecentFile>> recent(reinterpret_cast<std::vector<RecentFile>*>(object));
        bool wasVisible = RecentVisible();
        m_recent = std::move(*recent);
        if (m_open)
        {
            if (wasVisible != RecentVisible())
            {
                BuildModel();
                UpdateScrollClip();
                RenderContent(true);
            }
            RenderChrome();
            Commit();
        }
        break;
    }
    case LoadResult::Quick:
    {
        std::unique_ptr<QuickState> state(reinterpret_cast<QuickState*>(object));
        m_quickState = *state;
        m_quickKnown = true;
        if (m_open)
        {
            RenderChrome();
            Commit();
        }
        break;
    }
    case LoadResult::Files:
    {
        std::unique_ptr<FileResults> files(reinterpret_cast<FileResults*>(object));
        if (files->id == m_fileQuery && m_open && !m_query.empty())
        {
            m_fileResults = std::move(files->results);
            int sel = m_sel;
            BuildModel();
            m_sel = m_model.items.empty() ? 0 : std::min(sel, (int)m_model.items.size() - 1);
            RenderContent(true);
            UpdateHighlight(false);
            Commit();
        }
        break;
    }
    case LoadResult::Shortcuts:
    {
        std::unique_ptr<std::vector<ShortcutItem>> shortcuts(reinterpret_cast<std::vector<ShortcutItem>*>(object));
        m_shortcuts = std::move(*shortcuts);
        UpdateTooltips();
        if (m_open)
        {
            RenderChrome();
            Commit();
        }
        break;
    }
    case LoadResult::User:
    {
        std::unique_ptr<UserInfo> user(reinterpret_cast<UserInfo*>(object));
        if (m_user.picture)
        {
            m_bitmaps.erase(m_user.picture.get());
        }
        m_user = std::move(*user);
        if (m_open)
        {
            RenderChrome();
            Commit();
        }
        break;
    }
    default:
        FreeLoadResult(kind, object);
        break;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------------------------------------------

MenuView::Hit MenuView::HitTest(POINT px) const
{
    float x = px.x / m_scale, y = px.y / m_scale;
    if (m_powerOpen)
    {
        D2D1_RECT_F box = FlyoutRect();
        if (Contains(box, x, y))
        {
            for (int i = 0; i < (int)m_powerItems.size(); ++i)
            {
                if (Contains(FlyoutItemRect(i), x, y))
                {
                    return { Target::FlyoutItem, i };
                }
            }
            return { Target::Flyout, -1 };
        }
    }
    if (!m_query.empty() && Contains(ClearRect(), x, y))
    {
        return { Target::Clear, -1 };
    }
    if (SearchShown() && Contains(SearchRect(), x, y))
    {
        return { Target::Search, -1 };
    }
    if (m_query.empty() && Contains(ToggleRect(), x, y))
    {
        return { Target::Toggle, -1 };
    }
    if (y >= ContentTop() && y < ContentBottom())
    {
        float cy = y - ContentTop() + m_scrollY;
        for (size_t i = 0; i < m_model.items.size(); ++i)
        {
            if (Contains(m_model.items[i].rect, x, cy))
            {
                return { Target::Item, (int)i };
            }
        }
        if (m_showAll && m_query.empty() && !m_letters)
        {
            for (size_t i = 0; i < m_model.headings.size(); ++i)
            {
                if (Contains(m_model.headings[i].rect, x, cy))
                {
                    return { Target::Heading, (int)i };
                }
            }
        }
        return { Target::Content, -1 };
    }
    for (size_t i = 0; i < m_quickRects.size(); ++i)
    {
        if (Contains(m_quickRects[i], x, y))
        {
            return { Target::Quick, (int)i };
        }
    }
    for (size_t i = 0; i < m_chipRects.size(); ++i)
    {
        if (Contains(m_chipRects[i], x, y))
        {
            return { Target::Chip, (int)i };
        }
    }
    if (ShortcutsShown())
    {
        for (int i = 0; i < (int)m_shortcuts.size(); ++i)
        {
            if (Contains(ShortcutRect(i), x, y))
            {
                return { Target::Shortcut, i };
            }
        }
    }
    if (AccountShown() && Contains(AccountRect(), x, y))
    {
        return { Target::Account, -1 };
    }
    D2D1_RECT_F power = PowerRect();
    float dx = x - (power.left + power.right) / 2, dy = y - (power.top + power.bottom) / 2;
    if (PowerShown() && dx * dx + dy * dy <= metric::PowerBtn * metric::PowerBtn / 4)
    {
        return { Target::Power, -1 };
    }
    return {};
}

void MenuView::OnMouseMove(POINT px)
{
    if (!m_tracking)
    {
        TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, m_input, 0 };
        m_tracking = TrackMouseEvent(&track) != FALSE;
    }
    if (m_dragging || (m_pressedItem >= 0 && GetCapture() == m_input && m_query.empty() && !m_showAll &&
                       (abs(px.x - m_downPx.x) > 6 * m_scale || abs(px.y - m_downPx.y) > 6 * m_scale)))
    {
        UpdateDrag(px);
        return;
    }
    Hit hit = HitTest(px);
    if (hit.target == Target::Item && m_pressedItem < 0 && !m_powerOpen)
    {
        SetSelection(hit.index, false);
    }
    SetHot(hit);
}

// Drag and drop in the pinned grid. The picked-up cell is the pressed visual, following the cursor; the
// highlight marks where it will land; the order is written when it is dropped.
void MenuView::UpdateDrag(POINT px)
{
    if (!m_dragging)
    {
        m_dragging = true;
        m_dragFrom = m_pressedItem;
        const D2D1_RECT_F& r = m_model.items[m_dragFrom].rect;
        m_dragGrab = { m_downPx.x - (LONG)(r.left * m_scale),
                       m_downPx.y - (LONG)((ContentTop() + r.top - m_scrollY) * m_scale) };
        m_pressScale->SetScaleX(1.04f);
        m_pressScale->SetScaleY(1.04f);
    }
    // The cursor in content pixels, and the cell nearest to it.
    float cx = (float)px.x, cy = px.y + (m_scrollY - ContentTop()) * m_scale;
    m_pressed->SetOffsetX(cx - m_dragGrab.x);
    m_pressed->SetOffsetY(cy - m_dragGrab.y);
    int target = m_dragFrom;
    float best = FLT_MAX;
    for (int i = 0; i < (int)m_model.items.size(); ++i)
    {
        const Item& item = m_model.items[i];
        if (item.kind != ItemKind::Cell)
        {
            continue;
        }
        float dx = (item.rect.left + item.rect.right) / 2 * m_scale - cx;
        float dy = (item.rect.top + item.rect.bottom) / 2 * m_scale - cy;
        float distance = dx * dx + dy * dy;
        if (distance < best)
        {
            best = distance;
            target = i;
        }
    }
    if (target != m_sel)
    {
        m_sel = target;
        UpdateHighlight(true);
    }
    Commit();
}

void MenuView::FinishDrag(bool drop)
{
    int from = m_dragFrom, to = m_sel;
    m_dragging = false;
    m_dragFrom = -1;
    if (drop && m_apps && from >= 0 && to >= 0 && from != to && from < (int)m_model.items.size() &&
        to < (int)m_model.items.size())
    {
        std::wstring moving = m_apps->apps[m_model.items[from].app].id;
        std::wstring beside = m_apps->apps[m_model.items[to].app].id;
        m_pins.Unpin(moving);
        const auto& ids = m_pins.Ids();
        size_t index = 0;
        while (index < ids.size() && _wcsicmp(ids[index].c_str(), beside.c_str()) != 0)
        {
            ++index;
        }
        // Moving forward lands after the target cell, moving back lands before it: where the highlight was.
        m_pins.MoveTo(moving, from < to ? index + 1 : index);
        m_pins.Save();
    }
    m_pressedItem = -1;
    SetVisible(m_pressed, false);
    m_pressScale->SetScaleX(1.f);
    m_pressScale->SetScaleY(1.f);
    BuildModel();
    m_sel = std::clamp(to, 0, std::max(0, (int)m_model.items.size() - 1));
    RenderContent(true);
    UpdateHighlight(false);
    Commit();
}

void MenuView::OnMouseDown(POINT px)
{
    Hit hit = HitTest(px);
    if (m_powerOpen && hit.target != Target::FlyoutItem && hit.target != Target::Flyout &&
        hit.target != Target::Power)
    {
        // A click outside the power menu only closes it.
        SetFlyout(false);
        m_down = {};
        return;
    }
    m_down = hit;
    m_downPx = px;
    if (hit.target == Target::Item && m_model.items[hit.index].kind == ItemKind::Cell)
    {
        StartPress(hit.index);
    }
    SetCapture(m_input);
}

void MenuView::OnMouseUp(POINT px)
{
    Hit down = m_down;
    m_down = {};
    if (m_dragging)
    {
        m_releasingCapture = true;
        ReleaseCapture();
        m_releasingCapture = false;
        FinishDrag(true);
        return;
    }
    if (GetCapture() == m_input)
    {
        // ReleaseCapture sends WM_CAPTURECHANGED at once; that handler is for capture taken away, not given up.
        m_releasingCapture = true;
        ReleaseCapture();
        m_releasingCapture = false;
    }
    Hit hit = HitTest(px);
    bool activate = down.target != Target::None && hit == down;
    if (m_pressedItem >= 0 && !(activate && hit.target == Target::Item))
    {
        EndPress(false);   // a launch closes the menu with the cell still pressed, as it should look
    }
    if (activate)
    {
        Activate(hit);
    }
}

void MenuView::Activate(Hit hit)
{
    switch (hit.target)
    {
    case Target::Clear:
        SetQuery(L"");
        break;
    case Target::Toggle:
        m_showAll = !m_showAll;
        m_sel = 0;
        m_scrollY = 0;
        if (m_scrollLayer)
        {
            m_scrollLayer->SetOffsetY(0.f);
        }
        BuildModel();
        UpdateScrollClip();
        RenderChrome();
        RenderContent(true);
        UpdateHighlight(false);
        Commit();
        break;
    case Target::Item:
        LaunchSelected(hit.index);
        break;
    case Target::Chip:
        if (hit.index >= 0 && hit.index < (int)m_recent.size())
        {
            std::wstring path = m_recent[hit.index].path;
            Close(true, false);
            AllowSetForegroundWindow(ASFW_ANY);
            OpenPath(path);
        }
        break;
    case Target::Heading:
        m_letters = true;
        m_sel = 0;
        m_scrollY = 0;
        m_scrollLayer->SetOffsetY(0.f);
        BuildModel();
        RenderContent(true);
        UpdateHighlight(false);
        Commit();
        break;
    case Target::Shortcut:
        if (hit.index >= 0 && hit.index < (int)m_shortcuts.size())
        {
            IdListPtr pidl = m_shortcuts[hit.index].pidl;
            Close(true, false);
            AllowSetForegroundWindow(ASFW_ANY);
            OpenIdList(pidl->value);
        }
        break;
    case Target::Account:
        Close(true, false);
        AllowSetForegroundWindow(ASFW_ANY);
        OpenAccountSettings();
        break;
    case Target::Power:
        SetFlyout(!m_powerOpen);
        break;
    case Target::Search:
        if (m_selectAll)
        {
            m_selectAll = false;
            RenderChrome();
            Commit();
        }
        else if (m_query.empty() && m_options.searchHistory && (m_historyMode || !m_history.Lines().empty()))
        {
            // A click in the empty box shows the recent searches; a second click goes back.
            m_historyMode = !m_historyMode;
            m_showAll = false;
            m_sel = 0;
            m_scrollY = 0;
            m_scrollLayer->SetOffsetY(0.f);
            BuildModel();
            UpdateScrollClip();
            RenderChrome();
            RenderContent(true);
            UpdateHighlight(false);
            Commit();
        }
        break;
    case Target::Quick:
        if (hit.index >= 0 && hit.index < (int)m_quickKinds.size())
        {
            RunQuick(m_quickKinds[hit.index], 0);
        }
        break;
    case Target::FlyoutItem:
        RunPower(hit.index);
        break;
    default:
        break;
    }
}

void MenuView::LaunchSelected(int item)
{
    if (!m_apps || item < 0 || item >= (int)m_model.items.size())
    {
        return;
    }
    bool admin = GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_SHIFT) < 0;
    const Item& chosen = m_model.items[item];
    if (chosen.kind == ItemKind::Letter)
    {
        JumpToLetter(m_letterList[chosen.result]);
        return;
    }
    if (chosen.kind == ItemKind::Result && m_model.results[chosen.result].kind == ResultKind::History)
    {
        SetQuery(m_model.results[chosen.result].target);
        return;
    }
    if (!m_query.empty() && m_options.searchHistory)
    {
        m_history.Add(m_query, 20);   // what was searched for, when it led somewhere
    }
    if (chosen.kind == ItemKind::Result)
    {
        Result result = m_model.results[chosen.result];
        if (result.kind == ResultKind::Calc)
        {
            // Enter copies the value; the menu closes as after any other choice.
            if (OpenClipboard(m_input))
            {
                EmptyClipboard();
                size_t bytes = (result.target.size() + 1) * sizeof(wchar_t);
                if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes))
                {
                    memcpy(GlobalLock(memory), result.target.c_str(), bytes);
                    GlobalUnlock(memory);
                    if (!SetClipboardData(CF_UNICODETEXT, memory))
                    {
                        GlobalFree(memory);
                    }
                }
                CloseClipboard();
            }
            Close(true);
            return;
        }
        Close(true, false);
        ExecuteResult(result, admin);
        return;
    }
    // Keep the app alive past a list refresh that might arrive while the shell starts it.
    std::shared_ptr<AppList> apps = m_apps;
    const App& app = apps->apps[chosen.app];
    m_usage.Record(app.id);
    m_newApps.Clear(app.id);
    Close(true, false);
    AllowSetForegroundWindow(ASFW_ANY);
    LaunchApp(app, admin);
}

void MenuView::RunQuick(int which, int volumeSteps)
{
    // Show the change at once; the quick-settings thread reports the real state right after.
    switch (which)
    {
    case 0:
        m_quickState.wifiOn = !m_quickState.wifiOn;
        m_quick.Post(QuickAction::ToggleWifi);
        break;
    case 1:
        m_quickState.bluetoothOn = !m_quickState.bluetoothOn;
        m_quick.Post(QuickAction::ToggleBluetooth);
        break;
    case 2:
        if (volumeSteps)
        {
            m_quickState.volume = std::clamp(m_quickState.volume + volumeSteps, 0, 100);
            m_quickState.muted = m_quickState.volume == 0 && m_quickState.muted;
            m_quick.Post(QuickAction::Volume, volumeSteps);
        }
        else
        {
            m_quickState.muted = !m_quickState.muted;
            m_quick.Post(QuickAction::ToggleMute);
        }
        break;
    case 3:
        m_quickState.dark = !m_quickState.dark;
        m_quick.Post(QuickAction::ToggleDark);
        break;
    }
    RenderChrome();
    Commit();
}

bool MenuView::IsHidden(int app)
{
    return app >= 0 && m_apps && m_hidden.Contains(m_apps->apps[app].id);
}

void MenuView::JumpToLetter(const std::wstring& letter)
{
    m_letters = false;
    BuildModel();
    for (const Heading& heading : m_model.headings)
    {
        if (heading.text == letter)
        {
            m_scrollY = -1;
            ScrollTo(heading.rect.top);
            for (int i = 0; i < (int)m_model.items.size(); ++i)
            {
                if (m_model.items[i].rect.top >= heading.rect.bottom - 1)
                {
                    m_sel = i;
                    break;
                }
            }
            break;
        }
    }
    RenderContent(true);
    UpdateHighlight(false);
    Commit();
}

void MenuView::RunPower(int index)
{
    if (index < 0 || index >= (int)m_powerItems.size())
    {
        return;
    }
    PowerAction action = m_powerItems[index];
    Close(true, false);
    RunPowerAction(action);
}

void MenuView::OnRightClick(POINT px, bool fromKeyboard)
{
    Hit hit;
    if (fromKeyboard)
    {
        if (m_sel >= 0 && m_sel < (int)m_model.items.size())
        {
            hit = { Target::Item, m_sel };
            const D2D1_RECT_F& r = m_model.items[m_sel].rect;
            px.x = (LONG)((r.left + r.right) / 2 * m_scale);
            px.y = (LONG)((ContentTop() + r.bottom - m_scrollY) * m_scale);
        }
    }
    else
    {
        hit = HitTest(px);
    }
    POINT screen = px;
    ClientToScreen(m_input, &screen);

    if (hit.target == Target::Item && m_apps && m_model.items[hit.index].app >= 0)
    {
        SetSelection(hit.index, false);
        std::shared_ptr<AppList> apps = m_apps;
        const App& app = apps->apps[m_model.items[hit.index].app];
        enum : UINT { kPin = 1, kUnpin, kFront, kHide, kRecentFirst = 16 };
        std::vector<MenuExtra> extras;
        // The app's recent documents first, as its jump list on the taskbar has them.
        std::vector<JumpItem> recent = RecentItemsOf(app.id, 8);
        if (!recent.empty())
        {
            extras.push_back({ 0, Text(Str::JumpRecent), MF_GRAYED });
            for (size_t i = 0; i < recent.size(); ++i)
            {
                extras.push_back({ kRecentFirst + (UINT)i, recent[i].name.c_str() });
            }
            extras.push_back({ 0, nullptr });
        }
        bool pinned = m_pins.IsPinned(app.id);
        if (pinned)
        {
            extras.push_back({ kUnpin, Text(Str::UnpinFromStart) });
            if (!m_pins.Ids().empty() && _wcsicmp(m_pins.Ids().front().c_str(), app.id.c_str()) != 0)
            {
                extras.push_back({ kFront, Text(Str::MoveToFront) });
            }
        }
        else
        {
            extras.push_back({ kPin, Text(Str::PinToStart) });
        }
        extras.push_back({ kHide, Text(Str::HideApp) });
        m_inContextMenu = true;
        int chosen = ShowItemContextMenu(m_input, screen, app.pidl->value, extras);
        m_inContextMenu = false;
        if (chosen >= kRecentFirst && chosen < kRecentFirst + (int)recent.size())
        {
            IdListPtr pidl = recent[chosen - kRecentFirst].pidl;
            Close(true, false);
            AllowSetForegroundWindow(ASFW_ANY);
            OpenIdList(pidl->value);
        }
        else if (chosen > 0)
        {
            std::wstring id = app.id;
            if (chosen == kPin) m_pins.Pin(id);
            if (chosen == kUnpin) m_pins.Unpin(id);
            if (chosen == kFront) m_pins.MoveToFront(id);
            if (chosen == kHide) m_hidden.Add(id, 1000);
            m_pins.Save();
            if (m_open)
            {
                BuildModel();
                m_sel = m_model.items.empty() ? 0 : std::min(m_sel, (int)m_model.items.size() - 1);
                RenderContent(true);
                UpdateHighlight(false);
                Commit();
            }
        }
        else if (chosen == 0)
        {
            Close(true, false);
        }
        else if (m_open && GetForegroundWindow() != m_input)
        {
            Close(true, false);
        }
    }
    else if (hit.target == Target::Chip && hit.index < (int)m_recent.size())
    {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(m_recent[hit.index].path.c_str(), nullptr, &pidl, 0, nullptr)))
        {
            m_inContextMenu = true;
            int chosen = ShowItemContextMenu(m_input, screen, pidl, {});
            m_inContextMenu = false;
            CoTaskMemFree(pidl);
            if (chosen == 0)
            {
                Close(true, false);
            }
        }
    }
}

void MenuView::OnWheel(int delta)
{
    if (m_powerOpen)
    {
        return;
    }
    POINT over;
    if (GetCursorPos(&over) && ScreenToClient(m_input, &over))
    {
        Hit hit = HitTest(over);
        if (hit.target == Target::Quick && hit.index < (int)m_quickKinds.size() && m_quickKinds[hit.index] == 2)
        {
            RunQuick(2, delta > 0 ? 2 : -2);
            return;
        }
    }
    ScrollTo(m_scrollY - delta * metric::WheelStep / WHEEL_DELTA);
    // The content moved under a still cursor: select what is under it now, as a browser updates :hover.
    POINT cursor;
    if (GetCursorPos(&cursor) && ScreenToClient(m_input, &cursor))
    {
        Hit hit = HitTest(cursor);
        if (hit.target == Target::Item)
        {
            SetSelection(hit.index, false);
        }
    }
    Commit();
}

bool MenuView::OnKeyDown(WPARAM key)
{
    bool ctrl = GetKeyState(VK_CONTROL) < 0;
    bool alt = GetKeyState(VK_MENU) < 0;
    int cols = m_model.grid ? m_lay.GridCols : 1;
    switch (key)
    {
    case VK_ESCAPE:
        // Section 8.3: the first of these that applies.
        if (m_dragging)
        {
            ReleaseCapture();   // WM_CAPTURECHANGED cancels the drag
        }
        else if (m_historyMode)
        {
            Activate({ Target::Search, -1 });
        }
        else if (m_letters)
        {
            m_letters = false;
            BuildModel();
            RenderContent(true);
            UpdateHighlight(false);
            Commit();
        }
        else if (m_powerOpen)
        {
            SetFlyout(false);
        }
        else if (!m_query.empty())
        {
            SetQuery(L"");
        }
        else if (m_showAll)
        {
            Activate({ Target::Toggle, -1 });
        }
        else
        {
            Close(true);
        }
        return true;
    case VK_LEFT:
    case VK_RIGHT:
        if (!m_powerOpen)
        {
            MoveSpatial(key == VK_LEFT ? -1 : 1, 0);
        }
        return true;
    case VK_UP:
    case VK_DOWN:
        if (m_powerOpen)
        {
            int last = (int)m_powerItems.size() - 1;
            int hot = m_flyoutHot < 0 ? (key == VK_UP ? last : 0)
                                      : std::clamp(m_flyoutHot + (key == VK_UP ? -1 : 1), 0, last);
            m_flyoutHot = hot;
            RenderFlyout();
            Commit();
        }
        else
        {
            MoveSpatial(0, key == VK_UP ? -1 : 1);
        }
        return true;
    case VK_PRIOR:
    case VK_NEXT:
        if (!m_powerOpen && !m_model.items.empty())
        {
            float rowH = m_model.grid ? m_lay.CellH + metric::GridGap : m_lay.ListRowH;
            int rows = std::max(1, (int)(ContentHeight() / rowH));
            MoveSelection((key == VK_PRIOR ? -1 : 1) * rows * cols);
        }
        return true;
    case VK_HOME:
    case VK_END:
        if (!m_powerOpen && !m_model.items.empty())
        {
            SetSelection(key == VK_HOME ? 0 : (int)m_model.items.size() - 1, true);
        }
        return true;
    case VK_RETURN:
        if (m_powerOpen)
        {
            if (m_flyoutHot >= 0)
            {
                RunPower(m_flyoutHot);
            }
        }
        else if (!m_model.items.empty())
        {
            LaunchSelected(m_sel);
        }
        return true;
    case VK_BACK:
        if (m_query.empty())
        {
            return true;
        }
        if (m_selectAll)
        {
            SetQuery(L"");
        }
        else if (ctrl)
        {
            // Delete the last word and the spaces after it.
            std::wstring q = m_query;
            while (!q.empty() && iswspace(q.back())) q.pop_back();
            while (!q.empty() && !iswspace(q.back())) q.pop_back();
            SetQuery(q);
        }
        else
        {
            std::wstring q = m_query;
            q.pop_back();
            if (!q.empty() && IS_HIGH_SURROGATE(q.back()))
            {
                q.pop_back();
            }
            SetQuery(q);
        }
        return true;
    case VK_TAB:
        return true;
    }
    if (ctrl && !alt)
    {
        switch (key)
        {
        case 'A':
            m_selectAll = !m_query.empty();
            RenderChrome();
            Commit();
            return true;
        case 'C':
        case 'X':
            if (m_selectAll && OpenClipboard(m_input))
            {
                EmptyClipboard();
                size_t bytes = (m_query.size() + 1) * sizeof(wchar_t);
                if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes))
                {
                    memcpy(GlobalLock(memory), m_query.c_str(), bytes);
                    GlobalUnlock(memory);
                    if (!SetClipboardData(CF_UNICODETEXT, memory))
                    {
                        GlobalFree(memory);
                    }
                }
                CloseClipboard();
                if (key == 'X')
                {
                    SetQuery(L"");
                }
            }
            return true;
        case 'V':
            if (IsClipboardFormatAvailable(CF_UNICODETEXT) && OpenClipboard(m_input))
            {
                std::wstring pasted;
                if (HANDLE data = GetClipboardData(CF_UNICODETEXT))
                {
                    if (const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(data)))
                    {
                        for (const wchar_t* p = text; *p && pasted.size() < 200; ++p)
                        {
                            pasted.push_back(*p < 0x20 ? L' ' : *p);   // one line
                        }
                        GlobalUnlock(data);
                    }
                }
                CloseClipboard();
                if (!pasted.empty())
                {
                    SetQuery(m_selectAll ? pasted : m_query + pasted);
                }
            }
            return true;
        }
    }
    return false;
}

void MenuView::OnChar(wchar_t c)
{
    if (c < 0x20 || c == 0x7F)
    {
        return;   // Backspace, Enter, Esc and Ctrl+letter arrive here too; OnKeyDown has handled them
    }
    if (m_powerOpen)
    {
        SetFlyout(false);
    }
    if (m_query.size() >= kMaxQuery)
    {
        return;
    }
    SetQuery(m_selectAll ? std::wstring(1, c) : m_query + c);
}

void MenuView::PositionIme()
{
    if (!m_input || !m_open)
    {
        return;
    }
    HIMC imc = ImmGetContext(m_input);
    if (!imc)
    {
        return;
    }
    D2D1_RECT_F box = SearchRect();
    float x = box.left + metric::SearchTextX + m_queryWidth;
    COMPOSITIONFORM form = { CFS_POINT };
    form.ptCurrentPos = { (LONG)(x * m_scale), (LONG)(box.top * m_scale + 10 * m_scale) };
    ImmSetCompositionWindow(imc, &form);
    CANDIDATEFORM candidate = { 0, CFS_CANDIDATEPOS };
    candidate.ptCurrentPos = { (LONG)(x * m_scale), (LONG)(box.bottom * m_scale) };
    ImmSetCandidateWindow(imc, &candidate);
    ImmReleaseContext(m_input, imc);
}

// ---------------------------------------------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------------------------------------------

LRESULT CALLBACK MenuView::InputProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    MenuView* view = reinterpret_cast<MenuView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        view = static_cast<MenuView*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(view));
        view->m_input = hwnd;
    }
    if (!view)
    {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    LRESULT result = view->HandleMessage(message, wParam, lParam);
    if (view->m_deviceLost && view->m_input)
    {
        view->RecoverDevice();
    }
    return result;
}

LRESULT MenuView::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    LRESULT menuResult = 0;
    bool menuMessage = message == WM_INITMENUPOPUP || message == WM_DRAWITEM || message == WM_MEASUREITEM ||
                       message == WM_MENUCHAR;
    if (m_inContextMenu && menuMessage && HandleContextMenuMessage(message, wParam, lParam, &menuResult))
    {
        return menuResult;
    }
    switch (message)
    {
    case kMsgToggle:
        Toggle((OpenSource)wParam, reinterpret_cast<HMONITOR>(lParam));
        return 0;
    case kMsgClose:
        Close(false);
        return 0;
    case kMsgCommand:
        if (wParam == kCommandShowHidden)
        {
            m_hidden.Clear();
        }
        else if (wParam == kCommandClearHistory)
        {
            m_history.Clear();
            m_historyMode = false;
        }
        if (m_open)
        {
            BuildModel();
            RenderContent(true);
            UpdateHighlight(false);
            Commit();
        }
        return 0;
    case kMsgResetPins:
        if (m_apps)
        {
            m_pins.SeedDefaults(*m_apps);
            m_pins.Save();
            m_pinsLoaded = true;
            if (m_open && m_query.empty() && !m_showAll)
            {
                BuildModel();
                m_sel = 0;
                RenderContent(true);
                UpdateHighlight(false);
                Commit();
            }
        }
        else
        {
            PinStore::Delete();
            m_pinsLoaded = false;   // seeded when the app list arrives
        }
        SP_LOG_INF(kTag, L"Pinned apps reset to the defaults");
        return 0;
    case kMsgSetOptions:
    {
        std::unique_ptr<ViewOptions> options(reinterpret_cast<ViewOptions*>(lParam));
        if (options)
        {
            SetOptions(*options);
        }
        return 0;
    }
    case kMsgLoaded:
        OnLoaded(wParam, lParam);
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE && m_open && !m_inContextMenu)
        {
            m_closedAt = GetTickCount64();
            Close(true, false);
        }
        return 0;
    case WM_CLOSE:
        Close(true);
        return 0;

    case WM_TIMER:
        switch (wParam)
        {
        case kTimerHide:
            Hide();
            break;
        case kTimerCaret:
            m_caretOn = !m_caretOn;
            SetVisible(m_caret, m_open && m_caretOn && SearchShown());
            Commit();
            break;
        case kTimerField:
        {
            float t = std::min(1.f, (GetTickCount64() - m_fieldStart) / (float)(motion::FieldBorder * 1000));
            m_fieldT = m_fieldFrom + (m_fieldTo - m_fieldFrom) * t;
            if (t >= 1)
            {
                KillTimer(m_input, kTimerField);
            }
            RenderChrome();
            Commit();
            break;
        }
        case kTimerPress:
            KillTimer(m_input, kTimerPress);
            EndPress(true);
            break;
        }
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT)
        {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(m_input, &p);
            Hit hit = HitTest(p);
            SetCursor(LoadCursorW(nullptr, hit.target == Target::Search ? IDC_IBEAM : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE:
        OnMouseMove({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
        return 0;
    case WM_MOUSELEAVE:
        m_tracking = false;
        SetHot({});
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        OnMouseDown({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
        return 0;
    case WM_LBUTTONUP:
        OnMouseUp({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
        return 0;
    case WM_CAPTURECHANGED:
        if (m_dragging && (HWND)lParam != m_input && !m_releasingCapture)
        {
            m_down = {};
            FinishDrag(false);
        }
        else if (m_pressedItem >= 0 && (HWND)lParam != m_input && !m_releasingCapture)
        {
            m_down = {};
            EndPress(false);
        }
        return 0;
    case WM_RBUTTONUP:
        OnRightClick({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }, false);
        return 0;
    case WM_CONTEXTMENU:
        if (lParam == -1)   // Shift+F10 or the menu key
        {
            OnRightClick({}, true);
        }
        return 0;
    case WM_MOUSEWHEEL:
        OnWheel(GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;

    case WM_KEYDOWN:
        if (OnKeyDown(wParam))
        {
            return 0;
        }
        break;
    case WM_CHAR:
        OnChar((wchar_t)wParam);
        return 0;
    case WM_IME_STARTCOMPOSITION:
        PositionIme();
        break;

    case WM_DPICHANGED:
        return 0;   // the window is placed explicitly on every opening
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        if (message == WM_DISPLAYCHANGE || wParam == SPI_SETWORKAREA)
        {
            Close(false);
        }
        break;
    }
    return DefWindowProcW(m_input, message, wParam, lParam);
}

// ---------------------------------------------------------------------------------------------------------------
// Preview
// ---------------------------------------------------------------------------------------------------------------

int RunPreview(const char* options)
{
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HRESULT ole = OleInitialize(nullptr);
    std::string args = options ? options : "";
    ViewOptions viewOptions = ReadHostSettings().view;
    auto has = [&](const char* word) { return args.find(word) != std::string::npos; };
    auto number = [&](const char* key, int fallback) {
        size_t at = args.find(key);
        return at == std::string::npos ? fallback : atoi(args.c_str() + at + strlen(key));
    };
    if (has("left")) viewOptions.placement = Placement::Left;
    if (has("dark")) viewOptions.theme = ThemeMode::Dark;
    if (has("light")) viewOptions.theme = ThemeMode::Light;
    if (has("acrylic")) viewOptions.background = Background::Acrylic;
    if (has("opaque")) viewOptions.background = Background::Opaque;
    if (has("nolabels")) viewOptions.labels = false;
    viewOptions.columns = number("cols=", viewOptions.columns);
    viewOptions.iconSize = (float)number("icon=", (int)viewOptions.iconSize);
    viewOptions.accent = number("accent=", viewOptions.accent);
    viewOptions.cornerRadius = (float)number("radius=", (int)viewOptions.cornerRadius);
    viewOptions.opacity = number("opacity=", viewOptions.opacity);
    viewOptions.theme = (ThemeMode)std::clamp(number("theme=", (int)viewOptions.theme), 0, 5);
    viewOptions.background = (Background)std::clamp(number("bg=", (int)viewOptions.background), 0, 2);
    viewOptions.fontScale = number("fontsize=", viewOptions.fontScale);
    if (has("nosearch")) viewOptions.showSearch = false;
    if (has("notitle")) viewOptions.showTitle = false;
    if (has("nofooter")) viewOptions.showFooter = false;
    if (has("noname")) viewOptions.showUserName = false;
    viewOptions.forceRecent = args.find("recent") != std::string::npos;
    int result = 1;
    {
        MenuView view;
        if (view.Create(viewOptions))
        {
            view.onHidden = [] { PostQuitMessage(0); };
            // Give the loader a moment so the first frame has the apps, as it does in the shell.
            ULONGLONG until = GetTickCount64() + 1500;
            MSG msg;
            while (GetTickCount64() < until)
            {
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
                {
                    DispatchMessageW(&msg);
                }
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
            }
            view.Open(nullptr);
            double firstOpen = view.lastOpenMs;
            if (args.find("bench") != std::string::npos)
            {
                // A warm opening, the case that matters: close at once and open again.
                auto hidden = std::move(view.onHidden);
                view.Close(false);
                view.Open(nullptr);
                view.onHidden = std::move(hidden);
            }
            wchar_t logPath[MAX_PATH];
            if (GetTempPathW(MAX_PATH, logPath))
            {
                wcscat_s(logPath, L"ShadePatcher-startmenu-preview.txt");
                if (FILE* log = _wfopen(logPath, L"w"))
                {
                    fprintf(log, "first open %.2f ms, last open %.2f ms\n", firstOpen, view.lastOpenMs);
                    fclose(log);
                }
            }
            size_t search = args.find("search=");
            if (search != std::string::npos)
            {
                std::string text = args.substr(search + 7);
                text = text.substr(0, text.find(' '));
                for (char c : text)
                {
                    SendMessageW(view.Window(), WM_CHAR, (WPARAM)(unsigned char)c, 0);
                }
            }
            if (args.find("all") != std::string::npos)
            {
                // Same as clicking "All": press the toggle through the normal path.
                float s = GetDpiForWindow(view.Window()) / 96.f;
                RECT client;
                GetClientRect(view.Window(), &client);
                LPARAM at = MAKELPARAM(client.right - (int)((B + metric::HeaderPadX + 20) * s),
                                       (int)((B + (viewOptions.showSearch ? metric::HeaderY : 8.f) + metric::HeaderPadT + 14) * s));
                SendMessageW(view.Window(), WM_LBUTTONDOWN, MK_LBUTTON, at);
                SendMessageW(view.Window(), WM_LBUTTONUP, 0, at);
            }
            if (args.find("power") != std::string::npos)
            {
                RECT r;
                GetClientRect(view.Window(), &r);
                float s = GetDpiForWindow(view.Window()) / 96.f;
                LPARAM at = MAKELPARAM(r.right - (int)((B + metric::FooterPadR + 20) * s),
                                       r.bottom - (int)((B + 30) * s));
                SendMessageW(view.Window(), WM_LBUTTONDOWN, MK_LBUTTON, at);
                SendMessageW(view.Window(), WM_LBUTTONUP, 0, at);
            }
            while (GetMessageW(&msg, nullptr, 0, 0) > 0)
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            result = 0;
        }
        view.Destroy();
    }
    if (SUCCEEDED(ole))
    {
        OleUninitialize();
    }
    return result;
}

} // namespace sm

extern "C" int SP_StartMenuPreview(const char* options)
{
    return sm::RunPreview(options);
}
