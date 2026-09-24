//
// taskbar-start-button-position - keeps the Start button at the left edge of a centered Windows 11 taskbar.
//
// Adapted from the idea behind the Windhawk mod "Start button always on the left"
// (taskbar-start-button-position) by m417z. The implementation here is written against this engine's API.
//
// What it does
// -----------
// With "Center" alignment Windows lays every taskbar item out as one centered group: Start, search, task view,
// widgets and the app buttons. This mod takes the Start button out of that group and pins it at X = 0, so the
// app buttons stay centered while Start sits where it did on every earlier Windows. With the second option the
// search and task view buttons follow it, in that order, leaving only the app icons in the middle.
//
// How it works
// ------------
// The items live in an ItemsRepeater named TaskbarFrameRepeater whose layout is Taskbar.TaskbarCollapsibleLayout
// (Taskbar.View.dll). Two things are done to it:
//
//   1. A pinned button gets a negative right margin equal to its own width, which makes the layout treat it as
//      zero-width: the centered group is measured as if the button were not there and no longer leaves a gap.
//      This is what ApplyStyle does, and it is redone on every settings change and undone on unload.
//   2. During the layout's ArrangeOverride, the Arrange call XAML makes for each child is intercepted and the
//      rectangle of a pinned button is moved to the left edge. The interception is a hook on the one
//      IUIElement::Arrange implementation in Windows.UI.Xaml.dll, gated by a thread-local flag so it only acts
//      inside that layout pass.
//
// The widgets button is already left-pinned by Windows, at the exact place the Start button now occupies, so
// its left margin is nudged right by the width of the pinned cluster. When there is no widgets button the pinned
// buttons keep an eye on the centered group themselves: a button expands back to full width when the group would
// otherwise run into it and collapses again once there is room, throttled so it cannot oscillate.
//
// Two smaller things come with it, both behind "StartMenuOnLeft":
//   * the Start button's own context menu (Win+X) is aligned to the button's leading edge, by answering "Left"
//     to TaskbarFrame::Alignment while that menu is being built;
//   * the search flyout (SearchHost.exe) is moved to the left edge of the work area when it opens while the
//     Start menu is showing. Explorer cloaks and uncloaks that window through DwmSetWindowAttribute, which is
//     where the move happens; the original position is put back when it is cloaked again.
//
// Where the hooks are
// -------------------
//   Taskbar.View.dll  TaskbarCollapsibleLayout::ArrangeOverride    marks the layout pass, installs the Arrange hook
//                     ExperienceToggleButton::UpdateButtonPadding  restores the Start button's left padding
//                     TaskbarFrame::get_Alignment                  Win+X menu alignment
//                     ShowStartButtonContextMenuAsync (resume)     brackets the alignment override
//   Windows.UI.Xaml   IUIElement::Arrange (vtable slot)            moves the pinned buttons
//   dwmapi.dll        DwmSetWindowAttribute (export)               search flyout position
//   taskbar.dll       CTaskBand::GetTaskbarHost and friends        resolved only, to find the taskbar's XAML from
//                                                                  its window when settings change
//
// Taskbar.View.dll is loaded after the engine on a cold sign-in, so the hooks go in from the SP_WaitForModule
// callback. Every repeater the Arrange hook meets is remembered, so a taskbar that came up before the callback
// managed to reach it gets its style at its first layout pass, and a settings change can still reach it even if
// the taskbar.dll symbols are not there on some build.
//
// Exceptions
// ----------
// C++/WinRT reports failures by throwing, so this file is compiled with exceptions on while the rest of the
// engine is not. Every hook body and every dispatched callback is wrapped: an exception reaching the shell's UI
// thread would end the process.
//
#define SP_MOD_ID "taskbar-start-button-position"
#include "engine/modapi.h"

#include <dwmapi.h>

#include <atomic>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#undef GetCurrentTime

#include <unknwn.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>

namespace {

using namespace winrt::Windows::UI::Xaml;
using winrt::Windows::UI::Core::CoreDispatcher;
using winrt::Windows::UI::Core::CoreDispatcherPriority;

// ---------------------------------------------------------------------------------------------------------------
// State shared between the engine thread and the taskbar thread
// ---------------------------------------------------------------------------------------------------------------

// Settings, written on the engine thread and read inside the hooks.
std::atomic<bool> g_startMenuOnLeft{ true };
std::atomic<bool> g_searchAndTaskViewOnLeft{ false };

// Set in BeforeUninit: from then on every hook passes through and ApplyStyle restores instead of applies.
std::atomic<bool> g_unloading{ false };

// The Taskbar.View.dll hooks are in place.
std::atomic<bool> g_viewHooked{ false };

// The IUIElement::Arrange hook can only be installed from a XAML thread (a XAML object has to be created to
// read its vtable), so it goes in the first time the layout hook runs. Reset in Init so that a mod turned off
// and on again hooks afresh.
std::atomic<bool> g_arrangeHookTried{ false };

// Callbacks handed to the taskbar's dispatcher that have not run yet. Uninit waits for them (briefly) so that
// nothing runs into this code after the mod is gone.
std::atomic<int> g_pendingDispatches{ 0 };

// True while inside TaskbarCollapsibleLayout::ArrangeOverride on this thread; the Arrange hook acts only then.
thread_local bool t_inCollapsibleLayoutArrange = false;

// True while the Start button's context menu is being built on this thread.
thread_local bool t_inStartButtonContextMenu = false;

// ---------------------------------------------------------------------------------------------------------------
// Visual tree helpers
// ---------------------------------------------------------------------------------------------------------------

template <typename Pred>
FrameworkElement FirstChild(FrameworkElement const& element, Pred&& pred)
{
    const int count = Media::VisualTreeHelper::GetChildrenCount(element);
    for (int i = 0; i < count; ++i)
    {
        auto child = Media::VisualTreeHelper::GetChild(element, i).try_as<FrameworkElement>();
        if (child && pred(child))
        {
            return child;
        }
    }
    return nullptr;
}

FrameworkElement FindChildByName(FrameworkElement const& element, const wchar_t* name)
{
    return FirstChild(element, [name](FrameworkElement const& child) { return child.Name() == name; });
}

FrameworkElement FindChildByClassName(FrameworkElement const& element, const wchar_t* className)
{
    return FirstChild(element, [className](FrameworkElement const& child) {
        return winrt::get_class_name(child) == className;
    });
}

// The repeater is a WinUI 2 ItemsRepeater. An item it has recycled stays in the visual tree, parked at
// (-10000, -10000) with no size, and would add a phantom width to every sum below; so the walks over the
// repeater's children skip anything parked there.
bool IsParkedByRepeater(FrameworkElement const& child)
{
    return child.ActualOffset().x <= -9000.0f;
}

// Calls `visit` for every live item of the repeater; stops and returns the item when `visit` returns true.
template <typename Visit>
FrameworkElement ForEachLiveItem(FrameworkElement const& repeater, Visit&& visit)
{
    const int count = Media::VisualTreeHelper::GetChildrenCount(repeater);
    for (int i = 0; i < count; ++i)
    {
        auto child = Media::VisualTreeHelper::GetChild(repeater, i).try_as<FrameworkElement>();
        if (!child || IsParkedByRepeater(child))
        {
            continue;
        }
        if (visit(child))
        {
            return child;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------------------------------
// The system buttons
// ---------------------------------------------------------------------------------------------------------------

enum class SystemButton
{
    None,
    Start,
    Widgets,
    Search,
    TaskView,
    Count,
};

// Left-to-right order of the pinned cluster: Start, search, task view, then widgets. -1 for anything else.
int ClusterRank(SystemButton button)
{
    switch (button)
    {
        case SystemButton::Start:    return 0;
        case SystemButton::Search:   return 1;
        case SystemButton::TaskView: return 2;
        case SystemButton::Widgets:  return 3;
        default:                     return -1;
    }
}

SystemButton IdentifySystemButton(FrameworkElement const& element)
{
    auto className = winrt::get_class_name(element);

    if (className == L"Taskbar.ExperienceToggleButton")
    {
        auto automationId = Automation::AutomationProperties::GetAutomationId(element);
        if (automationId == L"StartButton")
        {
            return SystemButton::Start;
        }
        if (automationId == L"TaskViewButton")
        {
            return SystemButton::TaskView;
        }
    }
    else if (className == L"Taskbar.AugmentedEntryPointButton")
    {
        if (element.Name() == L"AugmentedEntryPointButton")
        {
            return SystemButton::Widgets;
        }
    }
    else if (className == L"Taskbar.TaskbarExtensionElement")
    {
        return SystemButton::Search;
    }

    return SystemButton::None;
}

// The Start button always; search and task view when the option is on. Widgets is handled on its own.
bool IsPinnedClusterButton(SystemButton button)
{
    if (button == SystemButton::Start)
    {
        return true;
    }
    return g_searchAndTaskViewOnLeft.load(std::memory_order_relaxed) &&
           (button == SystemButton::Search || button == SystemButton::TaskView);
}

// The width a cluster button takes when it is not collapsed. ActualWidth cannot be used: it includes the
// collapse margin, so it would shrink on every pass. The content child's DesiredSize does not depend on the
// button's own margin.
double ClusterButtonWidth(FrameworkElement const& element)
{
    if (Media::VisualTreeHelper::GetChildrenCount(element) > 0)
    {
        if (auto child = Media::VisualTreeHelper::GetChild(element, 0).try_as<FrameworkElement>())
        {
            return child.DesiredSize().Width;
        }
    }
    return element.ActualWidth();
}

// Where a pinned button goes: the summed widths of the cluster buttons ranked before it, so the result does not
// depend on the order the layout arranges its children in.
double PinnedButtonX(FrameworkElement const& repeater, SystemButton target)
{
    const int targetRank = ClusterRank(target);
    if (targetRank <= 0)
    {
        return 0;
    }

    double x = 0;
    ForEachLiveItem(repeater, [&x, targetRank](FrameworkElement const& child) {
        const int rank = ClusterRank(IdentifySystemButton(child));
        if (rank >= 0 && rank < targetRank)
        {
            x += ClusterButtonWidth(child);
        }
        return false;
    });
    return x;
}

// The widgets button at its Windows-given left-pinned position: its offset equals its left margin. That is the
// state in which it anchors the centered group away from the pinned cluster.
FrameworkElement FindLeftPinnedWidgetsButton(FrameworkElement const& repeater)
{
    return ForEachLiveItem(repeater, [](FrameworkElement const& child) {
        if (IdentifySystemButton(child) != SystemButton::Widgets)
        {
            return false;
        }
        auto margin = child.Margin();
        auto offset = child.ActualOffset();
        return offset.x == margin.Left && offset.y == 0;
    });
}

// ---------------------------------------------------------------------------------------------------------------
// Margin updates, run on the taskbar thread outside the layout pass
// ---------------------------------------------------------------------------------------------------------------

// Last tick at which each pinned button collapsed; collapses are throttled (see below). Taskbar thread only.
ULONGLONG g_lastCollapseTick[static_cast<size_t>(SystemButton::Count)] = {};

// Without a widgets button to anchor it, a pinned button keeps itself clear of the centered group: it expands
// to reserve its width when the group would run into it and collapses out of the group once there is room.
void UpdatePinnedButtonMargin(FrameworkElement const& element)
{
    const SystemButton self = IdentifySystemButton(element);
    if (g_unloading.load(std::memory_order_relaxed) || !IsPinnedClusterButton(self))
    {
        // Unloading, or the option was turned off after this was scheduled; ApplyStyle restores the margins.
        return;
    }

    auto repeater = Media::VisualTreeHelper::GetParent(element).try_as<FrameworkElement>();
    if (!repeater)
    {
        return;
    }

    double pinnedWidth = 0;
    double centeredLeftX = std::numeric_limits<double>::infinity();
    ForEachLiveItem(repeater, [&](FrameworkElement const& child) {
        const SystemButton button = IdentifySystemButton(child);
        if (IsPinnedClusterButton(button))
        {
            pinnedWidth += ClusterButtonWidth(child);
        }
        else if (button != SystemButton::Widgets)
        {
            auto offset = child.ActualOffset();
            if (offset.x >= 0 && offset.x < centeredLeftX)
            {
                centeredLeftX = offset.x;
            }
        }
        return false;
    });

    Thickness margin = element.Margin();

    double newRight;
    if (centeredLeftX < pinnedWidth)
    {
        newRight = 0;                              // expand: reserve this button's width
    }
    else if (margin.Right != 0 || centeredLeftX > pinnedWidth + 44)
    {
        newRight = -ClusterButtonWidth(element);   // collapse out of the group
    }
    else
    {
        return;                                    // already collapsed and not crowded
    }

    if (margin.Right == newRight)
    {
        return;
    }

    if (newRight < margin.Right)
    {
        // Collapsing shifts the centered group back, which can immediately make expanding look right again.
        // At most one collapse a second per button lets it settle expanded instead of flickering.
        const ULONGLONG now = GetTickCount64();
        ULONGLONG& last = g_lastCollapseTick[static_cast<size_t>(self)];
        if (now - last < 1000)
        {
            return;
        }
        last = now;
    }

    margin.Right = newRight;
    element.Margin(margin);
}

// Windows pins the widgets button at the far left, exactly where the Start button now is, so it is moved right
// by the width of the pinned cluster.
void UpdateWidgetsLeftMargin(FrameworkElement const& element)
{
    if (g_unloading.load(std::memory_order_relaxed))
    {
        return;     // ApplyStyle restores the margin on unload; do not fight it
    }

    auto repeater = Media::VisualTreeHelper::GetParent(element).try_as<FrameworkElement>();
    if (!repeater)
    {
        return;
    }

    const double left = g_searchAndTaskViewOnLeft.load(std::memory_order_relaxed)
                            ? PinnedButtonX(repeater, SystemButton::Widgets)
                            : 44;

    Thickness margin = element.Margin();
    if (margin.Left != left)
    {
        margin.Left = left;
        element.Margin(margin);
    }
}

// Runs `func` on the element's thread after the current layout pass: changing a margin during arrange would
// re-enter the layout.
void ScheduleOnTaskbarThread(FrameworkElement const& element, void (*func)(FrameworkElement const&))
{
    g_pendingDispatches.fetch_add(1, std::memory_order_relaxed);
    try
    {
        element.Dispatcher().TryRunAsync(CoreDispatcherPriority::High, [element, func]() {
            try
            {
                func(element);
            }
            catch (...)
            {
            }
            g_pendingDispatches.fetch_sub(1, std::memory_order_relaxed);
        });
    }
    catch (...)
    {
        g_pendingDispatches.fetch_sub(1, std::memory_order_relaxed);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Remembering the repeaters
//
// A settings change or the unload has to reach every taskbar's repeater from the engine thread. The taskbar.dll
// route below finds it from the taskbar window; as a second route, every repeater the Arrange hook meets is
// kept here with its dispatcher (the one member of a XAML object that may be read from any thread).
// ---------------------------------------------------------------------------------------------------------------

struct KnownRepeater
{
    winrt::weak_ref<FrameworkElement> element;
    CoreDispatcher                    dispatcher{ nullptr };
};

std::mutex                 g_knownLock;
std::vector<KnownRepeater> g_known;

// Returns true when this repeater had not been seen before. Taskbar thread.
bool RememberRepeater(FrameworkElement const& repeater)
{
    std::lock_guard<std::mutex> lock(g_knownLock);
    for (size_t i = 0; i < g_known.size();)
    {
        auto existing = g_known[i].element.get();
        if (!existing)
        {
            g_known.erase(g_known.begin() + i);
            continue;
        }
        if (existing == repeater)
        {
            return false;
        }
        ++i;
    }

    KnownRepeater known;
    known.element = winrt::make_weak(repeater);
    known.dispatcher = repeater.Dispatcher();
    g_known.push_back(known);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Applying (or restoring) the style
// ---------------------------------------------------------------------------------------------------------------

// Taskbar thread. Collapses the pinned buttons out of the centered group and places the widgets button after
// them; when unloading, puts every margin back.
void ApplyStyle(FrameworkElement const& repeater)
{
    const bool unloading = g_unloading.load(std::memory_order_relaxed);
    const bool others = g_searchAndTaskViewOnLeft.load(std::memory_order_relaxed);

    RememberRepeater(repeater);

    if (auto widgets = FindLeftPinnedWidgetsButton(repeater))
    {
        Thickness margin = widgets.Margin();
        if (unloading)
        {
            margin.Left = 0;
        }
        else if (others)
        {
            margin.Left = PinnedButtonX(repeater, SystemButton::Widgets);
        }
        else
        {
            margin.Left = 44;
        }
        widgets.Margin(margin);
    }

    ForEachLiveItem(repeater, [unloading](FrameworkElement const& child) {
        const SystemButton button = IdentifySystemButton(child);
        switch (button)
        {
            case SystemButton::Start:
            case SystemButton::Search:
            case SystemButton::TaskView:
            {
                Thickness margin = child.Margin();
                const double width = ClusterButtonWidth(child);
                if (IsPinnedClusterButton(button) && !unloading)
                {
                    margin.Right = -width;
                }
                else if (margin.Right < 0)
                {
                    margin.Right = 0;   // undo only a collapse this mod applied
                }
                else
                {
                    break;
                }
                SP_LogDebug(L"System button %d: width=%.1f, margin.Right=%.1f", (int)button, width, margin.Right);
                child.Margin(margin);
                break;
            }
            default:
                break;
        }
        return false;
    });
}

// Taskbar thread. Finds the repeater under a taskbar's XAML root and styles it.
bool ApplyStyleFromRoot(XamlRoot const& root)
{
    if (!root)
    {
        return false;
    }

    FrameworkElement element = root.Content().try_as<FrameworkElement>();
    if (element &&
        (element = FindChildByClassName(element, L"Taskbar.TaskbarFrame")) &&
        (element = FindChildByName(element, L"RootGrid")) &&
        (element = FindChildByName(element, L"TaskbarFrameRepeater")))
    {
        ApplyStyle(element);
        return true;
    }

    SP_LogDebug(L"TaskbarFrameRepeater not found under this root");
    return false;
}

// ---------------------------------------------------------------------------------------------------------------
// From a taskbar window to its XAML
//
// The tray window keeps its task band (CTaskBand, in taskbar.dll) in the window bytes of the TaskbandHWND child;
// the band's ITaskListWndSite interface hands out a shared_ptr<TaskbarHost>, and the host holds the XAML frame
// element a few bytes in. That offset is read from TaskbarHost::FrameHeight, which starts by adding it to `this`.
// Every symbol is resolved only, none is hooked. Missing symbols cost the on-demand route, not the mod.
// ---------------------------------------------------------------------------------------------------------------

using GetTaskbarHost_t = void*(WINAPI*)(void* pThis, void** result);
using RefCountDecref_t = void(WINAPI*)(void* pThis);

void*            g_CTaskBand_ITaskListWndSite_vftable = nullptr;
void*            g_CSecondaryTaskBand_ITaskListWndSite_vftable = nullptr;
GetTaskbarHost_t g_CTaskBand_GetTaskbarHost = nullptr;
GetTaskbarHost_t g_CSecondaryTaskBand_GetTaskbarHost = nullptr;
void*            g_TaskbarHost_FrameHeight = nullptr;
RefCountDecref_t g_RefCountBase_Decref = nullptr;

std::atomic<bool> g_taskbarDllReady{ false };

BOOL ResolveTaskbarDllSymbols()
{
    HMODULE module = LoadLibraryExW(L"taskbar.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module)
    {
        SP_LogError(L"taskbar.dll could not be loaded");
        return FALSE;
    }

    static const wchar_t* const kTaskBandVftable[] = {
        LR"(const CTaskBand::`vftable'{for `ITaskListWndSite'})",
    };
    static const wchar_t* const kSecondaryTaskBandVftable[] = {
        LR"(const CSecondaryTaskBand::`vftable'{for `ITaskListWndSite'})",
    };
    static const wchar_t* const kTaskBandGetTaskbarHost[] = {
        LR"(public: virtual class std::shared_ptr<class TaskbarHost> __cdecl CTaskBand::GetTaskbarHost(void)const )",
    };
    static const wchar_t* const kTaskbarHostFrameHeight[] = {
        LR"(public: int __cdecl TaskbarHost::FrameHeight(void)const )",
    };
    static const wchar_t* const kSecondaryTaskBandGetTaskbarHost[] = {
        LR"(public: virtual class std::shared_ptr<class TaskbarHost> __cdecl CSecondaryTaskBand::GetTaskbarHost(void)const )",
    };
    static const wchar_t* const kRefCountBaseDecref[] = {
        LR"(public: void __cdecl std::_Ref_count_base::_Decref(void))",
    };

    SP_SymbolHook hooks[6] = {};
    hooks[0].symbols = kTaskBandVftable;
    hooks[0].symbolCount = ARRAYSIZE(kTaskBandVftable);
    hooks[0].pOriginal = &g_CTaskBand_ITaskListWndSite_vftable;
    hooks[1].symbols = kSecondaryTaskBandVftable;
    hooks[1].symbolCount = ARRAYSIZE(kSecondaryTaskBandVftable);
    hooks[1].pOriginal = &g_CSecondaryTaskBand_ITaskListWndSite_vftable;
    hooks[2].symbols = kTaskBandGetTaskbarHost;
    hooks[2].symbolCount = ARRAYSIZE(kTaskBandGetTaskbarHost);
    hooks[2].pOriginal = (void**)&g_CTaskBand_GetTaskbarHost;
    hooks[3].symbols = kTaskbarHostFrameHeight;
    hooks[3].symbolCount = ARRAYSIZE(kTaskbarHostFrameHeight);
    hooks[3].pOriginal = &g_TaskbarHost_FrameHeight;
    hooks[4].symbols = kSecondaryTaskBandGetTaskbarHost;
    hooks[4].symbolCount = ARRAYSIZE(kSecondaryTaskBandGetTaskbarHost);
    hooks[4].pOriginal = (void**)&g_CSecondaryTaskBand_GetTaskbarHost;
    hooks[5].symbols = kRefCountBaseDecref;
    hooks[5].symbolCount = ARRAYSIZE(kRefCountBaseDecref);
    hooks[5].pOriginal = (void**)&g_RefCountBase_Decref;
    for (auto& hook : hooks)
    {
        hook.hookFunction = nullptr;    // resolve only
        hook.optional = FALSE;
    }

    if (!SP_ResolveSymbols(module, hooks, ARRAYSIZE(hooks)))
    {
        SP_LogError(L"The taskbar.dll symbols were not all found; settings changes apply at the next layout pass");
        return FALSE;
    }

    return TRUE;
}

// The frame element held by a shared_ptr<TaskbarHost> {object, control block}. The control block's reference
// is released here, which is what the shared_ptr's destructor would have done.
FrameworkElement TaskbarFrameFromHost(void* hostSharedPtr[2])
{
    if (!hostSharedPtr[0] && !hostSharedPtr[1])
    {
        return nullptr;
    }

    size_t frameOffset = 0x10;
#if defined(_M_X64)
    {
        // 48:83EC 28 | sub rsp,28
        // 48:83C1 48 | add rcx,48
        const BYTE* b = (const BYTE*)g_TaskbarHost_FrameHeight;
        if (b[0] == 0x48 && b[1] == 0x83 && b[2] == 0xEC && b[4] == 0x48 && b[5] == 0x83 && b[6] == 0xC1 &&
            b[7] <= 0x7F)
        {
            frameOffset = b[7];
        }
        else
        {
            SP_LogDebug(L"TaskbarHost::FrameHeight has an unfamiliar prologue; using offset 0x10");
        }
    }
#else
#error "Only x64 is supported"
#endif

    FrameworkElement frame{ nullptr };
    if (hostSharedPtr[0])
    {
        auto* unknown = *(::IUnknown**)((BYTE*)hostSharedPtr[0] + frameOffset);
        if (unknown)
        {
            unknown->QueryInterface(winrt::guid_of<FrameworkElement>(), winrt::put_abi(frame));
        }
    }

    if (hostSharedPtr[1])
    {
        g_RefCountBase_Decref(hostSharedPtr[1]);
    }

    return frame;
}

// The sub-object of the band that implements ITaskListWndSite: the first pointer slot holding that vftable.
void* FindTaskListWndSite(void* taskBand, void* vftable)
{
    if (!taskBand || !vftable)
    {
        return nullptr;
    }
    void** slot = (void**)taskBand;
    for (int i = 0; i < 20; ++i, ++slot)
    {
        if (*slot == vftable)
        {
            return slot;
        }
    }
    return nullptr;
}

FrameworkElement TaskbarFrameOfPrimary(HWND hTaskbarWnd)
{
    HWND hTaskSwWnd = (HWND)GetPropW(hTaskbarWnd, L"TaskbandHWND");
    if (!hTaskSwWnd)
    {
        return nullptr;
    }

    void* taskBand = (void*)GetWindowLongPtrW(hTaskSwWnd, 0);
    void* site = FindTaskListWndSite(taskBand, g_CTaskBand_ITaskListWndSite_vftable);
    if (!site)
    {
        return nullptr;
    }

    void* host[2] = {};
    g_CTaskBand_GetTaskbarHost(site, host);
    return TaskbarFrameFromHost(host);
}

FrameworkElement TaskbarFrameOfSecondary(HWND hSecondaryTaskbarWnd)
{
    HWND hTaskSwWnd = FindWindowExW(hSecondaryTaskbarWnd, nullptr, L"WorkerW", nullptr);
    if (!hTaskSwWnd)
    {
        return nullptr;
    }

    void* taskBand = (void*)GetWindowLongPtrW(hTaskSwWnd, 0);
    void* site = FindTaskListWndSite(taskBand, g_CSecondaryTaskBand_ITaskListWndSite_vftable);
    if (!site)
    {
        return nullptr;
    }

    void* host[2] = {};
    g_CSecondaryTaskBand_GetTaskbarHost(site, host);
    return TaskbarFrameFromHost(host);
}

// ---------------------------------------------------------------------------------------------------------------
// Applying from the engine thread
// ---------------------------------------------------------------------------------------------------------------

// One taskbar to style: its dispatcher, and either the frame element (taskbar.dll route) or the repeater
// (remembered route).
struct ApplyTarget
{
    CoreDispatcher                    dispatcher{ nullptr };
    FrameworkElement                  frame{ nullptr };
    winrt::weak_ref<FrameworkElement> repeater;
};

void CollectTargetsFromTaskbarWindows(std::vector<ApplyTarget>& targets)
{
    EnumWindows(
        [](HWND hWnd, LPARAM lParam) -> BOOL {
            auto* out = (std::vector<ApplyTarget>*)lParam;

            DWORD processId = 0;
            if (!GetWindowThreadProcessId(hWnd, &processId) || processId != GetCurrentProcessId())
            {
                return TRUE;
            }

            wchar_t className[64];
            if (!GetClassNameW(hWnd, className, ARRAYSIZE(className)))
            {
                return TRUE;
            }

            try
            {
                FrameworkElement frame{ nullptr };
                if (_wcsicmp(className, L"Shell_TrayWnd") == 0)
                {
                    frame = TaskbarFrameOfPrimary(hWnd);
                }
                else if (_wcsicmp(className, L"Shell_SecondaryTrayWnd") == 0)
                {
                    frame = TaskbarFrameOfSecondary(hWnd);
                }
                else
                {
                    return TRUE;
                }

                if (frame)
                {
                    ApplyTarget target;
                    target.dispatcher = frame.Dispatcher();
                    target.frame = frame;
                    out->push_back(target);
                }
                else
                {
                    SP_LogDebug(L"No XAML frame behind taskbar window %p", hWnd);
                }
            }
            catch (...)
            {
                SP_LogDebug(L"Reading the XAML frame of taskbar window %p failed", hWnd);
            }
            return TRUE;
        },
        (LPARAM)&targets);
}

// An event that outlives whichever side finishes last, so a callback that runs after the wait timed out can
// still signal safely.
struct Waiter
{
    HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~Waiter()
    {
        if (hEvent)
        {
            CloseHandle(hEvent);
        }
    }
};

// Runs ApplyStyle for one taskbar on its own thread and waits for it, briefly: BeforeUninit needs the restore
// done before the hooks go, and a UI thread that does not answer must not hold the engine.
void ApplyOnTarget(ApplyTarget const& target)
{
    auto waiter = std::make_shared<Waiter>();
    if (!waiter->hEvent)
    {
        return;
    }

    g_pendingDispatches.fetch_add(1, std::memory_order_relaxed);
    try
    {
        target.dispatcher.TryRunAsync(CoreDispatcherPriority::High, [target, waiter]() {
            try
            {
                if (target.frame)
                {
                    ApplyStyleFromRoot(target.frame.XamlRoot());
                }
                else if (auto repeater = target.repeater.get())
                {
                    ApplyStyle(repeater);
                }
            }
            catch (winrt::hresult_error const& e)
            {
                SP_LogError(L"Applying the taskbar style failed: 0x%08X", (unsigned)e.code());
            }
            catch (...)
            {
                SP_LogError(L"Applying the taskbar style failed");
            }
            g_pendingDispatches.fetch_sub(1, std::memory_order_relaxed);
            SetEvent(waiter->hEvent);
        });
    }
    catch (...)
    {
        g_pendingDispatches.fetch_sub(1, std::memory_order_relaxed);
        SP_LogError(L"The taskbar's dispatcher refused the style callback");
        return;
    }

    WaitForSingleObject(waiter->hEvent, 3000);
}

// Engine thread (or the module-wait thread). Styles every taskbar that can be reached right now.
void ApplySettings()
{
    std::vector<ApplyTarget> targets;

    if (g_taskbarDllReady.load(std::memory_order_relaxed))
    {
        CollectTargetsFromTaskbarWindows(targets);
    }

    if (targets.empty())
    {
        std::lock_guard<std::mutex> lock(g_knownLock);
        for (auto const& known : g_known)
        {
            ApplyTarget target;
            target.dispatcher = known.dispatcher;
            target.repeater = known.element;
            targets.push_back(target);
        }
    }

    if (targets.empty())
    {
        SP_LogDebug(L"No taskbar to style yet; the first layout pass will do it");
        return;
    }

    SP_Log(L"%s the style on %u taskbar(s)",
           g_unloading.load(std::memory_order_relaxed) ? L"Restoring" : L"Applying", (unsigned)targets.size());
    for (auto const& target : targets)
    {
        ApplyOnTarget(target);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Hook: IUIElement::Arrange, active only inside TaskbarCollapsibleLayout::ArrangeOverride
// ---------------------------------------------------------------------------------------------------------------

using IUIElement_Arrange_t = HRESULT(WINAPI*)(void* pThis, winrt::Windows::Foundation::Rect rect);
IUIElement_Arrange_t g_origArrange = nullptr;

HRESULT WINAPI IUIElement_Arrange_Hook(void* pThis, winrt::Windows::Foundation::Rect rect)
{
    if (!t_inCollapsibleLayoutArrange || g_unloading.load(std::memory_order_relaxed))
    {
        return g_origArrange(pThis, rect);
    }

    try
    {
        FrameworkElement element{ nullptr };
        ((::IUnknown*)pThis)->QueryInterface(winrt::guid_of<FrameworkElement>(), winrt::put_abi(element));
        if (!element)
        {
            return g_origArrange(pThis, rect);
        }

        const SystemButton button = IdentifySystemButton(element);

        // The widgets button needs its nudge whether or not the second option is on.
        if (button == SystemButton::Widgets)
        {
            ScheduleOnTaskbarThread(element, UpdateWidgetsLeftMargin);
            return g_origArrange(pThis, rect);
        }

        if (!IsPinnedClusterButton(button))
        {
            return g_origArrange(pThis, rect);
        }

        auto repeater = Media::VisualTreeHelper::GetParent(element).try_as<FrameworkElement>();
        if (!repeater)
        {
            return g_origArrange(pThis, rect);
        }

        // A repeater seen for the first time has not had ApplyStyle yet: the taskbar came up after the hooks
        // did, on a cold sign-in. Give it the style once this pass is over.
        if (RememberRepeater(repeater))
        {
            SP_Log(L"Taskbar repeater met for the first time; styling it");
            ScheduleOnTaskbarThread(repeater, ApplyStyle);
        }

        // With the widgets button anchoring the centered group nothing else is needed; without it the button
        // looks after its own margin.
        if (!FindLeftPinnedWidgetsButton(repeater))
        {
            ScheduleOnTaskbarThread(element, UpdatePinnedButtonMargin);
        }

        winrt::Windows::Foundation::Rect pinned = rect;
        pinned.X = (float)PinnedButtonX(repeater, button);
        return g_origArrange(pThis, pinned);
    }
    catch (...)
    {
        return g_origArrange(pThis, rect);
    }
}

// The slot of Arrange in the IUIElement vtable: the 6 IInspectable methods, then the interface's members in
// declaration order (Windows.UI.Xaml.h, ABI): 35 property accessors, 25 events with add/remove, Measure, then
// Arrange at 6 + 35 + 50 + 1 = 92.
constexpr size_t kIUIElementArrangeSlot = 92;

// Installs the Arrange hook. Must run on a XAML thread: a Rectangle is created to read the vtable, and the
// same Windows.UI.Xaml.dll function serves every element in the process.
void EnsureArrangeHook()
{
    if (g_arrangeHookTried.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }

    try
    {
        Shapes::Rectangle rectangle;
        IUIElement element = rectangle.as<IUIElement>();
        void** vtable = *(void***)winrt::get_abi(element);
        void* arrange = vtable[kIUIElementArrangeSlot];

        if (SP_SetFunctionHookNow(arrange, IUIElement_Arrange_Hook, &g_origArrange))
        {
            SP_Log(L"IUIElement::Arrange hooked at %p", arrange);
        }
        else
        {
            SP_LogError(L"IUIElement::Arrange could not be hooked; the Start button stays where Windows puts it");
        }
    }
    catch (...)
    {
        SP_LogError(L"Reading the IUIElement vtable failed");
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Hook: TaskbarCollapsibleLayout::ArrangeOverride
// ---------------------------------------------------------------------------------------------------------------

using ArrangeOverride_t = HRESULT(WINAPI*)(void* pThis, void* context, winrt::Windows::Foundation::Size size,
                                           winrt::Windows::Foundation::Size* resultSize);
ArrangeOverride_t g_origArrangeOverride = nullptr;

HRESULT WINAPI TaskbarCollapsibleLayout_ArrangeOverride_Hook(void* pThis, void* context,
                                                             winrt::Windows::Foundation::Size size,
                                                             winrt::Windows::Foundation::Size* resultSize)
{
    if (!g_unloading.load(std::memory_order_relaxed))
    {
        EnsureArrangeHook();
    }

    const bool previous = t_inCollapsibleLayoutArrange;
    t_inCollapsibleLayoutArrange = (g_origArrange != nullptr);
    HRESULT hr = g_origArrangeOverride(pThis, context, size, resultSize);
    t_inCollapsibleLayoutArrange = previous;
    return hr;
}

// Build 26200: the layout is TaskbarCollapsibleLayoutBase<TaskbarCollapsibleLayoutXamlTraits> and its own
// ArrangeOverride member is what runs (the produce thunk above is never reached). Same bracket; the arguments
// are carried opaquely, as its prologue takes them: rcx this, rdx the hidden pointer for the returned Size,
// r8 the context reference, r9 the Size value (movq xmm13, r9); rax is the hidden pointer again.
using ArrangeOverrideBase_t = void*(WINAPI*)(void* pThis, void* result, void* context, unsigned long long size);
ArrangeOverrideBase_t g_origArrangeOverrideBase = nullptr;

void* WINAPI TaskbarCollapsibleLayoutBase_ArrangeOverride_Hook(void* pThis, void* result, void* context,
                                                                unsigned long long size)
{
    if (!g_unloading.load(std::memory_order_relaxed))
    {
        EnsureArrangeHook();
    }

    const bool previous = t_inCollapsibleLayoutArrange;
    t_inCollapsibleLayoutArrange = (g_origArrange != nullptr);
    void* ret = g_origArrangeOverrideBase(pThis, result, context, size);
    t_inCollapsibleLayoutArrange = previous;
    return ret;
}

// ---------------------------------------------------------------------------------------------------------------
// Hook: TaskListButton::UpdateVisualStates (optional, build 26200)
//
// A taskbar that is already up when the mod is turned on may not lay itself out again for a long time, and
// without the taskbar.dll symbols nothing can be reached directly. The "ImmersiveColorSet" setting change the
// view callback sends makes every button run this; while no repeater is known, the first button to get here
// invalidates its repeater's layout, which brings the ArrangeOverride pass the Arrange hook is waiting for.
// ---------------------------------------------------------------------------------------------------------------

using UpdateVisualStates_t = void(WINAPI*)(void* pThis);
UpdateVisualStates_t g_origUpdateVisualStates = nullptr;
std::atomic<bool> g_layoutPassWanted{ false };

void WINAPI TaskListButton_UpdateVisualStates_Hook(void* pThis)
{
    g_origUpdateVisualStates(pThis);

    if (!g_layoutPassWanted.load(std::memory_order_relaxed) || g_unloading.load(std::memory_order_relaxed))
    {
        return;
    }
    try
    {
        // The XAML object behind the implementation: its fourth slot holds the composed base (see the labels
        // mod), which answers for FrameworkElement.
        void* abi = (void**)pThis + 3;
        if (!*(void**)abi)
        {
            return;
        }
        winrt::Windows::Foundation::IUnknown unknown{ nullptr };
        winrt::copy_from_abi(unknown, abi);
        auto button = unknown.try_as<FrameworkElement>();
        if (!button)
        {
            return;
        }
        auto repeater = Media::VisualTreeHelper::GetParent(button).try_as<FrameworkElement>();
        if (!repeater || repeater.Name() != L"TaskbarFrameRepeater")
        {
            return;
        }
        if (g_layoutPassWanted.exchange(false, std::memory_order_acq_rel))
        {
            repeater.InvalidateMeasure();
            repeater.InvalidateArrange();
            SP_LogDebug(L"Repeater layout invalidated from a button's visual state update");
        }
    }
    catch (...)
    {
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Hook: ExperienceToggleButton::UpdateButtonPadding
//
// The Start button is built differently depending on whether Explorer started with centered or left-aligned
// icons: started centered, it lacks the left padding the left-aligned one has, which Windows itself never fixes.
// Pinned to the left it would sit flush against the edge, so the padding is put in here.
// ---------------------------------------------------------------------------------------------------------------

using UpdateButtonPadding_t = void(WINAPI*)(void* pThis);
UpdateButtonPadding_t g_origUpdateButtonPadding = nullptr;

void WINAPI ExperienceToggleButton_UpdateButtonPadding_Hook(void* pThis)
{
    g_origUpdateButtonPadding(pThis);

    if (g_unloading.load(std::memory_order_relaxed))
    {
        return;
    }

    try
    {
        // The implementation object; its second slot is the IInspectable of the projected control.
        FrameworkElement button{ nullptr };
        ((::IUnknown**)pThis)[1]->QueryInterface(winrt::guid_of<FrameworkElement>(), winrt::put_abi(button));
        if (!button || winrt::get_class_name(button) != L"Taskbar.ExperienceToggleButton")
        {
            return;
        }
        if (Automation::AutomationProperties::GetAutomationId(button) != L"StartButton")
        {
            return;
        }

        auto panel = FindChildByName(button, L"ExperienceToggleButtonRootPanel").try_as<Controls::Grid>();
        if (!panel)
        {
            return;
        }

        if (panel.Width() == 45)
        {
            panel.Width(55);
        }

        Thickness padding = panel.Padding();
        if (padding.Left == 2 && padding.Top == 4 && padding.Right == 2 && padding.Bottom == 4)
        {
            padding.Left = 12;
            panel.Padding(padding);
        }
    }
    catch (...)
    {
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Hooks: the Start button's context menu
//
// The menu is centered over the button or aligned to its leading edge depending on TaskbarFrame::Alignment
// (Left = 0, Center = 1). With the button pinned left, the leading-edge placement is the right one, so the
// alignment is reported as Left while the menu is being shown. That read happens when the menu coroutine
// resumes after its items were fetched, so the override brackets the whole resume.
// ---------------------------------------------------------------------------------------------------------------

using GetAlignment_t = HRESULT(WINAPI*)(void* pThis, int* alignment);
GetAlignment_t g_origGetAlignment = nullptr;

HRESULT WINAPI TaskbarFrame_get_Alignment_Hook(void* pThis, int* alignment)
{
    HRESULT hr = g_origGetAlignment(pThis, alignment);
    if (SUCCEEDED(hr) && alignment && t_inStartButtonContextMenu &&
        !g_unloading.load(std::memory_order_relaxed) && g_startMenuOnLeft.load(std::memory_order_relaxed))
    {
        *alignment = 0;     // TaskbarAlignment::Left
    }
    return hr;
}

using ResumeCoro_t = void(WINAPI*)(void* coroFrame);
ResumeCoro_t g_origShowStartButtonContextMenuResume = nullptr;

void WINAPI ShowStartButtonContextMenu_Resume_Hook(void* coroFrame)
{
    const bool previous = t_inStartButtonContextMenu;
    t_inStartButtonContextMenu = true;
    g_origShowStartButtonContextMenuResume(coroFrame);
    t_inStartButtonContextMenu = previous;
}

// ---------------------------------------------------------------------------------------------------------------
// Hook: DwmSetWindowAttribute, for the search flyout
//
// The search flyout is a window of SearchHost.exe that Explorer shows and hides by cloaking. When it is
// uncloaked while the Start menu is open (it was opened from there), it is moved to the left edge of the work
// area to sit under the Start button; when cloaked again it goes back where it was.
// ---------------------------------------------------------------------------------------------------------------

using DwmSetWindowAttribute_t = decltype(&DwmSetWindowAttribute);
using DwmGetWindowAttribute_t = decltype(&DwmGetWindowAttribute);
DwmSetWindowAttribute_t g_origDwmSetWindowAttribute = nullptr;
DwmGetWindowAttribute_t g_DwmGetWindowAttribute = nullptr;

std::mutex g_searchLock;
HWND       g_searchMenuWnd = nullptr;
int        g_searchMenuOriginalX = 0;
HMONITOR   g_searchMenuMonitor = nullptr;

std::wstring ProcessFileName(DWORD processId)
{
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!hProcess)
    {
        return {};
    }

    wchar_t path[MAX_PATH];
    DWORD cch = ARRAYSIZE(path);
    BOOL ok = QueryFullProcessImageNameW(hProcess, 0, path, &cch);
    CloseHandle(hProcess);
    if (!ok)
    {
        return {};
    }

    const wchar_t* name = wcsrchr(path, L'\\');
    return name ? std::wstring(name + 1) : std::wstring();
}

// The Start menu is a CoreWindow of StartMenuExperienceHost.exe that stays cloaked while hidden.
bool IsStartMenuOpen()
{
    if (!g_DwmGetWindowAttribute)
    {
        return false;
    }

    bool open = false;
    EnumWindows(
        [](HWND hWnd, LPARAM lParam) -> BOOL {
            wchar_t className[64];
            if (!GetClassNameW(hWnd, className, ARRAYSIZE(className)) ||
                _wcsicmp(className, L"Windows.UI.Core.CoreWindow") != 0)
            {
                return TRUE;
            }

            DWORD processId = 0;
            if (!GetWindowThreadProcessId(hWnd, &processId))
            {
                return TRUE;
            }
            if (_wcsicmp(ProcessFileName(processId).c_str(), L"StartMenuExperienceHost.exe") != 0)
            {
                return TRUE;
            }

            BOOL cloaked = FALSE;
            if (SUCCEEDED(g_DwmGetWindowAttribute(hWnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && !cloaked)
            {
                *(bool*)lParam = true;
            }
            return FALSE;
        },
        (LPARAM)&open);
    return open;
}

HRESULT WINAPI DwmSetWindowAttribute_Hook(HWND hwnd, DWORD dwAttribute, LPCVOID pvAttribute, DWORD cbAttribute)
{
    if (dwAttribute != DWMWA_CLOAK || cbAttribute != sizeof(BOOL) || !pvAttribute || !hwnd)
    {
        return g_origDwmSetWindowAttribute(hwnd, dwAttribute, pvAttribute, cbAttribute);
    }

    try
    {
        const BOOL cloak = *(const BOOL*)pvAttribute;

        DWORD processId = 0;
        if (!GetWindowThreadProcessId(hwnd, &processId) ||
            _wcsicmp(ProcessFileName(processId).c_str(), L"SearchHost.exe") != 0)
        {
            return g_origDwmSetWindowAttribute(hwnd, dwAttribute, pvAttribute, cbAttribute);
        }

        HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        GetMonitorInfoW(monitor, &monitorInfo);

        RECT rect;
        if (!GetWindowRect(hwnd, &rect))
        {
            return g_origDwmSetWindowAttribute(hwnd, dwAttribute, pvAttribute, cbAttribute);
        }

        const int x = rect.left;
        int xNew = x;
        {
            std::lock_guard<std::mutex> lock(g_searchLock);

            const bool wantLeft = !cloak && !g_unloading.load(std::memory_order_relaxed) &&
                                  g_startMenuOnLeft.load(std::memory_order_relaxed) && IsStartMenuOpen();
            if (wantLeft)
            {
                // Not centered, or already moved.
                if (x == monitorInfo.rcWork.left)
                {
                    return g_origDwmSetWindowAttribute(hwnd, dwAttribute, pvAttribute, cbAttribute);
                }
                xNew = monitorInfo.rcWork.left;
                g_searchMenuWnd = hwnd;
                g_searchMenuOriginalX = x;
                g_searchMenuMonitor = monitor;
            }
            else
            {
                if (!g_searchMenuOriginalX)
                {
                    return g_origDwmSetWindowAttribute(hwnd, dwAttribute, pvAttribute, cbAttribute);
                }
                xNew = g_searchMenuOriginalX;
                const bool sameMonitor = (monitor == g_searchMenuMonitor);
                g_searchMenuWnd = nullptr;
                g_searchMenuOriginalX = 0;
                g_searchMenuMonitor = nullptr;
                if (!sameMonitor)
                {
                    return g_origDwmSetWindowAttribute(hwnd, dwAttribute, pvAttribute, cbAttribute);
                }
            }
        }

        if (xNew != x)
        {
            SP_LogDebug(L"Search flyout: x %d -> %d", x, xNew);
            SetWindowPos(hwnd, nullptr, xNew, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    catch (...)
    {
    }

    return g_origDwmSetWindowAttribute(hwnd, dwAttribute, pvAttribute, cbAttribute);
}

// Puts the search flyout back where it was, if this mod moved it and it is still on the same monitor.
void RestoreSearchMenuPosition()
{
    std::lock_guard<std::mutex> lock(g_searchLock);
    if (!g_searchMenuWnd || !g_searchMenuOriginalX)
    {
        return;
    }

    RECT rect;
    if (MonitorFromWindow(g_searchMenuWnd, MONITOR_DEFAULTTONEAREST) == g_searchMenuMonitor &&
        GetWindowRect(g_searchMenuWnd, &rect) && rect.left != g_searchMenuOriginalX)
    {
        SetWindowPos(g_searchMenuWnd, nullptr, g_searchMenuOriginalX, rect.top, rect.right - rect.left,
                     rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    g_searchMenuWnd = nullptr;
    g_searchMenuOriginalX = 0;
    g_searchMenuMonitor = nullptr;
}

BOOL HookDwm()
{
    HMODULE hDwmapi = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!hDwmapi)
    {
        return FALSE;
    }
    g_DwmGetWindowAttribute = (DwmGetWindowAttribute_t)GetProcAddress(hDwmapi, "DwmGetWindowAttribute");

    if (!SP_HookBegin())
    {
        return FALSE;
    }
    if (!SP_SetExportHook(L"dwmapi.dll", "DwmSetWindowAttribute", DwmSetWindowAttribute_Hook,
                          &g_origDwmSetWindowAttribute))
    {
        SP_HookAbort();
        return FALSE;
    }
    return SP_HookCommit();
}

// ---------------------------------------------------------------------------------------------------------------
// Installing the Taskbar.View.dll hooks
// ---------------------------------------------------------------------------------------------------------------

BOOL InstallTaskbarViewHooks(HMODULE hTaskbarView)
{
    static const wchar_t* const kArrangeOverride[] = {
        LR"(public: virtual int __cdecl winrt::impl::produce<struct winrt::Taskbar::implementation::TaskbarCollapsibleLayout,struct winrt::Microsoft::UI::Xaml::Controls::IVirtualizingLayoutOverrides>::ArrangeOverride(void *,struct winrt::Windows::Foundation::Size,struct winrt::Windows::Foundation::Size *))",
    };
    static const wchar_t* const kUpdateButtonPadding[] = {
        LR"(protected: virtual void __cdecl winrt::Taskbar::implementation::ExperienceToggleButton::UpdateButtonPadding(void))",
    };
    static const wchar_t* const kGetAlignment[] = {
        LR"(public: virtual int __cdecl winrt::impl::produce<struct winrt::Taskbar::implementation::TaskbarFrame,struct winrt::Taskbar::ITaskbarFrame>::get_Alignment(int *))",
    };
    static const wchar_t* const kShowStartButtonContextMenuResume[] = {
        LR"(static  winrt::Taskbar::implementation::ContextMenus::ShowStartButtonContextMenuAsync$_ResumeCoro$1())",
    };

    static const wchar_t* const kArrangeOverrideBase[] = {
        LR"(public: struct winrt::Windows::Foundation::Size __cdecl winrt::Taskbar::implementation::TaskbarCollapsibleLayoutBase<struct winrt::Taskbar::implementation::TaskbarCollapsibleLayoutXamlTraits>::ArrangeOverride(struct winrt::Microsoft::UI::Xaml::Controls::VirtualizingLayoutContext const &,struct winrt::Windows::Foundation::Size))",
    };

    static const wchar_t* const kUpdateVisualStates[] = {
        LR"(private: void __cdecl winrt::Taskbar::implementation::TaskListButton::UpdateVisualStates(void))",
    };

    SP_SymbolHook hooks[6] = {};

    hooks[5].symbols = kUpdateVisualStates;
    hooks[5].symbolCount = ARRAYSIZE(kUpdateVisualStates);
    hooks[5].pOriginal = (void**)&g_origUpdateVisualStates;
    hooks[5].hookFunction = (void*)TaskListButton_UpdateVisualStates_Hook;
    hooks[5].optional = TRUE;

    // Without one of these two there is no mod: the produce thunk on builds up to 24H2, the template base's
    // own member on 26200 (both are hooked when both exist; the base one is the one that runs there).
    hooks[0].symbols = kArrangeOverride;
    hooks[0].symbolCount = ARRAYSIZE(kArrangeOverride);
    hooks[0].pOriginal = (void**)&g_origArrangeOverride;
    hooks[0].hookFunction = (void*)TaskbarCollapsibleLayout_ArrangeOverride_Hook;
    hooks[0].optional = TRUE;

    hooks[4].symbols = kArrangeOverrideBase;
    hooks[4].symbolCount = ARRAYSIZE(kArrangeOverrideBase);
    hooks[4].pOriginal = (void**)&g_origArrangeOverrideBase;
    hooks[4].hookFunction = (void*)TaskbarCollapsibleLayoutBase_ArrangeOverride_Hook;
    hooks[4].optional = TRUE;

    // The rest are refinements: the button still moves without them.
    hooks[1].symbols = kUpdateButtonPadding;
    hooks[1].symbolCount = ARRAYSIZE(kUpdateButtonPadding);
    hooks[1].pOriginal = (void**)&g_origUpdateButtonPadding;
    hooks[1].hookFunction = (void*)ExperienceToggleButton_UpdateButtonPadding_Hook;
    hooks[1].optional = TRUE;

    hooks[2].symbols = kGetAlignment;
    hooks[2].symbolCount = ARRAYSIZE(kGetAlignment);
    hooks[2].pOriginal = (void**)&g_origGetAlignment;
    hooks[2].hookFunction = (void*)TaskbarFrame_get_Alignment_Hook;
    hooks[2].optional = TRUE;

    hooks[3].symbols = kShowStartButtonContextMenuResume;
    hooks[3].symbolCount = ARRAYSIZE(kShowStartButtonContextMenuResume);
    hooks[3].pOriginal = (void**)&g_origShowStartButtonContextMenuResume;
    hooks[3].hookFunction = (void*)ShowStartButtonContextMenu_Resume_Hook;
    hooks[3].optional = TRUE;

    if (!SP_HookSymbols(hTaskbarView, hooks, ARRAYSIZE(hooks)) || (!g_origArrangeOverride && !g_origArrangeOverrideBase))
    {
        SP_LogError(L"TaskbarCollapsibleLayout::ArrangeOverride was not found in this build (neither spelling)");
        return FALSE;
    }
    SP_Log(L"ArrangeOverride hooked: produce thunk %s, template base %s",
           g_origArrangeOverride ? L"yes" : L"no", g_origArrangeOverrideBase ? L"yes" : L"no");

    if (!g_origUpdateButtonPadding)
    {
        SP_Log(L"ExperienceToggleButton::UpdateButtonPadding not found; the Start button keeps its own padding");
    }
    if (!g_origGetAlignment || !g_origShowStartButtonContextMenuResume)
    {
        SP_Log(L"The Start button menu symbols were not found; its menu stays where Windows puts it");
    }

    SP_Log(L"Taskbar.View.dll hooks installed");
    return TRUE;
}

// SP_WaitForModule callback, on a helper thread.
void OnTaskbarViewLoaded(HMODULE hModule, void* /*context*/)
{
    if (!InstallTaskbarViewHooks(hModule))
    {
        return;
    }

    g_viewHooked.store(true, std::memory_order_release);

    // The taskbar may already be up (the mod was turned on from the settings window); style it now. On a cold
    // sign-in it is not there yet and the Arrange hook styles it at its first layout pass.
    try
    {
        ApplySettings();
    }
    catch (...)
    {
        SP_LogError(L"The first style pass failed");
    }

    // Without the taskbar.dll symbols (build 26200) nothing could be reached directly, and a taskbar that is
    // already up may not lay itself out again for a long time: ask it to (the logical DPI override setting
    // change re-measures the frame, the colour set change re-runs the buttons' visual states), so the Arrange
    // hook meets the repeater now rather than at the next window change.
    bool known;
    {
        std::lock_guard<std::mutex> lock(g_knownLock);
        known = !g_known.empty();
    }
    if (!known)
    {
        g_layoutPassWanted.store(g_origUpdateVisualStates != nullptr, std::memory_order_relaxed);
        EnumWindows(
            [](HWND hWnd, LPARAM) -> BOOL {
                DWORD processId = 0;
                wchar_t className[32];
                if (GetWindowThreadProcessId(hWnd, &processId) && processId == GetCurrentProcessId() &&
                    GetClassNameW(hWnd, className, ARRAYSIZE(className)) &&
                    (_wcsicmp(className, L"Shell_TrayWnd") == 0 || _wcsicmp(className, L"Shell_SecondaryTrayWnd") == 0))
                {
                    SendMessageTimeoutW(hWnd, WM_SETTINGCHANGE, SPI_SETLOGICALDPIOVERRIDE, 0,
                                        SMTO_NORMAL | SMTO_ABORTIFHUNG, 2000, nullptr);
                    SendMessageTimeoutW(hWnd, WM_SETTINGCHANGE, 0, (LPARAM)L"ImmersiveColorSet",
                                        SMTO_NORMAL | SMTO_ABORTIFHUNG, 2000, nullptr);
                }
                return TRUE;
            },
            0);
        SP_LogDebug(L"Asked the taskbar for a layout pass");
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------------------------

void LoadSettings()
{
    g_startMenuOnLeft.store(SP_GetIntSetting(L"StartMenuOnLeft", 1) != 0, std::memory_order_relaxed);
    g_searchAndTaskViewOnLeft.store(SP_GetIntSetting(L"SearchAndTaskViewOnLeft", 0) != 0,
                                    std::memory_order_relaxed);
}

BOOL Init()
{
    g_unloading.store(false, std::memory_order_relaxed);
    g_viewHooked.store(false, std::memory_order_relaxed);
    g_arrangeHookTried.store(false, std::memory_order_relaxed);
    g_layoutPassWanted.store(false, std::memory_order_relaxed);
    g_origArrange = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_knownLock);
        g_known.clear();
    }

    LoadSettings();

    // Neither of these is fatal: without taskbar.dll a settings change waits for the next layout pass, and
    // without dwmapi the search flyout simply stays put.
    g_taskbarDllReady.store(ResolveTaskbarDllSymbols() ? true : false, std::memory_order_relaxed);
    if (!HookDwm())
    {
        SP_LogError(L"DwmSetWindowAttribute could not be hooked; the search flyout will not follow the Start button");
    }

    if (!SP_WaitForModule(L"Taskbar.View.dll", 60000, OnTaskbarViewLoaded, nullptr))
    {
        SP_LogError(L"Could not wait for Taskbar.View.dll");
        return FALSE;
    }

    SP_Log(L"Waiting for the taskbar");
    return TRUE;
}

void SettingsChanged()
{
    RestoreSearchMenuPosition();
    LoadSettings();

    if (g_viewHooked.load(std::memory_order_acquire))
    {
        try
        {
            ApplySettings();
        }
        catch (...)
        {
            SP_LogError(L"Re-applying the style failed");
        }
    }
}

void BeforeUninit()
{
    // Still hooked. Every hook now passes through, and ApplyStyle puts the margins back; the layout pass that
    // follows the margin change lets Windows place the buttons the way it wants to.
    g_unloading.store(true, std::memory_order_relaxed);

    if (g_viewHooked.load(std::memory_order_acquire))
    {
        try
        {
            ApplySettings();
        }
        catch (...)
        {
            SP_LogError(L"Restoring the taskbar layout failed");
        }
    }
}

void Uninit()
{
    RestoreSearchMenuPosition();

    // Give the callbacks queued on the taskbar's dispatcher a moment to finish; each one checks g_unloading and
    // returns at once, but it still has to run before this code can go.
    for (int i = 0; i < 100 && g_pendingDispatches.load(std::memory_order_relaxed) > 0; ++i)
    {
        Sleep(20);
    }

    std::lock_guard<std::mutex> lock(g_knownLock);
    g_known.clear();
}

}   // namespace

SP_MOD_DEFINE(g_modTaskbarStartButtonPosition) =
{
    /* id             */ SP_MOD_ID,
    /* name           */ L"Keep the Start button on the left",
    /* basedOn        */ "taskbar-start-button-position",
    /* originalAuthor */ "m417z",
    /* targets        */ SP_TARGET_EXPLORER,
    /* minOsBuild     */ 22000,
    /* flags          */ 0,
    /* Init           */ Init,
    /* AfterInit      */ nullptr,
    /* SettingsChanged*/ SettingsChanged,
    /* BeforeUninit   */ BeforeUninit,
    /* Uninit         */ Uninit,
};
