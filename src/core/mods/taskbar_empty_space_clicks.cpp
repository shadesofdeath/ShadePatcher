//
// taskbar-empty-space-clicks - run an action when empty taskbar space is double clicked or middle clicked.
//
// Adapted from the idea behind the Windhawk mod "Click on empty taskbar space" (taskbar-empty-space-clicks)
// by m1lhaus. The implementation here is written against this engine's API and covers the Windows 11 XAML
// taskbar only.
//
// What it does
// ------------
// A double click (left button, or two touch taps) or a middle click on a part of the taskbar that holds no
// button, no tray icon and no widget runs one of a small set of actions the user picks: show the desktop, open
// Start, open Task Manager, run a command, mute the sound, or toggle the taskbar's auto-hide. Clicks on
// anything the taskbar itself reacts to are left alone. Primary and secondary taskbars both count.
//
// Where the clicks come from
// --------------------------
// On the XAML taskbar the top-level Shell_TrayWnd never sees a click. The taskbar is a XAML island, and its
// input arrives as WM_POINTERDOWN at a child window of class Windows.UI.Input.InputSite.WindowClass (under a
// Windows.UI.Composition.DesktopWindowContentBridge). That window cannot be subclassed: the input stack checks
// that its window procedure is untouched and brings the shell down otherwise. So the window procedure itself
// is hooked with an inline hook, the way the original mod does it. All InputSite windows share one procedure,
// so the hook is installed once and covers every taskbar, including a secondary one that appears later; the
// hook filters by the class of the root window the message belongs to.
//
// The tray windows are subclassed as well (SP_SetWindowSubclassFromAnyThread). That subclass carries the
// legacy mouse messages through the same pipeline, for any region the island does not cover, and gives the
// engine a way to release the UI Automation object on the taskbar's own thread at unload.
//
// Telling empty space from a button
// ---------------------------------
// A Win32 hit test only ever says "Shell_TrayWnd"; the buttons are XAML elements, not windows. UI Automation
// does know about them: IUIAutomation::ElementFromPoint returns the element under the click, and its class
// name is "Taskbar.TaskbarFrameAutomationPeer" (the frame itself) only when nothing sits on top of it. That
// is the rule the original mod uses and it is reproduced here unchanged.
//
// Double clicks
// -------------
// Pointer messages have no double-click form, so it is detected here: two left presses on the same taskbar,
// within GetDoubleClickTime and the system double-click rectangle, both on empty space. A middle click acts
// on the press. Nothing is held back from the taskbar; the message always continues to the original procedure.
//
// Threading
// ---------
// The hook body and the subclass run on the taskbar's UI thread. UI Automation is created there on first use
// and only ever used there. Actions that may block (launching a process, toggling the audio endpoints) run on
// a short-lived worker thread so the taskbar never waits on a UAC prompt. Settings are read on the engine
// thread into atomics.
//
#define SP_MOD_ID "taskbar-empty-space-clicks"
#include "engine/modapi.h"

#include <commctrl.h>
#include <windowsx.h>       // GET_X_LPARAM / GET_Y_LPARAM
#include <shellapi.h>       // SHAppBarMessage, ShellExecuteExW
#include <objbase.h>
#include <unknwn.h>
#include <UIAutomation.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>

#include <atomic>
#include <cstdlib>          // abs
#include <new>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "OleAut32.lib")

namespace {

// ---------------------------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------------------------

// The choice lists in the settings window store these indices. The order is part of the user's settings and
// must not change.
enum class Action : int
{
    Nothing        = 0,
    ShowDesktop    = 1,
    OpenStart      = 2,
    TaskManager    = 3,
    RunCommand     = 4,
    ToggleMute     = 5,
    ToggleAutoHide = 6,
    Count
};

// Modifier keys that may be required before a click counts. A bit set means the key must be held; zero means
// the modifiers are not looked at, which is what the original mod calls "None".
constexpr int kModCtrl  = 1;
constexpr int kModShift = 2;
constexpr int kModAlt   = 4;
constexpr int kModWin   = 8;

std::atomic<int> g_doubleClickAction{ (int)Action::Nothing };
std::atomic<int> g_middleClickAction{ (int)Action::Nothing };
std::atomic<int> g_doubleClickModifiers{ 0 };
std::atomic<int> g_middleClickModifiers{ 0 };

// The command behind Action::RunCommand. Written on the engine thread, copied out on the taskbar thread.
constexpr DWORD kCommandLength = 1024;
SRWLOCK  g_commandLock = SRWLOCK_INIT;
wchar_t  g_command[kCommandLength] = {};

Action ClampAction(int value)
{
    if (value < 0 || value >= (int)Action::Count)
    {
        return Action::Nothing;
    }
    return (Action)value;
}

void LoadSettings()
{
    g_doubleClickAction.store((int)ClampAction(SP_GetIntSetting(L"DoubleClickAction", 0)), std::memory_order_relaxed);
    g_middleClickAction.store((int)ClampAction(SP_GetIntSetting(L"MiddleClickAction", 0)), std::memory_order_relaxed);

    // Optional, not shown in the settings window: a bitmask of kMod* values.
    g_doubleClickModifiers.store(SP_GetIntSetting(L"DoubleClickModifier", 0) & 0xF, std::memory_order_relaxed);
    g_middleClickModifiers.store(SP_GetIntSetting(L"MiddleClickModifier", 0) & 0xF, std::memory_order_relaxed);

    wchar_t command[kCommandLength];
    SP_GetStringSetting(L"CustomCommand", command, kCommandLength, L"");

    AcquireSRWLockExclusive(&g_commandLock);
    wcscpy_s(g_command, kCommandLength, command);
    ReleaseSRWLockExclusive(&g_commandLock);

    SP_Log(L"Double click -> %d, middle click -> %d, command \"%s\"",
           g_doubleClickAction.load(std::memory_order_relaxed),
           g_middleClickAction.load(std::memory_order_relaxed), command);
}

// ---------------------------------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------------------------------

// A minimal owning COM pointer. No exceptions, no dependencies, just Release on the way out.
template <typename T>
class ComRef
{
public:
    ComRef() = default;
    ~ComRef() { Reset(); }
    ComRef(const ComRef&) = delete;
    ComRef& operator=(const ComRef&) = delete;

    T** Put() { Reset(); return &m_ptr; }
    void** PutVoid() { Reset(); return reinterpret_cast<void**>(&m_ptr); }
    T* Get() const { return m_ptr; }
    T* operator->() const { return m_ptr; }
    explicit operator bool() const { return m_ptr != nullptr; }

    T* Detach() { T* p = m_ptr; m_ptr = nullptr; return p; }
    void Reset()
    {
        if (m_ptr)
        {
            m_ptr->Release();
            m_ptr = nullptr;
        }
    }

private:
    T* m_ptr = nullptr;
};

bool IsClass(HWND hWnd, const wchar_t* className)
{
    wchar_t wszClass[64];
    return hWnd && GetClassNameW(hWnd, wszClass, ARRAYSIZE(wszClass)) && _wcsicmp(wszClass, className) == 0;
}

bool IsTaskbarWindow(HWND hWnd)
{
    return IsClass(hWnd, L"Shell_TrayWnd") || IsClass(hWnd, L"Shell_SecondaryTrayWnd");
}

bool IsOurProcess(HWND hWnd)
{
    DWORD processId = 0;
    return hWnd && GetWindowThreadProcessId(hWnd, &processId) && processId == GetCurrentProcessId();
}

HWND FindPrimaryTaskbar()
{
    HWND hTray = FindWindowW(L"Shell_TrayWnd", nullptr);
    return IsOurProcess(hTray) ? hTray : nullptr;
}

// The XAML island's input window: Shell_TrayWnd -> DesktopWindowContentBridge -> InputSite.
HWND FindInputSite(HWND hTaskbar)
{
    HWND hBridge = FindWindowExW(hTaskbar, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr);
    if (!hBridge)
    {
        return nullptr;
    }
    return FindWindowExW(hBridge, nullptr, L"Windows.UI.Input.InputSite.WindowClass", nullptr);
}

// Which modifier keys are down right now. GetAsyncKeyState rather than the message's key state, because the
// pointer message does not carry one.
int CurrentModifiers()
{
    int held = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) held |= kModCtrl;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)   held |= kModShift;
    if (GetAsyncKeyState(VK_MENU) & 0x8000)    held |= kModAlt;
    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000)) held |= kModWin;
    return held;
}

// Presses and releases one key. Used only as the fallback for opening Start.
void TapKey(WORD vk)
{
    INPUT input[2] = {};
    input[0].type = INPUT_KEYBOARD;
    input[0].ki.wVk = vk;
    input[1].type = INPUT_KEYBOARD;
    input[1].ki.wVk = vk;
    input[1].ki.dwFlags = KEYEVENTF_KEYUP;
    if (SendInput(2, input, sizeof(INPUT)) != 2)
    {
        SP_LogError(L"The key press could not be sent: %lu", GetLastError());
    }
}

// ---------------------------------------------------------------------------------------------------------------
// UI Automation
//
// One IUIAutomation object, created on the taskbar's thread the first time a click needs it and released there
// at unload (through a message to the subclassed tray window). It is never used from any other thread.
// ---------------------------------------------------------------------------------------------------------------

IUIAutomation* g_uia = nullptr;

IUIAutomation* GetAutomation()
{
    if (g_uia)
    {
        return g_uia;
    }

    IUIAutomation* uia = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                                  __uuidof(IUIAutomation), reinterpret_cast<void**>(&uia));
    if (hr == CO_E_NOTINITIALIZED)
    {
        // The taskbar's thread always has COM up already; this is only here so a surprise never leaves the mod
        // dead. The apartment is deliberately not torn down again: it is the shell's thread.
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                              __uuidof(IUIAutomation), reinterpret_cast<void**>(&uia));
    }

    if (FAILED(hr) || !uia)
    {
        SP_LogError(L"UI Automation could not be created: 0x%08X", (unsigned)hr);
        return nullptr;
    }

    g_uia = uia;
    SP_LogDebug(L"UI Automation is ready on thread %lu", GetCurrentThreadId());
    return g_uia;
}

void ReleaseAutomation()
{
    if (IUIAutomation* uia = g_uia)
    {
        g_uia = nullptr;
        uia->Release();
    }
}

// TRUE when the element under the point is the taskbar frame itself rather than something placed on it.
bool IsEmptyTaskbarSpace(POINT ptScreen)
{
    IUIAutomation* uia = GetAutomation();
    if (!uia)
    {
        return false;
    }

    ComRef<IUIAutomationElement> element;
    HRESULT hr = uia->ElementFromPoint(ptScreen, element.Put());
    if (FAILED(hr) || !element)
    {
        SP_LogDebug(L"No automation element at (%ld, %ld): 0x%08X", ptScreen.x, ptScreen.y, (unsigned)hr);
        return false;
    }

    BSTR className = nullptr;
    if (FAILED(element->get_CurrentClassName(&className)) || !className)
    {
        return false;
    }

    const bool empty =
        wcscmp(className, L"Taskbar.TaskbarFrameAutomationPeer") == 0 ||     // the XAML taskbar frame
        wcscmp(className, L"Windows.UI.Input.InputSite.WindowClass") == 0 ||  // 21H2 reports the island window
        wcscmp(className, L"Shell_TrayWnd") == 0 ||                           // the tray window itself
        wcscmp(className, L"Shell_SecondaryTrayWnd") == 0;

    SP_LogDebug(L"Under the click: %s%s", className, empty ? L" (empty space)" : L"");
    SysFreeString(className);
    return empty;
}

// ---------------------------------------------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------------------------------------------

// 407 is the tray's own "toggle desktop" command, the one behind the Show desktop corner. Posted rather than
// sent, so it is safe from whichever thread the click came in on.
void ShowDesktop()
{
    HWND hTray = FindPrimaryTaskbar();
    if (!hTray)
    {
        SP_LogError(L"The taskbar window was not found; the desktop cannot be shown");
        return;
    }
    if (!PostMessageW(hTray, WM_COMMAND, MAKEWPARAM(407, 0), 0))
    {
        SP_LogError(L"The show-desktop command could not be posted: %lu", GetLastError());
    }
}

// The Start button is a XAML element; UI Automation can toggle it, which is what the original mod does. From
// the element under the click (the taskbar frame) the button is the descendant whose automation id is
// "StartButton".
bool ToggleStartButton(POINT ptScreen)
{
    IUIAutomation* uia = GetAutomation();
    if (!uia)
    {
        return false;
    }

    ComRef<IUIAutomationElement> frame;
    if (FAILED(uia->ElementFromPoint(ptScreen, frame.Put())) || !frame)
    {
        return false;
    }

    VARIANT id;
    VariantInit(&id);
    id.vt = VT_BSTR;
    id.bstrVal = SysAllocString(L"StartButton");
    if (!id.bstrVal)
    {
        return false;
    }

    ComRef<IUIAutomationCondition> condition;
    HRESULT hr = uia->CreatePropertyCondition(UIA_AutomationIdPropertyId, id, condition.Put());
    VariantClear(&id);
    if (FAILED(hr) || !condition)
    {
        return false;
    }

    ComRef<IUIAutomationElement> button;
    if (FAILED(frame->FindFirst(TreeScope_Descendants, condition.Get(), button.Put())) || !button)
    {
        SP_LogDebug(L"No Start button under the taskbar frame");
        return false;
    }

    ComRef<IUIAutomationTogglePattern> toggle;
    if (FAILED(button->GetCurrentPatternAs(UIA_TogglePatternId, __uuidof(IUIAutomationTogglePattern), toggle.PutVoid())) ||
        !toggle)
    {
        SP_LogDebug(L"The Start button has no toggle pattern");
        return false;
    }

    return SUCCEEDED(toggle->Toggle());
}

void OpenStartMenu(POINT ptScreen)
{
    if (ToggleStartButton(ptScreen))
    {
        return;
    }
    // The button may be hidden by another mod; the key still works.
    SP_LogDebug(L"Falling back to the Windows key");
    TapKey(VK_LWIN);
}

void ToggleTaskbarAutoHide()
{
    HWND hTray = FindPrimaryTaskbar();
    if (!hTray)
    {
        SP_LogError(L"The taskbar window was not found; auto-hide cannot be toggled");
        return;
    }

    APPBARDATA data = {};
    data.cbSize = sizeof(data);
    data.hWnd = hTray;
    const UINT state = (UINT)SHAppBarMessage(ABM_GETSTATE, &data);
    const bool autoHide = (state & ABS_AUTOHIDE) != 0;

    data.lParam = autoHide ? 0 : ABS_AUTOHIDE;
    SHAppBarMessage(ABM_SETSTATE, &data);
    SP_Log(L"Taskbar auto-hide is now %s", autoHide ? L"off" : L"on");
}

// --- Worker-thread actions ------------------------------------------------------------------------------------

// Flips the mute state of every active playback device, using the default one as the reference, so a mixed
// state ends up uniform rather than swapped device by device.
void ToggleMute()
{
    ComRef<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                  __uuidof(IMMDeviceEnumerator), enumerator.PutVoid());
    if (FAILED(hr) || !enumerator)
    {
        SP_LogError(L"The audio device enumerator could not be created: 0x%08X", (unsigned)hr);
        return;
    }

    BOOL muted = FALSE;
    {
        ComRef<IMMDevice> defaultDevice;
        ComRef<IAudioEndpointVolume> volume;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, defaultDevice.Put())) && defaultDevice &&
            SUCCEEDED(defaultDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr, volume.PutVoid())) &&
            volume)
        {
            volume->GetMute(&muted);
        }
        else
        {
            SP_LogError(L"There is no default playback device to read the mute state from");
            return;
        }
    }

    ComRef<IMMDeviceCollection> devices;
    UINT count = 0;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, devices.Put())) || !devices ||
        FAILED(devices->GetCount(&count)))
    {
        SP_LogError(L"The playback devices could not be listed");
        return;
    }

    int changed = 0;
    for (UINT i = 0; i < count; ++i)
    {
        ComRef<IMMDevice> device;
        ComRef<IAudioEndpointVolume> volume;
        if (SUCCEEDED(devices->Item(i, device.Put())) && device &&
            SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr, volume.PutVoid())) &&
            volume && SUCCEEDED(volume->SetMute(!muted, nullptr)))
        {
            changed++;
        }
    }

    SP_Log(L"Sound %s on %d playback device(s)", muted ? L"unmuted" : L"muted", changed);
}

void OpenTaskManager()
{
    wchar_t wszPath[MAX_PATH];
    UINT n = GetSystemDirectoryW(wszPath, ARRAYSIZE(wszPath));
    if (n == 0 || n >= ARRAYSIZE(wszPath) - 12)
    {
        return;
    }
    wcscat_s(wszPath, ARRAYSIZE(wszPath), L"\\Taskmgr.exe");

    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"open";
    sei.lpFile = wszPath;
    sei.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei))
    {
        DWORD error = GetLastError();
        if (error != ERROR_CANCELLED)   // the user said no to the elevation prompt
        {
            SP_LogError(L"Task Manager could not be started: %lu", error);
        }
    }
}

bool PathExists(const wchar_t* path)
{
    return path[0] && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

// Splits the user's command into the thing to open and its parameters.
//
//   "C:\Program Files\App\app.exe" file.txt   quoted: the quotes decide
//   C:\Program Files\App\app.exe file.txt     unquoted: the shortest prefix that exists on disk is the file
//   cmd.exe /c echo hi                        nothing on disk matches: the first word is the file
//   https://example.com                       a single token is used whole
//
// The command has had its environment variables expanded already.
void SplitCommand(const wchar_t* command, wchar_t* file, DWORD cchFile, wchar_t* params, DWORD cchParams)
{
    file[0] = 0;
    params[0] = 0;

    // Leading blanks.
    while (*command == L' ' || *command == L'\t')
    {
        command++;
    }

    if (command[0] == L'"' || command[0] == L'\'')
    {
        const wchar_t quote = command[0];
        const wchar_t* close = wcschr(command + 1, quote);
        if (close)
        {
            size_t len = (size_t)(close - (command + 1));
            if (len >= cchFile) len = cchFile - 1;
            wcsncpy_s(file, cchFile, command + 1, len);

            const wchar_t* rest = close + 1;
            while (*rest == L' ' || *rest == L'\t') rest++;
            wcscpy_s(params, cchParams, rest);
            return;
        }
        // No closing quote: fall through and treat the text as unquoted.
    }

    // Try each prefix that ends before a blank, shortest first, then the whole string.
    for (const wchar_t* p = command; ; ++p)
    {
        if (*p == L' ' || *p == L'\t' || *p == 0)
        {
            size_t len = (size_t)(p - command);
            if (len > 0 && len < cchFile)
            {
                wcsncpy_s(file, cchFile, command, len);
                if (PathExists(file))
                {
                    const wchar_t* rest = p;
                    while (*rest == L' ' || *rest == L'\t') rest++;
                    wcscpy_s(params, cchParams, rest);
                    return;
                }
            }
        }
        if (*p == 0)
        {
            break;
        }
    }

    // Nothing on disk: first word, then the rest.
    const wchar_t* p = command;
    while (*p && *p != L' ' && *p != L'\t') p++;
    size_t len = (size_t)(p - command);
    if (len >= cchFile) len = cchFile - 1;
    wcsncpy_s(file, cchFile, command, len);
    while (*p == L' ' || *p == L'\t') p++;
    wcscpy_s(params, cchParams, p);
}

// Runs the user's command through ShellExecute, so a path, a folder, a URL or a shell: name all work. A leading
// "uac;" asks for elevation, as in the original mod.
void RunCommand(const wchar_t* rawCommand)
{
    wchar_t expanded[kCommandLength * 2];
    if (!ExpandEnvironmentStringsW(rawCommand, expanded, ARRAYSIZE(expanded)))
    {
        wcscpy_s(expanded, ARRAYSIZE(expanded), rawCommand);
    }

    const wchar_t* command = expanded;
    while (*command == L' ' || *command == L'\t')
    {
        command++;
    }
    if (!*command)
    {
        SP_Log(L"No command is set; nothing to run");
        return;
    }

    const wchar_t* verb = L"open";
    if (_wcsnicmp(command, L"uac;", 4) == 0)
    {
        verb = L"runas";
        command += 4;
    }

    wchar_t file[kCommandLength * 2];
    wchar_t params[kCommandLength * 2];
    SplitCommand(command, file, ARRAYSIZE(file), params, ARRAYSIZE(params));
    if (!file[0])
    {
        return;
    }

    // Launch on the monitor the click happened on; most programs ignore it, but the ones that listen open where
    // the user is looking.
    POINT ptCursor = {};
    GetCursorPos(&ptCursor);

    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI | SEE_MASK_HMONITOR;
    sei.lpVerb = verb;
    sei.lpFile = file;
    sei.lpParameters = params[0] ? params : nullptr;
    sei.nShow = SW_SHOWNORMAL;
    sei.hMonitor = MonitorFromPoint(ptCursor, MONITOR_DEFAULTTONEAREST);

    SP_Log(L"Running \"%s\" %s", file, params);
    if (!ShellExecuteExW(&sei))
    {
        DWORD error = GetLastError();
        if (error != ERROR_CANCELLED)
        {
            SP_LogError(L"The command could not be run (error %lu): %s", error, file);
        }
    }
}

// --- The worker thread ----------------------------------------------------------------------------------------

struct WorkItem
{
    Action  action;
    wchar_t command[kCommandLength];
};

// How many workers are alive, so unloading can wait for them rather than pull the code out from under one.
std::atomic<int> g_workers{ 0 };

DWORD WINAPI ActionThread(LPVOID parameter)
{
    WorkItem* item = (WorkItem*)parameter;

    // ShellExecute and the audio API both want an apartment; a fresh thread has none.
    const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    switch (item->action)
    {
        case Action::TaskManager: OpenTaskManager(); break;
        case Action::RunCommand:  RunCommand(item->command); break;
        case Action::ToggleMute:  ToggleMute(); break;
        default: break;
    }

    if (SUCCEEDED(hrInit))
    {
        CoUninitialize();
    }

    delete item;
    g_workers.fetch_sub(1, std::memory_order_acq_rel);
    return 0;
}

void RunOnWorker(Action action)
{
    WorkItem* item = new (std::nothrow) WorkItem();
    if (!item)
    {
        return;
    }
    item->action = action;

    AcquireSRWLockShared(&g_commandLock);
    wcscpy_s(item->command, kCommandLength, g_command);
    ReleaseSRWLockShared(&g_commandLock);

    g_workers.fetch_add(1, std::memory_order_acq_rel);
    HANDLE hThread = CreateThread(nullptr, 0, ActionThread, item, 0, nullptr);
    if (!hThread)
    {
        g_workers.fetch_sub(1, std::memory_order_acq_rel);
        delete item;
        SP_LogError(L"No thread for the action: %lu", GetLastError());
        return;
    }
    CloseHandle(hThread);
}

// --- Dispatch -------------------------------------------------------------------------------------------------

const wchar_t* ActionName(Action action)
{
    switch (action)
    {
        case Action::ShowDesktop:    return L"show desktop";
        case Action::OpenStart:      return L"open Start";
        case Action::TaskManager:    return L"Task Manager";
        case Action::RunCommand:     return L"run command";
        case Action::ToggleMute:     return L"toggle mute";
        case Action::ToggleAutoHide: return L"toggle auto-hide";
        default:                     return L"nothing";
    }
}

void RunAction(Action action, POINT ptScreen, const wchar_t* trigger)
{
    SP_Log(L"%s on empty taskbar space: %s", trigger, ActionName(action));

    switch (action)
    {
        case Action::ShowDesktop:    ShowDesktop(); break;
        case Action::OpenStart:      OpenStartMenu(ptScreen); break;
        case Action::ToggleAutoHide: ToggleTaskbarAutoHide(); break;

        case Action::TaskManager:
        case Action::RunCommand:
        case Action::ToggleMute:
            RunOnWorker(action);
            break;

        default:
            break;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Click detection
// ---------------------------------------------------------------------------------------------------------------

enum class Button
{
    None,
    Left,
    Middle,
};

// The last left press on empty space, for pairing into a double click. Touched from the taskbar thread only in
// practice; the lock is there so the legacy fallback path can never race the pointer path.
struct LastPress
{
    HWND  hRoot = nullptr;
    POINT pt = {};
    DWORD tick = 0;
    bool  touch = false;
};

SRWLOCK   g_pressLock = SRWLOCK_INIT;
LastPress g_lastPress;

void ForgetLastPress()
{
    AcquireSRWLockExclusive(&g_pressLock);
    g_lastPress = LastPress();
    ReleaseSRWLockExclusive(&g_pressLock);
}

// TRUE when `pt` on `hRoot` completes a double click with the remembered press. Either way the record is
// updated: a completed pair is cleared so a third click starts over, anything else becomes the new first press.
bool RegisterLeftPress(HWND hRoot, POINT pt, bool touch)
{
    const DWORD now = GetTickCount();

    // The system rectangle is meant for a mouse; a finger needs more room.
    const int dpi = (int)GetDpiForWindow(hRoot);
    const int cx = touch ? MulDiv(15, dpi, 96) : MulDiv(GetSystemMetrics(SM_CXDOUBLECLK), dpi, 96);
    const int cy = touch ? MulDiv(15, dpi, 96) : MulDiv(GetSystemMetrics(SM_CYDOUBLECLK), dpi, 96);

    AcquireSRWLockExclusive(&g_pressLock);

    const bool paired =
        g_lastPress.hRoot == hRoot &&
        g_lastPress.touch == touch &&
        (now - g_lastPress.tick) <= GetDoubleClickTime() &&
        abs(pt.x - g_lastPress.pt.x) <= cx &&
        abs(pt.y - g_lastPress.pt.y) <= cy;

    if (paired)
    {
        g_lastPress = LastPress();
    }
    else
    {
        g_lastPress.hRoot = hRoot;
        g_lastPress.pt = pt;
        g_lastPress.tick = now;
        g_lastPress.touch = touch;
    }

    ReleaseSRWLockExclusive(&g_pressLock);
    return paired;
}

bool ModifiersAllow(int required, const wchar_t* trigger)
{
    if (required == 0)
    {
        return true;
    }
    const int held = CurrentModifiers();
    if (held != required)
    {
        SP_LogDebug(L"%s ignored: modifiers held %d, required %d", trigger, held, required);
        return false;
    }
    return true;
}

// The one entry point for a button press on a taskbar, from the pointer hook or the legacy subclass.
// Runs on the taskbar's thread. Must never throw and never block for long.
void OnTaskbarPress(HWND hRoot, Button button, POINT ptScreen, bool touch)
{
    if (button == Button::None)
    {
        return;
    }

    const Action doubleAction = ClampAction(g_doubleClickAction.load(std::memory_order_relaxed));
    const Action middleAction = ClampAction(g_middleClickAction.load(std::memory_order_relaxed));

    // Nothing to do for this button: no UI Automation call, no cost.
    if ((button == Button::Left && doubleAction == Action::Nothing) ||
        (button == Button::Middle && middleAction == Action::Nothing))
    {
        return;
    }

    // A window holding the capture means a drag or a menu is in progress, not a click on the bar.
    if (GetCapture())
    {
        return;
    }

    if (!IsEmptyTaskbarSpace(ptScreen))
    {
        // A click on a button breaks a double click in progress.
        ForgetLastPress();
        return;
    }

    if (button == Button::Middle)
    {
        if (ModifiersAllow(g_middleClickModifiers.load(std::memory_order_relaxed), L"Middle click"))
        {
            RunAction(middleAction, ptScreen, L"Middle click");
        }
        return;
    }

    if (RegisterLeftPress(hRoot, ptScreen, touch))
    {
        if (ModifiersAllow(g_doubleClickModifiers.load(std::memory_order_relaxed), L"Double click"))
        {
            RunAction(doubleAction, ptScreen, touch ? L"Double tap" : L"Double click");
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// The tray window subclass
//
// Carries the legacy mouse messages (for whatever the island does not cover), notices the window going away and
// answers the release request from BeforeUninit on the taskbar's own thread.
// ---------------------------------------------------------------------------------------------------------------

constexpr UINT_PTR kSubclassId = 0x53504563;   // 'SPEc'

UINT g_releaseMessage = 0;

// The tray windows that carry the subclass, so it can be taken off again at unload.
constexpr int kMaxTaskbars = 16;
SRWLOCK g_taskbarsLock = SRWLOCK_INIT;
HWND    g_taskbars[kMaxTaskbars] = {};

bool RememberTaskbar(HWND hWnd)
{
    AcquireSRWLockExclusive(&g_taskbarsLock);
    bool stored = false;
    for (int i = 0; i < kMaxTaskbars; ++i)
    {
        if (g_taskbars[i] == hWnd)
        {
            stored = true;
            break;
        }
    }
    if (!stored)
    {
        for (int i = 0; i < kMaxTaskbars; ++i)
        {
            if (!g_taskbars[i])
            {
                g_taskbars[i] = hWnd;
                stored = true;
                break;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_taskbarsLock);
    return stored;
}

bool IsRememberedTaskbar(HWND hWnd)
{
    AcquireSRWLockShared(&g_taskbarsLock);
    bool found = false;
    for (int i = 0; i < kMaxTaskbars; ++i)
    {
        if (g_taskbars[i] == hWnd)
        {
            found = true;
            break;
        }
    }
    ReleaseSRWLockShared(&g_taskbarsLock);
    return found;
}

void ForgetTaskbar(HWND hWnd)
{
    AcquireSRWLockExclusive(&g_taskbarsLock);
    for (int i = 0; i < kMaxTaskbars; ++i)
    {
        if (g_taskbars[i] == hWnd)
        {
            g_taskbars[i] = nullptr;
        }
    }
    ReleaseSRWLockExclusive(&g_taskbarsLock);
}

LRESULT CALLBACK TaskbarSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                     UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    UNREFERENCED_PARAMETER(uIdSubclass);
    UNREFERENCED_PARAMETER(dwRefData);
    UNREFERENCED_PARAMETER(wParam);
    UNREFERENCED_PARAMETER(lParam);

    switch (uMsg)
    {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONDBLCLK:
        {
            // Only reached for a part of the bar the XAML island does not cover; the island's own clicks come
            // through the pointer hook. Both message forms are fed in as a press: the pairing is done here.
            POINT pt = {};
            GetCursorPos(&pt);
            const Button button = (uMsg == WM_MBUTTONDOWN || uMsg == WM_MBUTTONDBLCLK) ? Button::Middle : Button::Left;
            OnTaskbarPress(hWnd, button, pt, false);
            break;
        }

        case WM_NCDESTROY:
            ForgetTaskbar(hWnd);
            break;

        default:
            if (g_releaseMessage && uMsg == g_releaseMessage)
            {
                ReleaseAutomation();
                return 0;
            }
            break;
    }

    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

// Installs the subclass from any thread. Returns TRUE when the window carries it afterwards.
bool WatchTaskbar(HWND hWnd)
{
    if (!hWnd || !IsWindow(hWnd) || !IsOurProcess(hWnd))
    {
        return false;
    }
    if (IsRememberedTaskbar(hWnd))
    {
        return true;
    }

    const bool sameThread = GetWindowThreadProcessId(hWnd, nullptr) == GetCurrentThreadId();
    const BOOL ok = sameThread
        ? SetWindowSubclass(hWnd, TaskbarSubclassProc, kSubclassId, 0)
        : SP_SetWindowSubclassFromAnyThread(hWnd, TaskbarSubclassProc, kSubclassId, 0);

    if (!ok)
    {
        SP_LogError(L"The taskbar window %p could not be subclassed", hWnd);
        return false;
    }

    RememberTaskbar(hWnd);
    SP_Log(L"Watching taskbar window %p", hWnd);
    return true;
}

void UnwatchTaskbars()
{
    HWND windows[kMaxTaskbars];
    AcquireSRWLockExclusive(&g_taskbarsLock);
    for (int i = 0; i < kMaxTaskbars; ++i)
    {
        windows[i] = g_taskbars[i];
        g_taskbars[i] = nullptr;
    }
    ReleaseSRWLockExclusive(&g_taskbarsLock);

    for (int i = 0; i < kMaxTaskbars; ++i)
    {
        if (windows[i] && IsWindow(windows[i]))
        {
            SP_RemoveWindowSubclassFromAnyThread(windows[i], TaskbarSubclassProc, kSubclassId);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// The InputSite hook
// ---------------------------------------------------------------------------------------------------------------

using WndProc_t = LRESULT(CALLBACK*)(HWND, UINT, WPARAM, LPARAM);

WndProc_t          g_origInputSiteProc = nullptr;
std::atomic<void*> g_hookedInputSiteProc{ nullptr };

Button ButtonFromPointer(WPARAM wParam)
{
    if (IS_POINTER_FIRSTBUTTON_WPARAM(wParam))
    {
        return Button::Left;
    }
    if (IS_POINTER_THIRDBUTTON_WPARAM(wParam))
    {
        return Button::Middle;
    }
    return Button::None;
}

bool PointerIsTouch(WPARAM wParam)
{
    const UINT32 pointerId = GET_POINTERID_WPARAM(wParam);
    if (pointerId == 0)
    {
        return false;   // synthesized from a mouse
    }
    POINTER_INPUT_TYPE type = PT_POINTER;
    if (!GetPointerType(pointerId, &type))
    {
        return false;
    }
    return type == PT_TOUCH || type == PT_PEN;
}

LRESULT CALLBACK InputSiteProc_Hook(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (uMsg == WM_POINTERDOWN)
    {
        // The same procedure serves every XAML island in the shell (Start, search, widgets...). Only the two
        // taskbar classes are of interest.
        HWND hRoot = GetAncestor(hWnd, GA_ROOT);
        if (hRoot && IsTaskbarWindow(hRoot))
        {
            // A secondary taskbar that appeared after start-up is picked up here, on its own thread.
            WatchTaskbar(hRoot);

            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };   // screen coordinates
            OnTaskbarPress(hRoot, ButtonFromPointer(wParam), pt, PointerIsTouch(wParam));
        }
    }

    return g_origInputSiteProc(hWnd, uMsg, wParam, lParam);
}

bool HookInputSite(HWND hInputSite)
{
    void* proc = (void*)GetWindowLongPtrW(hInputSite, GWLP_WNDPROC);
    if (!proc)
    {
        SP_LogError(L"The InputSite window procedure could not be read: %lu", GetLastError());
        return false;
    }

    void* already = g_hookedInputSiteProc.load(std::memory_order_acquire);
    if (already == proc)
    {
        return true;
    }
    if (already)
    {
        // Another window with a different procedure. One hook is all this mod keeps; say so and carry on with
        // the one that is in place.
        SP_LogDebug(L"InputSite %p uses procedure %p, the hook is on %p", hInputSite, proc, already);
        return true;
    }

    if (!SP_SetFunctionHookNow(proc, InputSiteProc_Hook, &g_origInputSiteProc))
    {
        SP_LogError(L"The InputSite window procedure %p could not be hooked", proc);
        return false;
    }

    g_hookedInputSiteProc.store(proc, std::memory_order_release);
    SP_Log(L"Hooked the taskbar's InputSite window procedure %p", proc);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Attaching to the taskbar
// ---------------------------------------------------------------------------------------------------------------

std::atomic<bool> g_stopping{ false };

BOOL CALLBACK CollectSecondaryTaskbars(HWND hWnd, LPARAM)
{
    if (IsOurProcess(hWnd) && IsClass(hWnd, L"Shell_SecondaryTrayWnd"))
    {
        WatchTaskbar(hWnd);
    }
    return TRUE;
}

// One attempt. TRUE once the pointer hook is in place, which is the part that cannot wait: the subclasses are
// a fallback and are also picked up lazily from the hook.
bool AttachTaskbar()
{
    HWND hTray = FindPrimaryTaskbar();
    if (!hTray)
    {
        return false;
    }

    HWND hInputSite = FindInputSite(hTray);
    if (!hInputSite)
    {
        return false;   // the island is not built yet
    }

    if (!HookInputSite(hInputSite))
    {
        return false;
    }

    WatchTaskbar(hTray);
    EnumWindows(CollectSecondaryTaskbars, 0);
    return true;
}

// Runs on a worker thread of its own (never the loading thread: this polls for up to a minute) once Taskbar.View.dll is loaded. The library is loaded a moment before the
// tray window gets its island, so this keeps looking for a while, and stops at once when the mod unloads.
void OnTaskbarModuleLoaded(HMODULE, void*)
{
    for (int attempt = 0; attempt < 240; ++attempt)   // one minute
    {
        if (g_stopping.load(std::memory_order_relaxed))
        {
            return;
        }
        if (AttachTaskbar())
        {
            return;
        }
        Sleep(250);
    }
    SP_LogError(L"The taskbar's input window never appeared; clicks will not be seen");
}

// ---------------------------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------------------------

BOOL Init()
{
    LoadSettings();
    g_stopping.store(false, std::memory_order_relaxed);

    if (!g_releaseMessage)
    {
        g_releaseMessage = RegisterWindowMessageW(L"ShadePatcher.TaskbarEmptySpaceClicks.Release");
    }

    if (!SP_WaitForModuleOnWorker(L"Taskbar.View.dll", 60000, OnTaskbarModuleLoaded, nullptr))
    {
        SP_LogError(L"The wait for the taskbar could not be set up");
        return FALSE;
    }
    return TRUE;
}

void BeforeUninit()
{
    g_stopping.store(true, std::memory_order_relaxed);

    // The UI Automation object was made on the taskbar's thread; ask that thread to let it go. If the taskbar
    // is gone or busy the object is released from here: CUIAutomation is a free-threaded in-process object.
    HWND hTray = FindPrimaryTaskbar();
    if (hTray && g_releaseMessage && IsRememberedTaskbar(hTray))
    {
        DWORD_PTR ignored = 0;
        SendMessageTimeoutW(hTray, g_releaseMessage, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 3000, &ignored);
    }
    ReleaseAutomation();

    UnwatchTaskbars();
    ForgetLastPress();
}

void Uninit()
{
    // A worker still inside ShellExecute (a UAC prompt, say) must finish before this code goes away.
    for (int i = 0; i < 50 && g_workers.load(std::memory_order_acquire) > 0; ++i)
    {
        Sleep(100);
    }
    if (g_workers.load(std::memory_order_acquire) > 0)
    {
        SP_LogError(L"An action is still running while the mod unloads");
    }

    g_hookedInputSiteProc.store(nullptr, std::memory_order_release);
    g_origInputSiteProc = nullptr;
}

}   // namespace

SP_MOD_DEFINE(g_modTaskbarEmptySpaceClicks) =
{
    /* id             */ SP_MOD_ID,
    /* name           */ L"Run an action when empty taskbar space is clicked",
    /* basedOn        */ "taskbar-empty-space-clicks",
    /* originalAuthor */ "m1lhaus",
    /* targets        */ SP_TARGET_EXPLORER,
    /* minOsBuild     */ 22000,
    /* flags          */ 0,
    /* Init           */ Init,
    /* AfterInit      */ nullptr,
    /* SettingsChanged*/ LoadSettings,
    /* BeforeUninit   */ BeforeUninit,
    /* Uninit         */ Uninit,
};
