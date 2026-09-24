//
// tray-icons-rebroadcast - bring back notification icons that go missing when the shell starts.
//
// Adapted from the idea behind the Windhawk mod "Disappearing Tray Icons Fix"
// (taskbar-disappearing-tray-icons-fix) by Alchemy. The implementation here is written against this engine's API.
//
// The problem
// -----------
// An application puts its icon in the notification area by calling Shell_NotifyIcon. If the shell restarts, the
// icons are gone, so the shell broadcasts a message called "TaskbarCreated" and every application is expected to
// register its icon again.
//
// On a busy sign-in the taskbar sometimes sends that broadcast before an application is ready to answer it, and
// the icon is simply never registered. The fix is to send the broadcast again a few seconds later, when
// everything has settled.
//
// The broadcast is the one Windows itself sends and is meant to be received more than once, so sending it again
// costs nothing beyond each application re-registering an icon it already has.
//
#define SP_MOD_ID "tray-icons-rebroadcast"
#include "engine/modapi.h"

#include <atomic>

namespace {

// How long after the shell starts to send the broadcast, and how many times.
std::atomic<int> g_delaySeconds{ 5 };
std::atomic<int> g_repeatCount{ 1 };

HANDLE g_thread = nullptr;
HANDLE g_stopEvent = nullptr;

void Broadcast()
{
    // The message number is allocated by the system and is the same for every process, so registering the name
    // here yields exactly the id the shell uses.
    UINT message = RegisterWindowMessageW(L"TaskbarCreated");
    if (!message)
    {
        SP_LogError(L"TaskbarCreated could not be registered: %lu", GetLastError());
        return;
    }

    // SendNotifyMessage rather than SendMessage: a window that is not pumping messages would otherwise hold up
    // the whole broadcast, and this runs while the user is signing in.
    SendNotifyMessageW(HWND_BROADCAST, message, 0, 0);
    SP_Log(L"TaskbarCreated broadcast sent");
}

DWORD WINAPI BroadcastThread(LPVOID)
{
    int repeats = g_repeatCount.load(std::memory_order_relaxed);

    for (int i = 0; i < repeats; ++i)
    {
        int delay = g_delaySeconds.load(std::memory_order_relaxed);

        // Waiting on the stop event rather than sleeping means turning the mod off takes effect at once instead
        // of after the delay.
        if (WaitForSingleObject(g_stopEvent, (DWORD)delay * 1000) != WAIT_TIMEOUT)
        {
            return 0;
        }

        Broadcast();
    }

    return 0;
}

void LoadSettings()
{
    int delay = SP_GetIntSetting(L"DelaySeconds", 5);
    if (delay < 1)
    {
        delay = 1;
    }
    else if (delay > 120)
    {
        delay = 120;
    }
    g_delaySeconds.store(delay, std::memory_order_relaxed);

    int repeats = SP_GetIntSetting(L"RepeatCount", 1);
    if (repeats < 1)
    {
        repeats = 1;
    }
    else if (repeats > 5)
    {
        repeats = 5;
    }
    g_repeatCount.store(repeats, std::memory_order_relaxed);

    SP_Log(L"Broadcasting %d time(s), %d second(s) apart", repeats, delay);
}

BOOL Init()
{
    LoadSettings();

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stopEvent)
    {
        SP_LogError(L"The stop event could not be created: %lu", GetLastError());
        return FALSE;
    }
    return TRUE;
}

void AfterInit()
{
    // Started here rather than in Init so that the wait begins once the shell is actually up.
    g_thread = CreateThread(nullptr, 0, BroadcastThread, nullptr, 0, nullptr);
    if (!g_thread)
    {
        SP_LogError(L"The broadcast thread could not be started: %lu", GetLastError());
    }
}

void Uninit()
{
    if (g_stopEvent)
    {
        SetEvent(g_stopEvent);
    }
    if (g_thread)
    {
        // The thread only ever waits on the stop event, so this returns promptly. The timeout is there so that
        // turning the mod off can never hang the shell.
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_stopEvent)
    {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
}

}   // namespace

SP_MOD_DEFINE(g_modTrayIconsRebroadcast) =
{
    /* id             */ SP_MOD_ID,
    /* name           */ L"Fix notification icons missing after the shell starts",
    /* basedOn        */ "taskbar-disappearing-tray-icons-fix",
    /* originalAuthor */ "Alchemy",
    /* targets        */ SP_TARGET_EXPLORER,
    /* minOsBuild     */ 0,
    /* flags          */ 0,
    /* Init           */ Init,
    /* AfterInit      */ AfterInit,
    /* SettingsChanged*/ LoadSettings,
    /* BeforeUninit   */ nullptr,
    /* Uninit         */ Uninit,
};
