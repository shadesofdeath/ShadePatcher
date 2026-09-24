//
// legacy-file-copy - the Windows 7 style file copy dialog instead of the Windows 8+ one.
//
// Adapted from the idea behind the Windhawk mod "Legacy File Copy" (legacy-file-copy) by rounk-ctrl, which in
// turn follows ExplorerPatcher. The implementation here is written against this engine's API.
//
// How it works
// ------------
// Before a file operation shows progress, the copy engine asks shell32 whether the modern (Windows 8) dialog is
// usable through the internal export SHELL32_CanDisplayWin8CopyDialog, reached via the api set
// ext-ms-win-shell-exports-internal-l1-1-0.dll. Answering FALSE makes it fall back to the classic dialog, which
// is still fully present in shell32. Nothing else changes: cancel, pause and conflict handling all keep working.
//
// The api set is resolved to the real module by the loader; hooking the address it returns hooks the function
// every caller reaches. The dialog decision is made per operation, so an operation already on screen keeps the
// dialog it started with when the mod is toggled.
//
#define SP_MOD_ID "legacy-file-copy"
#include "engine/modapi.h"

namespace {

using CanDisplayWin8CopyDialog_t = BOOL(WINAPI*)();
CanDisplayWin8CopyDialog_t g_origCanDisplayWin8CopyDialog = nullptr;

BOOL WINAPI CanDisplayWin8CopyDialog_Hook()
{
    SP_LogDebug(L"The classic copy dialog was chosen");
    return FALSE;
}

BOOL Init()
{
    // The api set forwards to shell32, which is always loaded in the shell; loading the api set itself is
    // harmless and gives the loader the chance to resolve the forwarder on builds where it is not yet mapped.
    HMODULE hExports = LoadLibraryW(L"ext-ms-win-shell-exports-internal-l1-1-0.dll");
    if (!hExports)
    {
        SP_LogError(L"The shell exports api set could not be loaded: %lu", GetLastError());
        return FALSE;
    }

    void* target = (void*)GetProcAddress(hExports, "SHELL32_CanDisplayWin8CopyDialog");
    if (!target)
    {
        SP_LogError(L"SHELL32_CanDisplayWin8CopyDialog is not exported on this build");
        return FALSE;
    }

    if (!SP_SetFunctionHookNow(target, CanDisplayWin8CopyDialog_Hook, &g_origCanDisplayWin8CopyDialog))
    {
        SP_LogError(L"SHELL32_CanDisplayWin8CopyDialog could not be hooked");
        return FALSE;
    }
    return TRUE;
}

}   // namespace

SP_MOD_DEFINE(g_modLegacyFileCopy) =
{
    /* id             */ SP_MOD_ID,
    /* name           */ L"Classic (Windows 7) file copy dialog",
    /* basedOn        */ "legacy-file-copy",
    /* originalAuthor */ "rounk-ctrl",
    /* targets        */ SP_TARGET_EXPLORER,
    /* minOsBuild     */ 0,
    /* flags          */ 0,
    /* Init           */ Init,
    /* AfterInit      */ nullptr,
    /* SettingsChanged*/ nullptr,
    /* BeforeUninit   */ nullptr,
    /* Uninit         */ nullptr,
};
