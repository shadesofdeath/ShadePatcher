//
// taskbar_button_click - middle click on a taskbar button closes the window instead of opening a new one.
//
// Adapted from the idea behind the Windhawk mod "Middle click to close on the taskbar" (taskbar-button-click)
// by m417z. The implementation here is written against this engine's API and keeps only the Windows 11 path;
// the Windows 10 and ExplorerPatcher paths of the original are not carried over.
//
// How it works
// ------------
// A middle click on a taskbar button is, to the shell, a request to launch a new instance of the program. The
// XAML taskbar (Taskbar.View.dll) still hands every click to the classic click logic in Taskbar.dll, and that
// is where the request can be turned into something else. The chain of calls for one click is:
//
//     CTaskListWnd::HandleClick   -> CTaskListWnd::_HandleClick   -> CTaskBand::Launch
//     (the entry point)              (knows the button, the item    (creates the new instance)
//                                     index and the click action)
//
// The first two are hooked only to remember what is being clicked; they run the original code untouched. The
// third is where the decision is made: when the click action says "launch a new instance", Shift is not held
// (Shift+click is the other way to ask for a new instance and is left alone) and the button holds one or more
// running windows, the launch is replaced with a close.
//
// Closing goes through the shell's own CTaskListWnd::ProcessJumpViewCloseWindow, the same function the "Close
// window" entry in a jump list uses, so it behaves exactly like that entry: one window when a single button was
// clicked, or every window of the group when hWnd is null. Holding a configured key (Ctrl by default) ends the
// task through CTaskBand::_EndTask instead, which is what "End task" on the taskbar does. Immersive (Store)
// apps are never ended that way because several of them can share one process.
//
// Where the state lives
// ---------------------
// The three calls above nest on the taskbar's UI thread, so what HandleClick and _HandleClick learnt is kept in
// a thread-local record that Launch reads back; no other thread ever sees it. The one piece of state that lives
// across clicks is which button is the active one in each task list, needed for the "close only the foreground
// window" option on a group; it is recorded from CTaskListWnd::_SetActiveItem under a lock.
//
// Exceptions
// ----------
// Nothing here uses C++/WinRT, so this file is compiled like the rest of the engine, without exceptions. The
// hooks only touch pointers the shell handed them and functions resolved from Taskbar.dll's PDB.
//
#define SP_MOD_ID "taskbar-button-click"
#include "engine/modapi.h"

#include <atomic>
#include <unordered_map>

namespace {

// ---------------------------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------------------------

// What a middle click on a button that holds several combined windows does.
enum class GroupBehavior
{
    CloseAll = 0,          // every window of the group is closed
    CloseForeground = 1,   // only the window that is the active one in the group
    Nothing = 2,           // the click is swallowed
};

// Bits of the EndTaskKeys setting: which modifiers must be held, and no others, for the click to end the task
// rather than close the window. 0 turns the feature off.
constexpr int kEndTaskCtrl = 1;
constexpr int kEndTaskAlt = 2;

std::atomic<GroupBehavior> g_groupBehavior{ GroupBehavior::CloseAll };
std::atomic<int>           g_endTaskKeys{ kEndTaskCtrl };

void LoadSettings()
{
    int behavior = SP_GetIntSetting(L"MultipleItemsBehavior", (int)GroupBehavior::CloseAll);
    if (behavior < (int)GroupBehavior::CloseAll || behavior > (int)GroupBehavior::Nothing)
    {
        behavior = (int)GroupBehavior::CloseAll;
    }
    g_groupBehavior.store((GroupBehavior)behavior, std::memory_order_relaxed);

    int keys = SP_GetIntSetting(L"EndTaskKeys", kEndTaskCtrl);
    keys &= (kEndTaskCtrl | kEndTaskAlt);
    g_endTaskKeys.store(keys, std::memory_order_relaxed);

    SP_Log(L"Group behavior is %d, end-task keys are %d", behavior, keys);
}

// ---------------------------------------------------------------------------------------------------------------
// The shell's functions
//
// Names are kept exactly as the PDB spells them; several spellings for one function where Windows has changed
// it. The prototypes are the ones the original mod established.
// ---------------------------------------------------------------------------------------------------------------

using CTaskListWnd_HandleClick_t = long(WINAPI*)(void* pThis, void* taskGroup, void* taskItem, void* launcherOptions);
using CTaskListWnd__HandleClick_t = void(WINAPI*)(void* pThis, void* taskBtnGroup, int taskItemIndex,
                                                  int clickAction, int a5, int a6);
using CTaskBand_Launch_t = long(WINAPI*)(void* pThis, void* taskGroup, const POINT* pt, int launchOptions);
using CTaskListWnd__SetActiveItem_t = void(WINAPI*)(void* pThis, void* taskBtnGroup, int buttonIndex);
using CTaskListWnd_ProcessJumpViewCloseWindow_t = void(WINAPI*)(void* pThis, HWND hWnd, void* taskGroup,
                                                               HMONITOR monitor);
using CTaskBand__EndTask_t = void(WINAPI*)(void* pThis, HWND hWnd, BOOL force);
using CTaskBtnGroup_GetGroupType_t = int(WINAPI*)(void* pThis);
using CTaskBtnGroup_GetGroup_t = void*(WINAPI*)(void* pThis);
using CTaskBtnGroup_GetTaskItem_t = void*(WINAPI*)(void* pThis, int index);
using CTaskItem_GetWindow_t = HWND(WINAPI*)(void* pThis);

CTaskListWnd_HandleClick_t                g_origHandleClick = nullptr;
CTaskListWnd__HandleClick_t               g_origHandleClickInner = nullptr;
CTaskBand_Launch_t                        g_origLaunch = nullptr;
CTaskListWnd__SetActiveItem_t             g_origSetActiveItem = nullptr;
CTaskListWnd_ProcessJumpViewCloseWindow_t g_ProcessJumpViewCloseWindow = nullptr;
CTaskBand__EndTask_t                      g_EndTask = nullptr;
CTaskBtnGroup_GetGroupType_t              g_GetGroupType = nullptr;
CTaskBtnGroup_GetGroup_t                  g_GetGroup = nullptr;
CTaskBtnGroup_GetTaskItem_t               g_GetTaskItem = nullptr;
CTaskItem_GetWindow_t                     g_WindowTaskItem_GetWindow = nullptr;
CTaskItem_GetWindow_t                     g_ImmersiveTaskItem_GetWindow = nullptr;
void*                                     g_ImmersiveTaskItem_vftable = nullptr;

// CTaskListWnd::eCLICKACTION: the one that means "launch a new instance". It is produced by a middle click and by
// Shift + left click.
constexpr int kClickActionLaunchNew = 3;

// CTaskBtnGroup::GetGroupType (eTBGROUPTYPE).
constexpr int kGroupTypeWindows = 1;    // one window, or several shown as separate buttons
constexpr int kGroupTypePinned = 2;     // a pinned program that is not running
constexpr int kGroupTypeCombined = 3;   // several windows behind one button

// ---------------------------------------------------------------------------------------------------------------
// The click in flight
//
// HandleClick, _HandleClick and Launch nest on the same thread, so a thread-local record is the natural place
// for what the outer calls know and the inner one needs. It is plain data with no constructor or destructor, so
// it costs nothing on threads that never click the taskbar.
// ---------------------------------------------------------------------------------------------------------------

struct ClickInFlight
{
    void* taskListWndOuter;   // `this` of HandleClick: the interface ProcessJumpViewCloseWindow wants
    void* taskListWndInner;   // `this` of _HandleClick: the object _SetActiveItem is called on
    void* taskBtnGroup;
    int   taskItemIndex;
    int   clickAction;
};

thread_local ClickInFlight t_click = {};

// Which button is active in each task list, keyed by the CTaskListWnd it belongs to. Written from
// _SetActiveItem, read from Launch; one entry per taskbar (one per monitor).
struct ActiveItem
{
    void* taskBtnGroup;
    int   buttonIndex;
};

SRWLOCK g_activeItemsLock = SRWLOCK_INIT;
std::unordered_map<void*, ActiveItem> g_activeItems;

bool IsKeyDown(int vk)
{
    return (GetKeyState(vk) & 0x8000) != 0;
}

// ---------------------------------------------------------------------------------------------------------------
// The hooks
// ---------------------------------------------------------------------------------------------------------------

long WINAPI CTaskListWnd_HandleClick_Hook(void* pThis, void* taskGroup, void* taskItem, void* launcherOptions)
{
    void* previous = t_click.taskListWndOuter;
    t_click.taskListWndOuter = pThis;

    long result = g_origHandleClick(pThis, taskGroup, taskItem, launcherOptions);

    t_click.taskListWndOuter = previous;
    return result;
}

void WINAPI CTaskListWnd__HandleClick_Hook(void* pThis, void* taskBtnGroup, int taskItemIndex, int clickAction,
                                           int a5, int a6)
{
    SP_LogDebug(L"Click action %d on item %d", clickAction, taskItemIndex);

    ClickInFlight previous = t_click;
    t_click.taskListWndInner = pThis;
    t_click.taskBtnGroup = taskBtnGroup;
    t_click.taskItemIndex = taskItemIndex;
    t_click.clickAction = clickAction;

    g_origHandleClickInner(pThis, taskBtnGroup, taskItemIndex, clickAction, a5, a6);

    t_click.taskListWndInner = previous.taskListWndInner;
    t_click.taskBtnGroup = previous.taskBtnGroup;
    t_click.taskItemIndex = previous.taskItemIndex;
    t_click.clickAction = previous.clickAction;
}

void WINAPI CTaskListWnd__SetActiveItem_Hook(void* pThis, void* taskBtnGroup, int buttonIndex)
{
    AcquireSRWLockExclusive(&g_activeItemsLock);
    g_activeItems[pThis] = ActiveItem{ taskBtnGroup, buttonIndex };
    ReleaseSRWLockExclusive(&g_activeItemsLock);

    g_origSetActiveItem(pThis, taskBtnGroup, buttonIndex);
}

// The index of the active button in a combined group, or -1 when there is none or it belongs to another group.
int ActiveIndexIn(void* taskListWnd, void* taskBtnGroup)
{
    int index = -1;

    AcquireSRWLockShared(&g_activeItemsLock);
    auto it = g_activeItems.find(taskListWnd);
    if (it != g_activeItems.end() && it->second.taskBtnGroup == taskBtnGroup)
    {
        index = it->second.buttonIndex;
    }
    ReleaseSRWLockShared(&g_activeItemsLock);

    return index;
}

long WINAPI CTaskBand_Launch_Hook(void* pThis, void* taskGroup, const POINT* pt, int launchOptions)
{
    const ClickInFlight click = t_click;

    // A launch that is not the direct result of a click on a button (a jump list entry, a pinned item started
    // from elsewhere) is none of this mod's business.
    if (!click.taskListWndOuter || !click.taskListWndInner || !click.taskBtnGroup)
    {
        return g_origLaunch(pThis, taskGroup, pt, launchOptions);
    }

    // Only the "new instance" action is turned around, and only when it came from a middle click: with Shift
    // held the same action means the user really wants another instance.
    if (click.clickAction != kClickActionLaunchNew || IsKeyDown(VK_SHIFT))
    {
        return g_origLaunch(pThis, taskGroup, pt, launchOptions);
    }

    // The group is taken from the button rather than from the argument, so a mod that rewrites the argument on
    // its way in (taskbar grouping tweaks do) still leaves this one looking at the real group.
    void* realTaskGroup = g_GetGroup(click.taskBtnGroup);
    if (!realTaskGroup)
    {
        return g_origLaunch(pThis, taskGroup, pt, launchOptions);
    }

    // A pinned program that is not running has nothing to close; the launch is what the user wants.
    const int groupType = g_GetGroupType(click.taskBtnGroup);
    if (groupType != kGroupTypeWindows && groupType != kGroupTypeCombined)
    {
        return g_origLaunch(pThis, taskGroup, pt, launchOptions);
    }

    // Which window to close. -1 means the whole group.
    int taskItemIndex = -1;

    if (groupType == kGroupTypeCombined)
    {
        switch (g_groupBehavior.load(std::memory_order_relaxed))
        {
        case GroupBehavior::Nothing:
            SP_LogDebug(L"Middle click on a group ignored by choice");
            return 0;

        case GroupBehavior::CloseForeground:
            taskItemIndex = ActiveIndexIn(click.taskListWndInner, click.taskBtnGroup);
            if (taskItemIndex < 0)
            {
                SP_LogDebug(L"No active window in the group to close");
                return 0;
            }
            break;

        case GroupBehavior::CloseAll:
        default:
            break;
        }
    }
    else
    {
        taskItemIndex = click.taskItemIndex;
    }

    // End the task instead of closing when exactly the configured modifiers are held.
    const int  wantKeys = g_endTaskKeys.load(std::memory_order_relaxed);
    const bool ctrlDown = IsKeyDown(VK_CONTROL);
    const bool altDown = IsKeyDown(VK_MENU);
    bool endTask = (ctrlDown || altDown) &&
                   ((wantKeys & kEndTaskCtrl) != 0) == ctrlDown &&
                   ((wantKeys & kEndTaskAlt) != 0) == altDown;

    HWND hWnd = nullptr;

    if (taskItemIndex >= 0)
    {
        void* taskItem = g_GetTaskItem(click.taskBtnGroup, taskItemIndex);
        if (!taskItem)
        {
            SP_LogDebug(L"No task item at index %d", taskItemIndex);
            return 0;
        }

        // Two kinds of item sit behind a button, and each answers GetWindow through its own class. The vtable
        // tells them apart, as it does in the original.
        if (*(void**)taskItem == g_ImmersiveTaskItem_vftable)
        {
            hWnd = g_ImmersiveTaskItem_GetWindow(taskItem);
            // Several Store apps can share one process, so ending it could take down more than was clicked.
            endTask = false;
        }
        else
        {
            hWnd = g_WindowTaskItem_GetWindow(taskItem);
        }
    }

    if (endTask)
    {
        if (hWnd)
        {
            SP_Log(L"Ending the task behind window %p", hWnd);
            g_EndTask(pThis, hWnd, TRUE);
        }
        else
        {
            SP_LogDebug(L"No window to end the task of");
        }
        return 0;
    }

    SP_Log(L"Closing window %p (null means the whole group)", hWnd);

    POINT cursor = {};
    GetCursorPos(&cursor);
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    g_ProcessJumpViewCloseWindow(click.taskListWndOuter, hWnd, realTaskGroup, monitor);

    return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Installing the hooks
// ---------------------------------------------------------------------------------------------------------------

BOOL InstallHooks(HMODULE hTaskbar)
{
    static const wchar_t* const kHandleClick[] =
    {
        L"public: virtual long __cdecl CTaskListWnd::HandleClick(struct ITaskGroup *,struct ITaskItem *,"
        L"struct winrt::Windows::System::LauncherOptions const &)",
    };
    static const wchar_t* const kHandleClickInner[] =
    {
        L"protected: void __cdecl CTaskListWnd::_HandleClick(struct ITaskBtnGroup *,int,"
        L"enum CTaskListWnd::eCLICKACTION,int,int)",
    };
    static const wchar_t* const kLaunch[] =
    {
        L"public: virtual long __cdecl CTaskBand::Launch(struct ITaskGroup *,struct tagPOINT const &,"
        L"enum LaunchFromTaskbarOptions)",
    };
    static const wchar_t* const kSetActiveItem[] =
    {
        L"protected: void __cdecl CTaskListWnd::_SetActiveItem(struct ITaskBtnGroup *,int)",
    };
    static const wchar_t* const kProcessJumpViewCloseWindow[] =
    {
        L"public: virtual void __cdecl CTaskListWnd::ProcessJumpViewCloseWindow(struct HWND__ *,"
        L"struct ITaskGroup *,struct HMONITOR__ *)",
    };
    static const wchar_t* const kEndTask[] =
    {
        L"protected: void __cdecl CTaskBand::_EndTask(struct HWND__ * const,int)",
        L"protected: void __thiscall CTaskBand::_EndTask(struct HWND__ * const,int)",
    };
    static const wchar_t* const kGetGroupType[] =
    {
        L"public: virtual enum eTBGROUPTYPE __cdecl CTaskBtnGroup::GetGroupType(void)",
    };
    static const wchar_t* const kGetGroup[] =
    {
        L"public: virtual struct ITaskGroup * __cdecl CTaskBtnGroup::GetGroup(void)",
    };
    static const wchar_t* const kGetTaskItem[] =
    {
        L"public: virtual struct ITaskItem * __cdecl CTaskBtnGroup::GetTaskItem(int)",
    };
    static const wchar_t* const kWindowTaskItemGetWindow[] =
    {
        L"public: virtual struct HWND__ * __cdecl CWindowTaskItem::GetWindow(void)",
    };
    static const wchar_t* const kImmersiveTaskItemGetWindow[] =
    {
        L"public: virtual struct HWND__ * __cdecl CImmersiveTaskItem::GetWindow(void)",
    };
    static const wchar_t* const kImmersiveTaskItemVftable[] =
    {
        L"const CImmersiveTaskItem::`vftable'{for `ITaskItem'}",
    };

    // Every entry is required: with any one missing the click could not be turned into a close safely, and it
    // is better for the mod to do nothing and say so than to half-work.
    SP_SymbolHook hooks[12] = {};

    hooks[0].symbols = kHandleClick;
    hooks[0].symbolCount = ARRAYSIZE(kHandleClick);
    hooks[0].pOriginal = (void**)&g_origHandleClick;
    hooks[0].hookFunction = (void*)CTaskListWnd_HandleClick_Hook;

    hooks[1].symbols = kHandleClickInner;
    hooks[1].symbolCount = ARRAYSIZE(kHandleClickInner);
    hooks[1].pOriginal = (void**)&g_origHandleClickInner;
    hooks[1].hookFunction = (void*)CTaskListWnd__HandleClick_Hook;

    hooks[2].symbols = kLaunch;
    hooks[2].symbolCount = ARRAYSIZE(kLaunch);
    hooks[2].pOriginal = (void**)&g_origLaunch;
    hooks[2].hookFunction = (void*)CTaskBand_Launch_Hook;

    hooks[3].symbols = kSetActiveItem;
    hooks[3].symbolCount = ARRAYSIZE(kSetActiveItem);
    hooks[3].pOriginal = (void**)&g_origSetActiveItem;
    hooks[3].hookFunction = (void*)CTaskListWnd__SetActiveItem_Hook;

    // The rest are only called, never intercepted.
    hooks[4].symbols = kProcessJumpViewCloseWindow;
    hooks[4].symbolCount = ARRAYSIZE(kProcessJumpViewCloseWindow);
    hooks[4].pOriginal = (void**)&g_ProcessJumpViewCloseWindow;

    hooks[5].symbols = kEndTask;
    hooks[5].symbolCount = ARRAYSIZE(kEndTask);
    hooks[5].pOriginal = (void**)&g_EndTask;

    hooks[6].symbols = kGetGroupType;
    hooks[6].symbolCount = ARRAYSIZE(kGetGroupType);
    hooks[6].pOriginal = (void**)&g_GetGroupType;

    hooks[7].symbols = kGetGroup;
    hooks[7].symbolCount = ARRAYSIZE(kGetGroup);
    hooks[7].pOriginal = (void**)&g_GetGroup;

    hooks[8].symbols = kGetTaskItem;
    hooks[8].symbolCount = ARRAYSIZE(kGetTaskItem);
    hooks[8].pOriginal = (void**)&g_GetTaskItem;

    hooks[9].symbols = kWindowTaskItemGetWindow;
    hooks[9].symbolCount = ARRAYSIZE(kWindowTaskItemGetWindow);
    hooks[9].pOriginal = (void**)&g_WindowTaskItem_GetWindow;

    hooks[10].symbols = kImmersiveTaskItemGetWindow;
    hooks[10].symbolCount = ARRAYSIZE(kImmersiveTaskItemGetWindow);
    hooks[10].pOriginal = (void**)&g_ImmersiveTaskItem_GetWindow;

    hooks[11].symbols = kImmersiveTaskItemVftable;
    hooks[11].symbolCount = ARRAYSIZE(kImmersiveTaskItemVftable);
    hooks[11].pOriginal = &g_ImmersiveTaskItem_vftable;

    if (!SP_HookSymbols(hTaskbar, hooks, ARRAYSIZE(hooks)))
    {
        SP_LogError(L"The taskbar click functions were not all found in this build; middle click is unchanged");
        return FALSE;
    }

    SP_Log(L"Middle click on taskbar buttons now closes");
    return TRUE;
}

// Runs on a helper thread once Taskbar.dll is in the process; at once when it already is.
void OnTaskbarLoaded(HMODULE hTaskbar, void*)
{
    if (!hTaskbar)
    {
        SP_LogError(L"Taskbar.dll never loaded; there is no taskbar click logic to hook");
        return;
    }
    InstallHooks(hTaskbar);
}

// ---------------------------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------------------------

BOOL Init()
{
    LoadSettings();

    // The click logic lives in Taskbar.dll, which the shell brings up with the taskbar a moment after the process
    // starts. On a cold sign-in it is not there yet when this runs, so the hooks go in when it appears.
    if (!SP_WaitForModule(L"Taskbar.dll", 60000, OnTaskbarLoaded, nullptr))
    {
        SP_LogError(L"Could not wait for Taskbar.dll");
        return FALSE;
    }

    return TRUE;
}

void Uninit()
{
    // Hooks are gone by now, so nothing can add to the map; drop what it remembered.
    AcquireSRWLockExclusive(&g_activeItemsLock);
    g_activeItems.clear();
    ReleaseSRWLockExclusive(&g_activeItemsLock);
}

}   // namespace

SP_MOD_DEFINE(g_modTaskbarButtonClick) =
{
    /* id             */ SP_MOD_ID,
    /* name           */ L"Middle click closes the window on the taskbar",
    /* basedOn        */ "taskbar-button-click",
    /* originalAuthor */ "m417z",
    /* targets        */ SP_TARGET_EXPLORER,
    /* minOsBuild     */ 22000,     // the click logic hooked here is Taskbar.dll's, which is Windows 11 only
    /* flags          */ 0,
    /* Init           */ Init,
    /* AfterInit      */ nullptr,
    /* SettingsChanged*/ LoadSettings,
    /* BeforeUninit   */ nullptr,
    /* Uninit         */ Uninit,
};
