#include "osversion.h"

#pragma comment(lib, "Dwmapi.lib")

RTL_OSVERSIONINFOW global_rovi = { 0 };
DWORD32 global_ubr = 0;

typedef LONG(NTAPI* RtlGetVersion_t)(PRTL_OSVERSIONINFOW);

void InitializeGlobalVersionAndUBR(void)
{
    ZeroMemory(&global_rovi, sizeof(global_rovi));
    global_rovi.dwOSVersionInfoSize = sizeof(global_rovi);

    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    RtlGetVersion_t pRtlGetVersion = hNtdll ? (RtlGetVersion_t)GetProcAddress(hNtdll, "RtlGetVersion") : NULL;
    if (pRtlGetVersion)
    {
        pRtlGetVersion(&global_rovi);
    }

    DWORD dwSize = sizeof(global_ubr);
    global_ubr = 0;
    RegGetValueW(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
        L"UBR",
        RRF_RT_REG_DWORD,
        NULL,
        &global_ubr,
        &dwSize
    );
}

static void EnsureInitialized(void)
{
    if (!global_rovi.dwMajorVersion)
    {
        InitializeGlobalVersionAndUBR();
    }
}

BOOL IsWindows11(void)
{
    EnsureInitialized();
    return global_rovi.dwBuildNumber >= 21996;
}

BOOL IsWindows11Version22H2OrHigher(void)
{
    EnsureInitialized();
    return global_rovi.dwBuildNumber >= 22621;
}

BOOL IsWindows11Version23H2OrHigher(void)
{
    EnsureInitialized();
    return global_rovi.dwBuildNumber >= 22631;
}

BOOL IsDwmExtendFrameIntoClientAreaBrokenInThisBuild(void)
{
    if (!IsWindows11())
    {
        return FALSE;
    }
    if ((global_rovi.dwBuildNumber >= 21996 && global_rovi.dwBuildNumber < 22000) ||
        (global_rovi.dwBuildNumber == 22000 && (global_ubr >= 1 && global_ubr <= 51)))
    {
        return TRUE;
    }
    return FALSE;
}

HRESULT SetMicaMaterialForThisWindow(HWND hWnd, BOOL bApply)
{
    if (!IsWindows11() || IsDwmExtendFrameIntoClientAreaBrokenInThisBuild())
    {
        return S_FALSE;
    }
    // 38 = DWMWA_SYSTEMBACKDROP_TYPE (22523+), 1029 = the undocumented Mica attribute of the first builds.
    DWORD dwAttribute = (global_rovi.dwBuildNumber >= 22523) ? 38 : 1029;
    DWORD dwProp = bApply ? ((global_rovi.dwBuildNumber >= 22523) ? 2 /*DWMSBT_MAINWINDOW*/ : 1) : 0;
    return DwmSetWindowAttribute(hWnd, dwAttribute, &dwProp, sizeof(DWORD));
}
