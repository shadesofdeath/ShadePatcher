//
// extension-change-no-warning - rename a file to a different extension without the "are you sure" prompt.
//
// Adapted from the idea behind the Windhawk mod "Turn off change file extension warning"
// (extension-change-no-warning) by m417z. The implementation here is written against this engine's API.
//
// How it works
// ------------
// When a rename changes the extension, the shell asks
//     "If you change a file name extension, the file might become unusable. Are you sure you want to change it?"
// through ShellMessageBoxW in shlwapi.dll. It passes the text and the title as shell32 string resource ids
// (4112 and 4148) rather than as strings, with the MB_ICONEXCLAMATION | MB_YESNO style. That combination is
// specific enough to recognise the prompt without reading any text, so it works in every display language.
//
// The hook answers IDYES to exactly that call and forwards every other one untouched. Builds from April 2025
// onwards route some callers through ShellMessageBoxInternal, which takes an extra flags argument; it is hooked
// the same way when the export exists.
//
// Forwarding a variadic call
// --------------------------
// ShellMessageBoxW is printf-like: the format inserts follow fuStyle as variadic arguments, and C++ has no way to
// pass "whatever came after" on to the original. Windhawk's version leans on clang's musttail; MSVC has nothing
// equivalent, so this port uses the x64 calling convention instead. Every argument past the fourth lives on the
// stack in order, so a detour that declares a generous number of trailing pointer-sized parameters and hands all
// of them to the trampoline reproduces the caller's stack layout exactly, and the original's va_list reads the
// same values it would have seen without the hook. Slots beyond what the caller actually passed hold the caller's
// own frame, which is readable memory, and the original never looks at more inserts than its format names.
//
// The technique is x64-only, as is the engine; the #error below keeps the two facts tied together.
//
#define SP_MOD_ID "extension-change-no-warning"
#include "engine/modapi.h"

#ifndef _M_X64
#error "This mod forwards variadic arguments by relying on the x64 calling convention"
#endif

namespace {

// shell32.dll string resources. Unchanged from Windows 7 through Windows 11 build 26200.
constexpr UINT kStringExtensionChangeWarning = 4112;    // "If you change a file name extension, ..."
constexpr UINT kStringRenameTitle            = 4148;    // "Rename"

constexpr UINT kPromptStyle = MB_ICONEXCLAMATION | MB_YESNO;

// The trailing stack slots carried over to the original. Sixteen is far more than any shell32 format uses.
#define SP_VA_SLOT_PARAMS                                                                                   \
    ULONG_PTR va0,  ULONG_PTR va1,  ULONG_PTR va2,  ULONG_PTR va3,  ULONG_PTR va4,  ULONG_PTR va5,          \
    ULONG_PTR va6,  ULONG_PTR va7,  ULONG_PTR va8,  ULONG_PTR va9,  ULONG_PTR va10, ULONG_PTR va11,         \
    ULONG_PTR va12, ULONG_PTR va13, ULONG_PTR va14, ULONG_PTR va15

#define SP_VA_SLOT_ARGS                                                                                     \
    va0, va1, va2, va3, va4, va5, va6, va7, va8, va9, va10, va11, va12, va13, va14, va15

// TRUE for the one prompt this mod exists to silence. Resource ids arrive as small integers cast to pointers, so
// the comparison is exact and never reads through either pointer.
bool IsExtensionChangePrompt(HINSTANCE hAppInst, LPCWSTR lpcText, LPCWSTR lpcTitle, UINT fuStyle)
{
    if (!hAppInst || fuStyle != kPromptStyle)
    {
        return false;
    }
    if (lpcText != MAKEINTRESOURCEW(kStringExtensionChangeWarning) ||
        lpcTitle != MAKEINTRESOURCEW(kStringRenameTitle))
    {
        return false;
    }

    // The ids only mean this when they are looked up in shell32's own string table.
    return hAppInst == GetModuleHandleW(L"shell32.dll");
}

// ---------------------------------------------------------------------------------------------------------------
// The hooks
// ---------------------------------------------------------------------------------------------------------------

using ShellMessageBoxW_t = int(__cdecl*)(HINSTANCE hAppInst, HWND hWnd, LPCWSTR lpcText, LPCWSTR lpcTitle,
                                         UINT fuStyle, SP_VA_SLOT_PARAMS);

using ShellMessageBoxInternal_t = int(__cdecl*)(HINSTANCE hAppInst, HWND hWnd, DWORD dwFlags, LPCWSTR lpcText,
                                                LPCWSTR lpcTitle, UINT fuStyle, SP_VA_SLOT_PARAMS);

ShellMessageBoxW_t        g_origShellMessageBoxW = nullptr;
ShellMessageBoxInternal_t g_origShellMessageBoxInternal = nullptr;

int __cdecl ShellMessageBoxW_Hook(HINSTANCE hAppInst, HWND hWnd, LPCWSTR lpcText, LPCWSTR lpcTitle,
                                  UINT fuStyle, SP_VA_SLOT_PARAMS)
{
    if (IsExtensionChangePrompt(hAppInst, lpcText, lpcTitle, fuStyle))
    {
        SP_Log(L"Answered Yes to the extension change prompt");
        return IDYES;
    }

    return g_origShellMessageBoxW(hAppInst, hWnd, lpcText, lpcTitle, fuStyle, SP_VA_SLOT_ARGS);
}

int __cdecl ShellMessageBoxInternal_Hook(HINSTANCE hAppInst, HWND hWnd, DWORD dwFlags, LPCWSTR lpcText,
                                         LPCWSTR lpcTitle, UINT fuStyle, SP_VA_SLOT_PARAMS)
{
    if (IsExtensionChangePrompt(hAppInst, lpcText, lpcTitle, fuStyle))
    {
        SP_Log(L"Answered Yes to the extension change prompt (internal entry point)");
        return IDYES;
    }

    return g_origShellMessageBoxInternal(hAppInst, hWnd, dwFlags, lpcText, lpcTitle, fuStyle, SP_VA_SLOT_ARGS);
}

#undef SP_VA_SLOT_PARAMS
#undef SP_VA_SLOT_ARGS

// ---------------------------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------------------------

BOOL Init()
{
    // shell32's own ShellMessageBoxW export is a forwarder to this one, so hooking shlwapi catches every caller.
    HMODULE hShlwapi = GetModuleHandleW(L"shlwapi.dll");
    if (!hShlwapi)
    {
        // Always loaded in the shell in practice. If not, loading it is harmless: it is a system DLL that stays
        // for the life of the process, so the reference is never released.
        hShlwapi = LoadLibraryW(L"shlwapi.dll");
    }
    if (!hShlwapi)
    {
        SP_LogError(L"shlwapi.dll could not be loaded");
        return FALSE;
    }

    void* pShellMessageBoxW = (void*)GetProcAddress(hShlwapi, "ShellMessageBoxW");
    if (!pShellMessageBoxW)
    {
        SP_LogError(L"shlwapi.dll does not export ShellMessageBoxW");
        return FALSE;
    }

    // Present on newer builds only; its absence is not a failure.
    void* pShellMessageBoxInternal = (void*)GetProcAddress(hShlwapi, "ShellMessageBoxInternal");

    if (!SP_HookBegin())
    {
        return FALSE;
    }

    if (!SP_SetFunctionHook(pShellMessageBoxW, ShellMessageBoxW_Hook, &g_origShellMessageBoxW))
    {
        SP_HookAbort();
        SP_LogError(L"ShellMessageBoxW could not be hooked");
        return FALSE;
    }

    if (pShellMessageBoxInternal)
    {
        if (!SP_SetFunctionHook(pShellMessageBoxInternal, ShellMessageBoxInternal_Hook,
                                &g_origShellMessageBoxInternal))
        {
            SP_HookAbort();
            SP_LogError(L"ShellMessageBoxInternal could not be hooked");
            return FALSE;
        }
    }
    else
    {
        SP_Log(L"This build has no ShellMessageBoxInternal; only ShellMessageBoxW is hooked");
    }

    return SP_HookCommit();
}

}   // namespace

SP_MOD_DEFINE(g_modExtensionChangeNoWarning) =
{
    /* id             */ SP_MOD_ID,
    /* name           */ L"Turn off the file extension change warning",
    /* basedOn        */ "extension-change-no-warning",
    /* originalAuthor */ "m417z",
    /* targets        */ SP_TARGET_EXPLORER,
    /* minOsBuild     */ 0,
    /* flags          */ 0,
    /* Init           */ Init,
    /* AfterInit      */ nullptr,
    /* SettingsChanged*/ nullptr,
    /* BeforeUninit   */ nullptr,
    /* Uninit         */ nullptr,
};
