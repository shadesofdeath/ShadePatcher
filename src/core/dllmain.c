//
// core/dllmain.c - ShadePatcher.dll.
//
// This module is loaded in three quite different ways:
//
//   As the shell patcher. The installer copies it to C:\Windows\dxgi.dll, where explorer.exe loads it in place
//   of the real one (see dxgi_proxy.c); sp_inject and the ZZStartup entry point load it into a running shell
//   instead. Either way DllMain starts the mod engine on its own thread.
//
//   As the settings launcher. rundll32 calls the exported ZZGUI, which opens the settings window from
//   sp_gui.dll. Nothing is hooked in that case.
//
//   As a helper for things the shell cannot do to itself. ZZRestartExplorer restarts the shell and puts the
//   engine back; ZZStartup, run from the user's Run key, loads the engine into a fresh shell after sign-in when
//   the proxy is not installed. Both are meant for rundll32.
//
// DllMain runs under the loader lock, so it does as little as possible: it decides whether this process is one
// the engine belongs in and, if so, hands the work to a thread. Everything else, including loading dbghelp,
// touching the registry and installing hooks, happens there.
//
#include <Windows.h>
#include <Shlwapi.h>
#include <tchar.h>

#include "config.h"
#include "explorer.h"
#include "utils.h"
#include "engine/manager.h"

#pragma comment(lib, "Shlwapi.lib")

static HMODULE g_hModule = NULL;
static HANDLE  g_hEngineThread = NULL;

// The engine needs it for the icon and window class it registers; nothing else reads it.
HMODULE SP_GetCoreModule(void)
{
    return g_hModule;
}

typedef int (*ZZGUI_t)(HWND hWnd, HINSTANCE hInstance, LPSTR lpszCmdLine, int nCmdShow);

// Opens the settings window. The signature is the one rundll32 expects:
//     rundll32 "C:\Program Files\ShadePatcher\ShadePatcher.dll",ZZGUI
__declspec(dllexport) int ZZGUI(HWND hWnd, HINSTANCE hInstance, LPSTR lpszCmdLine, int nCmdShow)
{
    // sp_gui.dll sits next to this DLL in the build and install folders. When this DLL is the proxy copy in
    // the Windows folder it is alone there, and the window is found in the install folder instead.
    wchar_t wszPath[MAX_PATH];
    if (!FindProductFile(TEXT(GUI_DLL_NAME), wszPath, MAX_PATH))
    {
        return 1;
    }

    HMODULE hGui = LoadLibraryW(wszPath);
    if (!hGui)
    {
        return 2;
    }

    ZZGUI_t pfnZZGUI = (ZZGUI_t)GetProcAddress(hGui, "ZZGUI");
    int result = pfnZZGUI ? pfnZZGUI(hWnd, hInstance, lpszCmdLine, nCmdShow) : 3;

    FreeLibrary(hGui);
    return result;
}

// Whether this DLL is the installer's proxy copy, which every new shell loads on its own.
static BOOL IsProxyCopy(void)
{
    wchar_t wszSelf[MAX_PATH];
    wchar_t wszProxy[MAX_PATH];
    if (!GetModuleFileNameW(g_hModule, wszSelf, MAX_PATH) || !GetWindowsDirectoryW(wszProxy, MAX_PATH))
    {
        return FALSE;
    }
    PathAppendW(wszProxy, L"dxgi.dll");
    return _wcsicmp(wszSelf, wszProxy) == 0;
}

// Restarts the shell and puts the engine back the way it was, then reopens the settings window when asked:
//     rundll32 "...\ShadePatcher.dll",ZZRestartExplorer [reopen]
// This exists because the settings window can be running inside explorer.exe (opened from the taskbar menu),
// and a thread in the shell cannot restart the shell and survive to see it come back.
__declspec(dllexport) int ZZRestartExplorer(HWND hWnd, HINSTANCE hInstance, LPSTR lpszCmdLine, int nCmdShow)
{
    UNREFERENCED_PARAMETER(hWnd);
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(nCmdShow);

    if (IsExplorerRestartInProgress())
    {
        // A second click while the first restart is still running; that one reopens the window too.
        return 0;
    }

    // Decided before the shell goes away, while the answer can still be read off it.
    wchar_t wszEngine[MAX_PATH];
    int mode = FindEngineInShell(wszEngine, ARRAYSIZE(wszEngine));

    const wchar_t* reinject = NULL;
    wchar_t wszSelf[MAX_PATH];
    if (mode == SP_ENGINE_INJECTED)
    {
        reinject = wszEngine;
    }
    else if (mode == SP_ENGINE_MISSING && !IsProxyCopy() && GetModuleFileNameW(g_hModule, wszSelf, MAX_PATH))
    {
        // The shell had no engine and the proxy is not installed, so the fresh shell would have none either.
        // This DLL is a real build, so it is what goes in.
        wchar_t wszProxy[MAX_PATH];
        GetWindowsDirectoryW(wszProxy, MAX_PATH);
        PathAppendW(wszProxy, L"dxgi.dll");
        if (!FileExistsW(wszProxy))
        {
            reinject = wszSelf;
        }
    }

    RestartExplorerNow(reinject);

    if (lpszCmdLine && strstr(lpszCmdLine, "reopen"))
    {
        // The window died with the old shell. It comes back in this process, which is how rundll32 opens it
        // anyway, so the user is back where they were.
        Sleep(500);
        ZZGUI(NULL, NULL, NULL, SW_SHOWNORMAL);
    }
    return 0;
}

// Loads the engine into the shell after sign-in when nothing else has:
//     rundll32 "...\ShadePatcher.dll",ZZStartup
// Written to the user's Run key by the "load at sign-in" option. With the proxy installed the shell already has
// the engine and this returns at once, so the option is harmless either way.
__declspec(dllexport) int ZZStartup(HWND hWnd, HINSTANCE hInstance, LPSTR lpszCmdLine, int nCmdShow)
{
    UNREFERENCED_PARAMETER(hWnd);
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(lpszCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    if (IsProxyCopy())
    {
        return 0;
    }

    // Run entries start while the shell is still coming up.
    if (!WaitForShell(120000))
    {
        return 1;
    }
    Sleep(2000);

    if (FindEngineInShell(NULL, 0) != SP_ENGINE_MISSING)
    {
        return 0;
    }

    wchar_t wszSelf[MAX_PATH];
    if (!GetModuleFileNameW(g_hModule, wszSelf, MAX_PATH))
    {
        return 1;
    }

    for (int attempt = 0; attempt < 5; ++attempt)
    {
        if (InjectIntoShell(wszSelf))
        {
            return 0;
        }
        Sleep(2000);
    }
    return 1;
}

// Opens ShadePatcher's own Start menu in this process, with no shell hooks, and returns when it closes:
//     rundll32 "...\ShadePatcher.dll",ZZStartMenuPreview [all] [power] [left] [search=<text>]
// For working on the menu without restarting the shell (src/core/startmenu/view.cpp, RunPreview).
int SP_StartMenuPreview(const char* options);

__declspec(dllexport) int ZZStartMenuPreview(HWND hWnd, HINSTANCE hInstance, LPSTR lpszCmdLine, int nCmdShow)
{
    UNREFERENCED_PARAMETER(hWnd);
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(nCmdShow);
    return SP_StartMenuPreview(lpszCmdLine);
}

// The Start menu's process, into which the shell's engine loads this DLL while a Start-menu mod is on
// (engine/hostinject.c).
static BOOL IsThisProcessTheStartMenuHost(void)
{
    wchar_t wszPath[MAX_PATH];
    if (!GetModuleFileNameW(NULL, wszPath, MAX_PATH))
    {
        return FALSE;
    }
    PathStripPathW(wszPath);
    return _wcsicmp(wszPath, L"StartMenuExperienceHost.exe") == 0;
}

static DWORD WINAPI EngineThread(LPVOID lpParameter)
{
    UNREFERENCED_PARAMETER(lpParameter);
    SP_EngineRun();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    UNREFERENCED_PARAMETER(lpvReserved);

    switch (fdwReason)
    {
        case DLL_PROCESS_ATTACH:
        {
            g_hModule = hinstDLL;

            // Thread attach and detach notifications are of no use here and cost every thread the process
            // creates, so they are turned off.
            DisableThreadLibraryCalls(hinstDLL);

            // The proxy DLL is loaded by anything that draws with DXGI, and the overwhelming majority of those
            // have no mods, so the check is made before a thread is created rather than after.
            if (IsThisProcessTheShell() || IsThisProcessTheStartMenuHost())
            {
                // The thread does not run until the loader lock is released, which is exactly what is wanted.
                g_hEngineThread = CreateThread(NULL, 0, EngineThread, NULL, 0, NULL);
            }
            break;
        }

        case DLL_PROCESS_DETACH:
        {
            // lpvReserved is non-NULL when the process is exiting, in which case Windows is about to tear
            // everything down and unhooking would only risk running code on a dying process.
            if (lpvReserved == NULL)
            {
                SP_EngineStop();

                // A FreeLibrary (sp_inject /eject) unmaps this code the moment DllMain returns. The engine thread
                // is still unhooking on that code, so it is given time to finish first; a hook left in place
                // would otherwise point into unmapped memory and take the shell down. The wait is bounded: a
                // teardown that needs the loader lock this thread holds would otherwise deadlock.
                if (g_hEngineThread)
                {
                    WaitForSingleObject(g_hEngineThread, 15000);
                }
            }
            if (g_hEngineThread)
            {
                CloseHandle(g_hEngineThread);
                g_hEngineThread = NULL;
            }
            break;
        }
    }

    return TRUE;
}
