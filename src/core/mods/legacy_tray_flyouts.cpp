//
// legacy-tray-flyouts - the Windows 7/10 style flyouts for the clock, the volume icon and the battery icon on the
// Windows 11 taskbar.
//
// Adapted from the behaviour of three Windhawk mods by Anixx: "Legacy (Win32) clock flyout" (legacy-clock-flyout),
// "Legacy (Win32) sound volume flyout" (legacy-sound-flyout) and "Legacy (Win32) power flyout"
// (legacy-power-flyout), which follow ExplorerPatcher. Those mods subclass the Windows 10 tray windows
// (TrayClockWClass, the tray ToolbarWindow32), which the Windows 11 taskbar no longer has: its tray is XAML. The
// implementation here is written against this engine's API and reaches the same result on the XAML tray.
//
// How it works
// ------------
// Clock and volume. A click on the XAML taskbar arrives as WM_POINTERDOWN at the taskbar's InputSite window
// (see taskbar_empty_space_clicks.cpp for the full account of that window and why its procedure is hooked
// rather than subclassed). The tray buttons are XAML elements, so UI Automation is asked which element is under
// the press. When it is the clock, or the volume icon, the press and its matching release are swallowed, so the
// XAML tray never opens the notification centre or quick settings, and the classic flyout is shown instead:
//
//   * the clock uses the Win32 clock flyout that still ships in timedate.cpl, through the same COM class
//     ExplorerPatcher uses ({A323554A-0FE1-4E49-AEE1-6722465D799F}, IWin32Clock::ShowWin32Clock), anchored to
//     the clock button's rectangle;
//   * the volume icon runs SndVol.exe -f <MAKELONG(x, y)>, the classic volume flyout. On Windows 11 (checked on
//     build 26200) SndVol ignores the point in -f and always opens its dialog at the top left corner of the
//     screen, whatever the value, so the launcher thread waits for the dialog to appear and moves it itself,
//     centred over the volume button and touching the taskbar, the way the flyout sat on Windows 7.
//
// Telling the tray buttons apart. UI Automation reports every tray button with the same automation id
// (SystemTrayIcon) and a class that depends on the button's position in the group, so neither is a stable key.
// The accessible name is the button's tooltip, which is localized, so it is matched against facts the machine
// knows rather than words: the clock's name contains the current time in the user's short time format, and the
// volume button's name contains the friendly name of the default audio output device, as reported by the audio
// endpoint API. The English and Turkish tooltip prefixes are accepted as a fallback.
//
// Battery. The battery icon's flyout is chosen inside the tray objects (stobject.dll), which read the registry
// value UseWin32BatteryFlyout. Rather than writing to HKLM, the way the original mod does it is kept: the read is
// answered from a RegQueryValueExW hook while the option is on. Whether the XAML tray honours that choice on a
// given build is up to Windows; the option is offered as it is in the original.
//
// Threading
// ---------
// The InputSite hook runs on the taskbar's UI thread, which is an STA; UI Automation, the audio endpoint query
// and the clock flyout are used there and only there. SndVol.exe is started from a short worker thread so the
// taskbar never waits on process creation. Settings are read on the engine thread into atomics.
//
#define SP_MOD_ID "legacy-tray-flyouts"
#include "engine/modapi.h"

#include <windowsx.h>
#include <objbase.h>
#include <unknwn.h>
#include <UIAutomation.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <algorithm>
#include <atomic>
#include <new>
#include <cstdio>
#include <cstring>

namespace {

// ---------------------------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------------------------

std::atomic<bool> g_clock{ true };
std::atomic<bool> g_volume{ true };
std::atomic<bool> g_power{ false };

void LoadSettings()
{
    g_clock.store(SP_GetIntSetting(L"Clock", 1) != 0, std::memory_order_relaxed);
    g_volume.store(SP_GetIntSetting(L"Volume", 1) != 0, std::memory_order_relaxed);
    g_power.store(SP_GetIntSetting(L"Power", 0) != 0, std::memory_order_relaxed);
    SP_LogDebug(L"Settings: clock=%d volume=%d power=%d", g_clock.load(), g_volume.load(), g_power.load());
}

// ---------------------------------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------------------------------

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

// Case-insensitive "haystack contains needle".
bool ContainsNoCase(const wchar_t* haystack, const wchar_t* needle)
{
    if (!haystack || !needle || !*needle)
    {
        return false;
    }
    const size_t n = wcslen(needle);
    for (const wchar_t* p = haystack; *p; ++p)
    {
        if (_wcsnicmp(p, needle, n) == 0)
        {
            return true;
        }
    }
    return false;
}

bool StartsWithNoCase(const wchar_t* text, const wchar_t* prefix)
{
    return text && prefix && _wcsnicmp(text, prefix, wcslen(prefix)) == 0;
}

// ---------------------------------------------------------------------------------------------------------------
// UI Automation, on the taskbar's thread
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
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                              __uuidof(IUIAutomation), reinterpret_cast<void**>(&uia));
    }
    if (FAILED(hr))
    {
        SP_LogError(L"UI Automation could not be created: 0x%08X", (unsigned)hr);
        return nullptr;
    }
    g_uia = uia;
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

// ---------------------------------------------------------------------------------------------------------------
// Recognising the buttons
// ---------------------------------------------------------------------------------------------------------------

// The friendly name of the default audio output, cached for a few seconds: the query is cheap but not free and a
// double click asks twice.
wchar_t g_audioDeviceName[256] = {};
DWORD   g_audioDeviceNameTick = 0;

const wchar_t* DefaultAudioDeviceName()
{
    const DWORD now = GetTickCount();
    if (g_audioDeviceName[0] && now - g_audioDeviceNameTick < 5000)
    {
        return g_audioDeviceName;
    }
    g_audioDeviceName[0] = 0;
    g_audioDeviceNameTick = now;

    ComRef<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                __uuidof(IMMDeviceEnumerator), enumerator.PutVoid())))
    {
        return g_audioDeviceName;
    }
    ComRef<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.Put())) || !device)
    {
        return g_audioDeviceName;
    }
    ComRef<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, store.Put())) || !store)
    {
        return g_audioDeviceName;
    }
    PROPVARIANT value;
    PropVariantInit(&value);
    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR && value.pwszVal)
    {
        wcscpy_s(g_audioDeviceName, value.pwszVal);
    }
    PropVariantClear(&value);
    return g_audioDeviceName;
}

// TRUE when the name carries the current time in the user's short time format (or the minute before it, so a
// click at the turn of a minute still counts).
bool NameHasCurrentTime(const wchar_t* name)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    for (int back = 0; back < 2; ++back)
    {
        wchar_t time[64];
        if (GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, time, ARRAYSIZE(time)) &&
            ContainsNoCase(name, time))
        {
            return true;
        }
        // One minute earlier.
        FILETIME ft;
        SystemTimeToFileTime(&st, &ft);
        ULARGE_INTEGER u = { ft.dwLowDateTime, ft.dwHighDateTime };
        u.QuadPart -= 60ull * 10000000ull;
        ft.dwLowDateTime = u.LowPart;
        ft.dwHighDateTime = u.HighPart;
        FileTimeToSystemTime(&ft, &st);
    }
    return false;
}

enum class TrayButton
{
    None,
    Clock,
    Volume,
};

TrayButton ClassifyTrayButton(const wchar_t* className, const wchar_t* name)
{
    if (!className || !name || wcsncmp(className, L"SystemTray.", 11) != 0)
    {
        return TrayButton::None;
    }
    if (wcscmp(className, L"SystemTray.ShowDesktopButton") == 0 || wcscmp(className, L"SystemTray.NormalButton") == 0)
    {
        return TrayButton::None;   // show desktop, the overflow chevron
    }

    if (g_clock.load(std::memory_order_relaxed))
    {
        if (NameHasCurrentTime(name) || StartsWithNoCase(name, L"Clock") || StartsWithNoCase(name, L"Saat"))
        {
            return TrayButton::Clock;
        }
    }
    if (g_volume.load(std::memory_order_relaxed))
    {
        const wchar_t* device = DefaultAudioDeviceName();
        if ((device[0] && ContainsNoCase(name, device)) ||
            StartsWithNoCase(name, L"Volume") || StartsWithNoCase(name, L"Birim") || StartsWithNoCase(name, L"Ses "))
        {
            return TrayButton::Volume;
        }
    }
    return TrayButton::None;
}

// The tray button under the point, with its screen rectangle.
TrayButton TrayButtonAt(POINT ptScreen, RECT* pRect)
{
    IUIAutomation* uia = GetAutomation();
    if (!uia)
    {
        return TrayButton::None;
    }

    ComRef<IUIAutomationElement> element;
    if (FAILED(uia->ElementFromPoint(ptScreen, element.Put())) || !element)
    {
        return TrayButton::None;
    }

    BSTR className = nullptr;
    BSTR name = nullptr;
    element->get_CurrentClassName(&className);
    element->get_CurrentName(&name);

    TrayButton button = ClassifyTrayButton(className, name);
    if (button != TrayButton::None)
    {
        SP_LogDebug(L"Under the click: %s \"%s\" -> %s", className ? className : L"", name ? name : L"",
                    button == TrayButton::Clock ? L"clock" : L"volume");
        if (pRect && FAILED(element->get_CurrentBoundingRectangle(pRect)))
        {
            SetRect(pRect, ptScreen.x, ptScreen.y, ptScreen.x, ptScreen.y);
        }
    }

    SysFreeString(className);
    SysFreeString(name);
    return button;
}

// ---------------------------------------------------------------------------------------------------------------
// The flyouts
// ---------------------------------------------------------------------------------------------------------------

// The Win32 clock flyout that ships in timedate.cpl. ExplorerPatcher and the original mod call the same interface.
MIDL_INTERFACE("7A5FCA8A-76B1-44C8-A97C-E7173CCA5F4F")
IWin32Clock : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE ShowWin32Clock(HWND hWnd, LPRECT lpRect) = 0;
};

constexpr CLSID CLSID_Win32Clock = { 0xA323554A, 0x0FE1, 0x4E49, { 0xAE, 0xE1, 0x67, 0x22, 0x46, 0x5D, 0x79, 0x9F } };

void ShowClockFlyout(HWND hTaskbar, const RECT& rcAnchor)
{
    if (FindWindowW(L"ClockFlyoutWindow", nullptr))
    {
        return;   // already open; the click that reached the taskbar has closed it by the time this runs
    }

    ComRef<IWin32Clock> clock;
    HRESULT hr = CoCreateInstance(CLSID_Win32Clock, nullptr, CLSCTX_INPROC_SERVER | CLSCTX_INPROC_HANDLER,
                                  __uuidof(IWin32Clock), clock.PutVoid());
    if (FAILED(hr) || !clock)
    {
        SP_LogError(L"The Win32 clock flyout is not available: 0x%08X", (unsigned)hr);
        return;
    }
    RECT rc = rcAnchor;
    hr = clock->ShowWin32Clock(hTaskbar, &rc);
    if (FAILED(hr))
    {
        SP_LogError(L"The clock flyout could not be shown: 0x%08X", (unsigned)hr);
    }
}

std::atomic<int> g_workers{ 0 };
DWORD g_lastVolumeLaunch = 0;

// What the launcher thread is handed: where the click was and the rectangle of the volume button.
struct VolumeLaunch
{
    POINT pt;
    RECT  rcAnchor;
};

// The SndVol flyout is a dialog (class #32770) and the only visible top-level window of its process.
struct FlyoutSearch
{
    DWORD processId;
    HWND  hFound;
};

BOOL CALLBACK FindFlyoutProc(HWND hWnd, LPARAM lParam)
{
    FlyoutSearch* search = reinterpret_cast<FlyoutSearch*>(lParam);
    DWORD processId = 0;
    GetWindowThreadProcessId(hWnd, &processId);
    if (processId == search->processId && IsWindowVisible(hWnd) && IsClass(hWnd, L"#32770"))
    {
        search->hFound = hWnd;
        return FALSE;
    }
    return TRUE;
}

HWND WaitForVolumeFlyout(DWORD processId, HANDLE hProcess)
{
    FlyoutSearch search = { processId, nullptr };
    for (int attempt = 0; attempt < 150 && !search.hFound; ++attempt)   // three seconds
    {
        if (WaitForSingleObject(hProcess, 20) != WAIT_TIMEOUT)
        {
            return nullptr;   // SndVol is already gone
        }
        EnumWindows(FindFlyoutProc, reinterpret_cast<LPARAM>(&search));
    }
    return search.hFound;
}

// Centres the flyout over the anchor and puts it against the taskbar: above the button when the taskbar is in the
// lower half of the monitor, below it otherwise, and always fully on that monitor.
void PlaceVolumeFlyout(HWND hFlyout, const RECT& rcAnchor)
{
    RECT rcFlyout;
    if (!GetWindowRect(hFlyout, &rcFlyout))
    {
        return;
    }
    const int width = rcFlyout.right - rcFlyout.left;
    const int height = rcFlyout.bottom - rcFlyout.top;

    MONITORINFO mi = { sizeof(mi) };
    HMONITOR hMonitor = MonitorFromRect(&rcAnchor, MONITOR_DEFAULTTONEAREST);
    if (!hMonitor || !GetMonitorInfoW(hMonitor, &mi))
    {
        SetRect(&mi.rcMonitor, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    }
    const RECT& rcMonitor = mi.rcMonitor;

    int x = (rcAnchor.left + rcAnchor.right) / 2 - width / 2;
    const bool anchorInLowerHalf = (rcAnchor.top + rcAnchor.bottom) / 2 > (rcMonitor.top + rcMonitor.bottom) / 2;
    int y = anchorInLowerHalf ? rcAnchor.top - height : rcAnchor.bottom;

    x = (std::max)((int)rcMonitor.left, (std::min)(x, (int)(rcMonitor.right - width)));
    y = (std::max)((int)rcMonitor.top, (std::min)(y, (int)(rcMonitor.bottom - height)));

    SetWindowPos(hFlyout, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    SP_LogDebug(L"The volume flyout was moved to %d,%d", x, y);
}

DWORD WINAPI LaunchVolumeThread(LPVOID param)
{
    VolumeLaunch* launch = static_cast<VolumeLaunch*>(param);
    const DWORD encoded = (DWORD)MAKELONG(launch->pt.x, launch->pt.y);
    const RECT rcAnchor = launch->rcAnchor;
    delete launch;

    wchar_t system32[MAX_PATH];
    if (!GetSystemDirectoryW(system32, ARRAYSIZE(system32)))
    {
        g_workers.fetch_sub(1, std::memory_order_acq_rel);
        return 0;
    }
    wchar_t commandLine[MAX_PATH + 64];
    swprintf_s(commandLine, L"\"%s\\SndVol.exe\" -f %u", system32, encoded);

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_FORCEOFFFEEDBACK;
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, commandLine, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
    {
        CloseHandle(pi.hThread);
        // SndVol ignores the point in -f on Windows 11 and opens at the top left corner, so the dialog is placed
        // here once it exists.
        if (HWND hFlyout = WaitForVolumeFlyout(pi.dwProcessId, pi.hProcess))
        {
            PlaceVolumeFlyout(hFlyout, rcAnchor);
        }
        else
        {
            SP_LogDebug(L"The volume flyout window did not appear; it is left where SndVol put it");
        }
        CloseHandle(pi.hProcess);
    }
    else
    {
        SP_LogError(L"SndVol.exe could not be started: %lu", GetLastError());
    }
    g_workers.fetch_sub(1, std::memory_order_acq_rel);
    return 0;
}

// The classic volume flyout is a separate program that closes itself when it loses focus. A second click on the
// icon while it is open reaches the taskbar after the flyout has already gone, so a short debounce keeps that
// click from reopening it at once.
void ShowVolumeFlyout(POINT ptAnchor, const RECT& rcButton)
{
    const DWORD now = GetTickCount();
    if (now - g_lastVolumeLaunch < 400)
    {
        return;
    }
    g_lastVolumeLaunch = now;

    VolumeLaunch* launch = new (std::nothrow) VolumeLaunch{ ptAnchor, rcButton };
    if (!launch)
    {
        return;
    }
    // An empty rectangle (UI Automation gave none) anchors at the click itself.
    if (IsRectEmpty(&launch->rcAnchor))
    {
        SetRect(&launch->rcAnchor, ptAnchor.x, ptAnchor.y, ptAnchor.x + 1, ptAnchor.y + 1);
    }

    g_workers.fetch_add(1, std::memory_order_acq_rel);
    HANDLE hThread = CreateThread(nullptr, 0, LaunchVolumeThread, launch, 0, nullptr);
    if (hThread)
    {
        CloseHandle(hThread);
    }
    else
    {
        g_workers.fetch_sub(1, std::memory_order_acq_rel);
        delete launch;
        SP_LogError(L"The launcher thread could not be created: %lu", GetLastError());
    }
}

// ---------------------------------------------------------------------------------------------------------------
// The InputSite hook
// ---------------------------------------------------------------------------------------------------------------

using WndProc_t = LRESULT(CALLBACK*)(HWND, UINT, WPARAM, LPARAM);
WndProc_t          g_origInputSiteProc = nullptr;
std::atomic<void*> g_hookedInputSiteProc{ nullptr };

// The pointer whose press was swallowed; its release is swallowed too so XAML sees neither half of the click.
std::atomic<UINT32> g_swallowedPointer{ 0 };

bool PointerIsMouseLeft(WPARAM wParam)
{
    if (!IS_POINTER_FIRSTBUTTON_WPARAM(wParam))
    {
        return false;
    }
    const UINT32 pointerId = GET_POINTERID_WPARAM(wParam);
    POINTER_INPUT_TYPE type = PT_POINTER;
    if (pointerId && GetPointerType(pointerId, &type) && (type == PT_TOUCH || type == PT_PEN))
    {
        return false;   // touch gets the touch-friendly XAML flyouts
    }
    return true;
}

LRESULT CALLBACK InputSiteProc_Hook(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (uMsg == WM_POINTERUP)
    {
        const UINT32 pointerId = GET_POINTERID_WPARAM(wParam);
        if (pointerId && g_swallowedPointer.exchange(0, std::memory_order_acq_rel) == pointerId)
        {
            return 0;
        }
    }
    else if (uMsg == WM_POINTERDOWN && PointerIsMouseLeft(wParam))
    {
        HWND hRoot = GetAncestor(hWnd, GA_ROOT);
        if (hRoot && IsTaskbarWindow(hRoot))
        {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };   // screen coordinates
            RECT rc = {};
            const TrayButton button = TrayButtonAt(pt, &rc);
            if (button != TrayButton::None)
            {
                g_swallowedPointer.store(GET_POINTERID_WPARAM(wParam), std::memory_order_release);
                if (button == TrayButton::Clock)
                {
                    ShowClockFlyout(hRoot, rc);
                }
                else
                {
                    ShowVolumeFlyout(pt, rc);
                }
                return 0;
            }
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
    if (g_hookedInputSiteProc.load(std::memory_order_acquire))
    {
        return true;
    }
    if (!SP_SetFunctionHookNow(proc, InputSiteProc_Hook, &g_origInputSiteProc))
    {
        SP_LogError(L"The InputSite window procedure %p could not be hooked", proc);
        return false;
    }
    g_hookedInputSiteProc.store(proc, std::memory_order_release);
    SP_Log(L"Watching clicks on the tray through the InputSite procedure %p", proc);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// The battery flyout choice
// ---------------------------------------------------------------------------------------------------------------

using RegQueryValueExW_t = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
RegQueryValueExW_t g_origRegQueryValueExW = nullptr;

LSTATUS WINAPI RegQueryValueExW_Hook(HKEY hKey, LPCWSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType,
                                     LPBYTE lpData, LPDWORD lpcbData)
{
    if (lpValueName && g_power.load(std::memory_order_relaxed) &&
        _wcsicmp(lpValueName, L"UseWin32BatteryFlyout") == 0)
    {
        SP_LogDebug(L"UseWin32BatteryFlyout was asked for; answering 1");
        if (lpType)
        {
            *lpType = REG_DWORD;
        }
        if (lpcbData)
        {
            if (lpData && *lpcbData >= sizeof(DWORD))
            {
                *(DWORD*)lpData = 1;
            }
            else if (lpData)
            {
                *lpcbData = sizeof(DWORD);
                return ERROR_MORE_DATA;
            }
            *lpcbData = sizeof(DWORD);
        }
        return ERROR_SUCCESS;
    }
    return g_origRegQueryValueExW(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);
}

// ---------------------------------------------------------------------------------------------------------------
// Attaching to the taskbar
// ---------------------------------------------------------------------------------------------------------------

std::atomic<bool> g_stopping{ false };
UINT g_releaseMessage = 0;

// Runs on a worker thread of its own (never the loading thread: this polls for up to a minute) once Taskbar.View.dll is loaded; the island appears a moment later.
void OnTaskbarModuleLoaded(HMODULE, void*)
{
    for (int attempt = 0; attempt < 240; ++attempt)   // one minute
    {
        if (g_stopping.load(std::memory_order_relaxed))
        {
            return;
        }
        HWND hTray = FindPrimaryTaskbar();
        HWND hInputSite = hTray ? FindInputSite(hTray) : nullptr;
        if (hInputSite && HookInputSite(hInputSite))
        {
            return;
        }
        Sleep(250);
    }
    SP_LogError(L"The taskbar's input window never appeared; tray clicks will not be seen");
}

// ---------------------------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------------------------

BOOL Init()
{
    LoadSettings();
    g_stopping.store(false, std::memory_order_relaxed);
    g_swallowedPointer.store(0, std::memory_order_relaxed);

    if (!SP_HookBegin())
    {
        return FALSE;
    }
    if (!SP_SetExportHook(L"kernelbase.dll", "RegQueryValueExW", RegQueryValueExW_Hook, &g_origRegQueryValueExW))
    {
        SP_HookAbort();
        SP_LogError(L"RegQueryValueExW could not be hooked");
        return FALSE;
    }
    if (!SP_HookCommit())
    {
        return FALSE;
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
    // CUIAutomation is a free-threaded in-process object; releasing it from here is allowed.
    ReleaseAutomation();
}

void Uninit()
{
    for (int i = 0; i < 50 && g_workers.load(std::memory_order_acquire) > 0; ++i)
    {
        Sleep(100);
    }
    g_hookedInputSiteProc.store(nullptr, std::memory_order_release);
    g_origInputSiteProc = nullptr;
    g_origRegQueryValueExW = nullptr;
}

}   // namespace

SP_MOD_DEFINE(g_modLegacyTrayFlyouts) =
{
    /* id             */ SP_MOD_ID,
    /* name           */ L"Classic flyouts for the clock, volume and battery icons",
    /* basedOn        */ "legacy-clock-flyout, legacy-sound-flyout, legacy-power-flyout",
    /* originalAuthor */ "Anixx",
    /* targets        */ SP_TARGET_EXPLORER,
    /* minOsBuild     */ 22000,
    /* flags          */ 0,
    /* Init           */ Init,
    /* AfterInit      */ nullptr,
    /* SettingsChanged*/ LoadSettings,
    /* BeforeUninit   */ BeforeUninit,
    /* Uninit         */ Uninit,
};
