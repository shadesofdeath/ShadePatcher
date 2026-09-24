//
// taskbar_labels - shows text labels on the Windows 11 taskbar buttons and keeps their width in check.
//
// Adapted from the idea behind the Windhawk mod "Taskbar Labels for Windows 11" (taskbar-labels) by m417z. The
// logic is written against this engine's API and keeps only the path for the taskbar that has Windows' own
// labels implementation (22621.2361 and later, this machine is 26200). The original's custom label rendering
// for the taskbars before that, its per-program exclusions, its font and padding options and its running
// indicator styles are not carried over.
//
// What it does
// ------------
// Windows decides whether a button gets a label from one setting, "Combine taskbar buttons and hide labels":
// never combine means labels, always combine means none. Three things are changed here:
//
//   Mode 0  labels on every button, combined or not. Windows' combining choice is left alone; a combined group
//           simply gets its label too, the way Windows never lets it.
//   Mode 1  Windows decides, as if the mod were not there; only the width limits below apply.
//   Mode 2  no labels on any button, whatever the combining choice is.
//
//   MinimumTaskbarItemWidth  how narrow a labelled button may get before the taskbar starts overflowing.
//                            Windows' own floor is high, so only a handful of labelled buttons fit.
//   MaximumTaskbarItemWidth  how wide a labelled button may grow; long titles end in an ellipsis instead of
//                            being cut mid-letter, which is what Windows does.
//
// Where it hooks (all in Taskbar.View.dll, the XAML taskbar)
// ----------------------------------------------------------
// Every button is a TaskListButton whose view model answers HasLabel. Two view models exist: one per window
// (TaskListWindowViewModel, labelled when not combining) and one per group (TaskListGroupViewModel, never
// labelled). Their get_HasLabel implementations are hooked and the answer changed by mode. Only the calls that
// come through Taskbar.View.dll's own HasLabel wrapper are changed: other modules ask the same question for
// their own purposes and are left with the real answer, so the wrapper is hooked too, to mark those calls.
//
// TaskListButton::UpdateVisualStates is where the button lays itself out after any change; the label's
// maximum width and trimming are set right after it runs. get_MinScalableWidth is what the taskbar asks
// before it decides to overflow; it is answered with the minimum width setting.
//
// Applying a change live
// ----------------------
// The taskbar rebuilds its buttons when the combining setting changes. To make it do that on demand, the
// taskbar window is sent WM_SETTINGCHANGE while a hook on RegGetValueW answers the opposite combining mode,
// then again with the hook out of the way; the second pass rebuilds everything with the real setting and
// the mod's current answers. That is how the original applies its settings and how this mod applies them on
// SettingsChanged and puts everything back on BeforeUninit.
//
// Exceptions
// ----------
// The layout work uses C++/WinRT, which reports failures by throwing, so this file is compiled with exceptions
// on. Everything XAML can call into is wrapped: an exception reaching the taskbar's UI thread would end the
// shell.
//
#define SP_MOD_ID "taskbar-labels"
#include "engine/modapi.h"

#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// windows.h defines GetCurrentTime as a macro, which collides with a XAML method of the same name.
#undef GetCurrentTime

#include <winrt/Windows.Foundation.h>
// The grid's column list is an IVector, whose methods only become callable with this header.
#include <winrt/Windows.Foundation.Collections.h>
#include <winstring.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>

namespace {

using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using winrt::Windows::UI::Xaml::Media::VisualTreeHelper;

// ---------------------------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------------------------

enum class Mode
{
    Always = 0,          // labels on every button, combined or not
    FollowWindows = 1,   // Windows' "combine and hide labels" setting decides
    Never = 2,           // no labels at all
};

// The original's defaults: 50 lets several labelled buttons sit side by side before the overflow appears;
// 176 is the width Windows itself grows a labelled button to.
constexpr int kDefaultMinWidth = 50;
constexpr int kDefaultMaxWidth = 176;

std::atomic<Mode> g_mode{ Mode::Always };
std::atomic<int>  g_minWidth{ kDefaultMinWidth };   // 0: leave Windows' value
std::atomic<int>  g_maxWidth{ kDefaultMaxWidth };   // 0: leave Windows' value

std::atomic<bool> g_hooked{ false };        // the Taskbar.View.dll hooks are in
std::atomic<bool> g_unloading{ false };     // hooks answer as Windows would, layout is put back
std::atomic<bool> g_flipGrouping{ false };  // RegGetValueW answers the opposite combining mode

// Set while Taskbar.View.dll's own HasLabel wrapper is on the stack of this thread. The view models' get_HasLabel
// is only answered differently inside it.
thread_local int t_inHasLabel = 0;

void LoadSettings()
{
    int mode = SP_GetIntSetting(L"Mode", (int)Mode::Always);
    if (mode < (int)Mode::Always || mode > (int)Mode::Never)
    {
        mode = (int)Mode::Always;
    }
    g_mode.store((Mode)mode, std::memory_order_relaxed);

    int minWidth = SP_GetIntSetting(L"MinimumTaskbarItemWidth", kDefaultMinWidth);
    if (minWidth < 0 || minWidth > 1000)
    {
        minWidth = kDefaultMinWidth;
    }
    g_minWidth.store(minWidth, std::memory_order_relaxed);

    int maxWidth = SP_GetIntSetting(L"MaximumTaskbarItemWidth", kDefaultMaxWidth);
    if (maxWidth < 0 || maxWidth > 2000)
    {
        maxWidth = kDefaultMaxWidth;
    }
    g_maxWidth.store(maxWidth, std::memory_order_relaxed);

    SP_Log(L"Mode %d, minimum width %d, maximum width %d", mode, minWidth, maxWidth);
}

// ---------------------------------------------------------------------------------------------------------------
// Resolved functions
// ---------------------------------------------------------------------------------------------------------------

using TaskListButton_get_IsRunning_t = HRESULT(WINAPI*)(void* pThis, bool* running);
using TaskListButton_UpdateVisualStates_t = void(WINAPI*)(void* pThis);
using ITaskbarButton_get_MinScalableWidth_t = HRESULT(WINAPI*)(void* pThis, float* minWidth);
using ITaskbarAppItemViewModel_HasLabel_t = bool(WINAPI*)(void* pThis);
using ViewModel_get_HasLabel_t = HRESULT(WINAPI*)(void* pThis, bool* hasLabel);
using WilFeature_IsEnabled_t = bool(WINAPI*)(void* pThis, int reportingKind);
using RegGetValueW_t = decltype(&RegGetValueW);
using TaskListButton_HasLabel_t = void(WINAPI*)(void* pThis, bool hasLabel);

TaskListButton_HasLabel_t             g_origTaskListButtonHasLabel = nullptr;
TaskListButton_get_IsRunning_t        g_TaskListButton_get_IsRunning = nullptr;
TaskListButton_UpdateVisualStates_t   g_origUpdateVisualStates = nullptr;
ITaskbarButton_get_MinScalableWidth_t g_origGetMinScalableWidth = nullptr;
ITaskbarAppItemViewModel_HasLabel_t   g_origHasLabelWrapper = nullptr;
ViewModel_get_HasLabel_t              g_origWindowGetHasLabel = nullptr;
ViewModel_get_HasLabel_t              g_origGroupGetHasLabel = nullptr;
void*                                 g_wilFeatureImpl = nullptr;
WilFeature_IsEnabled_t                g_wilFeatureIsEnabled = nullptr;
RegGetValueW_t                        g_origRegGetValueW = nullptr;

// ---------------------------------------------------------------------------------------------------------------
// XAML helpers
// ---------------------------------------------------------------------------------------------------------------

FrameworkElement FindChildByName(FrameworkElement const& element, const wchar_t* name)
{
    const int count = VisualTreeHelper::GetChildrenCount(element);
    for (int i = 0; i < count; ++i)
    {
        auto child = VisualTreeHelper::GetChild(element, i).try_as<FrameworkElement>();
        if (child && child.Name() == name)
        {
            return child;
        }
    }
    return nullptr;
}

// The XAML object behind a TaskListButton implementation pointer. The implementation object starts with one
// vtable pointer per interface it implements, and the fourth slot is the one the original mods on this taskbar
// all read the element through; asking it for FrameworkElement goes through a normal QueryInterface.
FrameworkElement ButtonFromImplementation(void* pThis)
{
    if (!pThis)
    {
        return nullptr;
    }

    void* abi = (void**)pThis + 3;
    if (!*(void**)abi)
    {
        return nullptr;
    }

    winrt::Windows::Foundation::IUnknown unknown{ nullptr };
    winrt::copy_from_abi(unknown, abi);
    return unknown.try_as<FrameworkElement>();
}

// Whether the button holds a running window. A pinned program that is not running is the case that matters:
// in mode 0 its group is told it has a label, and that label is hidden again here.
bool IsRunning(FrameworkElement const& button)
{
    try
    {
        if (g_TaskListButton_get_IsRunning)
        {
            auto unknown = button.as<winrt::Windows::Foundation::IUnknown>();
            bool running = false;
            HRESULT hr = g_TaskListButton_get_IsRunning(winrt::get_abi(unknown), &running);
            return SUCCEEDED(hr) ? running : true;
        }

        // Build 26200 has no get_IsRunning; its taskbar hides the label of a pinned program with no window by
        // itself (observed: forcing HasLabel on such a button shows nothing), so the label is left alone.
        return true;
    }
    catch (...)
    {
        return true;
    }
}

// Sets the label's maximum width and trimming on a button, or puts them back when unloading. Runs on the
// taskbar's UI thread, right after the button laid itself out.
void ApplyLabelStyle(FrameworkElement const& button)
{
    // The overflow flyout lists the same control; only the buttons on the taskbar itself are changed.
    auto parent = VisualTreeHelper::GetParent(button).try_as<FrameworkElement>();
    if (!parent || parent.Name() != L"TaskbarFrameRepeater")
    {
        return;
    }

    auto iconPanel = FindChildByName(button, L"IconPanel").try_as<Grid>();
    if (!iconPanel)
    {
        return;
    }

    // Windows' labelled layout was a two-column grid: the icon, then an auto-sized label column. Build 26200's
    // IconPanel has no column definitions (the icon and the label are placed by margins), so the icon's 40 px
    // is assumed there.
    auto columns = iconPanel.ColumnDefinitions();
    const bool twoColumns = columns && columns.Size() == 2;

    auto label = FindChildByName(iconPanel, L"LabelControl").try_as<TextBlock>();
    if (!label)
    {
        return;
    }

    const bool unloading = g_unloading.load(std::memory_order_relaxed);
    const Mode mode = g_mode.load(std::memory_order_relaxed);

    // Mode 0 gives every group a label, which includes a pinned program that is not running; Windows never
    // shows a label there, so it is hidden. Collapsed rather than removed: the original found that removing it
    // leaves the running indicator behind the active button's highlight.
    if (!unloading && mode == Mode::Always && !IsRunning(button))
    {
        if (label.Visibility() != Visibility::Collapsed)
        {
            label.Visibility(Visibility::Collapsed);
        }
        return;
    }
    if (label.Visibility() != Visibility::Visible)
    {
        label.Visibility(Visibility::Visible);
    }

    // The label column is auto-sized, so the button's width is capped by capping the label. Its margins count
    // towards the column, so they are taken off the limit.
    const int maxWidth = unloading ? 0 : g_maxWidth.load(std::memory_order_relaxed);
    if (maxWidth > 0)
    {
        double iconColumnWidth = 40.0;
        if (twoColumns)
        {
            auto iconColumn = columns.GetAt(0).Width();
            if (iconColumn.GridUnitType == GridUnitType::Pixel)
            {
                iconColumnWidth = iconColumn.Value;
            }
        }
        auto margin = label.Margin();
        const double columnWidth = std::fmax(0.0, (double)maxWidth - iconColumnWidth);
        const double labelMaxWidth = std::fmax(0.0, columnWidth - margin.Left - margin.Right);

        // The comparison matters: a layout change here runs the button's layout again, which lands back here.
        if (label.MaxWidth() != labelMaxWidth)
        {
            label.MaxWidth(labelMaxWidth);
            button.InvalidateMeasure();
        }
    }
    else if (label.ReadLocalValue(FrameworkElement::MaxWidthProperty()) != DependencyProperty::UnsetValue())
    {
        label.ClearValue(FrameworkElement::MaxWidthProperty());
        button.InvalidateMeasure();
    }

    // A title that does not fit ends in an ellipsis. Windows clips it at the pixel instead, which is what the
    // label goes back to when the mod unloads.
    if (unloading)
    {
        if (label.ReadLocalValue(TextBlock::TextTrimmingProperty()) != DependencyProperty::UnsetValue())
        {
            label.ClearValue(TextBlock::TextTrimmingProperty());
        }
    }
    else if (label.TextTrimming() != TextTrimming::CharacterEllipsis)
    {
        label.TextTrimming(TextTrimming::CharacterEllipsis);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Hooks in Taskbar.View.dll
// ---------------------------------------------------------------------------------------------------------------

void WINAPI TaskListButton_UpdateVisualStates_Hook(void* pThis)
{
    g_origUpdateVisualStates(pThis);

    // XAML is on the stack below; nothing may escape back into it.
    try
    {
        if (auto button = ButtonFromImplementation(pThis))
        {
            ApplyLabelStyle(button);
        }
    }
    catch (winrt::hresult_error const& e)
    {
        SP_LogDebug(L"The label style could not be applied: 0x%08X", (unsigned)e.code());
    }
    catch (...)
    {
        SP_LogDebug(L"The label style could not be applied");
    }
}

HRESULT WINAPI ITaskbarButton_get_MinScalableWidth_Hook(void* pThis, float* minWidth)
{
    HRESULT hr = g_origGetMinScalableWidth(pThis, minWidth);

    if (FAILED(hr) || !minWidth || g_unloading.load(std::memory_order_relaxed))
    {
        return hr;
    }

    // Only ever lowered: a value above Windows' own floor is what the original calls unsupported, and it would
    // make the taskbar overflow sooner rather than later.
    const int wanted = g_minWidth.load(std::memory_order_relaxed);
    if (wanted > 0 && *minWidth > 0 && (float)wanted < *minWidth)
    {
        *minWidth = (float)wanted;
    }

    return hr;
}

bool WINAPI ITaskbarAppItemViewModel_HasLabel_Hook(void* pThis)
{
    ++t_inHasLabel;
    bool result = g_origHasLabelWrapper(pThis);
    --t_inHasLabel;
    return result;
}

// One window: Windows says yes when not combining. Mode 2 turns that into no.
HRESULT WINAPI TaskListWindowViewModel_get_HasLabel_Hook(void* pThis, bool* hasLabel)
{
    HRESULT hr = g_origWindowGetHasLabel(pThis, hasLabel);

    // The wrapper is inlined on build 26200 (no symbol), so the gate cannot be used there; every caller is
    // answered instead, which is what the simpler label mods do.
    const bool outsideWrapper = (t_inHasLabel == 0 && g_origHasLabelWrapper != nullptr);
    if (FAILED(hr) || !hasLabel || outsideWrapper || g_unloading.load(std::memory_order_relaxed))
    {
        return hr;
    }

    if (g_mode.load(std::memory_order_relaxed) == Mode::Never)
    {
        *hasLabel = false;
    }

    return hr;
}

// A group of windows, or a pinned program: Windows always says no. Mode 0 turns that into yes, mode 2 keeps no.
HRESULT WINAPI TaskListGroupViewModel_get_HasLabel_Hook(void* pThis, bool* hasLabel)
{
    HRESULT hr = g_origGroupGetHasLabel(pThis, hasLabel);

    const bool outsideWrapper = (t_inHasLabel == 0 && g_origHasLabelWrapper != nullptr);
    if (FAILED(hr) || !hasLabel || outsideWrapper || g_unloading.load(std::memory_order_relaxed))
    {
        return hr;
    }

    switch (g_mode.load(std::memory_order_relaxed))
    {
    case Mode::Always:
        *hasLabel = true;
        break;
    case Mode::Never:
        *hasLabel = false;
        break;
    default:
        break;
    }

    return hr;
}

// Build 26200 folds the view models' HasLabel accessors and the consume wrapper into their callers, so none of
// the three above can be hooked there. What is left is the button's own setter, which the binding calls with
// the view model's answer and which stores it and runs UpdateVisualStates: the decision is made on its
// argument. Same rules as above: mode 0 says yes to everything (the pinned-and-not-running label is hidden in
// ApplyLabelStyle), mode 2 says no, mode 1 leaves Windows' answer alone.
// The buttons the setter has been called on: the XAML element (weak, so a recycled button drops out) and the
// implementation pointer the setter takes. The rebuild's notifications arrive asynchronously and in no fixed
// order, so what a button ends up showing is not always what the last setter call said; once the rebuild has
// settled, ReapplyLabels says it again to every live button, on the taskbar's thread.
std::mutex g_buttonsLock;
std::vector<std::pair<winrt::weak_ref<FrameworkElement>, void*>> g_buttons;

void RememberButton(void* pThis)
{
    try
    {
        std::lock_guard<std::mutex> guard(g_buttonsLock);
        for (auto const& entry : g_buttons)
        {
            if (entry.second == pThis)
            {
                return;
            }
        }
        if (auto button = ButtonFromImplementation(pThis))
        {
            g_buttons.emplace_back(winrt::make_weak(button), pThis);
        }
    }
    catch (...)
    {
    }
}

void WINAPI TaskListButton_HasLabel_Hook(void* pThis, bool hasLabel)
{
    if (!g_unloading.load(std::memory_order_relaxed))
    {
        switch (g_mode.load(std::memory_order_relaxed))
        {
        case Mode::Always:
            hasLabel = true;
            break;
        case Mode::Never:
            hasLabel = false;
            break;
        default:
            break;
        }
        RememberButton(pThis);
    }

    g_origTaskListButtonHasLabel(pThis, hasLabel);
}

// Runs on the taskbar's thread: every live button is told the mode's value once more. The setter ignores a
// value it already holds, so it is told the opposite first; both calls run UpdateVisualStates with the flag
// set the way the binding would have set it.
void ReapplyLabelsOnUiThread()
{
    const Mode mode = g_mode.load(std::memory_order_relaxed);
    if (g_unloading.load(std::memory_order_relaxed) || mode == Mode::FollowWindows || !g_origTaskListButtonHasLabel)
    {
        return;
    }
    const bool want = (mode == Mode::Always);

    std::vector<std::pair<FrameworkElement, void*>> live;
    {
        std::lock_guard<std::mutex> guard(g_buttonsLock);
        for (auto it = g_buttons.begin(); it != g_buttons.end();)
        {
            auto element = it->first.get();
            if (!element)
            {
                it = g_buttons.erase(it);
                continue;
            }
            live.emplace_back(element, it->second);
            ++it;
        }
    }

    int applied = 0;
    FrameworkElement repeater{ nullptr };
    for (auto const& entry : live)
    {
        try
        {
            // Only the buttons on the taskbar itself; a button parked by the repeater or in the overflow is
            // left to Windows.
            auto parent = VisualTreeHelper::GetParent(entry.first).try_as<FrameworkElement>();
            if (!parent || parent.Name() != L"TaskbarFrameRepeater")
            {
                continue;
            }
            g_origTaskListButtonHasLabel(entry.second, !want);
            g_origTaskListButtonHasLabel(entry.second, want);

            // The button's panel is already in the labelled layout (98 px) but the button itself may still hold
            // its unlabelled desired size (44 px) from before the rebuild, and the frame lays it out at that
            // width, clipping the label away. A fresh measure puts the two in agreement.
            entry.first.InvalidateMeasure();
            if (!repeater)
            {
                repeater = parent;
            }
            applied++;
        }
        catch (...)
        {
        }
    }
    if (repeater)
    {
        try
        {
            repeater.InvalidateMeasure();
            repeater.InvalidateArrange();
        }
        catch (...)
        {
        }
    }
    SP_LogDebug(L"Labels re-applied to %d button(s) (%zu remembered)", applied, live.size());
}

void ReapplyLabels()
{
    FrameworkElement any{ nullptr };
    {
        std::lock_guard<std::mutex> guard(g_buttonsLock);
        for (auto const& entry : g_buttons)
        {
            any = entry.first.get();
            if (any)
            {
                break;
            }
        }
    }
    if (!any)
    {
        return;
    }
    try
    {
        any.Dispatcher().RunAsync(::winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
                                  ::winrt::Windows::UI::Core::DispatchedHandler([]() { ReapplyLabelsOnUiThread(); }));
    }
    catch (...)
    {
        SP_LogDebug(L"The labels could not be re-applied on the taskbar's thread");
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Making the taskbar rebuild its buttons
// ---------------------------------------------------------------------------------------------------------------

constexpr wchar_t kAdvancedKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";

// The taskbar reads its combining mode through kernelbase's RegGetValueW. While g_flipGrouping is set the
// answer is the opposite of what is stored, so a WM_SETTINGCHANGE looks like the user changed the setting and
// the buttons are rebuilt. Outside that window every read goes through untouched.
LONG WINAPI RegGetValueW_Hook(HKEY hkey, LPCWSTR lpSubKey, LPCWSTR lpValue, DWORD dwFlags,
                              LPDWORD pdwType, PVOID pvData, LPDWORD pcbData)
{
    LONG result = g_origRegGetValueW(hkey, lpSubKey, lpValue, dwFlags, pdwType, pvData, pcbData);

    if (!g_flipGrouping.load(std::memory_order_relaxed))
    {
        return result;
    }

    const bool isGroupingRead =
        hkey == HKEY_CURRENT_USER && lpSubKey && _wcsicmp(lpSubKey, kAdvancedKey) == 0 &&
        lpValue && (_wcsicmp(lpValue, L"TaskbarGlomLevel") == 0 || _wcsicmp(lpValue, L"MMTaskbarGlomLevel") == 0) &&
        (dwFlags & RRF_RT_REG_DWORD) && pvData && pcbData && *pcbData >= sizeof(DWORD);

    if (!isGroupingRead)
    {
        return result;
    }

    // 0 = always combine, 1 = when the taskbar is full, 2 = never. A missing value means 0.
    const DWORD stored = (result == ERROR_SUCCESS) ? *(DWORD*)pvData : 0;
    const DWORD flipped = (stored == 0) ? 2 : 0;

    *(DWORD*)pvData = flipped;
    *pcbData = sizeof(DWORD);
    if (pdwType)
    {
        *pdwType = REG_DWORD;
    }

    SP_LogDebug(L"Answering %s with %u instead of %u", lpValue, flipped, stored);
    return ERROR_SUCCESS;
}

HWND FindTaskbarWindow()
{
    HWND hTaskbar = nullptr;
    EnumWindows(
        [](HWND hWnd, LPARAM lParam) -> BOOL {
            DWORD processId = 0;
            wchar_t className[32];
            if (GetWindowThreadProcessId(hWnd, &processId) && processId == GetCurrentProcessId() &&
                GetClassNameW(hWnd, className, ARRAYSIZE(className)) && _wcsicmp(className, L"Shell_TrayWnd") == 0)
            {
                *reinterpret_cast<HWND*>(lParam) = hWnd;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&hTaskbar));
    return hTaskbar;
}

// Two passes: the first, with the combining mode flipped, makes the taskbar throw its buttons away; the second
// builds them again from the real setting, and every new button goes through the hooks with the current mode.
// The pause between them is the original's; the taskbar needs it to finish the first pass.
void RebuildTaskbarButtons()
{
    if (!g_origRegGetValueW)
    {
        SP_LogDebug(L"No registry hook; the change applies when the taskbar next rebuilds its buttons");
        return;
    }

    HWND hTaskbar = FindTaskbarWindow();
    if (!hTaskbar)
    {
        SP_LogDebug(L"No taskbar window yet; nothing to rebuild");
        return;
    }

    // A timeout rather than a plain send: this runs on the engine thread, and the shell's UI thread must never
    // be able to hold it forever.
    g_flipGrouping.store(true, std::memory_order_relaxed);
    SendMessageTimeoutW(hTaskbar, WM_SETTINGCHANGE, 0, 0, SMTO_NORMAL | SMTO_ABORTIFHUNG, 5000, nullptr);
    g_flipGrouping.store(false, std::memory_order_relaxed);

    Sleep(400);

    SendMessageTimeoutW(hTaskbar, WM_SETTINGCHANGE, 0, 0, SMTO_NORMAL | SMTO_ABORTIFHUNG, 5000, nullptr);

    SP_Log(L"Taskbar buttons rebuilt");

    // The rebuild's view-model notifications keep arriving for a moment; then the mode is said once more to
    // every button that is still there (see ReapplyLabelsOnUiThread).
    Sleep(800);
    ReapplyLabels();
}

// ---------------------------------------------------------------------------------------------------------------
// Installing
// ---------------------------------------------------------------------------------------------------------------

BOOL InstallTaskbarViewHooks(HMODULE hTaskbarView)
{
    static const wchar_t* const kUpdateVisualStates[] =
    {
        LR"(private: void __cdecl winrt::Taskbar::implementation::TaskListButton::UpdateVisualStates(void))",
    };
    static const wchar_t* const kGetIsRunning[] =
    {
        LR"(public: virtual int __cdecl winrt::impl::produce<struct winrt::Taskbar::implementation::TaskListButton,struct winrt::Taskbar::ITaskListButton>::get_IsRunning(bool *))",
    };
    static const wchar_t* const kGetMinScalableWidth[] =
    {
        LR"(public: virtual int __cdecl winrt::impl::produce<struct winrt::Taskbar::implementation::TaskListButton,struct winrt::Taskbar::ITaskbarButton>::get_MinScalableWidth(float *))",
    };
    static const wchar_t* const kHasLabelWrapper[] =
    {
        LR"(public: __cdecl winrt::impl::consume_Taskbar_ITaskbarAppItemViewModel<struct winrt::Taskbar::ITaskbarAppItemViewModel>::HasLabel(void)const )",
    };
    static const wchar_t* const kWindowGetHasLabel[] =
    {
        LR"(public: virtual int __cdecl winrt::impl::produce<struct winrt::Taskbar::implementation::TaskListWindowViewModel,struct winrt::Taskbar::ITaskbarAppItemViewModel>::get_HasLabel(bool *))",
    };
    static const wchar_t* const kGroupGetHasLabel[] =
    {
        LR"(public: virtual int __cdecl winrt::impl::produce<struct winrt::Taskbar::implementation::TaskListGroupViewModel,struct winrt::Taskbar::ITaskbarAppItemViewModel>::get_HasLabel(bool *))",
    };
    // The feature gate Windows' labels implementation shipped behind. Gone on builds where it is simply part of
    // the taskbar, so both entries are optional; the spelling changed with KB5036980.
    static const wchar_t* const kWilFeatureImpl[] =
    {
        LR"(class wil::details::FeatureImpl<struct __WilExternalFeatureTraits_Feature_29785186> `private: static class wil::details::FeatureImpl<struct __WilExternalFeatureTraits_Feature_29785186> & __cdecl wil::Feature<struct __WilExternalFeatureTraits_Feature_29785186>::GetImpl(void)'::`2'::impl)",
        LR"(class wil::details::FeatureImpl<struct __WilFeatureTraits_Feature_29785186> `private: static class wil::details::FeatureImpl<struct __WilFeatureTraits_Feature_29785186> & __cdecl wil::Feature<struct __WilFeatureTraits_Feature_29785186>::GetImpl(void)'::`2'::impl)",
    };
    static const wchar_t* const kWilFeatureIsEnabled[] =
    {
        LR"(public: bool __cdecl wil::details::FeatureImpl<struct __WilExternalFeatureTraits_Feature_29785186>::__private_IsEnabled(enum wil::ReportingKind))",
        LR"(public: bool __cdecl wil::details::FeatureImpl<struct __WilFeatureTraits_Feature_29785186>::__private_IsEnabled(enum wil::ReportingKind))",
    };

    // Build 26200: the one function left that carries the label decision (see TaskListButton_HasLabel_Hook).
    static const wchar_t* const kButtonHasLabel[] =
    {
        LR"(public: void __cdecl winrt::Taskbar::implementation::TaskListButton::HasLabel(bool))",
    };

    // Everything is optional so that one renamed function costs one feature rather than the whole mod; what is
    // missing is reported below.
    SP_SymbolHook hooks[9] = {};

    hooks[8].symbols = kButtonHasLabel;
    hooks[8].symbolCount = ARRAYSIZE(kButtonHasLabel);
    hooks[8].pOriginal = (void**)&g_origTaskListButtonHasLabel;
    hooks[8].hookFunction = (void*)TaskListButton_HasLabel_Hook;
    hooks[8].optional = TRUE;

    hooks[0].symbols = kUpdateVisualStates;
    hooks[0].symbolCount = ARRAYSIZE(kUpdateVisualStates);
    hooks[0].pOriginal = (void**)&g_origUpdateVisualStates;
    hooks[0].hookFunction = (void*)TaskListButton_UpdateVisualStates_Hook;
    hooks[0].optional = TRUE;

    hooks[1].symbols = kGetIsRunning;
    hooks[1].symbolCount = ARRAYSIZE(kGetIsRunning);
    hooks[1].pOriginal = (void**)&g_TaskListButton_get_IsRunning;
    hooks[1].optional = TRUE;

    hooks[2].symbols = kGetMinScalableWidth;
    hooks[2].symbolCount = ARRAYSIZE(kGetMinScalableWidth);
    hooks[2].pOriginal = (void**)&g_origGetMinScalableWidth;
    hooks[2].hookFunction = (void*)ITaskbarButton_get_MinScalableWidth_Hook;
    hooks[2].optional = TRUE;

    hooks[3].symbols = kHasLabelWrapper;
    hooks[3].symbolCount = ARRAYSIZE(kHasLabelWrapper);
    hooks[3].pOriginal = (void**)&g_origHasLabelWrapper;
    hooks[3].hookFunction = (void*)ITaskbarAppItemViewModel_HasLabel_Hook;
    hooks[3].optional = TRUE;

    hooks[4].symbols = kWindowGetHasLabel;
    hooks[4].symbolCount = ARRAYSIZE(kWindowGetHasLabel);
    hooks[4].pOriginal = (void**)&g_origWindowGetHasLabel;
    hooks[4].hookFunction = (void*)TaskListWindowViewModel_get_HasLabel_Hook;
    hooks[4].optional = TRUE;

    hooks[5].symbols = kGroupGetHasLabel;
    hooks[5].symbolCount = ARRAYSIZE(kGroupGetHasLabel);
    hooks[5].pOriginal = (void**)&g_origGroupGetHasLabel;
    hooks[5].hookFunction = (void*)TaskListGroupViewModel_get_HasLabel_Hook;
    hooks[5].optional = TRUE;

    hooks[6].symbols = kWilFeatureImpl;
    hooks[6].symbolCount = ARRAYSIZE(kWilFeatureImpl);
    hooks[6].pOriginal = &g_wilFeatureImpl;
    hooks[6].optional = TRUE;

    hooks[7].symbols = kWilFeatureIsEnabled;
    hooks[7].symbolCount = ARRAYSIZE(kWilFeatureIsEnabled);
    hooks[7].pOriginal = (void**)&g_wilFeatureIsEnabled;
    hooks[7].optional = TRUE;

    if (!SP_HookSymbols(hTaskbarView, hooks, ARRAYSIZE(hooks)))
    {
        SP_LogError(L"The taskbar button functions could not be hooked");
        return FALSE;
    }

    if (!g_origUpdateVisualStates)
    {
        SP_LogError(L"TaskListButton::UpdateVisualStates was not found; label widths are unchanged");
    }
    if (!g_origGetMinScalableWidth)
    {
        SP_LogError(L"get_MinScalableWidth was not found; the minimum width setting does nothing");
    }
    if (g_origTaskListButtonHasLabel)
    {
        SP_Log(L"The mode is applied through TaskListButton::HasLabel(bool) (build 26200 spelling)");
    }
    else if (!g_origWindowGetHasLabel || !g_origGroupGetHasLabel)
    {
        SP_LogError(L"The HasLabel functions were not all found; the mode setting does nothing");
    }
    else if (!g_origHasLabelWrapper)
    {
        SP_Log(L"The HasLabel wrapper is inlined on this build; every HasLabel caller is answered by the mode");
    }
    if (!g_TaskListButton_get_IsRunning)
    {
        SP_Log(L"get_IsRunning was not found; pinned programs without a window keep the taskbar's own label handling");
    }

    return g_origUpdateVisualStates || g_origGetMinScalableWidth || g_origTaskListButtonHasLabel ||
           (g_origHasLabelWrapper && (g_origWindowGetHasLabel || g_origGroupGetHasLabel));
}

BOOL InstallRegistryHook()
{
    // The taskbar reaches the registry through kernelbase, so that is where the hook goes; the advapi32 export
    // is a forwarder most callers never touch.
    if (!SP_HookBegin())
    {
        return FALSE;
    }
    if (!SP_SetExportHook(L"kernelbase.dll", "RegGetValueW", RegGetValueW_Hook, &g_origRegGetValueW))
    {
        SP_HookAbort();
        return FALSE;
    }
    return SP_HookCommit();
}

// Asks the feature gate behind Windows' labels implementation. A build without the gate has the implementation
// as a plain part of the taskbar, so a missing gate counts as enabled. Kept free of C++ objects: the call is
// guarded by SEH, which the compiler refuses in a function that has anything to unwind.
bool LabelsFeatureEnabled()
{
    if (!g_wilFeatureImpl || !g_wilFeatureIsEnabled)
    {
        return true;
    }

    bool enabled = true;
    __try
    {
        enabled = g_wilFeatureIsEnabled(g_wilFeatureImpl, 0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        enabled = true;
    }
    return enabled;
}

// Runs on a helper thread once Taskbar.View.dll is in the process; at once when it already is.
void OnTaskbarViewLoaded(HMODULE hTaskbarView, void*)
{
    if (!hTaskbarView)
    {
        SP_LogError(L"Taskbar.View.dll never loaded; this build's taskbar is not the XAML one");
        return;
    }

    if (!InstallTaskbarViewHooks(hTaskbarView))
    {
        return;
    }

    // The labels implementation shipped behind a feature gate. When the gate is still in this build and shut,
    // the buttons this mod knows how to shape do not exist; the hooks stay in but there is nothing to apply.
    if (!LabelsFeatureEnabled())
    {
        SP_LogError(L"This build's taskbar has no labels implementation; nothing to do");
        return;
    }

    if (!InstallRegistryHook())
    {
        // Not fatal: the hooks above still answer, the change just waits for the taskbar's next rebuild.
        SP_LogError(L"RegGetValueW could not be hooked; settings apply when the taskbar next rebuilds");
    }

    g_hooked.store(true, std::memory_order_relaxed);
    SP_Log(L"Taskbar label hooks are in");

    // Buttons that already exist were laid out without the hooks; rebuild them so the setting shows now. On a
    // cold sign-in there may be no taskbar window yet, in which case every button is still to come.
    RebuildTaskbarButtons();
}

// ---------------------------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------------------------

BOOL Init()
{
    // The DLL stays resident across unload/load: the previous life's unloading flag must not leak into this one
    // (with it set every hook answers as Windows would and nothing is ever labelled).
    g_unloading.store(false, std::memory_order_relaxed);
    g_flipGrouping.store(false, std::memory_order_relaxed);

    LoadSettings();

    // The XAML taskbar is a separate package the shell brings up a moment after the process starts, so on a
    // cold sign-in it is not there yet when this runs. The hooks go in when it appears.
    // On a worker: the callback rebuilds the buttons, which sends to the taskbar and sleeps; not on the
    // taskbar's own thread.
    if (!SP_WaitForModuleOnWorker(L"Taskbar.View.dll", 60000, OnTaskbarViewLoaded, nullptr))
    {
        SP_LogError(L"Could not wait for Taskbar.View.dll");
        return FALSE;
    }

    return TRUE;
}

void SettingsChanged()
{
    LoadSettings();

    if (g_hooked.load(std::memory_order_relaxed))
    {
        RebuildTaskbarButtons();
    }
}

void BeforeUninit()
{
    // With this set the hooks answer exactly as Windows would and the layout pass clears what it set; one more
    // rebuild then leaves the taskbar as it was found. The pause is the original's: buttons of Store apps take
    // a moment to lay out again, and the hooks must still be there while they do.
    g_unloading.store(true, std::memory_order_relaxed);

    if (g_hooked.load(std::memory_order_relaxed))
    {
        RebuildTaskbarButtons();
        Sleep(400);
    }
}

void Uninit()
{
    g_hooked.store(false, std::memory_order_relaxed);
}

}   // namespace

SP_MOD_DEFINE(g_modTaskbarLabels) =
{
    /* id             */ SP_MOD_ID,
    /* name           */ L"Show labels on taskbar buttons",
    /* basedOn        */ "taskbar-labels",
    /* originalAuthor */ "m417z",
    /* targets        */ SP_TARGET_EXPLORER,
    /* minOsBuild     */ 22621,     // Windows' own labels implementation arrived with 22621.2361
    /* flags          */ 0,
    /* Init           */ Init,
    /* AfterInit      */ nullptr,
    /* SettingsChanged*/ SettingsChanged,
    /* BeforeUninit   */ BeforeUninit,
    /* Uninit         */ Uninit,
};
