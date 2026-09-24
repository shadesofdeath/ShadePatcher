//
// explorer-auto-file-sizes - show file sizes with sensible units instead of always in KB.
//
// Adapted from the idea behind the Windhawk mod "Better file sizes in Explorer details"
// (explorer-details-better-file-sizes) by m417z. The implementation here is written against this engine's API.
//
// The Size column in File Explorer reports everything in kilobytes, so a 4 GB file reads as "4,194,304 KB". The
// formatting goes through propsys.dll, which has two entry points for it:
//
//     ordinal 421   format with the unit that fits the value (KB, MB, GB, ...)
//     ordinal 422   format in KB, whatever the value
//
// Explorer calls the second one. Hooking it and handing the call to the first is the whole mod: the number is
// still produced by Windows, so it is formatted, rounded and localized exactly as the rest of the shell does it.
//
// The two functions are exported without names, which is why they are resolved by ordinal.
//
#define SP_MOD_ID "explorer-auto-file-sizes"
#include "engine/modapi.h"

#include <atomic>

namespace {

// Both entry points take the same arguments and return the end of the text they wrote.
using PSStrFormatSize_t = void*(WINAPI*)(ULONGLONG size, LPWSTR pszText, DWORD cchText);

constexpr WORD kOrdinalFormatAuto = 421;    // PSStrFormatByteSizeW
constexpr WORD kOrdinalFormatKB   = 422;    // PSStrFormatKBSizeW

PSStrFormatSize_t g_formatAuto = nullptr;         // resolved, not hooked: the mod calls it
PSStrFormatSize_t g_origFormatKB = nullptr;       // the trampoline for the hooked entry point

// When set, "KB" becomes "KiB" and so on, making it explicit that the units are powers of 1024.
std::atomic<bool> g_binaryUnits{ false };

// Turns the unit Windows wrote into its binary spelling, in place. The text ends with a unit of one or two
// letters followed by "B"; only that last part is touched, so the number and its separators are left alone.
void ApplyBinaryUnit(LPWSTR pszText, DWORD cchText)
{
    size_t len = wcsnlen(pszText, cchText);

    // "4 GB" -> "4 GiB" needs one more character plus the terminator.
    if (len < 2 || len + 2 > cchText)
    {
        return;
    }
    if (pszText[len - 1] != L'B')
    {
        return;
    }

    wchar_t unit = pszText[len - 2];
    if (unit != L'K' && unit != L'M' && unit != L'G' && unit != L'T' && unit != L'P' && unit != L'E')
    {
        return;     // plain bytes, which has no binary spelling
    }

    pszText[len - 1] = L'i';
    pszText[len] = L'B';
    pszText[len + 1] = L'\0';
}

void* WINAPI FormatKB_Hook(ULONGLONG size, LPWSTR pszText, DWORD cchText)
{
    if (!g_formatAuto || !pszText || cchText == 0)
    {
        return g_origFormatKB(size, pszText, cchText);
    }

    void* result = g_formatAuto(size, pszText, cchText);

    if (g_binaryUnits.load(std::memory_order_relaxed))
    {
        ApplyBinaryUnit(pszText, cchText);
    }

    return result;
}

void LoadSettings()
{
    g_binaryUnits.store(SP_GetIntSetting(L"BinaryUnits", 0) != 0, std::memory_order_relaxed);
}

BOOL Init()
{
    LoadSettings();

    // propsys is loaded by the shell long before any window appears, so it is always present here.
    HMODULE hPropsys = GetModuleHandleW(L"propsys.dll");
    if (!hPropsys)
    {
        SP_LogError(L"propsys.dll is not loaded");
        return FALSE;
    }

    g_formatAuto = (PSStrFormatSize_t)GetProcAddress(hPropsys, MAKEINTRESOURCEA(kOrdinalFormatAuto));
    void* pFormatKB = (void*)GetProcAddress(hPropsys, MAKEINTRESOURCEA(kOrdinalFormatKB));

    if (!g_formatAuto || !pFormatKB)
    {
        // A Windows build that numbers these differently is a reason to do nothing, not to guess: hooking the
        // wrong ordinal would corrupt whatever function actually lives there.
        SP_LogError(L"propsys.dll does not export the expected size formatting ordinals");
        return FALSE;
    }

    return SP_SetFunctionHookNow(pFormatKB, FormatKB_Hook, &g_origFormatKB);
}

}   // namespace

SP_MOD_DEFINE(g_modExplorerAutoFileSizes) =
{
    /* id             */ SP_MOD_ID,
    /* name           */ L"Show file sizes in KB, MB and GB instead of only KB",
    /* basedOn        */ "explorer-details-better-file-sizes",
    /* originalAuthor */ "m417z",
    /* targets        */ SP_TARGET_EXPLORER,
    /* minOsBuild     */ 0,
    /* flags          */ 0,
    /* Init           */ Init,
    /* AfterInit      */ nullptr,
    /* SettingsChanged*/ LoadSettings,
    /* BeforeUninit   */ nullptr,
    /* Uninit         */ nullptr,
};
