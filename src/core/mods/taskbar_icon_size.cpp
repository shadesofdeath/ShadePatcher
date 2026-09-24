//
// taskbar_icon_size - taskbar height, icon size and button width on the Windows 11 taskbar.
//
// Adapted from the idea behind the Windhawk mod "Taskbar height and icon size" (taskbar-icon-size) by m417z.
// The implementation here is written against this engine's API.
//
// What it does
// ------------
// The Windows 11 taskbar is 48 pixels tall, draws its icons at 24x24 and gives every button 44 pixels. None of
// that is a setting. This mod makes all three numbers the user's:
//
//   TaskbarHeight        the height of the taskbar frame and of the system tray next to it
//   IconSize             the size the taskbar draws an application icon at
//   TaskbarButtonWidth   the width of one taskbar button (and of the Start / search / widgets buttons)
//
// A 16 or 32 pixel icon size also fixes the blur: Windows ships icons at 16 and 32, and the stock 24 is a
// downscaled 32.
//
// Where it hooks and why
// ----------------------
// The numbers live in three places, each with its own module:
//
//   Taskbar.View.dll   the XAML taskbar. TaskbarConfiguration::GetFrameSize answers the frame height,
//                      TaskbarConfiguration::GetIconHeightInViewPixels the icon size, and the button width is
//                      a value ("MediumTaskbarButtonExtent") in the XAML resource dictionary. The TaskListButton
//                      keeps a copy of both in its own fields, and several of its methods compare the icon
//                      height against the stock 16 / 24 / 32 to tell the taskbar postures apart, so those get
//                      the stock value for the duration of the call and the custom value back afterwards.
//   SystemTray.dll     the clock and notification area. Its controller asks for the frame size again
//                      (SystemTrayController::GetFrameSize) and caches the last height it applied; the cache is
//                      reset so a change is applied instead of skipped. On Taskbar.View.dll older than 2604
//                      these types are inside Taskbar.View.dll itself and are hooked there.
//   taskbar.dll        the Win32 side: TrayUI::GetMinSize (the height explorer reports to the desktop),
//                      TrayUI::_HandleSettingChange (the entry point that re-lays the taskbar out when settings
//                      are applied) and, on builds without dynamic icon scaling, the icon loader that decides
//                      which icon size to ask an application for.
//   SearchUx.UI.dll    the search button, which sizes its own icon and root panel.
//
// Two builds of the taskbar exist. Since October 2024 the taskbar scales icons itself ("dynamic icon scaling",
// a TaskbarConfiguration::GetIconHeightInViewPixels() method that takes no arguments); before that the icon
// size was a constant and the icon loader in taskbar.dll had to be steered. Both are handled: the presence of
// the method plus the OS feature flag picks the path, and the hooks of the other path pass through.
//
// Applying a change live: the mod writes the new height into its state and sends WM_SETTINGCHANGE to the
// taskbar window, which the shell answers with a full re-layout (TrayUI::_HandleSettingChange), during which
// every hook above hands out the new numbers. The tray is nudged too (TrayUI::_StuckTrayChange).
//
// Field offsets: a few hooks write into private fields of the shell's objects (the TaskListButton's icon
// height, the tray controller's cached height, the controller's frame pointer, the button's extent). The
// offsets are not in the PDB; they are read from the first instructions of the functions that use them, the
// same way the original does it, and a pattern that does not match leaves the offset at 0 and that hook
// passing through.
//
// Exceptions
// ----------
// C++/WinRT reports failures by throwing, so this file is compiled with exceptions on. Every hook body is
// wrapped: nothing may escape into the shell's UI thread.
//
#define SP_MOD_ID "taskbar-icon-size"
#include "engine/modapi.h"

#include <shellapi.h>

#undef GetCurrentTime

#include <unknwn.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>

#include <atomic>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

// rpcndr.h defines small as a macro, which collides with the variable names below.
#undef small

namespace {

using namespace winrt::Windows::UI::Xaml;
using winrt::Windows::Foundation::IInspectable;
using winrt::Windows::Foundation::Size;

// ---------------------------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------------------------

// The stock numbers. The small posture (the "smaller taskbar buttons" option, or a full taskbar) is not
// customized by this port: it keeps the stock small icon and button width.
constexpr int kStockTaskbarHeight = 48;
constexpr int kStockIconSize = 24;
constexpr int kStockButtonWidth = 44;
constexpr int kStockSmallIconSize = 16;
constexpr int kStockSmallButtonWidth = 32;

// Written on the engine thread, read from whatever thread the taskbar calls a hook on.
std::atomic<int> g_settingTaskbarHeight{ kStockTaskbarHeight };
std::atomic<int> g_settingIconSize{ kStockIconSize };
std::atomic<int> g_settingButtonWidth{ kStockButtonWidth };

std::atomic<int>  g_taskbarHeight{ 0 };          // the height currently handed out; 0 until applied
std::atomic<int>  g_originalTaskbarHeight{ 0 };  // what the shell answered before this mod changed it
std::atomic<bool> g_applyingSettings{ false };
std::atomic<bool> g_pendingMeasureOverride{ false };
std::atomic<bool> g_unloading{ false };
std::atomic<int>  g_hookCallCounter{ 0 };

std::atomic<bool> g_hasDynamicIconScaling{ false };
std::atomic<bool> g_smallIconSize{ false };
std::atomic<bool> g_taskbarButtonWidthCustomized{ false };
std::atomic<DWORD> g_iconLoaderThreadId{ 0 };

std::atomic<bool> g_taskbarViewHooked{ false };
std::atomic<bool> g_systemTrayHooked{ false };
std::atomic<bool> g_afterInitDone{ false };

// ApplySettings may be reached from the engine thread and from a module-wait thread; one at a time.
std::mutex g_applyMutex;

// Per-thread markers for the hooks that need to know what called them.
thread_local bool g_inSystemTrayUpdateFrameSize = false;
thread_local bool g_inTaskbarFrameGetMetrics = false;
thread_local std::optional<double> g_getMetricsIconHeight;
thread_local Size* g_systemTrayFrameMeasureSize = nullptr;
thread_local double g_taskListButtonPostureIconHeight = 0;
thread_local double g_taskListButtonCustomIconHeight = 0;
thread_local bool g_inExperienceToggleButtonIconHeight = false;
thread_local bool g_inSearchButtonIconHeight = false;

int CurrentIconSize()   { return g_settingIconSize.load(std::memory_order_relaxed); }
int CurrentButtonWidth() { return g_settingButtonWidth.load(std::memory_order_relaxed); }
int CurrentTaskbarHeight() { return g_taskbarHeight.load(std::memory_order_relaxed); }
bool IsUnloading() { return g_unloading.load(std::memory_order_relaxed); }
bool IsSmallPosture() { return g_smallIconSize.load(std::memory_order_relaxed); }
bool HasDynamicIconScaling() { return g_hasDynamicIconScaling.load(std::memory_order_relaxed); }

int IconSizeFor(bool small)    { return small ? kStockSmallIconSize : CurrentIconSize(); }
int ButtonWidthFor(bool small) { return small ? kStockSmallButtonWidth : CurrentButtonWidth(); }
double PostureIconHeight()     { return IsSmallPosture() ? 16.0 : 24.0; }

// The pre-dynamic-icon-scaling hooks in taskbar.dll are only meaningful once the view is hooked and has said
// which kind of build this is; before that they pass through.
bool PreScalingPathActive()
{
    return g_taskbarViewHooked.load(std::memory_order_relaxed) && !HasDynamicIconScaling();
}

// ---------------------------------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------------------------------

using SHAppBarMessage_t = decltype(&SHAppBarMessage);
SHAppBarMessage_t g_origSHAppBarMessage = nullptr;

bool IsVerticalTaskbar()
{
    APPBARDATA data = {};
    data.cbSize = sizeof(data);
    SHAppBarMessage_t pfn = g_origSHAppBarMessage ? g_origSHAppBarMessage : SHAppBarMessage;
    if (!pfn(ABM_GETTASKBARPOS, &data))
    {
        return false;
    }
    return data.uEdge == ABE_LEFT || data.uEdge == ABE_RIGHT;
}

// shcore.lib is not linked, so the DPI function is fetched by hand. 0 when it cannot be told.
UINT GetMonitorDpi(HMONITOR monitor)
{
    typedef HRESULT (WINAPI *GetDpiForMonitor_t)(HMONITOR, int, UINT*, UINT*);
    static GetDpiForMonitor_t pfn = []() -> GetDpiForMonitor_t {
        HMODULE hShcore = LoadLibraryW(L"shcore.dll");
        return hShcore ? (GetDpiForMonitor_t)GetProcAddress(hShcore, "GetDpiForMonitor") : nullptr;
    }();

    UINT dpiX = 0, dpiY = 0;
    if (monitor && pfn && SUCCEEDED(pfn(monitor, 0 /* MDT_EFFECTIVE_DPI */, &dpiX, &dpiY)) && dpiY)
    {
        return dpiY;
    }
    return 0;
}

HWND FindTaskbarWindow()
{
    HWND found = nullptr;
    EnumWindows([](HWND hWnd, LPARAM lParam) -> BOOL {
        DWORD pid = 0;
        wchar_t className[32];
        if (GetWindowThreadProcessId(hWnd, &pid) && pid == GetCurrentProcessId() &&
            GetClassNameW(hWnd, className, ARRAYSIZE(className)) && _wcsicmp(className, L"Shell_TrayWnd") == 0)
        {
            *reinterpret_cast<HWND*>(lParam) = hWnd;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&found));
    return found;
}

// The major part of a module's file version, read from its version resource. version.dll is not linked, so
// the VS_FIXEDFILEINFO block is found by its signature instead of through VerQueryValue. 0 when unknown.
WORD GetModuleVersionMajor(HMODULE hModule)
{
    HRSRC hRes = FindResourceW(hModule, MAKEINTRESOURCEW(VS_VERSION_INFO), RT_VERSION);
    if (!hRes)
    {
        return 0;
    }
    HGLOBAL hGlobal = LoadResource(hModule, hRes);
    const BYTE* data = hGlobal ? (const BYTE*)LockResource(hGlobal) : nullptr;
    DWORD size = SizeofResource(hModule, hRes);
    if (!data || size < sizeof(VS_FIXEDFILEINFO))
    {
        return 0;
    }

    for (DWORD at = 0; at + sizeof(VS_FIXEDFILEINFO) <= size && at < 0x80; at += sizeof(DWORD))
    {
        const VS_FIXEDFILEINFO* info = (const VS_FIXEDFILEINFO*)(data + at);
        if (info->dwSignature == 0xFEEF04BD)
        {
            return HIWORD(info->dwFileVersionMS);
        }
    }
    return 0;
}

// Whether a Windows feature is switched on in the feature store. No answer means "left at its default".
std::optional<bool> IsOsFeatureEnabled(UINT32 featureId)
{
#pragma pack(push, 1)
    struct RTL_FEATURE_CONFIGURATION
    {
        unsigned int featureId;
        unsigned __int32 group : 4;
        unsigned __int32 enabledState : 2;
        unsigned __int32 enabledStateOptions : 1;
        unsigned __int32 unused1 : 1;
        unsigned __int32 variant : 6;
        unsigned __int32 variantPayloadKind : 2;
        unsigned __int32 unused2 : 16;
        unsigned int payload;
    };
#pragma pack(pop)

    typedef int (NTAPI *RtlQueryFeatureConfiguration_t)(UINT32, int, INT64*, RTL_FEATURE_CONFIGURATION*);
    static RtlQueryFeatureConfiguration_t pfn = []() -> RtlQueryFeatureConfiguration_t {
        HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
        return hNtdll ? (RtlQueryFeatureConfiguration_t)GetProcAddress(hNtdll, "RtlQueryFeatureConfiguration")
                      : nullptr;
    }();
    if (!pfn)
    {
        return std::nullopt;
    }

    RTL_FEATURE_CONFIGURATION feature = {};
    INT64 changeStamp = 0;
    if (pfn(featureId, 1, &changeStamp, &feature) < 0)
    {
        return std::nullopt;
    }
    switch (feature.enabledState)
    {
    case 1:  return false;
    case 2:  return true;
    default: return std::nullopt;
    }
}

// --- XAML tree walking ---------------------------------------------------------------------------------------

FrameworkElement EnumChildElements(FrameworkElement const& element,
                                   std::function<bool(FrameworkElement const&)> const& callback)
{
    int count = Media::VisualTreeHelper::GetChildrenCount(element);
    for (int i = 0; i < count; ++i)
    {
        auto child = Media::VisualTreeHelper::GetChild(element, i).try_as<FrameworkElement>();
        if (child && callback(child))
        {
            return child;
        }
    }
    return nullptr;
}

FrameworkElement FindChildByName(FrameworkElement const& element, const wchar_t* name)
{
    return EnumChildElements(element, [name](FrameworkElement const& child) {
        return child.Name() == name;
    });
}

FrameworkElement FindChildByClassName(FrameworkElement const& element, const wchar_t* className)
{
    return EnumChildElements(element, [className](FrameworkElement const& child) {
        return winrt::get_class_name(child) == className;
    });
}

// The FrameworkElement behind a C++/WinRT implementation object, two ways:
//
//   ElementFromVtable(p, n)   the object's n-th embedded interface (its vtable pointer sits at p + n*8);
//                             TaskListButton's XAML interface is the fourth, a produce<> thunk's the first.
//   ElementFromMember(p, n)   the interface pointer stored in the object's n-th pointer-sized field; the
//                             composable shell controls keep their inner XAML object in the second one.
FrameworkElement ElementFromVtable(void* pThis, int index)
{
    FrameworkElement element{ nullptr };
    if (!pThis)
    {
        return element;
    }
    ::IUnknown* unknown = reinterpret_cast<::IUnknown*>(reinterpret_cast<void**>(pThis) + index);
    unknown->QueryInterface(winrt::guid_of<FrameworkElement>(), winrt::put_abi(element));
    return element;
}

FrameworkElement ElementFromMember(void* pThis, int index)
{
    FrameworkElement element{ nullptr };
    if (!pThis)
    {
        return element;
    }
    ::IUnknown* unknown = reinterpret_cast<::IUnknown**>(pThis)[index];
    if (unknown)
    {
        unknown->QueryInterface(winrt::guid_of<FrameworkElement>(), winrt::put_abi(element));
    }
    return element;
}

// A property accessor of a C++/WinRT interface is a generic "use a fixed vtable slot" thunk that the linker
// folds with the identical accessors of unrelated interfaces, so a hook on one is called for all of them and
// has to confirm what it was called on. pThis is a C++/WinRT wrapper: it points at the ABI interface pointer.
bool IsRuntimeClass(void* pThis, std::wstring_view className)
{
    void* abi = pThis ? *(void**)pThis : nullptr;
    if (!abi)
    {
        return false;
    }

    static std::mutex mutex;
    static std::unordered_map<void*, std::wstring> cache;   // keyed by vtable, so one query per interface

    std::lock_guard<std::mutex> guard(mutex);
    auto [it, inserted] = cache.try_emplace(*(void**)abi);
    if (inserted)
    {
        try
        {
            it->second = std::wstring(winrt::get_class_name(*reinterpret_cast<IInspectable*>(pThis)));
            SP_LogDebug(L"Folded accessor called on %s", it->second.c_str());
        }
        catch (...)
        {
        }
    }
    return it->second == className;
}

// ---------------------------------------------------------------------------------------------------------------
// Field offsets read from code
//
// x64 only. Each scanner looks at the first bytes of a function the mod resolved but does not hook, so the
// bytes are the shell's own. A pattern that is not found gives 0, and the hook that needs it passes through.
// ---------------------------------------------------------------------------------------------------------------

// "movsd xmmN, qword ptr [rcx+disp]": F2 [REX] 0F 10 modrm [disp8|disp32], with rcx as the base. The first
// such load in a property setter is the compare of the member against the new value.
LONG FindMovsdLoadFromRcx(const void* function, size_t limit)
{
#if defined(_M_X64)
    const BYTE* p = (const BYTE*)function;
    if (!p)
    {
        return 0;
    }
    for (size_t i = 0; i + 8 <= limit; ++i)
    {
        if (p[i] != 0xF2)
        {
            continue;
        }
        size_t at = i + 1;
        if ((p[at] & 0xF0) == 0x40)
        {
            if (p[at] & 0x01)       // REX.B: base is r9, not rcx
            {
                continue;
            }
            ++at;
        }
        if (p[at] != 0x0F || p[at + 1] != 0x10)
        {
            continue;
        }
        BYTE modrm = p[at + 2];
        if ((modrm & 0x07) != 0x01)
        {
            continue;
        }
        if ((modrm & 0xC0) == 0x40)
        {
            return (LONG)(signed char)p[at + 3];
        }
        if ((modrm & 0xC0) == 0x80)
        {
            LONG offset = *(const LONG*)(p + at + 3);
            return (offset < 0 || offset > 0xFFFF) ? 0 : offset;
        }
    }
#else
    (void)function; (void)limit;
#endif
    return 0;
}

// SystemTrayController::UpdateFrameSize compares the new height against the one it applied last:
//   66 0F 2E modrm disp32    ucomisd xmmN, qword ptr [reg+disp32]
//   7A xx                    jp
//   74|75 xx / 0F 84|85 ..   je / jne
LONG FindLastHeightOffset(const void* function)
{
#if defined(_M_X64)
    const BYTE* p = (const BYTE*)function;
    if (!p)
    {
        return 0;
    }
    for (size_t i = 0; i + 12 <= 0x400; ++i)
    {
        if (p[i] == 0x66 && p[i + 1] == 0x0F && p[i + 2] == 0x2E && (p[i + 3] & 0xC0) == 0x80 &&
            p[i + 8] == 0x7A &&
            (p[i + 10] == 0x74 || p[i + 10] == 0x75 ||
             (p[i + 10] == 0x0F && (p[i + 11] == 0x84 || p[i + 11] == 0x85))))
        {
            LONG offset = *(const LONG*)(p + i + 4);
            return (offset < 0 || offset > 0xFFFF) ? 0 : offset;
        }
    }
#else
    (void)function;
#endif
    return 0;
}

// TaskbarController::OnGroupingModeChanged starts by loading the controller's TaskbarFrame:
//   48 83 EC 28              sub rsp, 28h
//   48|4C 8B modrm disp32    mov rax|r8, qword ptr [rcx+disp32]
LONG FindTaskbarFrameOffset(const void* function)
{
#if defined(_M_X64)
    const BYTE* p = (const BYTE*)function;
    if (p && p[0] == 0x48 && p[1] == 0x83 && p[2] == 0xEC && (p[4] == 0x48 || p[4] == 0x4C) && p[5] == 0x8B &&
        (p[6] & 0xC0) == 0x80)
    {
        LONG offset = *(const LONG*)(p + 7);
        return (offset < 0 || offset > 0xFFFF) ? 0 : offset;
    }
#else
    (void)function;
#endif
    return 0;
}

// TaskListButton::UpdateIconColumnDefinition subtracts paddings from the button extent: the last movsd load
// before the first subsd is the extent field.
//   F2 [44] 0F 10 modrm disp32   movsd xmmN, [reg+disp32]
//   F2 [44] 0F 5C modrm disp32   subsd xmmN, [reg+disp32]
LONG FindButtonExtentOffset(const void* function)
{
#if defined(_M_X64)
    const BYTE* p = (const BYTE*)function;
    if (!p)
    {
        return 0;
    }
    LONG candidate = 0;
    for (size_t i = 0; i + 9 <= 0x200; ++i)
    {
        if (p[i] != 0xF2)
        {
            continue;
        }
        size_t at = i + 1;
        if (p[at] == 0x44)
        {
            ++at;
        }
        if (p[at] != 0x0F || (p[at + 2] & 0xC0) != 0x80)
        {
            continue;
        }
        if (p[at + 1] == 0x10)
        {
            candidate = *(const LONG*)(p + at + 3);
        }
        else if (p[at + 1] == 0x5C)
        {
            return (candidate < 0 || candidate > 0xFFFF) ? 0 : candidate;
        }
    }
#else
    (void)function;
#endif
    return 0;
}

LONG g_offsetTaskListButtonIconHeight = 0;
LONG g_offsetTaskbarComponentHostIconHeight = 0;
LONG g_offsetSystemTrayLastHeight = 0;
LONG g_offsetTaskbarFrame = 0;
LONG g_offsetButtonExtent = 0;

// Swaps a double field of an object for the duration of a call, then puts the old value back.
struct ScopedField
{
    double* field = nullptr;
    double  previous = 0;

    ScopedField(void* object, LONG offset, double value)
    {
        if (object && offset)
        {
            field = (double*)((BYTE*)object + offset);
            previous = *field;
            *field = value;
        }
    }
    ~ScopedField()
    {
        if (field)
        {
            *field = previous;
        }
    }
    ScopedField(const ScopedField&) = delete;
    ScopedField& operator=(const ScopedField&) = delete;
};

// ---------------------------------------------------------------------------------------------------------------
// taskbar.dll hooks
// ---------------------------------------------------------------------------------------------------------------

// The icon loader asks for the icon size once per icon type. Without dynamic icon scaling, the size is scaled
// from the stock 24 to the custom one, so the icon is rendered at the size it is shown at.
using IconUtils_GetIconSize_t = void (WINAPI*)(bool isSmall, int type, SIZE* size);
IconUtils_GetIconSize_t g_origIconUtilsGetIconSize = nullptr;

void WINAPI IconUtils_GetIconSize_Hook(bool isSmall, int type, SIZE* size)
{
    g_origIconUtilsGetIconSize(isSmall, type, size);

    if (size && PreScalingPathActive() && !IsUnloading() && !isSmall)
    {
        size->cx = MulDiv(size->cx, CurrentIconSize(), kStockIconSize);
        size->cy = MulDiv(size->cy, CurrentIconSize(), kStockIconSize);
    }
}

// While settings are being applied every icon container is told its storage is stale, so the icons are
// re-rendered at the new size instead of reused.
using IconContainer_IsStorageRecreationRequired_t = bool (WINAPI*)(void* pThis, void* param1, int flags);
IconContainer_IsStorageRecreationRequired_t g_origIconContainerIsStorageRecreationRequired = nullptr;

bool WINAPI IconContainer_IsStorageRecreationRequired_Hook(void* pThis, void* param1, int flags)
{
    if (PreScalingPathActive() && g_applyingSettings.load(std::memory_order_relaxed))
    {
        return true;
    }
    return g_origIconContainerIsStorageRecreationRequired(pThis, param1, flags);
}

// The minimum height explorer reports. Left alone, a secondary taskbar with auto-hide ends up displaced.
using TrayUI_GetMinSize_t = void (WINAPI*)(void* pThis, HMONITOR monitor, SIZE* size);
TrayUI_GetMinSize_t g_origTrayUIGetMinSize = nullptr;

void WINAPI TrayUI_GetMinSize_Hook(void* pThis, HMONITOR monitor, SIZE* size)
{
    g_origTrayUIGetMinSize(pThis, monitor, size);

    const int height = CurrentTaskbarHeight();
    if (size && height && !IsVerticalTaskbar())
    {
        UINT dpi = GetMonitorDpi(monitor);
        size->cy = MulDiv(height, dpi ? dpi : 96, 96);
    }
}

// With icons of 16 pixels or less, the small icon of a window is the one to ask for: it is drawn for that size
// rather than scaled down.
using CIconLoadingFunctions_GetClassLongPtrW_t = ULONG_PTR (WINAPI*)(void* pThis, HWND hWnd, int nIndex);
CIconLoadingFunctions_GetClassLongPtrW_t g_origCIconLoadingFunctionsGetClassLongPtrW = nullptr;

ULONG_PTR WINAPI CIconLoadingFunctions_GetClassLongPtrW_Hook(void* pThis, HWND hWnd, int nIndex)
{
    if (PreScalingPathActive() && !IsUnloading() && nIndex == GCLP_HICON && CurrentIconSize() <= 16)
    {
        nIndex = GCLP_HICONSM;
    }
    return g_origCIconLoadingFunctionsGetClassLongPtrW(pThis, hWnd, nIndex);
}

using CIconLoadingFunctions_SendMessageCallbackW_t =
    BOOL (WINAPI*)(void* pThis, HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam, SENDASYNCPROC callback,
                   ULONG_PTR dwData);
CIconLoadingFunctions_SendMessageCallbackW_t g_origCIconLoadingFunctionsSendMessageCallbackW = nullptr;

BOOL WINAPI CIconLoadingFunctions_SendMessageCallbackW_Hook(void* pThis, HWND hWnd, UINT Msg, WPARAM wParam,
                                                            LPARAM lParam, SENDASYNCPROC callback,
                                                            ULONG_PTR dwData)
{
    if (PreScalingPathActive() && !IsUnloading() && Msg == WM_GETICON && wParam == ICON_BIG &&
        CurrentIconSize() <= 16)
    {
        wParam = ICON_SMALL2;
    }
    return g_origCIconLoadingFunctionsSendMessageCallbackW(pThis, hWnd, Msg, wParam, lParam, callback, dwData);
}

// The newer icon loader sends WM_GETICON through SendMessageTimeoutW; the thread is marked while it runs so
// the export hook below knows which calls are its.
using ShellIconLoaderV2_ResumeCoro_t = void (WINAPI*)(void* pThis);
ShellIconLoaderV2_ResumeCoro_t g_origShellIconLoaderV2ResumeCoro = nullptr;

void WINAPI ShellIconLoaderV2_ResumeCoro_Hook(void* pThis)
{
    if (!PreScalingPathActive())
    {
        g_origShellIconLoaderV2ResumeCoro(pThis);
        return;
    }

    g_iconLoaderThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_origShellIconLoaderV2ResumeCoro(pThis);
    g_iconLoaderThreadId.store(0, std::memory_order_relaxed);
}

using SendMessageTimeoutW_t = decltype(&SendMessageTimeoutW);
SendMessageTimeoutW_t g_origSendMessageTimeoutW = nullptr;

LRESULT WINAPI SendMessageTimeoutW_Hook(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam, UINT fuFlags,
                                        UINT uTimeout, PDWORD_PTR lpdwResult)
{
    if (Msg == WM_GETICON && wParam == ICON_BIG && !IsUnloading() &&
        g_iconLoaderThreadId.load(std::memory_order_relaxed) == GetCurrentThreadId() &&
        IconSizeFor(IsSmallPosture()) <= 16)
    {
        wParam = ICON_SMALL2;
    }
    return g_origSendMessageTimeoutW(hWnd, Msg, wParam, lParam, fuFlags, uTimeout, lpdwResult);
}

// Applying the settings sends the taskbar a WM_SETTINGCHANGE, which lands here. The tray is then told to
// re-lay itself out as if the taskbar had been stuck to another edge, which is what makes it take the height.
using TrayUI_StuckTrayChange_t = void (WINAPI*)(void* pThis);
TrayUI_StuckTrayChange_t g_pfnTrayUIStuckTrayChange = nullptr;      // resolved, not hooked

using TrayUI_HandleSettingChange_t = void (WINAPI*)(void* pThis, void* p1, void* p2, void* p3, void* p4);
TrayUI_HandleSettingChange_t g_origTrayUIHandleSettingChange = nullptr;

void WINAPI TrayUI_HandleSettingChange_Hook(void* pThis, void* p1, void* p2, void* p3, void* p4)
{
    g_origTrayUIHandleSettingChange(pThis, p1, p2, p3, p4);

    if (g_applyingSettings.load(std::memory_order_relaxed) && g_pfnTrayUIStuckTrayChange)
    {
        g_pfnTrayUIStuckTrayChange(pThis);
    }
}

// The taskbar size setting: 0 small, 1 regular, 2 large. The customization is for the regular size, so the
// small one is reported as regular. The getter is a folded thunk shared with unrelated properties, hence the
// class check. One copy lives in each module.
using TaskbarSettings_Size_t = int (WINAPI*)(void* pThis);

int OverrideTaskbarSettingsSize(void* pThis, int size)
{
    if (size != 0 || IsUnloading())
    {
        return size;
    }
    try
    {
        if (IsRuntimeClass(pThis, L"WindowsUdk.UI.Shell.TaskbarSettings"))
        {
            return 1;
        }
    }
    catch (...)
    {
    }
    return size;
}

TaskbarSettings_Size_t g_origTaskbarSettingsSizeTaskbarDll = nullptr;
int WINAPI TaskbarSettings_Size_TaskbarDll_Hook(void* pThis)
{
    return OverrideTaskbarSettingsSize(pThis, g_origTaskbarSettingsSizeTaskbarDll(pThis));
}

TaskbarSettings_Size_t g_origTaskbarSettingsSizeSystemTray = nullptr;
int WINAPI TaskbarSettings_Size_SystemTray_Hook(void* pThis)
{
    return OverrideTaskbarSettingsSize(pThis, g_origTaskbarSettingsSizeSystemTray(pThis));
}

TaskbarSettings_Size_t g_origTaskbarSettingsSizeTaskbarView = nullptr;
int WINAPI TaskbarSettings_Size_TaskbarView_Hook(void* pThis)
{
    return OverrideTaskbarSettingsSize(pThis, g_origTaskbarSettingsSizeTaskbarView(pThis));
}

// ---------------------------------------------------------------------------------------------------------------
// System tray hooks (SystemTray.dll, or Taskbar.View.dll before version 2604)
// ---------------------------------------------------------------------------------------------------------------

// The size enum: 1 and 2 are the regular and large postures; 0 (small) is left alone.
bool IsCustomizedPosture(int enumTaskbarSize)
{
    return enumTaskbarSize == 1 || enumTaskbarSize == 2;
}

using Controller_GetFrameSize_t = double (WINAPI*)(void* pThis, int enumTaskbarSize);
Controller_GetFrameSize_t g_origSystemTrayControllerGetFrameSize = nullptr;
Controller_GetFrameSize_t g_origSystemTraySecondaryControllerGetFrameSize = nullptr;

double WINAPI SystemTrayController_GetFrameSize_Hook(void* pThis, int enumTaskbarSize)
{
    const int height = CurrentTaskbarHeight();
    if (height && IsCustomizedPosture(enumTaskbarSize) && !IsVerticalTaskbar())
    {
        return height;
    }
    return g_origSystemTrayControllerGetFrameSize(pThis, enumTaskbarSize);
}

double WINAPI SystemTraySecondaryController_GetFrameSize_Hook(void* pThis, int enumTaskbarSize)
{
    const int height = CurrentTaskbarHeight();
    if (height && IsCustomizedPosture(enumTaskbarSize) && !IsVerticalTaskbar())
    {
        return height;
    }
    return g_origSystemTraySecondaryControllerGetFrameSize(pThis, enumTaskbarSize);
}

// The controller remembers the height it last applied and does nothing when asked for the same one again.
// That memory is cleared first, so the frame is always laid out with what GetFrameSize now says.
using Controller_UpdateFrameSize_t = void (WINAPI*)(void* pThis);
void* g_pfnSystemTrayControllerUpdateFrameSize = nullptr;           // resolved first, hooked after the scan
Controller_UpdateFrameSize_t g_origSystemTrayControllerUpdateFrameSize = nullptr;
Controller_UpdateFrameSize_t g_origSystemTraySecondaryControllerUpdateFrameSize = nullptr;

// Build 26200 dropped the SystemTrayFrame::Height wrapper: UpdateFrameSize computes the height itself (the
// old GetFrameSize is inlined) and calls IFrameworkElement::put_Height on the frame through its vtable, so
// nothing of the above can be hooked. The controller's public View() accessor hands out that same frame, and
// its height is set here after the original has finished.
using Controller_View_t = void* (WINAPI*)(void* pThis, void** result);
Controller_View_t g_pfnSystemTrayControllerView = nullptr;
thread_local bool g_inSystemTrayViewFixup = false;

using Element_SetDouble_t = void (WINAPI*)(void* pThis, double value);
extern Element_SetDouble_t g_origSystemTrayFrameHeight;

void SetSystemTrayFrameHeightThroughView(void* pThis)
{
    if (g_origSystemTrayFrameHeight || !g_pfnSystemTrayControllerView || g_inSystemTrayViewFixup)
    {
        return;
    }
    const int height = CurrentTaskbarHeight();
    if (!height)
    {
        return;
    }

    g_inSystemTrayViewFixup = true;
    try
    {
        void* abi = nullptr;
        g_pfnSystemTrayControllerView(pThis, &abi);
        FrameworkElement frame{ nullptr };
        winrt::attach_abi(frame, abi);
        if (frame)
        {
            if (frame.Height() != height)
            {
                frame.Height(height);
            }
            if (g_offsetSystemTrayLastHeight)
            {
                *(double*)((BYTE*)pThis + g_offsetSystemTrayLastHeight) = height;
            }
            SP_LogDebug(L"Tray frame height set to %d through View() (no Height wrapper on this build)", height);
        }
    }
    catch (...)
    {
        SP_LogError(L"The tray frame height could not be set");
    }
    g_inSystemTrayViewFixup = false;
}

void WINAPI SystemTrayController_UpdateFrameSize_Hook(void* pThis)
{
    if (IsVerticalTaskbar())
    {
        g_origSystemTrayControllerUpdateFrameSize(pThis);
        return;
    }

    if (g_offsetSystemTrayLastHeight)
    {
        *(double*)((BYTE*)pThis + g_offsetSystemTrayLastHeight) = 0;
    }

    SP_LogDebug(L"SystemTrayController::UpdateFrameSize (wanted height %d)", CurrentTaskbarHeight());

    g_inSystemTrayUpdateFrameSize = true;
    g_origSystemTrayControllerUpdateFrameSize(pThis);
    g_inSystemTrayUpdateFrameSize = false;

    SetSystemTrayFrameHeightThroughView(pThis);
}

void WINAPI SystemTraySecondaryController_UpdateFrameSize_Hook(void* pThis)
{
    g_inSystemTrayUpdateFrameSize = true;
    g_origSystemTraySecondaryControllerUpdateFrameSize(pThis);
    g_inSystemTrayUpdateFrameSize = false;
}

// The tray frame's height is set explicitly by the controller; it gets the custom height so it matches the
// taskbar frame next to it.
using Element_SetDouble_t = void (WINAPI*)(void* pThis, double value);
Element_SetDouble_t g_origSystemTrayFrameHeight = nullptr;

void WINAPI SystemTrayFrame_Height_Hook(void* pThis, double value)
{
    const int height = CurrentTaskbarHeight();
    if (height && g_inSystemTrayUpdateFrameSize && !IsVerticalTaskbar())
    {
        value = height;
    }
    g_origSystemTrayFrameHeight(pThis, value);
}

// The tray takes its mode (icon spacing, compact clock) from the height it is measured with, so a custom
// height that equals a stock one (24, 32, 72) would give it that mode. It is measured with the stock height,
// while the elements inside it are measured with the real one: the base MeasureOverride, which the frame calls
// once right away, hands the real size back.
using FrameworkElementOverrides_MeasureOverride_t = Size* (WINAPI*)(void* pThis, Size* result, Size* available);
FrameworkElementOverrides_MeasureOverride_t g_origFrameworkElementOverridesMeasureOverride = nullptr;

Size* WINAPI FrameworkElementOverrides_MeasureOverride_Hook(void* pThis, Size* result, Size* available)
{
    if (g_systemTrayFrameMeasureSize)
    {
        available = g_systemTrayFrameMeasureSize;
        g_systemTrayFrameMeasureSize = nullptr;
    }
    return g_origFrameworkElementOverridesMeasureOverride(pThis, result, available);
}

using Produce_MeasureOverride_t = int (WINAPI*)(void* pThis, Size size, Size* resultSize);
Produce_MeasureOverride_t g_origSystemTrayFrameMeasureOverride = nullptr;

int WINAPI SystemTrayFrame_MeasureOverride_Hook(void* pThis, Size size, Size* resultSize)
{
    const int original = g_originalTaskbarHeight.load(std::memory_order_relaxed);
    if (!original || !g_origFrameworkElementOverridesMeasureOverride || IsVerticalTaskbar())
    {
        return g_origSystemTrayFrameMeasureOverride(pThis, size, resultSize);
    }

    Size available = size;
    size.Height = (float)original;

    g_systemTrayFrameMeasureSize = &available;
    int ret = g_origSystemTrayFrameMeasureOverride(pThis, size, resultSize);
    g_systemTrayFrameMeasureSize = nullptr;
    return ret;
}

// ---------------------------------------------------------------------------------------------------------------
// Taskbar.View.dll hooks: the numbers themselves
// ---------------------------------------------------------------------------------------------------------------

// The button width is a value in the XAML resource dictionary. Every lookup of it answers the custom width.
void OverrideResourceLookup(const IInspectable* key, IInspectable* value)
{
    if (IsUnloading() || !key || !value || !*value)
    {
        return;
    }

    auto keyString = key->try_as<winrt::hstring>();
    if (!keyString || *keyString != L"MediumTaskbarButtonExtent")
    {
        return;
    }

    auto current = value->try_as<double>();
    if (!current)
    {
        return;
    }

    const double wanted = CurrentButtonWidth();
    if (wanted != *current)
    {
        *value = winrt::box_value(wanted);
    }
}

using ResourceDictionary_Lookup_t = IInspectable* (WINAPI*)(void* pThis, void** result, IInspectable* key);
ResourceDictionary_Lookup_t g_origResourceDictionaryLookupTaskbarView = nullptr;
ResourceDictionary_Lookup_t g_origResourceDictionaryLookupSearchUx = nullptr;

IInspectable* WINAPI ResourceDictionary_Lookup_TaskbarView_Hook(void* pThis, void** result, IInspectable* key)
{
    IInspectable* ret = g_origResourceDictionaryLookupTaskbarView(pThis, result, key);
    try
    {
        OverrideResourceLookup(key, ret);
    }
    catch (...)
    {
    }
    return ret;
}

IInspectable* WINAPI ResourceDictionary_Lookup_SearchUx_Hook(void* pThis, void** result, IInspectable* key)
{
    IInspectable* ret = g_origResourceDictionaryLookupSearchUx(pThis, result, key);
    try
    {
        OverrideResourceLookup(key, ret);
    }
    catch (...)
    {
    }
    return ret;
}

// --- Icon size, builds without dynamic icon scaling ---------------------------------------------------------

using ViewModel_GetIconHeight_t = int (WINAPI*)(void* pThis, void* param1, double* iconHeight);
ViewModel_GetIconHeight_t g_origTaskListItemViewModelGetIconHeight = nullptr;
ViewModel_GetIconHeight_t g_origTaskListGroupViewModelGetIconHeight = nullptr;

int WINAPI TaskListItemViewModel_GetIconHeight_Hook(void* pThis, void* param1, double* iconHeight)
{
    int ret = g_origTaskListItemViewModelGetIconHeight(pThis, param1, iconHeight);
    if (!HasDynamicIconScaling() && !IsUnloading() && iconHeight)
    {
        *iconHeight = CurrentIconSize();
    }
    return ret;
}

int WINAPI TaskListGroupViewModel_GetIconHeight_Hook(void* pThis, void* param1, double* iconHeight)
{
    int ret = g_origTaskListGroupViewModelGetIconHeight(pThis, param1, iconHeight);
    if (!HasDynamicIconScaling() && !IsUnloading() && iconHeight)
    {
        *iconHeight = CurrentIconSize();
    }
    return ret;
}

// These two are only called on builds where the icon size is a constant per posture. Being called at all
// means the feature flag was misleading, so the flag is cleared here.
using GetIconHeightInViewPixels_Enum_t = double (WINAPI*)(int enumTaskbarSize);
GetIconHeightInViewPixels_Enum_t g_origGetIconHeightInViewPixelsEnum = nullptr;

double WINAPI TaskbarConfiguration_GetIconHeightInViewPixels_Enum_Hook(int enumTaskbarSize)
{
    if (HasDynamicIconScaling())
    {
        SP_Log(L"The icon height is per posture after all; dynamic icon scaling is off");
        g_hasDynamicIconScaling.store(false, std::memory_order_relaxed);
    }
    if (!IsUnloading() && IsCustomizedPosture(enumTaskbarSize))
    {
        return CurrentIconSize();
    }
    return g_origGetIconHeightInViewPixelsEnum(enumTaskbarSize);
}

using GetIconHeightInViewPixels_Double_t = double (WINAPI*)(double baseHeight);
GetIconHeightInViewPixels_Double_t g_origGetIconHeightInViewPixelsDouble = nullptr;

double WINAPI TaskbarConfiguration_GetIconHeightInViewPixels_Double_Hook(double baseHeight)
{
    if (HasDynamicIconScaling())
    {
        SP_Log(L"The icon height is per posture after all; dynamic icon scaling is off");
        g_hasDynamicIconScaling.store(false, std::memory_order_relaxed);
    }
    if (!IsUnloading())
    {
        return CurrentIconSize();
    }
    return g_origGetIconHeightInViewPixelsDouble(baseHeight);
}

// --- Icon size, builds with dynamic icon scaling ------------------------------------------------------------

// The stock height tells the postures apart (16 small, 24 medium, 32 tablet); the custom one cannot, so the
// posture is noted here from the stock answer before the custom one is returned. TaskbarFrame::GetMetrics
// only uses the height to pick a button extent, so it gets the stock one and the GetMetrics hook is told.
using GetIconHeightInViewPixels_Method_t = double (WINAPI*)(void* pThis);
GetIconHeightInViewPixels_Method_t g_origGetIconHeightInViewPixelsMethod = nullptr;

double WINAPI TaskbarConfiguration_GetIconHeightInViewPixels_Method_Hook(void* pThis)
{
    double stock = g_origGetIconHeightInViewPixelsMethod(pThis);

    const bool small = stock <= 16;
    g_smallIconSize.store(small, std::memory_order_relaxed);

    if (g_inTaskbarFrameGetMetrics)
    {
        g_getMetricsIconHeight = stock;
        return stock;
    }
    if (!IsUnloading())
    {
        return IconSizeFor(small);
    }
    return stock;
}

// --- Frame height ---------------------------------------------------------------------------------------------

using GetFrameSize_t = double (WINAPI*)(int enumTaskbarSize);
GetFrameSize_t g_origTaskbarConfigurationGetFrameSize = nullptr;

double WINAPI TaskbarConfiguration_GetFrameSize_Hook(int enumTaskbarSize)
{
    if (IsCustomizedPosture(enumTaskbarSize) && !g_originalTaskbarHeight.load(std::memory_order_relaxed))
    {
        g_originalTaskbarHeight.store((int)g_origTaskbarConfigurationGetFrameSize(enumTaskbarSize),
                                      std::memory_order_relaxed);
    }

    const int height = CurrentTaskbarHeight();
    if (height && IsCustomizedPosture(enumTaskbarSize) && !IsVerticalTaskbar())
    {
        return height;
    }
    return g_origTaskbarConfigurationGetFrameSize(enumTaskbarSize);
}

// Older views cap the frame with MaxHeight; the cap is lifted before the height is set.
Element_SetDouble_t g_pfnTaskbarFrameMaxHeight = nullptr;       // resolved, not hooked
Element_SetDouble_t g_origTaskbarFrameHeight = nullptr;

void WINAPI TaskbarFrame_Height_Hook(void* pThis, double value)
{
    if (!IsVerticalTaskbar() && g_pfnTaskbarFrameMaxHeight)
    {
        g_pfnTaskbarFrameMaxHeight(pThis, std::numeric_limits<double>::infinity());
    }
    g_origTaskbarFrameHeight(pThis, value);
}

// Newer views set the frame height from the controller. The frame's MaxHeight is lifted first, and the grid
// the frame sits in is given the same height afterwards.
using Controller_UpdateFrameHeight_t = void (WINAPI*)(void* pThis);
Controller_UpdateFrameHeight_t g_origTaskbarControllerUpdateFrameHeight = nullptr;

void WINAPI TaskbarController_UpdateFrameHeight_Hook(void* pThis)
{
    if (IsVerticalTaskbar() || !g_offsetTaskbarFrame)
    {
        g_origTaskbarControllerUpdateFrameHeight(pThis);
        return;
    }

    try
    {
        void* taskbarFrame = *(void**)((BYTE*)pThis + g_offsetTaskbarFrame);
        FrameworkElement frame = ElementFromMember(taskbarFrame, 1);
        if (!frame)
        {
            g_origTaskbarControllerUpdateFrameHeight(pThis);
            return;
        }

        frame.MaxHeight(std::numeric_limits<double>::infinity());

        g_origTaskbarControllerUpdateFrameHeight(pThis);

        if (auto contentGrid = Media::VisualTreeHelper::GetParent(frame).try_as<FrameworkElement>())
        {
            double height = frame.Height();
            double gridHeight = contentGrid.Height();
            if (gridHeight > 0 && gridHeight != height)
            {
                contentGrid.Height(height);
            }
        }
    }
    catch (...)
    {
        SP_LogError(L"The taskbar frame height could not be adjusted");
    }
}

// Every layout pass of the frame ends here; ApplySettings waits for one to know the change has landed.
Produce_MeasureOverride_t g_origTaskbarFrameMeasureOverride = nullptr;

int WINAPI TaskbarFrame_MeasureOverride_Hook(void* pThis, Size size, Size* resultSize)
{
    g_hookCallCounter.fetch_add(1, std::memory_order_relaxed);
    int ret = g_origTaskbarFrameMeasureOverride(pThis, size, resultSize);
    g_pendingMeasureOverride.store(false, std::memory_order_relaxed);
    g_hookCallCounter.fetch_sub(1, std::memory_order_relaxed);
    return ret;
}

// The frame reads the button extents from the resource dictionary once and keeps them. The overflow flyout
// reads them again and fail-fasts unless the metrics extent is one of them, which would take explorer down
// after the width changed in between; so the metrics carry the current width.
using TaskbarFrame_GetMetrics_t = void* (WINAPI*)(void* pThis, void* metrics);
TaskbarFrame_GetMetrics_t g_origTaskbarFrameGetMetrics = nullptr;

void* WINAPI TaskbarFrame_GetMetrics_Hook(void* pThis, void* metrics)
{
    g_inTaskbarFrameGetMetrics = true;
    g_getMetricsIconHeight.reset();

    void* ret = g_origTaskbarFrameGetMetrics(pThis, metrics);

    g_inTaskbarFrameGetMetrics = false;
    std::optional<double> iconHeight = g_getMetricsIconHeight;

    // Without dynamic icon scaling the height is never consulted and the extent cannot be told. 32 is the
    // tablet extent, which is not customized.
    if (!metrics || !iconHeight || *iconHeight == 32)
    {
        return ret;
    }

    double wanted;
    if (*iconHeight == 16)
    {
        wanted = kStockSmallButtonWidth;
    }
    else
    {
        wanted = IsUnloading() ? kStockButtonWidth : CurrentButtonWidth();
    }

    // The button extent is the second member of TaskbarFrameMetrics.
    double* extent = (double*)((BYTE*)metrics + sizeof(double));
    if (*extent >= 1 && *extent < 10000 && *extent != wanted)
    {
        *extent = wanted;
    }
    return ret;
}

// ---------------------------------------------------------------------------------------------------------------
// Taskbar.View.dll hooks: the taskbar button
//
// With dynamic icon scaling the button keeps the icon height in a field and compares it against the stock
// 16 / 24 / 32 to decide paddings, badge style and clip shape. Those methods run with the stock height in the
// field and get the custom one back afterwards.
// ---------------------------------------------------------------------------------------------------------------

using Button_Method_t = void (WINAPI*)(void* pThis);
Button_Method_t g_origTaskListButtonUpdateButtonPadding = nullptr;
Button_Method_t g_origTaskListButtonUpdateBadge = nullptr;
Button_Method_t g_origTaskListButtonUpdateMultiWindowClip = nullptr;
Button_Method_t g_origTaskListButtonCreateMultiWindowClip = nullptr;
Button_Method_t g_origTaskListButtonUpdateVisualStates = nullptr;
void* g_pfnTaskListButtonIconHeight = nullptr;                  // resolved, for the field offset
void* g_pfnTaskListButtonUpdateIconColumnDefinition = nullptr;  // resolved, for the field offset

bool SwapButtonIconHeight()
{
    return HasDynamicIconScaling() && !IsUnloading() && g_offsetTaskListButtonIconHeight != 0;
}

void WINAPI TaskListButton_UpdateButtonPadding_Hook(void* pThis)
{
    if (!SwapButtonIconHeight())
    {
        g_origTaskListButtonUpdateButtonPadding(pThis);
        return;
    }
    ScopedField swap(pThis, g_offsetTaskListButtonIconHeight, PostureIconHeight());
    g_origTaskListButtonUpdateButtonPadding(pThis);
}

// A height of 16 turns badges into a dot; the badge code always sees 24. Overlay icons are the non-UWP
// badges, UpdateBadge the UWP ones.
using TaskListButton_OverlayIcon_t = void (WINAPI*)(void* pThis, void* stream);
TaskListButton_OverlayIcon_t g_origTaskListButtonOverlayIcon = nullptr;

void WINAPI TaskListButton_OverlayIcon_Hook(void* pThis, void* stream)
{
    if (!SwapButtonIconHeight())
    {
        g_origTaskListButtonOverlayIcon(pThis, stream);
        return;
    }
    ScopedField swap(pThis, g_offsetTaskListButtonIconHeight, 24.0);
    g_origTaskListButtonOverlayIcon(pThis, stream);
}

void WINAPI TaskListButton_UpdateBadge_Hook(void* pThis)
{
    if (!SwapButtonIconHeight())
    {
        g_origTaskListButtonUpdateBadge(pThis);
        return;
    }
    ScopedField swap(pThis, g_offsetTaskListButtonIconHeight, 24.0);
    g_origTaskListButtonUpdateBadge(pThis);
}

// The clip of the strip that marks a group of windows is chosen by comparing the height against 16, which a
// custom size may equal by coincidence.
void WINAPI TaskListButton_UpdateMultiWindowClip_Hook(void* pThis)
{
    if (!SwapButtonIconHeight())
    {
        g_origTaskListButtonUpdateMultiWindowClip(pThis);
        return;
    }
    ScopedField swap(pThis, g_offsetTaskListButtonIconHeight, PostureIconHeight());
    g_origTaskListButtonUpdateMultiWindowClip(pThis);
}

void WINAPI TaskListButton_CreateMultiWindowClip_Hook(void* pThis)
{
    if (!SwapButtonIconHeight())
    {
        g_origTaskListButtonCreateMultiWindowClip(pThis);
        return;
    }
    ScopedField swap(pThis, g_offsetTaskListButtonIconHeight, PostureIconHeight());
    g_origTaskListButtonCreateMultiWindowClip(pThis);
}

// The button's own copy of the extent is refreshed here, since the dictionary lookup only happens once per
// button. UpdateVisualStates also reads the icon height both as the posture marker and as the progress bar
// size: it gets the posture height, and ProgressBar::Width below gets the custom one back.
void WINAPI TaskListButton_UpdateVisualStates_Hook(void* pThis)
{
    if (g_offsetButtonExtent &&
        (g_applyingSettings.load(std::memory_order_relaxed) ||
         g_taskbarButtonWidthCustomized.load(std::memory_order_relaxed)))
    {
        double* extent = (double*)((BYTE*)pThis + g_offsetButtonExtent);
        if (*extent >= 1 && *extent < 10000)
        {
            const double wanted = IsUnloading() ? kStockButtonWidth : CurrentButtonWidth();
            if (wanted != *extent)
            {
                *extent = wanted;
                g_taskbarButtonWidthCustomized.store(true, std::memory_order_relaxed);
                TaskListButton_UpdateButtonPadding_Hook(pThis);
            }
        }
    }

    {
        std::optional<ScopedField> swap;
        if (SwapButtonIconHeight())
        {
            const double posture = PostureIconHeight();
            swap.emplace(pThis, g_offsetTaskListButtonIconHeight, posture);
            g_taskListButtonPostureIconHeight = posture;
            g_taskListButtonCustomIconHeight = swap->previous;
        }

        g_origTaskListButtonUpdateVisualStates(pThis);

        g_taskListButtonPostureIconHeight = 0;
    }

    // Without dynamic icon scaling the icon element keeps the size it was created with; while settings are
    // being applied it is resized by hand.
    if (g_applyingSettings.load(std::memory_order_relaxed) && !HasDynamicIconScaling())
    {
        try
        {
            if (auto button = ElementFromVtable(pThis, 3))
            {
                if (auto iconPanel = FindChildByName(button, L"IconPanel"))
                {
                    if (auto icon = FindChildByName(iconPanel, L"Icon"))
                    {
                        const double size = IsUnloading() ? kStockIconSize : CurrentIconSize();
                        icon.Width(size);
                        icon.Height(size);
                    }
                }
            }
        }
        catch (...)
        {
        }
    }
}

// The progress indicator is sized with the icon height, which UpdateVisualStates was just handed as the
// posture height; the width setter is a folded thunk, so the class is checked.
Element_SetDouble_t g_origProgressBarWidth = nullptr;

void WINAPI ProgressBar_Width_Hook(void* pThis, double width)
{
    if (g_taskListButtonPostureIconHeight && width == g_taskListButtonPostureIconHeight)
    {
        try
        {
            if (IsRuntimeClass(pThis, L"Microsoft.UI.Xaml.Controls.ProgressBar"))
            {
                width = g_taskListButtonCustomIconHeight;
            }
        }
        catch (...)
        {
        }
    }
    g_origProgressBarWidth(pThis, width);
}

// Taskbar extensions (the search button among them) size their icons with the height the host keeps, so it
// stays custom; UpdateDefaultWidth is the one place that uses it as a posture marker.
void* g_pfnTaskbarComponentHostIconHeight = nullptr;            // resolved, for the field offset
Button_Method_t g_origTaskbarComponentHostUpdateDefaultWidth = nullptr;

void WINAPI TaskbarComponentHost_UpdateDefaultWidth_Hook(void* pThis)
{
    if (!HasDynamicIconScaling() || IsUnloading() || !g_offsetTaskbarComponentHostIconHeight)
    {
        g_origTaskbarComponentHostUpdateDefaultWidth(pThis);
        return;
    }
    ScopedField swap(pThis, g_offsetTaskbarComponentHostIconHeight, PostureIconHeight());
    g_origTaskbarComponentHostUpdateDefaultWidth(pThis);
}

// ---------------------------------------------------------------------------------------------------------------
// Taskbar.View.dll hooks: Start, search and widgets buttons
// ---------------------------------------------------------------------------------------------------------------

using Element_GetDouble_t = double (WINAPI*)(void* pThis);
Element_GetDouble_t g_pfnExperienceToggleButtonIconHeightGet = nullptr;   // resolved, not hooked
Element_SetDouble_t g_pfnExperienceToggleButtonIconHeightSet = nullptr;   // resolved, not hooked
Button_Method_t g_origExperienceToggleButtonUpdateButtonPadding = nullptr;

// The setter ends by calling UpdateButtonPadding, which would re-enter the hook below.
void SetExperienceToggleButtonIconHeight(void* pThis, double height)
{
    g_inExperienceToggleButtonIconHeight = true;
    g_pfnExperienceToggleButtonIconHeightSet(pThis, height);
    g_inExperienceToggleButtonIconHeight = false;
}

// The widgets button's content is laid out for the stock icon size; its margins are scaled to the custom one.
void UpdateAugmentedEntryPointContent(FrameworkElement const& panel)
{
    auto contentGrid = FindChildByName(panel, L"AugmentedEntryPointContentGrid");
    if (!contentGrid)
    {
        return;
    }

    double marginValue = (double)(40 - CurrentIconSize()) / 2;
    if (marginValue < 0)
    {
        marginValue = 0;
    }
    const bool unloading = IsUnloading();
    const int taskbarHeight = CurrentTaskbarHeight();

    EnumChildElements(contentGrid, [&](FrameworkElement const& child) {
        if (winrt::get_class_name(child) != L"Windows.UI.Xaml.Controls.Grid")
        {
            return false;
        }
        auto panelGrid = FindChildByClassName(child, L"Windows.UI.Xaml.Controls.Grid");
        if (!panelGrid)
        {
            return false;
        }
        auto itemsPanel = FindChildByClassName(panelGrid, L"AdaptiveCards.Rendering.Uwp.WholeItemsPanel");
        if (!itemsPanel)
        {
            return false;
        }

        double labelsTopExtraMargin = 0;

        if (panelGrid.Width() > panelGrid.Height())
        {
            Thickness margin{ 3, 3, 3, 3 };
            if (!unloading && marginValue <= 3)
            {
                labelsTopExtraMargin = 3 - marginValue;
                margin.Left = marginValue;
                margin.Top = marginValue;
                // No right/bottom margin: tight values there make the icon vanish now and then.
                margin.Right = 0;
                margin.Bottom = 0;
            }
            itemsPanel.Margin(margin);
            panelGrid.VerticalAlignment(unloading ? VerticalAlignment::Stretch : VerticalAlignment::Center);
        }
        else
        {
            Thickness margin{ 8, 8, 8, 8 };
            if (!unloading)
            {
                margin.Left = marginValue;
                margin.Top = marginValue;
                margin.Right = 0;
                margin.Bottom = 0;
                if (taskbarHeight && taskbarHeight < 48)
                {
                    margin.Top -= (double)(48 - taskbarHeight) / 2;
                    if (margin.Top < 0)
                    {
                        margin.Top = 0;
                    }
                }
            }
            itemsPanel.Margin(margin);
        }

        FrameworkElement tickerGrid = itemsPanel;
        if (!(tickerGrid = FindChildByClassName(tickerGrid, L"Windows.UI.Xaml.Controls.Border")) ||
            !(tickerGrid = FindChildByClassName(tickerGrid, L"AdaptiveCards.Rendering.Uwp.WholeItemsPanel")) ||
            !(tickerGrid = FindChildByClassName(tickerGrid, L"Windows.UI.Xaml.Controls.Grid")))
        {
            return false;
        }

        const double badgeMax = unloading ? 24 : 40 - marginValue * 2;

        FrameworkElement badge = tickerGrid;
        if ((badge = FindChildByName(badge, L"SmallTicker1")) &&
            (badge = FindChildByClassName(badge, L"AdaptiveCards.Rendering.Uwp.WholeItemsPanel")) &&
            (badge = FindChildByName(badge, L"BadgeAnchorSmallTicker")))
        {
            badge.MaxWidth(badgeMax);
            badge.MaxHeight(badgeMax);
        }

        badge = tickerGrid;
        if ((badge = FindChildByName(badge, L"LargeTicker1")) &&
            (badge = FindChildByClassName(badge, L"AdaptiveCards.Rendering.Uwp.WholeItemsPanel")) &&
            (badge = FindChildByName(badge, L"BadgeAnchorLargeTicker")))
        {
            badge.MaxWidth(badgeMax);
            badge.MaxHeight(badgeMax);
        }

        if (auto labels = FindChildByName(tickerGrid, L"LargeTicker2"))
        {
            labels.Margin(Thickness{ 0, labelsTopExtraMargin, 0, 0 });
        }
        return false;
    });
}

// The Start / search / widgets buttons pick their extent and padding by comparing the icon height against the
// stock values, so they see the posture height for the call. The width of the root panel is then set from the
// custom button width; Start's padding differs between left and centre alignment, so it is read back.
void WINAPI ExperienceToggleButton_UpdateButtonPadding_Hook(void* pThis)
{
    if (g_inExperienceToggleButtonIconHeight)
    {
        return;
    }

    std::optional<double> previousHeight;
    try
    {
        if (HasDynamicIconScaling() && !IsUnloading() && g_pfnExperienceToggleButtonIconHeightGet &&
            g_pfnExperienceToggleButtonIconHeightSet)
        {
            const double posture = PostureIconHeight();
            const double current = g_pfnExperienceToggleButtonIconHeightGet(pThis);
            if (current != posture)
            {
                previousHeight = current;
                SetExperienceToggleButtonIconHeight(pThis, posture);
            }
        }
    }
    catch (...)
    {
    }

    g_origExperienceToggleButtonUpdateButtonPadding(pThis);

    try
    {
        if (previousHeight)
        {
            SetExperienceToggleButtonIconHeight(pThis, *previousHeight);
        }

        auto button = ElementFromMember(pThis, 1);
        if (!button)
        {
            return;
        }
        auto panel = FindChildByName(button, L"ExperienceToggleButtonRootPanel").try_as<Controls::Grid>();
        if (!panel)
        {
            return;
        }

        auto className = winrt::get_class_name(button);

        // The widgets button's UpdateButtonPadding tail-calls this one, so it arrives here as well.
        if (className == L"Taskbar.AugmentedEntryPointButton")
        {
            UpdateAugmentedEntryPointContent(panel);
            return;
        }

        if (HasDynamicIconScaling() && IsUnloading())
        {
            return;
        }

        double widthExtra = -4;
        if (className == L"Taskbar.ExperienceToggleButton")
        {
            if (Automation::AutomationProperties::GetAutomationId(button) == L"StartButton")
            {
                widthExtra = -3;
            }
        }
        else if (className == L"Taskbar.SearchBoxButton")
        {
            if (panel.Margin() != Thickness{})
            {
                return;     // a search box, not the icon-only button
            }
        }
        else
        {
            return;
        }

        const double width = panel.Width();
        if (!(width > 0))
        {
            return;
        }

        const bool small = IsSmallPosture();
        const double overrideWidth = IsUnloading() ? (small ? kStockSmallButtonWidth : kStockButtonWidth)
                                                   : ButtonWidthFor(small);
        auto padding = panel.Padding();
        const double wanted = overrideWidth + padding.Left + padding.Right + widthExtra;
        if (wanted != width)
        {
            panel.Width(wanted);
        }
    }
    catch (...)
    {
    }
}

// ---------------------------------------------------------------------------------------------------------------
// SearchUx.UI.dll hooks: the search button
// ---------------------------------------------------------------------------------------------------------------

Element_SetDouble_t g_origSearchButtonBaseIconHeight = nullptr;
Button_Method_t g_origSearchButtonBaseUpdateButtonPadding = nullptr;
Produce_MeasureOverride_t g_origSearchButtonBaseMeasureOverride = nullptr;

// The search box sizes the icon next to its text with the same property, so only the icon-only button is
// customized. The class name is used because the template is not applied yet when the height first arrives.
bool IsSearchIconButton(void* pThis)
{
    auto button = ElementFromMember(pThis, 1);
    return button && winrt::get_class_name(button) != L"SearchUx.SearchUI.SearchBoxButton";
}

void SetSearchButtonIconHeight(void* pThis, double height)
{
    g_inSearchButtonIconHeight = true;
    g_origSearchButtonBaseIconHeight(pThis, height);
    g_inSearchButtonIconHeight = false;
}

void WINAPI SearchButtonBase_IconHeight_Hook(void* pThis, double height)
{
    try
    {
        if (!IsUnloading() && IsSearchIconButton(pThis))
        {
            height = IconSizeFor(IsSmallPosture());
        }
    }
    catch (...)
    {
    }
    g_origSearchButtonBaseIconHeight(pThis, height);
}

Controls::Grid GetSearchButtonRootPanel(FrameworkElement const& button)
{
    auto panel = FindChildByName(button, L"SearchBoxButtonRootPanel").try_as<Controls::Grid>();
    if (!panel || FindChildByName(panel, L"SearchBoxTextBlock"))
    {
        return nullptr;     // a search box, not the icon-only button
    }
    return panel;
}

void SetSearchButtonRootPanelWidth(Controls::Grid const& panel)
{
    const double width = panel.Width();
    if (!(width > 0))
    {
        return;
    }
    const bool small = IsSmallPosture();
    const double overrideWidth = IsUnloading() ? (small ? kStockSmallButtonWidth : kStockButtonWidth)
                                               : ButtonWidthFor(small);
    auto padding = panel.Padding();
    const double wanted = overrideWidth + padding.Left + padding.Right - 4;
    if (wanted != width)
    {
        panel.Width(wanted);
    }
}

// The button applies its extent on template apply and on height changes, neither of which a settings change
// goes through, so the width is applied again on every measure.
int WINAPI SearchButtonBase_MeasureOverride_Hook(void* pThis, Size size, Size* resultSize)
{
    try
    {
        if (auto button = ElementFromVtable(pThis, 0))
        {
            if (auto panel = GetSearchButtonRootPanel(button))
            {
                SetSearchButtonRootPanelWidth(panel);
            }
        }
    }
    catch (...)
    {
    }
    return g_origSearchButtonBaseMeasureOverride(pThis, size, resultSize);
}

void WINAPI SearchButtonBase_UpdateButtonPadding_Hook(void* pThis)
{
    if (g_inSearchButtonIconHeight)
    {
        return;
    }

    std::optional<double> previousHeight;
    try
    {
        if (!IsUnloading() && g_origSearchButtonBaseIconHeight && IsSearchIconButton(pThis))
        {
            const double posture = PostureIconHeight();
            // Every write of the property goes through the hook above, so the custom size is what it holds.
            const double current = IconSizeFor(IsSmallPosture());
            if (current != posture)
            {
                previousHeight = current;
                SetSearchButtonIconHeight(pThis, posture);
            }
        }
    }
    catch (...)
    {
    }

    g_origSearchButtonBaseUpdateButtonPadding(pThis);

    try
    {
        if (previousHeight)
        {
            SetSearchButtonIconHeight(pThis, *previousHeight);
        }
        if (HasDynamicIconScaling() && IsUnloading())
        {
            return;
        }
        if (auto button = ElementFromMember(pThis, 1))
        {
            if (auto panel = GetSearchButtonRootPanel(button))
            {
                SetSearchButtonRootPanelWidth(panel);
            }
        }
    }
    catch (...)
    {
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Secondary taskbars are positioned through the app bar API; their rectangle gets the custom height.
// ---------------------------------------------------------------------------------------------------------------

UINT_PTR WINAPI SHAppBarMessage_Hook(DWORD dwMessage, PAPPBARDATA pData)
{
    UINT_PTR ret = g_origSHAppBarMessage(dwMessage, pData);

    const int height = CurrentTaskbarHeight();
    if (dwMessage == ABM_QUERYPOS && ret && pData && height && !IsVerticalTaskbar())
    {
        HMONITOR monitor = (HMONITOR)GetPropW(pData->hWnd, L"TaskbarMonitor");
        if (UINT dpi = GetMonitorDpi(monitor))
        {
            pData->rc.top = pData->rc.bottom - MulDiv(height, dpi, 96);
        }
    }
    return ret;
}

// ---------------------------------------------------------------------------------------------------------------
// Applying the settings
//
// The height is written into the mod's state and the taskbar is asked to re-lay itself out, which runs every
// hook above with the new numbers. The taskbar only re-lays out when the height it computes differs from the
// one it has, so applying the same height twice (a change of icon size alone) goes through a one-pixel detour.
// ---------------------------------------------------------------------------------------------------------------

#ifndef SPI_SETLOGICALDPIOVERRIDE
#define SPI_SETLOGICALDPIOVERRIDE 0x009F
#endif

void WaitForLayoutPass()
{
    // Only the frame's MeasureOverride clears the flag; without that hook there is nothing to wait for.
    if (!g_taskbarViewHooked.load(std::memory_order_relaxed) || !g_origTaskbarFrameMeasureOverride)
    {
        g_pendingMeasureOverride.store(false, std::memory_order_relaxed);
        return;
    }
    for (int i = 0; i < 100 && g_pendingMeasureOverride.load(std::memory_order_relaxed); ++i)
    {
        Sleep(100);
    }
}

void ApplySettings(int taskbarHeight)
{
    std::lock_guard<std::mutex> lock(g_applyMutex);

    if (taskbarHeight < 2)
    {
        taskbarHeight = 2;
    }

    HWND hTaskbar = FindTaskbarWindow();
    if (!hTaskbar)
    {
        // The taskbar is not up yet; it will be created with this height.
        SP_Log(L"No taskbar window yet; height %d will be used when it appears", taskbarHeight);
        g_taskbarHeight.store(taskbarHeight, std::memory_order_relaxed);
        return;
    }

    if (!g_taskbarHeight.load(std::memory_order_relaxed))
    {
        g_taskbarHeight.store(g_originalTaskbarHeight.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    if (!g_taskbarHeight.load(std::memory_order_relaxed))
    {
        // Nothing has asked for the frame size yet; measure the window instead.
        RECT rc{};
        GetWindowRect(hTaskbar, &rc);
        UINT dpi = GetMonitorDpi((HMONITOR)GetPropW(hTaskbar, L"TaskbarMonitor"));
        g_taskbarHeight.store(MulDiv(rc.bottom - rc.top, 96, dpi ? dpi : 96), std::memory_order_relaxed);
    }

    SP_Log(L"Applying height %d, icon size %d, button width %d", taskbarHeight, CurrentIconSize(),
           CurrentButtonWidth());

    g_applyingSettings.store(true, std::memory_order_relaxed);

    const bool vertical = IsVerticalTaskbar();

    if (!vertical && taskbarHeight == g_taskbarHeight.load(std::memory_order_relaxed))
    {
        // Same height: go through a different one first so the taskbar notices a change.
        g_pendingMeasureOverride.store(true, std::memory_order_relaxed);
        g_taskbarHeight.store(taskbarHeight - 1, std::memory_order_relaxed);
        SendMessageW(hTaskbar, WM_SETTINGCHANGE, SPI_SETLOGICALDPIOVERRIDE, 0);
        WaitForLayoutPass();
    }

    g_pendingMeasureOverride.store(true, std::memory_order_relaxed);
    g_taskbarHeight.store(taskbarHeight, std::memory_order_relaxed);
    SendMessageW(hTaskbar, WM_SETTINGCHANGE, SPI_SETLOGICALDPIOVERRIDE, 0);

    if (!vertical)
    {
        WaitForLayoutPass();
    }
    else
    {
        g_pendingMeasureOverride.store(false, std::memory_order_relaxed);
    }

    // The Win32 task band re-reads the display metrics on this message (CTaskBand::_HandleSyncDisplayChange).
    if (HWND hRebar = FindWindowExW(hTaskbar, nullptr, L"ReBarWindow32", nullptr))
    {
        if (HWND hTaskBand = FindWindowExW(hRebar, nullptr, L"MSTaskSwWClass", nullptr))
        {
            SendMessageW(hTaskBand, 0x452, 3, 0);
        }
    }

    g_applyingSettings.store(false, std::memory_order_relaxed);
}

// Settings are applied once every mod is up and the taskbar's view is hooked, whichever comes last.
void ApplyIfReady()
{
    if (g_afterInitDone.load(std::memory_order_relaxed) && g_taskbarViewHooked.load(std::memory_order_relaxed))
    {
        ApplySettings(g_settingTaskbarHeight.load(std::memory_order_relaxed));
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Installing the hooks
//
// Every symbol is optional: a name that a build has dropped costs that one hook, and its pointer stays null
// so the code above passes through. The names are the ones the PDBs give; several spellings where Windows
// renamed one between builds.
// ---------------------------------------------------------------------------------------------------------------

#define SYMBOL_HOOK(names, original, hook) \
    { names, ARRAYSIZE(names), (void**)&(original), (void*)(hook), TRUE }

void* g_pfnTaskbarControllerOnGroupingModeChanged = nullptr;    // resolved, for the field offset

BOOL HookTaskbarDll()
{
    HMODULE hModule = GetModuleHandleW(L"taskbar.dll");
    if (!hModule)
    {
        // The library explorer will use anyway; loading it early only moves the moment it is mapped.
        hModule = LoadLibraryExW(L"taskbar.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    }
    if (!hModule)
    {
        SP_LogError(L"taskbar.dll is not available");
        return FALSE;
    }

    static const wchar_t* const kIconUtilsGetIconSize[] = {
        LR"(void __cdecl IconUtils::GetIconSize(bool,enum IconUtils::IconType,struct tagSIZE *))",
    };
    static const wchar_t* const kIsStorageRecreationRequired[] = {
        LR"(public: virtual bool __cdecl IconContainer::IsStorageRecreationRequired(class CCoSimpleArray<unsigned int,4294967294,class CSimpleArrayStandardCompareHelper<unsigned int> > const &,enum IconContainerFlags))",
    };
    static const wchar_t* const kTrayUIGetMinSize[] = {
        LR"(public: virtual void __cdecl TrayUI::GetMinSize(struct HMONITOR__ *,struct tagSIZE *))",
    };
    static const wchar_t* const kTaskbarSettingsSize[] = {
        LR"(public: __cdecl winrt::impl::consume_WindowsUdk_UI_Shell_ITaskbarSettings<struct winrt::WindowsUdk::UI::Shell::ITaskbarSettings>::Size(void)const )",
    };
    static const wchar_t* const kGetClassLongPtrW[] = {
        LR"(public: virtual unsigned __int64 __cdecl CIconLoadingFunctions::GetClassLongPtrW(struct HWND__ *,int))",
    };
    static const wchar_t* const kSendMessageCallbackW[] = {
        LR"(public: virtual int __cdecl CIconLoadingFunctions::SendMessageCallbackW(struct HWND__ *,unsigned int,unsigned __int64,__int64,void (__cdecl*)(struct HWND__ *,unsigned int,unsigned __int64,__int64),unsigned __int64))",
    };
    static const wchar_t* const kLoadAsyncIconResumeCoro[] = {
        LR"(static  ShellIconLoaderV2::LoadAsyncIcon$_ResumeCoro$1())",
    };
    static const wchar_t* const kStuckTrayChange[] = {
        LR"(public: void __cdecl TrayUI::_StuckTrayChange(void))",
    };
    static const wchar_t* const kHandleSettingChange[] = {
        LR"(public: void __cdecl TrayUI::_HandleSettingChange(struct HWND__ *,unsigned int,unsigned __int64,__int64))",
    };

    SP_SymbolHook hooks[] = {
        SYMBOL_HOOK(kIconUtilsGetIconSize, g_origIconUtilsGetIconSize, IconUtils_GetIconSize_Hook),
        SYMBOL_HOOK(kIsStorageRecreationRequired, g_origIconContainerIsStorageRecreationRequired,
                    IconContainer_IsStorageRecreationRequired_Hook),
        SYMBOL_HOOK(kTrayUIGetMinSize, g_origTrayUIGetMinSize, TrayUI_GetMinSize_Hook),
        SYMBOL_HOOK(kTaskbarSettingsSize, g_origTaskbarSettingsSizeTaskbarDll, TaskbarSettings_Size_TaskbarDll_Hook),
        SYMBOL_HOOK(kGetClassLongPtrW, g_origCIconLoadingFunctionsGetClassLongPtrW,
                    CIconLoadingFunctions_GetClassLongPtrW_Hook),
        SYMBOL_HOOK(kSendMessageCallbackW, g_origCIconLoadingFunctionsSendMessageCallbackW,
                    CIconLoadingFunctions_SendMessageCallbackW_Hook),
        SYMBOL_HOOK(kLoadAsyncIconResumeCoro, g_origShellIconLoaderV2ResumeCoro, ShellIconLoaderV2_ResumeCoro_Hook),
        SYMBOL_HOOK(kStuckTrayChange, g_pfnTrayUIStuckTrayChange, nullptr),
        SYMBOL_HOOK(kHandleSettingChange, g_origTrayUIHandleSettingChange, TrayUI_HandleSettingChange_Hook),
    };

    if (!SP_HookSymbols(hModule, hooks, ARRAYSIZE(hooks)))
    {
        SP_LogError(L"The taskbar.dll hooks could not be installed");
        return FALSE;
    }

    if (!g_origTrayUIHandleSettingChange || !g_pfnTrayUIStuckTrayChange)
    {
        SP_Log(L"TrayUI's setting-change path was not found; changes will need a shell restart to show");
    }
    return TRUE;
}

// The tray's types: in SystemTray.dll on this build, inside Taskbar.View.dll before version 2604. The
// TaskbarSettings::Size getter is only added from SystemTray.dll, since the view batch already hooks its own.
void HookSystemTray(HMODULE hModule, bool withSettingsSize)
{
    static const wchar_t* const kTaskbarSettingsSize[] = {
        LR"(public: __cdecl winrt::impl::consume_WindowsUdk_UI_Shell_ITaskbarSettings<struct winrt::WindowsUdk::UI::Shell::ITaskbarSettings>::Size(void)const )",
    };
    static const wchar_t* const kControllerGetFrameSize[] = {
        LR"(private: double __cdecl winrt::SystemTray::implementation::SystemTrayController::GetFrameSize(enum winrt::WindowsUdk::UI::Shell::TaskbarSize))",
    };
    static const wchar_t* const kSecondaryGetFrameSize[] = {
        LR"(private: double __cdecl winrt::SystemTray::implementation::SystemTraySecondaryController::GetFrameSize(enum winrt::WindowsUdk::UI::Shell::TaskbarSize))",
    };
    static const wchar_t* const kControllerUpdateFrameSize[] = {
        LR"(private: void __cdecl winrt::SystemTray::implementation::SystemTrayController::UpdateFrameSize(void))",
    };
    static const wchar_t* const kSecondaryUpdateFrameSize[] = {
        LR"(private: void __cdecl winrt::SystemTray::implementation::SystemTraySecondaryController::UpdateFrameSize(void))",
    };
    static const wchar_t* const kFrameHeight[] = {
        LR"(public: __cdecl winrt::impl::consume_Windows_UI_Xaml_IFrameworkElement<struct winrt::SystemTray::SystemTrayFrame>::Height(double)const )",
    };
    static const wchar_t* const kFrameMeasureOverride[] = {
        LR"(public: virtual int __cdecl winrt::impl::produce<struct winrt::SystemTray::implementation::SystemTrayFrame,struct winrt::Windows::UI::Xaml::IFrameworkElementOverrides>::MeasureOverride(struct winrt::Windows::Foundation::Size,struct winrt::Windows::Foundation::Size *))",
    };
    static const wchar_t* const kOverridesMeasureOverride[] = {
        LR"(public: __cdecl winrt::impl::consume_Windows_UI_Xaml_IFrameworkElementOverrides<struct winrt::Windows::UI::Xaml::IFrameworkElementOverrides>::MeasureOverride(struct winrt::Windows::Foundation::Size const &)const )",
    };
    static const wchar_t* const kControllerView[] = {
        LR"(public: struct winrt::Windows::UI::Xaml::FrameworkElement __cdecl winrt::SystemTray::implementation::SystemTrayController::View(void))",
    };

    SP_SymbolHook hooks[] = {
        // Resolved only: the frame the controller sizes, for builds without the Height wrapper.
        SYMBOL_HOOK(kControllerView, g_pfnSystemTrayControllerView, nullptr),
        SYMBOL_HOOK(kControllerGetFrameSize, g_origSystemTrayControllerGetFrameSize, SystemTrayController_GetFrameSize_Hook),
        SYMBOL_HOOK(kSecondaryGetFrameSize, g_origSystemTraySecondaryControllerGetFrameSize,
                    SystemTraySecondaryController_GetFrameSize_Hook),
        // Resolved only: its field offset is read from its untouched code, then it is hooked below.
        SYMBOL_HOOK(kControllerUpdateFrameSize, g_pfnSystemTrayControllerUpdateFrameSize, nullptr),
        SYMBOL_HOOK(kSecondaryUpdateFrameSize, g_origSystemTraySecondaryControllerUpdateFrameSize,
                    SystemTraySecondaryController_UpdateFrameSize_Hook),
        SYMBOL_HOOK(kFrameHeight, g_origSystemTrayFrameHeight, SystemTrayFrame_Height_Hook),
        SYMBOL_HOOK(kFrameMeasureOverride, g_origSystemTrayFrameMeasureOverride, SystemTrayFrame_MeasureOverride_Hook),
        SYMBOL_HOOK(kOverridesMeasureOverride, g_origFrameworkElementOverridesMeasureOverride,
                    FrameworkElementOverrides_MeasureOverride_Hook),
        SYMBOL_HOOK(kTaskbarSettingsSize, g_origTaskbarSettingsSizeSystemTray, TaskbarSettings_Size_SystemTray_Hook),
    };
    const size_t count = withSettingsSize ? ARRAYSIZE(hooks) : ARRAYSIZE(hooks) - 1;

    if (!SP_HookSymbols(hModule, hooks, count))
    {
        SP_LogError(L"The system tray hooks could not be installed");
        return;
    }

    if (g_pfnSystemTrayControllerUpdateFrameSize)
    {
        g_offsetSystemTrayLastHeight = FindLastHeightOffset(g_pfnSystemTrayControllerUpdateFrameSize);
        SP_LogDebug(L"SystemTrayController last height offset: 0x%X", g_offsetSystemTrayLastHeight);

        if (!SP_SetFunctionHookNow(g_pfnSystemTrayControllerUpdateFrameSize,
                                   SystemTrayController_UpdateFrameSize_Hook,
                                   &g_origSystemTrayControllerUpdateFrameSize))
        {
            SP_LogError(L"SystemTrayController::UpdateFrameSize could not be hooked");
        }
    }

    g_systemTrayHooked.store(true, std::memory_order_relaxed);
    SP_Log(L"System tray hooks installed (frame size %s, update %s, height wrapper %s, view %s)",
           g_origSystemTrayControllerGetFrameSize ? L"yes" : L"no",
           g_origSystemTrayControllerUpdateFrameSize ? L"yes" : L"no",
           g_origSystemTrayFrameHeight ? L"yes" : L"no",
           g_pfnSystemTrayControllerView ? L"yes" : L"no");
}

void HookTaskbarView(HMODULE hModule)
{
    static const wchar_t* const kResourceDictionaryLookup[] = {
        LR"(public: __cdecl winrt::impl::consume_Windows_Foundation_Collections_IMap<struct winrt::Windows::UI::Xaml::ResourceDictionary,struct winrt::Windows::Foundation::IInspectable,struct winrt::Windows::Foundation::IInspectable>::Lookup(struct winrt::Windows::Foundation::IInspectable const &)const )",
    };
    static const wchar_t* const kItemViewModelGetIconHeight[] = {
        LR"(public: virtual int __cdecl winrt::impl::produce<struct winrt::Taskbar::implementation::TaskListItemViewModel,struct winrt::Taskbar::ITaskListItemViewModel>::GetIconHeight(void *,double *))",
    };
    static const wchar_t* const kGroupViewModelGetIconHeight[] = {
        LR"(public: virtual int __cdecl winrt::impl::produce<struct winrt::Taskbar::implementation::TaskListGroupViewModel,struct winrt::Taskbar::ITaskbarAppItemViewModel>::GetIconHeight(void *,double *))",
    };
    static const wchar_t* const kGetIconHeightEnum[] = {
        LR"(public: static double __cdecl winrt::Taskbar::implementation::TaskbarConfiguration::GetIconHeightInViewPixels(enum winrt::WindowsUdk::UI::Shell::TaskbarSize))",
    };
    static const wchar_t* const kGetIconHeightDouble[] = {
        LR"(public: static double __cdecl winrt::Taskbar::implementation::TaskbarConfiguration::GetIconHeightInViewPixels(double))",
    };
    static const wchar_t* const kGetIconHeightMethod[] = {
        LR"(public: double __cdecl winrt::Taskbar::implementation::TaskbarConfiguration::GetIconHeightInViewPixels(void))",
    };
    static const wchar_t* const kTaskListButtonIconHeight[] = {
        LR"(public: void __cdecl winrt::Taskbar::implementation::TaskListButton::IconHeight(double))",
    };
    static const wchar_t* const kTaskbarSettingsSize[] = {
        LR"(public: __cdecl winrt::impl::consume_WindowsUdk_UI_Shell_ITaskbarSettings<struct winrt::WindowsUdk::UI::Shell::ITaskbarSettings>::Size(void)const )",
    };
    static const wchar_t* const kGetFrameSize[] = {
        LR"(public: static double __cdecl winrt::Taskbar::implementation::TaskbarConfiguration::GetFrameSize(enum winrt::WindowsUdk::UI::Shell::TaskbarSize))",
    };
    static const wchar_t* const kFrameMaxHeight[] = {
        LR"(public: __cdecl winrt::impl::consume_Windows_UI_Xaml_IFrameworkElement<struct winrt::Taskbar::implementation::TaskbarFrame>::MaxHeight(double)const )",
    };
    static const wchar_t* const kFrameHeight[] = {
        LR"(public: __cdecl winrt::impl::consume_Windows_UI_Xaml_IFrameworkElement<struct winrt::Taskbar::implementation::TaskbarFrame>::Height(double)const )",
    };
    static const wchar_t* const kOnGroupingModeChanged[] = {
        LR"(private: void __cdecl winrt::Taskbar::implementation::TaskbarController::OnGroupingModeChanged(void))",
    };
    static const wchar_t* const kUpdateFrameHeight[] = {
        LR"(private: void __cdecl winrt::Taskbar::implementation::TaskbarController::UpdateFrameHeight(void))",
    };
    static const wchar_t* const kFrameMeasureOverride[] = {
        LR"(public: virtual int __cdecl winrt::impl::produce<struct winrt::Taskbar::implementation::TaskbarFrame,struct winrt::Windows::UI::Xaml::IFrameworkElementOverrides>::MeasureOverride(struct winrt::Windows::Foundation::Size,struct winrt::Windows::Foundation::Size *))",
    };
    static const wchar_t* const kFrameGetMetrics[] = {
        LR"(public: struct winrt::Taskbar::implementation::TaskbarFrameMetrics __cdecl winrt::Taskbar::implementation::TaskbarFrame::GetMetrics(void)const )",
    };
    static const wchar_t* const kButtonUpdateButtonPadding[] = {
        LR"(private: void __cdecl winrt::Taskbar::implementation::TaskListButton::UpdateButtonPadding(void))",
    };
    static const wchar_t* const kButtonOverlayIcon[] = {
        LR"(public: void __cdecl winrt::Taskbar::implementation::TaskListButton::OverlayIcon(struct winrt::Windows::Storage::Streams::IRandomAccessStream const &))",
    };
    static const wchar_t* const kButtonUpdateBadge[] = {
        LR"(private: void __cdecl winrt::Taskbar::implementation::TaskListButton::UpdateBadge(void))",
    };
    static const wchar_t* const kButtonUpdateMultiWindowClip[] = {
        LR"(private: void __cdecl winrt::Taskbar::implementation::TaskListButton::UpdateMultiWindowClip(void))",
    };
    static const wchar_t* const kButtonCreateMultiWindowClip[] = {
        LR"(private: void __cdecl winrt::Taskbar::implementation::TaskListButton::CreateMultiWindowClip(void))",
    };
    static const wchar_t* const kButtonUpdateIconColumnDefinition[] = {
        LR"(private: void __cdecl winrt::Taskbar::implementation::TaskListButton::UpdateIconColumnDefinition(void))",
    };
    static const wchar_t* const kButtonUpdateVisualStates[] = {
        LR"(private: void __cdecl winrt::Taskbar::implementation::TaskListButton::UpdateVisualStates(void))",
    };
    static const wchar_t* const kComponentHostIconHeight[] = {
        LR"(public: void __cdecl winrt::Microsoft::Windows::Taskbar::implementation::TaskbarComponentHost::IconHeight(double))",
    };
    static const wchar_t* const kComponentHostUpdateDefaultWidth[] = {
        LR"(private: void __cdecl winrt::Microsoft::Windows::Taskbar::implementation::TaskbarComponentHost::UpdateDefaultWidth(void))",
    };
    static const wchar_t* const kToggleButtonIconHeightGet[] = {
        LR"(public: double __cdecl winrt::Taskbar::implementation::ExperienceToggleButton::IconHeight(void)const )",
    };
    static const wchar_t* const kToggleButtonIconHeightSet[] = {
        LR"(public: void __cdecl winrt::Taskbar::implementation::ExperienceToggleButton::IconHeight(double))",
    };
    static const wchar_t* const kToggleButtonUpdateButtonPadding[] = {
        LR"(protected: virtual void __cdecl winrt::Taskbar::implementation::ExperienceToggleButton::UpdateButtonPadding(void))",
    };
    static const wchar_t* const kProgressBarWidth[] = {
        LR"(public: __cdecl winrt::impl::consume_Windows_UI_Xaml_IFrameworkElement<struct winrt::Microsoft::UI::Xaml::Controls::ProgressBar>::Width(double)const )",
    };

    SP_SymbolHook hooks[] = {
        SYMBOL_HOOK(kResourceDictionaryLookup, g_origResourceDictionaryLookupTaskbarView, ResourceDictionary_Lookup_TaskbarView_Hook),
        SYMBOL_HOOK(kItemViewModelGetIconHeight, g_origTaskListItemViewModelGetIconHeight, TaskListItemViewModel_GetIconHeight_Hook),
        SYMBOL_HOOK(kGroupViewModelGetIconHeight, g_origTaskListGroupViewModelGetIconHeight, TaskListGroupViewModel_GetIconHeight_Hook),
        SYMBOL_HOOK(kGetIconHeightEnum, g_origGetIconHeightInViewPixelsEnum, TaskbarConfiguration_GetIconHeightInViewPixels_Enum_Hook),
        SYMBOL_HOOK(kGetIconHeightDouble, g_origGetIconHeightInViewPixelsDouble, TaskbarConfiguration_GetIconHeightInViewPixels_Double_Hook),
        SYMBOL_HOOK(kGetIconHeightMethod, g_origGetIconHeightInViewPixelsMethod, TaskbarConfiguration_GetIconHeightInViewPixels_Method_Hook),
        SYMBOL_HOOK(kTaskListButtonIconHeight, g_pfnTaskListButtonIconHeight, nullptr),
        SYMBOL_HOOK(kTaskbarSettingsSize, g_origTaskbarSettingsSizeTaskbarView, TaskbarSettings_Size_TaskbarView_Hook),
        SYMBOL_HOOK(kGetFrameSize, g_origTaskbarConfigurationGetFrameSize, TaskbarConfiguration_GetFrameSize_Hook),
        SYMBOL_HOOK(kFrameMaxHeight, g_pfnTaskbarFrameMaxHeight, nullptr),
        SYMBOL_HOOK(kFrameHeight, g_origTaskbarFrameHeight, TaskbarFrame_Height_Hook),
        SYMBOL_HOOK(kOnGroupingModeChanged, g_pfnTaskbarControllerOnGroupingModeChanged, nullptr),
        SYMBOL_HOOK(kUpdateFrameHeight, g_origTaskbarControllerUpdateFrameHeight, TaskbarController_UpdateFrameHeight_Hook),
        SYMBOL_HOOK(kFrameMeasureOverride, g_origTaskbarFrameMeasureOverride, TaskbarFrame_MeasureOverride_Hook),
        SYMBOL_HOOK(kFrameGetMetrics, g_origTaskbarFrameGetMetrics, TaskbarFrame_GetMetrics_Hook),
        SYMBOL_HOOK(kButtonUpdateButtonPadding, g_origTaskListButtonUpdateButtonPadding, TaskListButton_UpdateButtonPadding_Hook),
        SYMBOL_HOOK(kButtonOverlayIcon, g_origTaskListButtonOverlayIcon, TaskListButton_OverlayIcon_Hook),
        SYMBOL_HOOK(kButtonUpdateBadge, g_origTaskListButtonUpdateBadge, TaskListButton_UpdateBadge_Hook),
        SYMBOL_HOOK(kButtonUpdateMultiWindowClip, g_origTaskListButtonUpdateMultiWindowClip, TaskListButton_UpdateMultiWindowClip_Hook),
        SYMBOL_HOOK(kButtonCreateMultiWindowClip, g_origTaskListButtonCreateMultiWindowClip, TaskListButton_CreateMultiWindowClip_Hook),
        SYMBOL_HOOK(kButtonUpdateIconColumnDefinition, g_pfnTaskListButtonUpdateIconColumnDefinition, nullptr),
        SYMBOL_HOOK(kButtonUpdateVisualStates, g_origTaskListButtonUpdateVisualStates, TaskListButton_UpdateVisualStates_Hook),
        SYMBOL_HOOK(kComponentHostIconHeight, g_pfnTaskbarComponentHostIconHeight, nullptr),
        SYMBOL_HOOK(kComponentHostUpdateDefaultWidth, g_origTaskbarComponentHostUpdateDefaultWidth, TaskbarComponentHost_UpdateDefaultWidth_Hook),
        SYMBOL_HOOK(kToggleButtonIconHeightGet, g_pfnExperienceToggleButtonIconHeightGet, nullptr),
        SYMBOL_HOOK(kToggleButtonIconHeightSet, g_pfnExperienceToggleButtonIconHeightSet, nullptr),
        SYMBOL_HOOK(kToggleButtonUpdateButtonPadding, g_origExperienceToggleButtonUpdateButtonPadding, ExperienceToggleButton_UpdateButtonPadding_Hook),
        SYMBOL_HOOK(kProgressBarWidth, g_origProgressBarWidth, ProgressBar_Width_Hook),
    };

    // Resolve first: the offsets and the scaling mode are read from the functions before anything is
    // patched, and are in place by the time the first hook fires.
    if (!SP_ResolveSymbols(hModule, hooks, ARRAYSIZE(hooks)))
    {
        SP_LogError(L"The Taskbar.View.dll symbols could not be resolved");
        return;
    }

    g_offsetTaskListButtonIconHeight = FindMovsdLoadFromRcx(g_pfnTaskListButtonIconHeight, 0x80);
    g_offsetTaskbarComponentHostIconHeight = FindMovsdLoadFromRcx(g_pfnTaskbarComponentHostIconHeight, 0x80);
    g_offsetTaskbarFrame = FindTaskbarFrameOffset(g_pfnTaskbarControllerOnGroupingModeChanged);
    g_offsetButtonExtent = FindButtonExtentOffset(g_pfnTaskListButtonUpdateIconColumnDefinition);
    SP_LogDebug(L"Offsets: button icon height 0x%X, host icon height 0x%X, frame 0x%X, extent 0x%X",
                g_offsetTaskListButtonIconHeight, g_offsetTaskbarComponentHostIconHeight, g_offsetTaskbarFrame,
                g_offsetButtonExtent);

    constexpr UINT32 kDynamicIconScalingFeature = 29785184;
    if (g_origGetIconHeightInViewPixelsMethod && IsOsFeatureEnabled(kDynamicIconScalingFeature).value_or(true))
    {
        g_hasDynamicIconScaling.store(true, std::memory_order_relaxed);
    }
    SP_Log(L"Dynamic icon scaling: %s", HasDynamicIconScaling() ? L"on" : L"off");

    if (!SP_HookSymbols(hModule, hooks, ARRAYSIZE(hooks)))
    {
        SP_LogError(L"The Taskbar.View.dll hooks could not be installed");
        return;
    }

    if (!g_origTaskbarConfigurationGetFrameSize)
    {
        SP_LogError(L"TaskbarConfiguration::GetFrameSize was not found; the height cannot be changed");
    }
    if (!g_origTaskbarFrameMeasureOverride)
    {
        SP_LogError(L"TaskbarFrame::MeasureOverride was not found; changes are applied without waiting");
    }
}

void HookSearchUx(HMODULE hModule)
{
    static const wchar_t* const kResourceDictionaryLookup[] = {
        LR"(public: __cdecl winrt::impl::consume_Windows_Foundation_Collections_IMap<struct winrt::Windows::UI::Xaml::ResourceDictionary,struct winrt::Windows::Foundation::IInspectable,struct winrt::Windows::Foundation::IInspectable>::Lookup(struct winrt::Windows::Foundation::IInspectable const &)const )",
    };
    static const wchar_t* const kIconHeight[] = {
        LR"(public: void __cdecl winrt::SearchUx::SearchUI::implementation::SearchButtonBase::IconHeight(double))",
    };
    static const wchar_t* const kUpdateButtonPadding[] = {
        LR"(protected: virtual void __cdecl winrt::SearchUx::SearchUI::implementation::SearchButtonBase::UpdateButtonPadding(void))",
    };
    static const wchar_t* const kMeasureOverride[] = {
        LR"(public: virtual int __cdecl winrt::impl::produce<struct winrt::SearchUx::SearchUI::implementation::SearchButtonBase,struct winrt::Windows::UI::Xaml::IFrameworkElementOverrides>::MeasureOverride(struct winrt::Windows::Foundation::Size,struct winrt::Windows::Foundation::Size *))",
    };

    SP_SymbolHook hooks[] = {
        SYMBOL_HOOK(kResourceDictionaryLookup, g_origResourceDictionaryLookupSearchUx, ResourceDictionary_Lookup_SearchUx_Hook),
        SYMBOL_HOOK(kIconHeight, g_origSearchButtonBaseIconHeight, SearchButtonBase_IconHeight_Hook),
        SYMBOL_HOOK(kUpdateButtonPadding, g_origSearchButtonBaseUpdateButtonPadding, SearchButtonBase_UpdateButtonPadding_Hook),
        SYMBOL_HOOK(kMeasureOverride, g_origSearchButtonBaseMeasureOverride, SearchButtonBase_MeasureOverride_Hook),
    };

    if (!SP_HookSymbols(hModule, hooks, ARRAYSIZE(hooks)))
    {
        SP_LogError(L"The SearchUx.UI.dll hooks could not be installed");
        return;
    }
    SP_Log(L"Search button hooks installed");
}

#undef SYMBOL_HOOK

// --- Module callbacks (each on its own helper thread) ---------------------------------------------------------

void OnSystemTrayLoaded(HMODULE hModule, void*)
{
    HookSystemTray(hModule, true);
    ApplyIfReady();
}

void OnTaskbarViewLoaded(HMODULE hModule, void*)
{
    const WORD major = GetModuleVersionMajor(hModule);
    SP_Log(L"Taskbar.View.dll %u is loaded", major);

    HookTaskbarView(hModule);

    if (major && major < 2604)
    {
        // The tray's types are still inside the view on this build.
        HookSystemTray(hModule, false);
    }
    else
    {
        // On a worker: the callback applies the settings, which sends to the taskbar and waits for its
        // layout pass. On the loading thread (the taskbar's own) that deadlocks against an apply already
        // inside SendMessage to the taskbar (both hold g_applyMutex).
        SP_WaitForModuleOnWorker(L"SystemTray.dll", 60000, OnSystemTrayLoaded, nullptr);
    }

    g_taskbarViewHooked.store(true, std::memory_order_relaxed);
    ApplyIfReady();
}

void OnSearchUxLoaded(HMODULE hModule, void*)
{
    HookSearchUx(hModule);
}

// ---------------------------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------------------------

int ClampSetting(int value, int low, int high)
{
    return value < low ? low : (value > high ? high : value);
}

void LoadSettings()
{
    g_settingTaskbarHeight.store(ClampSetting(SP_GetIntSetting(L"TaskbarHeight", kStockTaskbarHeight), 2, 256),
                                 std::memory_order_relaxed);
    g_settingIconSize.store(ClampSetting(SP_GetIntSetting(L"IconSize", kStockIconSize), 1, 128),
                            std::memory_order_relaxed);
    g_settingButtonWidth.store(ClampSetting(SP_GetIntSetting(L"TaskbarButtonWidth", kStockButtonWidth), 1, 512),
                               std::memory_order_relaxed);
    SP_Log(L"Settings: height %d, icon size %d, button width %d",
           g_settingTaskbarHeight.load(), g_settingIconSize.load(), g_settingButtonWidth.load());
}

BOOL Init()
{
    // The DLL stays resident across unload/load, so the run state of the previous life is cleared here; with
    // g_unloading left true every hook keeps answering the stock numbers.
    g_unloading.store(false, std::memory_order_relaxed);
    g_taskbarViewHooked.store(false, std::memory_order_relaxed);
    g_systemTrayHooked.store(false, std::memory_order_relaxed);
    g_afterInitDone.store(false, std::memory_order_relaxed);
    g_applyingSettings.store(false, std::memory_order_relaxed);
    g_pendingMeasureOverride.store(false, std::memory_order_relaxed);
    g_hasDynamicIconScaling.store(false, std::memory_order_relaxed);
    g_taskbarButtonWidthCustomized.store(false, std::memory_order_relaxed);
    g_iconLoaderThreadId.store(0, std::memory_order_relaxed);

    LoadSettings();

    if (!HookTaskbarDll())
    {
        return FALSE;
    }

    // Secondary taskbars are placed through the app bar API, and the older icon loader asks windows for
    // their icon through SendMessageTimeoutW.
    if (SP_HookBegin())
    {
        if (!SP_SetExportHook(L"shell32.dll", "SHAppBarMessage", SHAppBarMessage_Hook, &g_origSHAppBarMessage) ||
            !SP_SetExportHook(L"user32.dll", "SendMessageTimeoutW", SendMessageTimeoutW_Hook, &g_origSendMessageTimeoutW))
        {
            SP_HookAbort();
            g_origSHAppBarMessage = nullptr;
            g_origSendMessageTimeoutW = nullptr;
            SP_LogError(L"The shell32/user32 exports could not be hooked; secondary taskbars keep their height");
        }
        else
        {
            SP_HookCommit();
        }
    }

    // The XAML taskbar and the search button come up after the shell starts.
    // The view callback applies the settings too (see OnSystemTrayLoaded): a worker thread, never the loader's.
    SP_WaitForModuleOnWorker(L"Taskbar.View.dll", 60000, OnTaskbarViewLoaded, nullptr);
    SP_WaitForModule(L"SearchUx.UI.dll", 0, OnSearchUxLoaded, nullptr);

    return TRUE;
}

void AfterInit()
{
    g_afterInitDone.store(true, std::memory_order_relaxed);
    ApplyIfReady();
}

void SettingsChanged()
{
    LoadSettings();
    if (g_taskbarViewHooked.load(std::memory_order_relaxed))
    {
        ApplySettings(g_settingTaskbarHeight.load(std::memory_order_relaxed));
    }
}

void BeforeUninit()
{
    // Still hooked: every hook now answers the stock numbers, and one more layout pass puts them on screen.
    g_unloading.store(true, std::memory_order_relaxed);

    if (g_taskbarViewHooked.load(std::memory_order_relaxed))
    {
        const int original = g_originalTaskbarHeight.load(std::memory_order_relaxed);
        ApplySettings(original ? original : kStockTaskbarHeight);
    }
}

void Uninit()
{
    // A layout pass that was inside the measure hook when the hooks were removed must finish before this code
    // goes away.
    for (int i = 0; i < 50 && g_hookCallCounter.load(std::memory_order_relaxed) > 0; ++i)
    {
        Sleep(100);
    }
}

}   // namespace

SP_MOD_DEFINE(g_modTaskbarIconSize) =
{
    /* id             */ SP_MOD_ID,
    /* name           */ L"Taskbar height and icon size",
    /* basedOn        */ "taskbar-icon-size",
    /* originalAuthor */ "m417z",
    /* targets        */ SP_TARGET_EXPLORER,
    /* minOsBuild     */ 22621,     // the symbols are those of the 22H2 and later XAML taskbar
    /* flags          */ 0,
    /* Init           */ Init,
    /* AfterInit      */ AfterInit,
    /* SettingsChanged*/ SettingsChanged,
    /* BeforeUninit   */ BeforeUninit,
    /* Uninit         */ Uninit,
};
