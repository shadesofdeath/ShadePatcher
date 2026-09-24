#pragma once
//
// osversion.h - Windows build detection and the DWM helpers that depend on it.
//
#include <Windows.h>
#include <dwmapi.h>

#ifdef __cplusplus
extern "C" {
#endif

// Filled on first use by any of the functions below.
extern RTL_OSVERSIONINFOW global_rovi;
extern DWORD32 global_ubr;   // "Update Build Revision", the number after the build (e.g. 22631.4037 -> 4037)

void InitializeGlobalVersionAndUBR(void);

BOOL IsWindows11(void);
BOOL IsWindows11Version22H2OrHigher(void);
BOOL IsWindows11Version23H2OrHigher(void);

// Early Windows 11 builds could not extend the frame into the client area correctly; the GUI falls back to a
// plain background on those.
BOOL IsDwmExtendFrameIntoClientAreaBrokenInThisBuild(void);

// Applies (or removes) the Mica backdrop on a top-level window. No-op on Windows 10.
HRESULT SetMicaMaterialForThisWindow(HWND hWnd, BOOL bApply);

#ifdef __cplusplus
}
#endif
