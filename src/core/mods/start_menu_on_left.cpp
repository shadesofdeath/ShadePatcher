//
// start_menu_on_left - the Start menu half of taskbar-start-button-position: opens the Start menu at the left edge
// of the screen while the taskbar icons stay centered.
//
// Adapted from the idea behind the Windhawk mod "Start button always on the left"
// (taskbar-start-button-position) by m417z, whose Start menu part runs in StartMenuExperienceHost.exe. The logic
// here is written against this engine's API.
//
// Where it runs
// -------------
// The Start menu is drawn by StartMenuExperienceHost.exe, not by explorer.exe, so this is a separate mod entry
// with the SP_TARGET_STARTMENU target. It shares its id (and so its settings key and its Enabled switch) with the
// taskbar half in taskbar_start_button_position.cpp; the explorer engine loads the core DLL into the Start menu's
// process while it is enabled (engine/hostinject.c). It acts only while "StartMenuOnLeft" is on.
//
// What it does
// ------------
// The menu is a XAML tree whose root is the content of the host's Window:
//
//     Canvas > StartDocked.StartSizingFrame     the classic Windows 11 menu: placed by Canvas.Left
//     StartMenu.StartBlendedFlexFrame > FrameRoot   the redesigned menu: placed by HorizontalAlignment
//
// The frame is moved to the left (Canvas.Left = 12, or HorizontalAlignment = Left). Windows puts the old value back
// when the menu opens on another monitor or its size changes, so the property is watched and the value re-applied.
// The pass runs every time the window becomes visible, and the old values are restored when the option or the mod
// is turned off.
//
// Reaching the UI thread
// ----------------------
// Window::Current() and every element only work on the Start menu's UI thread. The DLL usually arrives after the
// menu was built, so the engine thread gets there through a thread-local WH_CALLWNDPROC hook on the thread of the
// host's CoreWindow and a message sent to that window. When the host is started fresh, the menu is caught instead
// as it creates its XAML island (RoGetActivationFactory for Windows.UI.Xaml.Hosting.XamlIsland, on the UI thread).
//
// Exceptions
// ----------
// C++/WinRT reports failures by throwing, so this file is compiled with exceptions on. Every hook body, event
// handler and callback is wrapped: nothing may escape into the Start menu.
//
#define SP_MOD_ID "taskbar-start-button-position"
#include "engine/modapi.h"

#include <roapi.h>
#include <winstring.h>

#include <atomic>
#include <functional>
#include <optional>

#undef GetCurrentTime

#include <unknwn.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>

namespace {

using namespace winrt::Windows::UI::Xaml;
using winrt::Windows::UI::Xaml::Media::VisualTreeHelper;

// How far from the screen edge the classic menu is placed, in the menu's own units (the original's value).
constexpr double kStartMenuMargin = 12.0;

// ---------------------------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------------------------

// Written on the engine thread, read on the UI thread.
std::atomic<bool> g_onLeft{ true };
std::atomic<bool> g_unloading{ false };

// UI thread only.
bool                              g_attached = false;
bool                              g_inApplyStyle = false;
winrt::event_token                g_visibilityToken{};
std::optional<double>             g_previousCanvasLeft;
winrt::weak_ref<DependencyObject> g_sizingFrame;
int64_t                           g_canvasLeftToken = 0;
int64_t                           g_canvasTopToken = 0;
std::optional<HorizontalAlignment> g_previousAlignment;
winrt::weak_ref<DependencyObject> g_frameRoot;
int64_t                           g_alignmentToken = 0;

bool WantLeft()
{
    return g_onLeft.load(std::memory_order_relaxed) && !g_unloading.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------------------------------
// Visual tree helpers
// ---------------------------------------------------------------------------------------------------------------

// The first element below `parent`, at most `depth` levels down, for which `match` is true.
FrameworkElement FindDescendant(DependencyObject const& parent, int depth,
                                std::function<bool(FrameworkElement const&)> const& match)
{
    if (!parent || depth <= 0)
    {
        return nullptr;
    }
    const int count = VisualTreeHelper::GetChildrenCount(parent);
    for (int i = 0; i < count; ++i)
    {
        auto child = VisualTreeHelper::GetChild(parent, i);
        if (auto element = child.try_as<FrameworkElement>())
        {
            if (match(element))
            {
                return element;
            }
        }
    }
    for (int i = 0; i < count; ++i)
    {
        if (auto found = FindDescendant(VisualTreeHelper::GetChild(parent, i), depth - 1, match))
        {
            return found;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------------------------------
// The two menu layouts
// ---------------------------------------------------------------------------------------------------------------

void ApplyStyle();

// Classic menu: Canvas > StartDocked.StartSizingFrame, placed with Canvas.Left.
void StyleClassicMenu(FrameworkElement const& content)
{
    FrameworkElement frame = FindDescendant(content, 2, [](FrameworkElement const& e) {
        return winrt::get_class_name(e) == L"StartDocked.StartSizingFrame";
    });
    if (!frame)
    {
        SP_LogDebug(L"StartDocked.StartSizingFrame was not found");
        return;
    }

    if (!WantLeft())
    {
        if (g_previousCanvasLeft)
        {
            SP_LogDebug(L"Canvas.Left back to %.1f", *g_previousCanvasLeft);
            Controls::Canvas::SetLeft(frame, *g_previousCanvasLeft);
        }
        return;
    }

    if (!g_previousCanvasLeft)
    {
        // Zero means the menu has not been placed yet; the real value is taken on a later pass.
        const double left = Controls::Canvas::GetLeft(frame);
        if (left != 0.0)
        {
            g_previousCanvasLeft = left;
        }
    }
    if (Controls::Canvas::GetLeft(frame) != kStartMenuMargin)
    {
        SP_LogDebug(L"Canvas.Left %.1f -> %.1f", Controls::Canvas::GetLeft(frame), kStartMenuMargin);
        Controls::Canvas::SetLeft(frame, kStartMenuMargin);
    }

    // Windows re-places the menu when it opens on another monitor or its size changes; a moved Canvas.Top is the
    // sign of that, and a moved Canvas.Left is Windows undoing ours. Either way the pass runs again.
    if (!g_sizingFrame.get())
    {
        g_sizingFrame = winrt::make_weak(frame.as<DependencyObject>());
        auto onMoved = [](DependencyObject const&, DependencyProperty const&) {
            try
            {
                if (!g_inApplyStyle && !g_unloading.load(std::memory_order_relaxed))
                {
                    ApplyStyle();
                }
            }
            catch (...)
            {
            }
        };
        g_canvasLeftToken = frame.RegisterPropertyChangedCallback(Controls::Canvas::LeftProperty(), onMoved);
        g_canvasTopToken = frame.RegisterPropertyChangedCallback(Controls::Canvas::TopProperty(), onMoved);
    }
}

// Redesigned menu: StartMenu.StartBlendedFlexFrame > FrameRoot, placed with HorizontalAlignment.
void StyleRedesignedMenu(FrameworkElement const& content)
{
    FrameworkElement frameRoot = FindDescendant(content, 3, [](FrameworkElement const& e) {
        return e.Name() == L"FrameRoot";
    });
    if (!frameRoot)
    {
        SP_LogDebug(L"The Start menu's FrameRoot was not found");
        return;
    }

    if (!WantLeft())
    {
        frameRoot.HorizontalAlignment(g_previousAlignment.value_or(HorizontalAlignment::Center));
        return;
    }

    if (!g_previousAlignment)
    {
        g_previousAlignment = frameRoot.HorizontalAlignment();
    }
    if (frameRoot.HorizontalAlignment() != HorizontalAlignment::Left)
    {
        SP_LogDebug(L"FrameRoot aligned left");
        frameRoot.HorizontalAlignment(HorizontalAlignment::Left);
    }

    if (!g_frameRoot.get())
    {
        g_frameRoot = winrt::make_weak(frameRoot.as<DependencyObject>());
        g_alignmentToken = frameRoot.RegisterPropertyChangedCallback(
            FrameworkElement::HorizontalAlignmentProperty(), [](DependencyObject const&, DependencyProperty const&) {
                try
                {
                    if (!g_inApplyStyle && !g_unloading.load(std::memory_order_relaxed))
                    {
                        ApplyStyle();
                    }
                }
                catch (...)
                {
                }
            });
    }
}

// UI thread. One pass over the menu, applying or restoring.
void ApplyStyle()
{
    Window window = Window::Current();
    if (!window)
    {
        return;
    }
    FrameworkElement content = window.Content().try_as<FrameworkElement>();
    if (!content)
    {
        return;
    }

    g_inApplyStyle = true;
    try
    {
        const auto className = winrt::get_class_name(content);
        if (className == L"Windows.UI.Xaml.Controls.Canvas")
        {
            StyleClassicMenu(content);
        }
        else if (className == L"StartMenu.StartBlendedFlexFrame")
        {
            StyleRedesignedMenu(content);
        }
        else
        {
            SP_LogDebug(L"Unfamiliar Start menu content %s", className.c_str());
        }
    }
    catch (winrt::hresult_error const& e)
    {
        SP_LogError(L"Placing the Start menu failed: 0x%08X", (unsigned)e.code());
    }
    catch (...)
    {
        SP_LogError(L"Placing the Start menu failed");
    }
    g_inApplyStyle = false;
}

// ---------------------------------------------------------------------------------------------------------------
// Attaching to and detaching from the menu (UI thread)
// ---------------------------------------------------------------------------------------------------------------

void Attach()
{
    if (g_attached || g_unloading.load(std::memory_order_relaxed))
    {
        return;
    }
    Window window = Window::Current();
    if (!window)
    {
        return;
    }

    g_visibilityToken = window.VisibilityChanged(
        [](winrt::Windows::Foundation::IInspectable const&, winrt::Windows::UI::Core::VisibilityChangedEventArgs const& args) {
            try
            {
                if (args.Visible() && !g_unloading.load(std::memory_order_relaxed))
                {
                    ApplyStyle();
                }
            }
            catch (...)
            {
            }
        });
    g_attached = true;
    SP_Log(L"Attached to the Start menu");

    ApplyStyle();
}

void Detach()
{
    if (!g_attached)
    {
        return;
    }

    try
    {
        if (Window window = Window::Current())
        {
            window.VisibilityChanged(g_visibilityToken);
        }
    }
    catch (...)
    {
    }
    g_visibilityToken = {};

    if (auto frame = g_sizingFrame.get())
    {
        frame.UnregisterPropertyChangedCallback(Controls::Canvas::LeftProperty(), g_canvasLeftToken);
        frame.UnregisterPropertyChangedCallback(Controls::Canvas::TopProperty(), g_canvasTopToken);
    }
    g_sizingFrame = nullptr;
    g_canvasLeftToken = g_canvasTopToken = 0;

    if (auto frameRoot = g_frameRoot.get())
    {
        frameRoot.UnregisterPropertyChangedCallback(FrameworkElement::HorizontalAlignmentProperty(), g_alignmentToken);
    }
    g_frameRoot = nullptr;
    g_alignmentToken = 0;

    // g_unloading is set, so this pass restores.
    ApplyStyle();

    g_attached = false;
    SP_Log(L"Detached from the Start menu");
}

// ---------------------------------------------------------------------------------------------------------------
// Running code on the UI thread from the engine thread
// ---------------------------------------------------------------------------------------------------------------

UINT g_runMessage = 0;
constexpr WPARAM kRunMagic = 0x53507372;   // 'SPsr'

struct RunRequest
{
    HWND                  hWnd;
    void                (*fn)();
    bool                  done;
};

LRESULT CALLBACK RunHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION)
    {
        const CWPSTRUCT* cwp = (const CWPSTRUCT*)lParam;
        if (cwp->message == g_runMessage && cwp->wParam == kRunMagic && cwp->lParam)
        {
            RunRequest* request = (RunRequest*)cwp->lParam;
            if (!request->done && request->hWnd == cwp->hwnd)
            {
                request->done = true;
                try
                {
                    request->fn();
                }
                catch (...)
                {
                }
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

// The CoreWindow of this process, which lives on the Start menu's UI thread.
HWND FindCoreWindow()
{
    HWND found = nullptr;
    EnumWindows(
        [](HWND hWnd, LPARAM lParam) -> BOOL {
            DWORD processId = 0;
            wchar_t className[64];
            if (GetWindowThreadProcessId(hWnd, &processId) && processId == GetCurrentProcessId() &&
                GetClassNameW(hWnd, className, ARRAYSIZE(className)) &&
                _wcsicmp(className, L"Windows.UI.Core.CoreWindow") == 0)
            {
                *(HWND*)lParam = hWnd;
                return FALSE;
            }
            return TRUE;
        },
        (LPARAM)&found);
    return found;
}

// Runs `fn` on the Start menu's UI thread and waits for it. False when there is no menu window yet.
bool RunOnUiThread(void (*fn)())
{
    HWND hWnd = FindCoreWindow();
    if (!hWnd)
    {
        return false;
    }
    const DWORD threadId = GetWindowThreadProcessId(hWnd, nullptr);
    if (threadId == GetCurrentThreadId())
    {
        fn();
        return true;
    }

    if (!g_runMessage)
    {
        g_runMessage = RegisterWindowMessageW(L"ShadePatcher.StartMenu.Run");
    }
    HHOOK hHook = SetWindowsHookExW(WH_CALLWNDPROC, RunHookProc, nullptr, threadId);
    if (!hHook)
    {
        SP_LogError(L"The hook on the Start menu thread could not be set: %lu", GetLastError());
        return false;
    }

    RunRequest request{ hWnd, fn, false };
    DWORD_PTR ignored = 0;
    SendMessageTimeoutW(hWnd, g_runMessage, kRunMagic, (LPARAM)&request, SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000,
                        &ignored);
    UnhookWindowsHookEx(hHook);
    return request.done;
}

// ---------------------------------------------------------------------------------------------------------------
// Hook: RoGetActivationFactory, to meet a menu built after the DLL arrived
// ---------------------------------------------------------------------------------------------------------------

using RoGetActivationFactory_t = HRESULT(WINAPI*)(HSTRING activatableClassId, REFIID iid, void** factory);
RoGetActivationFactory_t g_origRoGetActivationFactory = nullptr;

HRESULT WINAPI RoGetActivationFactory_Hook(HSTRING activatableClassId, REFIID iid, void** factory)
{
    thread_local bool inHook = false;
    if (!inHook && !g_attached && !g_unloading.load(std::memory_order_relaxed))
    {
        inHook = true;
        try
        {
            const wchar_t* name = WindowsGetStringRawBuffer(activatableClassId, nullptr);
            if (name && wcscmp(name, L"Windows.UI.Xaml.Hosting.XamlIsland") == 0)
            {
                Attach();
            }
        }
        catch (...)
        {
        }
        inHook = false;
    }
    return g_origRoGetActivationFactory(activatableClassId, iid, factory);
}

// ---------------------------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------------------------

void LoadSettings()
{
    g_onLeft.store(SP_GetIntSetting(L"StartMenuOnLeft", 1) != 0, std::memory_order_relaxed);
}

BOOL Init()
{
    g_unloading.store(false, std::memory_order_relaxed);
    LoadSettings();

    if (!SP_HookBegin())
    {
        return FALSE;
    }
    if (!SP_SetExportHook(L"combase.dll", "RoGetActivationFactory", RoGetActivationFactory_Hook,
                          &g_origRoGetActivationFactory))
    {
        SP_HookAbort();
        SP_LogError(L"RoGetActivationFactory could not be hooked");
        return FALSE;
    }
    return SP_HookCommit();
}

void AfterInit()
{
    // Usually the menu is already built by the time the DLL arrives.
    if (!RunOnUiThread(Attach))
    {
        SP_LogDebug(L"No Start menu window yet; waiting for the menu to be built");
    }
}

void SettingsChanged()
{
    LoadSettings();
    RunOnUiThread([]() {
        if (g_attached)
        {
            ApplyStyle();
        }
        else
        {
            Attach();
        }
    });
}

void BeforeUninit()
{
    g_unloading.store(true, std::memory_order_relaxed);
    RunOnUiThread(Detach);
}

}   // namespace

SP_MOD_DEFINE(g_modStartMenuOnLeft) =
{
    /* id             */ SP_MOD_ID,
    /* name           */ L"Open the Start menu at the left edge",
    /* basedOn        */ "taskbar-start-button-position",
    /* originalAuthor */ "m417z",
    /* targets        */ SP_TARGET_STARTMENU,
    /* minOsBuild     */ 22000,
    /* flags          */ 0,
    /* Init           */ Init,
    /* AfterInit      */ AfterInit,
    /* SettingsChanged*/ SettingsChanged,
    /* BeforeUninit   */ BeforeUninit,
    /* Uninit         */ nullptr,
};
