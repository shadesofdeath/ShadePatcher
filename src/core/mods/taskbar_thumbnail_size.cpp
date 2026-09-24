//
// taskbar_thumbnail_size - the size of the taskbar thumbnail previews on the Windows 11 taskbar.
//
// Adapted from the idea behind the Windhawk mod "Taskbar Thumbnail Size" (taskbar-thumbnail-size) by m417z.
// The implementation here is written against this engine's API.
//
// What it does
// ------------
// Hovering a taskbar button shows a preview of each of its windows. Since Windows 11 24H2 those previews are
// drawn by the XAML taskbar (Taskbar.View.dll) and their size is a constant of that library, not a setting; the
// old registry values that sized the classic previews do nothing to them. This mod makes the size the user's:
//
//   ThumbnailWidth    the widest a preview may be, in logical pixels; 0 leaves Windows' own limit
//   ThumbnailHeight   the tallest a preview may be, in logical pixels; 0 leaves Windows' own limit
//
// A preview keeps the shape of its window: it is scaled so that it fits inside Width x Height. Give only one of
// the two and the preview grows (or shrinks) until that side is reached.
//
// Where it hooks and why
// ----------------------
// Two functions of Taskbar.View.dll, both installed once the library is loaded:
//
//   ThumbnailHelpers::GetScaledThumbnailSize(Size, float)
//       Fits a window's size into the stock preview box, scaled for the monitor's DPI. The hook lets the
//       original answer first, works out the factor that brings that answer to the wanted box, and asks the
//       original again with the DPI scale multiplied by that factor. Going through the original a second time
//       rather than writing the numbers in directly keeps whatever rounding and minimums it applies.
//
//   TaskItemThumbnailView::OnApplyTemplate()
//       The preview control's template caps its MaxWidth at the stock width, so a bigger answer from the size
//       function would still be clipped. Once the template is applied the cap is lifted (MaxWidth = infinity);
//       the size function alone then decides how wide the preview is. Nothing is put back on unload: with the
//       hooks gone the size function answers the stock numbers, and the control is never wider than they say.
//
// The XAML object behind an implementation pointer sits in the second pointer-sized slot of the object, the
// place every m417z mod reads it from; it is checked through QueryInterface before it is used, under an
// exception guard so a build that lays the object out differently costs the feature rather than the shell.
//
// Exceptions
// ----------
// C++/WinRT reports failures by throwing, so this file is compiled with exceptions on while the rest of the
// engine is not. Every path XAML calls into is wrapped; an exception reaching the taskbar's UI thread would end
// the shell.
//
#define SP_MOD_ID "taskbar-thumbnail-size"
#include "engine/modapi.h"

// Windows.h leaves a GetCurrentTime macro behind that collides with a method of the same name in the XAML
// projection, so it goes before the C++/WinRT headers.
#undef GetCurrentTime

#include <unknwn.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Xaml.h>

#include <atomic>
#include <cmath>
#include <limits>

namespace {

using winrt::Windows::UI::Xaml::FrameworkElement;

// ---------------------------------------------------------------------------------------------------------------
// Settings
//
// Written on the engine thread, read on the taskbar's UI thread. 0 means "leave that side to Windows".
// ---------------------------------------------------------------------------------------------------------------

constexpr int kMaxDimension = 4000;

std::atomic<int> g_thumbnailWidth{ 0 };
std::atomic<int> g_thumbnailHeight{ 0 };

// Set in BeforeUninit: from then on both hooks pass straight through, so the engine removes them from a taskbar
// that is already back to stock.
std::atomic<bool> g_unloading{ false };

// ---------------------------------------------------------------------------------------------------------------
// The size function
//
// The shell's own Size type is winrt::Windows::Foundation::Size: two floats with a user-defined constructor,
// which on x64 means it comes back through a hidden result pointer in the first argument while the by-value
// argument travels in a register like an 8-byte integer. The plain struct below has the same layout and the
// signature spells the hidden pointer out, so the hook does not depend on how this compiler would return the
// projection's type.
// ---------------------------------------------------------------------------------------------------------------

struct ThumbnailSize
{
    float Width;
    float Height;
};

using GetScaledThumbnailSize_t = ThumbnailSize* (WINAPI*)(ThumbnailSize* result, ThumbnailSize size, float scale);
GetScaledThumbnailSize_t g_origGetScaledThumbnailSize = nullptr;

ThumbnailSize* WINAPI GetScaledThumbnailSize_Hook(ThumbnailSize* result, ThumbnailSize size, float scale)
{
    ThumbnailSize* stock = g_origGetScaledThumbnailSize(result, size, scale);

    if (g_unloading.load(std::memory_order_relaxed) || !stock)
    {
        return stock;
    }

    const int wantWidth = g_thumbnailWidth.load(std::memory_order_relaxed);
    const int wantHeight = g_thumbnailHeight.load(std::memory_order_relaxed);
    if (wantWidth <= 0 && wantHeight <= 0)
    {
        return stock;
    }

    const float stockWidth = stock->Width;
    const float stockHeight = stock->Height;
    if (!(stockWidth > 0.0f) || !(stockHeight > 0.0f) || !std::isfinite(stockWidth) || !std::isfinite(stockHeight))
    {
        // A zero or nonsense answer is not something to scale; hand it back as it is.
        return stock;
    }

    // The factor that brings the stock answer to the wanted box while keeping the window's shape: the smaller
    // of the two ratios, so that neither side goes past what was asked for. A side left at 0 does not take
    // part.
    float factor = std::numeric_limits<float>::infinity();
    if (wantWidth > 0)
    {
        factor = std::fmin(factor, (float)wantWidth / stockWidth);
    }
    if (wantHeight > 0)
    {
        factor = std::fmin(factor, (float)wantHeight / stockHeight);
    }

    if (!std::isfinite(factor) || !(factor > 0.0f) || std::fabs(factor - 1.0f) < 0.001f)
    {
        return stock;
    }

    ThumbnailSize* scaled = g_origGetScaledThumbnailSize(result, size, scale * factor);
    if (!scaled)
    {
        return stock;
    }

    SP_LogDebug(L"Thumbnail %.0fx%.0f at scale %.2f: stock %.0fx%.0f, now %.0fx%.0f (x%.3f)",
                size.Width, size.Height, scale, stockWidth, stockHeight, scaled->Width, scaled->Height, factor);
    return scaled;
}

// ---------------------------------------------------------------------------------------------------------------
// The preview control's template
// ---------------------------------------------------------------------------------------------------------------

// Reads the XAML object out of the implementation object and asks it for the interface. No C++ objects live in
// here: a function with __try cannot unwind them (C2712), and the guard is the point, since a build that lays
// the object out differently would otherwise take the shell down with it. The returned pointer carries the
// reference QueryInterface added.
void* QueryElementAbi(void* pThis, const GUID* iid)
{
    void* abi = nullptr;
    __try
    {
        ::IUnknown* unknown = reinterpret_cast<::IUnknown**>(pThis)[1];
        if (unknown && FAILED(unknown->QueryInterface(*iid, &abi)))
        {
            abi = nullptr;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        abi = nullptr;
    }
    return abi;
}

FrameworkElement ElementFromThumbnailView(void* pThis)
{
    FrameworkElement element{ nullptr };
    if (!pThis)
    {
        return element;
    }

    const winrt::guid iid = winrt::guid_of<FrameworkElement>();
    if (void* abi = QueryElementAbi(pThis, reinterpret_cast<const GUID*>(&iid)))
    {
        // The wrapper takes over the reference QueryInterface added.
        *winrt::put_abi(element) = abi;
    }
    return element;
}

using OnApplyTemplate_t = void (WINAPI*)(void* pThis);
OnApplyTemplate_t g_origOnApplyTemplate = nullptr;

void WINAPI OnApplyTemplate_Hook(void* pThis)
{
    g_origOnApplyTemplate(pThis);

    if (g_unloading.load(std::memory_order_relaxed))
    {
        return;
    }

    // The cap is lifted whatever the settings say at this moment: a preview templated while the size was still
    // stock would otherwise stay clipped after the user picks a wider one.
    try
    {
        FrameworkElement element = ElementFromThumbnailView(pThis);
        if (!element)
        {
            SP_LogDebug(L"The thumbnail view's element was not reachable; its width cap stays");
            return;
        }

        const double maxWidth = element.MaxWidth();
        if (std::isfinite(maxWidth))
        {
            element.MaxWidth(std::numeric_limits<double>::infinity());
            SP_LogDebug(L"Thumbnail view width cap of %.0f lifted", maxWidth);
        }
    }
    catch (winrt::hresult_error const& e)
    {
        SP_LogError(L"The thumbnail view's width cap could not be lifted: 0x%08X", (unsigned)e.code());
    }
    catch (...)
    {
        SP_LogError(L"The thumbnail view's width cap could not be lifted");
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Installing the hooks
// ---------------------------------------------------------------------------------------------------------------

void HookTaskbarView(HMODULE hModule)
{
    static const wchar_t* const kGetScaledThumbnailSize[] =
    {
        LR"(struct winrt::Windows::Foundation::Size __cdecl winrt::Taskbar::implementation::ThumbnailHelpers::GetScaledThumbnailSize(struct winrt::Windows::Foundation::Size,float))",
    };
    static const wchar_t* const kOnApplyTemplate[] =
    {
        LR"(public: void __cdecl winrt::Taskbar::implementation::TaskItemThumbnailView::OnApplyTemplate(void))",
    };

    // One batch for the module. Both are optional so that a rename in a future build costs the feature it
    // belongs to and is logged, rather than refusing the whole mod.
    SP_SymbolHook hooks[2] = {};
    hooks[0].symbols = kGetScaledThumbnailSize;
    hooks[0].symbolCount = ARRAYSIZE(kGetScaledThumbnailSize);
    hooks[0].pOriginal = (void**)&g_origGetScaledThumbnailSize;
    hooks[0].hookFunction = (void*)GetScaledThumbnailSize_Hook;
    hooks[0].optional = TRUE;

    hooks[1].symbols = kOnApplyTemplate;
    hooks[1].symbolCount = ARRAYSIZE(kOnApplyTemplate);
    hooks[1].pOriginal = (void**)&g_origOnApplyTemplate;
    hooks[1].hookFunction = (void*)OnApplyTemplate_Hook;
    hooks[1].optional = TRUE;

    if (!SP_HookSymbols(hModule, hooks, ARRAYSIZE(hooks)))
    {
        SP_LogError(L"The thumbnail hooks could not be installed in Taskbar.View.dll");
        return;
    }

    if (!g_origGetScaledThumbnailSize)
    {
        SP_LogError(L"ThumbnailHelpers::GetScaledThumbnailSize was not found in this build; "
                    L"the thumbnail size cannot be changed");
    }
    if (!g_origOnApplyTemplate)
    {
        SP_LogError(L"TaskItemThumbnailView::OnApplyTemplate was not found in this build; "
                    L"previews wider than the stock width will be clipped");
    }

    SP_Log(L"Taskbar thumbnail hooks are in: size %s, template %s",
           g_origGetScaledThumbnailSize ? L"yes" : L"no",
           g_origOnApplyTemplate ? L"yes" : L"no");
}

void OnTaskbarViewLoaded(HMODULE hModule, void*)
{
    HookTaskbarView(hModule);
}

// ---------------------------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------------------------

int ReadDimension(const wchar_t* name)
{
    int value = SP_GetIntSetting(name, 0);
    if (value < 0)
    {
        value = 0;
    }
    else if (value > kMaxDimension)
    {
        value = kMaxDimension;
    }
    return value;
}

void LoadSettings()
{
    g_thumbnailWidth.store(ReadDimension(L"ThumbnailWidth"), std::memory_order_relaxed);
    g_thumbnailHeight.store(ReadDimension(L"ThumbnailHeight"), std::memory_order_relaxed);

    SP_Log(L"Thumbnail box: width %d, height %d (0 = Windows default)",
           g_thumbnailWidth.load(std::memory_order_relaxed),
           g_thumbnailHeight.load(std::memory_order_relaxed));
}

BOOL Init()
{
    LoadSettings();

    // A minute is far longer than the taskbar has ever taken to appear. The wait is cancelled by the engine if
    // the mod is unloaded first.
    if (!SP_WaitForModule(L"Taskbar.View.dll", 60000, OnTaskbarViewLoaded, nullptr))
    {
        SP_LogError(L"Could not wait for Taskbar.View.dll");
        return FALSE;
    }

    return TRUE;
}

void SettingsChanged()
{
    // The size function is asked afresh each time a preview flyout is built, so the new numbers show on the
    // next hover; nothing that is already open is touched.
    LoadSettings();
}

void BeforeUninit()
{
    // Still hooked: from here on every preview gets the stock size, so the engine removes hooks from a taskbar
    // that already looks like stock Windows.
    g_unloading.store(true, std::memory_order_relaxed);
}

}   // namespace

SP_MOD_DEFINE(g_modTaskbarThumbnailSize) =
{
    /* id             */ SP_MOD_ID,
    /* name           */ L"Taskbar thumbnail size",
    /* basedOn        */ "taskbar-thumbnail-size",
    /* originalAuthor */ "m417z",
    /* targets        */ SP_TARGET_EXPLORER,
    /* minOsBuild     */ 26100,     // the XAML previews (and ThumbnailHelpers) arrived with 24H2
    /* flags          */ 0,
    /* Init           */ Init,
    /* AfterInit      */ nullptr,
    /* SettingsChanged*/ SettingsChanged,
    /* BeforeUninit   */ BeforeUninit,
    /* Uninit         */ nullptr,
};
