#include <initguid.h>
DEFINE_GUID(LiveSetting_Property_GUID, 0xc12bcd8e, 0x2a8e, 0x4950, 0x8a, 0xe7, 0x36, 0x25, 0x11, 0x1d, 0x58, 0xeb);
#include <oleacc.h>
#include "GUI.h"

#pragma region "ShadePatcher toggle switch"
// Minimal GDI+ flat API declarations (gdiplus.h is C++ only). Used to draw anti-aliased toggle switches
// into a 32bpp premultiplied DIB that is then alpha blended onto the (possibly transparent) paint buffer.
typedef struct EP_GpGraphics EP_GpGraphics;
typedef struct EP_GpBitmap EP_GpBitmap;
typedef struct EP_GpBrush EP_GpBrush;
typedef struct EP_GpPen EP_GpPen;
typedef struct EP_GpPath EP_GpPath;
typedef struct _EP_GdiplusStartupInput
{
    UINT32 GdiplusVersion;
    void* DebugEventCallback;
    BOOL SuppressBackgroundThread;
    BOOL SuppressExternalCodecs;
} EP_GdiplusStartupInput;
#define EP_PixelFormat32bppPARGB 0x000E200B
#define EP_SmoothingModeAntiAlias 4
#define EP_UnitPixel 2
int WINAPI GdiplusStartup(ULONG_PTR* token, const EP_GdiplusStartupInput* input, void* output);
int WINAPI GdipCreateBitmapFromScan0(INT width, INT height, INT stride, INT format, BYTE* scan0, EP_GpBitmap** bitmap);
int WINAPI GdipGetImageGraphicsContext(EP_GpBitmap* image, EP_GpGraphics** graphics);
int WINAPI GdipSetSmoothingMode(EP_GpGraphics* graphics, int smoothingMode);
int WINAPI GdipCreateSolidFill(DWORD color, EP_GpBrush** brush);
int WINAPI GdipCreatePen1(DWORD color, float width, int unit, EP_GpPen** pen);
int WINAPI GdipCreatePath(int brushMode, EP_GpPath** path);
int WINAPI GdipAddPathArc(EP_GpPath* path, float x, float y, float width, float height, float startAngle, float sweepAngle);
int WINAPI GdipClosePathFigure(EP_GpPath* path);
int WINAPI GdipFillPath(EP_GpGraphics* graphics, EP_GpBrush* brush, EP_GpPath* path);
int WINAPI GdipDrawPath(EP_GpGraphics* graphics, EP_GpPen* pen, EP_GpPath* path);
int WINAPI GdipFillEllipse(EP_GpGraphics* graphics, EP_GpBrush* brush, float x, float y, float width, float height);
int WINAPI GdipDrawEllipse(EP_GpGraphics* graphics, EP_GpPen* pen, float x, float y, float width, float height);
int WINAPI GdipDrawLine(EP_GpGraphics* graphics, EP_GpPen* pen, float x1, float y1, float x2, float y2);
int WINAPI GdipSetPenStartCap(EP_GpPen* pen, int startCap);
int WINAPI GdipSetPenEndCap(EP_GpPen* pen, int endCap);
int WINAPI GdipDeletePath(EP_GpPath* path);
int WINAPI GdipDeleteBrush(EP_GpBrush* brush);
int WINAPI GdipDeletePen(EP_GpPen* pen);
int WINAPI GdipDeleteGraphics(EP_GpGraphics* graphics);
int WINAPI GdipDisposeImage(EP_GpBitmap* image);
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "Msimg32.lib")

static DWORD GUI_ColorRefToArgb(COLORREF cr)
{
    return 0xFF000000 | ((DWORD)GetRValue(cr) << 16) | ((DWORD)GetGValue(cr) << 8) | (DWORD)GetBValue(cr);
}

// Fills a rectangle with a (non premultiplied) ARGB color through AlphaBlend, so that it also works on the
// transparent (Mica) background of the window.
static void GUI_FillRectAlpha(HDC hdc, const RECT* prc, DWORD argb)
{
    int w = prc->right - prc->left, h = prc->bottom - prc->top;
    if (w <= 0 || h <= 0 || w > 4096 || h > 16384)
    {
        return;
    }
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = 1;
    bi.bmiHeader.biHeight = 1;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    DWORD* pBits = NULL;
    HBITMAP hBitmap = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, (void**)&pBits, NULL, 0);
    if (!hBitmap || !pBits)
    {
        if (hBitmap) DeleteObject(hBitmap);
        return;
    }
    DWORD a = argb >> 24;
    *pBits = (a << 24) | ((((argb >> 16) & 0xFF) * a / 255) << 16) | ((((argb >> 8) & 0xFF) * a / 255) << 8) | ((argb & 0xFF) * a / 255);
    HDC hdcMem = CreateCompatibleDC(hdc);
    if (hdcMem)
    {
        HGDIOBJ hOld = SelectObject(hdcMem, hBitmap);
        BLENDFUNCTION bf;
        bf.BlendOp = AC_SRC_OVER;
        bf.BlendFlags = 0;
        bf.SourceConstantAlpha = 255;
        bf.AlphaFormat = AC_SRC_ALPHA;
        AlphaBlend(hdc, prc->left, prc->top, w, h, hdcMem, 0, 0, 1, 1, bf);
        SelectObject(hdcMem, hOld);
        DeleteDC(hdcMem);
    }
    DeleteObject(hBitmap);
}

// Draws a minimal "line and dot" switch at the left edge of the line rectangle, vertically centered: a thin rail
// with a hollow ring at its left end when off, and a filled accent colored dot at its right end when on.
// Returns the horizontal space (in pixels) taken by the switch plus a gap, or 0 when nothing was drawn.
static int GUI_DrawToggleSwitch(HDC hdc, const RECT* prcLine, BOOL bOn, BOOL bFocused, BOOL bDark, double dx, double dy)
{
    static ULONG_PTR gdiplusToken = 0;
    if (!gdiplusToken)
    {
        EP_GdiplusStartupInput input;
        ZeroMemory(&input, sizeof(input));
        input.GdiplusVersion = 1;
        if (GdiplusStartup(&gdiplusToken, &input, NULL) != 0)
        {
            gdiplusToken = 0;
            return 0;
        }
    }

    int w = (int)(30 * dx + 0.5), h = (int)(16 * dy + 0.5);
    if (w < 20) w = 20;
    if (h < 10) h = 10;
    int cxLine = prcLine->right - prcLine->left, cyLine = prcLine->bottom - prcLine->top;
    if (cxLine < w || cyLine < h)
    {
        return 0;
    }
    int x = prcLine->left, y = prcLine->top + (cyLine - h) / 2;

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* pBits = NULL;
    HBITMAP hBitmap = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &pBits, NULL, 0);
    if (!hBitmap || !pBits)
    {
        if (hBitmap) DeleteObject(hBitmap);
        return 0;
    }
    ZeroMemory(pBits, (size_t)w * h * 4);

    BOOL bDrawn = FALSE;
    EP_GpBitmap* pBitmap = NULL;
    EP_GpGraphics* pGraphics = NULL;
    if (GdipCreateBitmapFromScan0(w, h, w * 4, EP_PixelFormat32bppPARGB, (BYTE*)pBits, &pBitmap) == 0 && pBitmap &&
        GdipGetImageGraphicsContext(pBitmap, &pGraphics) == 0 && pGraphics)
    {
        GdipSetSmoothingMode(pGraphics, EP_SmoothingModeAntiAlias);

        COLORREF crAccent = RGB(0, 120, 212);
        DWORD dwColorization = 0;
        BOOL bOpaque = FALSE;
        if (SUCCEEDED(DwmGetColorizationColor(&dwColorization, &bOpaque)))
        {
            crAccent = RGB((dwColorization >> 16) & 0xFF, (dwColorization >> 8) & 0xFF, dwColorization & 0xFF);
        }
        DWORD argbAccent = GUI_ColorRefToArgb(crAccent);
        DWORD argbRail = bDark ? 0xFF5C5C5C : 0xFFB4B4B4;
        DWORD argbRing = bDark ? 0xFF9E9E9E : 0xFF6E6E6E;

        float cy = (float)h / 2.0f;
        float stroke = (float)h / 8.0f;
        if (stroke < 1.5f) stroke = 1.5f;
        float kd = (float)h * 0.62f;                 // knob diameter
        float inset = ((float)h - kd) / 2.0f;        // keeps room for the focus ring
        float kx = bOn ? ((float)w - inset - kd) : inset;
        float railLeft = inset + kd / 2.0f, railRight = (float)w - inset - kd / 2.0f;

        // Rail: the part that is not covered by the knob; accent colored when on.
        EP_GpPen* pRailPen = NULL;
        if (GdipCreatePen1(bOn ? argbAccent : argbRail, stroke, EP_UnitPixel, &pRailPen) == 0 && pRailPen)
        {
            GdipSetPenStartCap(pRailPen, 2);
            GdipSetPenEndCap(pRailPen, 2);
            if (bOn)
            {
                GdipDrawLine(pGraphics, pRailPen, railLeft, cy, kx, cy);
            }
            else
            {
                GdipDrawLine(pGraphics, pRailPen, kx + kd + stroke, cy, railRight, cy);
            }
            GdipDeletePen(pRailPen);
        }

        if (bOn)
        {
            EP_GpBrush* pKnobBrush = NULL;
            if (GdipCreateSolidFill(argbAccent, &pKnobBrush) == 0 && pKnobBrush)
            {
                GdipFillEllipse(pGraphics, pKnobBrush, kx, inset, kd, kd);
                GdipDeleteBrush(pKnobBrush);
            }
        }
        else
        {
            EP_GpPen* pRingPen = NULL;
            if (GdipCreatePen1(argbRing, stroke, EP_UnitPixel, &pRingPen) == 0 && pRingPen)
            {
                GdipDrawEllipse(pGraphics, pRingPen, kx + stroke / 2.0f, inset + stroke / 2.0f, kd - stroke, kd - stroke);
                GdipDeletePen(pRingPen);
            }
        }

        if (bFocused)
        {
            // Keyboard focus: a thin circle around the knob in the selection color.
            EP_GpPen* pFocusPen = NULL;
            if (GdipCreatePen1(GUI_ColorRefToArgb(bDark ? GUI_TEXTCOLOR_SELECTED_DARK : GUI_TEXTCOLOR_SELECTED), 1.0f, EP_UnitPixel, &pFocusPen) == 0 && pFocusPen)
            {
                float fd = (float)h - 1.0f;
                GdipDrawEllipse(pGraphics, pFocusPen, kx + kd / 2.0f - fd / 2.0f, 0.5f, fd, fd);
                GdipDeletePen(pFocusPen);
            }
        }
        bDrawn = TRUE;
    }
    if (pGraphics) GdipDeleteGraphics(pGraphics);
    if (pBitmap) GdipDisposeImage(pBitmap);

    if (bDrawn)
    {
        HDC hdcMem = CreateCompatibleDC(hdc);
        if (hdcMem)
        {
            HGDIOBJ hOld = SelectObject(hdcMem, hBitmap);
            BLENDFUNCTION bf;
            bf.BlendOp = AC_SRC_OVER;
            bf.BlendFlags = 0;
            bf.SourceConstantAlpha = 255;
            bf.AlphaFormat = AC_SRC_ALPHA;
            AlphaBlend(hdc, x, y, w, h, hdcMem, 0, 0, w, h, bf);
            SelectObject(hdcMem, hOld);
            DeleteDC(hdcMem);
        }
    }
    DeleteObject(hBitmap);
    return bDrawn ? (w + (int)(8 * dx + 0.5)) : 0;
}
#pragma endregion

TCHAR GUI_title[260];
WCHAR wszLanguage[LOCALE_NAME_MAX_LENGTH];
WCHAR wszThreadLanguage[LOCALE_NAME_MAX_LENGTH];
void* GUI_FileMapping = NULL;
DWORD GUI_FileSize = 0;
BOOL g_darkModeEnabled = FALSE;

// Undocumented uxtheme.dll exports (by ordinal) that switch a Win32 window to the dark app mode. Resolved in ZZGUI.
typedef BOOL (WINAPI* RefreshImmersiveColorPolicyState_t)(void);          // 104
typedef BOOL (WINAPI* ShouldAppsUseDarkMode_t)(void);                     // 132
typedef BOOL (WINAPI* AllowDarkModeForWindow_t)(HWND hWnd, BOOL bAllow);  // 133
typedef BOOL (WINAPI* SetPreferredAppMode_t)(BOOL bAllowDark);            // 135
static RefreshImmersiveColorPolicyState_t RefreshImmersiveColorPolicyState = NULL;
static ShouldAppsUseDarkMode_t ShouldAppsUseDarkMode = NULL;
static AllowDarkModeForWindow_t AllowDarkModeForWindow = NULL;
static SetPreferredAppMode_t SetPreferredAppMode = NULL;

void PlayHelpMessage(GUI* _this)
{
    unsigned int max_section = 0;
    for (unsigned int i = 0; i < 100; ++i)
    {
        if (_this->sectionNames[i][0] == 0)
        {
            max_section = i - 1;
            break;
        }
    }

    WCHAR wszAccText[1000];
    swprintf_s(
        wszAccText,
        1000,
        L"Welcome to ExplorerPatcher. "
        L"Selected page is: %s: %d of %d. "
        L"To switch pages, press the Left or Right arrow keys or press a number (%d to %d). "
        L"To select an item, press the Up or Down arrow keys or Shift+Tab and Tab. "
        L"To interact with the selected item, press Space or Return. "
        L"To close this window, press Escape. "
        L"Press a number to switch to the corresponding page: ",
        _this->sectionNames[_this->section],
        _this->section + 1,
        max_section + 1,
        1,
        max_section + 1
    );
    for (unsigned int i = 0; i < 100; ++i)
    {
        if (_this->sectionNames[i][0] == 0)
        {
            break;
        }
        WCHAR wszAdd[100];
        swprintf_s(wszAdd, 100, L"%d: %s, ", i + 1, _this->sectionNames[i]);
        wcscat_s(wszAccText, 1000, wszAdd);
    }
    wcscat_s(wszAccText, 1000, L"\nTo listen to this message again, press the F1 key at any time.\n");
    SetWindowTextW(_this->hAccLabel, wszAccText);
    NotifyWinEvent(
        EVENT_OBJECT_LIVEREGIONCHANGED,
        _this->hAccLabel,
        OBJID_CLIENT,
        CHILDID_SELF
    );
}

BOOL IsColorSchemeChangeMessage(LPARAM lParam)
{
    BOOL is = FALSE;
    if (lParam && CompareStringOrdinal(lParam, -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL)
    {
        is = TRUE;
    }
    return is;
}

static void GUI_SetSection(GUI* _this, BOOL bCheckEnablement, int dwSection)
{
    if (_this->section != (SIZE_T)dwSection)
    {
        _this->scrollY = 0;
    }
    _this->section = dwSection;

    HKEY hKey = NULL;
    DWORD dwSize = sizeof(DWORD);
    RegCreateKeyExW(
        HKEY_CURRENT_USER,
        TEXT(REGPATH),
        0,
        NULL,
        REG_OPTION_NON_VOLATILE,
        KEY_READ | KEY_WOW64_64KEY | KEY_WRITE,
        NULL,
        &hKey,
        NULL
    );
    if (hKey == NULL || hKey == INVALID_HANDLE_VALUE)
    {
        return;
    }

    BOOL bEnabled = FALSE;
    if (bCheckEnablement)
    {
        dwSize = sizeof(DWORD);
        RegQueryValueExW(
            hKey,
            TEXT("LastSectionInProperties"),
            0,
            NULL,
            &bEnabled,
            &dwSize
        );
        dwSection++;
    }
    else
    {
        bEnabled = TRUE;
    }

    if (bEnabled)
    {
        RegSetValueExW(
            hKey,
            TEXT("LastSectionInProperties"),
            0,
            REG_DWORD,
            &dwSection,
            sizeof(DWORD)
        );
    }

    RegCloseKey(hKey);
}

static void GUI_SubstituteLocalizedString(wchar_t* str, size_t cch)
{
    // %R:1212%
    //    ^^^^ The resource ID
    wchar_t* pszSubstituteBegin = wcsstr(str, L"%R:");
    if (!pszSubstituteBegin) return;

    wchar_t* pszSubstituteEnd = wcschr(pszSubstituteBegin + 3, L'%');
    if (!pszSubstituteEnd) return;
    ++pszSubstituteEnd; // Skip the %

    int resId = _wtoi(pszSubstituteBegin + 3);
    if (resId == 0) return;

    const wchar_t* pszLocalized = NULL;
    int cchLocalized = LoadStringW(hModule, resId, (LPWSTR)&pszLocalized, 0);
    if (cchLocalized == 0 || !pszLocalized) return;

    size_t cchStrBeginToSubstituteBegin = pszSubstituteBegin - str;
    size_t cchSubstituteEndToStrEnd = wcslen(pszSubstituteEnd);

    wchar_t* pszLocalizedEnd = pszSubstituteBegin + cchLocalized;
    size_t cchStrBeginToLocalizedEnd = cchStrBeginToSubstituteBegin + cchLocalized;
    size_t cchLocalizedEnd = cch - cchStrBeginToLocalizedEnd;

    // Move the end to make space
    memmove_s(
        pszLocalizedEnd,
        sizeof(wchar_t) * cchLocalizedEnd,
        pszSubstituteEnd,
        sizeof(wchar_t) * (cchSubstituteEndToStrEnd + 1 /*NUL*/)
    );

    // Copy the localized string
    memcpy_s(
        pszSubstituteBegin,
        sizeof(wchar_t) * (cch - cchStrBeginToSubstituteBegin),
        pszLocalized,
        sizeof(wchar_t) * cchLocalized
    );
}

static void GUI_EnumerateLanguagesCallback(const L10N_Language* language, void* data)
{
    HMENU hMenu = data;

    MENUITEMINFOW menuInfo;
    ZeroMemory(&menuInfo, sizeof(MENUITEMINFOW));
    menuInfo.cbSize = sizeof(MENUITEMINFOW);
    menuInfo.fMask = MIIM_ID | MIIM_STRING | MIIM_DATA | MIIM_STATE;
    menuInfo.wID = language->id + 1;
    menuInfo.dwItemData = NULL;
    menuInfo.fType = MFT_STRING;
    menuInfo.dwTypeData = (LPWSTR)language->wszDisplayName; // Copied by the system
    menuInfo.cch = (UINT)wcslen(language->wszDisplayName);
    InsertMenuItemW(hMenu, 2, FALSE, &menuInfo);
}

static void GUI_PopulateLanguageSelectorMenu(HMENU hMenu)
{
    // Follow system setting (default)
    wchar_t wszItemTitle[128];
    wszItemTitle[0] = 0;
    LoadStringW(hModule, IDS_LANG_FOLLOW_SYSTEM, wszItemTitle, ARRAYSIZE(wszItemTitle));

    MENUITEMINFOW menuInfo;
    ZeroMemory(&menuInfo, sizeof(MENUITEMINFOW));
    menuInfo.cbSize = sizeof(MENUITEMINFOW);
    menuInfo.fMask = MIIM_ID | MIIM_STRING | MIIM_DATA | MIIM_STATE;
    menuInfo.wID = 0 + 1;
    menuInfo.dwItemData = NULL;
    menuInfo.fType = MFT_STRING;
    menuInfo.dwTypeData = wszItemTitle;
    menuInfo.cch = (UINT)wcslen(wszItemTitle);

    InsertMenuItemW(hMenu, 0, FALSE, &menuInfo);

    // --------------------
    menuInfo.fMask = MIIM_TYPE;
    menuInfo.fType = MFT_SEPARATOR;
    InsertMenuItemW(hMenu, 0, FALSE, &menuInfo);

    // English
    // <All other languages sorted in ascending order>
    L10N_EnumerateLanguages(hModule, RT_STRING, MAKEINTRESOURCEW(IDS_PAGE_GENERAL / 16 + 1), GUI_EnumerateLanguagesCallback, hMenu);
}

static void GUI_UpdateLanguages()
{
    L10N_ApplyPreferredLanguageForCurrentThread();
    L10N_GetCurrentUserLanguage(wszLanguage, ARRAYSIZE(wszLanguage));
    L10N_GetCurrentThreadLanguage(wszThreadLanguage, ARRAYSIZE(wszThreadLanguage));
}

static BOOL GUI_Build(HDC hDC, HWND hwnd, POINT pt)
{
    GUI* _this;
    LONG_PTR ptr = GetWindowLongPtr(hwnd, GWLP_USERDATA);
    _this = (GUI*)(ptr);
    double dx = _this->dpi.x / 96.0, dy = _this->dpi.y / 96.0;
    size_t nSectionCount = 0; // number of ";T" pages seen in this pass
    _this->padding.left = GUI_PADDING_LEFT * dx;
    _this->padding.right = GUI_PADDING_RIGHT * dx;
    _this->padding.top = GUI_PADDING_TOP * dy;
    _this->padding.bottom = GUI_PADDING_BOTTOM * dy;
    _this->sidebarWidth = GUI_SIDEBAR_WIDTH * dx;

    RECT rc;
    GetClientRect(hwnd, &rc);

    PVOID pRscr = NULL;
    DWORD cbRscr = 0;
    if (GUI_FileMapping && GUI_FileSize)
    {
        pRscr = GUI_FileMapping;
        cbRscr = GUI_FileSize;
    }
    else
    {
        HRSRC hRscr = FindResource(
            hModule,
            MAKEINTRESOURCE(IDR_SETTINGS),
            RT_RCDATA
        );
        if (!hRscr)
        {
            return FALSE;
        }
        HGLOBAL hgRscr = LoadResource(
            hModule,
            hRscr
        );
        if (!hgRscr)
        {
            return FALSE;
        }
        pRscr = LockResource(hgRscr);
        cbRscr = SizeofResource(
            hModule,
            hRscr
        );
    }

    // The page definition is fixed text, but the user's own menu entries are not: there are as many of them as
    // the user has made. Rather than teach the drawing loop about lists, the ";k" line in the page is replaced
    // here with one real line per entry. Everything below then draws them exactly like any other line, which is
    // the whole point: they look like the rest of the window because the same code draws them.
    {
        DWORD cbExpanded = 0;
        PVOID pExpanded = SP_ExpandCustomMenuLines(pRscr, cbRscr, &cbExpanded);
        if (pExpanded)
        {
            pRscr = pExpanded;
            cbRscr = cbExpanded;
        }
    }

    UINT dpiX = 0, dpiY = 0;
    HRESULT hr = GetDpiForMonitor(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), MDT_DEFAULT, &dpiX, &dpiY);
    LOGFONT logFont;
    memset(&logFont, 0, sizeof(logFont));
    NONCLIENTMETRICS ncm;
    ncm.cbSize = sizeof(NONCLIENTMETRICS);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(NONCLIENTMETRICS), &ncm, 0, dpiX);
    // The whole window is drawn in Segoe UI rather than in whatever the system metrics happen to name. The
    // metrics still supply the size and the DPI scaling; only the typeface and the weight are decided here, so
    // the window looks the same on every machine.
    //
    // Windows 11 reports "Segoe UI Variable Text" for the menu font. It renders well but its weights are not
    // addressable through LOGFONT, so asking for a semibold gives back the regular face. Segoe UI has real
    // Semibold and Bold faces, which is what the headings need.
    logFont = ncm.lfMenuFont;
    wcscpy_s(logFont.lfFaceName, LF_FACESIZE, L"Segoe UI");
    logFont.lfQuality = CLEARTYPE_QUALITY;
    logFont.lfUnderline = 0;

    // Window caption: the product name in the title row.
    logFont.lfWeight = FW_SEMIBOLD;
    HFONT hFontCaption = CreateFontIndirect(&logFont);

    // Sidebar page names. The selected one is underlined as well.
    logFont.lfWeight = FW_BOLD;
    HFONT hFontSection = CreateFontIndirect(&logFont);
    logFont.lfUnderline = 1;
    HFONT hFontSectionSel = CreateFontIndirect(&logFont);
    logFont.lfUnderline = 0;

    // Group headings inside a page (the ";a" lines), one step below the sidebar.
    logFont.lfWeight = FW_SEMIBOLD;
    HFONT hFontHeading = CreateFontIndirect(&logFont);

    // Ordinary text, and the same weight underlined for links and actions.
    logFont.lfWeight = FW_NORMAL;
    HFONT hFontRegular = CreateFontIndirect(&logFont);
    logFont.lfUnderline = 1;
    HFONT hFontUnderline = CreateFontIndirect(&logFont);
    logFont.lfUnderline = 0;

    HFONT hFontTitle = hFontRegular;
    HFONT hOldFont = NULL;

    DTTOPTS DttOpts;
    DttOpts.dwSize = sizeof(DTTOPTS);
    DttOpts.dwFlags = DTT_COMPOSITED | DTT_TEXTCOLOR;
    //DttOpts.crText = GetSysColor(COLOR_WINDOWTEXT);
    DttOpts.crText = g_darkModeEnabled ? GUI_TEXTCOLOR_DARK : GUI_TEXTCOLOR;
    DWORD dwTextFlags = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS;
    RECT rcText;
    DWORD dwMinWidthDp = 600; // 480
    // if (!wcscmp(wszThreadLanguage, L"nl-NL")) dwMinWidthDp = 600;
    DWORD dwMaxHeight = 0, dwMaxWidth = (DWORD)(dwMinWidthDp * (_this->dpi.x / 96.0));
    BOOL bTabOrderHit = FALSE;
    DWORD dwLeftPad = _this->padding.left + _this->sidebarWidth + _this->padding.right;
    DWORD dwInitialLeftPad = dwLeftPad;

    HDC hdcPaint = NULL;
    BP_PAINTPARAMS params = { sizeof(BP_PAINTPARAMS) };
    params.dwFlags = BPPF_ERASE;
    HPAINTBUFFER hBufferedPaint = BeginBufferedPaint(hDC, &rc, BPBF_TOPDOWNDIB, &params, &hdcPaint);

    if (!hDC || (hDC && hdcPaint))
    {
        if (!hDC)
        {
            hdcPaint = GetDC(hwnd);
        }

        if ((!IsThemeActive() || IsHighContrast()) && hDC)
        {
            COLORREF oldcr = SetBkColor(hdcPaint, GetSysColor(COLOR_3DFACE));
            ExtTextOutW(hdcPaint, 0, 0, ETO_OPAQUE, &rc, L"", 0, 0);
            SetBkColor(hdcPaint, oldcr);
            SetTextColor(hdcPaint, GetSysColor(COLOR_WINDOWTEXT));
            SetBkMode(hdcPaint, TRANSPARENT);
        }
        else if ((!IsWindows11() || IsDwmExtendFrameIntoClientAreaBrokenInThisBuild()) && hDC)
        {
            COLORREF oldcr = SetBkColor(hdcPaint, g_darkModeEnabled ? RGB(0, 0, 0) : RGB(255, 255, 255));
            ExtTextOutW(hdcPaint, 0, 0, ETO_OPAQUE, &rc, L"", 0, 0);
            SetBkColor(hdcPaint, oldcr);
        }

        BOOL bResetLastHeading = TRUE;
        BOOL bWasSpecifiedSectionValid = FALSE;
        // scrolling. The content of the selected page is drawn shifted by scrollY and clipped to
        // the viewport between the caption and the footer.
        BOOL bContentClip = FALSE;
        LONG lContentBottom = 0, lViewTop = 0, lViewBottom = 0;
        RECT rcScrollLine;
        SetRectEmpty(&rcScrollLine);
        FILE* f = fmemopen(pRscr, cbRscr, "r");
        char* line = malloc(MAX_LINE_LENGTH * sizeof(char));
        wchar_t* text = malloc((MAX_LINE_LENGTH + 3) * sizeof(wchar_t)); 
        wchar_t* name = malloc(MAX_LINE_LENGTH * sizeof(wchar_t));
        wchar_t* section = malloc(MAX_LINE_LENGTH * sizeof(wchar_t));
        size_t bufsiz = MAX_LINE_LENGTH, numChRd = 0, tabOrder = 1, currentSection = -1, topAdj = 0;
        if (_this->bCalcExtent)
        {
            // A measuring pass computes the footer position of the page it measures from scratch.
            _this->dwStatusbarY = 0;
        }
        wchar_t* lastHeading = calloc(MAX_LINE_LENGTH, sizeof(wchar_t));
        while ((numChRd = getline(&line, &bufsiz, f)) != -1)
        {
            // Editors tend to strip the trailing space of an empty ";e " line; accept the bare form too.
            if (!strcmp(line, ";e\r\n") || !strcmp(line, ";e\n"))
            {
                strcpy_s(line, bufsiz, ";e \r\n");
                numChRd = 5;
            }
            hOldFont = NULL;
            if (currentSection == _this->section)
            {
                bWasSpecifiedSectionValid = TRUE;
            }
            if (!strncmp(line, ";g ", 3))
            {
                continue;
            }
            if (!strncmp(line, ";s ", 3))
            {
                if (_this->bCalcExtent) continue;
                char* funcName = strchr(line + 3, ' ');
                funcName[0] = 0;
                char* skipToName = line + 3;
                funcName++;
                strchr(funcName, '\r')[0] = 0;
                BOOL bSkipLines = !GUI_EvaluateCondition(funcName);
                if (bSkipLines)
                {
                    do
                    {
                        getline(&text, &bufsiz, f);
                        strchr(text, '\r')[0] = 0;
                    } while (strncmp(text, ";g ", 3) || _stricmp((char*)text + 3, skipToName));
                }
                continue;
            }
            if (!strncmp(line, ";q", 2))
            {
                bResetLastHeading = TRUE;
                lastHeading[0] = 0;
                continue;
            }
            if (strcmp(line, "Windows Registry Editor Version 5.00\r\n") && 
                strcmp(line, "\r\n") && 
                (currentSection == -1 || currentSection == _this->section || !strncmp(line, ";T ", 3) || !strncmp(line, ";f", 2) || AuditFile) &&
                !((!IsThemeActive() || IsHighContrast() || !IsWindows11() || IsDwmExtendFrameIntoClientAreaBrokenInThisBuild()) && !strncmp(line, ";M ", 3))
                )
            {
#ifndef USE_PRIVATE_INTERFACES
                if (!strncmp(line, ";p ", 3))
                {
                    int num = atoi(line + 3);
                    for (int i = 0; i < num; ++i)
                    {
                        getline(&line, &bufsiz, f);
                    }
                }
#endif
                if (!strncmp(line, ";f", 2))
                {
                    if (currentSection == _this->section)
                    {
                        lContentBottom = dwMaxHeight;
                    }
                    if (bContentClip)
                    {
                        RestoreDC(hdcPaint, -1);
                        bContentClip = FALSE;
                    }
                    //if (topAdj + ((currentSection + 2) * GUI_SECTION_HEIGHT * dy) > dwMaxHeight)
                    //{
                    //    dwMaxHeight = topAdj + ((currentSection + 2) * GUI_SECTION_HEIGHT * dy);
                    //}
                    if (_this->dwStatusbarY == 0)
                    {
                        //dwMaxHeight += GUI_STATUS_PADDING * dy;
                        _this->dwStatusbarY = dwMaxHeight / dy;
                    }
                    else
                    {
                        dwMaxHeight = _this->dwStatusbarY * dy;
                    }
                    currentSection = -1;
                    dwLeftPad = 0;
                    continue;
                }

                if (!strncmp(line, "[", 1))
                {
                    ZeroMemory(section, MAX_LINE_LENGTH * sizeof(wchar_t));
                    MultiByteToWideChar(
                        CP_UTF8,
                        0,
                        line[1] == '-' ? line + 2 : line + 1,
                        numChRd - (line[1] == '-' ? 5 : 4),
                        section,
                        MAX_LINE_LENGTH
                    );
                    //wprintf(L"%s\n", section);
                }

                DWORD dwLineHeight = !strncmp(line, ";M ", 3) ? _this->GUI_CAPTION_LINE_HEIGHT : GUI_LINE_HEIGHT;
                DWORD dwBottom = _this->padding.bottom;
                DWORD dwTop = _this->padding.top;
                if (!strncmp(line, ";a ", 3) || !strncmp(line, ";e ", 3))
                {
                    dwBottom = 0;
                    dwLineHeight -= 0.2 * dwLineHeight;
                }

                rcText.left = dwLeftPad + _this->padding.left;
                rcText.top = !strncmp(line, ";M ", 3) ? 0 : (dwTop + dwMaxHeight);
                rcText.right = (rc.right - rc.left) - _this->padding.right;
                rcText.bottom = !strncmp(line, ";M ", 3) ? _this->GUI_CAPTION_LINE_HEIGHT * dy : (dwMaxHeight + dwLineHeight * dy - dwBottom);

                if (strncmp(line, ";T ", 3) && strncmp(line, ";M ", 3) && currentSection != -1 && currentSection == _this->section &&
                    !_this->bCalcExtent && !AuditFile && _this->dwStatusbarY)
                {
                    if (lViewBottom == 0)
                    {
                        // First content line of the page: the viewport starts where the content starts.
                        lViewTop = (LONG)dwMaxHeight;
                        lViewBottom = (LONG)(_this->dwStatusbarY * dy);
                    }
                    OffsetRect(&rcText, 0, -_this->scrollY);
                    if (hDC)
                    {
                        if (!bContentClip)
                        {
                            SaveDC(hdcPaint);
                            IntersectClipRect(hdcPaint, rc.left, lViewTop, rc.right, lViewBottom);
                            bContentClip = TRUE;
                        }
                    }
                    // Themed (composited) text ignores the clip region, so lines that are not inside the viewport are
                    // moved out of sight altogether; that keeps them from being drawn or clicked.
                    rcScrollLine = rcText;
                    LONG lTolerance = (LONG)(8 * dy);
                    if (rcText.top < lViewTop - lTolerance || rcText.bottom > lViewBottom + lTolerance)
                    {
                        OffsetRect(&rcText, 0, -1000000);
                    }
                }
                else if (bContentClip)
                {
                    RestoreDC(hdcPaint, -1);
                    bContentClip = FALSE;
                }

                if (!strncmp(line, ";T ", 3))
                {
                    if (currentSection + 1 == _this->section)
                    {
                        hOldFont = SelectObject(hdcPaint, hFontSectionSel);
                    }
                    else
                    {
                        hOldFont = SelectObject(hdcPaint, hFontSection);
                    }
                    rcText.left = _this->padding.left;
                    rcText.right = _this->padding.left + _this->sidebarWidth;
                    rcText.top = topAdj + ((currentSection + 1) * GUI_SECTION_HEIGHT * dy);
                    rcText.bottom = topAdj + ((currentSection + 2) * GUI_SECTION_HEIGHT * dy);
                    ZeroMemory(text, (MAX_LINE_LENGTH + 3) * sizeof(wchar_t));
                    MultiByteToWideChar(
                        CP_UTF8,
                        0,
                        line + 3,
                        numChRd - 3,
                        text,
                        MAX_LINE_LENGTH
                    );
                    GUI_SubstituteLocalizedString(text, MAX_LINE_LENGTH);
                    if (_this->sectionNames[currentSection + 1][0] == 0)
                    {
                        wcscpy_s(_this->sectionNames[currentSection + 1], 64, text);
                    }
                    if (hDC)
                    {
                        if (IsThemeActive() && !IsHighContrast())
                        {
                            DrawThemeTextEx(
                                _this->hTheme,
                                hdcPaint,
                                0,
                                0,
                                text,
                                -1,
                                dwTextFlags,
                                &rcText,
                                &DttOpts
                            );
                        }
                        else
                        {
                            DrawTextW(
                                hdcPaint,
                                text,
                                -1,
                                &rcText,
                                dwTextFlags
                            );
                        }
                    }
                    else
                    {
                        RECT rcTemp;
                        rcTemp = rcText;
                        DrawTextW(
                            hdcPaint,
                            text,
                            -1,
                            &rcTemp,
                            DT_CALCRECT
                        );
                        rcTemp.bottom = rcText.bottom;
                        if (PtInRect(&rcTemp, pt))
                        {
                            _this->bShouldAnnounceSelected = TRUE;
                            _this->bRebuildIfTabOrderIsEmpty = FALSE;
                            _this->tabOrder = 0;
                            GUI_SetSection(_this, TRUE, currentSection + 1);
                            InvalidateRect(hwnd, NULL, FALSE);
                        }
                    }
                    if (currentSection != -1 && currentSection == _this->section)
                    {
                        lContentBottom = dwMaxHeight;
                    }
                    currentSection++;
                    nSectionCount++;
                    continue;
                }
                else if (!strncmp(line, ";M ", 3))
                {
                    UINT diff = (((_this->GUI_CAPTION_LINE_HEIGHT - 16) * dx) / 2.0);
                    rcText.left = diff + (int)(16.0 * dx) + diff / 2;
                    topAdj = dwMaxHeight + _this->GUI_CAPTION_LINE_HEIGHT * dy;
                    hOldFont = SelectObject(hdcPaint, hFontCaption);
                }
                else if (!strncmp(line, ";a ", 3))
                {
                    // A group heading inside a page. Heavier than the options under it, lighter than the
                    // sidebar, so the page reads as two levels rather than three.
                    hOldFont = SelectObject(hdcPaint, hFontHeading);
                }
                else if (!strncmp(line, ";u ", 3) || !strncmp(line, ";m ", 3) || (!strncmp(line, ";y ", 3) && !strstr(line, "\xF0\x9F")))
                {
                    hOldFont = SelectObject(hdcPaint, hFontUnderline);
                }
                else
                {
                    hOldFont = SelectObject(hdcPaint, hFontRegular);
                }

                if (!strncmp(line, ";e ", 3) || !strncmp(line, ";a ", 3) || !strncmp(line, ";T ", 3) || !strncmp(line, ";t ", 3) || !strncmp(line, ";u ", 3) || !strncmp(line, ";m ", 3) || !strncmp(line, ";M ", 3))
                {
                    ZeroMemory(text, (MAX_LINE_LENGTH + 3) * sizeof(wchar_t));
                    MultiByteToWideChar(
                        CP_UTF8,
                        0,
                        line + 3,
                        numChRd - 3,
                        text,
                        MAX_LINE_LENGTH
                    );
                    if (!wcsncmp(text, L"%VERSIONINFORMATIONSTRING%", 26))
                    {
                        DWORD dwLeftMost = 0;
                        DWORD dwSecondLeft = 0;
                        DWORD dwSecondRight = 0;
                        DWORD dwRightMost = 0;

                        QueryVersionInfo(hModule, VS_VERSION_INFO, &dwLeftMost, &dwSecondLeft, &dwSecondRight, &dwRightMost);

                        wchar_t wszFormat[MAX_PATH];
                        int numChars = LoadStringW(hModule, IDS_ABOUT_VERSION, wszFormat, MAX_PATH);
                        if (numChars != 0)
                        {
                            swprintf_s(text, MAX_LINE_LENGTH, wszFormat, dwLeftMost, dwSecondLeft, dwSecondRight, dwRightMost,
#if defined(DEBUG) | defined(_DEBUG)
                                L" (Debug)"
#else
                                L""
#endif
                            );
                        }
                    }
                    else if (!wcsncmp(text, L"%AUTHORSSTRING%", 15))
                    {
                        wchar_t wszFormat[MAX_PATH];
                        int numChars = LoadStringW(hModule, IDS_ABOUT_AUTHOR, wszFormat, MAX_PATH);
                        if (numChars != 0)
                        {
                            swprintf_s(text, MAX_LINE_LENGTH, wszFormat, _T(PRODUCT_PUBLISHER));
                        }
                    }
                    else if (!wcsncmp(text, L"%OSVERSIONSTRING%", 17))
                    {
                        wchar_t wszFormat[MAX_PATH];
                        int numChars = LoadStringW(hModule, IDS_ABOUT_OS, wszFormat, MAX_PATH);
                        if (numChars != 0)
                        {
                            swprintf_s(
                                text, MAX_LINE_LENGTH, wszFormat,
                                IsWindows11() ? L"Windows 11" : L"Windows 10",
                                global_rovi.dwBuildNumber,
                                global_ubr
                            );
                        }
                    }
                    else if (!wcsncmp(text, L"%ENGINESTATUSSTRING%", 20))
                    {
                        // Whether the shell of this session has the engine in it right now. Looked up on every
                        // paint, which is cheap (one module snapshot) and means the line follows a restart
                        // without the window having to be reopened.
                        wchar_t wszPath[MAX_PATH];
                        int mode = FindEngineInShell(wszPath, MAX_PATH);
                        UINT ids;
                        if (IsExplorerRestartInProgress())
                        {
                            ids = IDS_GENERAL_ENGINE_RESTARTING;
                        }
                        else if (mode == SP_ENGINE_MISSING)
                        {
                            ids = IDS_GENERAL_ENGINE_MISSING;
                        }
                        else if (IsEngineInSafeMode())
                        {
                            ids = IDS_GENERAL_ENGINE_SAFEMODE;
                        }
                        else
                        {
                            ids = (mode == SP_ENGINE_PROXY) ? IDS_GENERAL_ENGINE_INSTALLED : IDS_GENERAL_ENGINE_INJECTED;
                        }
                        LoadStringW(hModule, ids, text, MAX_LINE_LENGTH);
                    }
                    else
                    {
                        GUI_SubstituteLocalizedString(text, MAX_LINE_LENGTH);
                    }
                    if (!strncmp(line, ";m ", 3))
                    {
                        // A drop-down that toggles several things at once looks like a choice, so it gets the
                        // same arrow.
                        wcscat_s(text, MAX_LINE_LENGTH, L" \x25BE");
                    }
                    if (bResetLastHeading)
                    {
                        wcscpy_s(lastHeading, MAX_LINE_LENGTH, text);
                        bResetLastHeading = FALSE;
                    }
                    else
                    {
                        wcscat_s(lastHeading, MAX_LINE_LENGTH, L" ");
                        wcscat_s(lastHeading, MAX_LINE_LENGTH, text);
                    }
                    if (!strncmp(line, ";a ", 3))
                    {
                        RECT rcTemp;
                        rcTemp = rcText;
                        DrawTextW(
                            hdcPaint,
                            L"\u2795  ",
                            3,
                            &rcTemp,
                            DT_CALCRECT
                        );
                        rcText.left += rcTemp.right - rcTemp.left;
                        rcText.right += rcTemp.right - rcTemp.left;
                    }
                    if (!strncmp(line, ";M ", 3))
                    {
                        if (hDC)
                        {
                            UINT diff = (int)(((_this->GUI_CAPTION_LINE_HEIGHT - 16) * dx) / 2.0);
                            //printf("!!! %d %d\n", (int)(16.0 * dx), diff);
                            DrawIconEx(
                                hdcPaint,
                                diff,
                                diff,
                                _this->hIcon,
                                (int)(16.0 * dx),
                                (int)(16.0 * dy),
                                0,
                                NULL,
                                DI_NORMAL
                            );
                        }

                        LoadStringW(hModule, IDS_PRODUCTNAME, text, MAX_LINE_LENGTH);
                        //}
                        //rcText.bottom += _this->GUI_CAPTION_LINE_HEIGHT - dwLineHeight;
                        dwLineHeight = _this->GUI_CAPTION_LINE_HEIGHT;
                        _this->extent.cyTopHeight = rcText.bottom;
                    }
                    if (hDC)
                    {
                        COLORREF cr;
                        if ((!strncmp(line, ";u ", 3) || !strncmp(line, ";m ", 3)) && tabOrder == _this->tabOrder)
                        {
                            bTabOrderHit = TRUE;
                            if (_this->bEnsureFocusVisible && lViewBottom > lViewTop)
                            {
                                _this->bEnsureFocusVisible = FALSE;
                                int nDelta = 0;
                                if (rcScrollLine.top < lViewTop) nDelta = rcScrollLine.top - lViewTop;
                                else if (rcScrollLine.bottom > lViewBottom) nDelta = rcScrollLine.bottom - lViewBottom;
                                if (nDelta)
                                {
                                    _this->scrollY = max(0, _this->scrollY + nDelta);
                                    InvalidateRect(hwnd, NULL, FALSE);
                                }
                            }
                            if (!IsThemeActive() || IsHighContrast())
                            {
                                cr = SetTextColor(hdcPaint, GetSysColor(COLOR_HIGHLIGHT));
                            }
                            else
                            {
                                DttOpts.crText = g_darkModeEnabled ? GUI_TEXTCOLOR_SELECTED_DARK : GUI_TEXTCOLOR_SELECTED;
                                //DttOpts.crText = GetSysColor(COLOR_HIGHLIGHT);
                            }
                            if (_this->bShouldAnnounceSelected)
                            {
                                WCHAR accText[1000];
                                swprintf_s(
                                    accText, 
                                    1000, 
                                    L"%s %s - Button.",
                                    (_this->dwPageLocation < 0 ?
                                    L"Reached end of the page." :
                                    (_this->dwPageLocation > 0 ?
                                    L"Reached beginning of the page." : L"")),
                                    text
                                );
                                _this->dwPageLocation = 0;
                                for (unsigned int i = 0; i < wcslen(accText) - 2; ++i)
                                {
                                    if (accText[i] == L'(' && accText[i + 1] == L'*' && accText[i + 2] == L')')
                                    {
                                        accText[i] = L' ';
                                        accText[i + 1] = L' ';
                                        accText[i + 2] = L' ';
                                    }
                                }
                                SetWindowTextW(_this->hAccLabel, accText);
                                NotifyWinEvent(
                                    EVENT_OBJECT_LIVEREGIONCHANGED,
                                    _this->hAccLabel,
                                    OBJID_CLIENT,
                                    CHILDID_SELF);
                                _this->bShouldAnnounceSelected = FALSE;
                            }
                        }
                        RECT rcNew = rcText;
                        DrawTextW(
                            hdcPaint,
                            text,
                            -1,
                            &rcNew,
                            DT_CALCRECT
                        );
                        if (rcNew.right - rcNew.left > dwMaxWidth)
                        {
                            dwMaxWidth = rcNew.right - rcNew.left + 50 * dx;
                        }
                        if (IsThemeActive() && !IsHighContrast())
                        {
                            DrawThemeTextEx(
                                _this->hTheme,
                                hdcPaint,
                                hOldFont ? 0 : 8,
                                0,
                                text,
                                -1,
                                dwTextFlags,
                                &rcText,
                                &DttOpts
                            );
                        }
                        else
                        {
                            DrawTextW(
                                hdcPaint,
                                text,
                                -1,
                                &rcText,
                                dwTextFlags
                            );
                        }
                        if ((!strncmp(line, ";u ", 3) || !strncmp(line, ";m ", 3)) && tabOrder == _this->tabOrder)
                        {
                            if (!IsThemeActive() || IsHighContrast())
                            {
                                SetTextColor(hdcPaint, cr);
                            }
                            else
                            {
                                DttOpts.crText = g_darkModeEnabled ? GUI_TEXTCOLOR_DARK : GUI_TEXTCOLOR;
                                //DttOpts.crText = GetSysColor(COLOR_WINDOWTEXT);
                            }
                        }
                    }
                    else
                    {
                        RECT rcTemp;
                        rcTemp = rcText;
                        DrawTextW(
                            hdcPaint,
                            text,
                            -1,
                            &rcTemp,
                            DT_CALCRECT
                        );
                        rcTemp.bottom = rcText.bottom;
                        if (!strncmp(line, ";m ", 3) && (PtInRect(&rcTemp, pt) || (pt.x == 0 && pt.y == 0 && tabOrder == _this->tabOrder)))
                        {
                            // A multi-select drop-down. The lines after it are its items, ";x id text" for an
                            // item that is off and ";X id text" for one that is on, closed by the action line
                            // ";<prefix>". Picking an item calls the action with the id appended, and the
                            // handler decides what toggling it means; the page is then rebuilt, so the check
                            // marks follow whatever the handler did.
                            HMENU hToggleMenu = CreatePopupMenu();
                            char szAction[MAX_LINE_LENGTH];
                            szAction[0] = 0;
                            for (;;)
                            {
                                numChRd = getline(&line, &bufsiz, f);
                                if (numChRd <= 0)
                                {
                                    break;
                                }
                                char* p = strchr(line, '\r');
                                if (p) *p = 0;
                                p = strchr(line, '\n');
                                if (p) *p = 0;

                                if (!strncmp(line, ";x ", 3) || !strncmp(line, ";X ", 3))
                                {
                                    BOOL bChecked = (line[1] == 'X');
                                    char* sep = strchr(line + 3, ' ');
                                    if (!sep)
                                    {
                                        continue;
                                    }
                                    *sep = 0;
                                    wchar_t miText[MAX_PATH];
                                    ZeroMemory(miText, sizeof(miText));
                                    MultiByteToWideChar(CP_UTF8, 0, sep + 1, -1, miText, MAX_PATH);
                                    GUI_SubstituteLocalizedString(miText, MAX_PATH);

                                    MENUITEMINFOW menuInfo;
                                    ZeroMemory(&menuInfo, sizeof(MENUITEMINFOW));
                                    menuInfo.cbSize = sizeof(MENUITEMINFOW);
                                    menuInfo.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE;
                                    menuInfo.wID = atoi(line + 3) + 1;
                                    menuInfo.fState = bChecked ? MFS_CHECKED : MFS_UNCHECKED;
                                    menuInfo.dwTypeData = miText;
                                    menuInfo.cch = (UINT)wcslen(miText);
                                    InsertMenuItemW(hToggleMenu, (UINT)-1, TRUE, &menuInfo);
                                    continue;
                                }
                                if (line[0] == ';')
                                {
                                    strcpy_s(szAction, MAX_LINE_LENGTH, line + 1);
                                }
                                break;
                            }

                            POINT p;
                            p.x = rcText.left;
                            p.y = rcText.bottom;
                            ClientToScreen(hwnd, &p);
                            int val = TrackPopupMenu(hToggleMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd, 0);
                            DestroyMenu(hToggleMenu);

                            if (val > 0 && szAction[0])
                            {
                                char szFull[MAX_LINE_LENGTH];
                                sprintf_s(szFull, MAX_LINE_LENGTH, "%s%d", szAction, val - 1);
                                if (SP_HandleCustomMenuAction(hwnd, szFull))
                                {
                                    _this->tabOrder = 0;
                                    _this->bCalcExtent = 2;
                                    _this->last_section = _this->section;
                                    InvalidateRect(hwnd, NULL, FALSE);
                                }
                            }
                        }
                        else if (!strncmp(line, ";u ", 3) && (PtInRect(&rcTemp, pt) || (pt.x == 0 && pt.y == 0 && tabOrder == _this->tabOrder)))
                        {
                            numChRd = getline(&line, &bufsiz, f);
                            char* p = strchr(line, '\r');
                            if (p) *p = 0;
                            p = strchr(line, '\n');
                            if (p) *p = 0;
                            if (!strncmp(line + 1, "restart", 7))
                            {
                                RestartExplorer();
                                // The status line follows the shell going away and coming back.
                                _this->engineStatusTicks = 0;
                                SetTimer(hwnd, GUI_TIMER_ENGINE_STATUS, GUI_TIMER_ENGINE_STATUS_TIMEOUT, NULL);
                            }
                            else if (!strncmp(line + 1, "resetpins", 9))
                            {
                                // The Start menu (custom-start-menu mod) watches its key and puts the default
                                // pins back when it sees this; it clears the value itself.
                                DWORD dwOne = 1;
                                RegSetKeyValueW(HKEY_CURRENT_USER, TEXT(REGPATH) L"\\Mods\\custom-start-menu", L"ResetPins",
                                                REG_DWORD, &dwOne, sizeof(dwOne));
                            }
                            else if (!strncmp(line + 1, "clearimage", 10))
                            {
                                RegDeleteKeyValueW(HKEY_CURRENT_USER, TEXT(REGPATH) L"\\Mods\\custom-start-menu", L"BackgroundImage");
                                _this->tabOrder = 0;
                                InvalidateRect(hwnd, NULL, FALSE);
                            }
                            else if (!strncmp(line + 1, "resethidden", 11) || !strncmp(line + 1, "clearhistory", 12))
                            {
                                // One-shot requests to the Start menu, cleared by the menu itself (see resetpins).
                                DWORD dwOne = 1;
                                RegSetKeyValueW(HKEY_CURRENT_USER, TEXT(REGPATH) L"\\Mods\\custom-start-menu",
                                                line[1] == 'r' ? L"ResetHidden" : L"ClearHistory", REG_DWORD, &dwOne, sizeof(dwOne));
                            }
                            else if (!strncmp(line + 1, "reset", 5))
                            {
                                // The embedded settings.reg holds the defaults: write it out and import it.
                                wchar_t wszPath[MAX_PATH];
                                if (Registry_WriteTempRegFile(pRscr, cbRscr, wszPath, MAX_PATH))
                                {
                                    if (Registry_ImportFile(wszPath))
                                    {
                                        _this->tabOrder = 0;
                                        InvalidateRect(hwnd, NULL, FALSE);
                                    }
                                    DeleteFileW(wszPath);
                                }
                            }
                            else if (!strncmp(line + 1, "export", 6))
                            {
                                WCHAR title[MAX_PATH];
                                WCHAR filter[MAX_PATH];
                                WCHAR wszRegedit[MAX_PATH];
                                GetWindowsDirectoryW(wszRegedit, MAX_PATH);
                                wcscat_s(wszRegedit, MAX_PATH, L"\\regedit.exe");
                                HMODULE hRegedit = LoadLibraryExW(wszRegedit, NULL, LOAD_LIBRARY_AS_DATAFILE);
                                if (hRegedit)
                                {
                                    LoadStringW(hRegedit, 301, title, MAX_PATH);
                                    LoadStringW(hRegedit, 302, filter, MAX_PATH);
                                    unsigned int j = 0;
                                    for (unsigned int i = 0; i < MAX_PATH; ++i)
                                    {
                                        if (filter[i] == L'#')
                                        {
                                            filter[i] = L'\0';
                                            j++;
                                            if (j == 2)
                                            {
                                                filter[i + 1] = L'\0';
                                                break;
                                            }
                                        }
                                    }
                                    FreeLibrary(hRegedit);
                                }
                                else
                                {
                                    wcscpy_s(title, MAX_PATH, L"Export settings");
                                    wcscpy_s(filter, MAX_PATH, L"Registration Files (*.reg)\0*.reg\0\0");
                                }
                                WCHAR wszPath[MAX_PATH];
                                ZeroMemory(wszPath, MAX_PATH * sizeof(WCHAR));
                                DWORD dwLeftMost = 0;
                                DWORD dwSecondLeft = 0;
                                DWORD dwSecondRight = 0;
                                DWORD dwRightMost = 0;
                                QueryVersionInfo(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE), VS_VERSION_INFO, &dwLeftMost, &dwSecondLeft, &dwSecondRight, &dwRightMost);
                                swprintf_s(wszPath, MAX_PATH, _T(PRODUCT_NAME) L"_%d.%d.%d.%d.reg", dwLeftMost, dwSecondLeft, dwSecondRight, dwRightMost);
                                OPENFILENAMEW ofn;
                                ZeroMemory(&ofn, sizeof(OPENFILENAMEW));
                                ofn.lStructSize = sizeof(OPENFILENAMEW);
                                ofn.hwndOwner = hwnd;
                                ofn.hInstance = GetModuleHandleW(NULL);
                                ofn.lpstrFilter = filter;
                                ofn.lpstrCustomFilter = NULL;
                                ofn.nMaxCustFilter = 0;
                                ofn.nFilterIndex = 1;
                                ofn.lpstrFile = wszPath;
                                ofn.nMaxFile = MAX_PATH;
                                ofn.lpstrFileTitle = NULL;
                                ofn.nMaxFileTitle = 0;
                                ofn.lpstrInitialDir = NULL;
                                ofn.lpstrTitle = title;
                                ofn.Flags = OFN_DONTADDTORECENT | OFN_CREATEPROMPT | OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT;
                                ofn.nFileOffset = 0;
                                ofn.nFileExtension = 0;
                                ofn.lpstrDefExt = L"reg";
                                ofn.lCustData = NULL;
                                ofn.lpfnHook = NULL;
                                ofn.lpTemplateName = NULL;
                                if (GetSaveFileNameW(&ofn))
                                {
                                    _wfopen_s(&AuditFile, wszPath, L"w");
                                    if (AuditFile)
                                    {
                                        fwprintf(AuditFile, L"Windows Registry Editor Version 5.00\n\n");
                                        POINT pt;
                                        pt.x = 0;
                                        pt.y = 0;
                                        GUI_Build(0, hwnd, pt);
                                        fclose(AuditFile);
                                        AuditFile = NULL;
                                        wchar_t mbText[256];
                                        mbText[0] = 0;
                                        LoadStringW(hModule, IDS_EXPORT_SUCCESS, mbText, ARRAYSIZE(mbText));
                                        MessageBoxW(hwnd, mbText, GUI_title, MB_ICONINFORMATION);
                                    }
                                }
                            }
                            else if (!strncmp(line + 1, "import", 6))
                            {
                                WCHAR title[MAX_PATH];
                                WCHAR filter[MAX_PATH];
                                WCHAR wszRegedit[MAX_PATH];
                                GetWindowsDirectoryW(wszRegedit, MAX_PATH);
                                wcscat_s(wszRegedit, MAX_PATH, L"\\regedit.exe");
                                HMODULE hRegedit = LoadLibraryExW(wszRegedit, NULL, LOAD_LIBRARY_AS_DATAFILE);
                                if (hRegedit)
                                {
                                    LoadStringW(hRegedit, 300, title, MAX_PATH);
                                    LoadStringW(hRegedit, 302, filter, MAX_PATH);
                                    unsigned j = 0;
                                    for (unsigned int i = 0; i < MAX_PATH; ++i)
                                    {
                                        if (filter[i] == L'#')
                                        {
                                            filter[i] = L'\0';
                                            j++;
                                            if (j == 2)
                                            {
                                                filter[i + 1] = L'\0';
                                                break;
                                            }
                                        }
                                    }
                                    FreeLibrary(hRegedit);
                                }
                                else
                                {
                                    wcscpy_s(title, MAX_PATH, L"Import settings");
                                    wcscpy_s(filter, MAX_PATH, L"Registration Files (*.reg)\0*.reg\0\0");
                                }
                                WCHAR wszPath[MAX_PATH];
                                ZeroMemory(wszPath, MAX_PATH * sizeof(WCHAR));
                                DWORD dwLeftMost = 0;
                                DWORD dwSecondLeft = 0;
                                DWORD dwSecondRight = 0;
                                DWORD dwRightMost = 0;
                                QueryVersionInfo(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE), VS_VERSION_INFO, &dwLeftMost, &dwSecondLeft, &dwSecondRight, &dwRightMost);
                                swprintf_s(wszPath, MAX_PATH, _T(PRODUCT_NAME) L"_%d.%d.%d.%d.reg", dwLeftMost, dwSecondLeft, dwSecondRight, dwRightMost);
                                OPENFILENAMEW ofn;
                                ZeroMemory(&ofn, sizeof(OPENFILENAMEW));
                                ofn.lStructSize = sizeof(OPENFILENAMEW);
                                ofn.hwndOwner = hwnd;
                                ofn.hInstance = GetModuleHandleW(NULL);
                                ofn.lpstrFilter = filter;
                                ofn.lpstrCustomFilter = NULL;
                                ofn.nMaxCustFilter = 0;
                                ofn.nFilterIndex = 1;
                                ofn.lpstrFile = wszPath;
                                ofn.nMaxFile = MAX_PATH;
                                ofn.lpstrFileTitle = NULL;
                                ofn.nMaxFileTitle = 0;
                                ofn.lpstrInitialDir = NULL;
                                ofn.lpstrTitle = title;
                                ofn.Flags = OFN_DONTADDTORECENT | OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT | OFN_FILEMUSTEXIST;
                                ofn.nFileOffset = 0;
                                ofn.nFileExtension = 0;
                                ofn.lpstrDefExt = L"reg";
                                ofn.lCustData = NULL;
                                ofn.lpfnHook = NULL;
                                ofn.lpTemplateName = NULL;
                                if (GetOpenFileNameW(&ofn))
                                {
                                    if (Registry_ImportFile(wszPath))
                                    {
                                        _this->tabOrder = 0;
                                        InvalidateRect(hwnd, NULL, FALSE);
                                    }
                                    else
                                    {
                                        wchar_t mbText[256];
                                        mbText[0] = 0;
                                        LoadStringW(hModule, IDS_IMPORT_FAILED, mbText, ARRAYSIZE(mbText));
                                        MessageBoxW(hwnd, mbText, GUI_title, MB_ICONERROR);
                                    }
                                }
                            }
                            else if (!strncmp(line + 1, "cm_", 3) || !strncmp(line + 1, "cd_", 3))
                            {
                                // The user's own menu entries. They are drawn as ordinary lines of this page,
                                // so acting on one only has to change the stored list and redraw: the next
                                // pass expands the list again and the rows follow.
                                if (SP_HandleCustomMenuAction(hwnd, line + 1))
                                {
                                    // The engine inside explorer watches the product key, so the entries reach
                                    // the taskbar menu without restarting anything.
                                    _this->tabOrder = 0;
                                    _this->bCalcExtent = 2;
                                    _this->last_section = _this->section;
                                    InvalidateRect(hwnd, NULL, FALSE);
                                }
                            }
                            else if (!strncmp(line + 1, "uninstall", 9))
                            {
                                HWND hwndExistingMb = FindWindowExW(NULL, NULL, L"#32770", _T(PRODUCT_NAME));
                                if (hwndExistingMb)
                                {
                                    SwitchToThisWindow(hwndExistingMb, TRUE);
                                }
                                else
                                {
                                    wchar_t uninstallLink[MAX_PATH];
                                    ZeroMemory(uninstallLink, sizeof(uninstallLink));
                                    SHGetFolderPathW(NULL, SPECIAL_FOLDER, NULL, SHGFP_TYPE_CURRENT, uninstallLink);
                                    wcscat_s(uninstallLink, MAX_PATH, _T(APP_RELATIVE_PATH) L"\\" _T(SETUP_UTILITY_NAME));

                                    SHELLEXECUTEINFOW sei;
                                    ZeroMemory(&sei, sizeof(SHELLEXECUTEINFOW));
                                    sei.cbSize = sizeof(sei);
                                    sei.hwnd = hwnd;
                                    sei.lpFile = uninstallLink;
                                    sei.nShow = SW_NORMAL;
                                    sei.lpParameters = L"/uninstall";
                                    ShellExecuteExW(&sei);
                                }
                            }
                        }
                    }
                    dwMaxHeight += dwLineHeight * dy;
                    if (!strncmp(line, ";u ", 3) || !strncmp(line, ";m ", 3))
                    {
                        tabOrder++;
                    }
                }
                else if (!strncmp(line, ";l ", 3) || !strncmp(line, ";y ", 3) || !strncmp(line, ";c ", 3) || !strncmp(line, ";o ", 3) || !strncmp(line, ";w ", 3) || !strncmp(line, ";z ", 3) || !strncmp(line, ";b ", 3) || !strncmp(line, ";i ", 3) || !strncmp(line, ";d ", 3) || !strncmp(line, ";v ", 3))
                {
                    ZeroMemory(text, (MAX_LINE_LENGTH + 3) * sizeof(wchar_t));
                    text[0] = L'\u2795';
                    text[1] = L' ';
                    text[2] = L' ';
                    MultiByteToWideChar(
                        CP_UTF8,
                        0,
                        !strncmp(line, ";c ", 3) || !strncmp(line, ";o ", 3) || !strncmp(line, ";z ", 3) ? strchr(line + 3, ' ') + 1 : line + 3,
                        numChRd - 3,
                        text + 3,
                        MAX_LINE_LENGTH - 3
                    );

                    wchar_t* x = wcschr(text, L'\n');
                    if (x) *x = 0;
                    x = wcschr(text, L'\r');
                    if (x) *x = 0;
                    GUI_SubstituteLocalizedString(text + 3, MAX_LINE_LENGTH - 3);
                    if (!strncmp(line, ";w ", 3) || !strncmp(line, ";c ", 3) || !strncmp(line, ";o ", 3) || !strncmp(line, ";z ", 3) || !strncmp(line, ";b ", 3) || !strncmp(line, ";i ", 3) || !strncmp(line, ";d ", 3) || !strncmp(line, ";v ", 3))
                    {
                        WCHAR* wszTitle = NULL;
                        WCHAR* wszPrompt = NULL;
                        WCHAR* wszDefault = NULL;
                        WCHAR* wszFallbackDefault = NULL;
                        HMENU hMenu = NULL;
                        BOOL bInput = !strncmp(line, ";w ", 3);
                        BOOL bChoice = !strncmp(line, ";c ", 3);
                        BOOL bChoiceLefted = !strncmp(line, ";z ", 3);
                        BOOL bInvert = !strncmp(line, ";i ", 3);
                        BOOL bJustCheck = !strncmp(line, ";d ", 3);
                        BOOL bBool = !strncmp(line, ";b ", 3);
                        BOOL bValue = !strncmp(line, ";v ", 3);
                        // A drop-down of preset strings for a REG_SZ value:
                        //     ;o <count> <title>
                        //     ;x <value>|<label>      (an empty value deletes the registry value)
                        //     ;x *|<label>[|<prompt>] (asks for a value of the user's own; the prompt is the
                        //                              hint shown in the box, the title when left out)
                        //     "Name"="default"
                        // A value that matches no preset is shown as it is.
                        BOOL bStrChoice = !strncmp(line, ";o ", 3);
                        DWORD numChoices = 0;
                        if (bChoice || bChoiceLefted)
                        {
                            char* p = strchr(line + 3, ' ');
                            if (p) *p = 0;
                            numChoices = atoi(line + 3);
                            hMenu = CreatePopupMenu();
                            if (numChoices == 10001)
                            {
                                GUI_PopulateLanguageSelectorMenu(hMenu);
                            }
                            else
                            {
                                for (unsigned int i = 0; i < numChoices; ++i)
                                {
                                    char* l = malloc(MAX_LINE_LENGTH * sizeof(char));
                                    numChRd = getline(&l, &bufsiz, f);
                                    if (strncmp(l, ";x ", 3))
                                    {
                                        free(l);
                                        i--;
                                        continue;
                                    }
                                    char* p = strchr(l + 3, ' ');
                                    if (p) *p = 0;
                                    char* ln = p + 1;
                                    p = strchr(p + 1, '\r');
                                    if (p) *p = 0;
                                    p = strchr(p + 1, '\n');
                                    if (p) *p = 0;

                                    wchar_t* miText = malloc(MAX_PATH * sizeof(wchar_t));
                                    MultiByteToWideChar(
                                        CP_UTF8,
                                        0,
                                        ln,
                                        MAX_PATH,
                                        miText,
                                        MAX_PATH
                                    );
                                    GUI_SubstituteLocalizedString(miText, MAX_PATH);

                                    MENUITEMINFOW menuInfo;
                                    ZeroMemory(&menuInfo, sizeof(MENUITEMINFOW));
                                    menuInfo.cbSize = sizeof(MENUITEMINFOW);
                                    menuInfo.fMask = MIIM_ID | MIIM_STRING | MIIM_DATA | MIIM_STATE;
                                    menuInfo.wID = atoi(l + 3) + 1;
                                    menuInfo.dwItemData = l;
                                    menuInfo.fType = MFT_STRING;
                                    menuInfo.dwTypeData = miText;
                                    menuInfo.cch = strlen(ln);
                                    InsertMenuItemW(
                                        hMenu,
                                        i,
                                        TRUE,
                                        &menuInfo
                                    );

                                    free(miText);
                                }
                            }
                        }
                        else if (bStrChoice)
                        {
                            char* p = strchr(line + 3, ' ');
                            if (p) *p = 0;
                            numChoices = atoi(line + 3);
                            hMenu = CreatePopupMenu();
                            wszTitle = calloc(MAX_LINE_LENGTH, sizeof(WCHAR));
                            wszPrompt = calloc(MAX_LINE_LENGTH, sizeof(WCHAR));
                            wszDefault = calloc(MAX_LINE_LENGTH, sizeof(WCHAR));
                            wszFallbackDefault = calloc(MAX_LINE_LENGTH, sizeof(WCHAR));
                            for (unsigned int i = 0; i < numChoices; ++i)
                            {
                                char* l = malloc(MAX_LINE_LENGTH * sizeof(char));
                                numChRd = getline(&l, &bufsiz, f);
                                if (numChRd == (size_t)-1)
                                {
                                    free(l);
                                    break;
                                }
                                if (strncmp(l, ";x ", 3))
                                {
                                    free(l);
                                    i--;
                                    continue;
                                }
                                char* q = strchr(l, '\r');
                                if (q) *q = 0;
                                q = strchr(l, '\n');
                                if (q) *q = 0;
                                // The buffer becomes "value\0label\0prompt\0" from l + 3 on; the prompt is
                                // empty when the line has none. The buffer is far larger than any line, so the
                                // extra terminator always fits.
                                char* bar = strchr(l + 3, '|');
                                char* ln = l + 3;
                                if (bar)
                                {
                                    *bar = 0;
                                    ln = bar + 1;
                                }
                                char* bar2 = strchr(ln, '|');
                                if (bar2)
                                {
                                    *bar2 = 0;
                                }
                                else
                                {
                                    ln[strlen(ln) + 1] = 0;
                                }

                                wchar_t* miText = calloc(MAX_PATH, sizeof(wchar_t));
                                MultiByteToWideChar(CP_UTF8, 0, ln, -1, miText, MAX_PATH);
                                GUI_SubstituteLocalizedString(miText, MAX_PATH);

                                MENUITEMINFOW menuInfo;
                                ZeroMemory(&menuInfo, sizeof(MENUITEMINFOW));
                                menuInfo.cbSize = sizeof(MENUITEMINFOW);
                                menuInfo.fMask = MIIM_ID | MIIM_STRING | MIIM_DATA | MIIM_STATE;
                                menuInfo.wID = i + 1;
                                menuInfo.dwItemData = (ULONG_PTR)l;
                                menuInfo.fType = MFT_STRING;
                                menuInfo.dwTypeData = miText;
                                menuInfo.cch = (UINT)wcslen(miText);
                                InsertMenuItemW(hMenu, i, TRUE, &menuInfo);
                                free(miText);
                            }
                        }
                        else if (bInput)
                        {
                            wszTitle = malloc(MAX_LINE_LENGTH * sizeof(WCHAR));
                            wszPrompt = malloc(MAX_LINE_LENGTH * sizeof(WCHAR));
                            wszDefault = malloc(MAX_LINE_LENGTH * sizeof(WCHAR));
                            wszFallbackDefault = malloc(MAX_LINE_LENGTH * sizeof(WCHAR));
                            char* l = malloc(MAX_LINE_LENGTH * sizeof(char));
                            numChRd = getline(&l, &bufsiz, f);
                            char* p = l;
                            p = strchr(p + 1, '\r');
                            if (p) *p = 0;
                            p = strchr(p + 1, '\n');
                            if (p) *p = 0;
                            MultiByteToWideChar(
                                CP_UTF8,
                                0,
                                l + 1,
                                numChRd - 1,
                                wszPrompt,
                                MAX_LINE_LENGTH
                            );
                            GUI_SubstituteLocalizedString(wszPrompt, MAX_LINE_LENGTH);
                            numChRd = getline(&l, &bufsiz, f);
                            p = l;
                            p = strchr(p + 1, '\r');
                            if (p) *p = 0;
                            p = strchr(p + 1, '\n');
                            if (p) *p = 0;
                            MultiByteToWideChar(
                                CP_UTF8,
                                0,
                                l + 1,
                                numChRd - 1,
                                wszFallbackDefault,
                                MAX_LINE_LENGTH
                            );
                            GUI_SubstituteLocalizedString(wszFallbackDefault, MAX_LINE_LENGTH);
                            free(l);
                        }
                        numChRd = getline(&line, &bufsiz, f);
                        if (!strncmp(line, ";\"Virtualized_" APP_CLSID, 52))
                        {
                            for (unsigned int kkkk = 1; kkkk < MAX_LINE_LENGTH; ++kkkk)
                            {
                                if (line[kkkk])
                                {
                                    line[kkkk - 1] = line[kkkk];
                                }
                                else
                                {
                                    line[kkkk - 1] = 0;
                                    break;
                                }
                            }
                            ////////printf("%s\n", line);
                        }
                        ZeroMemory(name, MAX_LINE_LENGTH * sizeof(wchar_t));
                        MultiByteToWideChar(
                            CP_UTF8,
                            0,
                            line[0] == '"' ? line + 1 : line,
                            numChRd,
                            name,
                            MAX_LINE_LENGTH
                        );
                        wchar_t* d = wcschr(name, L'=');
                        if (d) *d = 0;
                        wchar_t* p = wcschr(name, L'"');
                        if (p) *p = 0;
                        HKEY hKey = NULL;
                        wchar_t* matchHKLM = wcsstr(section, L"HKEY_LOCAL_MACHINE");
                        BOOL bIsHKLM = matchHKLM && (matchHKLM - section) < 3;
                        DWORD dwDisposition;
                        DWORD dwSize = sizeof(DWORD);
                        DWORD value = FALSE;

                        //wprintf(L"%s %s %s\n", section, name, d + 1);
                        if (!bInput && !wcsncmp(d + 1, L"dword:", 6))
                        {
                            wchar_t* x = wcschr(d + 1, L':');
                            x++;
                            value = wcstol(x, NULL, 16);
                        }
                        if (bInput || bStrChoice)
                        {
                            wchar_t* x = wcschr(d + 2, L'"');
                            x[0] = 0;
                            wcscpy_s(wszDefault, MAX_LINE_LENGTH, d + 2);
                        }

                        if (bInput || bStrChoice)
                        {
                            dwSize = MAX_LINE_LENGTH * sizeof(WCHAR);
                            GUI_RegCreateKeyExW(
                                bIsHKLM ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER,
                                wcschr(section, L'\\') + 1,
                                0,
                                NULL,
                                REG_OPTION_NON_VOLATILE,
                                KEY_READ | (hDC ? 0 : (!bIsHKLM ? KEY_WRITE : 0)),
                                NULL,
                                & hKey,
                                & dwDisposition
                            );
                            if (hKey == NULL || hKey == INVALID_HANDLE_VALUE)
                            {
                                hKey = NULL;
                            }
                            GUI_RegQueryValueExW(
                                hKey,
                                name,
                                0,
                                NULL,
                                wszDefault,
                                &dwSize
                            );
                        }
                        else if (!bJustCheck)
                        {
                            GUI_RegCreateKeyExW(
                                bIsHKLM ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER,
                                wcschr(section, L'\\') + 1,
                                0,
                                NULL,
                                REG_OPTION_NON_VOLATILE,
                                KEY_READ | (hDC ? 0 : (!bIsHKLM ? KEY_WRITE : 0)),
                                NULL,
                                &hKey,
                                &dwDisposition
                            );
                            if (hKey == NULL || hKey == INVALID_HANDLE_VALUE)
                            {
                                hKey = NULL;
                            }
                            GUI_RegQueryValueExW(
                                hKey,
                                name,
                                0,
                                NULL,
                                &value,
                                &dwSize
                            );
                            if (hDC && bInvert)
                            {
                                value = !value;
                            }
                        }
                        else
                        {
                            GUI_RegOpenKeyExW(
                                bIsHKLM ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER,
                                wcschr(section, L'\\') + 1,
                                REG_OPTION_NON_VOLATILE,
                                KEY_READ | (hDC ? 0 : (!bIsHKLM ? KEY_WRITE : 0)),
                                &hKey
                            );
                            if (hKey == NULL || hKey == INVALID_HANDLE_VALUE)
                            {
                                hKey = NULL;
                            }
                            value = hKey;
                        }
                        if (bInput)
                        {
                            wcscpy_s(wszTitle, MAX_LINE_LENGTH, text + 3);
                            wcscat_s(
                                text,
                                MAX_LINE_LENGTH,
                                L" : "
                            );
                            if (wszDefault[0] == 0)
                            {
                                wcscat_s(text, MAX_LINE_LENGTH, wszFallbackDefault);
                            }
                            else if (!wcscmp(wszPrompt, L"@image") && wcsrchr(wszDefault, L'\\'))
                            {
                                wcscat_s(text, MAX_LINE_LENGTH, wcsrchr(wszDefault, L'\\') + 1);
                            }
                            else
                            {
                                wcscat_s(
                                    text,
                                    MAX_LINE_LENGTH,
                                    wszDefault
                                );
                            }
                        }
                        else if (bStrChoice)
                        {
                            wcscpy_s(wszTitle, MAX_LINE_LENGTH, text + 3);
                            wcscpy_s(wszPrompt, MAX_LINE_LENGTH, text + 3);
                            wcscat_s(text, MAX_LINE_LENGTH, L" : ");
                            BOOL bMatched = FALSE;
                            for (int i = 0; i < GetMenuItemCount(hMenu); ++i)
                            {
                                MENUITEMINFOW menuInfo;
                                ZeroMemory(&menuInfo, sizeof(MENUITEMINFOW));
                                menuInfo.cbSize = sizeof(MENUITEMINFOW);
                                menuInfo.fMask = MIIM_DATA;
                                if (!GetMenuItemInfoW(hMenu, i, TRUE, &menuInfo) || !menuInfo.dwItemData)
                                {
                                    continue;
                                }
                                WCHAR wszValue[MAX_PATH];
                                MultiByteToWideChar(CP_UTF8, 0, (char*)menuInfo.dwItemData + 3, -1, wszValue, MAX_PATH);
                                if (!bMatched && wcscmp(wszValue, L"*") && !wcscmp(wszValue, wszDefault))
                                {
                                    bMatched = TRUE;
                                    WCHAR wszLabel[MAX_PATH] = { 0 };
                                    GetMenuStringW(hMenu, i, wszLabel, MAX_PATH, MF_BYPOSITION);
                                    wcscat_s(text, MAX_LINE_LENGTH, wszLabel);
                                    CheckMenuItem(hMenu, i, MF_BYPOSITION | MF_CHECKED);
                                }
                            }
                            if (!bMatched)
                            {
                                // A value of the user's own: shown as it is, and the custom entry is ticked.
                                wcscat_s(text, MAX_LINE_LENGTH, wszDefault);
                                for (int i = 0; i < GetMenuItemCount(hMenu); ++i)
                                {
                                    MENUITEMINFOW menuInfo;
                                    ZeroMemory(&menuInfo, sizeof(MENUITEMINFOW));
                                    menuInfo.cbSize = sizeof(MENUITEMINFOW);
                                    menuInfo.fMask = MIIM_DATA;
                                    if (GetMenuItemInfoW(hMenu, i, TRUE, &menuInfo) && menuInfo.dwItemData &&
                                        !strcmp((char*)menuInfo.dwItemData + 3, "*"))
                                    {
                                        CheckMenuItem(hMenu, i, MF_BYPOSITION | MF_CHECKED);
                                    }
                                }
                            }
                            wcscat_s(text, MAX_LINE_LENGTH, L" \u25BE");
                        }
                        else if (bInvert || bBool || bJustCheck)
                        {
                            if (value)
                            {
                                text[0] = L'\u2714';
                            }
                            else
                            {
                                text[0] = L'\u274C';
                            }
                            text[1] = L' ';
                            text[2] = L' ';
                        }
                        else if (bValue)
                        {
                            wcscat_s(
                                text,
                                MAX_LINE_LENGTH,
                                L" : "
                            );
                            wchar_t buf[100];
                            _itow_s(value, buf, 100, 10);
                            wcscat_s(
                                text,
                                MAX_LINE_LENGTH,
                                buf
                            );
                        }
                        else if (bChoice || bChoiceLefted)
                        {
                            wcscat_s(
                                text,
                                MAX_LINE_LENGTH,
                                L" : "
                            );
                            MENUITEMINFOW menuInfo;
                            ZeroMemory(&menuInfo, sizeof(MENUITEMINFOW));
                            menuInfo.cbSize = sizeof(MENUITEMINFOW);
                            menuInfo.fMask = MIIM_STRING;
                            int vvv = value + 1;
                            GetMenuItemInfoW(hMenu, vvv, FALSE, &menuInfo);
                            menuInfo.cch += 1;
                            menuInfo.dwTypeData = text + wcslen(text);
                            GetMenuItemInfoW(hMenu, vvv, FALSE, &menuInfo);
                            ZeroMemory(&menuInfo, sizeof(MENUITEMINFOW));
                            menuInfo.cbSize = sizeof(MENUITEMINFOW);
                            menuInfo.fMask = MIIM_STATE;
                            menuInfo.fState = MFS_CHECKED;
                            SetMenuItemInfoW(hMenu, vvv, FALSE, &menuInfo);
                            wcscat_s(text, MAX_LINE_LENGTH, L" \u25BE");
                        }
                        if (hDC && !bInvert && !bBool && !bJustCheck)
                        {
                            RECT rcTemp;
                            rcTemp = rcText;
                            DrawTextW(
                                hdcPaint,
                                text,
                                3,
                                &rcTemp,
                                DT_CALCRECT
                            );
                            rcText.left += (!bChoiceLefted ? (rcTemp.right - rcTemp.left) : 0);
                            for (unsigned int i = 0; i < wcslen(text); ++i)
                            {
                                text[i] = text[i + 3];
                            }
                        }
                        RECT rcTemp;
                        rcTemp = rcText;
                        DrawTextW(
                            hdcPaint,
                            text,
                            -1,
                            &rcTemp,
                            DT_CALCRECT
                        );
                        rcTemp.bottom = rcText.bottom;
                        if (!hDC && (PtInRect(&rcTemp, pt) || (pt.x == 0 && pt.y == 0 && tabOrder == _this->tabOrder)))
                        {
                            if (bJustCheck)
                            {
                                {
                                    if (hKey)
                                    {
                                        RegCloseKey(hKey);
                                        hKey = NULL;
                                        RegDeleteKeyExW(
                                            bIsHKLM ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER,
                                            wcschr(section, L'\\') + 1,
                                            REG_OPTION_NON_VOLATILE,
                                            0
                                        );
                                    }
                                    else
                                    {
                                        GUI_RegCreateKeyExW(
                                            bIsHKLM ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER,
                                            wcschr(section, L'\\') + 1,
                                            0,
                                            NULL,
                                            REG_OPTION_NON_VOLATILE,
                                            KEY_WRITE,
                                            NULL,
                                            &hKey,
                                            &dwDisposition
                                        );
                                        if (hKey == NULL || hKey == INVALID_HANDLE_VALUE)
                                        {
                                            hKey = NULL;
                                        }
                                        if (d[1] == '"')
                                        {
                                            wchar_t* p = wcschr(d + 2, L'"');
                                            if (p) *p = 0;
                                            GUI_RegSetValueExW(
                                                hKey,
                                                !wcsncmp(name, L"@", 1) ? NULL : name,
                                                0,
                                                REG_SZ,
                                                d + 2,
                                                wcslen(d + 2) * sizeof(wchar_t)
                                            );
                                        }
                                    }
                                }
                            }
                            else
                            {
                                DWORD val = 0;
                                if (bInput && !wcscmp(wszPrompt, L"@image"))
                                {
                                    // ";w <title>" + ";@image": the value is the path of a picture, chosen with the
                                    // Windows file picker rather than typed.
                                    WCHAR wszFile[MAX_PATH] = { 0 };
                                    if (wszDefault[0])
                                    {
                                        wcscpy_s(wszFile, MAX_PATH, wszDefault);
                                    }
                                    OPENFILENAMEW ofn;
                                    ZeroMemory(&ofn, sizeof(ofn));
                                    ofn.lStructSize = sizeof(ofn);
                                    ofn.hwndOwner = hwnd;
                                    ofn.lpstrFilter = L"JPG, PNG, BMP, GIF, WebP, TIFF\0*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.webp;*.tif;*.tiff;*.heic\0\0";
                                    ofn.lpstrFile = wszFile;
                                    ofn.nMaxFile = MAX_PATH;
                                    ofn.lpstrTitle = wszTitle;
                                    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_DONTADDTORECENT;
                                    if (GetOpenFileNameW(&ofn))
                                    {
                                        GUI_RegSetValueExW(hKey, name, 0, REG_SZ, wszFile, (wcslen(wszFile) + 1) * sizeof(WCHAR));
                                        Sleep(100);
                                    }
                                }
                                else if (bInput)
                                {
                                    WCHAR* wszAnswer = calloc(MAX_LINE_LENGTH, sizeof(WCHAR));
                                    BOOL bWasCancelled = FALSE;
                                    if (SUCCEEDED(InputBox(FALSE, hwnd, wszPrompt, wszTitle, wszDefault, wszAnswer, MAX_LINE_LENGTH, &bWasCancelled)) && !bWasCancelled)
                                    {
                                        if (wszAnswer[0])
                                        {
                                            GUI_RegSetValueExW(
                                                hKey,
                                                name,
                                                0,
                                                REG_SZ,
                                                wszAnswer,
                                                (wcslen(wszAnswer) + 1) * sizeof(WCHAR)
                                            );
                                        }
                                        else
                                        {
                                            RegDeleteValueW(hKey, name);
                                        }
                                        Sleep(100);
                                    }
                                    free(wszAnswer);
                                }
                                else if (bStrChoice)
                                {
                                    RECT rcTemp;
                                    rcTemp = rcText;
                                    DrawTextW(hdcPaint, text, 3, &rcTemp, DT_CALCRECT);
                                    POINT p;
                                    p.x = rcText.left + (rcTemp.right - rcTemp.left);
                                    p.y = rcText.bottom;
                                    ClientToScreen(hwnd, &p);
                                    UINT cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd, 0);
                                    MENUITEMINFOW menuInfo;
                                    ZeroMemory(&menuInfo, sizeof(MENUITEMINFOW));
                                    menuInfo.cbSize = sizeof(MENUITEMINFOW);
                                    menuInfo.fMask = MIIM_DATA;
                                    if (cmd > 0 && GetMenuItemInfoW(hMenu, cmd, FALSE, &menuInfo) && menuInfo.dwItemData)
                                    {
                                        WCHAR* wszAnswer = calloc(MAX_LINE_LENGTH, sizeof(WCHAR));
                                        BOOL bWrite = TRUE;
                                        if (!strcmp((char*)menuInfo.dwItemData + 3, "*"))
                                        {
                                            const char* label = (char*)menuInfo.dwItemData + 3 + 2;
                                            const char* prompt = label + strlen(label) + 1;
                                            if (prompt[0])
                                            {
                                                MultiByteToWideChar(CP_UTF8, 0, prompt, -1, wszPrompt, MAX_LINE_LENGTH);
                                                GUI_SubstituteLocalizedString(wszPrompt, MAX_LINE_LENGTH);
                                            }
                                            BOOL bWasCancelled = FALSE;
                                            bWrite = SUCCEEDED(InputBox(FALSE, hwnd, wszPrompt, wszTitle, wszDefault, wszAnswer, MAX_LINE_LENGTH, &bWasCancelled)) && !bWasCancelled;
                                        }
                                        else
                                        {
                                            MultiByteToWideChar(CP_UTF8, 0, (char*)menuInfo.dwItemData + 3, -1, wszAnswer, MAX_LINE_LENGTH);
                                        }
                                        if (bWrite)
                                        {
                                            if (wszAnswer[0])
                                            {
                                                GUI_RegSetValueExW(hKey, name, 0, REG_SZ, wszAnswer, (wcslen(wszAnswer) + 1) * sizeof(WCHAR));
                                            }
                                            else
                                            {
                                                RegDeleteValueW(hKey, name);
                                            }
                                            Sleep(100);
                                        }
                                        free(wszAnswer);
                                    }
                                    KillTimer(hwnd, GUI_TIMER_READ_REPEAT_SELECTION);
                                    SetTimer(hwnd, GUI_TIMER_READ_REPEAT_SELECTION, GUI_TIMER_READ_REPEAT_SELECTION_TIMEOUT, NULL);
                                }
                                else if (bChoice || bChoiceLefted)
                                {
                                    RECT rcTemp;
                                    rcTemp = rcText;
                                    DrawTextW(
                                        hdcPaint,
                                        text,
                                        3,
                                        &rcTemp,
                                        DT_CALCRECT
                                    );
                                    POINT p;
                                    p.x = rcText.left + (bChoiceLefted ? 0 : (rcTemp.right - rcTemp.left));
                                    p.y = rcText.bottom;
                                    ClientToScreen(
                                        hwnd,
                                        &p
                                    );
                                    val = TrackPopupMenu(
                                        hMenu, 
                                        TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                        p.x,
                                        p.y,
                                        0,
                                        hwnd,
                                        0
                                    );
                                    if (val > 0) value = val - 1;
                                    KillTimer(hwnd, GUI_TIMER_READ_REPEAT_SELECTION);
                                    SetTimer(hwnd, GUI_TIMER_READ_REPEAT_SELECTION, GUI_TIMER_READ_REPEAT_SELECTION_TIMEOUT, NULL);

                                }
                                else if (bValue)
                                {

                                }
                                else
                                {
                                    value = !value;
                                }
                                if (!wcscmp(name, L"LastSectionInProperties") && wcsstr(section, _T(REGPATH)) && value)
                                {
                                    value = _this->section + 1;
                                }
                                if (!bInput && !bStrChoice && (!(bChoice || bChoiceLefted) || ((bChoice || bChoiceLefted) && val)))
                                {
                                    GUI_RegSetValueExW(
                                        hKey,
                                        name,
                                        0,
                                        REG_DWORD,
                                        &value,
                                        sizeof(DWORD)
                                    );
                                    // The taskbar re-reads its button switches (search box, task view, widgets,
                                    // seconds) when told the tray settings changed, which is what the Settings
                                    // app broadcasts after writing the same values.
                                    if (wcsstr(section, L"Explorer\\Advanced") || wcsstr(section, L"CurrentVersion\\Search"))
                                    {
                                        SendNotifyMessageW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"TraySettings");
                                    }
                                    if (!wcscmp(name, L"Language"))
                                    {
                                        GUI_UpdateLanguages();
                                        POINT pt;
                                        pt.x = 0;
                                        pt.y = 0;
                                        _this->bCalcExtent = TRUE;
                                        GUI_Build(0, hwnd, pt);
                                    }
                                }
                            }
                            InvalidateRect(hwnd, NULL, FALSE);
                        }
                        if (hKey)
                        {
                            RegCloseKey(hKey);
                        }
                        if (bChoice || bChoiceLefted || bStrChoice)
                        {
                            for (unsigned int i = 0; ; ++i)
                            {
                                MENUITEMINFOW menuInfo;
                                ZeroMemory(&menuInfo, sizeof(MENUITEMINFOW));
                                menuInfo.cbSize = sizeof(MENUITEMINFOW);
                                menuInfo.fMask = MIIM_DATA;
                                if (!GetMenuItemInfoW(hMenu, i, TRUE, &menuInfo))
                                {
                                    break;
                                }
                                if (menuInfo.dwItemData)
                                {
                                    free(menuInfo.dwItemData);
                                }
                            }
                            DestroyMenu(hMenu);
                        }
                        if (wszTitle)
                        {
                            free(wszTitle);
                        }
                        if (wszPrompt)
                        {
                            free(wszPrompt);
                        }
                        if (wszDefault)
                        {
                            free(wszDefault);
                        }
                        if (wszFallbackDefault)
                        {
                            free(wszFallbackDefault);
                        }
                    }
                    if (hDC && (!strncmp(line, ";l ", 3) || !strncmp(line, ";y ", 3)))
                    {
                        RECT rcTemp;
                        rcTemp = rcText;
                        DrawTextW(
                            hdcPaint,
                            text,
                            3,
                            &rcTemp,
                            DT_CALCRECT
                        );
                        rcText.left += (!strncmp(line, ";l ", 3) ? (rcTemp.right - rcTemp.left) : 0);
                        for (unsigned int i = 0; i < wcslen(text); ++i)
                        {
                            text[i] = text[i + 3];
                        }
                    }
                    if (!hDC && (!strncmp(line, ";l ", 3) || !strncmp(line, ";y ", 3)))
                    {
                        RECT rcTemp;
                        rcTemp = rcText;
                        DrawTextW(
                            hdcPaint,
                            text,
                            -1,
                            &rcTemp,
                            DT_CALCRECT
                        );
                        rcTemp.bottom = rcText.bottom;
                        //printf("%d %d %d %d %d %d %d %d\n", rcText.left, rcText.top, rcText.right, rcText.bottom, rcTemp.left, rcTemp.top, rcTemp.right, rcTemp.bottom);
                        if (PtInRect(&rcTemp, pt) || (pt.x == 0 && pt.y == 0 && tabOrder == _this->tabOrder))
                        {
                            numChRd = getline(&line, &bufsiz, f);
                            char* p = strchr(line, '\r');
                            if (p) *p = 0;
                            p = strchr(line, '\n');
                            if (p) *p = 0;
                            if (line[1] != 0)
                            {
                                if (line[1] == ';')
                                {
                                    // Lines of the form ";;NAME" are internal commands. None are defined yet.
                                }
                                else
                                {
                                    ShellExecuteA(
                                        NULL,
                                        "open",
                                        line + 1,
                                        NULL,
                                        NULL,
                                        SW_SHOWNORMAL
                                    );
                                }
                            }
                        }
                    }
                    if (hDC)
                    {
                        COLORREF cr;
                        if (tabOrder == _this->tabOrder)
                        {
                            bTabOrderHit = TRUE;
                            if (_this->bEnsureFocusVisible && lViewBottom > lViewTop)
                            {
                                _this->bEnsureFocusVisible = FALSE;
                                int nDelta = 0;
                                if (rcScrollLine.top < lViewTop) nDelta = rcScrollLine.top - lViewTop;
                                else if (rcScrollLine.bottom > lViewBottom) nDelta = rcScrollLine.bottom - lViewBottom;
                                if (nDelta)
                                {
                                    _this->scrollY = max(0, _this->scrollY + nDelta);
                                    InvalidateRect(hwnd, NULL, FALSE);
                                }
                            }
                            if (!IsThemeActive() || IsHighContrast())
                            {
                                cr = SetTextColor(hdcPaint, GetSysColor(COLOR_HIGHLIGHT));
                            }
                            else
                            {
                                DttOpts.crText = g_darkModeEnabled ? GUI_TEXTCOLOR_SELECTED_DARK : GUI_TEXTCOLOR_SELECTED;
                                //DttOpts.crText = GetSysColor(COLOR_HIGHLIGHT);
                            }
                        }
                        RECT rcNew = rcText;
                        DrawTextW(
                            hdcPaint,
                            text,
                            -1,
                            &rcNew,
                            DT_CALCRECT
                        );
                        if (rcNew.right - rcNew.left > dwMaxWidth)
                        {
                            dwMaxWidth = rcNew.right - rcNew.left + 50 * dx;
                        }
                        if (tabOrder == _this->tabOrder)
                        {
                            if (_this->bShouldAnnounceSelected)
                            {
                                unsigned int accLen = wcslen(text);
                                DWORD dwType = 0;
                                if (!strncmp(line, ";y ", 3))
                                {
                                    dwType = 4;
                                }
                                if (text[0] == L'\u2714') dwType = 1;
                                else if (text[0] == L'\u274C') dwType = 2;
                                else if (text[accLen - 1] == 56405) dwType = 3;
                                else if (!strstr(line, "dword")) dwType = 5;
                                WCHAR accText[1000], accText2[1000];
                                ZeroMemory(accText, 1000 * sizeof(wchar_t));
                                ZeroMemory(accText2, 1000 * sizeof(wchar_t));
                                swprintf_s(
                                    accText,
                                    1000,
                                    L"%s %s %s: %s",
                                    (_this->dwPageLocation < 0 ?
                                        L"Reached end of the page." :
                                        (_this->dwPageLocation > 0 ?
                                            L"Reached beginning of the page." : L"")),
                                    (lastHeading[0] == 0) ? L"" : lastHeading,
                                    (dwType == 1 || dwType == 2) ? text + 1 : text,
                                    dwType == 1 ? L"Enabled" :
                                    (dwType == 2 ? L"Disabled" :
                                        (dwType == 3 ? L"Link" :
                                            (dwType == 4 ? L"Button" :
                                                (dwType == 5 ? L"Input" : 
                                                    L"List"))))
                                );
                                accLen = wcslen(accText);
                                unsigned int j = 0;
                                for (unsigned int i = 0; i < accLen; ++i)
                                {
                                    if (accText[i] == L'%')
                                    {
                                        accText2[j] = L'%';
                                        accText2[j + 1] = L'%';
                                        j++;
                                    }
                                    else
                                    {
                                        accText2[j] = accText[i];
                                    }
                                    ++j;
                                }
                                _this->dwPageLocation = 0;
                                BOOL dwTypeRepl = 0;
                                accLen = wcslen(accText2);
                                for (unsigned int i = 0; i < accLen; ++i)
                                {
                                    if (accText2[i] == L'*')
                                    {
                                        if (accText2[i + 1] == L'*')
                                        {
                                            dwTypeRepl = 1;
                                        }
                                        accText2[i] = L'%';
                                        if (i + 1 >= accLen)
                                        {
                                            accText2[i + 2] = 0;
                                        }
                                        accText2[i + 1] = L's';
                                    }
                                }
                                if (dwTypeRepl == 1)
                                {
                                    swprintf_s(accText, 1000, accText2, L" - Requires registration as shell extension to work in Open or Save file dialogs - ");
                                }
                                else
                                {
                                    swprintf_s(accText, 1000, accText2, L" - Requires File Explorer restart to apply - ");
                                }
                                //wprintf(L">>> %s\n", accText);
                                SetWindowTextW(_this->hAccLabel, accText);
                                NotifyWinEvent(
                                    EVENT_OBJECT_LIVEREGIONCHANGED,
                                    _this->hAccLabel,
                                    OBJID_CLIENT,
                                    CHILDID_SELF);
                                _this->bShouldAnnounceSelected = FALSE;
                            }
                        }
                        // boolean options are drawn as toggle switches instead of check / cross glyphs
                        const WCHAR* pszDrawText = text;
                        RECT rcDrawText = rcText;
                        if ((text[0] == L'\u2714' || text[0] == L'\u274C') && IsThemeActive() && !IsHighContrast())
                        {
                            int cxSwitch = GUI_DrawToggleSwitch(hdcPaint, &rcText, text[0] == L'\u2714', tabOrder == _this->tabOrder, g_darkModeEnabled, dx, dy);
                            if (cxSwitch > 0)
                            {
                                pszDrawText = text + 3;
                                rcDrawText.left += cxSwitch;
                            }
                        }
                        if (IsThemeActive() && !IsHighContrast())
                        {
                            DrawThemeTextEx(
                                _this->hTheme,
                                hdcPaint,
                                0,
                                0,
                                pszDrawText,
                                -1,
                                dwTextFlags,
                                &rcDrawText,
                                &DttOpts
                            );
                        }
                        else
                        {
                            DrawTextW(
                                hdcPaint,
                                pszDrawText,
                                -1,
                                &rcDrawText,
                                dwTextFlags
                            );
                        }
                        if (tabOrder == _this->tabOrder)
                        {
                            if (!IsThemeActive() || IsHighContrast())
                            {
                                SetTextColor(hdcPaint, cr);
                            }
                            else
                            {
                                DttOpts.crText = g_darkModeEnabled ? GUI_TEXTCOLOR_DARK : GUI_TEXTCOLOR;
                                //DttOpts.crText = GetSysColor(COLOR_WINDOWTEXT);
                            }
                        }
                    }
                    dwMaxHeight += dwLineHeight * dy;
                    tabOrder++;
                }
            }
            if (hOldFont)
            {
                SelectObject(hdcPaint, hOldFont);
            }
        }
        fclose(f);
        if (bContentClip)
        {
            RestoreDC(hdcPaint, -1);
            bContentClip = FALSE;
        }
        if (!_this->bCalcExtent && !AuditFile && lViewBottom > lViewTop)
        {
            int nMaxScroll = (int)(lContentBottom + (LONG)(GUI_PADDING * dy) - lViewBottom);
            _this->maxScroll = nMaxScroll > 0 ? nMaxScroll : 0;
            if (_this->scrollY > _this->maxScroll)
            {
                _this->scrollY = _this->maxScroll;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            if (hDC && _this->maxScroll > 0)
            {
                // Minimal scroll indicator at the right edge of the viewport.
                LONG lViewHeight = lViewBottom - lViewTop;
                LONG lThumbHeight = max((LONG)(24 * dy), lViewHeight * lViewHeight / (lViewHeight + _this->maxScroll));
                RECT rcThumb;
                rcThumb.right = rc.right - (LONG)(4 * dx);
                rcThumb.left = rcThumb.right - max(2, (LONG)(3 * dx));
                rcThumb.top = lViewTop + (LONG)((LONGLONG)(lViewHeight - lThumbHeight) * _this->scrollY / _this->maxScroll);
                rcThumb.bottom = rcThumb.top + lThumbHeight;
                GUI_FillRectAlpha(hdcPaint, &rcThumb, g_darkModeEnabled ? 0x70FFFFFF : 0x70000000);
            }
        }
        else if (!_this->bCalcExtent && !AuditFile)
        {
            _this->maxScroll = 0;
        }
        free(section);
        free(name);
        free(text);
        free(line);
        free(lastHeading);
        if (!bWasSpecifiedSectionValid)
        {
            _this->bRebuildIfTabOrderIsEmpty = FALSE;
            _this->tabOrder = 0;
            GUI_SetSection(_this, FALSE, 0);
            InvalidateRect(hwnd, NULL, FALSE);
        }

        if (!hDC)
        {
            ReleaseDC(hwnd, hdcPaint);
        }
        // hFontTitle is an alias for hFontRegular, so it is not deleted separately.
        DeleteObject(hFontSectionSel);
        DeleteObject(hFontSection);
        DeleteObject(hFontHeading);
        DeleteObject(hFontRegular);
        DeleteObject(hFontUnderline);
        DeleteObject(hFontCaption);

        if (_this->bShouldAnnounceSelected)
        {
            int max_section = 100;
            for (unsigned int i = 0; i < 100; ++i)
            {
                if (_this->sectionNames[i][0] == 0)
                {
                    max_section = i - 1;
                    break;
                }
            }
            WCHAR wszAccText[100];
            swprintf_s(
                wszAccText,
                100,
                L"Selected page: %s: %d of %d.",
                _this->sectionNames[_this->section],
                _this->section + 1,
                max_section + 1
            );
            SetWindowTextW(_this->hAccLabel, wszAccText);
            if (!_this->bRebuildIfTabOrderIsEmpty)
            {
                NotifyWinEvent(
                    EVENT_OBJECT_LIVEREGIONCHANGED,
                    _this->hAccLabel,
                    OBJID_CLIENT,
                    CHILDID_SELF
                );
            }
        }

        if (hDC)
        {
            if (_this->tabOrder == GUI_MAX_TABORDER)
            {
                _this->tabOrder = tabOrder - 1;
                _this->dwPageLocation = -1;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            else if (!bTabOrderHit)
            {
                if (_this->bRebuildIfTabOrderIsEmpty)
                {
                    _this->dwPageLocation = 1;
                    _this->bRebuildIfTabOrderIsEmpty = FALSE;
                    _this->tabOrder = 1;
                    InvalidateRect(hwnd, NULL, FALSE);
                }
                else
                {
                    _this->tabOrder = 0;
                }
            }
        }

        if (_this->bRebuildIfTabOrderIsEmpty)
        {
            _this->bRebuildIfTabOrderIsEmpty = FALSE;
        }
    }
    if (_this->bCalcExtent)
    {
        dwMaxWidth += dwInitialLeftPad + _this->padding.left + _this->padding.right;
        if (!IsThemeActive() || IsHighContrast() || !IsWindows11() || IsDwmExtendFrameIntoClientAreaBrokenInThisBuild())
        {
            dwMaxHeight += GUI_LINE_HEIGHT * dy + 20 * dy;
        }
        else
        {
            dwMaxHeight += GUI_PADDING * 2 * dy;
        }

        // Each page is measured in its own pass; the window is sized to the largest one so that switching pages
        // never clips content. (ExplorerPatcher sized the window to the first page only.)
        if (dwMaxWidth > _this->measuredWidth) _this->measuredWidth = dwMaxWidth;
        if (dwMaxHeight > _this->measuredHeight) _this->measuredHeight = dwMaxHeight;
        if (_this->dwStatusbarY > _this->measuredStatusbarY) _this->measuredStatusbarY = _this->dwStatusbarY;
        if (_this->measureSection + 1 < nSectionCount)
        {
            _this->measureSection++;
            _this->section = _this->measureSection;
            EndBufferedPaint(hBufferedPaint, TRUE);
            InvalidateRect(hwnd, NULL, FALSE);
            return TRUE;
        }
        dwMaxWidth = _this->measuredWidth;
        dwMaxHeight = _this->measuredHeight;
        _this->dwStatusbarY = _this->measuredStatusbarY;
        _this->measuredWidth = _this->measuredHeight = _this->measuredStatusbarY = 0;
        _this->measureSection = 0;
        _this->section = 0;
        printf("window extent: %d x %d\n", dwMaxWidth, dwMaxHeight);

        HMONITOR hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi;
        mi.cbSize = sizeof(MONITORINFO);
        GetMonitorInfo(hMonitor, &mi);

        // A page taller than the work area does not grow the window past the screen; it scrolls instead.
        DWORD dwWorkHeight = mi.rcWork.bottom - mi.rcWork.top;
        if (dwMaxHeight > dwWorkHeight)
        {
            DWORD dwBelowContent = dwMaxHeight - (DWORD)(_this->dwStatusbarY * dy);
            dwMaxHeight = dwWorkHeight;
            _this->dwStatusbarY = (DWORD)((dwMaxHeight - dwBelowContent) / dy);
        }

        SetWindowPos(
            hwnd,
            hwnd,
            mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) / 2 - (dwMaxWidth) / 2),
            mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) / 2 - (dwMaxHeight) / 2),
            dwMaxWidth,
            dwMaxHeight,
            SWP_NOZORDER | SWP_NOACTIVATE | (_this->bCalcExtent == 2 ? SWP_NOMOVE : 0)
        );

        if (_this->bCalcExtent != 2)
        {
            DWORD dwReadSection = 0;

            HKEY hKey = NULL;
            DWORD dwSize = sizeof(DWORD);
            RegCreateKeyExW(
                HKEY_CURRENT_USER,
                TEXT(REGPATH),
                0,
                NULL,
                REG_OPTION_NON_VOLATILE,
                KEY_READ | KEY_WOW64_64KEY | KEY_WRITE,
                NULL,
                &hKey,
                NULL
            );
            if (hKey == NULL || hKey == INVALID_HANDLE_VALUE)
            {
                hKey = NULL;
            }
            if (hKey)
            {
                dwReadSection = 0;
                dwSize = sizeof(DWORD);
                RegQueryValueExW(
                    hKey,
                    TEXT("LastSectionInProperties"),
                    0,
                    NULL,
                    &dwReadSection,
                    &dwSize
                );
                if (dwReadSection)
                {
                    _this->section = dwReadSection - 1;
                }
                dwReadSection = 0;
                dwSize = sizeof(DWORD);
                RegQueryValueExW(
                    hKey,
                    TEXT("OpenPropertiesAtNextStart"),
                    0,
                    NULL,
                    &dwReadSection,
                    &dwSize
                );
                if (dwReadSection)
                {
                    _this->section = dwReadSection - 1;
                    dwReadSection = 0;
                    RegSetValueExW(
                        hKey,
                        TEXT("OpenPropertiesAtNextStart"),
                        0,
                        REG_DWORD,
                        &dwReadSection,
                        sizeof(DWORD)
                    );
                }
                RegCloseKey(hKey);
            }
        }
        if (_this->bCalcExtent == 2)
        {
            _this->section = _this->last_section;
        }
        
        _this->bCalcExtent = 0;
        InvalidateRect(hwnd, NULL, FALSE);
    }

    EndBufferedPaint(hBufferedPaint, TRUE);
    return TRUE;
}

LRESULT CALLBACK GUI_WndProc(GUI* pThis, HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_CREATE:
        {
            UINT dpiX, dpiY, dpiXP, dpiYP;
            POINT ptCursor, ptZero;
            ptZero.x = 0;
            ptZero.y = 0;
            GetCursorPos(&ptCursor);
            HMONITOR hMonitor = MonitorFromPoint(ptCursor, MONITOR_DEFAULTTOPRIMARY);
            HMONITOR hPrimaryMonitor = MonitorFromPoint(ptZero, MONITOR_DEFAULTTOPRIMARY);
            HRESULT hr = GetDpiForMonitor(
                hMonitor,
                MDT_DEFAULT,
                &dpiX,
                &dpiY
            );
            hr = GetDpiForMonitor(
                hPrimaryMonitor,
                MDT_DEFAULT,
                &dpiXP,
                &dpiYP
            );
            MONITORINFO mi;
            mi.cbSize = sizeof(MONITORINFO);
            GetMonitorInfoW(hMonitor, &mi);
            double dx = dpiX / 96.0, dy = dpiY / 96.0, dxp = dpiXP / 96.0, dyp = dpiYP / 96.0;
            pThis->dpi.x = dpiX;
            pThis->dpi.y = dpiY;

            SetRect(&pThis->border_thickness, 2, 2, 2, 2);
            if (IsThemeActive() && IsWindows11() && !IsDwmExtendFrameIntoClientAreaBrokenInThisBuild())
            {
                BOOL bIsCompositionEnabled = TRUE;
                DwmIsCompositionEnabled(&bIsCompositionEnabled);
                if (bIsCompositionEnabled)
                {
                    MARGINS marGlassInset;
                    if (!IsHighContrast())
                    {
                        // Extend the glass frame into the whole window
                        marGlassInset.cxLeftWidth = -1;
                        marGlassInset.cxRightWidth = -1;
                        marGlassInset.cyBottomHeight = -1;
                        marGlassInset.cyTopHeight = -1;
                    }
                    else
                    {
                        marGlassInset.cxLeftWidth = 0;
                        marGlassInset.cxRightWidth = 0;
                        marGlassInset.cyBottomHeight = 0;
                        marGlassInset.cyTopHeight = 0;
                    }
                    DwmExtendFrameIntoClientArea(hWnd, &marGlassInset);
                }
            }
            SetWindowPos(
                hWnd,
                hWnd,
                mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) / 2 - (pThis->size.cx * dx) / 2),
                mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) / 2 - (pThis->size.cy * dy) / 2),
                pThis->size.cx * dxp,
                pThis->size.cy * dyp,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED
            );
            SetTimer(hWnd, GUI_TIMER_READ_HELP, GUI_TIMER_READ_HELP_TIMEOUT, NULL);

            if (IsThemeActive() && !IsHighContrast() && IsWindows11() && !IsDwmExtendFrameIntoClientAreaBrokenInThisBuild())
            {
                RECT rcTitle;
                DwmGetWindowAttribute(hWnd, DWMWA_CAPTION_BUTTON_BOUNDS, &rcTitle, sizeof(RECT));
                pThis->GUI_CAPTION_LINE_HEIGHT = rcTitle.bottom - rcTitle.top;
            }
            else
            {
                pThis->GUI_CAPTION_LINE_HEIGHT = GUI_CAPTION_LINE_HEIGHT_DEFAULT;
            }
            if (IsThemeActive() && ShouldAppsUseDarkMode && !IsHighContrast())
            {
                AllowDarkModeForWindow(hWnd, g_darkModeEnabled);
                BOOL value = g_darkModeEnabled;
                int s = 0;
                if (global_rovi.dwBuildNumber < 18985)
                {
                    s = -1;
                }
                DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE + s, &value, sizeof(BOOL));
            }
            if (!IsThemeActive() || IsHighContrast() || !IsWindows11() || IsDwmExtendFrameIntoClientAreaBrokenInThisBuild())
            {
                int extendedStyle = GetWindowLongPtrW(hWnd, GWL_EXSTYLE);
                SetWindowLongPtrW(hWnd, GWL_EXSTYLE, extendedStyle | WS_EX_DLGMODALFRAME);
            }
            break;
        }
        case WM_DESTROY:
        {
            PostQuitMessage(0);
            return 0;
        }
        case WM_GETICON:
        {
            return pThis->hIcon;
        }
        case WM_SETTINGCHANGE:
        {
            if (IsColorSchemeChangeMessage(lParam))
            {
                if (IsThemeActive() && IsWindows11() && !IsDwmExtendFrameIntoClientAreaBrokenInThisBuild())
                {
                    BOOL bIsCompositionEnabled = TRUE;
                    DwmIsCompositionEnabled(&bIsCompositionEnabled);
                    if (bIsCompositionEnabled)
                    {
                        MARGINS marGlassInset;
                        if (!IsHighContrast())
                        {
                            // Extend the glass frame into the whole window
                            marGlassInset.cxLeftWidth = -1;
                            marGlassInset.cxRightWidth = -1;
                            marGlassInset.cyBottomHeight = -1;
                            marGlassInset.cyTopHeight = -1;
                        }
                        else
                        {
                            marGlassInset.cxLeftWidth = 0;
                            marGlassInset.cxRightWidth = 0;
                            marGlassInset.cyBottomHeight = 0;
                            marGlassInset.cyTopHeight = 0;
                        }
                        DwmExtendFrameIntoClientArea(hWnd, &marGlassInset);
                    }
                }

                BOOL fCompositionEnabled = TRUE;
                DwmIsCompositionEnabled(&fCompositionEnabled);
                if (fCompositionEnabled)
                {
                    BOOL fApply = (IsThemeActive() && !IsHighContrast() && IsWindows11()) ? 1 : 0;
                    SetMicaMaterialForThisWindow(hWnd, fApply);
                }
                if (IsThemeActive() && ShouldAppsUseDarkMode && !IsHighContrast())
                {
                    RefreshImmersiveColorPolicyState();
                    BOOL bDarkModeEnabled = IsThemeActive() && fCompositionEnabled && ShouldAppsUseDarkMode() && !IsHighContrast();
                    if (bDarkModeEnabled != g_darkModeEnabled)
                    {
                        g_darkModeEnabled = bDarkModeEnabled;
                        AllowDarkModeForWindow(hWnd, g_darkModeEnabled);
                        BOOL value = g_darkModeEnabled;
                        int s = 0;
                        if (global_rovi.dwBuildNumber < 18985)
                        {
                            s = -1;
                        }
                        DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE + s, &value, sizeof(BOOL));
                        pThis->bCalcExtent = 2;
                        pThis->last_section = pThis->section;
                        pThis->section = 0;
                        InvalidateRect(hWnd, NULL, FALSE);
                    }
                }
                else
                {
                    pThis->bCalcExtent = 2;
                    pThis->last_section = pThis->section;
                    pThis->section = 0;
                    InvalidateRect(hWnd, NULL, FALSE);
                }
            }
            break;
        }
        case WM_MOUSEWHEEL:
        {
            // scroll the page content
            if (pThis->maxScroll > 0)
            {
                int nStep = (int)(GUI_LINE_HEIGHT * (pThis->dpi.y / 96.0) * 2.0);
                int nScrollY = pThis->scrollY - (GET_WHEEL_DELTA_WPARAM(wParam) * nStep) / WHEEL_DELTA;
                nScrollY = max(0, min(pThis->maxScroll, nScrollY));
                if (nScrollY != pThis->scrollY)
                {
                    pThis->scrollY = nScrollY;
                    InvalidateRect(hWnd, NULL, FALSE);
                }
            }
            return 0;
        }
        case WM_KEYDOWN:
        {
            pThis->bRebuildIfTabOrderIsEmpty = FALSE;
            switch (wParam)
            {
                case VK_PRIOR:
                case VK_NEXT:
                case VK_HOME:
                case VK_END:
                {
                    if (pThis->maxScroll > 0)
                    {
                        int nPage = (int)(GUI_LINE_HEIGHT * (pThis->dpi.y / 96.0) * 8.0);
                        int nScrollY = wParam == VK_HOME ? 0 : (wParam == VK_END ? pThis->maxScroll : pThis->scrollY + (wParam == VK_NEXT ? nPage : -nPage));
                        pThis->scrollY = max(0, min(pThis->maxScroll, nScrollY));
                        InvalidateRect(hWnd, NULL, FALSE);
                    }
                    return 0;
                }
                case VK_ESCAPE:
                {
                    PostMessageW(hWnd, WM_CLOSE, 0, 0);
                    return 0;
                }
                case VK_TAB:
                case VK_DOWN:
                case VK_UP:
                {
                    if ((GetKeyState(VK_SHIFT) & 0x8000) || wParam == VK_UP)
                    {
                        if (pThis->tabOrder == 0)
                        {
                            pThis->tabOrder = GUI_MAX_TABORDER;
                        }
                        else
                        {
                            pThis->tabOrder--;
                            if (pThis->tabOrder == 0)
                            {
                                pThis->tabOrder = GUI_MAX_TABORDER;
                            }
                        }
                    }
                    else
                    {
                        pThis->tabOrder++;
                    }
                    pThis->bRebuildIfTabOrderIsEmpty = TRUE;
                    pThis->bShouldAnnounceSelected = TRUE;
                    pThis->bEnsureFocusVisible = TRUE;
                    InvalidateRect(hWnd, NULL, FALSE);
                    return 0;
                }
                case VK_SPACE:
                case VK_RETURN:
                {
                    POINT pt = { 0, 0 };
                    pThis->bShouldAnnounceSelected = TRUE;
                    GUI_Build(0, hWnd, pt);
                    return 0;
                }
                case VK_LEFT:
                case VK_RIGHT:
                {
                    int min_section = 0;
                    int max_section = 100;
                    int new_section = pThis->section;
                    for (unsigned int i = 0; i < 100; ++i)
                    {
                        if (pThis->sectionNames[i][0] == 0)
                        {
                            max_section = i - 1;
                            break;
                        }
                    }
                    if (wParam == VK_LEFT)
                    {
                        new_section--;
                    }
                    else
                    {
                        new_section++;
                    }
                    if (new_section < min_section)
                    {
                        new_section = max_section;
                    }
                    if (new_section > max_section)
                    {
                        new_section = min_section;
                    }
                    if (pThis->section != new_section)
                    {
                        pThis->tabOrder = 0;
                        GUI_SetSection(pThis, TRUE, new_section);
                        pThis->bShouldAnnounceSelected = TRUE;
                        InvalidateRect(hWnd, NULL, FALSE);
                    }
                    return 0;
                }
                case 'H':
                case VK_F1:
                {
                    SetTimer(hWnd, GUI_TIMER_READ_HELP, 200, NULL);
                    return 0;
                }
                case VK_F5:
                {
                    InvalidateRect(hWnd, NULL, FALSE);
                    return 0;
                }
                case 'Z':
                {
                    return 0;
                }
                case 'X':
                {
                    return 0;
                }
            }
            if (wParam >= '1' && wParam <= '9' || wParam == '0' || wParam == MapVirtualKeyW(0x0C, MAPVK_VSC_TO_VK_EX)
                || wParam == MapVirtualKeyW(0x0D, MAPVK_VSC_TO_VK_EX))
            {
                int min_section = 0;
                int max_section = 100;
                for (unsigned int i = 0; i < 100; ++i)
                {
                    if (pThis->sectionNames[i][0] == 0)
                    {
                        max_section = i - 1;
                        break;
                    }
                }
                int new_section = 0;
                if (wParam == MapVirtualKeyW(0x0C, MAPVK_VSC_TO_VK_EX)) new_section = 10;
                else if (wParam == MapVirtualKeyW(0x0D, MAPVK_VSC_TO_VK_EX)) new_section = 11;
                else new_section = (wParam == '0' ? 9 : wParam - '1');
                if (new_section < min_section) return 0;
                if (new_section > max_section) return 0;
                if (pThis->section != new_section)
                {
                    pThis->tabOrder = 0;
                    GUI_SetSection(pThis, TRUE, new_section);
                    pThis->bShouldAnnounceSelected = TRUE;
                    InvalidateRect(hWnd, NULL, FALSE);
                }
                return 0;
            }
            break;
        }
        case WM_NCMOUSELEAVE:
        {
            if (IsThemeActive() && !IsHighContrast() && IsWindows11()
                && !IsDwmExtendFrameIntoClientAreaBrokenInThisBuild())
            {
                LRESULT lRes = 0;
                if (DwmDefWindowProc(hWnd, uMsg, wParam, lParam, &lRes))
                {
                    return lRes;
                }
            }
            break;
        }
        case WM_NCRBUTTONUP:
        {
            if (IsThemeActive() && !IsHighContrast() && IsWindows11()
                && !IsDwmExtendFrameIntoClientAreaBrokenInThisBuild())
            {
                HMENU pSysMenu = GetSystemMenu(hWnd, FALSE);
                if (pSysMenu != NULL)
                {
                    int xPos = GET_X_LPARAM(lParam);
                    int yPos = GET_Y_LPARAM(lParam);
                    EnableMenuItem(pSysMenu, SC_RESTORE, MF_GRAYED);
                    EnableMenuItem(pSysMenu, SC_SIZE, MF_GRAYED);
                    EnableMenuItem(pSysMenu, SC_MAXIMIZE, MF_GRAYED);
                    BOOL cmd = TrackPopupMenu(
                        pSysMenu, TPM_LEFTALIGN | TPM_LEFTBUTTON | TPM_RIGHTBUTTON | TPM_RETURNCMD, xPos, yPos, NULL,
                        hWnd, 0);
                    if (cmd)
                    {
                        PostMessageW(hWnd, WM_SYSCOMMAND, cmd, 0);
                    }
                }
                return 0;
            }
            break;
        }
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        {
            if (IsThemeActive() && !IsHighContrast() && IsWindows11()
                && !IsDwmExtendFrameIntoClientAreaBrokenInThisBuild())
            {
                POINT pt;
                pt.x = GET_X_LPARAM(lParam);
                pt.y = GET_Y_LPARAM(lParam);

                double dx = pThis->dpi.x / 96.0, dy = pThis->dpi.y / 96.0;
                UINT diff = (int)(((pThis->GUI_CAPTION_LINE_HEIGHT - 16) * dx) / 2.0);
                RECT rc;
                SetRect(&rc, diff, diff, diff + (int)(16.0 * dx), diff + (int)(16.0 * dy));
                if (PtInRect(&rc, pt))
                {
                    if (uMsg == WM_LBUTTONUP && pThis->LeftClickTime != 0)
                    {
                        pThis->LeftClickTime = milliseconds_now() - pThis->LeftClickTime;
                    }
                    if (uMsg == WM_LBUTTONUP && pThis->LeftClickTime != 0 && pThis->LeftClickTime < GetDoubleClickTime())
                    {
                        pThis->LeftClickTime = 0;
                        PostQuitMessage(0);
                    }
                    else
                    {
                        if (uMsg == WM_LBUTTONUP)
                        {
                            pThis->LeftClickTime = milliseconds_now();
                        }
                        if (uMsg == WM_RBUTTONUP || !pThis->LastClickTime || milliseconds_now() - pThis->LastClickTime > 500)
                        {
                            HMENU pSysMenu = GetSystemMenu(hWnd, FALSE);
                            if (pSysMenu != NULL)
                            {
                                if (uMsg == WM_LBUTTONUP)
                                {
                                    pt.x = 0;
                                    pt.y = pThis->GUI_CAPTION_LINE_HEIGHT * dy;
                                }
                                ClientToScreen(hWnd, &pt);
                                EnableMenuItem(pSysMenu, SC_RESTORE, MF_GRAYED);
                                EnableMenuItem(pSysMenu, SC_SIZE, MF_GRAYED);
                                EnableMenuItem(pSysMenu, SC_MAXIMIZE, MF_GRAYED);
                                BOOL cmd = TrackPopupMenu(pSysMenu, TPM_LEFTALIGN | TPM_LEFTBUTTON | TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, NULL, hWnd, 0);
                                if (cmd)
                                {
                                    PostMessageW(hWnd, WM_SYSCOMMAND, cmd, 0);
                                }
                                if (uMsg == WM_LBUTTONUP)
                                {
                                    pThis->LastClickTime = milliseconds_now();
                                }
                            }
                        }
                    }
                    return 0;
                }
            }
            break;
        }
        case WM_NCHITTEST:
        {
            if (IsThemeActive() && !IsHighContrast() && IsWindows11()
                && !IsDwmExtendFrameIntoClientAreaBrokenInThisBuild())
            {
                LRESULT lRes = 0;
                if (DwmDefWindowProc(hWnd, uMsg, wParam, lParam, &lRes))
                {
                    return lRes;
                }

                POINT pt;
                pt.x = GET_X_LPARAM(lParam);
                pt.y = GET_Y_LPARAM(lParam);
                ScreenToClient(hWnd, &pt);

                double dx = pThis->dpi.x / 96.0, dy = pThis->dpi.y / 96.0;
                UINT diff = (int)(((pThis->GUI_CAPTION_LINE_HEIGHT - 16) * dx) / 2.0);
                RECT rc;
                SetRect(&rc, diff, diff, diff + (int)(16.0 * dx), diff + (int)(16.0 * dy));
                if (PtInRect(&rc, pt))
                {
                    return HTCLIENT;
                }

                if (pt.y < pThis->extent.cyTopHeight)
                {
                    return HTCAPTION;
                }
            }
            break;
        }
        case WM_NCCALCSIZE:
        {
            if (wParam == TRUE && IsThemeActive() && !IsHighContrast() && IsWindows11()
                && !IsDwmExtendFrameIntoClientAreaBrokenInThisBuild())
            {
                NCCALCSIZE_PARAMS* sz = (NCCALCSIZE_PARAMS*)lParam;
                sz->rgrc[0].left += pThis->border_thickness.left;
                sz->rgrc[0].right -= pThis->border_thickness.right;
                sz->rgrc[0].bottom -= pThis->border_thickness.bottom;
                return 0;
            }
            break;
        }
        case WM_LBUTTONDOWN:
        {
            POINT pt;
            pt.x = GET_X_LPARAM(lParam);
            pt.y = GET_Y_LPARAM(lParam);
            GUI_Build(0, hWnd, pt);
            //InvalidateRect(hWnd, NULL, FALSE);
            break;
        }
        case WM_DPICHANGED:
        {
            pThis->dpi.x = LOWORD(wParam);
            pThis->dpi.y = HIWORD(wParam);

            RECT* rc = (RECT*)lParam;
            SetWindowPos(
                hWnd,
                hWnd,
                rc->left,
                rc->top,
                rc->right - rc->left,
                rc->bottom - rc->top,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS
            );
            RECT rcTitle;
            DwmGetWindowAttribute(hWnd, DWMWA_CAPTION_BUTTON_BOUNDS, &rcTitle, sizeof(RECT));
            pThis->GUI_CAPTION_LINE_HEIGHT = (rcTitle.bottom - rcTitle.top) * (96.0 / pThis->dpi.y);
            return 0;
        }
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hDC = BeginPaint(hWnd, &ps);

            RECT rc;
            GetClientRect(hWnd, &rc);

            POINT pt = { 0, 0 };
            GUI_Build(hDC, hWnd, pt);

            EndPaint(hWnd, &ps);
            return 0;
        }
        case WM_INPUTLANGCHANGE:
        {
            InvalidateRect(hWnd, NULL, FALSE);
            return 0;
        }
        case WM_MSG_GUI_SECTION:
        {
            if (wParam == WM_MSG_GUI_SECTION_GET)
            {
                return pThis->section + 1;
            }
            break;
        }
        /*case WM_USER + 1: // same value as WM_MSG_GUI_SECTION; used to be called by PeopleButton_CalculateMinimumSizeHook()
        {
            SetTimer(hWnd, GUI_TIMER_REFRESH_FOR_PEOPLEBAND, GUI_TIMER_REFRESH_FOR_PEOPLEBAND_TIMEOUT, NULL);
            return 0;
        }*/
        case WM_TIMER:
        {
            switch (wParam)
            {
                case GUI_TIMER_READ_HELP:
                {
                    PlayHelpMessage(pThis);
                    KillTimer(hWnd, GUI_TIMER_READ_HELP);
                    break;
                }
                case GUI_TIMER_READ_REPEAT_SELECTION:
                {
                    pThis->bShouldAnnounceSelected = TRUE;
                    InvalidateRect(hWnd, NULL, FALSE);
                    KillTimer(hWnd, GUI_TIMER_READ_REPEAT_SELECTION);
                    break;
                }
                case GUI_TIMER_ENGINE_STATUS:
                {
                    InvalidateRect(hWnd, NULL, FALSE);
                    if (++pThis->engineStatusTicks >= GUI_TIMER_ENGINE_STATUS_TICKS)
                    {
                        KillTimer(hWnd, GUI_TIMER_ENGINE_STATUS);
                    }
                    break;
                }
                /*case GUI_TIMER_REFRESH_FOR_PEOPLEBAND: // Same value as GUI_TIMER_READ_REPEAT_SELECTION
                {
                    InvalidateRect(hWnd, NULL, FALSE);
                    KillTimer(hWnd, GUI_TIMER_REFRESH_FOR_PEOPLEBAND);
                    return 0;
                }*/
            }
            break;
        }
    }

    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK s_GUI_WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    GUI* pThis = (GUI*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
    if (uMsg == WM_NCCREATE)
    {
        CREATESTRUCTW* pcs = (CREATESTRUCTW*)lParam;
        pThis = (GUI*)pcs->lpCreateParams;
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)pThis);
        // pThis->hWnd = hWnd;
    }
    if (pThis)
    {
        return GUI_WndProc(pThis, hWnd, uMsg, wParam, lParam);
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

__declspec(dllexport) int ZZGUI(HWND hWnd, HINSTANCE hInstance, LPSTR lpszCmdLine, int nCmdShow)
{
    HWND hOther = NULL;
    if (hOther = FindWindowW(_T(GUI_WINDOW_CLASS), NULL))
    {
        SwitchToThisWindow(hOther, TRUE);
        return 0;
    }

    HRESULT hr = CoInitializeEx(0, COINIT_APARTMENTTHREADED);

    HKEY hKey = NULL;
    DWORD dwSize = sizeof(DWORD);
    RegCreateKeyExW(
        HKEY_CURRENT_USER,
        TEXT(REGPATH),
        0,
        NULL,
        REG_OPTION_NON_VOLATILE,
        KEY_READ | KEY_WOW64_64KEY,
        NULL,
        &hKey,
        NULL
    );
    if (hKey == NULL || hKey == INVALID_HANDLE_VALUE)
    {
        hKey = NULL;
    }
    DWORD bAllocConsole = FALSE;
    if (hKey)
    {
        dwSize = sizeof(DWORD);
        RegQueryValueExW(
            hKey,
            TEXT("AllocConsole"),
            0,
            NULL,
            &bAllocConsole,
            &dwSize
        );
        if (bAllocConsole)
        {
            FILE* conout;
            AllocConsole();
            freopen_s(
                &conout,
                "CONOUT$",
                "w",
                stdout
            );
        }
    }

    wprintf(L"Running on Windows %d, OS Build %d.%d.%d.%d.\n", IsWindows11() ? 11 : 10, global_rovi.dwMajorVersion, global_rovi.dwMinorVersion, global_rovi.dwBuildNumber, global_ubr);

    GUI_UpdateLanguages();

    wchar_t wszPath[MAX_PATH];
    ZeroMemory(
        wszPath,
        (MAX_PATH) * sizeof(char)
    );
    GetModuleFileNameW(hModule, wszPath, MAX_PATH);
    PathRemoveFileSpecW(wszPath);
    wcscat_s(
        wszPath,
        MAX_PATH,
        L"\\settings.reg"
    );
    wprintf(L"%s\n", wszPath);
    if (FileExistsW(wszPath))
    {
        HANDLE hFile = CreateFileW(
            wszPath,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            0
        );
        if (hFile)
        {
            HANDLE hFileMapping = CreateFileMappingW(hFile, NULL, PAGE_READWRITE, 0, 0, NULL);
            if (hFileMapping)
            {
                GUI_FileMapping = MapViewOfFile(hFileMapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
                GUI_FileSize = GetFileSize(hFile, NULL);
            }
        }
    }

    printf("Started \"GUI\" thread.\n");

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    GUI _this;
    ZeroMemory(&_this, sizeof(GUI));
    _this.hBackgroundBrush = (HBRUSH)(CreateSolidBrush(RGB(255, 255, 255)));// (HBRUSH)GetStockObject(BLACK_BRUSH);
    _this.location.x = GUI_POSITION_X;
    _this.location.y = GUI_POSITION_Y;
    _this.size.cx = GUI_POSITION_WIDTH;
    _this.size.cy = GUI_POSITION_HEIGHT;
    _this.padding.left = GUI_PADDING_LEFT;
    _this.padding.right = GUI_PADDING_RIGHT;
    _this.padding.top = GUI_PADDING_TOP;
    _this.padding.bottom = GUI_PADDING_BOTTOM;
    _this.sidebarWidth = GUI_SIDEBAR_WIDTH;
    _this.hTheme = OpenThemeData(NULL, TEXT(GUI_WINDOWSWITCHER_THEME_CLASS));
    _this.tabOrder = 0;
    _this.bCalcExtent = 1;
    _this.section = 0;
    _this.dwStatusbarY = 0;
    _this.scrollY = 0;
    _this.maxScroll = 0;
    _this.bEnsureFocusVisible = FALSE;
    _this.hIcon = NULL;

    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof(wc);
    wc.style = 0;// CS_DBLCLKS;
    wc.lpfnWndProc = s_GUI_WndProc;
    wc.hbrBackground = _this.hBackgroundBrush;
    wc.hInstance = hModule;
    wc.lpszClassName = _T(GUI_WINDOW_CLASS);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);

    // The product icon, from this module's own resources. Two sizes are loaded rather than one: Windows picks
    // hIconSm for the title bar and hIcon for Alt+Tab, and letting it scale one into the other is what makes an
    // icon look smudged. LoadImage with the requested metric picks the matching frame out of the .ico.
    wc.hIcon = (HICON)LoadImageW(hModule, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                 GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageW(hModule, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);

    // The caption row draws the icon itself at 16 device-independent pixels, so it asks for that size scaled to
    // the system DPI instead of reusing one of the two above and letting GDI stretch it.
    int captionIconSize = MulDiv(16, GetDpiForSystem(), 96);
    _this.hIcon = (HICON)LoadImageW(hModule, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                    captionIconSize, captionIconSize, 0);
    if (!_this.hIcon)
    {
        _this.hIcon = wc.hIconSm;
    }

    RegisterClassExW(&wc);

    LoadStringW(hModule, IDS_PRODUCTNAME, GUI_title, 260);

    BOOL bIsCompositionEnabled = TRUE;
    DwmIsCompositionEnabled(&bIsCompositionEnabled);
    HANDLE hUxtheme = NULL;
    BOOL bHasLoadedUxtheme = FALSE;
    bHasLoadedUxtheme = TRUE;
    hUxtheme = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (hUxtheme)
    {
        RefreshImmersiveColorPolicyState = (RefreshImmersiveColorPolicyState_t)GetProcAddress(hUxtheme, (LPCSTR)104);
        SetPreferredAppMode = (SetPreferredAppMode_t)GetProcAddress(hUxtheme, (LPCSTR)135);
        AllowDarkModeForWindow = (AllowDarkModeForWindow_t)GetProcAddress(hUxtheme, (LPCSTR)133);
        ShouldAppsUseDarkMode = (ShouldAppsUseDarkMode_t)GetProcAddress(hUxtheme, (LPCSTR)132);
        if (ShouldAppsUseDarkMode &&
            SetPreferredAppMode &&
            AllowDarkModeForWindow &&
            RefreshImmersiveColorPolicyState
            )
        {
            SetPreferredAppMode(TRUE);
            RefreshImmersiveColorPolicyState();
            g_darkModeEnabled = IsThemeActive() && bIsCompositionEnabled && ShouldAppsUseDarkMode() && !IsHighContrast();
        }
    }
    HWND hwnd = CreateWindowEx(
        NULL,
        _T(GUI_WINDOW_CLASS),
        GUI_title,
        WS_SYSMENU | WS_CAPTION | WS_MINIMIZEBOX,
        0,
        0,
        0,
        0,
        NULL, NULL, hModule, &_this
    );
    if (!hwnd)
    {
        return 1;
    }

    _this.hAccLabel = CreateWindowExW(
        0,
        L"Static",
        L"",
        WS_CHILD,
        10,   
        10,   
        100, 
        100, 
        hwnd,
        NULL,
        (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE),
        NULL
    );

    hr = CoCreateInstance(
        &CLSID_AccPropServices,
        NULL,
        CLSCTX_INPROC,
        &IID_IAccPropServices,
        &_this.pAccPropServices);
    if (SUCCEEDED(hr))
    {
        VARIANT var;
        var.vt = VT_I4;
        var.lVal = 2; // Assertive;

        hr = ((IAccPropServices*)(_this.pAccPropServices))->lpVtbl->SetHwndProp(
            _this.pAccPropServices,
            _this.hAccLabel,
            OBJID_CLIENT,
            CHILDID_SELF,
            LiveSetting_Property_GUID,
            var
        );
    }

    if (IsThemeActive() && !IsHighContrast() && IsWindows11())
    {
        if (bIsCompositionEnabled)
        {
            SetMicaMaterialForThisWindow(hwnd, TRUE);
            /*WTA_OPTIONS ops;
            ops.dwFlags = WTNCA_NODRAWCAPTION | WTNCA_NODRAWICON;
            ops.dwMask = WTNCA_NODRAWCAPTION | WTNCA_NODRAWICON;
            SetWindowThemeAttribute(
                hwnd,
                WTA_NONCLIENT,
                &ops,
                sizeof(WTA_OPTIONS)
            );*/
        }
    }
    ShowWindow(hwnd, SW_SHOW);
    if (hKey)
    {
        RegCloseKey(hKey);
    }

    MSG msg = { 0 };
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (_this.pAccPropServices != NULL)
    {
        MSAAPROPID props[] = { LiveSetting_Property_GUID };
        ((IAccPropServices*)(_this.pAccPropServices))->lpVtbl->ClearHwndProps(
            _this.pAccPropServices,
            _this.hAccLabel,
            OBJID_CLIENT,
            CHILDID_SELF,
            props,
            ARRAYSIZE(props));

        ((IAccPropServices*)(_this.pAccPropServices))->lpVtbl->Release(_this.pAccPropServices);
        _this.pAccPropServices = NULL;
    }

    DestroyWindow(_this.hAccLabel);

    // The window class icons belong to the class and are released with it; only the caption icon was loaded
    // separately, and only when it is not simply the small class icon.
    if (_this.hIcon && _this.hIcon != wc.hIconSm)
    {
        DestroyIcon(_this.hIcon);
    }
    _this.hIcon = NULL;

    if (bHasLoadedUxtheme && hUxtheme)
    {
        FreeLibrary(hUxtheme);
    }

    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDOUT);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDOUT);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDOUT);
    _CrtDumpMemoryLeaks();
#ifdef _DEBUG
    _getch();
#endif

    printf("Ended \"GUI\" thread.\n");
}
