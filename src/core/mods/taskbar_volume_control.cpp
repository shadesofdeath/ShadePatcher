//
// taskbar-volume-control - change the system volume by scrolling the mouse wheel over the taskbar.
//
// Adapted from the idea behind the Windhawk mod "Taskbar Volume Control" (taskbar-volume-control) by m417z.
// The implementation here is written against this engine's API and keeps only the Windows 11 behaviour.
//
// What it does
// ------------
// A wheel notch over the taskbar (the whole bar, or only the notification area, by setting) raises or lowers
// the master volume by a configurable number of percent and shows the same volume flyout Windows shows for a
// keyboard's volume keys. A middle click on the tray's volume icon mutes and unmutes. Optionally, holding Ctrl
// and/or Shift lets the wheel change the volume anywhere on screen.
//
// How the wheel reaches the taskbar on Windows 11
// -----------------------------------------------
// The Windows 11 taskbar is XAML, hosted in an island under Shell_TrayWnd:
//
//     Shell_TrayWnd (or Shell_SecondaryTrayWnd)
//       Windows.UI.Composition.DesktopWindowContentBridge
//         Windows.UI.Input.InputSite.WindowClass          <- pointer input lands here, as WM_POINTERWHEEL
//
// The wheel never arrives at Shell_TrayWnd as WM_MOUSEWHEEL, so subclassing the taskbar alone sees nothing. The
// InputSite window cannot be subclassed either: inputsite.dll checks that its window procedure is untouched and
// crashes if it is not. So its procedure is hooked as a function instead, the way the original mod does it. All
// InputSite windows share that procedure, so one hook covers the primary and every secondary taskbar; the hook
// only acts when the window's root is a taskbar and the pointer is inside the chosen scroll area.
//
// The taskbar windows are still subclassed, for two things the InputSite hook cannot do: receiving the
// "scroll anywhere" request posted from the low-level mouse hook thread (the volume work is done on the taskbar
// thread, never inside the hook procedure, which must return quickly), and a WM_MOUSEWHEEL fallback for a build
// where the InputSite procedure could not be hooked.
//
// Taskbar windows are found in two ways: those that already exist are enumerated in AfterInit, and new ones (a
// monitor plugged in later, or the shell restarting its taskbar) are caught by hooking CreateWindowExW and
// CreateWindowInBand, which is also how the InputSite window is caught on a cold sign-in.
//
// How the volume is changed
// -------------------------
// The original mod's Windows 11 path is reproduced: the shell is asked to do the last 2% itself by posting
// HSHELL_APPCOMMAND / APPCOMMAND_VOLUME_UP or _DOWN to its shell-hook window (MSTaskSwWClass), which is exactly
// what a keyboard volume key ends up as, so the standard volume flyout appears. Any remaining percent beyond
// those 2 is applied first through Core Audio (IAudioEndpointVolume). A step of 1 works too: the endpoint is
// moved 1% the other way before the shell adds its 2.
//
// Middle click on the volume icon
// -------------------------------
// The tray icon's click handler, VolumeSystemTrayIconDataModel::OnIconClicked, is hooked from its PDB name. The
// types live in SystemTray.dll on current builds and in Taskbar.View.dll (below version 2604) on older ones; the
// mod waits for Taskbar.View.dll, then hooks whichever module holds them. When the middle button is down at the
// time of the click the mute state is toggled through Core Audio and the shell's own handler is skipped.
//
// Threading
// ---------
// Wheel handling runs on the taskbar's thread (the InputSite hook, the subclass and the posted scroll-anywhere
// message all arrive there). Settings are atomics written on the engine thread. The Core Audio enumerator is
// created lazily under a lock and released in Uninit; MMDevice objects are free-threaded, so the thread that
// creates it does not matter.
//
#define SP_MOD_ID "taskbar-volume-control"
#include "engine/modapi.h"

#include <commctrl.h>
#include <windowsx.h>       // GET_X_LPARAM / GET_Y_LPARAM
#include <mmdeviceapi.h>
#include <endpointvolume.h>

#include <string.h>

#include <atomic>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Ole32.lib")

#ifndef WM_POINTERWHEEL
#define WM_POINTERWHEEL 0x024E
#endif

namespace {

// ---------------------------------------------------------------------------------------------------------------
// Settings and shared state
// ---------------------------------------------------------------------------------------------------------------

enum class ScrollArea
{
    WholeTaskbar = 0,
    TrayOnly = 1,
};

// Bits of the ScrollAnywhereModifier setting. Alt and Win are deliberately not offered: a lone press of either
// opens a menu on release, and suppressing that needs a keyboard hook and injected key events.
constexpr int kModifierCtrl = 1;
constexpr int kModifierShift = 2;

std::atomic<int>        g_volumeStep{ 2 };             // percent per wheel notch
std::atomic<ScrollArea> g_scrollArea{ ScrollArea::WholeTaskbar };
std::atomic<bool>       g_middleClickMute{ true };
std::atomic<int>        g_scrollAnywhereModifier{ 0 }; // 0 = off

// Cleared in BeforeUninit so the hooks and subclasses that are still live fall silent before they are removed.
std::atomic<bool> g_active{ true };

std::atomic<HWND>  g_hTaskbar{ nullptr };          // the primary taskbar, Shell_TrayWnd
std::atomic<DWORD> g_taskbarThreadId{ 0 };
std::atomic<bool>  g_inputSiteHooked{ false };

// Secondary taskbars, one per extra monitor. Written from the taskbar thread (window creation and destruction)
// and read from the engine thread when the mod unloads.
constexpr int kMaxSecondary = 16;
SRWLOCK g_secondaryLock = SRWLOCK_INIT;
HWND    g_secondary[kMaxSecondary] = {};
int     g_secondaryCount = 0;

UINT g_scrollAnywhereMsg = 0;   // posted by the mouse hook thread to the primary taskbar
UINT g_shellHookMsg = 0;        // "SHELLHOOK", the message the shell's app-command handling listens for

constexpr UINT_PTR kSubclassId = 1;

// High-resolution wheels and touchpads report deltas smaller than one notch; what is left over is carried into
// the next event for a few seconds so slow scrolling still adds up. Only touched on the taskbar thread.
std::atomic<DWORD> g_lastScrollTick{ 0 };
std::atomic<int>   g_scrollRemainder{ 0 };

// ---------------------------------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------------------------------

bool ClassNameIs(HWND hWnd, const wchar_t* name)
{
    wchar_t wszClass[64];
    if (!hWnd || !GetClassNameW(hWnd, wszClass, ARRAYSIZE(wszClass)))
    {
        return false;
    }
    return _wcsicmp(wszClass, name) == 0;
}

bool IsTaskbarWindow(HWND hWnd)
{
    return ClassNameIs(hWnd, L"Shell_TrayWnd") || ClassNameIs(hWnd, L"Shell_SecondaryTrayWnd");
}

UINT DpiForWindow(HWND hWnd)
{
    using GetDpiForWindow_t = UINT(WINAPI*)(HWND);
    static GetDpiForWindow_t pfn = []() -> GetDpiForWindow_t {
        HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
        return hUser32 ? (GetDpiForWindow_t)GetProcAddress(hUser32, "GetDpiForWindow") : nullptr;
    }();

    UINT dpi = pfn ? pfn(hWnd) : 0;
    return dpi ? dpi : 96;
}

// The major part of a module's file version, read straight from its version resource. 0 when there is none.
WORD ModuleMajorVersion(HMODULE hModule)
{
    HRSRC hRes = FindResourceW(hModule, MAKEINTRESOURCEW(VS_VERSION_INFO), RT_VERSION);
    if (!hRes)
    {
        return 0;
    }
    HGLOBAL hGlobal = LoadResource(hModule, hRes);
    if (!hGlobal)
    {
        return 0;
    }
    const BYTE* data = (const BYTE*)LockResource(hGlobal);
    DWORD size = SizeofResource(hModule, hRes);
    if (!data || size < sizeof(VS_FIXEDFILEINFO))
    {
        return 0;
    }

    // VS_VERSIONINFO is a header, the key "VS_VERSION_INFO" and padding, then the fixed block, which starts with
    // a signature. Looking for the signature avoids the version API and a link dependency for one number.
    for (DWORD at = 0; at + sizeof(VS_FIXEDFILEINFO) <= size; at += sizeof(DWORD))
    {
        const VS_FIXEDFILEINFO* info = (const VS_FIXEDFILEINFO*)(data + at);
        if (info->dwSignature == 0xFEEF04BD)
        {
            return HIWORD(info->dwFileVersionMS);
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Secondary taskbar bookkeeping
// ---------------------------------------------------------------------------------------------------------------

void RememberSecondary(HWND hWnd)
{
    AcquireSRWLockExclusive(&g_secondaryLock);
    bool present = false;
    for (int i = 0; i < g_secondaryCount; ++i)
    {
        if (g_secondary[i] == hWnd)
        {
            present = true;
            break;
        }
    }
    if (!present && g_secondaryCount < kMaxSecondary)
    {
        g_secondary[g_secondaryCount++] = hWnd;
    }
    ReleaseSRWLockExclusive(&g_secondaryLock);
}

void ForgetSecondary(HWND hWnd)
{
    AcquireSRWLockExclusive(&g_secondaryLock);
    for (int i = 0; i < g_secondaryCount; ++i)
    {
        if (g_secondary[i] == hWnd)
        {
            g_secondary[i] = g_secondary[g_secondaryCount - 1];
            g_secondary[g_secondaryCount - 1] = nullptr;
            g_secondaryCount--;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_secondaryLock);
}

// Copies the current list out so it can be walked without holding the lock.
int SnapshotSecondary(HWND* out, int capacity)
{
    AcquireSRWLockShared(&g_secondaryLock);
    int n = g_secondaryCount < capacity ? g_secondaryCount : capacity;
    for (int i = 0; i < n; ++i)
    {
        out[i] = g_secondary[i];
    }
    ReleaseSRWLockShared(&g_secondaryLock);
    return n;
}

// ---------------------------------------------------------------------------------------------------------------
// Core Audio
// ---------------------------------------------------------------------------------------------------------------

SRWLOCK             g_audioLock = SRWLOCK_INIT;
IMMDeviceEnumerator* g_enumerator = nullptr;

// The default render endpoint's volume interface, with a reference the caller releases. Null when there is no
// audio device or COM is not available on this thread.
IAudioEndpointVolume* OpenEndpointVolume()
{
    AcquireSRWLockExclusive(&g_audioLock);
    if (!g_enumerator)
    {
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                      __uuidof(IMMDeviceEnumerator), (void**)&g_enumerator);
        if (FAILED(hr))
        {
            g_enumerator = nullptr;
            SP_LogError(L"The audio device enumerator could not be created: 0x%08X", (unsigned)hr);
        }
    }
    IMMDeviceEnumerator* enumerator = g_enumerator;
    if (enumerator)
    {
        enumerator->AddRef();
    }
    ReleaseSRWLockExclusive(&g_audioLock);

    if (!enumerator)
    {
        return nullptr;
    }

    IAudioEndpointVolume* volume = nullptr;
    IMMDevice* device = nullptr;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) && device)
    {
        if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr, (void**)&volume)))
        {
            volume = nullptr;
        }
        device->Release();
    }
    enumerator->Release();
    return volume;
}

void ReleaseAudio()
{
    AcquireSRWLockExclusive(&g_audioLock);
    if (g_enumerator)
    {
        g_enumerator->Release();
        g_enumerator = nullptr;
    }
    ReleaseSRWLockExclusive(&g_audioLock);
}

// Moves the master volume by a signed number of percent, clamped to 0..100.
bool AddVolumePercent(int percent)
{
    IAudioEndpointVolume* volume = OpenEndpointVolume();
    if (!volume)
    {
        return false;
    }

    bool ok = false;
    float level = 0.0f;
    if (SUCCEEDED(volume->GetMasterVolumeLevelScalar(&level)))
    {
        level += (float)percent / 100.0f;
        if (level < 0.0f)
        {
            level = 0.0f;
        }
        else if (level > 1.0f)
        {
            level = 1.0f;
        }
        ok = SUCCEEDED(volume->SetMasterVolumeLevelScalar(level, nullptr));
    }
    volume->Release();
    return ok;
}

bool ToggleMute()
{
    IAudioEndpointVolume* volume = OpenEndpointVolume();
    if (!volume)
    {
        return false;
    }

    bool ok = false;
    BOOL muted = FALSE;
    if (SUCCEEDED(volume->GetMute(&muted)))
    {
        ok = SUCCEEDED(volume->SetMute(!muted, nullptr));
        SP_LogDebug(L"Mute %s", muted ? L"off" : L"on");
    }
    volume->Release();
    return ok;
}

// ---------------------------------------------------------------------------------------------------------------
// Changing the volume the way a volume key does
// ---------------------------------------------------------------------------------------------------------------

// Hands the shell one volume key press. The shell-hook window under the primary taskbar is where the system
// delivers HSHELL_APPCOMMAND for the media keys; posting the same message there makes the shell change the
// volume by its own 2% and show the volume flyout.
bool PostVolumeAppCommand(SHORT command)
{
    HWND hTaskbar = g_hTaskbar.load(std::memory_order_relaxed);
    if (!hTaskbar || !g_shellHookMsg)
    {
        return false;
    }
    HWND hRebar = FindWindowExW(hTaskbar, nullptr, L"ReBarWindow32", nullptr);
    HWND hShellHook = hRebar ? FindWindowExW(hRebar, nullptr, L"MSTaskSwWClass", nullptr) : nullptr;
    if (!hShellHook)
    {
        return false;
    }
    return PostMessageW(hShellHook, g_shellHookMsg, HSHELL_APPCOMMAND, MAKELPARAM(0, command)) != FALSE;
}

// Applies one wheel movement. Runs on the taskbar thread.
void ScrollVolume(int delta)
{
    DWORD now = GetTickCount();
    if (now - g_lastScrollTick.load(std::memory_order_relaxed) < 5000)
    {
        delta += g_scrollRemainder.load(std::memory_order_relaxed);
    }
    g_lastScrollTick.store(now, std::memory_order_relaxed);
    g_scrollRemainder.store(delta % WHEEL_DELTA, std::memory_order_relaxed);

    int notches = delta / WHEEL_DELTA;
    if (notches == 0)
    {
        return;
    }

    int step = g_volumeStep.load(std::memory_order_relaxed);
    int total = notches * step;                    // signed percent
    SHORT command = total > 0 ? APPCOMMAND_VOLUME_UP : APPCOMMAND_VOLUME_DOWN;
    int shellPart = total > 0 ? 2 : -2;            // what the shell adds for one key press

    SP_LogDebug(L"%d notch(es), %d%%", notches, total);

    // Everything except the shell's own 2% goes to the endpoint first, so the flyout shows the final value.
    if (total != shellPart)
    {
        AddVolumePercent(total - shellPart);
    }

    if (!PostVolumeAppCommand(command))
    {
        // No shell-hook window on this build: finish the change directly, without the flyout.
        AddVolumePercent(shellPart);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// The scroll area
// ---------------------------------------------------------------------------------------------------------------

// The notification area of a taskbar, in screen coordinates. On the primary taskbar it is the TrayNotifyWnd child,
// which on Windows 11 still carries the real rectangle of the tray. A secondary taskbar has no such window; its
// clock and tray live in a second XAML bridge whose rectangle is not the whole bar. Failing both, the last 50 DIPs
// of the bar are taken, which is rough but better than nothing.
bool GetTrayAreaRect(HWND hTaskbarWnd, RECT* rc)
{
    if (hTaskbarWnd == g_hTaskbar.load(std::memory_order_relaxed))
    {
        HWND hTray = FindWindowExW(hTaskbarWnd, nullptr, L"TrayNotifyWnd", nullptr);
        if (hTray && GetWindowRect(hTray, rc) && !IsRectEmpty(rc))
        {
            return true;
        }
    }
    else
    {
        RECT rcTaskbar;
        if (GetWindowRect(hTaskbarWnd, &rcTaskbar))
        {
            HWND hBridge = nullptr;
            while ((hBridge = FindWindowExW(hTaskbarWnd, hBridge,
                                            L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr)) != nullptr)
            {
                RECT rcBridge;
                if (!GetWindowRect(hBridge, &rcBridge))
                {
                    break;
                }
                if (!EqualRect(&rcBridge, &rcTaskbar))
                {
                    if (IsRectEmpty(&rcBridge))
                    {
                        break;
                    }
                    *rc = rcBridge;
                    return true;
                }
            }
        }
    }

    RECT rcTaskbar;
    if (!GetWindowRect(hTaskbarWnd, &rcTaskbar))
    {
        return false;
    }

    int tail = MulDiv(50, DpiForWindow(hTaskbarWnd), 96);
    *rc = rcTaskbar;
    if (rc->right - rc->left > tail)
    {
        if (GetWindowLongW(hTaskbarWnd, GWL_EXSTYLE) & WS_EX_LAYOUTRTL)
        {
            rc->right = rc->left + tail;
        }
        else
        {
            rc->left = rc->right - tail;
        }
    }
    return true;
}

bool IsPointInScrollArea(HWND hTaskbarWnd, POINT pt)
{
    RECT rc;
    switch (g_scrollArea.load(std::memory_order_relaxed))
    {
    case ScrollArea::WholeTaskbar:
        return GetWindowRect(hTaskbarWnd, &rc) && PtInRect(&rc, pt);

    case ScrollArea::TrayOnly:
        return GetTrayAreaRect(hTaskbarWnd, &rc) && PtInRect(&rc, pt);
    }
    return false;
}

// A wheel message over a taskbar window. Returns true when it was used, in which case the caller swallows it.
// wParam and lParam have the WM_MOUSEWHEEL layout, which WM_POINTERWHEEL shares: delta in the high word of
// wParam, screen position in lParam.
bool OnWheel(HWND hTaskbarWnd, WPARAM wParam, LPARAM lParam)
{
    if (!g_active.load(std::memory_order_relaxed))
    {
        return false;
    }

    // A drag in progress owns the mouse; leave it alone.
    if (GetCapture())
    {
        return false;
    }

    POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    if (!IsPointInScrollArea(hTaskbarWnd, pt))
    {
        return false;
    }

    // An empty input event, as the original mod sends: it marks this process as the last to have produced
    // input, which is what lets the shell bring the volume flyout forward.
    INPUT nudge = {};
    SendInput(1, &nudge, sizeof(nudge));

    ScrollVolume(GET_WHEEL_DELTA_WPARAM(wParam));
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// The InputSite window procedure hook
// ---------------------------------------------------------------------------------------------------------------

WNDPROC g_origInputSiteProc = nullptr;

LRESULT CALLBACK InputSiteProc_Hook(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (uMsg == WM_POINTERWHEEL)
    {
        HWND hRoot = GetAncestor(hWnd, GA_ROOT);
        if (hRoot && IsTaskbarWindow(hRoot) && OnWheel(hRoot, wParam, lParam))
        {
            return 0;
        }
    }
    return g_origInputSiteProc(hWnd, uMsg, wParam, lParam);
}

// Hooks the procedure behind an InputSite window. One hook serves every InputSite window in the process, so
// this runs once; a second taskbar's InputSite is already covered.
void HookInputSiteProc(HWND hInputSite)
{
    if (g_inputSiteHooked.load(std::memory_order_relaxed))
    {
        return;
    }

    WNDPROC proc = (WNDPROC)GetWindowLongPtrW(hInputSite, GWLP_WNDPROC);
    if (!proc)
    {
        SP_LogError(L"The InputSite window procedure could not be read: %lu", GetLastError());
        return;
    }

    if (g_inputSiteHooked.exchange(true))
    {
        return;
    }

    if (!SP_SetFunctionHookNow(proc, InputSiteProc_Hook, &g_origInputSiteProc))
    {
        g_inputSiteHooked.store(false);
        SP_LogError(L"The InputSite window procedure could not be hooked; falling back to WM_MOUSEWHEEL");
        return;
    }

    SP_Log(L"Hooked the InputSite window procedure at %p", proc);
}

// The InputSite that belongs to a taskbar sits two levels down: taskbar > bridge > InputSite.
bool IsTaskbarInputSite(HWND hInputSite)
{
    HWND hBridge = GetParent(hInputSite);
    if (!hBridge || !ClassNameIs(hBridge, L"Windows.UI.Composition.DesktopWindowContentBridge"))
    {
        return false;
    }
    HWND hRoot = GetParent(hBridge);
    return hRoot && IsTaskbarWindow(hRoot);
}

void HookInputSiteUnder(HWND hTaskbarWnd)
{
    if (g_inputSiteHooked.load(std::memory_order_relaxed))
    {
        return;
    }
    HWND hBridge = FindWindowExW(hTaskbarWnd, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr);
    HWND hInputSite = hBridge
        ? FindWindowExW(hBridge, nullptr, L"Windows.UI.Input.InputSite.WindowClass", nullptr)
        : nullptr;
    if (hInputSite)
    {
        HookInputSiteProc(hInputSite);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// The taskbar subclass
// ---------------------------------------------------------------------------------------------------------------

LRESULT CALLBACK TaskbarSubclass(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                 UINT_PTR idSubclass, DWORD_PTR refData)
{
    UNREFERENCED_PARAMETER(idSubclass);
    UNREFERENCED_PARAMETER(refData);

    if (uMsg == WM_MOUSEWHEEL)
    {
        // Only a fallback: with the InputSite hook in place the wheel was already handled there.
        if (!g_inputSiteHooked.load(std::memory_order_relaxed) && OnWheel(hWnd, wParam, lParam))
        {
            return 0;
        }
    }
    else if (g_scrollAnywhereMsg && uMsg == g_scrollAnywhereMsg)
    {
        if (g_active.load(std::memory_order_relaxed))
        {
            ScrollVolume((short)HIWORD(wParam));
        }
        return 0;
    }
    else if (uMsg == WM_NCDESTROY)
    {
        RemoveWindowSubclass(hWnd, TaskbarSubclass, kSubclassId);
        if (hWnd == g_hTaskbar.load(std::memory_order_relaxed))
        {
            g_hTaskbar.store(nullptr);
        }
        else
        {
            ForgetSecondary(hWnd);
        }
    }

    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

void SubclassTaskbar(HWND hWnd)
{
    DWORD_PTR existing = 0;
    DWORD threadId = GetWindowThreadProcessId(hWnd, nullptr);

    BOOL ok;
    if (threadId == GetCurrentThreadId())
    {
        // Called from the window's own thread (the creation hooks), where the plain call is the right one.
        ok = GetWindowSubclass(hWnd, TaskbarSubclass, kSubclassId, &existing) ||
             SetWindowSubclass(hWnd, TaskbarSubclass, kSubclassId, 0);
    }
    else
    {
        ok = SP_SetWindowSubclassFromAnyThread(hWnd, TaskbarSubclass, kSubclassId, 0);
    }

    if (!ok)
    {
        SP_LogError(L"Taskbar window %p could not be subclassed", hWnd);
    }
}

void UnsubclassTaskbar(HWND hWnd)
{
    if (hWnd && IsWindow(hWnd))
    {
        SP_RemoveWindowSubclassFromAnyThread(hWnd, TaskbarSubclass, kSubclassId);
    }
}

// A taskbar window has appeared, or was already there when the mod started.
void OnTaskbarFound(HWND hWnd, bool primary)
{
    if (!g_active.load(std::memory_order_relaxed))
    {
        return;
    }

    DWORD threadId = GetWindowThreadProcessId(hWnd, nullptr);
    if (primary)
    {
        g_hTaskbar.store(hWnd);
        g_taskbarThreadId.store(threadId);
        SP_Log(L"Taskbar window %p", hWnd);
    }
    else
    {
        // A secondary taskbar always shares the primary's thread; anything else is not the shell's.
        DWORD primaryThread = g_taskbarThreadId.load(std::memory_order_relaxed);
        if (primaryThread && threadId != primaryThread)
        {
            return;
        }
        RememberSecondary(hWnd);
        SP_Log(L"Secondary taskbar window %p", hWnd);
    }

    SubclassTaskbar(hWnd);
    HookInputSiteUnder(hWnd);
}

// ---------------------------------------------------------------------------------------------------------------
// Catching windows as they are created
// ---------------------------------------------------------------------------------------------------------------

void OnWindowCreated(HWND hWnd, LPCWSTR lpClassName)
{
    // A class may be given as an atom rather than a string; those are never the ones wanted here.
    if (!hWnd || ((ULONG_PTR)lpClassName & ~(ULONG_PTR)0xffff) == 0)
    {
        return;
    }
    if (!g_active.load(std::memory_order_relaxed))
    {
        return;
    }

    if (_wcsicmp(lpClassName, L"Shell_TrayWnd") == 0)
    {
        OnTaskbarFound(hWnd, true);
    }
    else if (_wcsicmp(lpClassName, L"Shell_SecondaryTrayWnd") == 0)
    {
        OnTaskbarFound(hWnd, false);
    }
    else if (_wcsicmp(lpClassName, L"Windows.UI.Input.InputSite.WindowClass") == 0)
    {
        if (!g_inputSiteHooked.load(std::memory_order_relaxed) && IsTaskbarInputSite(hWnd))
        {
            HookInputSiteProc(hWnd);
        }
    }
}

using CreateWindowExW_t = decltype(&CreateWindowExW);
CreateWindowExW_t g_origCreateWindowExW = nullptr;

HWND WINAPI CreateWindowExW_Hook(DWORD dwExStyle, LPCWSTR lpClassName, LPCWSTR lpWindowName, DWORD dwStyle,
                                 int X, int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu,
                                 HINSTANCE hInstance, LPVOID lpParam)
{
    HWND hWnd = g_origCreateWindowExW(dwExStyle, lpClassName, lpWindowName, dwStyle, X, Y, nWidth, nHeight,
                                      hWndParent, hMenu, hInstance, lpParam);
    OnWindowCreated(hWnd, lpClassName);
    return hWnd;
}

// The XAML island's windows are made with this undocumented user32 export rather than CreateWindowExW.
using CreateWindowInBand_t = HWND(WINAPI*)(DWORD dwExStyle, LPCWSTR lpClassName, LPCWSTR lpWindowName,
                                           DWORD dwStyle, int X, int Y, int nWidth, int nHeight,
                                           HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam,
                                           DWORD dwBand);
CreateWindowInBand_t g_origCreateWindowInBand = nullptr;

HWND WINAPI CreateWindowInBand_Hook(DWORD dwExStyle, LPCWSTR lpClassName, LPCWSTR lpWindowName, DWORD dwStyle,
                                    int X, int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu,
                                    HINSTANCE hInstance, LPVOID lpParam, DWORD dwBand)
{
    HWND hWnd = g_origCreateWindowInBand(dwExStyle, lpClassName, lpWindowName, dwStyle, X, Y, nWidth, nHeight,
                                         hWndParent, hMenu, hInstance, lpParam, dwBand);
    OnWindowCreated(hWnd, lpClassName);
    return hWnd;
}

// Taskbar windows that exist before the mod started. The primary is handled first so the secondaries can be
// checked against its thread.
void FindExistingTaskbars()
{
    struct Found
    {
        HWND primary = nullptr;
        HWND secondary[kMaxSecondary] = {};
        int  secondaryCount = 0;
    } found;

    EnumWindows([](HWND hWnd, LPARAM lParam) -> BOOL {
        Found& f = *(Found*)lParam;
        DWORD processId = 0;
        GetWindowThreadProcessId(hWnd, &processId);
        if (processId != GetCurrentProcessId())
        {
            return TRUE;
        }
        if (ClassNameIs(hWnd, L"Shell_TrayWnd"))
        {
            f.primary = hWnd;
        }
        else if (ClassNameIs(hWnd, L"Shell_SecondaryTrayWnd") && f.secondaryCount < kMaxSecondary)
        {
            f.secondary[f.secondaryCount++] = hWnd;
        }
        return TRUE;
    }, (LPARAM)&found);

    if (found.primary)
    {
        OnTaskbarFound(found.primary, true);
    }
    for (int i = 0; i < found.secondaryCount; ++i)
    {
        OnTaskbarFound(found.secondary[i], false);
    }

    if (!found.primary)
    {
        SP_Log(L"No taskbar yet; waiting for it to be created");
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Middle click on the volume icon
// ---------------------------------------------------------------------------------------------------------------

// The return type is int to fit both spellings: the produce<> COM thunk returns an HRESULT and the plain
// implementation returns void, whose caller ignores whatever is in the register.
using OnIconClicked_t = int(WINAPI*)(void* pThis, void* args);
OnIconClicked_t g_origOnIconClicked = nullptr;

int WINAPI OnIconClicked_Hook(void* pThis, void* args)
{
    if (g_active.load(std::memory_order_relaxed) &&
        g_middleClickMute.load(std::memory_order_relaxed) &&
        GetKeyState(VK_MBUTTON) < 0)
    {
        SP_LogDebug(L"Middle click on the volume icon");
        ToggleMute();
        return 0;   // S_OK
    }
    return g_origOnIconClicked(pThis, args);
}

// In SystemTray.dll the implementation was inlined into the COM thunk; in Taskbar.View.dll it stands alone.
// Both spellings are listed for each module and the first that resolves is used.
const wchar_t* const kOnIconClickedNamesSystemTray[] =
{
    L"public: virtual int __cdecl winrt::impl::produce<struct winrt::SystemTray::implementation::VolumeSystemTrayIconDataModel,struct winrt::SystemTray::IIconDataModel>::OnIconClicked(void *)",
    L"public: void __cdecl winrt::SystemTray::implementation::VolumeSystemTrayIconDataModel::OnIconClicked(struct winrt::SystemTray::IconClickedEventArgs const &)",
};

const wchar_t* const kOnIconClickedNamesTaskbarView[] =
{
    L"public: void __cdecl winrt::SystemTray::implementation::VolumeSystemTrayIconDataModel::OnIconClicked(struct winrt::SystemTray::IconClickedEventArgs const &)",
    L"public: virtual int __cdecl winrt::impl::produce<struct winrt::SystemTray::implementation::VolumeSystemTrayIconDataModel,struct winrt::SystemTray::IIconDataModel>::OnIconClicked(void *)",
};

void HookTrayIconClick(HMODULE hModule, bool isSystemTrayDll, const wchar_t* moduleName)
{
    if (g_origOnIconClicked)
    {
        return;     // already hooked
    }

    SP_SymbolHook hooks[1] = {};
    hooks[0].symbols = isSystemTrayDll ? kOnIconClickedNamesSystemTray : kOnIconClickedNamesTaskbarView;
    hooks[0].symbolCount = isSystemTrayDll ? ARRAYSIZE(kOnIconClickedNamesSystemTray)
                                           : ARRAYSIZE(kOnIconClickedNamesTaskbarView);
    hooks[0].pOriginal = (void**)&g_origOnIconClicked;
    hooks[0].hookFunction = (void*)OnIconClicked_Hook;
    hooks[0].optional = FALSE;

    if (!SP_HookSymbols(hModule, hooks, ARRAYSIZE(hooks)))
    {
        g_origOnIconClicked = nullptr;
        SP_LogError(L"The volume icon's click handler was not found in %s; middle click to mute is off", moduleName);
        return;
    }

    SP_Log(L"Watching the volume icon's click handler in %s", moduleName);
}

void OnSystemTrayLoaded(HMODULE hModule, void*)
{
    HookTrayIconClick(hModule, true, L"SystemTray.dll");
}

// Runs on a helper thread once Taskbar.View.dll is in the process. The tray types moved from Taskbar.View.dll to
// SystemTray.dll at version 2604; the version decides which module to look in.
void OnTaskbarViewLoaded(HMODULE hTaskbarView, void*)
{
    if (HMODULE hSystemTray = GetModuleHandleW(L"SystemTray.dll"))
    {
        HookTrayIconClick(hSystemTray, true, L"SystemTray.dll");
        return;
    }

    WORD major = ModuleMajorVersion(hTaskbarView);
    if (major && major < 2604)
    {
        HookTrayIconClick(hTaskbarView, false, L"Taskbar.View.dll");
        return;
    }

    // Not loaded yet: the taskbar brings SystemTray.dll in a moment after its own library.
    SP_Log(L"Taskbar.View.dll %u; waiting for SystemTray.dll", major);
    if (!SP_WaitForModule(L"SystemTray.dll", 60000, OnSystemTrayLoaded, nullptr))
    {
        SP_LogError(L"Could not wait for SystemTray.dll; middle click to mute is off");
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Scroll anywhere: a low-level mouse hook on its own thread
//
// A low-level hook procedure runs on the thread that installed it, which must pump messages, and must return
// quickly or Windows silently drops the hook. So it does nothing but post the wheel to the primary taskbar, whose
// subclass does the volume work.
// ---------------------------------------------------------------------------------------------------------------

HANDLE g_hookThread = nullptr;
DWORD  g_hookThreadId = 0;

bool ModifiersMatch(int wanted)
{
    bool ctrl = GetAsyncKeyState(VK_CONTROL) < 0;
    bool shift = GetAsyncKeyState(VK_SHIFT) < 0;
    bool alt = GetAsyncKeyState(VK_MENU) < 0;
    bool win = GetAsyncKeyState(VK_LWIN) < 0 || GetAsyncKeyState(VK_RWIN) < 0;

    // Exactly the chosen keys and nothing else, so Alt+wheel and Win+wheel shortcuts keep working.
    return ((wanted & kModifierCtrl) != 0) == ctrl &&
           ((wanted & kModifierShift) != 0) == shift &&
           !alt && !win;
}

LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && wParam == WM_MOUSEWHEEL && g_active.load(std::memory_order_relaxed))
    {
        int wanted = g_scrollAnywhereModifier.load(std::memory_order_relaxed);
        HWND hTaskbar = g_hTaskbar.load(std::memory_order_relaxed);
        if (wanted && hTaskbar && ModifiersMatch(wanted))
        {
            const MSLLHOOKSTRUCT* info = (const MSLLHOOKSTRUCT*)lParam;
            PostMessageW(hTaskbar, g_scrollAnywhereMsg,
                         MAKEWPARAM(0, HIWORD(info->mouseData)), MAKELPARAM(info->pt.x, info->pt.y));
            return 1;   // swallowed: the window under the cursor does not scroll
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

DWORD WINAPI MouseHookThread(LPVOID lpParameter)
{
    HANDLE hReady = (HANDLE)lpParameter;

    HMODULE hSelf = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&MouseHookThread, &hSelf);

    HHOOK hHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, hSelf, 0);
    DWORD error = hHook ? 0 : GetLastError();

    // The message queue exists once a message function has been called; do that before reporting ready so a
    // WM_QUIT posted right away is not lost.
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    SetEvent(hReady);

    if (!hHook)
    {
        SP_LogError(L"The mouse hook could not be set: %lu", error);
        return 1;
    }

    SP_Log(L"Scroll anywhere is on");
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnhookWindowsHookEx(hHook);
    SP_Log(L"Scroll anywhere is off");
    return 0;
}

void StartMouseHook()
{
    if (g_hookThread)
    {
        return;
    }

    HANDLE hReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!hReady)
    {
        return;
    }

    g_hookThread = CreateThread(nullptr, 0, MouseHookThread, hReady, 0, &g_hookThreadId);
    if (g_hookThread)
    {
        WaitForSingleObject(hReady, 5000);
    }
    CloseHandle(hReady);
}

void StopMouseHook()
{
    if (!g_hookThread)
    {
        return;
    }

    PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
    WaitForSingleObject(g_hookThread, 5000);
    CloseHandle(g_hookThread);
    g_hookThread = nullptr;
    g_hookThreadId = 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------------------------

void LoadSettings()
{
    int step = SP_GetIntSetting(L"VolumeStep", 2);
    if (step < 1)
    {
        step = 1;
    }
    else if (step > 50)
    {
        step = 50;
    }
    g_volumeStep.store(step, std::memory_order_relaxed);

    int area = SP_GetIntSetting(L"ScrollAreaMode", 0);
    g_scrollArea.store(area == 1 ? ScrollArea::TrayOnly : ScrollArea::WholeTaskbar, std::memory_order_relaxed);

    g_middleClickMute.store(SP_GetIntSetting(L"MiddleClickMute", 1) != 0, std::memory_order_relaxed);

    int modifier = SP_GetIntSetting(L"ScrollAnywhereModifier", 0) & (kModifierCtrl | kModifierShift);
    g_scrollAnywhereModifier.store(modifier, std::memory_order_relaxed);

    SP_Log(L"Step %d%%, area %d, middle click mute %d, scroll anywhere modifier %d",
           step, area, g_middleClickMute.load() ? 1 : 0, modifier);
}

BOOL Init()
{
    LoadSettings();
    g_active.store(true);

    g_scrollAnywhereMsg = RegisterWindowMessageW(L"ShadePatcher." SP_MOD_ID ".ScrollAnywhere");
    g_shellHookMsg = RegisterWindowMessageW(L"SHELLHOOK");

    // Both creation paths go in one transaction: a taskbar caught by one but not the other would be half done.
    if (!SP_HookBegin())
    {
        return FALSE;
    }

    if (!SP_SetExportHook(L"user32.dll", "CreateWindowExW", CreateWindowExW_Hook, &g_origCreateWindowExW))
    {
        SP_HookAbort();
        SP_LogError(L"CreateWindowExW could not be hooked");
        return FALSE;
    }

    // Undocumented, so its absence is not fatal: the InputSite is then only found when a taskbar window is
    // handled, which covers every case but a cold sign-in on a build without the export.
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    auto pCreateWindowInBand = hUser32 ? (CreateWindowInBand_t)GetProcAddress(hUser32, "CreateWindowInBand") : nullptr;
    if (pCreateWindowInBand)
    {
        if (!SP_SetFunctionHook(pCreateWindowInBand, CreateWindowInBand_Hook, &g_origCreateWindowInBand))
        {
            SP_LogError(L"CreateWindowInBand could not be hooked");
        }
    }
    else
    {
        SP_Log(L"CreateWindowInBand is not exported on this build");
    }

    if (!SP_HookCommit())
    {
        return FALSE;
    }

    // The volume icon's click handler lives in a library the taskbar loads after the shell starts.
    if (!SP_WaitForModule(L"Taskbar.View.dll", 60000, OnTaskbarViewLoaded, nullptr))
    {
        SP_LogError(L"Could not wait for Taskbar.View.dll; middle click to mute is off");
    }

    return TRUE;
}

void AfterInit()
{
    // Every window made from here on comes through the hooks; the ones already up are picked up now.
    FindExistingTaskbars();

    if (g_scrollAnywhereModifier.load(std::memory_order_relaxed))
    {
        StartMouseHook();
    }
}

void SettingsChanged()
{
    int before = g_scrollAnywhereModifier.load(std::memory_order_relaxed);
    LoadSettings();
    int after = g_scrollAnywhereModifier.load(std::memory_order_relaxed);

    // The hook procedure reads the modifier itself, so only turning the feature on or off needs the thread.
    if (before && !after)
    {
        StopMouseHook();
    }
    else if (!before && after)
    {
        StartMouseHook();
    }
}

void BeforeUninit()
{
    // Hooks are still live; make them inert first so nothing is subclassed or changed while unwinding.
    g_active.store(false);

    StopMouseHook();

    HWND secondary[kMaxSecondary];
    int n = SnapshotSecondary(secondary, kMaxSecondary);
    for (int i = 0; i < n; ++i)
    {
        UnsubclassTaskbar(secondary[i]);
    }
    UnsubclassTaskbar(g_hTaskbar.load());

    AcquireSRWLockExclusive(&g_secondaryLock);
    g_secondaryCount = 0;
    ReleaseSRWLockExclusive(&g_secondaryLock);
    g_hTaskbar.store(nullptr);
    g_taskbarThreadId.store(0);
}

void Uninit()
{
    // The function hooks (window creation, InputSite, the icon click) are gone by now.
    g_inputSiteHooked.store(false);
    g_origInputSiteProc = nullptr;
    g_origOnIconClicked = nullptr;
    ReleaseAudio();
}

}   // namespace

SP_MOD_DEFINE(g_modTaskbarVolumeControl) =
{
    /* id             */ SP_MOD_ID,
    /* name           */ L"Change the volume by scrolling over the taskbar",
    /* basedOn        */ "taskbar-volume-control",
    /* originalAuthor */ "m417z",
    /* targets        */ SP_TARGET_EXPLORER,
    /* minOsBuild     */ 22000,
    /* flags          */ 0,
    /* Init           */ Init,
    /* AfterInit      */ AfterInit,
    /* SettingsChanged*/ SettingsChanged,
    /* BeforeUninit   */ BeforeUninit,
    /* Uninit         */ Uninit,
};
