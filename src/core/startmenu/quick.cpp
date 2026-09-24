//
// quick.cpp - see quick.h. C++/WinRT reports failures by throwing; every call into it is wrapped, and a failure
// only leaves the corresponding chip hidden or unchanged.
//
#include <unknwn.h>

#include "quick.h"

#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>

#include "engine/log.h"

namespace sm {

using Microsoft::WRL::ComPtr;
namespace radios = winrt::Windows::Devices::Radios;

namespace {

constexpr char kTag[] = "custom-start-menu";
constexpr wchar_t kPersonalize[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";

ComPtr<IAudioEndpointVolume> DefaultEndpoint()
{
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioEndpointVolume> volume;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enumerator))) ||
        FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device)) ||
        FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr,
                                reinterpret_cast<void**>(volume.GetAddressOf()))))
    {
        return nullptr;
    }
    return volume;
}

void SetDarkMode(bool dark)
{
    DWORD light = dark ? 0 : 1;
    RegSetKeyValueW(HKEY_CURRENT_USER, kPersonalize, L"AppsUseLightTheme", REG_DWORD, &light, sizeof(light));
    RegSetKeyValueW(HKEY_CURRENT_USER, kPersonalize, L"SystemUsesLightTheme", REG_DWORD, &light, sizeof(light));
    // What the Settings app sends: every top-level window re-reads its colours.
    DWORD_PTR result = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"ImmersiveColorSet"),
                        SMTO_ABORTIFHUNG, 200, &result);
}

} // namespace

bool DarkModeOn()
{
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, kPersonalize, L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light == 0;
}

bool QuickSettings::Start(HWND notify, UINT message, WPARAM kind)
{
    m_notify = notify;
    m_message = message;
    m_kind = kind;
    m_stop = 0;
    m_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_thread = m_wake ? CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr) : nullptr;
    return m_thread != nullptr;
}

void QuickSettings::Stop()
{
    if (m_thread)
    {
        InterlockedExchange(&m_stop, 1);
        SetEvent(m_wake);
        WaitForSingleObject(m_thread, INFINITE);
        CloseHandle(m_thread);
        m_thread = nullptr;
    }
    if (m_wake)
    {
        CloseHandle(m_wake);
        m_wake = nullptr;
    }
}

void QuickSettings::Post(QuickAction action, int argument)
{
    AcquireSRWLockExclusive(&m_lock);
    m_pending |= 1u << (unsigned)action;
    if (action == QuickAction::Volume)
    {
        m_volumeSteps += argument;
    }
    ReleaseSRWLockExclusive(&m_lock);
    if (m_wake)
    {
        SetEvent(m_wake);
    }
}

DWORD WINAPI QuickSettings::ThreadProc(void* param)
{
    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    }
    catch (...)
    {
    }
    static_cast<QuickSettings*>(param)->Run();
    winrt::uninit_apartment();
    return 0;
}

void QuickSettings::Run()
{
    radios::Radio wifi{ nullptr }, bluetooth{ nullptr };
    bool radiosRead = false;
    auto readRadios = [&] {
        wifi = nullptr;
        bluetooth = nullptr;
        try
        {
            radios::Radio::RequestAccessAsync().get();
            for (const radios::Radio& radio : radios::Radio::GetRadiosAsync().get())
            {
                if (radio.Kind() == radios::RadioKind::WiFi && !wifi) wifi = radio;
                if (radio.Kind() == radios::RadioKind::Bluetooth && !bluetooth) bluetooth = radio;
            }
        }
        catch (const winrt::hresult_error& error)
        {
            SP_LOG_ERR(kTag, L"Radios could not be read (0x%08X)", (unsigned)error.code().value);
        }
        radiosRead = true;
    };
    auto toggle = [](radios::Radio& radio) {
        if (!radio)
        {
            return;
        }
        try
        {
            bool on = radio.State() == radios::RadioState::On;
            radio.SetStateAsync(on ? radios::RadioState::Off : radios::RadioState::On).get();
        }
        catch (const winrt::hresult_error& error)
        {
            SP_LOG_ERR(kTag, L"A radio could not be switched (0x%08X)", (unsigned)error.code().value);
        }
    };

    while (!m_stop)
    {
        WaitForSingleObject(m_wake, INFINITE);
        if (m_stop)
        {
            break;
        }
        AcquireSRWLockExclusive(&m_lock);
        unsigned pending = m_pending;
        int steps = m_volumeSteps;
        m_pending = 0;
        m_volumeSteps = 0;
        ReleaseSRWLockExclusive(&m_lock);
        auto wants = [pending](QuickAction a) { return (pending & (1u << (unsigned)a)) != 0; };

        if (!radiosRead || wants(QuickAction::Refresh))
        {
            readRadios();   // adapters come and go (a dongle, airplane mode); read them on every opening
        }
        if (wants(QuickAction::ToggleWifi)) toggle(wifi);
        if (wants(QuickAction::ToggleBluetooth)) toggle(bluetooth);
        if (wants(QuickAction::ToggleDark)) SetDarkMode(!DarkModeOn());

        auto* state = new QuickState();
        ComPtr<IAudioEndpointVolume> volume = DefaultEndpoint();
        if (volume)
        {
            if (wants(QuickAction::ToggleMute))
            {
                BOOL muted = FALSE;
                volume->GetMute(&muted);
                volume->SetMute(!muted, nullptr);
            }
            if (steps)
            {
                float level = 0;
                volume->GetMasterVolumeLevelScalar(&level);
                level = std::clamp(level + steps / 100.f, 0.f, 1.f);
                volume->SetMasterVolumeLevelScalar(level, nullptr);
                if (level > 0)
                {
                    volume->SetMute(FALSE, nullptr);
                }
            }
            float level = 0;
            BOOL muted = FALSE;
            volume->GetMasterVolumeLevelScalar(&level);
            volume->GetMute(&muted);
            state->volumePresent = true;
            state->volume = (int)lroundf(level * 100);
            state->muted = muted != FALSE;
        }
        try
        {
            if (wifi)
            {
                state->wifiPresent = true;
                state->wifiOn = wifi.State() == radios::RadioState::On;
            }
            if (bluetooth)
            {
                state->bluetoothPresent = true;
                state->bluetoothOn = bluetooth.State() == radios::RadioState::On;
            }
        }
        catch (...)
        {
        }
        state->dark = DarkModeOn();
        if (!PostMessageW(m_notify, m_message, m_kind, reinterpret_cast<LPARAM>(state)))
        {
            delete state;
        }
    }
    wifi = nullptr;
    bluetooth = nullptr;
}

} // namespace sm
