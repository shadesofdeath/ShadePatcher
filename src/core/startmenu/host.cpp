//
// host.cpp - see host.h.
//
#include "host.h"

#include <windowsx.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <initializer_list>

#include "engine/log.h"

namespace sm {

namespace {

constexpr char kTag[] = "custom-start-menu";

// Settings, read by the hooks on the taskbar and Progman threads.
std::atomic<bool> g_winKey{ true };
std::atomic<bool> g_startButton{ true };
std::atomic<bool> g_shiftClickWindows{ true };
std::atomic<int> g_middleClick{ 0 };
std::atomic<bool> g_fullscreenGuard{ true };

// A full-screen game, video or presentation is in front: the Windows key should not pull the menu over it.
bool FullscreenInFront()
{
    QUERY_USER_NOTIFICATION_STATE state = QUNS_ACCEPTS_NOTIFICATIONS;
    if (FAILED(SHQueryUserNotificationState(&state)))
    {
        return false;
    }
    return state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_PRESENTATION_MODE || state == QUNS_BUSY;
}

std::atomic<HWND> g_menuWindow{ nullptr };

HANDLE g_thread = nullptr;
DWORD g_threadId = 0;
HANDLE g_ready = nullptr;
bool g_started = false;
HostSettings g_initial;

// Menu-thread state.
HHOOK g_mouseHook = nullptr;          // taskbar thread, WH_MOUSE
HHOOK g_taskbarMsgHook = nullptr;     // taskbar thread, WH_GETMESSAGE (pointer input)
HHOOK g_shellMsgHook = nullptr;       // Progman thread, WH_GETMESSAGE (SC_TASKLIST)
DWORD g_taskbarThread = 0;
DWORD g_shellThread = 0;
UINT_PTR g_retryTimer = 0;

// Taskbar-thread state: whether the last button-down on Start was let through to XAML (Shift+click), so the
// matching button-up is let through as well.
bool t_passedDown = false;

// ---------------------------------------------------------------------------------------------------------------
// Start button hit-testing
// ---------------------------------------------------------------------------------------------------------------

bool IsTaskbarWindow(HWND hwnd)
{
    wchar_t name[32] = L"";
    GetClassNameW(hwnd, name, ARRAYSIZE(name));
    return wcscmp(name, L"Shell_TrayWnd") == 0 || wcscmp(name, L"Shell_SecondaryTrayWnd") == 0;
}

// True when `pt` (screen) is on the Start button of the taskbar that owns `hwnd`. The hidden "Start" window the
// taskbar keeps in step with its XAML button marks the area; it is widened by one DIP on its inner side because
// XAML's active area is a little larger (Open-Shell measured the same).
bool OnStartButton(HWND hwnd, POINT pt, HWND* taskbarOut)
{
    HWND root = hwnd ? GetAncestor(hwnd, GA_ROOT) : nullptr;
    if (!root || !IsTaskbarWindow(root))
    {
        return false;
    }
    HWND start = FindWindowExW(root, nullptr, L"Start", nullptr);
    RECT rect;
    if (!start || !GetWindowRect(start, &rect) || IsRectEmpty(&rect))
    {
        return false;
    }
    int adjust = MulDiv(1, GetDpiForWindow(root), 96);
    if (GetWindowLongPtrW(root, GWL_EXSTYLE) & WS_EX_LAYOUTRTL)
    {
        rect.left -= adjust;
    }
    else
    {
        rect.right += adjust;
    }
    if (!PtInRect(&rect, pt))
    {
        return false;
    }
    *taskbarOut = root;
    return true;
}

void RequestToggle(HWND taskbar)
{
    HWND menu = g_menuWindow.load();
    if (menu)
    {
        HMONITOR monitor = taskbar ? MonitorFromWindow(taskbar, MONITOR_DEFAULTTOPRIMARY) : nullptr;
        PostMessageW(menu, kMsgToggle, (WPARAM)(taskbar ? OpenSource::StartButton : OpenSource::Keyboard),
                     (LPARAM)monitor);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------------------------------------------

LRESULT CALLBACK MouseHook(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && g_middleClick.load(std::memory_order_relaxed) != 0)
    {
        UINT message = (UINT)wParam;
        if (message == WM_MBUTTONDOWN || message == WM_NCMBUTTONDOWN || message == WM_MBUTTONUP ||
            message == WM_NCMBUTTONUP)
        {
            const MOUSEHOOKSTRUCT* info = reinterpret_cast<const MOUSEHOOKSTRUCT*>(lParam);
            HWND taskbar = nullptr;
            if (OnStartButton(info->hwnd, info->pt, &taskbar))
            {
                if (message == WM_MBUTTONDOWN || message == WM_NCMBUTTONDOWN)
                {
                    if (HWND menu = g_menuWindow.load())
                    {
                        PostMessageW(menu, kMsgToggle, (WPARAM)OpenSource::MiddleClick,
                                     (LPARAM)MonitorFromWindow(taskbar, MONITOR_DEFAULTTOPRIMARY));
                    }
                }
                return 1;
            }
        }
    }
    if (code == HC_ACTION && g_startButton.load(std::memory_order_relaxed))
    {
        UINT message = (UINT)wParam;
        bool down = message == WM_LBUTTONDOWN || message == WM_NCLBUTTONDOWN || message == WM_LBUTTONDBLCLK ||
                    message == WM_NCLBUTTONDBLCLK;
        bool up = message == WM_LBUTTONUP || message == WM_NCLBUTTONUP;
        if (down || up)
        {
            const MOUSEHOOKSTRUCT* info = reinterpret_cast<const MOUSEHOOKSTRUCT*>(lParam);
            HWND taskbar = nullptr;
            if (OnStartButton(info->hwnd, info->pt, &taskbar))
            {
                if (down)
                {
                    t_passedDown = g_shiftClickWindows.load(std::memory_order_relaxed) && GetKeyState(VK_SHIFT) < 0;
                    if (!t_passedDown)
                    {
                        if (message == WM_LBUTTONDOWN || message == WM_NCLBUTTONDOWN)
                        {
                            RequestToggle(taskbar);
                        }
                        return 1;   // XAML never sees the press, so the Windows menu does not open
                    }
                }
                else if (!t_passedDown)
                {
                    return 1;
                }
                else
                {
                    t_passedDown = false;
                }
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

// Pointer input (touch, pen, and mouse when the thread takes mouse input as pointers) never reaches WH_MOUSE.
LRESULT CALLBACK TaskbarMessageHook(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && wParam == PM_REMOVE && g_startButton.load(std::memory_order_relaxed))
    {
        MSG* msg = reinterpret_cast<MSG*>(lParam);
        if (msg->message == WM_POINTERDOWN || msg->message == WM_NCPOINTERDOWN || msg->message == WM_POINTERUP ||
            msg->message == WM_NCPOINTERUP)
        {
            UINT32 pointerId = GET_POINTERID_WPARAM(msg->wParam);
            POINTER_INPUT_TYPE type = PT_POINTER;
            GetPointerType(pointerId, &type);
            POINT pt = { GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam) };
            HWND taskbar = nullptr;
            bool primary = IS_POINTER_PRIMARY_WPARAM(msg->wParam) != 0;
            if (primary && OnStartButton(msg->hwnd, pt, &taskbar))
            {
                bool isDown = msg->message == WM_POINTERDOWN || msg->message == WM_NCPOINTERDOWN;
                if (isDown && type == PT_MOUSE && g_shiftClickWindows.load(std::memory_order_relaxed) &&
                    GetKeyState(VK_SHIFT) < 0)
                {
                    t_passedDown = true;
                }
                else if (isDown)
                {
                    t_passedDown = false;
                    RequestToggle(taskbar);
                    msg->message = WM_NULL;
                }
                else if (!t_passedDown)
                {
                    msg->message = WM_NULL;
                }
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT CALLBACK ShellMessageHook(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && wParam == PM_REMOVE)
    {
        MSG* msg = reinterpret_cast<MSG*>(lParam);
        if (msg->message == WM_SYSCOMMAND && (msg->wParam & 0xFFF0) == SC_TASKLIST)
        {
            if (g_winKey.load(std::memory_order_relaxed))
            {
                // Swallowed either way: with the guard on, neither menu opens over a full-screen app.
                msg->message = WM_NULL;
                if (!(g_fullscreenGuard.load(std::memory_order_relaxed) && FullscreenInFront()))
                {
                    RequestToggle(nullptr);
                }
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void RemoveHooks()
{
    if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
    if (g_taskbarMsgHook) UnhookWindowsHookEx(g_taskbarMsgHook);
    if (g_shellMsgHook) UnhookWindowsHookEx(g_shellMsgHook);
    g_mouseHook = g_taskbarMsgHook = g_shellMsgHook = nullptr;
    g_taskbarThread = g_shellThread = 0;
}

// Installs whatever is missing. Returns true when every hook is in place.
bool InstallHooks()
{
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    HWND shell = GetShellWindow();
    DWORD taskbarThread = taskbar ? GetWindowThreadProcessId(taskbar, nullptr) : 0;
    DWORD shellThread = shell ? GetWindowThreadProcessId(shell, nullptr) : 0;
    DWORD pid = 0;
    if (taskbar)
    {
        GetWindowThreadProcessId(taskbar, &pid);
        if (pid != GetCurrentProcessId())
        {
            taskbarThread = 0;   // another process's taskbar (the preview, or a second shell): not ours to hook
        }
    }
    if (shell)
    {
        GetWindowThreadProcessId(shell, &pid);
        if (pid != GetCurrentProcessId())
        {
            shellThread = 0;
        }
    }

    // A taskbar or desktop recreated on a new thread takes the old hooks with it.
    if (taskbarThread && taskbarThread != g_taskbarThread)
    {
        if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
        if (g_taskbarMsgHook) UnhookWindowsHookEx(g_taskbarMsgHook);
        g_mouseHook = SetWindowsHookExW(WH_MOUSE, MouseHook, nullptr, taskbarThread);
        g_taskbarMsgHook = SetWindowsHookExW(WH_GETMESSAGE, TaskbarMessageHook, nullptr, taskbarThread);
        g_taskbarThread = g_mouseHook ? taskbarThread : 0;
        if (g_mouseHook)
        {
            SP_LOG_INF(kTag, L"Start button hooked on the taskbar thread %lu", taskbarThread);
        }
        else
        {
            SP_LOG_ERR(kTag, L"The taskbar thread could not be hooked (%lu)", GetLastError());
        }
    }
    if (shellThread && shellThread != g_shellThread)
    {
        if (g_shellMsgHook) UnhookWindowsHookEx(g_shellMsgHook);
        g_shellMsgHook = SetWindowsHookExW(WH_GETMESSAGE, ShellMessageHook, nullptr, shellThread);
        g_shellThread = g_shellMsgHook ? shellThread : 0;
        if (g_shellMsgHook)
        {
            SP_LOG_INF(kTag, L"Windows key routed through the shell window's thread %lu", shellThread);
        }
        else
        {
            SP_LOG_ERR(kTag, L"The shell window's thread could not be hooked (%lu)", GetLastError());
        }
    }
    return g_mouseHook && g_shellMsgHook;
}

void CALLBACK RetryTimer(HWND, UINT, UINT_PTR, DWORD)
{
    if (InstallHooks() && g_retryTimer)
    {
        KillTimer(nullptr, g_retryTimer);
        g_retryTimer = 0;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// The menu thread
// ---------------------------------------------------------------------------------------------------------------

DWORD WINAPI MenuThread(void*)
{
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HRESULT ole = OleInitialize(nullptr);   // shell context menus and the clipboard want OLE on this thread

    MenuView* view = new MenuView();
    bool created = view->Create(g_initial.view);
    UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (created)
    {
        g_menuWindow.store(view->Window());
        if (!InstallHooks())
        {
            // A cold sign-in: the taskbar is not up yet.
            g_retryTimer = SetTimer(nullptr, 0, 1000, RetryTimer);
        }
    }
    SetEvent(g_ready);

    if (created)
    {
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            if (msg.message == taskbarCreated && taskbarCreated)
            {
                if (!InstallHooks() && !g_retryTimer)
                {
                    g_retryTimer = SetTimer(nullptr, 0, 1000, RetryTimer);
                }
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (g_retryTimer)
    {
        KillTimer(nullptr, g_retryTimer);
        g_retryTimer = 0;
    }
    RemoveHooks();
    g_menuWindow.store(nullptr);
    view->Destroy();
    delete view;
    if (SUCCEEDED(ole))
    {
        OleUninitialize();
    }
    return 0;
}

void StoreFlags(const HostSettings& settings)
{
    g_winKey.store(settings.winKey);
    g_startButton.store(settings.startButton);
    g_shiftClickWindows.store(settings.shiftClickWindows);
    g_middleClick.store(settings.middleClick);
    g_fullscreenGuard.store(settings.fullscreenGuard);
}

int ReadInt(const wchar_t* name, int fallback)
{
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\ShadePatcher\\Mods\\custom-start-menu", name, RRF_RT_REG_DWORD,
                     nullptr, &value, &size) != ERROR_SUCCESS)
    {
        return fallback;
    }
    return (int)value;
}

// A REG_SZ value with its %VARIABLES% expanded, or "" when missing.
std::wstring ReadString(const wchar_t* name)
{
    wchar_t value[MAX_PATH * 2] = L"";
    DWORD bytes = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\ShadePatcher\\Mods\\custom-start-menu", name,
                     RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, value, &bytes) != ERROR_SUCCESS)
    {
        return L"";
    }
    std::wstring text = value;
    // A path pasted with its quotes still works.
    if (text.size() >= 2 && text.front() == L'"' && text.back() == L'"')
    {
        text = text.substr(1, text.size() - 2);
    }
    wchar_t expanded[MAX_PATH * 2] = L"";
    if (ExpandEnvironmentStringsW(text.c_str(), expanded, ARRAYSIZE(expanded)))
    {
        text = expanded;
    }
    return text;
}

// A value from a fixed list; anything else (a hand-edited registry) falls back to the default.
int ReadChoice(const wchar_t* name, int fallback, std::initializer_list<int> allowed)
{
    int value = ReadInt(name, fallback);
    for (int a : allowed)
    {
        if (a == value)
        {
            return value;
        }
    }
    return fallback;
}

} // namespace

HostSettings ReadHostSettings()
{
    HostSettings s;
    s.winKey = ReadInt(L"WinKey", 1) != 0;
    s.startButton = ReadInt(L"StartButton", 1) != 0;
    s.shiftClickWindows = ReadInt(L"ShiftClickWindows", 1) != 0;
    s.middleClick = ReadChoice(L"MiddleClick", 0, { 0, 1, 2, 3, 4 });
    s.fullscreenGuard = ReadInt(L"FullscreenGuard", 1) != 0;

    ViewOptions& v = s.view;
    v.placement = (Placement)ReadChoice(L"Position", 0, { 0, 1, 2 });
    v.edgeGap = (float)ReadChoice(L"EdgeGap", 12, { 0, 4, 8, 12, 16, 24, 32 });
    v.showRecent = ReadInt(L"ShowRecent", 1) != 0;
    v.startWithAllApps = ReadInt(L"DefaultView", 0) == 1;

    v.theme = (ThemeMode)ReadChoice(L"Theme", 0, { 0, 1, 2, 3, 4, 5, 6 });
    v.darkFrom = std::clamp(ReadInt(L"DarkFrom", 19), 0, 23);
    v.lightFrom = std::clamp(ReadInt(L"LightFrom", 7), 0, 23);
    v.backgroundImage = ReadString(L"BackgroundImage");
    v.imageBlur = ReadChoice(L"ImageBlur", 20, { 0, 10, 20, 40, 60 });
    v.imageTint = std::clamp(ReadInt(L"ImageTint", 55), 0, 100);
    v.background = (Background)ReadChoice(L"Background", 0, { 0, 1, 2 });
    v.opacity = std::clamp(ReadInt(L"Opacity", 80), 30, 100);
    v.accent = ReadChoice(L"Accent", 0, { 0, 1, 2, 3, 4, 5, 6, 7 });
    v.cornerRadius = (float)std::clamp(ReadInt(L"CornerRadius", 16), 0, 32);
    v.shadow = ReadInt(L"Shadow", 1) != 0;
    v.font = ReadChoice(L"Font", 0, { 0, 1, 2 });
    v.scale = std::clamp(ReadInt(L"Scale", 100), 75, 150);
    v.fontScale = std::clamp(ReadInt(L"FontSize", 100), 80, 130);

    v.columns = std::clamp(ReadInt(L"Columns", 5), 3, 8);
    v.iconSize = (float)std::clamp(ReadInt(L"IconSize", 44), 24, 72);
    v.listIconSize = (float)std::clamp(ReadInt(L"ListIconSize", 28), 16, 48);
    v.labels = ReadInt(L"ShowLabels", 1) != 0;
    v.maxHeight = (float)std::clamp(ReadInt(L"MaxHeight", 600), 400, 1200);
    v.showSearch = ReadInt(L"ShowSearch", 1) != 0;
    v.showTitle = ReadInt(L"ShowTitle", 1) != 0;
    v.showFooter = ReadInt(L"ShowFooter", 1) != 0;
    v.showAccount = ReadInt(L"ShowAccount", 1) != 0;
    v.showUserName = ReadInt(L"ShowUserName", 1) != 0;
    v.showPower = ReadInt(L"ShowPower", 1) != 0;

    v.showMostUsed = ReadInt(L"ShowMostUsed", 1) != 0;
    v.mostUsedCount = std::clamp(ReadInt(L"MostUsedCount", 4), 1, 10);
    v.searchSettings = ReadInt(L"SearchSettings", 1) != 0;
    v.searchCalculator = ReadInt(L"SearchCalculator", 1) != 0;
    v.searchCommands = ReadInt(L"SearchCommands", 1) != 0;
    unsigned shortcuts = 0;
    const struct { const wchar_t* name; Shortcut which; } kShortcuts[] = {
        { L"ShortcutExplorer", Shortcut::Explorer },   { L"ShortcutDocuments", Shortcut::Documents },
        { L"ShortcutDownloads", Shortcut::Downloads }, { L"ShortcutPictures", Shortcut::Pictures },
        { L"ShortcutMusic", Shortcut::Music },         { L"ShortcutVideos", Shortcut::Videos },
        { L"ShortcutUserFolder", Shortcut::UserFolder }, { L"ShortcutSettings", Shortcut::Settings },
    };
    for (const auto& item : kShortcuts)
    {
        bool fallback = (kDefaultShortcuts & ShortcutBit(item.which)) != 0;
        if (ReadInt(item.name, fallback ? 1 : 0) != 0)
        {
            shortcuts |= ShortcutBit(item.which);
        }
    }
    v.shortcuts = shortcuts;
    v.middleClick = s.middleClick;
    v.searchFiles = ReadInt(L"SearchFiles", 1) != 0;
    v.fileCount = std::clamp(ReadInt(L"FileCount", 6), 1, 12);
    v.showNewApps = ReadInt(L"ShowNewApps", 1) != 0;
    v.showQuick = ReadInt(L"ShowQuickSettings", 1) != 0;
    v.searchWindows = ReadInt(L"SearchWindows", 1) != 0;
    v.searchHistory = ReadInt(L"SearchHistory", 1) != 0;
    v.powerUpdates = ReadInt(L"PowerUpdates", 1) != 0;
    if (ReadInt(L"PowerAdvanced", 0) != 0) v.powerItems |= PowerBit(PowerAction::AdvancedStartup);
    if (ReadInt(L"PowerFirmware", 0) != 0) v.powerItems |= PowerBit(PowerAction::Firmware);

    v.animation = (Animation)ReadChoice(L"Animation", 0, { 0, 1, 2, 3 });
    v.slide = ReadInt(L"OpenStyle", 0) == 0;
    unsigned power = 0;
    const struct { const wchar_t* name; PowerAction action; int fallback; } kPower[] = {
        { L"PowerLock", PowerAction::Lock, 1 },         { L"PowerSignOut", PowerAction::SignOut, 0 },
        { L"PowerSleep", PowerAction::Sleep, 1 },       { L"PowerHibernate", PowerAction::Hibernate, 0 },
        { L"PowerRestart", PowerAction::Restart, 1 },   { L"PowerShutDown", PowerAction::ShutDown, 1 },
    };
    for (const auto& item : kPower)
    {
        if (ReadInt(item.name, item.fallback) != 0)
        {
            power |= PowerBit(item.action);
        }
    }
    v.powerItems = power;
    return s;
}

bool HostStart(const HostSettings& settings)
{
    if (g_started)
    {
        HostUpdate(settings);
        return true;
    }
    StoreFlags(settings);
    g_initial = settings;
    g_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = g_ready ? CreateThread(nullptr, 0, MenuThread, nullptr, 0, &g_threadId) : nullptr;
    if (!g_thread)
    {
        SP_LOG_ERR(kTag, L"The menu thread could not start (%lu)", GetLastError());
        if (g_ready)
        {
            CloseHandle(g_ready);
            g_ready = nullptr;
        }
        return false;
    }
    HANDLE waits[] = { g_ready, g_thread };
    WaitForMultipleObjects(2, waits, FALSE, 10000);
    g_started = true;
    if (!g_menuWindow.load())
    {
        SP_LOG_ERR(kTag, L"The menu could not be created");
        HostStop();
        return false;
    }
    SP_LOG_INF(kTag, L"Start menu ready");
    return true;
}

void HostUpdate(const HostSettings& settings)
{
    StoreFlags(settings);
    if (!g_started)
    {
        return;
    }
    HWND menu = g_menuWindow.load();
    if (!menu)
    {
        return;
    }
    // A window message, not a thread message: those are lost while a context menu runs its modal loop.
    ViewOptions* options = new ViewOptions(settings.view);
    if (!PostMessageW(menu, kMsgSetOptions, 0, reinterpret_cast<LPARAM>(options)))
    {
        delete options;
    }
    if (!settings.startButton && !settings.winKey)
    {
        PostMessageW(menu, kMsgClose, 0, 0);
    }
}

void HostStop()
{
    if (!g_thread)
    {
        g_started = false;
        return;
    }
    // WM_QUIT as a thread message also ends a context menu's modal loop, which re-posts it to the thread.
    PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);
    if (WaitForSingleObject(g_thread, 5000) == WAIT_TIMEOUT)
    {
        // It still runs this DLL's code and owns hooks on the taskbar's threads: keep the DLL for the rest of the
        // process rather than let those calls land in unmapped memory.
        SP_LOG_ERR(kTag, L"The menu thread did not end in time; keeping the module loaded");
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&HostStop), &self);
    }
    CloseHandle(g_thread);
    g_thread = nullptr;
    g_threadId = 0;
    if (g_ready)
    {
        CloseHandle(g_ready);
        g_ready = nullptr;
    }
    g_started = false;
}

void HostResetPins()
{
    HWND menu = g_menuWindow.load();
    if (!menu || !PostMessageW(menu, kMsgResetPins, 0, 0))
    {
        PinStore::Delete();
    }
}

void HostCommand(WPARAM command)
{
    HWND menu = g_menuWindow.load();
    if (!menu || !PostMessageW(menu, kMsgCommand, command, 0))
    {
        LineStore::Delete(command == kCommandShowHidden ? L"hidden.txt" : L"history.txt");
    }
}

bool HostWantsWinKey()
{
    return g_winKey.load(std::memory_order_relaxed);
}

} // namespace sm
