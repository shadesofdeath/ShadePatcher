#ifndef _H_GUI_H_
#define _H_GUI_H_
//
// GUI.h - the settings window.
//
// The window is not built from dialog controls; GUI.c draws every line itself from the directives found in
// resources/settings.reg (see docs/settings-format.md). GUI_Build() is the single routine that both paints the
// window (when given a DC) and performs hit testing (when given a point), so drawing and input can never disagree.
//
#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#pragma comment(linker,"\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#include <Windows.h>
#include <Shlwapi.h>
#pragma comment(lib, "Shlwapi.lib")
#include <windowsx.h>
#include <tlhelp32.h>
#include <shellscalingapi.h>
#pragma comment(lib, "Shcore.lib")
#include <conio.h>
#include <stdio.h>
#include <tchar.h>
#include <Uxtheme.h>
#pragma comment(lib, "UxTheme.lib")
#include <dwmapi.h>
#pragma comment(lib, "Dwmapi.lib")
#include <Shlobj.h>
#include <shellapi.h>
#include <commdlg.h>

#include "config.h"
#include "osversion.h"
#include "utils.h"
#include "localization.h"
#include "explorer.h"
#include "inputbox.h"
#include "getline.h"
#include "fmemopen.h"

#include "registry.h"
#include "conditions.h"
#include "custommenu.h"
#include "resources/resource.h"

#define MAX_LINE_LENGTH 2000
extern HMODULE hModule;

// Sent by the core DLL to ask which page is open (wParam = WM_MSG_GUI_SECTION_GET).
#define WM_MSG_GUI_SECTION      (WM_USER + 1)
#define WM_MSG_GUI_SECTION_GET  1

#define GUI_POSITION_X CW_USEDEFAULT
#define GUI_POSITION_Y CW_USEDEFAULT
#define GUI_POSITION_WIDTH 367
#define GUI_POSITION_HEIGHT 316
#define GUI_WINDOWSWITCHER_THEME_CLASS "ControlPanelStyle"
#define GUI_CAPTION_FONT_SIZE -12
#define GUI_SECTION_FONT_SIZE -12
#define GUI_SECTION_HEIGHT 32
#define GUI_TITLE_FONT_SIZE -12
#define GUI_LINE_HEIGHT 26
#define GUI_CAPTION_LINE_HEIGHT_DEFAULT 42
#define GUI_TEXTCOLOR RGB(0, 0, 0)
#define GUI_TEXTCOLOR_SELECTED RGB(255, 0, 0)
#define GUI_TEXTCOLOR_DARK RGB(240, 240, 240)
#define GUI_TEXTCOLOR_SELECTED_DARK RGB(255, 150, 150)
#define GUI_MAX_TABORDER 9999
#define GUI_PADDING 5
#define GUI_PADDING_LEFT GUI_PADDING * 3
#define GUI_SIDEBAR_WIDTH 150
#define GUI_PADDING_RIGHT GUI_PADDING * 3
#define GUI_PADDING_TOP GUI_PADDING
#define GUI_PADDING_BOTTOM GUI_PADDING
#define GUI_STATUS_PADDING 10

#define GUI_TIMER_READ_HELP 1
#define GUI_TIMER_READ_HELP_TIMEOUT 1000
#define GUI_TIMER_READ_REPEAT_SELECTION 2
#define GUI_TIMER_READ_REPEAT_SELECTION_TIMEOUT 1000
// Repaints while the shell is being restarted, so the engine status line follows it; stops by itself.
#define GUI_TIMER_ENGINE_STATUS 3
#define GUI_TIMER_ENGINE_STATUS_TIMEOUT 1000
#define GUI_TIMER_ENGINE_STATUS_TICKS 90

typedef struct _GUI
{
    int engineStatusTicks;     // GUI_TIMER_ENGINE_STATUS: repaints so far
    POINT location;
    SIZE size;
    RECT padding;
    UINT sidebarWidth;
    HBRUSH hBackgroundBrush;
    HTHEME hTheme;
    POINT dpi;
    MARGINS extent;
    UINT tabOrder;
    DWORD bCalcExtent;
    SIZE_T section;
    DWORD dwStatusbarY;
    HICON hIcon;
    RECT border_thickness;
    UINT GUI_CAPTION_LINE_HEIGHT;
    long long LeftClickTime;
    long long LastClickTime;
    void* pAccPropServices;
    HWND hAccLabel;
    BOOL bShouldAnnounceSelected;
    WCHAR sectionNames[20][64];
    BOOL bRebuildIfTabOrderIsEmpty;
    int dwPageLocation;
    DWORD last_section;
    // Vertical scrolling of the page content (pixels)
    int scrollY;
    int maxScroll;
    BOOL bEnsureFocusVisible;
    // Window sizing: every page is measured in turn and the window is sized to the largest one.
    SIZE_T measureSection;
    DWORD measuredWidth;
    DWORD measuredHeight;
    DWORD measuredStatusbarY;
} GUI;

__declspec(dllexport) int ZZGUI(HWND hWnd, HINSTANCE hInstance, LPSTR lpszCmdLine, int nCmdShow);
#endif
