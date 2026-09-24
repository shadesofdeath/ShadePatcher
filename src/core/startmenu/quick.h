#pragma once
//
// quick.h - the quick settings strip: Wi-Fi, Bluetooth, volume and dark mode.
//
// Radios go through Windows.Devices.Radios and the volume through the default render endpoint's
// IAudioEndpointVolume. Both can take a moment (a radio switches in the background), so everything runs on a
// thread of its own and the state comes back as a message; the strip shows the change at once and corrects
// itself if the system disagrees.
//
#include <Windows.h>

namespace sm {

struct QuickState
{
    bool wifiPresent = false, wifiOn = false;
    bool bluetoothPresent = false, bluetoothOn = false;
    bool volumePresent = false, muted = false;
    int volume = 0;         // percent
    bool dark = false;      // apps and system in dark mode
};

enum class QuickAction { Refresh, ToggleWifi, ToggleBluetooth, ToggleMute, Volume, ToggleDark };

class QuickSettings
{
public:
    // State arrives as a QuickState* (owned by the receiver) with `kind` as wParam.
    bool Start(HWND notify, UINT message, WPARAM kind);
    void Stop();

    // `argument` is the volume step in percent for QuickAction::Volume.
    void Post(QuickAction action, int argument = 0);

private:
    static DWORD WINAPI ThreadProc(void* param);
    void Run();

    HANDLE m_thread = nullptr;
    HANDLE m_wake = nullptr;
    HWND m_notify = nullptr;
    UINT m_message = 0;
    WPARAM m_kind = 0;
    volatile LONG m_stop = 0;

    SRWLOCK m_lock = SRWLOCK_INIT;
    unsigned m_pending = 0;     // bits of QuickAction
    int m_volumeSteps = 0;      // accumulated volume change
};

// Dark mode as the "Colours" page sets it: both the apps and Windows use the dark theme.
bool DarkModeOn();

} // namespace sm
