//
// taskbar-wheel-cycle - cycle through the taskbar buttons with the mouse wheel on the Windows 11 taskbar.
//
// Adapted from the idea behind the Windhawk mod "Cycle taskbar buttons with mouse wheel" (taskbar-wheel-cycle)
// by m417z. The implementation here is written against this engine's API.
//
// What it does
// ------------
// Turning the wheel while the pointer is over a taskbar button activates the previous or next window on that
// taskbar, in the order the buttons are laid out. Scrolling down moves right, scrolling up moves left, and a
// setting reverses that. Minimized windows can be skipped, and the walk can wrap around at either end.
//
// Two halves
// ----------
// 1. The wheel. The Windows 11 taskbar is XAML, in Taskbar.View.dll. A wheel anywhere over it bubbles up to
//        winrt::Taskbar::implementation::TaskbarFrame::OnPointerWheelChanged
//    the IControlOverrides override on the frame, which is hooked. The element the wheel landed on is read from
//    the event and walked up the visual tree; only a wheel over a Taskbar.TaskListButton is acted upon.
//    Everything else - the empty space, Start, search, widgets, the tray and the clock - is passed on untouched.
//    That is the boundary with any other mod that wants the wheel on the taskbar (volume control over the tray
//    or the empty space, say): it can hook the same function, and the events this mod does not claim reach it
//    exactly as they would without this mod.
//
// 2. The model. The XAML buttons are still driven by the classic task list object, CTaskListWnd in taskbar.dll:
//    one per taskbar, behind that taskbar's MSTaskListWClass window, with the object pointer in the window's
//    first extra slot. Its button groups (CTaskBtnGroup) hold the task items (CWindowTaskItem for Win32
//    windows, CImmersiveTaskItem for packaged apps), and CTaskListWnd::SwitchToItem does what a click on a
//    button does. None of that is exported, so the functions are resolved from the PDB and called directly.
//    CTaskListWnd::_SetActiveItem is hooked, not to change anything but to remember which item the taskbar
//    considers active on each task list; that is where the walk starts. Until the first activation has been
//    seen, the foreground window is matched against the items instead.
//
// The button group array
// ----------------------
// CTaskListWnd keeps its groups in a DPA at a slot of the object that is not published. It is recovered the way
// the original mod does it: CTaskListWnd::GetButtonGroupCount is nothing but DPA_GetPtrCount(this->groups),
// which reads one int through one pointer at a fixed slot of `this`. Calling it with a fake object whose every
// slot points at an int holding that slot's own index therefore returns the slot number. The call is made once,
// under an exception guard, and an implausible answer disables the mod rather than being trusted.
//
// Exceptions
// ----------
// C++/WinRT reports failures by throwing, so this file is compiled with exceptions on while the rest of the
// engine is not. Every path XAML calls into is wrapped; an exception reaching the taskbar's UI thread would end
// the shell.
//
#define SP_MOD_ID "taskbar-wheel-cycle"
#include "engine/modapi.h"

#include <windowsx.h>     // GET_X_LPARAM / GET_Y_LPARAM

// Windows.h leaves a GetCurrentTime macro behind that collides with a method of the same name in the XAML
// projection, so it goes before the C++/WinRT headers.
#undef GetCurrentTime

#include <unknwn.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Media.h>

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace {

using winrt::Windows::Foundation::IInspectable;
using winrt::Windows::UI::Xaml::DependencyObject;
using winrt::Windows::UI::Xaml::UIElement;
using winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs;
using winrt::Windows::UI::Xaml::Media::VisualTreeHelper;

// ---------------------------------------------------------------------------------------------------------------
// Settings
//
// Written on the engine thread, read on the taskbar's UI thread.
// ---------------------------------------------------------------------------------------------------------------

std::atomic<bool> g_skipMinimized{ true };
std::atomic<bool> g_wrapAround{ true };
std::atomic<bool> g_reverseDirection{ false };

// ---------------------------------------------------------------------------------------------------------------
// The task list model (taskbar.dll)
// ---------------------------------------------------------------------------------------------------------------

using GetButtonGroupCount_t = int(WINAPI*)(void* pThis);
using GetGroupType_t        = int(WINAPI*)(void* pThis);
using GetNumItems_t         = int(WINAPI*)(void* pThis);
using GetTaskItem_t         = void*(WINAPI*)(void* pThis, int index);
using GetWindow_t           = HWND(WINAPI*)(void* pThis);
using SwitchToItem_t        = void(WINAPI*)(void* pThis, void* taskItem);
using SetActiveItem_t       = void(WINAPI*)(void* pThis, void* taskBtnGroup, int buttonIndex);

// Vtables, used to find the right base subobject of a CTaskListWnd and to tell the two task item classes apart.
void* g_vtblTaskListUI = nullptr;
void* g_vtblTaskListSite = nullptr;
void* g_vtblImmersiveTaskItem = nullptr;

// Resolved, called directly.
GetButtonGroupCount_t g_GetButtonGroupCount = nullptr;
GetGroupType_t        g_GetGroupType = nullptr;
GetNumItems_t         g_GetNumItems = nullptr;
GetTaskItem_t         g_GetTaskItem = nullptr;
GetWindow_t           g_WindowTaskItem_GetWindow = nullptr;
GetWindow_t           g_ImmersiveTaskItem_GetWindow = nullptr;
SwitchToItem_t        g_SwitchToItem = nullptr;

// Hooked.
SetActiveItem_t       g_origSetActiveItem = nullptr;

// Slot of the group DPA inside the ITaskListUI view of a CTaskListWnd, in pointer-sized units. -1 until probed.
int g_groupsSlot = -1;

// Set once everything above is in place. Published with release semantics, so a reader that sees true also sees
// the pointers.
std::atomic<bool> g_modelReady{ false };

// Everything below is touched from the taskbar's UI thread only, but a lock costs nothing here and a second
// taskbar thread on some build would otherwise be a silent race.
std::mutex g_lock;

// CTaskListWnd -> the task item the taskbar last made active on it, or nullptr for "none". Keys and values are
// compared, never dereferenced, so an object that has since gone away can do no harm.
std::unordered_map<void*, void*> g_activeItem;

// Wheel deltas smaller than one notch are carried over to the next event on the same task list, for a short
// while, so a high-resolution wheel still adds up to whole steps.
HWND  g_lastTarget = nullptr;
DWORD g_lastTime = 0;
int   g_lastRemainder = 0;
constexpr DWORD kRemainderLifetimeMs = 5000;

// eTBGROUPTYPE values of the groups that hold windows: 1 is a running application, 3 a pinned one that is also
// running. 2 is pinned only and has nothing to switch to.
bool IsWindowGroup(int type)
{
    return type == 1 || type == 3;
}

// A CTaskListWnd has several interface bases. The one whose vtable matches is found by scanning the object's
// leading slots; bounded, so a vtable that is not there can never walk off the object.
void* FindInterface(void* object, void* vtable)
{
    if (!object || !vtable)
    {
        return nullptr;
    }
    void** slot = (void**)object;
    for (int i = 0; i < 64; ++i)
    {
        if (slot[i] == vtable)
        {
            return slot + i;
        }
    }
    return nullptr;
}

// See "The button group array" in the header. Plain arrays only: a function with a __try block may not hold
// objects that need unwinding.
int ProbeGroupsSlot()
{
    constexpr int kSlots = 256;
    int  values[kSlots];
    int* fake[kSlots];
    for (int i = 0; i < kSlots; ++i)
    {
        values[i] = i;
        fake[i] = &values[i];
    }

    int slot = -1;
    __try
    {
        slot = g_GetButtonGroupCount(fake);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        slot = -1;
    }

    // Slot 0 is the vtable pointer, so it can never be the answer; anything past the fake object is nonsense.
    if (slot <= 0 || slot >= kSlots)
    {
        return -1;
    }
    return slot;
}

struct GroupList
{
    void** items = nullptr;
    int    count = 0;
};

// The groups of one task list. A DPA starts with its item count, and its item array is the next pointer-sized
// field: the same layout commctrl.h's DPA_GetPtrCount and DPA_FastGetPtr read.
bool GetGroups(void* taskList, GroupList* out)
{
    void* ui = FindInterface(taskList, g_vtblTaskListUI);
    if (!ui || g_groupsSlot < 0)
    {
        return false;
    }

    void* hdpa = ((void**)ui)[g_groupsSlot];
    if (!hdpa)
    {
        return false;
    }

    int    count = *(int*)hdpa;
    void** items = *(void***)((BYTE*)hdpa + sizeof(void*));
    if (count < 0 || count > 4096 || (count > 0 && !items))
    {
        return false;
    }

    out->items = items;
    out->count = count;
    return true;
}

int GroupType(const GroupList& groups, int group)
{
    void* object = groups.items[group];
    return object ? g_GetGroupType(object) : 0;
}

int ItemCount(const GroupList& groups, int group)
{
    void* object = groups.items[group];
    return object ? g_GetNumItems(object) : 0;
}

void* ItemAt(const GroupList& groups, int group, int index)
{
    void* object = groups.items[group];
    return object ? g_GetTaskItem(object, index) : nullptr;
}

HWND ItemWindow(void* item)
{
    if (!item)
    {
        return nullptr;
    }
    if (*(void**)item == g_vtblImmersiveTaskItem)
    {
        return g_ImmersiveTaskItem_GetWindow(item);
    }
    return g_WindowTaskItem_GetWindow(item);
}

bool ItemMinimized(void* item)
{
    HWND hWnd = ItemWindow(item);
    return hWnd && IsIconic(hWnd);
}

// ---------------------------------------------------------------------------------------------------------------
// Walking the buttons
// ---------------------------------------------------------------------------------------------------------------

// A button, as a group and an index within it. group == -1 means "before the first / after the last".
struct Position
{
    int group = -1;
    int index = -1;

    bool operator==(const Position& other) const { return group == other.group && index == other.index; }
};

// One step to the right; FALSE at the end. Only groups that hold at least one window are visited.
bool StepRight(const GroupList& groups, Position* pos)
{
    int count = (pos->group >= 0) ? ItemCount(groups, pos->group) : 0;
    if (pos->index + 1 < count)
    {
        pos->index++;
        return true;
    }

    for (int next = pos->group + 1; next < groups.count; ++next)
    {
        if (IsWindowGroup(GroupType(groups, next)) && ItemCount(groups, next) > 0)
        {
            pos->group = next;
            pos->index = 0;
            return true;
        }
    }
    return false;
}

// One step to the left; FALSE at the start.
bool StepLeft(const GroupList& groups, Position* pos)
{
    if (pos->group >= 0 && pos->index > 0)
    {
        pos->index--;
        return true;
    }

    int start = (pos->group >= 0) ? pos->group - 1 : groups.count - 1;
    for (int prev = start; prev >= 0; --prev)
    {
        if (IsWindowGroup(GroupType(groups, prev)))
        {
            int count = ItemCount(groups, prev);
            if (count > 0)
            {
                pos->group = prev;
                pos->index = count - 1;
                return true;
            }
        }
    }
    return false;
}

// Moves `steps` buttons from `active` (positive to the right, negative to the left) and returns the task item
// that lands on, or nullptr when there is nothing to switch to or the walk ends where it began.
void* Cycle(const GroupList& groups, Position active, int steps, bool skipMinimized, bool wrapAround)
{
    if (steps == 0)
    {
        return nullptr;
    }

    const bool right = steps > 0;
    int remaining = right ? steps : -steps;

    Position pos = active;
    Position last = active;    // the last position actually reached

    while (remaining-- > 0)
    {
        bool ok = right ? StepRight(groups, &pos) : StepLeft(groups, &pos);
        while (ok && skipMinimized && ItemMinimized(ItemAt(groups, pos.group, pos.index)))
        {
            ok = right ? StepRight(groups, &pos) : StepLeft(groups, &pos);
        }

        if (!ok)
        {
            // Ran off the end. Starting from nowhere and still finding nothing means the taskbar has no window
            // to offer, which also stops a wrap-around from looping.
            if (last.group == -1)
            {
                return nullptr;
            }

            if (wrapAround)
            {
                pos = Position{};
                remaining++;        // this step is retried from the far end
            }
            else
            {
                pos = last;         // stop at the edge
                break;
            }
        }

        last = pos;
    }

    if (pos.group < 0 || pos == active)
    {
        return nullptr;
    }
    return ItemAt(groups, pos.group, pos.index);
}

// The button the walk starts from: the item the taskbar last made active on this task list if that is known and
// still present, otherwise the button of the foreground window, otherwise nowhere.
Position FindActive(void* taskList, const GroupList& groups)
{
    void* tracked = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        auto it = g_activeItem.find(taskList);
        if (it != g_activeItem.end())
        {
            tracked = it->second;
        }
    }

    HWND hForeground = GetForegroundWindow();
    HWND hForegroundOwner = hForeground ? GetAncestor(hForeground, GA_ROOTOWNER) : nullptr;

    Position byWindow;
    for (int g = 0; g < groups.count; ++g)
    {
        if (!IsWindowGroup(GroupType(groups, g)))
        {
            continue;
        }
        int count = ItemCount(groups, g);
        for (int i = 0; i < count; ++i)
        {
            void* item = ItemAt(groups, g, i);
            if (!item)
            {
                continue;
            }
            if (tracked && item == tracked)
            {
                return Position{ g, i };
            }
            if (byWindow.group < 0 && hForeground)
            {
                HWND hItem = ItemWindow(item);
                if (hItem && (hItem == hForeground || hItem == hForegroundOwner))
                {
                    byWindow = Position{ g, i };
                }
            }
        }
    }
    return byWindow;
}

// One wheel event over a task list, in raw wheel units.
void OnTaskListWheel(HWND hTaskList, int delta)
{
    int accumulated = delta;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (g_lastTarget == hTaskList && GetTickCount() - g_lastTime < kRemainderLifetimeMs)
        {
            accumulated += g_lastRemainder;
        }
        g_lastTarget = hTaskList;
        g_lastTime = GetTickCount();
        g_lastRemainder = accumulated % WHEEL_DELTA;
    }

    // Scrolling down gives a negative delta and moves to the right.
    int steps = -accumulated / WHEEL_DELTA;
    if (steps == 0)
    {
        return;
    }
    if (g_reverseDirection.load(std::memory_order_relaxed))
    {
        steps = -steps;
    }

    void* taskList = (void*)GetWindowLongPtrW(hTaskList, 0);
    if (!taskList)
    {
        return;
    }

    GroupList groups;
    if (!GetGroups(taskList, &groups))
    {
        SP_LogDebug(L"The task list has no group array to walk");
        return;
    }

    Position active = FindActive(taskList, groups);
    void* target = Cycle(groups, active, steps,
                         g_skipMinimized.load(std::memory_order_relaxed),
                         g_wrapAround.load(std::memory_order_relaxed));
    if (!target)
    {
        SP_LogDebug(L"%d step(s) from group %d item %d: nothing to switch to", steps, active.group, active.index);
        return;
    }

    // SwitchToItem is a method of the ITaskListSite base, so it wants that subobject as `this`.
    void* site = FindInterface(taskList, g_vtblTaskListSite);
    if (!site)
    {
        return;
    }

    SP_LogDebug(L"%d step(s) from group %d item %d", steps, active.group, active.index);
    g_SwitchToItem(site, target);
}

// ---------------------------------------------------------------------------------------------------------------
// Finding the task list under the pointer
//
// Each taskbar keeps its classic task list window even though XAML draws the buttons. The primary taskbar has it
// under Shell_TrayWnd > ReBarWindow32 > MSTaskSwWClass; a secondary one under Shell_SecondaryTrayWnd > WorkerW.
// ---------------------------------------------------------------------------------------------------------------

HWND TaskListFromTaskbar(HWND hTaskbar)
{
    wchar_t wszClass[32];
    if (!GetClassNameW(hTaskbar, wszClass, ARRAYSIZE(wszClass)))
    {
        return nullptr;
    }

    if (_wcsicmp(wszClass, L"Shell_TrayWnd") == 0)
    {
        HWND hRebar = FindWindowExW(hTaskbar, nullptr, L"ReBarWindow32", nullptr);
        HWND hSwitcher = hRebar ? FindWindowExW(hRebar, nullptr, L"MSTaskSwWClass", nullptr) : nullptr;
        return hSwitcher ? FindWindowExW(hSwitcher, nullptr, L"MSTaskListWClass", nullptr) : nullptr;
    }

    if (_wcsicmp(wszClass, L"Shell_SecondaryTrayWnd") == 0)
    {
        HWND hWorker = FindWindowExW(hTaskbar, nullptr, L"WorkerW", nullptr);
        return hWorker ? FindWindowExW(hWorker, nullptr, L"MSTaskListWClass", nullptr) : nullptr;
    }

    return nullptr;
}

HWND TaskListFromPoint(POINT pt)
{
    HWND hWnd = WindowFromPoint(pt);
    HWND hRoot = hWnd ? GetAncestor(hWnd, GA_ROOT) : nullptr;
    if (!hRoot)
    {
        return nullptr;
    }

    // Only this process's taskbars carry a task list object in their window slots.
    DWORD processId = 0;
    GetWindowThreadProcessId(hRoot, &processId);
    if (processId != GetCurrentProcessId())
    {
        return nullptr;
    }

    return TaskListFromTaskbar(hRoot);
}

// ---------------------------------------------------------------------------------------------------------------
// The XAML side (Taskbar.View.dll)
// ---------------------------------------------------------------------------------------------------------------

// Wraps a raw ABI pointer handed to a hook as a projected object, taking its own reference and leaving the
// shell's alone. Empty when the object is not what was asked for.
template <typename T>
T FromAbi(void* abi)
{
    T result{ nullptr };
    if (abi)
    {
        static_cast<::IUnknown*>(abi)->QueryInterface(winrt::guid_of<T>(), winrt::put_abi(result));
    }
    return result;
}

// TRUE when the element the wheel landed on is a taskbar button or something inside one. The walk stops at the
// frame itself, which is the root of the taskbar's tree.
bool IsOverTaskListButton(PointerRoutedEventArgs const& args)
{
    IInspectable source = args.OriginalSource();
    if (!source)
    {
        return false;
    }

    DependencyObject current = source.try_as<DependencyObject>();
    for (int depth = 0; current && depth < 48; ++depth)
    {
        winrt::hstring className = winrt::get_class_name(current);
        if (depth == 0)
        {
            SP_LogDebug(L"Wheel over %s", className.c_str());
        }
        if (className == L"Taskbar.TaskListButton")
        {
            return true;
        }
        if (className == L"Taskbar.TaskbarFrame")
        {
            return false;
        }
        current = VisualTreeHelper::GetParent(current);
    }
    return false;
}

// Returns TRUE when the event was consumed. May throw; the hook body catches.
bool HandleWheel(void* pThis, void* pArgs)
{
    if (!g_modelReady.load(std::memory_order_acquire))
    {
        return false;
    }

    // The override is shared by every control produced from the same template; only the frame is wanted.
    IInspectable frame = FromAbi<IInspectable>(pThis);
    if (!frame || winrt::get_class_name(frame) != L"Taskbar.TaskbarFrame")
    {
        return false;
    }

    PointerRoutedEventArgs args = FromAbi<PointerRoutedEventArgs>(pArgs);
    if (!args)
    {
        return false;
    }

    if (!IsOverTaskListButton(args))
    {
        return false;
    }

    int delta = args.GetCurrentPoint(frame.try_as<UIElement>()).Properties().MouseWheelDelta();
    if (delta == 0)
    {
        return false;
    }

    // Which taskbar: the wheel message the shell is handling right now carries the screen position.
    DWORD messagePos = GetMessagePos();
    POINT pt = { GET_X_LPARAM(messagePos), GET_Y_LPARAM(messagePos) };
    HWND hTaskList = TaskListFromPoint(pt);
    if (!hTaskList)
    {
        SP_LogDebug(L"No task list window under (%d, %d)", pt.x, pt.y);
        return false;
    }

    // A window can only be brought to the foreground by the process that last received input. Sending an empty
    // input event makes that this process, so the switch is honoured instead of ending as a flashing button.
    INPUT input = {};
    SendInput(1, &input, sizeof(input));

    OnTaskListWheel(hTaskList, delta);

    args.Handled(true);
    return true;
}

using OnPointerWheelChanged_t = int(WINAPI*)(void* pThis, void* pArgs);
OnPointerWheelChanged_t g_origOnPointerWheelChanged = nullptr;

int WINAPI TaskbarFrame_OnPointerWheelChanged_Hook(void* pThis, void* pArgs)
{
    bool handled = false;
    try
    {
        handled = HandleWheel(pThis, pArgs);
    }
    catch (winrt::hresult_error const& e)
    {
        SP_LogError(L"The wheel could not be handled: 0x%08X", (unsigned)e.code());
    }
    catch (...)
    {
        SP_LogError(L"The wheel could not be handled");
    }

    if (handled)
    {
        return 0;   // S_OK; the event was consumed and the frame never sees it
    }
    return g_origOnPointerWheelChanged(pThis, pArgs);
}

// ---------------------------------------------------------------------------------------------------------------
// The active item hook (taskbar.dll)
// ---------------------------------------------------------------------------------------------------------------

void WINAPI CTaskListWnd_SetActiveItem_Hook(void* pThis, void* taskBtnGroup, int buttonIndex)
{
    void* item = nullptr;
    if (taskBtnGroup && buttonIndex >= 0 && g_GetTaskItem)
    {
        item = g_GetTaskItem(taskBtnGroup, buttonIndex);
    }

    try
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_activeItem[pThis] = item;
    }
    catch (...)
    {
        // Out of memory while remembering a pointer: the walk will fall back to the foreground window.
    }

    g_origSetActiveItem(pThis, taskBtnGroup, buttonIndex);
}

// ---------------------------------------------------------------------------------------------------------------
// Installing
//
// Both libraries arrive after the engine on a cold sign-in, so each half is installed from the callback for its
// module. The wheel hook does nothing until the model half reports ready, whichever order they come in.
// ---------------------------------------------------------------------------------------------------------------

SP_SymbolHook Entry(const wchar_t* const* names, size_t count, void** original, void* hook)
{
    SP_SymbolHook entry = {};
    entry.symbols = names;
    entry.symbolCount = count;
    entry.pOriginal = original;
    entry.hookFunction = hook;
    entry.optional = FALSE;
    return entry;
}

void OnTaskbarDllLoaded(HMODULE hModule, void*)
{
    static const wchar_t* const kVtblTaskListUI[] = {
        L"const CTaskListWnd::`vftable'{for `ITaskListUI'}",
    };
    static const wchar_t* const kVtblTaskListSite[] = {
        L"const CTaskListWnd::`vftable'{for `ITaskListSite'}",
    };
    static const wchar_t* const kVtblImmersiveTaskItem[] = {
        L"const CImmersiveTaskItem::`vftable'{for `ITaskItem'}",
    };
    static const wchar_t* const kGetButtonGroupCount[] = {
        L"public: virtual int __cdecl CTaskListWnd::GetButtonGroupCount(void)",
    };
    static const wchar_t* const kGetGroupType[] = {
        L"public: virtual enum eTBGROUPTYPE __cdecl CTaskBtnGroup::GetGroupType(void)",
    };
    static const wchar_t* const kGetNumItems[] = {
        L"public: virtual int __cdecl CTaskBtnGroup::GetNumItems(void)",
    };
    static const wchar_t* const kGetTaskItem[] = {
        L"public: virtual struct ITaskItem * __cdecl CTaskBtnGroup::GetTaskItem(int)",
    };
    static const wchar_t* const kWindowTaskItemGetWindow[] = {
        L"public: virtual struct HWND__ * __cdecl CWindowTaskItem::GetWindow(void)",
    };
    static const wchar_t* const kImmersiveTaskItemGetWindow[] = {
        L"public: virtual struct HWND__ * __cdecl CImmersiveTaskItem::GetWindow(void)",
    };
    static const wchar_t* const kSwitchToItem[] = {
        L"public: virtual void __cdecl CTaskListWnd::SwitchToItem(struct ITaskItem *)",
    };
    static const wchar_t* const kSetActiveItem[] = {
        L"protected: void __cdecl CTaskListWnd::_SetActiveItem(struct ITaskBtnGroup *,int)",
    };

    const SP_SymbolHook hooks[] = {
        Entry(kVtblTaskListUI,            ARRAYSIZE(kVtblTaskListUI),            &g_vtblTaskListUI,                      nullptr),
        Entry(kVtblTaskListSite,          ARRAYSIZE(kVtblTaskListSite),          &g_vtblTaskListSite,                    nullptr),
        Entry(kVtblImmersiveTaskItem,     ARRAYSIZE(kVtblImmersiveTaskItem),     &g_vtblImmersiveTaskItem,               nullptr),
        Entry(kGetButtonGroupCount,       ARRAYSIZE(kGetButtonGroupCount),       (void**)&g_GetButtonGroupCount,         nullptr),
        Entry(kGetGroupType,              ARRAYSIZE(kGetGroupType),              (void**)&g_GetGroupType,                nullptr),
        Entry(kGetNumItems,               ARRAYSIZE(kGetNumItems),               (void**)&g_GetNumItems,                 nullptr),
        Entry(kGetTaskItem,               ARRAYSIZE(kGetTaskItem),               (void**)&g_GetTaskItem,                 nullptr),
        Entry(kWindowTaskItemGetWindow,   ARRAYSIZE(kWindowTaskItemGetWindow),   (void**)&g_WindowTaskItem_GetWindow,    nullptr),
        Entry(kImmersiveTaskItemGetWindow,ARRAYSIZE(kImmersiveTaskItemGetWindow),(void**)&g_ImmersiveTaskItem_GetWindow, nullptr),
        Entry(kSwitchToItem,              ARRAYSIZE(kSwitchToItem),              (void**)&g_SwitchToItem,                nullptr),
        Entry(kSetActiveItem,             ARRAYSIZE(kSetActiveItem),             (void**)&g_origSetActiveItem,
              (void*)CTaskListWnd_SetActiveItem_Hook),
    };

    if (!SP_HookSymbols(hModule, hooks, ARRAYSIZE(hooks)))
    {
        SP_LogError(L"The task list functions were not found in taskbar.dll on this build; the wheel will do nothing");
        return;
    }

    g_groupsSlot = ProbeGroupsSlot();
    if (g_groupsSlot < 0)
    {
        SP_LogError(L"The button group array could not be located; the wheel will do nothing");
        return;
    }

    g_modelReady.store(true, std::memory_order_release);
    SP_Log(L"Task list model ready; the group array is at slot %d", g_groupsSlot);
}

void OnTaskbarViewLoaded(HMODULE hModule, void*)
{
    static const wchar_t* const kNames[] = {
        L"public: virtual int __cdecl winrt::impl::produce<struct winrt::Taskbar::implementation::TaskbarFrame,struct winrt::Windows::UI::Xaml::Controls::IControlOverrides>::OnPointerWheelChanged(void *)",
    };

    const SP_SymbolHook hooks[] = {
        Entry(kNames, ARRAYSIZE(kNames), (void**)&g_origOnPointerWheelChanged,
              (void*)TaskbarFrame_OnPointerWheelChanged_Hook),
    };

    if (!SP_HookSymbols(hModule, hooks, ARRAYSIZE(hooks)))
    {
        SP_LogError(L"The taskbar frame's wheel handler was not found in Taskbar.View.dll on this build");
        return;
    }

    SP_Log(L"Watching the taskbar for the wheel");
}

// ---------------------------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------------------------

void LoadSettings()
{
    g_skipMinimized.store(SP_GetIntSetting(L"SkipMinimized", 1) != 0, std::memory_order_relaxed);
    g_wrapAround.store(SP_GetIntSetting(L"WrapAround", 1) != 0, std::memory_order_relaxed);
    g_reverseDirection.store(SP_GetIntSetting(L"ReverseScrollingDirection", 0) != 0, std::memory_order_relaxed);

    SP_Log(L"Skip minimized: %d, wrap around: %d, reverse: %d",
           (int)g_skipMinimized.load(std::memory_order_relaxed),
           (int)g_wrapAround.load(std::memory_order_relaxed),
           (int)g_reverseDirection.load(std::memory_order_relaxed));
}

BOOL Init()
{
    LoadSettings();

    // A minute is far longer than either library has ever taken to appear. The waits are cancelled by the engine
    // if the mod is unloaded first.
    if (!SP_WaitForModule(L"taskbar.dll", 60000, OnTaskbarDllLoaded, nullptr))
    {
        SP_LogError(L"Could not wait for taskbar.dll");
        return FALSE;
    }
    if (!SP_WaitForModule(L"Taskbar.View.dll", 60000, OnTaskbarViewLoaded, nullptr))
    {
        SP_LogError(L"Could not wait for Taskbar.View.dll");
        return FALSE;
    }

    return TRUE;
}

void Uninit()
{
    // Hooks are already gone. Nothing was changed on any live window, so there is only memory to let go of.
    g_modelReady.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lock(g_lock);
    g_activeItem.clear();
    g_lastTarget = nullptr;
}

}   // namespace

SP_MOD_DEFINE(g_modTaskbarWheelCycle) =
{
    /* id             */ SP_MOD_ID,
    /* name           */ L"Cycle through the taskbar buttons with the mouse wheel",
    /* basedOn        */ "taskbar-wheel-cycle",
    /* originalAuthor */ "m417z",
    /* targets        */ SP_TARGET_EXPLORER,
    /* minOsBuild     */ 22000,     // the XAML taskbar; the classic task list lives in taskbar.dll from here on
    /* flags          */ 0,
    /* Init           */ Init,
    /* AfterInit      */ nullptr,
    /* SettingsChanged*/ LoadSettings,
    /* BeforeUninit   */ nullptr,
    /* Uninit         */ Uninit,
};
