#include <Windows.h>

// Module handle of sp_gui.dll; GUI.c loads resources (strings, settings.reg) from it.
HMODULE hModule = NULL;

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    UNREFERENCED_PARAMETER(lpvReserved);
    if (fdwReason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hinstDLL);
        hModule = hinstDLL;
    }
    return TRUE;
}
