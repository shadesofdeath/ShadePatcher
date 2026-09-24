//
// taskbar-styler - restyles the Windows 11 taskbar (and the shell's other XAML surfaces) with a theme.
//
// What it does
// ------------
// A theme is a list of rules: a target that names XAML elements by type, name, position and ancestors, and the
// property values to put on them (see taskbar_styler_rules.h for the language, taskbar_styler_themes.h for the
// seven themes shipped). The mod applies those rules to every matching element the shell creates, keeps them
// applied while the element changes visual state, and puts every value back when it is turned off.
//
// How it sees the elements
// ------------------------
// Not through a hook. The XAML runtime has a diagnostics channel, the one Visual Studio's live visual tree uses:
// InitializeXamlDiagnosticsEx (Windows.UI.Xaml.dll) loads a "TAP" COM object from a DLL of the caller's choice,
// hands it the diagnostics interface through IObjectWithSite, and from then on reports every element that enters
// or leaves a visual tree, on that tree's own UI thread, through IVisualTreeServiceCallback2. This DLL is the TAP
// DLL: it exports DllGetClassObject for one CLSID, and the object it makes is the watcher below.
//
// The channel holds a reference on every element it reports, so the mod hands those references back through
// IXamlDiagnosticsTestHooks::UnregisterInstance once it has looked at an element; only elements it still tracks
// stay held, and those are released when their removal is reported.
//
// When it applies
// ---------------
// Rules are loaded per UI thread (the taskbar has one, Task View, the input switcher and the volume flyout have
// their own). A thread is initialized when its host window is created (CreateWindowExW / CreateWindowInBand
// hooks) or, for threads that already exist when the mod starts, from a sweep of the shell's XAML host windows.
// Only then is the diagnostics channel connected, so the burst of existing elements it reports lands on threads
// that already know the rules.
//
// Two registry hooks answer one read of HKLM\Software\Microsoft\XAML\Debug\DisableCompositionDiag during
// AdviseVisualTreeChange: the composition diagnostics that would otherwise start are not thread-safe and were
// seen to corrupt the heap; element reports come from a different path and are all this mod needs.
//
// Exceptions
// ----------
// C++/WinRT throws, so this file is compiled with exceptions on. Every callback the shell can enter is wrapped;
// an exception reaching a UI thread would end the process.
//
// Based on the Windhawk mod "windows-11-taskbar-styler" by m417z (the visual tree watcher there comes from
// TranslucentTB's ExplorerTAP, and the WindhawkBlur brush from its XamlBlurBrush). The theme data is copied as
// published; the engine is written against ShadePatcher's API.
//
#define SP_MOD_ID "taskbar-styler"
#include "engine/modapi.h"

#include <unknwn.h>
#include <objbase.h>
#include <ocidl.h>
#include <weakreference.h>
#include <commctrl.h>
#include <xamlom.h>
#include <d2d1_1.h>
#include <d2d1effects.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#undef GetCurrentTime

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Effects.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.System.Power.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>

#include "taskbar_styler_rules.h"
#include "taskbar_styler_themes.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "ole32.lib")

// XamlDiagnostics implements this too; xamlom.h does not declare it. UnregisterInstance drops the runtime
// object the diagnostics cache for a handle, which is the reference they keep on a reported element.
struct __declspec(uuid("735941a2-3ee3-495a-8da9-972627003075")) IXamlDiagnosticsTestHooks : ::IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE UnregisterInstance(InstanceHandle handle) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryGetDispatcherQueueForObject(InstanceHandle handle, void** dispatcherQueue) = 0;
};

// The interop side of a composition effect: what the compositor asks an IGraphicsEffect for when it builds the
// D2D effect graph. Declared here (windows.graphics.effects.interop.h wants the ABI projection of
// IPropertyValue); the vtable is the documented one.
struct __declspec(uuid("2FC57384-A068-44D7-A331-30982FCF7177")) IGraphicsEffectD2D1Interop : ::IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetEffectId(GUID* id) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNamedPropertyMapping(LPCWSTR name, UINT* index, UINT* mapping) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyCount(UINT* count) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProperty(UINT index, ::IUnknown** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSource(UINT index, ::IUnknown** source) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSourceCount(UINT* count) = 0;
};

namespace {

namespace wf = winrt::Windows::Foundation;
namespace wux = winrt::Windows::UI::Xaml;
namespace wge = winrt::Windows::Graphics::Effects;
namespace wuc = winrt::Windows::UI::Composition;
using namespace winrt::Windows::UI::Xaml;
using winrt::Windows::Foundation::IInspectable;

constexpr UINT kMappingDirect = 1;   // GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT

// The CLSID under which the diagnostics ask this DLL for the TAP object.
// {7D2E9F41-3C6B-4A8E-9B1D-5F0C2A7E8D31}
constexpr CLSID CLSID_StylerTap = { 0x7d2e9f41, 0x3c6b, 0x4a8e, { 0x9b, 0x1d, 0x5f, 0x0c, 0x2a, 0x7e, 0x8d, 0x31 } };

// ---------------------------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------------------------

std::atomic<bool> g_active{ false };            // hooks do nothing once this is off
std::atomic<int>  g_themeIndex{ 0 };
std::atomic<int>  g_taskbarHeight{ 48 };        // $TaskbarHeight for the themes

const stylerthemes::Theme* ThemeForIndex(int index)
{
    switch (index)
    {
    case 1: return &stylerthemes::Theme_TranslucentTaskbar();
    case 2: return &stylerthemes::Theme_DockLike();
    case 3: return &stylerthemes::Theme_SimplyTransparent();
    case 4: return &stylerthemes::Theme_Squircle();
    case 5: return &stylerthemes::Theme_Matter();
    case 6: return &stylerthemes::Theme_Surface();
    case 7: return &stylerthemes::Theme_Luminosity_variant_Dock();
    case 8: return &stylerthemes::Theme_Luminosity_variant_Classic();
    case 9: return &stylerthemes::Theme_Luminosity_variant_Compact();
    default: return nullptr;
    }
}

const wchar_t* ThemeNameForIndex(int index)
{
    static const wchar_t* const kNames[] =
    {
        L"None", L"TranslucentTaskbar", L"DockLike", L"SimplyTransparent", L"Squircle", L"Matter", L"Surface",
        L"Luminosity (Dock)", L"Luminosity (Classic)", L"Luminosity (Compact)",
    };
    return (index >= 0 && index < (int)ARRAYSIZE(kNames)) ? kNames[index] : L"None";
}

void LoadSettings()
{
    int theme = SP_GetIntSetting(L"Theme", 1);
    if (theme < 0 || theme > 9)
    {
        theme = 0;
    }
    g_themeIndex.store(theme, std::memory_order_relaxed);

    // The taskbar's height, from the taskbar-icon-size mod when it sets one. Themes size their frame with it
    // rather than with the height their author's taskbar had.
    int height = 48;
    if (SP_SettingsIsModEnabled("taskbar-icon-size"))
    {
        height = SP_SettingsGetInt("taskbar-icon-size", L"TaskbarHeight", 48);
    }
    g_taskbarHeight.store(std::clamp(height, 24, 160), std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------------------------------
// Small WinRT helpers
// ---------------------------------------------------------------------------------------------------------------

std::wstring HResultMessage(winrt::hresult_error const& e)
{
    return std::wstring(e.message());
}

// C++/WinRT's make_weak dereferences null for an object without weak reference support; this reports it instead.
winrt::weak_ref<IInspectable> TryMakeWeak(IInspectable const& object)
{
    if (!object || !object.try_as<::IWeakReferenceSource>())
    {
        return nullptr;
    }
    return winrt::make_weak(object);
}

// COM only promises a stable pointer for IUnknown, so that is what identifies an object.
void* IdentityKey(IInspectable const& object)
{
    if (!object)
    {
        return nullptr;
    }
    winrt::com_ptr<::IUnknown> unknown;
    if (FAILED(reinterpret_cast<::IUnknown*>(winrt::get_abi(object))->QueryInterface(IID_IUnknown, unknown.put_void())))
    {
        return nullptr;
    }
    return unknown.get();
}

bool SameObject(IInspectable const& a, IInspectable const& b)
{
    return IdentityKey(a) == IdentityKey(b);
}

// The handle the diagnostics report for an element: the address of its IInspectable. Derived the same way, so
// an element reached by walking the tree can be looked up without asking the diagnostics (which would take a new
// reference on it).
InstanceHandle HandleFromInspectable(IInspectable const& object)
{
    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(reinterpret_cast<::IUnknown*>(winrt::get_abi(object))->QueryInterface(
        winrt::guid_of<IInspectable>(), inspectable.put_void()));
    return reinterpret_cast<InstanceHandle>(inspectable.get());
}

// ---------------------------------------------------------------------------------------------------------------
// Element ids
//
// An InstanceHandle is an address, so a destroyed element can be followed by a new one reported under the same
// handle. Everything is keyed by an id minted per reported element and never reused; a weak reference on the
// entry proves it still names the element being asked about.
// ---------------------------------------------------------------------------------------------------------------

enum class ElementId : uint64_t { None = 0 };

struct ElementIdEntry
{
    ElementId id = ElementId::None;
    winrt::weak_ref<IInspectable> element;
};

thread_local std::unordered_map<InstanceHandle, ElementIdEntry> g_elementIds;
thread_local uint64_t g_lastElementId;
thread_local size_t g_elementIdsReapThreshold = 64;
thread_local bool g_initializedForThread;

void CleanupCustomizations(ElementId elementId);

ElementId GetOrCreateElementId(InstanceHandle handle, IInspectable const& element)
{
    if (!handle || !element)
    {
        return ElementId::None;
    }

    auto& entry = g_elementIds[handle];
    if (entry.id != ElementId::None && SameObject(entry.element.get(), element))
    {
        return entry.id;
    }

    entry.id = static_cast<ElementId>(++g_lastElementId);

    winrt::weak_ref<IInspectable> weak;
    try
    {
        weak = TryMakeWeak(element);
    }
    catch (winrt::hresult_error const&)
    {
    }

    if (!weak)
    {
        // Without a weak reference the entry could not be told from a successor at the same address.
        g_elementIds.erase(handle);
        return ElementId::None;
    }

    entry.element = std::move(weak);
    return entry.id;
}

ElementId FindElementId(InstanceHandle handle)
{
    auto it = g_elementIds.find(handle);
    return it != g_elementIds.end() ? it->second.id : ElementId::None;
}

void ForgetElementId(InstanceHandle handle)
{
    g_elementIds.erase(handle);
}

// The id of an element reached by walking the tree; None when the diagnostics never reported it.
ElementId ElementIdFromElement(FrameworkElement const& element)
{
    if (!element)
    {
        return ElementId::None;
    }
    try
    {
        auto it = g_elementIds.find(HandleFromInspectable(element));
        if (it == g_elementIds.end() || !SameObject(it->second.element.get(), element))
        {
            return ElementId::None;
        }
        return it->second.id;
    }
    catch (winrt::hresult_error const&)
    {
        return ElementId::None;
    }
}

// An element whose diagnostics reference was handed back is destroyed without a removal being reported, so the
// dead entries are found rather than told, once the map has grown enough to make a sweep worthwhile.
void ReapDeadElementIdsIfNeeded()
{
    if (g_elementIds.size() < g_elementIdsReapThreshold)
    {
        return;
    }

    std::vector<std::pair<InstanceHandle, ElementId>> dead;
    for (const auto& [handle, entry] : g_elementIds)
    {
        if (!entry.element.get())
        {
            dead.push_back({ handle, entry.id });
        }
    }

    for (const auto& [handle, elementId] : dead)
    {
        CleanupCustomizations(elementId);
        g_elementIds.erase(handle);
    }

    g_elementIdsReapThreshold = std::max<size_t>(64, g_elementIds.size() * 2);
}

// ---------------------------------------------------------------------------------------------------------------
// Rules and their resolved forms
// ---------------------------------------------------------------------------------------------------------------

using PropertyKeyValue = std::pair<DependencyProperty, IInspectable>;

struct Matcher
{
    styler::MatcherSpec spec;
    // The [Property=Value] conditions turned into DependencyProperty + typed value, on first use.
    std::optional<std::vector<PropertyKeyValue>> conditions;
};

// A style whose value holds {{...}}: kept as text and re-resolved when its variables change.
struct DynamicStyleTemplate
{
    std::wstring propertyName;
    std::wstring rawValue;
    bool isXamlValue = false;
};

// One (property, visual state) cell: a ready WinRT value, blur brush parameters (the brush is made per element)
// or a template.
using PropertyOverrideValue = std::variant<IInspectable, styler::BlurParams, DynamicStyleTemplate>;

// property -> visual state name ("" for any) -> value
using PropertyOverrides = std::unordered_map<DependencyProperty, std::unordered_map<std::wstring, PropertyOverrideValue>>;

struct CaptureSpec
{
    DependencyProperty property{ nullptr };
    std::wstring varName;
};

struct ResolvedRules
{
    PropertyOverrides overrides;
    std::vector<CaptureSpec> captures;
    bool hasDynamicValues = false;
};

struct CustomizationRule
{
    std::wstring targetText;
    Matcher matcher;
    std::vector<Matcher> parents;        // nearest parent first
    styler::UnresolvedRules unresolved;
    std::optional<ResolvedRules> resolved;
};

thread_local std::vector<CustomizationRule> g_rules;

// ---------------------------------------------------------------------------------------------------------------
// Per-element state
// ---------------------------------------------------------------------------------------------------------------

struct VariableDependency
{
    std::wstring name;
    ElementId owner = ElementId::None;   // the capture the value came from; None when it was undefined
};

struct PropertyState
{
    std::optional<IInspectable> originalValue;
    std::optional<PropertyOverrideValue> customValue;
    IInspectable lastAppliedValue{ nullptr };
    int64_t propertyChangedToken = 0;
    std::optional<DynamicStyleTemplate> dynamicTemplate;
    std::vector<VariableDependency> variableDependencies;
    bool lastResolveFailed = false;
};

struct VisualStateGroupState
{
    std::unordered_map<DependencyProperty, PropertyState> properties;
    winrt::event_token stateChangedToken;
};

struct CaptureState
{
    std::wstring varName;
    int64_t propertyChangedToken = 0;
};

struct ElementState
{
    winrt::weak_ref<FrameworkElement> element;
    winrt::weak_ref<XamlRoot> xamlRoot;
    std::unordered_map<DependencyProperty, CaptureState> captures;
    winrt::event_token sizeChangedToken;
    // A list: callbacks hold pointers to the entries.
    std::list<std::pair<std::optional<winrt::weak_ref<VisualStateGroup>>, VisualStateGroupState>> perVisualStateGroup;
};

thread_local std::unordered_map<ElementId, ElementState> g_elementStates;

// Set while the mod itself writes a property, so its own writes are not taken for outside changes.
thread_local bool g_modifyingProperty;

// ---------------------------------------------------------------------------------------------------------------
// Style variables
//
// `Property=>Name` captures publish a value; `{{Name}}` in another style reads it. Scoped per XamlRoot so one
// taskbar's capture is not read by another's on the same thread. Entries live in a list: callbacks keep pointers
// to them.
// ---------------------------------------------------------------------------------------------------------------

struct VariableCapture
{
    ElementId elementId;
    styler::VariableValue value;
};

struct VariableConsumer
{
    ElementId elementId;
    DependencyProperty property{ nullptr };
    std::wstring fallbackClassName;
};

struct VariableState
{
    winrt::weak_ref<XamlRoot> xamlRoot;
    std::unordered_map<std::wstring, std::vector<VariableCapture>> variables;
    std::unordered_map<std::wstring, std::vector<VariableConsumer>> consumers;
    std::unordered_map<ElementId, size_t> elementRefs;   // how many entries above mention each element
};

thread_local std::list<VariableState> g_variableStates;
thread_local int g_variableStatePinDepth;      // > 0 while a frame holds a VariableState*: no reaping
thread_local int g_propagationDepth;

struct VariableStatePin
{
    VariableStatePin() { ++g_variableStatePinDepth; }
    ~VariableStatePin() { --g_variableStatePinDepth; }
    VariableStatePin(const VariableStatePin&) = delete;
    VariableStatePin& operator=(const VariableStatePin&) = delete;
};

struct PendingPropagation
{
    VariableState* state;
    std::wstring varName;
    std::optional<ElementId> changedOwner;

    bool operator==(const PendingPropagation& other) const
    {
        return state == other.state && varName == other.varName && changedOwner == other.changedOwner;
    }
};

thread_local std::vector<PendingPropagation> g_pendingPropagations;

void AddElementRef(VariableState* state, ElementId elementId)
{
    state->elementRefs[elementId]++;
}

void ReleaseElementRefs(VariableState* state, ElementId elementId, size_t count)
{
    if (!count)
    {
        return;
    }
    auto it = state->elementRefs.find(elementId);
    if (it == state->elementRefs.end())
    {
        return;
    }
    if (it->second > count)
    {
        it->second -= count;
    }
    else
    {
        state->elementRefs.erase(it);
    }
}

VariableState* GetVariableState(XamlRoot const& xamlRoot)
{
    if (!xamlRoot)
    {
        return nullptr;
    }
    if (g_variableStatePinDepth == 0)
    {
        g_variableStates.remove_if([](VariableState const& entry) { return !entry.xamlRoot.get(); });
    }
    for (auto& entry : g_variableStates)
    {
        if (entry.xamlRoot.get() == xamlRoot)
        {
            return &entry;
        }
    }
    auto& fresh = g_variableStates.emplace_back();
    fresh.xamlRoot = xamlRoot;
    return &fresh;
}

VariableState* GetVariableState(winrt::weak_ref<XamlRoot> const& weak)
{
    auto strong = weak.get();
    return strong ? GetVariableState(strong) : nullptr;
}

VariableState* GetVariableState(FrameworkElement const& element)
{
    if (!element)
    {
        return nullptr;
    }
    XamlRoot xamlRoot{ nullptr };
    try
    {
        xamlRoot = element.XamlRoot();
    }
    catch (...)
    {
    }
    return GetVariableState(xamlRoot);
}

// ---------------------------------------------------------------------------------------------------------------
// Noise for the blur brush: a 256x256 tileable grey BMP made in memory. Density shapes the brightness curve;
// opacity is applied later in the effect graph.
// ---------------------------------------------------------------------------------------------------------------

winrt::Windows::Storage::Streams::IRandomAccessStream CreateNoiseStream(float density)
{
    thread_local float cachedDensity = std::numeric_limits<float>::quiet_NaN();
    thread_local winrt::Windows::Storage::Streams::InMemoryRandomAccessStream cachedStream{ nullptr };

    if (cachedStream && density == cachedDensity)
    {
        return cachedStream.CloneStream();
    }

    constexpr int kSize = 256;
    constexpr DWORD kRowSize = kSize * 4;
    constexpr DWORD kDataSize = kRowSize * kSize;

    BITMAPFILEHEADER fileHeader = {};
    fileHeader.bfType = 0x4D42;
    fileHeader.bfSize = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + kDataSize;
    fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);

    BITMAPINFOHEADER infoHeader = {};
    infoHeader.biSize = sizeof(BITMAPINFOHEADER);
    infoHeader.biWidth = kSize;
    infoHeader.biHeight = kSize;
    infoHeader.biPlanes = 1;
    infoHeader.biBitCount = 32;
    infoHeader.biSizeImage = kDataSize;

    const float safeDensity = std::clamp(density, 0.001f, 1.0f);
    const float exponent = 1.0f / safeDensity;
    uint8_t lut[256];
    for (int i = 0; i < 256; ++i)
    {
        lut[i] = static_cast<uint8_t>(std::pow(i / 255.0f, exponent) * 255.0f);
    }

    std::vector<uint8_t> bytes;
    bytes.reserve(fileHeader.bfSize);
    bytes.insert(bytes.end(), reinterpret_cast<const uint8_t*>(&fileHeader), reinterpret_cast<const uint8_t*>(&fileHeader) + sizeof(fileHeader));
    bytes.insert(bytes.end(), reinterpret_cast<const uint8_t*>(&infoHeader), reinterpret_cast<const uint8_t*>(&infoHeader) + sizeof(infoHeader));

    std::mt19937 rng(0);
    std::uniform_int_distribution<int> dist(0, 255);
    for (DWORD i = 0; i < kDataSize; i += 4)
    {
        const uint8_t grey = lut[dist(rng)];
        bytes.push_back(grey);
        bytes.push_back(grey);
        bytes.push_back(grey);
        bytes.push_back(255);
    }

    // An in-memory WinRT stream, the same way the original does it. The store completes at once for an
    // in-memory stream, so the wait on the UI thread costs nothing; a COM stream wrapped through shcore was
    // rejected by LoadedImageSurface with E_INVALIDARG.
    winrt::Windows::Storage::Streams::InMemoryRandomAccessStream stream;
    winrt::Windows::Storage::Streams::DataWriter writer(stream);
    writer.WriteBytes(winrt::array_view<const uint8_t>(bytes.data(), bytes.data() + bytes.size()));
    writer.StoreAsync().get();
    writer.DetachStream();

    cachedStream = stream;
    cachedDensity = density;
    return cachedStream.CloneStream();
}

// ---------------------------------------------------------------------------------------------------------------
// Composition effects
//
// Windows.UI.Composition builds an effect graph from IGraphicsEffect objects that also implement
// IGraphicsEffectD2D1Interop; each one below names a D2D effect and hands over its properties and sources.
// ---------------------------------------------------------------------------------------------------------------

::IUnknown* DetachPropertyValue(IInspectable const& boxed)
{
    return static_cast<::IUnknown*>(winrt::detach_abi(boxed.as<wf::IPropertyValue>()));
}

template <typename Derived>
struct EffectBase : winrt::implements<Derived, wge::IGraphicsEffect, wge::IGraphicsEffectSource, IGraphicsEffectD2D1Interop>
{
    winrt::hstring Name() { return m_name; }
    void Name(winrt::hstring const& name) { m_name = name; }

    HRESULT STDMETHODCALLTYPE GetEffectId(GUID* id) noexcept override
    {
        if (!id) return E_INVALIDARG;
        *id = static_cast<Derived*>(this)->EffectId();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetNamedPropertyMapping(LPCWSTR name, UINT* index, UINT* mapping) noexcept override
    {
        if (!name || !index || !mapping) return E_INVALIDARG;
        const int found = static_cast<Derived*>(this)->PropertyIndex(name);
        if (found < 0) return E_INVALIDARG;
        *index = (UINT)found;
        *mapping = kMappingDirect;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPropertyCount(UINT* count) noexcept override
    {
        if (!count) return E_INVALIDARG;
        *count = static_cast<Derived*>(this)->PropertyCount();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetProperty(UINT index, ::IUnknown** value) noexcept override
    {
        if (!value) return E_INVALIDARG;
        try
        {
            IInspectable boxed = static_cast<Derived*>(this)->Property(index);
            if (!boxed) return E_BOUNDS;
            *value = DetachPropertyValue(boxed);
            return S_OK;
        }
        catch (...)
        {
            return winrt::to_hresult();
        }
    }

    HRESULT STDMETHODCALLTYPE GetSource(UINT index, ::IUnknown** source) noexcept override
    {
        if (!source) return E_INVALIDARG;
        wge::IGraphicsEffectSource found = static_cast<Derived*>(this)->SourceAt(index);
        if (!found) return E_BOUNDS;
        *source = static_cast<::IUnknown*>(winrt::detach_abi(found));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetSourceCount(UINT* count) noexcept override
    {
        if (!count) return E_INVALIDARG;
        *count = static_cast<Derived*>(this)->SourceCount();
        return S_OK;
    }

private:
    // Empty by default: the compositor rejects a graph in which two effects carry the same name, and only the
    // effects that are addressed later (blur, saturation, luminosity, noise) are named at all.
    winrt::hstring m_name;
};

struct GaussianBlurEffect : EffectBase<GaussianBlurEffect>
{
    wge::IGraphicsEffectSource Source{ nullptr };
    float BlurAmount = 3.0f;

    GUID EffectId() const { return CLSID_D2D1GaussianBlur; }
    int PropertyIndex(std::wstring_view name) const
    {
        if (name == L"BlurAmount") return D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION;
        if (name == L"Optimization") return D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION;
        if (name == L"BorderMode") return D2D1_GAUSSIANBLUR_PROP_BORDER_MODE;
        return -1;
    }
    UINT PropertyCount() const { return 3; }
    IInspectable Property(UINT index) const
    {
        switch (index)
        {
        case D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION: return wf::PropertyValue::CreateSingle(BlurAmount);
        case D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION: return wf::PropertyValue::CreateUInt32(D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED);
        case D2D1_GAUSSIANBLUR_PROP_BORDER_MODE: return wf::PropertyValue::CreateUInt32(D2D1_BORDER_MODE_SOFT);
        default: return nullptr;
        }
    }
    wge::IGraphicsEffectSource SourceAt(UINT index) const { return index == 0 ? Source : nullptr; }
    UINT SourceCount() const { return 1; }
};

struct FloodEffect : EffectBase<FloodEffect>
{
    winrt::Windows::UI::Color Color{};

    GUID EffectId() const { return CLSID_D2D1Flood; }
    int PropertyIndex(std::wstring_view name) const { return name == L"Color" ? D2D1_FLOOD_PROP_COLOR : -1; }
    UINT PropertyCount() const { return 1; }
    IInspectable Property(UINT index) const
    {
        if (index != D2D1_FLOOD_PROP_COLOR) return nullptr;
        const float components[4] = { Color.R / 255.0f, Color.G / 255.0f, Color.B / 255.0f, Color.A / 255.0f };
        return wf::PropertyValue::CreateSingleArray(winrt::array_view<const float>(components, components + 4));
    }
    wge::IGraphicsEffectSource SourceAt(UINT) const { return nullptr; }
    UINT SourceCount() const { return 0; }
};

struct BorderEffect : EffectBase<BorderEffect>
{
    wge::IGraphicsEffectSource Source{ nullptr };

    GUID EffectId() const { return CLSID_D2D1Border; }
    int PropertyIndex(std::wstring_view name) const
    {
        if (name == L"ExtendX") return D2D1_BORDER_PROP_EDGE_MODE_X;
        if (name == L"ExtendY") return D2D1_BORDER_PROP_EDGE_MODE_Y;
        return -1;
    }
    UINT PropertyCount() const { return 2; }
    IInspectable Property(UINT index) const
    {
        switch (index)
        {
        case D2D1_BORDER_PROP_EDGE_MODE_X:
        case D2D1_BORDER_PROP_EDGE_MODE_Y:
            return wf::PropertyValue::CreateUInt32(D2D1_BORDER_EDGE_MODE_WRAP);
        default:
            return nullptr;
        }
    }
    wge::IGraphicsEffectSource SourceAt(UINT index) const { return index == 0 ? Source : nullptr; }
    UINT SourceCount() const { return 1; }
};

struct CompositeEffect : EffectBase<CompositeEffect>
{
    std::vector<wge::IGraphicsEffectSource> Sources;

    GUID EffectId() const { return CLSID_D2D1Composite; }
    int PropertyIndex(std::wstring_view name) const { return name == L"Mode" ? D2D1_COMPOSITE_PROP_MODE : -1; }
    UINT PropertyCount() const { return 1; }
    IInspectable Property(UINT index) const
    {
        return index == D2D1_COMPOSITE_PROP_MODE ? wf::PropertyValue::CreateUInt32(D2D1_COMPOSITE_MODE_SOURCE_OVER) : nullptr;
    }
    wge::IGraphicsEffectSource SourceAt(UINT index) const { return index < Sources.size() ? Sources[index] : nullptr; }
    UINT SourceCount() const { return (UINT)Sources.size(); }
};

struct ColorMatrixEffect : EffectBase<ColorMatrixEffect>
{
    wge::IGraphicsEffectSource Source{ nullptr };
    // D2D1_MATRIX_5X4_F, row-major, identity.
    float Matrix[20] =
    {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1,
        0, 0, 0, 0,
    };

    GUID EffectId() const { return CLSID_D2D1ColorMatrix; }
    int PropertyIndex(std::wstring_view name) const
    {
        if (name == L"ColorMatrix") return D2D1_COLORMATRIX_PROP_COLOR_MATRIX;
        if (name == L"AlphaMode") return D2D1_COLORMATRIX_PROP_ALPHA_MODE;
        if (name == L"ClampOutput") return D2D1_COLORMATRIX_PROP_CLAMP_OUTPUT;
        return -1;
    }
    UINT PropertyCount() const { return 3; }
    IInspectable Property(UINT index) const
    {
        switch (index)
        {
        case D2D1_COLORMATRIX_PROP_COLOR_MATRIX:
            return wf::PropertyValue::CreateSingleArray(winrt::array_view<const float>(Matrix, Matrix + 20));
        case D2D1_COLORMATRIX_PROP_ALPHA_MODE:
            return wf::PropertyValue::CreateUInt32(D2D1_COLORMATRIX_ALPHA_MODE_PREMULTIPLIED);
        case D2D1_COLORMATRIX_PROP_CLAMP_OUTPUT:
            return wf::PropertyValue::CreateBoolean(false);
        default:
            return nullptr;
        }
    }
    wge::IGraphicsEffectSource SourceAt(UINT index) const { return index == 0 ? Source : nullptr; }
    UINT SourceCount() const { return 1; }
};

// ---------------------------------------------------------------------------------------------------------------
// The WindhawkBlur brush
//
// A XAML brush drawn by the compositor: the backdrop behind the element, blurred, optionally desaturated,
// pushed towards the tint's luminosity and dusted with noise, then the tint colour laid over it. When the theme
// gives a fallback colour, that flat colour is used instead while energy saver is on or transparency effects are
// turned off in Settings. Tint and fallback may be theme resources; a proxy SolidColorBrush bound to the resource
// is put in the element's resources so a theme change is noticed.
// ---------------------------------------------------------------------------------------------------------------

struct XamlBlurBrush : Media::XamlCompositionBrushBaseT<XamlBlurBrush>
{
    XamlBlurBrush(UIElement const& element, styler::BlurParams const& params) :
        m_compositor(Hosting::ElementCompositionPreview::GetElementVisual(element).Compositor()),
        m_params(params)
    {
        m_tint = ToColor(params.tint);
        if (params.fallbackColor)
        {
            m_fallbackColor = ToColor(*params.fallbackColor);
        }

        auto fe = element.try_as<FrameworkElement>();

        auto createProxy = [&](std::wstring const& key) -> Media::SolidColorBrush
        {
            if (!fe)
            {
                return nullptr;
            }
            std::wstring xaml = L"<SolidColorBrush xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\" Color=\"{ThemeResource ";
            xaml += key;
            xaml += L"}\"/>";
            try
            {
                return Markup::XamlReader::Load(winrt::hstring(xaml)).try_as<Media::SolidColorBrush>();
            }
            catch (winrt::hresult_error const& e)
            {
                SP_LogError(L"WindhawkBlur: theme resource %s could not be read: 0x%08X", key.c_str(), (unsigned)e.code());
                return nullptr;
            }
        };

        static std::atomic<uint64_t> s_proxyCounter{ 0 };

        if (!params.tintThemeResourceKey.empty())
        {
            if (auto proxy = createProxy(params.tintThemeResourceKey))
            {
                m_proxyKey = winrt::hstring(L"__SpBlurProxy_" + std::to_wstring(++s_proxyCounter));
                fe.Resources().Insert(winrt::box_value(m_proxyKey), proxy);
                m_proxyBrush = proxy;
                m_weakProxyElement = winrt::make_weak(fe);
                proxy.RegisterPropertyChangedCallback(Media::SolidColorBrush::ColorProperty(),
                    [weak = get_weak()](DependencyObject const&, DependencyProperty const&)
                    {
                        if (auto self = weak.get())
                        {
                            self->RefreshBrush();
                        }
                    });
            }
        }

        if (!params.fallbackThemeResourceKey.empty())
        {
            if (auto proxy = createProxy(params.fallbackThemeResourceKey))
            {
                m_fallbackProxyKey = winrt::hstring(L"__SpBlurFallbackProxy_" + std::to_wstring(++s_proxyCounter));
                fe.Resources().Insert(winrt::box_value(m_fallbackProxyKey), proxy);
                m_fallbackProxyBrush = proxy;
                if (!m_weakProxyElement.get())
                {
                    m_weakProxyElement = winrt::make_weak(fe);
                }
                proxy.RegisterPropertyChangedCallback(Media::SolidColorBrush::ColorProperty(),
                    [weak = get_weak()](DependencyObject const&, DependencyProperty const&)
                    {
                        if (auto self = weak.get())
                        {
                            self->RefreshBrush();
                        }
                    });
            }
        }

        if (m_fallbackColor || !params.fallbackThemeResourceKey.empty())
        {
            m_dispatcher = winrt::Windows::System::DispatcherQueue::GetForCurrentThread();
            try
            {
                m_uiSettings = winrt::Windows::UI::ViewManagement::UISettings();
                auto dispatcher = m_dispatcher;
                m_advancedEffectsToken = m_uiSettings.AdvancedEffectsEnabledChanged(
                    [weak = get_weak(), dispatcher](auto&&, auto&&)
                    {
                        if (dispatcher)
                        {
                            dispatcher.TryEnqueue([weak] { if (auto self = weak.get()) self->RefreshBrush(); });
                        }
                    });
                m_energySaverToken = winrt::Windows::System::Power::PowerManager::EnergySaverStatusChanged(
                    [weak = get_weak(), dispatcher](auto&&, auto&&)
                    {
                        if (dispatcher)
                        {
                            dispatcher.TryEnqueue([weak] { if (auto self = weak.get()) self->RefreshBrush(); });
                        }
                    });
            }
            catch (winrt::hresult_error const& e)
            {
                SP_LogDebug(L"WindhawkBlur: fallback listeners not registered: 0x%08X", (unsigned)e.code());
            }

            // On 24H2 and later the registry is the one signal that follows "Always use energy saver".
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Power", 0, KEY_NOTIFY, &m_powerKey) == ERROR_SUCCESS)
            {
                m_regNotifyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (m_regNotifyEvent &&
                    RegNotifyChangeKeyValue(m_powerKey, FALSE, REG_NOTIFY_CHANGE_LAST_SET, m_regNotifyEvent, TRUE) == ERROR_SUCCESS)
                {
                    if (!RegisterWaitForSingleObject(&m_regWaitHandle, m_regNotifyEvent, OnPowerKeyChanged, this, INFINITE, WT_EXECUTEINWAITTHREAD))
                    {
                        m_regWaitHandle = nullptr;
                    }
                }
                if (!m_regWaitHandle)
                {
                    if (m_regNotifyEvent)
                    {
                        CloseHandle(m_regNotifyEvent);
                        m_regNotifyEvent = nullptr;
                    }
                    RegCloseKey(m_powerKey);
                    m_powerKey = nullptr;
                }
            }
        }
    }

    ~XamlBlurBrush()
    {
        if (m_regWaitHandle)
        {
            UnregisterWaitEx(m_regWaitHandle, INVALID_HANDLE_VALUE);
            m_regWaitHandle = nullptr;
        }
        if (m_regNotifyEvent)
        {
            CloseHandle(m_regNotifyEvent);
            m_regNotifyEvent = nullptr;
        }
        if (m_powerKey)
        {
            RegCloseKey(m_powerKey);
            m_powerKey = nullptr;
        }

        try
        {
            if (m_uiSettings && m_advancedEffectsToken.value)
            {
                m_uiSettings.AdvancedEffectsEnabledChanged(m_advancedEffectsToken);
            }
        }
        catch (...)
        {
        }
        try
        {
            if (m_energySaverToken.value)
            {
                winrt::Windows::System::Power::PowerManager::EnergySaverStatusChanged(m_energySaverToken);
            }
        }
        catch (...)
        {
        }

        if (auto element = m_weakProxyElement.get())
        {
            try
            {
                if (!m_proxyKey.empty())
                {
                    element.Resources().Remove(winrt::box_value(m_proxyKey));
                }
                if (!m_fallbackProxyKey.empty())
                {
                    element.Resources().Remove(winrt::box_value(m_fallbackProxyKey));
                }
            }
            catch (...)
            {
            }
        }
    }

    void OnConnected()
    {
        try
        {
            if (!CompositionBrush())
            {
                RefreshThemeColors();
                CompositionBrush(ShouldUseFallback() ? CreateFallbackBrush() : CreateEffectBrush());
            }
        }
        catch (winrt::hresult_error const& e)
        {
            SP_LogError(L"WindhawkBlur: brush not created: 0x%08X", (unsigned)e.code());
        }
    }

    void OnDisconnected()
    {
        try
        {
            if (auto brush = CompositionBrush())
            {
                brush.Close();
                CompositionBrush(nullptr);
            }
        }
        catch (...)
        {
        }
    }

private:
    static winrt::Windows::UI::Color ToColor(styler::BlurColor const& c)
    {
        return winrt::Windows::UI::Color{ c.a, c.r, c.g, c.b };
    }

    static void CALLBACK OnPowerKeyChanged(PVOID context, BOOLEAN)
    {
        auto* self = static_cast<XamlBlurBrush*>(context);
        if (self->m_powerKey && self->m_regNotifyEvent)
        {
            RegNotifyChangeKeyValue(self->m_powerKey, FALSE, REG_NOTIFY_CHANGE_LAST_SET, self->m_regNotifyEvent, TRUE);
        }
        if (self->m_dispatcher)
        {
            auto weak = self->get_weak();
            self->m_dispatcher.TryEnqueue([weak] { if (auto strong = weak.get()) strong->RefreshBrush(); });
        }
    }

    void RefreshThemeColors()
    {
        if (m_proxyBrush)
        {
            m_tint = m_proxyBrush.Color();
            if (m_params.tintOpacity)
            {
                m_tint.A = *m_params.tintOpacity;
            }
        }
        if (m_fallbackProxyBrush)
        {
            m_fallbackColor = m_fallbackProxyBrush.Color();
        }
    }

    bool ShouldUseFallback() const
    {
        if (!m_fallbackColor && m_params.fallbackThemeResourceKey.empty())
        {
            return false;
        }

        bool energySaver = false;
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Power", 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS)
        {
            DWORD value = 0, type = 0, size = sizeof(value);
            if (RegQueryValueExW(key, L"EnergySaverState", nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size) == ERROR_SUCCESS && type == REG_DWORD)
            {
                energySaver = (value == 1);
            }
            RegCloseKey(key);
        }
        if (!energySaver)
        {
            SYSTEM_POWER_STATUS status = {};
            if (GetSystemPowerStatus(&status) && status.SystemStatusFlag != 0)
            {
                energySaver = true;
            }
        }

        bool effectsOff = false;
        if (m_uiSettings)
        {
            try
            {
                effectsOff = !m_uiSettings.AdvancedEffectsEnabled();
            }
            catch (...)
            {
            }
        }
        return energySaver || effectsOff;
    }

    void RefreshBrush()
    {
        try
        {
            if (auto brush = CompositionBrush())
            {
                brush.Close();
                CompositionBrush(nullptr);
                OnConnected();
            }
        }
        catch (...)
        {
        }
    }

    wuc::CompositionBrush CreateFallbackBrush()
    {
        return m_compositor.CreateColorBrush(m_fallbackColor.value_or(m_tint));
    }

    wuc::CompositionBrush CreateEffectBrush()
    {
        constexpr float kLumaR = 0.2126f;
        constexpr float kLumaG = 0.7152f;
        constexpr float kLumaB = 0.0722f;

        auto backdrop = m_compositor.CreateBackdropBrush();

        auto blur = winrt::make_self<GaussianBlurEffect>();
        blur->Source = wuc::CompositionEffectSourceParameter(L"backdrop");
        blur->BlurAmount = m_params.blurAmount;
        blur->Name(L"BlurEffect");
        wge::IGraphicsEffectSource top = blur.as<wge::IGraphicsEffectSource>();

        if (m_params.tintSaturation && *m_params.tintSaturation != 1.0f)
        {
            const float s = std::max(*m_params.tintSaturation, 0.0f);
            const float inv = 1.0f - s;
            auto sat = winrt::make_self<ColorMatrixEffect>();
            sat->Source = top;
            float* m = sat->Matrix;
            m[0] = inv * kLumaR + s; m[1] = inv * kLumaR;     m[2] = inv * kLumaR;     m[3] = 0;
            m[4] = inv * kLumaG;     m[5] = inv * kLumaG + s; m[6] = inv * kLumaG;     m[7] = 0;
            m[8] = inv * kLumaB;     m[9] = inv * kLumaB;     m[10] = inv * kLumaB + s; m[11] = 0;
            m[12] = 0;               m[13] = 0;               m[14] = 0;                m[15] = 1;
            m[16] = 0;               m[17] = 0;               m[18] = 0;                m[19] = 0;
            sat->Name(L"SaturationEffect");
            top = sat.as<wge::IGraphicsEffectSource>();
        }

        if (m_params.tintLuminosityOpacity && *m_params.tintLuminosityOpacity > 0.0f)
        {
            const float op = std::clamp(*m_params.tintLuminosityOpacity, 0.0f, 1.0f);
            const float tintLum = (m_tint.R / 255.0f) * kLumaR + (m_tint.G / 255.0f) * kLumaG + (m_tint.B / 255.0f) * kLumaB;
            auto lum = winrt::make_self<ColorMatrixEffect>();
            lum->Source = top;
            float* m = lum->Matrix;
            m[0] = 1.0f - kLumaR * op; m[1] = -(kLumaR * op);     m[2] = -(kLumaR * op);     m[3] = 0;
            m[4] = -(kLumaG * op);     m[5] = 1.0f - kLumaG * op; m[6] = -(kLumaG * op);     m[7] = 0;
            m[8] = -(kLumaB * op);     m[9] = -(kLumaB * op);     m[10] = 1.0f - kLumaB * op; m[11] = 0;
            m[12] = 0;                 m[13] = 0;                 m[14] = 0;                  m[15] = 1;
            m[16] = tintLum * op;      m[17] = tintLum * op;      m[18] = tintLum * op;       m[19] = 0;
            lum->Name(L"LuminosityBlend");
            top = lum.as<wge::IGraphicsEffectSource>();
        }

        wuc::CompositionSurfaceBrush noiseBrush{ nullptr };
        if (m_params.noiseOpacity && *m_params.noiseOpacity > 0.0f)
        {
            auto stream = CreateNoiseStream(m_params.noiseDensity.value_or(1.0f));
            SP_LogDebug(L"WindhawkBlur: loading the noise surface");
            auto surface = Media::LoadedImageSurface::StartLoadFromStream(stream);
            SP_LogDebug(L"WindhawkBlur: noise surface created");
            noiseBrush = m_compositor.CreateSurfaceBrush(surface.as<wuc::ICompositionSurface>());
            noiseBrush.Stretch(wuc::CompositionStretch::None);

            auto border = winrt::make_self<BorderEffect>();
            border->Source = wuc::CompositionEffectSourceParameter(L"NoiseSource");

            const float nOp = std::clamp(*m_params.noiseOpacity, 0.0f, 1.0f);
            auto opacity = winrt::make_self<ColorMatrixEffect>();
            opacity->Source = border.as<wge::IGraphicsEffectSource>();
            opacity->Matrix[0] = nOp;
            opacity->Matrix[5] = nOp;
            opacity->Matrix[10] = nOp;
            opacity->Matrix[15] = nOp;
            opacity->Name(L"NoiseOpacityEffect");

            auto composite = winrt::make_self<CompositeEffect>();
            composite->Sources.push_back(top);
            composite->Sources.push_back(opacity.as<wge::IGraphicsEffectSource>());
            composite->Name(L"NoiseComposite");
            top = composite.as<wge::IGraphicsEffectSource>();
        }

        auto flood = winrt::make_self<FloodEffect>();
        flood->Color = m_tint;
        flood->Name(L"FloodEffect");

        auto composite = winrt::make_self<CompositeEffect>();
        composite->Sources.push_back(top);
        composite->Sources.push_back(flood.as<wge::IGraphicsEffectSource>());

        // Each step is named in the log when it fails: the compositor reports every mistake in the graph as
        // E_INVALIDARG, which says nothing about where.
        const wchar_t* stage = L"CreateEffectFactory";
        try
        {
            auto factory = m_compositor.CreateEffectFactory(composite.as<wge::IGraphicsEffect>());
            stage = L"CreateBrush";
            auto brush = factory.CreateBrush();
            stage = L"SetSourceParameter(backdrop)";
            brush.SetSourceParameter(L"backdrop", backdrop);
            if (noiseBrush)
            {
                stage = L"SetSourceParameter(NoiseSource)";
                brush.SetSourceParameter(L"NoiseSource", noiseBrush);
            }
            return brush;
        }
        catch (winrt::hresult_error const& e)
        {
            SP_LogError(L"WindhawkBlur: %s failed: 0x%08X %s", stage, (unsigned)e.code(), e.message().c_str());
            throw;
        }
    }

    wuc::Compositor m_compositor;
    styler::BlurParams m_params;
    winrt::Windows::UI::Color m_tint{};
    std::optional<winrt::Windows::UI::Color> m_fallbackColor;
    Media::SolidColorBrush m_proxyBrush{ nullptr };
    Media::SolidColorBrush m_fallbackProxyBrush{ nullptr };
    winrt::weak_ref<FrameworkElement> m_weakProxyElement;
    winrt::hstring m_proxyKey;
    winrt::hstring m_fallbackProxyKey;
    winrt::Windows::UI::ViewManagement::UISettings m_uiSettings{ nullptr };
    winrt::event_token m_advancedEffectsToken{};
    winrt::event_token m_energySaverToken{};
    winrt::Windows::System::DispatcherQueue m_dispatcher{ nullptr };
    HKEY m_powerKey = nullptr;
    HANDLE m_regNotifyEvent = nullptr;
    HANDLE m_regWaitHandle = nullptr;
};

// ---------------------------------------------------------------------------------------------------------------
// Reading and writing property values
// ---------------------------------------------------------------------------------------------------------------

// A property set as {TemplateBinding ...} reads back as a BindingExpression, which SetValue refuses; the
// animation base value is what can be put back.
IInspectable ReadLocalValue(DependencyObject const& object, DependencyProperty const& property)
{
    IInspectable value = object.ReadLocalValue(property);
    if (value)
    {
        const winrt::hstring className = winrt::get_class_name(value);
        if (className == L"Windows.UI.Xaml.Data.BindingExpressionBase" || className == L"Windows.UI.Xaml.Data.BindingExpression")
        {
            value = object.GetAnimationBaseValue(property);
        }
    }
    return value;
}

using UnboxedValue = std::variant<std::wstring, bool, char16_t, uint8_t, int16_t, uint16_t, int32_t, uint32_t, int64_t, uint64_t, float, double>;

std::optional<UnboxedValue> TryUnbox(IInspectable const& value)
{
    using wf::PropertyType;
    auto pv = value.try_as<wf::IPropertyValue>();
    if (!pv)
    {
        return std::nullopt;
    }
    switch (pv.Type())
    {
    case PropertyType::String:  return UnboxedValue{ std::wstring(pv.GetString()) };
    case PropertyType::Boolean: return UnboxedValue{ pv.GetBoolean() };
    case PropertyType::Char16:  return UnboxedValue{ pv.GetChar16() };
    case PropertyType::Double:  return UnboxedValue{ pv.GetDouble() };
    case PropertyType::Single:  return UnboxedValue{ pv.GetSingle() };
    case PropertyType::UInt8:   return UnboxedValue{ pv.GetUInt8() };
    case PropertyType::Int16:   return UnboxedValue{ pv.GetInt16() };
    case PropertyType::UInt16:  return UnboxedValue{ pv.GetUInt16() };
    case PropertyType::Int32:   return UnboxedValue{ pv.GetInt32() };
    case PropertyType::UInt32:  return UnboxedValue{ pv.GetUInt32() };
    case PropertyType::Int64:   return UnboxedValue{ pv.GetInt64() };
    case PropertyType::UInt64:  return UnboxedValue{ pv.GetUInt64() };
    case PropertyType::OtherType:
        // Enums box as int32.
        if (auto asInt = value.try_as<int32_t>())
        {
            return UnboxedValue{ *asInt };
        }
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

std::wstring FormatUnboxed(UnboxedValue const& v)
{
    return std::visit([](auto const& x) -> std::wstring
    {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::wstring>) return x;
        else if constexpr (std::is_same_v<T, bool>) return x ? L"True" : L"False";
        else if constexpr (std::is_same_v<T, char16_t>) return std::wstring(1, static_cast<wchar_t>(x));
        else if constexpr (std::is_floating_point_v<T>) return styler::FormatDoubleInvariant(static_cast<double>(x));
        else return std::to_wstring(x);
    }, v);
}

std::optional<double> UnboxedAsNumber(UnboxedValue const& v)
{
    return std::visit([](auto const& x) -> std::optional<double>
    {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::wstring>) return std::nullopt;
        else return static_cast<double>(x);
    }, v);
}

template <typename T, typename... Ts>
std::optional<bool> SameBoxedStruct(IInspectable const& a, IInspectable const& b)
{
    if (auto ra = a.try_as<wf::IReference<T>>())
    {
        auto rb = b.try_as<wf::IReference<T>>();
        return rb && ra.Value() == rb.Value();
    }
    if constexpr (sizeof...(Ts) > 0)
    {
        return SameBoxedStruct<Ts...>(a, b);
    }
    else
    {
        return std::nullopt;
    }
}

// Whether two values read from a property are the same local value. XAML boxes value types anew on every read,
// so those compare by value; reference types, UnsetValue included, by identity. An unrecognized boxed value type
// is taken as unchanged: adopting the mod's own value as "original" would leave it in place on cleanup.
bool SameLocalValue(IInspectable const& a, IInspectable const& b)
{
    if (a == b)
    {
        return true;
    }
    if (!a || !b)
    {
        return false;
    }

    auto ua = TryUnbox(a);
    auto ub = TryUnbox(b);
    if (ua || ub)
    {
        return ua && ub && std::visit([](auto const& x, auto const& y) -> bool
        {
            using X = std::decay_t<decltype(x)>;
            if constexpr (!std::is_same_v<X, std::decay_t<decltype(y)>>) return false;
            else if constexpr (std::is_floating_point_v<X>) return x == y || (std::isnan(x) && std::isnan(y));
            else return x == y;
        }, *ua, *ub);
    }

    if (auto same = SameBoxedStruct<Thickness, CornerRadius, GridLength, wf::Point, wf::Size, wf::Rect,
                                    winrt::Windows::UI::Color, winrt::Windows::UI::Text::FontWeight>(a, b))
    {
        return *same;
    }

    auto isBoxed = [](IInspectable const& v)
    {
        return v.try_as<wf::IPropertyValue>() != nullptr ||
               styler::StartsWith(std::wstring_view(winrt::get_class_name(v)), L"Windows.Foundation.IReference`1<");
    };
    return isBoxed(a) && isBoxed(b);
}

// A Fill on the taskbar's BackgroundFill set before the shell's own OnApplyTemplate ran was seen to crash or be
// overwritten, so the first write to it is deferred to the dispatcher.
thread_local std::list<std::pair<winrt::weak_ref<DependencyObject>, wf::IAsyncOperation<bool>>> g_delayedBackgroundFill;

IInspectable SetOrClearValue(DependencyObject const& object, DependencyProperty const& property,
                             PropertyOverrideValue const& overrideValue, bool initialApply = false)
{
    IInspectable value;
    if (auto* inspectable = std::get_if<IInspectable>(&overrideValue))
    {
        value = *inspectable;
    }
    else if (auto* blur = std::get_if<styler::BlurParams>(&overrideValue))
    {
        auto uiElement = object.try_as<UIElement>();
        if (!uiElement)
        {
            SP_LogDebug(L"WindhawkBlur wants a UIElement");
            return nullptr;
        }
        value = winrt::make<XamlBlurBrush>(uiElement, *blur);
    }
    else
    {
        return nullptr;
    }

    if (winrt::get_class_name(object) == L"Windows.UI.Xaml.Shapes.Rectangle" &&
        object.as<FrameworkElement>().Name() == L"BackgroundFill" &&
        property == Shapes::Shape::FillProperty())
    {
        auto it = std::find_if(g_delayedBackgroundFill.begin(), g_delayedBackgroundFill.end(),
            [&object](const auto& entry) { auto strong = entry.first.get(); return strong && strong == object; });

        if (value != DependencyProperty::UnsetValue() && initialApply && it == g_delayedBackgroundFill.end())
        {
            auto op = object.Dispatcher().TryRunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::High,
                [object, property, value]()
                {
                    g_modifyingProperty = true;
                    try
                    {
                        object.SetValue(property, value);
                    }
                    catch (winrt::hresult_error const& e)
                    {
                        SP_LogDebug(L"Delayed BackgroundFill write failed: 0x%08X", (unsigned)e.code());
                    }
                    g_modifyingProperty = false;
                    g_delayedBackgroundFill.remove_if([&object](const auto& entry) { auto strong = entry.first.get(); return strong && strong == object; });
                });
            g_delayedBackgroundFill.emplace_back(object, std::move(op));
            return value;
        }
        else if (it != g_delayedBackgroundFill.end())
        {
            it->second.Cancel();
            g_delayedBackgroundFill.erase(it);
        }
    }

    if (value == DependencyProperty::UnsetValue())
    {
        try
        {
            object.ClearValue(property);
        }
        catch (winrt::hresult_error const& e)
        {
            SP_LogDebug(L"ClearValue failed: 0x%08X", (unsigned)e.code());
        }
        return value;
    }

    try
    {
        // A setter reads font weight back as an int, which SetValue rejects; box it as a FontWeight.
        if (property == Controls::TextBlock::FontWeightProperty() ||
            property == Controls::Control::FontWeightProperty() ||
            property == Controls::RichTextBlock::FontWeightProperty() ||
            property == Controls::FontIcon::FontWeightProperty() ||
            property == Controls::ContentPresenter::FontWeightProperty())
        {
            if (auto asInt = value.try_as<int32_t>())
            {
                if (*asInt >= 0 && *asInt <= 0xFFFF)
                {
                    value = winrt::box_value(winrt::Windows::UI::Text::FontWeight{ static_cast<uint16_t>(*asInt) });
                }
            }
        }

        // Row and column definitions are written back into by layout, so each grid gets its own copy.
        Controls::Grid cloneOwner{ nullptr };
        if (auto columns = value.try_as<Controls::ColumnDefinitionCollection>())
        {
            cloneOwner = Controls::Grid();
            auto cloned = cloneOwner.ColumnDefinitions();
            for (auto const& column : columns)
            {
                Controls::ColumnDefinition c;
                c.Width(column.Width());
                c.MinWidth(column.MinWidth());
                c.MaxWidth(column.MaxWidth());
                cloned.Append(c);
            }
            value = cloned;
        }
        else if (auto rows = value.try_as<Controls::RowDefinitionCollection>())
        {
            cloneOwner = Controls::Grid();
            auto cloned = cloneOwner.RowDefinitions();
            for (auto const& row : rows)
            {
                Controls::RowDefinition r;
                r.Height(row.Height());
                r.MinHeight(row.MinHeight());
                r.MaxHeight(row.MaxHeight());
                cloned.Append(r);
            }
            value = cloned;
        }

        object.SetValue(property, value);
    }
    catch (winrt::hresult_error const& e)
    {
        SP_LogDebug(L"SetValue failed on %s: 0x%08X %s", winrt::get_class_name(object).c_str(), (unsigned)e.code(), HResultMessage(e).c_str());
    }

    return value;
}

// ---------------------------------------------------------------------------------------------------------------
// Turning rule text into DependencyProperty + value
//
// XAML itself does the work: the rules become Setters of a Style for the element's type, XamlReader parses it,
// and the Setters hand back the property and the converted value.
// ---------------------------------------------------------------------------------------------------------------

Style LoadStyleFromSetters(std::wstring_view type, std::wstring_view setters)
{
    std::wstring xaml =
        L"<ResourceDictionary"
        L" xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\""
        L" xmlns:x=\"http://schemas.microsoft.com/winfx/2006/xaml\""
        L" xmlns:d=\"http://schemas.microsoft.com/expression/blend/2008\""
        L" xmlns:mc=\"http://schemas.openxmlformats.org/markup-compatibility/2006\""
        L" xmlns:muxc=\"using:Microsoft.UI.Xaml.Controls\"";

    const size_t dot = type.rfind(L'.');
    if (dot != std::wstring_view::npos)
    {
        xaml += L" xmlns:spstyler=\"using:";
        xaml += styler::EscapeXmlAttribute(type.substr(0, dot));
        xaml += L"\">\n<Style TargetType=\"spstyler:";
        xaml += styler::EscapeXmlAttribute(type.substr(dot + 1));
        xaml += L"\">\n";
    }
    else
    {
        xaml += L">\n<Style TargetType=\"";
        xaml += styler::EscapeXmlAttribute(type);
        xaml += L"\">\n";
    }
    xaml += setters;
    xaml += L"</Style>\n</ResourceDictionary>";

    auto dictionary = Markup::XamlReader::Load(winrt::hstring(xaml)).as<ResourceDictionary>();
    auto first = dictionary.First().Current();
    return first.Value().as<Style>();
}

// Some types (JumpViewUI.JumpListListViewItem was one) cannot be named in a Style; the basic properties can
// still be set through a base type.
Style LoadStyleWithFallback(std::wstring_view type, std::wstring_view fallbackType, std::wstring_view setters)
{
    try
    {
        return LoadStyleFromSetters(type, setters);
    }
    catch (winrt::hresult_error const& e)
    {
        constexpr HRESULT kStowedException = 0x802B000A;
        if (e.code() != kStowedException || fallbackType.empty() || fallbackType == type)
        {
            throw;
        }
        return LoadStyleFromSetters(fallbackType, setters);
    }
}

std::wstring SetterXaml(std::wstring_view propertyName, std::wstring_view value, bool isXamlValue)
{
    std::wstring xaml = L"<Setter Property=\"";
    xaml += styler::EscapeXmlAttribute(propertyName);
    xaml += L"\"";
    if (!isXamlValue)
    {
        xaml += L" Value=\"";
        xaml += styler::EscapeXmlAttribute(value);
        xaml += L"\"/>\n";
    }
    else
    {
        xaml += L">\n<Setter.Value>\n";
        xaml += value;
        xaml += L"\n</Setter.Value>\n</Setter>\n";
    }
    return xaml;
}

std::wstring FallbackTypeFor(PCWSTR fallbackClassName)
{
    return fallbackClassName ? std::wstring(fallbackClassName) : std::wstring(winrt::name_of<FrameworkElement>());
}

const ResolvedRules& ResolveRules(CustomizationRule& rule, std::wstring_view fallbackType)
{
    if (rule.resolved)
    {
        return *rule.resolved;
    }

    ResolvedRules resolved;
    const auto& valueRules = rule.unresolved.valueRules;
    const auto& captureRules = rule.unresolved.captureRules;

    try
    {
        if (!valueRules.empty() || !captureRules.empty())
        {
            std::wstring xaml;
            std::vector<std::optional<PropertyOverrideValue>> parsedValues;
            parsedValues.reserve(valueRules.size());

            for (const auto& valueRule : valueRules)
            {
                const bool isDynamic = valueRule.IsDynamic();

                std::optional<PropertyOverrideValue> parsed;
                if (!isDynamic && valueRule.isXamlValue)
                {
                    if (auto blur = styler::ParseWindhawkBlur(valueRule.value))
                    {
                        parsed = PropertyOverrideValue{ *blur };
                    }
                }
                parsedValues.push_back(parsed);

                // A placeholder setter still resolves the property; the value comes from elsewhere.
                if (isDynamic || parsed || (valueRule.isXamlValue && valueRule.value.empty()))
                {
                    xaml += SetterXaml(valueRule.propertyName, L"{x:Null}", false);
                }
                else
                {
                    xaml += SetterXaml(valueRule.propertyName, valueRule.value, valueRule.isXamlValue);
                }
            }
            for (const auto& captureRule : captureRules)
            {
                xaml += SetterXaml(captureRule.propertyName, L"{x:Null}", false);
            }

            Style style = LoadStyleWithFallback(rule.matcher.spec.type, fallbackType, xaml);
            auto setters = style.Setters();

            uint32_t setterIndex = 0;
            for (size_t i = 0; i < valueRules.size(); ++i, ++setterIndex)
            {
                const auto& valueRule = valueRules[i];
                auto setter = setters.GetAt(setterIndex).as<Setter>();
                DependencyProperty property = setter.Property();
                if (valueRule.IsDynamic())
                {
                    resolved.overrides[property][valueRule.visualState] = DynamicStyleTemplate{ valueRule.propertyName, valueRule.value, valueRule.isXamlValue };
                    resolved.hasDynamicValues = true;
                }
                else if (parsedValues[i])
                {
                    resolved.overrides[property][valueRule.visualState] = *parsedValues[i];
                }
                else if (valueRule.isXamlValue && valueRule.value.empty())
                {
                    resolved.overrides[property][valueRule.visualState] = PropertyOverrideValue{ DependencyProperty::UnsetValue() };
                }
                else
                {
                    resolved.overrides[property][valueRule.visualState] = PropertyOverrideValue{ setter.Value() };
                }
            }
            for (const auto& captureRule : captureRules)
            {
                auto setter = setters.GetAt(setterIndex++).as<Setter>();
                resolved.captures.push_back({ setter.Property(), captureRule.varName });
            }
        }
    }
    catch (winrt::hresult_error const& e)
    {
        SP_LogError(L"Rules for '%s' could not be read by XAML (0x%08X %s); the target is skipped",
                    rule.targetText.c_str(), (unsigned)e.code(), HResultMessage(e).c_str());
        resolved = ResolvedRules();
    }
    catch (std::exception const& e)
    {
        SP_LogError(L"Rules for '%s' are not understood (%S); the target is skipped", rule.targetText.c_str(), e.what());
        resolved = ResolvedRules();
    }

    rule.resolved = std::move(resolved);
    return *rule.resolved;
}

// One rule body re-resolved after its {{...}} were expanded.
std::optional<PropertyOverrideValue> ResolveExpandedValue(std::wstring_view type, std::wstring_view fallbackType,
                                                          std::wstring_view propertyName, std::wstring_view expanded, bool isXamlValue)
{
    try
    {
        if (isXamlValue)
        {
            if (auto blur = styler::ParseWindhawkBlur(expanded))
            {
                return PropertyOverrideValue{ *blur };
            }
            if (styler::Trim(expanded).empty())
            {
                return PropertyOverrideValue{ DependencyProperty::UnsetValue() };
            }
        }

        Style style = LoadStyleWithFallback(type, fallbackType, SetterXaml(propertyName, expanded, isXamlValue));
        return PropertyOverrideValue{ style.Setters().GetAt(0).as<Setter>().Value() };
    }
    catch (winrt::hresult_error const& e)
    {
        SP_LogDebug(L"Dynamic value '%.*s' for %.*s not accepted: 0x%08X", (int)expanded.size(), expanded.data(),
                    (int)propertyName.size(), propertyName.data(), (unsigned)e.code());
    }
    catch (std::exception const& e)
    {
        SP_LogDebug(L"Dynamic value for %.*s not understood: %S", (int)propertyName.size(), propertyName.data(), e.what());
    }
    return std::nullopt;
}

const std::vector<PropertyKeyValue>& ResolveConditions(Matcher& matcher, std::wstring_view fallbackType)
{
    if (matcher.conditions)
    {
        return *matcher.conditions;
    }

    std::vector<PropertyKeyValue> conditions;
    try
    {
        if (!matcher.spec.propertyValues.empty())
        {
            std::wstring xaml;
            for (const auto& [property, value] : matcher.spec.propertyValues)
            {
                xaml += SetterXaml(property, value, false);
            }
            Style style = LoadStyleWithFallback(matcher.spec.type, fallbackType, xaml);
            auto setters = style.Setters();
            for (uint32_t i = 0; i < matcher.spec.propertyValues.size(); ++i)
            {
                auto setter = setters.GetAt(i).as<Setter>();
                conditions.push_back({ setter.Property(), setter.Value() });
            }
        }
    }
    catch (winrt::hresult_error const& e)
    {
        SP_LogError(L"The conditions on %s could not be read by XAML: 0x%08X %s", matcher.spec.type.c_str(), (unsigned)e.code(), HResultMessage(e).c_str());
        conditions.clear();
        // A condition that cannot be read must not match anything: an impossible entry does that.
        conditions.push_back({ nullptr, nullptr });
    }

    matcher.conditions = std::move(conditions);
    return *matcher.conditions;
}

// ---------------------------------------------------------------------------------------------------------------
// Condition watches
//
// A [Property=Value] condition is decided when the element is matched. The taskbar recycles its buttons: a
// TaskListButton that showed one window is handed another, and its AutomationProperties.Name changes without any
// element being added or removed. So every element a condition was tested on is watched for that property, and
// when the outcome of a condition flips, the element and its subtree are matched again.
// ---------------------------------------------------------------------------------------------------------------

struct ConditionExpectation
{
    IInspectable expected;
    bool matched;
};

struct ConditionWatch
{
    DependencyProperty property{ nullptr };
    int64_t token = 0;
    std::vector<ConditionExpectation> expectations;
};

struct ConditionWatches
{
    winrt::weak_ref<FrameworkElement> element;
    std::vector<ConditionWatch> watches;
};

thread_local std::unordered_map<ElementId, ConditionWatches> g_conditionWatches;
thread_local std::vector<ElementId> g_pendingRematch;
thread_local bool g_rematchQueued;

void ReapplyCustomizationsForSubtree(FrameworkElement const& element);

bool ConditionMatches(IInspectable const& expected, IInspectable const& actual)
{
    if (!actual || actual == DependencyProperty::UnsetValue())
    {
        return false;
    }
    auto ue = TryUnbox(expected);
    auto ua = TryUnbox(actual);
    return ue && ua && *ue == *ua;
}

void RunPendingRematches()
{
    g_rematchQueued = false;
    if (!g_initializedForThread)
    {
        g_pendingRematch.clear();
        return;
    }

    auto pending = std::move(g_pendingRematch);
    g_pendingRematch.clear();
    for (ElementId elementId : pending)
    {
        auto it = g_conditionWatches.find(elementId);
        if (it == g_conditionWatches.end())
        {
            continue;
        }
        if (auto element = it->second.element.get())
        {
            SP_LogDebug(L"A condition on %s changed; matching its subtree again", winrt::get_class_name(element).c_str());
            ReapplyCustomizationsForSubtree(element);
        }
    }
}

void QueueRematch(ElementId elementId)
{
    if (std::find(g_pendingRematch.begin(), g_pendingRematch.end(), elementId) == g_pendingRematch.end())
    {
        g_pendingRematch.push_back(elementId);
    }
    if (g_rematchQueued)
    {
        return;
    }
    try
    {
        auto queue = winrt::Windows::System::DispatcherQueue::GetForCurrentThread();
        if (queue && queue.TryEnqueue([]() { try { RunPendingRematches(); } catch (...) {} }))
        {
            g_rematchQueued = true;
        }
    }
    catch (...)
    {
    }
}

void WatchCondition(FrameworkElement const& element, DependencyProperty const& property, IInspectable const& expected, bool matched)
{
    const ElementId elementId = ElementIdFromElement(element);
    if (elementId == ElementId::None)
    {
        return;
    }

    auto& entry = g_conditionWatches[elementId];
    if (!entry.element.get())
    {
        entry.element = element;
    }

    ConditionWatch* watch = nullptr;
    for (auto& existing : entry.watches)
    {
        if (existing.property == property)
        {
            watch = &existing;
            break;
        }
    }

    if (!watch)
    {
        entry.watches.push_back({});
        watch = &entry.watches.back();
        watch->property = property;
        try
        {
            watch->token = element.RegisterPropertyChangedCallback(property,
                [elementId](DependencyObject const& sender, DependencyProperty const& changed)
                {
                    try
                    {
                        if (g_modifyingProperty || !g_initializedForThread)
                        {
                            return;
                        }
                        auto it = g_conditionWatches.find(elementId);
                        if (it == g_conditionWatches.end())
                        {
                            return;
                        }
                        for (auto& w : it->second.watches)
                        {
                            if (w.property != changed)
                            {
                                continue;
                            }
                            IInspectable actual = ReadLocalValue(sender, changed);
                            bool flipped = false;
                            for (auto& expectation : w.expectations)
                            {
                                const bool now = ConditionMatches(expectation.expected, actual);
                                if (now != expectation.matched)
                                {
                                    expectation.matched = now;
                                    flipped = true;
                                }
                            }
                            if (flipped)
                            {
                                QueueRematch(elementId);
                            }
                        }
                    }
                    catch (...)
                    {
                    }
                });
        }
        catch (winrt::hresult_error const&)
        {
            entry.watches.pop_back();
            return;
        }
    }

    for (const auto& expectation : watch->expectations)
    {
        if (expectation.expected == expected)
        {
            return;
        }
    }
    watch->expectations.push_back({ expected, matched });
}

void RemoveConditionWatches(ElementId elementId)
{
    auto it = g_conditionWatches.find(elementId);
    if (it == g_conditionWatches.end())
    {
        return;
    }
    if (auto element = it->second.element.get())
    {
        for (const auto& watch : it->second.watches)
        {
            try
            {
                element.UnregisterPropertyChangedCallback(watch.property, watch.token);
            }
            catch (...)
            {
            }
        }
    }
    g_conditionWatches.erase(it);
}

// ---------------------------------------------------------------------------------------------------------------
// Matching
// ---------------------------------------------------------------------------------------------------------------

VisualStateGroup FindVisualStateGroup(FrameworkElement const& element, std::wstring_view groupName)
{
    // Reading the groups of these two crashes inside the shell (a list of size one whose only item is null).
    const winrt::hstring className = winrt::get_class_name(element);
    if (className == L"Taskbar.TaskListButtonPanel" || className == L"SearchUx.SearchUI.SearchButtonRootGrid")
    {
        auto parent = Media::VisualTreeHelper::GetParent(element).try_as<FrameworkElement>();
        if (parent)
        {
            const winrt::hstring parentClass = winrt::get_class_name(parent);
            if (parentClass == L"Taskbar.SearchBoxLaunchListButton" || parentClass == L"SearchUx.SearchUI.SearchPillButton")
            {
                return nullptr;
            }
        }
    }

    for (auto const& group : VisualStateManager::GetVisualStateGroups(element))
    {
        if (group.Name() == groupName)
        {
            return group;
        }
    }
    return nullptr;
}

bool TestMatcher(FrameworkElement const& element, Matcher& matcher, VisualStateGroup* visualStateGroup, PCWSTR fallbackClassName)
{
    if (!matcher.spec.type.empty())
    {
        const winrt::hstring className = winrt::get_class_name(element);
        if (matcher.spec.type != std::wstring_view(className) && (!fallbackClassName || matcher.spec.type != fallbackClassName))
        {
            return false;
        }
    }

    if (!matcher.spec.name.empty() && matcher.spec.name != std::wstring_view(element.Name()))
    {
        return false;
    }

    if (matcher.spec.oneBasedIndex)
    {
        auto parent = Media::VisualTreeHelper::GetParent(element);
        if (!parent)
        {
            return false;
        }
        const int index = matcher.spec.oneBasedIndex - 1;
        if (index < 0 || index >= Media::VisualTreeHelper::GetChildrenCount(parent) ||
            !SameObject(Media::VisualTreeHelper::GetChild(parent, index), element))
        {
            return false;
        }
    }

    bool result = true;
    for (const auto& condition : ResolveConditions(matcher, FallbackTypeFor(fallbackClassName)))
    {
        if (!condition.first)
        {
            return false;
        }
        const IInspectable actual = ReadLocalValue(element, condition.first);
        const bool matched = ConditionMatches(condition.second, actual);
        WatchCondition(element, condition.first, condition.second, matched);
        if (!matched)
        {
            result = false;
        }
    }
    if (!result)
    {
        return false;
    }

    if (matcher.spec.visualStateGroupName && visualStateGroup)
    {
        *visualStateGroup = FindVisualStateGroup(element, *matcher.spec.visualStateGroupName);
    }
    return true;
}

struct ElementRules
{
    std::unordered_map<VisualStateGroup, PropertyOverrides> overridesPerGroup;
    std::vector<CaptureSpec> captures;
    bool hasDynamicValues = false;
};

// Later rules win over earlier ones for the same property, which is why the list is walked backwards.
ElementRules FindElementRules(FrameworkElement const& element, PCWSTR fallbackClassName)
{
    ElementRules result;
    std::unordered_set<DependencyProperty> propertiesTaken;
    std::unordered_set<std::wstring> capturesTaken;

    for (auto it = g_rules.rbegin(); it != g_rules.rend(); ++it)
    {
        CustomizationRule& rule = *it;
        VisualStateGroup visualStateGroup = nullptr;

        if (!TestMatcher(element, rule.matcher, &visualStateGroup, fallbackClassName))
        {
            continue;
        }

        auto& parents = rule.parents;
        auto matchParents = [&](auto& self, FrameworkElement const& from, size_t index) -> bool
        {
            if (index >= parents.size())
            {
                return true;
            }
            Matcher& matcher = parents[index];

            if (matcher.spec.kind == styler::MatcherSpec::Kind::Root)
            {
                return !Media::VisualTreeHelper::GetParent(from) && self(self, from, index + 1);
            }

            if (matcher.spec.kind == styler::MatcherSpec::Kind::Wildcard)
            {
                // Always followed by a real matcher; try every ancestor for it and backtrack.
                Matcher& next = parents[index + 1];
                FrameworkElement current = from;
                while (true)
                {
                    auto parent = Media::VisualTreeHelper::GetParent(current).try_as<FrameworkElement>();
                    if (!parent)
                    {
                        return false;
                    }
                    current = parent;
                    if (TestMatcher(current, next, &visualStateGroup, nullptr) && self(self, current, index + 2))
                    {
                        return true;
                    }
                }
            }

            auto parent = Media::VisualTreeHelper::GetParent(from).try_as<FrameworkElement>();
            if (!parent || !TestMatcher(parent, matcher, &visualStateGroup, nullptr))
            {
                return false;
            }
            return self(self, parent, index + 1);
        };

        if (!matchParents(matchParents, element, 0))
        {
            continue;
        }

        const ResolvedRules& resolved = ResolveRules(rule, FallbackTypeFor(fallbackClassName));
        result.hasDynamicValues = result.hasDynamicValues || resolved.hasDynamicValues;

        auto& overridesForGroup = result.overridesPerGroup[visualStateGroup];
        for (const auto& [property, valuesPerState] : resolved.overrides)
        {
            if (!propertiesTaken.insert(property).second)
            {
                continue;
            }
            auto& cell = overridesForGroup[property];
            for (const auto& [state, value] : valuesPerState)
            {
                cell.insert({ state, value });
            }
        }
        for (const auto& capture : resolved.captures)
        {
            if (capturesTaken.insert(capture.varName).second)
            {
                result.captures.push_back(capture);
            }
        }
    }

    for (auto it = result.overridesPerGroup.begin(); it != result.overridesPerGroup.end();)
    {
        it = it->second.empty() ? result.overridesPerGroup.erase(it) : std::next(it);
    }
    return result;
}

// ---------------------------------------------------------------------------------------------------------------
// Style variable resolution
//
// A consumer reads the capture closest to it: deepest common ancestor first, then the shallowest capture (the
// one on the consumer's own ancestor chain), then the newest.
// ---------------------------------------------------------------------------------------------------------------

std::vector<void*> AncestorChain(FrameworkElement const& element)
{
    std::vector<void*> chain;
    DependencyObject current = element;
    for (int depth = 0; current && depth < 256; ++depth)
    {
        chain.push_back(IdentityKey(current));
        try
        {
            current = Media::VisualTreeHelper::GetParent(current);
        }
        catch (...)
        {
            break;
        }
    }
    std::reverse(chain.begin(), chain.end());   // root first
    return chain;
}

std::pair<int, int> CaptureRank(std::vector<void*> const& consumerChain, FrameworkElement const& captureElement)
{
    if (!captureElement)
    {
        return { 0, std::numeric_limits<int>::max() };
    }
    const std::vector<void*> captureChain = AncestorChain(captureElement);
    int common = 0;
    while (common < (int)consumerChain.size() && common < (int)captureChain.size() && consumerChain[common] == captureChain[common])
    {
        ++common;
    }
    return { -common, (int)captureChain.size() };
}

struct VariableResolution
{
    const styler::VariableValue* value = nullptr;   // into state->variables; read it before applying anything
    ElementId owner = ElementId::None;
};

VariableResolution FindWinningCapture(VariableState* state, std::wstring const& varName, FrameworkElement const& consumer)
{
    VariableResolution result;
    auto it = state->variables.find(varName);
    if (it == state->variables.end() || it->second.empty())
    {
        return result;
    }

    const auto& captures = it->second;
    if (captures.size() == 1)
    {
        return { &captures.front().value, captures.front().elementId };
    }

    const std::vector<void*> consumerChain = consumer ? AncestorChain(consumer) : std::vector<void*>();
    std::pair<int, int> best;
    for (const auto& capture : captures)
    {
        FrameworkElement captureElement{ nullptr };
        if (auto stateIt = g_elementStates.find(capture.elementId); stateIt != g_elementStates.end())
        {
            captureElement = stateIt->second.element.get();
        }
        const auto rank = CaptureRank(consumerChain, captureElement);
        if (!result.value || rank <= best)
        {
            best = rank;
            result = { &capture.value, capture.elementId };
        }
    }
    return result;
}

ElementId PickWinningOwner(VariableState* state, std::wstring const& varName, FrameworkElement const& consumer)
{
    return FindWinningCapture(state, varName, consumer).owner;
}

styler::VariableValue ReadCapturedValue(FrameworkElement const& element, DependencyProperty const& property)
{
    styler::VariableValue out;
    IInspectable value{ nullptr };
    try
    {
        value = element.GetValue(property);   // the effective value: ActualWidth has no local value
    }
    catch (winrt::hresult_error const&)
    {
    }
    if (!value || value == DependencyProperty::UnsetValue())
    {
        return out;
    }
    try
    {
        if (auto unboxed = TryUnbox(value))
        {
            out.text = FormatUnboxed(*unboxed);
            out.number = UnboxedAsNumber(*unboxed);
            out.substitutable = true;
            return out;
        }
        out.text = std::wstring(winrt::get_class_name(value));
    }
    catch (winrt::hresult_error const&)
    {
        out.text.clear();
    }
    return out;
}

void UpdateConsumers(VariableState* state, ElementId elementId, DependencyProperty const& property, PCWSTR fallbackClassName,
                     std::vector<VariableDependency> const& oldDeps, std::vector<VariableDependency> const& newDeps)
{
    if (!state)
    {
        return;
    }

    for (const auto& dep : oldDeps)
    {
        auto it = state->consumers.find(dep.name);
        if (it == state->consumers.end())
        {
            continue;
        }
        auto& consumers = it->second;
        const size_t before = consumers.size();
        consumers.erase(std::remove_if(consumers.begin(), consumers.end(),
            [&](const VariableConsumer& c) { return c.elementId == elementId && c.property == property; }), consumers.end());
        ReleaseElementRefs(state, elementId, before - consumers.size());
        if (consumers.empty())
        {
            state->consumers.erase(it);
        }
    }

    const std::wstring fallback = fallbackClassName ? fallbackClassName : L"";
    for (const auto& dep : newDeps)
    {
        auto& consumers = state->consumers[dep.name];
        const bool already = std::any_of(consumers.begin(), consumers.end(),
            [&](const VariableConsumer& c) { return c.elementId == elementId && c.property == property; });
        if (!already)
        {
            consumers.push_back({ elementId, property, fallback });
            AddElementRef(state, elementId);
        }
    }
}

// Records a write by something other than the mod as the property's pre-style value.
void AdoptExternalValueAsOriginal(FrameworkElement const& element, DependencyProperty const& property, PropertyState* propertyState)
{
    if (!propertyState->customValue)
    {
        return;
    }
    IInspectable local = ReadLocalValue(element, property);
    if (!SameLocalValue(local, propertyState->lastAppliedValue))
    {
        propertyState->originalValue = local;
    }
}

void UnapplyStyleValue(FrameworkElement const& element, DependencyProperty const& property, PropertyState* propertyState)
{
    AdoptExternalValueAsOriginal(element, property, propertyState);
    if (propertyState->originalValue)
    {
        const bool was = g_modifyingProperty;
        g_modifyingProperty = true;
        SetOrClearValue(element, property, PropertyOverrideValue{ *propertyState->originalValue });
        g_modifyingProperty = was;
        propertyState->originalValue.reset();
    }
    propertyState->lastAppliedValue = nullptr;
    propertyState->customValue.reset();
}

// Re-evaluates a dynamic template. The consumer registrations are updated before the XAML step, so a later
// change to any variable it read re-enters here even when this attempt fails.
std::optional<PropertyOverrideValue> ResolveDynamicValue(VariableState* state, ElementId elementId, FrameworkElement const& element,
                                                         DependencyProperty const& property, PCWSTR fallbackClassName, PropertyState* propertyState)
{
    if (!propertyState->dynamicTemplate)
    {
        return std::nullopt;
    }
    const DynamicStyleTemplate& tmpl = *propertyState->dynamicTemplate;

    std::vector<VariableDependency> newDeps;
    styler::VariableLookup lookup = [&](std::wstring_view name) -> std::optional<styler::VariableValue>
    {
        const std::wstring key(name);
        VariableResolution resolution = FindWinningCapture(state, key, element);
        newDeps.push_back({ key, resolution.owner });
        if (!resolution.value)
        {
            return std::nullopt;
        }
        return *resolution.value;
    };

    std::optional<std::wstring> expanded;
    std::wstring error;
    bool skipped = false;
    try
    {
        expanded = styler::ExpandStyleVariables(tmpl.rawValue, lookup, &error);
    }
    catch (styler::SkipRequested const&)
    {
        skipped = true;
    }

    UpdateConsumers(state, elementId, property, fallbackClassName, propertyState->variableDependencies, newDeps);
    propertyState->variableDependencies = std::move(newDeps);

    if (skipped)
    {
        propertyState->lastResolveFailed = false;
        UnapplyStyleValue(element, property, propertyState);
        return std::nullopt;
    }
    if (!expanded)
    {
        SP_LogDebug(L"%s on %s not applied: %s", tmpl.propertyName.c_str(), winrt::get_class_name(element).c_str(), error.c_str());
        propertyState->lastResolveFailed = true;
        return std::nullopt;
    }

    const winrt::hstring typeName = winrt::get_class_name(element);
    auto resolved = ResolveExpandedValue(std::wstring_view(typeName), FallbackTypeFor(fallbackClassName), tmpl.propertyName, *expanded, tmpl.isXamlValue);
    propertyState->lastResolveFailed = !resolved;
    return resolved;
}

bool ChangeAffectsConsumer(PropertyState const& propertyState, std::wstring const& varName, std::optional<ElementId> changedOwner, ElementId winningOwner)
{
    if (propertyState.lastResolveFailed)
    {
        return true;
    }
    for (const auto& dep : propertyState.variableDependencies)
    {
        if (dep.name == varName)
        {
            return changedOwner ? dep.owner == *changedOwner : dep.owner != winningOwner;
        }
    }
    return false;
}

void PropagateVariableChangeCore(VariableState* state, std::wstring const& varName, std::optional<ElementId> changedOwner)
{
    auto consumersIt = state->consumers.find(varName);
    if (consumersIt == state->consumers.end())
    {
        return;
    }

    const auto consumers = consumersIt->second;   // a copy: applying a style can re-enter
    for (const auto& consumer : consumers)
    {
        auto stateIt = g_elementStates.find(consumer.elementId);
        if (stateIt == g_elementStates.end())
        {
            continue;
        }
        ElementState& elementState = stateIt->second;
        auto element = elementState.element.get();
        if (!element)
        {
            continue;
        }

        const ElementId winningOwner = changedOwner ? ElementId::None : PickWinningOwner(state, varName, element);
        PCWSTR fallback = consumer.fallbackClassName.empty() ? nullptr : consumer.fallbackClassName.c_str();

        for (auto& [groupWeak, groupState] : elementState.perVisualStateGroup)
        {
            auto propIt = groupState.properties.find(consumer.property);
            if (propIt == groupState.properties.end() || !propIt->second.dynamicTemplate)
            {
                continue;
            }
            PropertyState& propertyState = propIt->second;
            if (!ChangeAffectsConsumer(propertyState, varName, changedOwner, winningOwner))
            {
                continue;
            }

            auto resolved = ResolveDynamicValue(state, consumer.elementId, element, consumer.property, fallback, &propertyState);
            if (!resolved)
            {
                continue;
            }
            AdoptExternalValueAsOriginal(element, consumer.property, &propertyState);
            if (!propertyState.originalValue)
            {
                propertyState.originalValue = ReadLocalValue(element, consumer.property);
            }
            propertyState.customValue = *resolved;

            const bool was = g_modifyingProperty;
            g_modifyingProperty = true;
            propertyState.lastAppliedValue = SetOrClearValue(element, consumer.property, *resolved);
            g_modifyingProperty = was;
        }
    }
}

// Nested calls queue; the outermost frame drains the queue, which also folds a burst into one pass.
void PropagateVariableChange(VariableState* state, std::wstring const& varName, std::optional<ElementId> changedOwner)
{
    PendingPropagation propagation{ state, varName, changedOwner };
    if (g_propagationDepth > 0)
    {
        if (std::find(g_pendingPropagations.begin(), g_pendingPropagations.end(), propagation) == g_pendingPropagations.end())
        {
            g_pendingPropagations.push_back(std::move(propagation));
        }
        return;
    }

    VariableStatePin pin;
    struct DepthScope
    {
        DepthScope() { ++g_propagationDepth; }
        ~DepthScope() { --g_propagationDepth; }
    } depth;

    PropagateVariableChangeCore(state, varName, changedOwner);

    constexpr int kMaxRounds = 32;
    for (int round = 0; !g_pendingPropagations.empty(); ++round)
    {
        if (round >= kMaxRounds)
        {
            SP_LogError(L"Style variables did not settle after %d rounds; %zu queued update(s) dropped", kMaxRounds, g_pendingPropagations.size());
            g_pendingPropagations.clear();
            break;
        }
        auto pending = std::move(g_pendingPropagations);
        g_pendingPropagations.clear();
        for (const auto& p : pending)
        {
            PropagateVariableChangeCore(p.state, p.varName, p.changedOwner);
        }
    }
}

bool SameNumber(std::optional<double> const& a, std::optional<double> const& b)
{
    if (a.has_value() != b.has_value())
    {
        return false;
    }
    return !a || *a == *b || (std::isnan(*a) && std::isnan(*b));
}

void SetVariableIfChanged(VariableState* state, std::wstring const& varName, ElementId owner, styler::VariableValue value)
{
    auto varIt = state->variables.find(varName);
    if (varIt == state->variables.end())
    {
        return;
    }
    auto& captures = varIt->second;
    auto it = std::find_if(captures.begin(), captures.end(), [owner](const VariableCapture& c) { return c.elementId == owner; });
    if (it == captures.end())
    {
        return;
    }
    if (it->value.text == value.text && SameNumber(it->value.number, value.number) && it->value.substitutable == value.substitutable)
    {
        return;
    }
    SP_LogDebug(L"Style variable '%s': '%s' -> '%s'", varName.c_str(), it->value.text.c_str(), value.text.c_str());
    it->value = std::move(value);
    PropagateVariableChange(state, varName, owner);
}

bool IsLayoutSizeProperty(DependencyProperty const& property)
{
    return property == FrameworkElement::ActualWidthProperty() || property == FrameworkElement::ActualHeightProperty();
}

// ActualWidth/ActualHeight fire no property-changed callback on layout, so those captures listen to SizeChanged.
void SetUpCaptures(VariableState* state, ElementId elementId, FrameworkElement const& element, std::vector<CaptureSpec> const& captures, ElementState* elementState)
{
    if (captures.empty())
    {
        return;
    }

    winrt::weak_ref<FrameworkElement> weakElement = element;
    std::vector<std::wstring> seeded;
    std::vector<std::pair<DependencyProperty, std::wstring>> sizeCaptures;

    for (const auto& capture : captures)
    {
        auto [it, inserted] = elementState->captures.insert({ capture.property, {} });
        if (!inserted)
        {
            SP_LogDebug(L"Property already captured on %s; variable '%s' dropped", winrt::get_class_name(element).c_str(), capture.varName.c_str());
            continue;
        }
        CaptureState& captureState = it->second;
        captureState.varName = capture.varName;

        styler::VariableValue value = ReadCapturedValue(element, capture.property);
        SP_LogDebug(L"Capture '%s' from %s = '%s'", capture.varName.c_str(), winrt::get_class_name(element).c_str(), value.text.c_str());
        state->variables[capture.varName].push_back({ elementId, std::move(value) });
        AddElementRef(state, elementId);
        seeded.push_back(capture.varName);

        if (IsLayoutSizeProperty(capture.property))
        {
            sizeCaptures.push_back({ capture.property, capture.varName });
            continue;
        }

        std::wstring varName = capture.varName;
        captureState.propertyChangedToken = element.RegisterPropertyChangedCallback(capture.property,
            [state, varName, elementId, weakElement](DependencyObject const&, DependencyProperty const& property)
            {
                try
                {
                    auto strong = weakElement.get();
                    if (!strong || !g_initializedForThread)
                    {
                        return;
                    }
                    SetVariableIfChanged(state, varName, elementId, ReadCapturedValue(strong, property));
                }
                catch (...)
                {
                }
            });
    }

    if (!sizeCaptures.empty())
    {
        elementState->sizeChangedToken = element.SizeChanged(
            [state, elementId, weakElement, sizeCaptures](IInspectable const&, SizeChangedEventArgs const&)
            {
                try
                {
                    auto strong = weakElement.get();
                    if (!strong || !g_initializedForThread)
                    {
                        return;
                    }
                    for (const auto& [property, varName] : sizeCaptures)
                    {
                        SetVariableIfChanged(state, varName, elementId, ReadCapturedValue(strong, property));
                    }
                }
                catch (...)
                {
                }
            });
    }

    // A new capture may be closer to existing consumers than the one they read.
    for (const auto& varName : seeded)
    {
        PropagateVariableChange(state, varName, std::nullopt);
    }
}

void RestoreCaptures(FrameworkElement const& element, ElementState const& elementState)
{
    if (!element)
    {
        return;
    }
    for (const auto& [property, captureState] : elementState.captures)
    {
        if (captureState.propertyChangedToken)
        {
            try
            {
                element.UnregisterPropertyChangedCallback(property, captureState.propertyChangedToken);
            }
            catch (...)
            {
            }
        }
    }
    if (elementState.sizeChangedToken)
    {
        try
        {
            element.SizeChanged(elementState.sizeChangedToken);
        }
        catch (...)
        {
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Applying and restoring styles
// ---------------------------------------------------------------------------------------------------------------

void ApplyForVisualStateGroup(VariableState* state, ElementId elementId, FrameworkElement const& element, VisualStateGroup const& group,
                              PCWSTR fallbackClassName, PropertyOverrides overrides, VisualStateGroupState* groupState)
{
    VisualState current = group ? group.CurrentState() : nullptr;
    const std::wstring currentName = current ? std::wstring(current.Name()) : std::wstring();

    for (const auto& [property, valuesPerState] : overrides)
    {
        auto [propIt, inserted] = groupState->properties.insert({ property, {} });
        if (!inserted)
        {
            continue;
        }
        PropertyState& propertyState = propIt->second;

        auto it = valuesPerState.find(currentName);
        if (it == valuesPerState.end() && !currentName.empty())
        {
            it = valuesPerState.find(L"");
        }

        if (it != valuesPerState.end())
        {
            std::optional<PropertyOverrideValue> resolved;
            if (auto* tmpl = std::get_if<DynamicStyleTemplate>(&it->second))
            {
                propertyState.dynamicTemplate = *tmpl;
                resolved = ResolveDynamicValue(state, elementId, element, property, fallbackClassName, &propertyState);
            }
            else
            {
                resolved = it->second;
            }

            if (resolved)
            {
                propertyState.originalValue = ReadLocalValue(element, property);
                propertyState.customValue = *resolved;
                const bool was = g_modifyingProperty;
                g_modifyingProperty = true;
                propertyState.lastAppliedValue = SetOrClearValue(element, property, *resolved, true);
                g_modifyingProperty = was;
            }
        }

        // Something else writing the property (an animation, a system Setter) gets the style pushed back.
        propertyState.propertyChangedToken = element.RegisterPropertyChangedCallback(property,
            [&propertyState](DependencyObject const& sender, DependencyProperty const& changed)
            {
                try
                {
                    if (g_modifyingProperty || !g_initializedForThread)
                    {
                        return;
                    }
                    auto fe = sender.try_as<FrameworkElement>();
                    if (!fe || !propertyState.customValue)
                    {
                        return;
                    }
                    AdoptExternalValueAsOriginal(fe, changed, &propertyState);
                    g_modifyingProperty = true;
                    propertyState.lastAppliedValue = SetOrClearValue(fe, changed, *propertyState.customValue);
                    g_modifyingProperty = false;
                }
                catch (...)
                {
                    g_modifyingProperty = false;
                }
            });
    }

    if (!group)
    {
        return;
    }

    winrt::weak_ref<FrameworkElement> weakElement = element;
    const std::wstring fallback = fallbackClassName ? fallbackClassName : L"";
    groupState->stateChangedToken = group.CurrentStateChanged(
        [state, weakElement, overrides, elementId, fallback, groupState](IInspectable const&, VisualStateChangedEventArgs const& e)
        {
            auto element = weakElement.get();
            if (!element || !g_initializedForThread)
            {
                return;
            }

            const bool was = g_modifyingProperty;
            g_modifyingProperty = true;
            try
            {
                PCWSTR fallbackPtr = fallback.empty() ? nullptr : fallback.c_str();
                auto newState = e.NewState();
                const std::wstring newName = newState ? std::wstring(newState.Name()) : std::wstring();
                auto oldState = e.OldState();
                const std::wstring oldName = oldState ? std::wstring(oldState.Name()) : std::wstring();

                for (const auto& [property, valuesPerState] : overrides)
                {
                    auto propIt = groupState->properties.find(property);
                    if (propIt == groupState->properties.end())
                    {
                        continue;
                    }
                    PropertyState& propertyState = propIt->second;

                    auto it = valuesPerState.find(newName);
                    if (it == valuesPerState.end())
                    {
                        it = valuesPerState.find(L"");
                        // The default value is only re-pushed when a state-specific one is being left.
                        if (it != valuesPerState.end() && valuesPerState.find(oldName) == valuesPerState.end())
                        {
                            continue;
                        }
                    }

                    if (it != valuesPerState.end())
                    {
                        std::optional<PropertyOverrideValue> resolved;
                        if (auto* tmpl = std::get_if<DynamicStyleTemplate>(&it->second))
                        {
                            propertyState.dynamicTemplate = *tmpl;
                            resolved = ResolveDynamicValue(state, elementId, element, property, fallbackPtr, &propertyState);
                        }
                        else
                        {
                            if (propertyState.dynamicTemplate)
                            {
                                UpdateConsumers(state, elementId, property, nullptr, propertyState.variableDependencies, {});
                                propertyState.variableDependencies.clear();
                                propertyState.dynamicTemplate.reset();
                            }
                            resolved = it->second;
                        }

                        if (resolved)
                        {
                            AdoptExternalValueAsOriginal(element, property, &propertyState);
                            if (!propertyState.originalValue)
                            {
                                propertyState.originalValue = ReadLocalValue(element, property);
                            }
                            propertyState.customValue = *resolved;
                            propertyState.lastAppliedValue = SetOrClearValue(element, property, *resolved);
                        }
                    }
                    else
                    {
                        if (propertyState.dynamicTemplate)
                        {
                            UpdateConsumers(state, elementId, property, nullptr, propertyState.variableDependencies, {});
                            propertyState.variableDependencies.clear();
                            propertyState.dynamicTemplate.reset();
                        }
                        UnapplyStyleValue(element, property, &propertyState);
                    }
                }
            }
            catch (winrt::hresult_error const& e2)
            {
                SP_LogDebug(L"Visual state change not applied: 0x%08X", (unsigned)e2.code());
            }
            catch (...)
            {
            }
            g_modifyingProperty = was;
        });
}

void RestoreForVisualStateGroup(VariableState* state, ElementId elementId, FrameworkElement const& element,
                                std::optional<winrt::weak_ref<VisualStateGroup>> const& groupWeak, VisualStateGroupState const& groupState)
{
    for (const auto& [property, propertyState] : groupState.properties)
    {
        if (element)
        {
            try
            {
                element.UnregisterPropertyChangedCallback(property, propertyState.propertyChangedToken);
            }
            catch (...)
            {
            }
        }
        if (!propertyState.variableDependencies.empty())
        {
            UpdateConsumers(state, elementId, property, nullptr, propertyState.variableDependencies, {});
        }
        if (element && propertyState.originalValue)
        {
            const bool was = g_modifyingProperty;
            g_modifyingProperty = true;
            SetOrClearValue(element, property, PropertyOverrideValue{ *propertyState.originalValue });
            g_modifyingProperty = was;
        }
    }

    auto group = groupWeak ? groupWeak->get() : nullptr;
    if (group && groupState.stateChangedToken)
    {
        try
        {
            group.CurrentStateChanged(groupState.stateChangedToken);
        }
        catch (...)
        {
        }
    }
}

void ApplyCustomizations(ElementId elementId, FrameworkElement const& element, PCWSTR fallbackClassName)
{
    VariableStatePin pin;

    VariableState* state = GetVariableState(element);
    if (!state)
    {
        return;   // not in a tree yet
    }

    ElementRules rules = FindElementRules(element, fallbackClassName);
    if (rules.overridesPerGroup.empty() && rules.captures.empty())
    {
        return;
    }

    if (SP_LogEnabled(SP_LOG_DEBUG))
    {
        SP_LogDebug(L"Styling %s#%s (%zu group(s), %zu capture(s))", winrt::get_class_name(element).c_str(),
                    element.Name().c_str(), rules.overridesPerGroup.size(), rules.captures.size());
    }

    ElementState& elementState = g_elementStates[elementId];
    for (const auto& [groupWeak, groupState] : elementState.perVisualStateGroup)
    {
        RestoreForVisualStateGroup(state, elementId, element, groupWeak, groupState);
    }
    elementState.element = element;
    elementState.xamlRoot = state->xamlRoot;
    elementState.perVisualStateGroup.clear();

    SetUpCaptures(state, elementId, element, rules.captures, &elementState);

    for (auto& [group, overrides] : rules.overridesPerGroup)
    {
        std::optional<winrt::weak_ref<VisualStateGroup>> groupWeak;
        if (group)
        {
            groupWeak = group;
        }
        elementState.perVisualStateGroup.push_back({ groupWeak, {} });
        VisualStateGroupState* groupState = &elementState.perVisualStateGroup.back().second;
        ApplyForVisualStateGroup(state, elementId, element, group, fallbackClassName, std::move(overrides), groupState);
    }
}

void CleanupCustomizations(ElementId elementId)
{
    if (elementId == ElementId::None)
    {
        return;
    }

    RemoveConditionWatches(elementId);

    auto it = g_elementStates.find(elementId);
    if (it == g_elementStates.end())
    {
        return;
    }
    ElementState& elementState = it->second;   // a reference: a rehash keeps it valid, an iterator would not be

    VariableStatePin pin;
    auto element = elementState.element.get();
    VariableState* state = GetVariableState(elementState.xamlRoot);

    RestoreCaptures(element, elementState);

    std::vector<std::wstring> removedVars;
    if (state)
    {
        for (const auto& [property, captureState] : elementState.captures)
        {
            if (captureState.varName.empty())
            {
                continue;
            }
            auto varIt = state->variables.find(captureState.varName);
            if (varIt == state->variables.end())
            {
                continue;
            }
            auto& captures = varIt->second;
            const size_t before = captures.size();
            captures.erase(std::remove_if(captures.begin(), captures.end(),
                [elementId](const VariableCapture& c) { return c.elementId == elementId; }), captures.end());
            const size_t removed = before - captures.size();
            if (!removed)
            {
                continue;
            }
            ReleaseElementRefs(state, elementId, removed);
            removedVars.push_back(captureState.varName);
            if (captures.empty())
            {
                state->variables.erase(varIt);
            }
        }
    }

    for (const auto& [groupWeak, groupState] : elementState.perVisualStateGroup)
    {
        RestoreForVisualStateGroup(state, elementId, element, groupWeak, groupState);
    }

    g_elementStates.erase(elementId);

    for (const auto& varName : removedVars)
    {
        PropagateVariableChange(state, varName, std::nullopt);
    }
}

// Every element of a subtree is torn down and matched again: a rule can reach a descendant through a condition
// on an ancestor.
void ReapplyCustomizationsForSubtree(FrameworkElement const& element)
{
    try
    {
        if (ElementId elementId = ElementIdFromElement(element); elementId != ElementId::None)
        {
            CleanupCustomizations(elementId);
            const winrt::hstring className = winrt::get_class_name(element);
            ApplyCustomizations(elementId, element, className.c_str());
        }
    }
    catch (winrt::hresult_error const& e)
    {
        SP_LogDebug(L"Re-match failed: 0x%08X", (unsigned)e.code());
    }

    std::vector<FrameworkElement> children;
    try
    {
        const int count = Media::VisualTreeHelper::GetChildrenCount(element);
        for (int i = 0; i < count; ++i)
        {
            if (auto child = Media::VisualTreeHelper::GetChild(element, i).try_as<FrameworkElement>())
            {
                children.push_back(std::move(child));
            }
        }
    }
    catch (...)
    {
        return;
    }
    for (const auto& child : children)
    {
        ReapplyCustomizationsForSubtree(child);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Handing diagnostics references back
//
// The diagnostics hold every element they report until its removal is reported, and a tree discarded whole (Task
// View, say) never reports removals, so it could never be freed. Elements the mod keys nothing by are released
// once a burst of reports has ended; the drain runs from a dispatcher timer, outside the tree walk the reports
// come from.
// ---------------------------------------------------------------------------------------------------------------

// True on the advise thread while it is inside AdviseVisualTreeChange; read by the two registry hooks.
thread_local bool g_reportCompositionDiagDisabled;

thread_local std::vector<InstanceHandle> g_pendingReleases;
thread_local ULONGLONG g_lastReleaseQueueTick;
thread_local bool g_releaseDrainQueued;
thread_local winrt::Windows::System::DispatcherQueueTimer g_releaseTimer{ nullptr };
thread_local winrt::event_token g_releaseTimerToken;

constexpr ULONGLONG kReleaseQuietMs = 200;

bool ReleaseDiagnosticsReference(InstanceHandle handle);

bool ElementHasState(ElementId elementId)
{
    if (elementId == ElementId::None)
    {
        return false;
    }
    if (g_elementStates.count(elementId) || g_conditionWatches.count(elementId))
    {
        return true;
    }
    for (const auto& state : g_variableStates)
    {
        if (state.elementRefs.count(elementId))
        {
            return true;
        }
    }
    for (const auto& p : g_pendingPropagations)
    {
        if (p.changedOwner == elementId)
        {
            return true;
        }
    }
    for (ElementId pending : g_pendingRematch)
    {
        if (pending == elementId)
        {
            return true;
        }
    }
    return false;
}

void FlushDiagnosticsReleases()
{
    auto pending = std::move(g_pendingReleases);
    g_pendingReleases.clear();

    std::sort(pending.begin(), pending.end());
    pending.erase(std::unique(pending.begin(), pending.end()), pending.end());

    for (InstanceHandle handle : pending)
    {
        if (ElementHasState(FindElementId(handle)))
        {
            continue;
        }
        if (ReleaseDiagnosticsReference(handle))
        {
            ForgetElementId(handle);
        }
    }

    ReapDeadElementIdsIfNeeded();
}

void QueueDiagnosticsRelease(InstanceHandle handle)
{
    if (handle)
    {
        g_pendingReleases.push_back(handle);
        g_lastReleaseQueueTick = GetTickCount64();
    }
}

void FlushDiagnosticsReleasesIfQuiet()
{
    if (g_pendingReleases.empty() || g_releaseDrainQueued || GetTickCount64() - g_lastReleaseQueueTick < kReleaseQuietMs)
    {
        return;
    }
    try
    {
        if (!g_releaseTimer)
        {
            auto queue = winrt::Windows::System::DispatcherQueue::GetForCurrentThread();
            if (!queue)
            {
                return;   // nowhere safe to release from; the elements stay held
            }
            g_releaseTimer = queue.CreateTimer();
            g_releaseTimer.IsRepeating(false);
            g_releaseTimer.Interval(std::chrono::milliseconds(1));
            g_releaseTimerToken = g_releaseTimer.Tick([](auto&&, auto&&)
            {
                try
                {
                    g_releaseDrainQueued = false;
                    if (g_initializedForThread)
                    {
                        FlushDiagnosticsReleases();
                    }
                }
                catch (...)
                {
                }
            });
        }
        g_releaseTimer.Start();
        g_releaseDrainQueued = true;
    }
    catch (winrt::hresult_error const& e)
    {
        SP_LogDebug(L"Release timer not started: 0x%08X", (unsigned)e.code());
    }
}

void StopDiagnosticsReleases()
{
    g_pendingReleases.clear();
    if (g_releaseTimer)
    {
        try
        {
            g_releaseTimer.Stop();
            g_releaseTimer.Tick(g_releaseTimerToken);
        }
        catch (...)
        {
        }
    }
    g_releaseTimerToken = {};
    g_releaseTimer = nullptr;
    g_releaseDrainQueued = false;
}

// ---------------------------------------------------------------------------------------------------------------
// The visual tree watcher: what the diagnostics call for every element entering or leaving a tree
// ---------------------------------------------------------------------------------------------------------------

struct VisualTreeWatcher : winrt::implements<VisualTreeWatcher, IVisualTreeServiceCallback2, winrt::non_agile>
{
    explicit VisualTreeWatcher(winrt::com_ptr<::IUnknown> const& site) : m_diagnostics(site.as<IXamlDiagnostics>())
    {
        if (FAILED(m_diagnostics->QueryInterface(__uuidof(IXamlDiagnosticsTestHooks), m_testHooks.put_void())))
        {
            SP_Log(L"IXamlDiagnosticsTestHooks is not available; reported elements will be kept alive by the diagnostics");
        }
    }

    // Calling AdviseVisualTreeChange on the calling thread was seen to hang inside the diagnostics; a thread of
    // its own does not.
    void StartAdvising()
    {
        AddRef();
        HANDLE thread = CreateThread(nullptr, 0, [](LPVOID param) -> DWORD
        {
            auto* self = static_cast<VisualTreeWatcher*>(param);
            try
            {
                auto service = self->m_diagnostics.as<IVisualTreeService3>();
                g_reportCompositionDiagDisabled = true;
                const HRESULT hr = service->AdviseVisualTreeChange(self);
                g_reportCompositionDiagDisabled = false;
                if (FAILED(hr))
                {
                    SP_LogError(L"AdviseVisualTreeChange failed: 0x%08X (a shell restart may be needed)", (unsigned)hr);
                }
                else
                {
                    SP_Log(L"Watching the XAML visual trees");
                }
            }
            catch (...)
            {
                g_reportCompositionDiagDisabled = false;
                SP_LogError(L"The diagnostics service could not be reached");
            }
            self->Release();
            return 0;
        }, this, 0, nullptr);
        if (thread)
        {
            CloseHandle(thread);
        }
        else
        {
            Release();
        }
    }

    void Unadvise()
    {
        m_stopped.store(true, std::memory_order_release);
        try
        {
            const HRESULT hr = m_diagnostics.as<IVisualTreeService3>()->UnadviseVisualTreeChange(this);
            if (FAILED(hr))
            {
                SP_LogDebug(L"UnadviseVisualTreeChange: 0x%08X", (unsigned)hr);
            }
        }
        catch (...)
        {
        }
    }

    // Whether dropping the reference destroyed the element (so its handle may name something else from now on).
    bool ReleaseReference(InstanceHandle handle)
    {
        if (!m_testHooks)
        {
            return false;
        }
        winrt::weak_ref<IInspectable> weak;
        {
            IInspectable element;
            if (SUCCEEDED(m_diagnostics->GetIInspectableFromHandle(handle, reinterpret_cast<::IInspectable**>(winrt::put_abi(element)))) && element)
            {
                try
                {
                    weak = TryMakeWeak(element);
                }
                catch (...)
                {
                }
            }
        }
        if (FAILED(m_testHooks->UnregisterInstance(handle)))
        {
            return false;
        }
        return weak && !weak.get();
    }

    HRESULT STDMETHODCALLTYPE OnVisualTreeChange(ParentChildRelation relation, VisualElement element, VisualMutationType mutationType) noexcept override
    {
        if (m_stopped.load(std::memory_order_acquire) || !g_initializedForThread)
        {
            return S_OK;
        }

        try
        {
            try
            {
                if (mutationType == Add)
                {
                    IInspectable inspectable;
                    winrt::check_hresult(m_diagnostics->GetIInspectableFromHandle(element.Handle, reinterpret_cast<::IInspectable**>(winrt::put_abi(inspectable))));
                    const ElementId elementId = GetOrCreateElementId(element.Handle, inspectable);
                    if (auto fe = inspectable.try_as<FrameworkElement>(); fe && elementId != ElementId::None)
                    {
                        ApplyCustomizations(elementId, fe, element.Type);
                    }
                }
                else if (mutationType == Remove)
                {
                    CleanupCustomizations(FindElementId(element.Handle));
                }
            }
            catch (winrt::hresult_error const& e)
            {
                SP_LogDebug(L"Element %s not handled: 0x%08X %s", element.Type ? element.Type : L"?", (unsigned)e.code(), HResultMessage(e).c_str());
            }
            catch (...)
            {
            }

            FlushDiagnosticsReleasesIfQuiet();

            if (mutationType == Add)
            {
                QueueDiagnosticsRelease(element.Handle);
                QueueDiagnosticsRelease(relation.Parent);
            }
            else if (mutationType == Remove)
            {
                // Queued: this arrives from inside the Leave walk still visiting the subtree.
                QueueDiagnosticsRelease(element.Handle);
                ForgetElementId(element.Handle);
            }
        }
        catch (...)
        {
        }
        return S_OK;   // an error would stop further reports
    }

    HRESULT STDMETHODCALLTYPE OnElementStateChanged(InstanceHandle, VisualElementState, LPCWSTR) noexcept override
    {
        return S_OK;
    }

    winrt::com_ptr<IXamlDiagnostics> m_diagnostics;
    winrt::com_ptr<IXamlDiagnosticsTestHooks> m_testHooks;
    std::atomic<bool> m_stopped{ false };
};

std::mutex g_watcherMutex;
winrt::com_ptr<VisualTreeWatcher> g_watcher;

bool ReleaseDiagnosticsReference(InstanceHandle handle)
{
    winrt::com_ptr<VisualTreeWatcher> watcher;
    {
        std::lock_guard<std::mutex> lock(g_watcherMutex);
        watcher = g_watcher;
    }
    return watcher && watcher->ReleaseReference(handle);
}

// ---------------------------------------------------------------------------------------------------------------
// The TAP object and its class factory: what InitializeXamlDiagnosticsEx creates from this DLL
// ---------------------------------------------------------------------------------------------------------------

struct StylerTap : winrt::implements<StylerTap, IObjectWithSite, winrt::non_agile>
{
    HRESULT STDMETHODCALLTYPE SetSite(::IUnknown* site) noexcept override
    {
        try
        {
            winrt::com_ptr<VisualTreeWatcher> previous;
            {
                std::lock_guard<std::mutex> lock(g_watcherMutex);
                previous = std::move(g_watcher);
                g_watcher = nullptr;
            }
            if (previous)
            {
                previous->Unadvise();
            }

            m_site.copy_from(site);
            if (m_site)
            {
                // The diagnostics loaded this DLL by path to make this object; that reference is handed back
                // here so the module's reference count stays what the shell gave it.
                HMODULE self = nullptr;
                if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       reinterpret_cast<LPCWSTR>(&LoadSettings), &self) && self)
                {
                    FreeLibrary(self);
                }

                auto watcher = winrt::make_self<VisualTreeWatcher>(m_site);
                {
                    std::lock_guard<std::mutex> lock(g_watcherMutex);
                    g_watcher = watcher;
                }
                watcher->StartAdvising();
            }
            return S_OK;
        }
        catch (...)
        {
            return winrt::to_hresult();
        }
    }

    HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void** result) noexcept override
    {
        if (!result)
        {
            return E_POINTER;
        }
        *result = nullptr;
        return m_site ? m_site->QueryInterface(riid, result) : E_FAIL;
    }

private:
    winrt::com_ptr<::IUnknown> m_site;
};

struct StylerTapFactory : winrt::implements<StylerTapFactory, IClassFactory, winrt::non_agile>
{
    HRESULT STDMETHODCALLTYPE CreateInstance(::IUnknown* outer, REFIID riid, void** result) noexcept override
    {
        if (!result)
        {
            return E_POINTER;
        }
        *result = nullptr;
        if (outer)
        {
            return CLASS_E_NOAGGREGATION;
        }
        try
        {
            return winrt::make_self<StylerTap>()->QueryInterface(riid, result);
        }
        catch (...)
        {
            return winrt::to_hresult();
        }
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL) noexcept override
    {
        return S_OK;
    }
};

// ---------------------------------------------------------------------------------------------------------------
// Connecting the diagnostics
// ---------------------------------------------------------------------------------------------------------------

using InitializeXamlDiagnosticsEx_t = HRESULT(WINAPI*)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, CLSID, LPCWSTR);

std::atomic<bool> g_tapConnected{ false };

HRESULT ConnectTap()
{
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ConnectTap), &self) || !self)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    wchar_t location[MAX_PATH];
    const DWORD length = GetModuleFileNameW(self, location, ARRAYSIZE(location));
    if (length == 0 || length == ARRAYSIZE(location))
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    HMODULE wux = LoadLibraryExW(L"Windows.UI.Xaml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!wux)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    auto ixde = reinterpret_cast<InitializeXamlDiagnosticsEx_t>(GetProcAddress(wux, "InitializeXamlDiagnosticsEx"));
    if (!ixde)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    // A connection name that is in use answers ERROR_NOT_FOUND; the next one is tried.
    HRESULT hr = E_FAIL;
    for (int i = 1; i <= 10000; ++i)
    {
        wchar_t name[64];
        swprintf_s(name, L"VisualDiagConnection%d", i);
        hr = ixde(name, GetCurrentProcessId(), L"", location, CLSID_StylerTap, nullptr);
        if (hr != HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
        {
            break;
        }
    }
    return hr;
}

void EnsureTapConnected()
{
    if (g_tapConnected.exchange(true))
    {
        return;
    }
    const HRESULT hr = ConnectTap();
    if (FAILED(hr))
    {
        SP_LogError(L"The XAML diagnostics connection failed: 0x%08X", (unsigned)hr);
        g_tapConnected.store(false);
    }
    else
    {
        SP_Log(L"XAML diagnostics connected");
    }
}

void DisconnectTap()
{
    winrt::com_ptr<VisualTreeWatcher> watcher;
    {
        std::lock_guard<std::mutex> lock(g_watcherMutex);
        watcher = std::move(g_watcher);
        g_watcher = nullptr;
    }
    if (watcher)
    {
        watcher->Unadvise();
    }
    g_tapConnected.store(false);
}

// ---------------------------------------------------------------------------------------------------------------
// The taskbar window's own surface
//
// The taskbar window is a plain Win32 window with the XAML island as a child. When the island is made
// transparent by a theme, whatever the window's surface last held shows through, so it is painted black on every
// paint (black is what the DWM treats as clear here).
// ---------------------------------------------------------------------------------------------------------------

thread_local std::unordered_set<HWND> g_taskbarSurfaceWindows;
constexpr UINT_PTR kSurfaceSubclassId = 1;

bool IsTaskbarTopLevelWindow(HWND hWnd)
{
    wchar_t className[32];
    if (!GetClassNameW(hWnd, className, ARRAYSIZE(className)))
    {
        return false;
    }
    return _wcsicmp(className, L"Shell_TrayWnd") == 0 || _wcsicmp(className, L"Shell_SecondaryTrayWnd") == 0;
}

void ResetTaskbarWindowSurface(HWND hWnd)
{
    RECT rect;
    if (!GetWindowRect(hWnd, &rect))
    {
        return;
    }
    HDC hdc = GetDCEx(hWnd, nullptr, DCX_WINDOW | DCX_CACHE);
    if (!hdc)
    {
        return;
    }
    RECT fill = { 0, 0, rect.right - rect.left, rect.bottom - rect.top };
    FillRect(hdc, &fill, (HBRUSH)GetStockObject(BLACK_BRUSH));
    ReleaseDC(hWnd, hdc);
}

LRESULT CALLBACK TaskbarSurfaceSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR)
{
    switch (uMsg)
    {
    case WM_PAINT:
        ResetTaskbarWindowSurface(hWnd);
        break;
    case WM_NCDESTROY:
        g_taskbarSurfaceWindows.erase(hWnd);
        break;
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

void EnsureTaskbarSurfaceSubclass(HWND hWnd)
{
    if (g_taskbarSurfaceWindows.count(hWnd))
    {
        return;
    }
    if (!SP_SetWindowSubclassFromAnyThread(hWnd, TaskbarSurfaceSubclassProc, kSurfaceSubclassId, 0))
    {
        return;
    }
    g_taskbarSurfaceWindows.insert(hWnd);
    ResetTaskbarWindowSurface(hWnd);
}

void EnsureTaskbarSurfaceSubclasses()
{
    EnumThreadWindows(GetCurrentThreadId(), [](HWND hWnd, LPARAM) -> BOOL
    {
        if (IsTaskbarTopLevelWindow(hWnd))
        {
            EnsureTaskbarSurfaceSubclass(hWnd);
        }
        return TRUE;
    }, 0);
}

void RemoveTaskbarSurfaceSubclasses()
{
    for (HWND hWnd : g_taskbarSurfaceWindows)
    {
        SP_RemoveWindowSubclassFromAnyThread(hWnd, TaskbarSurfaceSubclassProc, kSurfaceSubclassId);
    }
    g_taskbarSurfaceWindows.clear();
}

// ---------------------------------------------------------------------------------------------------------------
// Per-thread setup: the rules, and their teardown
// ---------------------------------------------------------------------------------------------------------------

std::mutex g_threadsMutex;
std::vector<DWORD> g_initializedThreads;

void AddRule(std::wstring_view target, std::vector<std::wstring> const& styles)
{
    for (std::wstring_view single : styler::SplitTargetString(target))
    {
        try
        {
            styler::TargetRule parsed = styler::ParseSingleTarget(single, styles);
            CustomizationRule rule;
            rule.targetText = std::wstring(styler::Trim(single));
            rule.matcher.spec = std::move(parsed.matcher);
            for (auto& parent : parsed.parents)
            {
                Matcher m;
                m.spec = std::move(parent);
                rule.parents.push_back(std::move(m));
            }
            rule.unresolved = std::move(parsed.rules);
            g_rules.push_back(std::move(rule));
        }
        catch (std::exception const& e)
        {
            SP_LogError(L"Rule '%.*s' skipped: %S", (int)single.size(), single.data(), e.what());
        }
    }
}

void LoadRulesForCurrentThread()
{
    g_rules.clear();

    const int themeIndex = g_themeIndex.load(std::memory_order_relaxed);
    const stylerthemes::Theme* theme = ThemeForIndex(themeIndex);

    styler::StyleConstants constants;
    styler::AddStyleConstant(constants, styler::StyleConstant{ L"TaskbarHeight",
                                                               std::to_wstring(g_taskbarHeight.load()) });
    if (theme)
    {
        for (PCWSTR constant : theme->styleConstants)
        {
            if (auto parsed = styler::ParseStyleConstant(constant, constants))
            {
                styler::AddStyleConstant(constants, std::move(*parsed));
            }
        }
        for (const auto& targetStyles : theme->targetStyles)
        {
            std::vector<std::wstring> styles;
            styles.reserve(targetStyles.styles.size());
            for (PCWSTR style : targetStyles.styles)
            {
                styles.push_back(styler::ApplyStyleConstants(style, constants));
            }
            AddRule(targetStyles.target, styles);
        }
    }

    SP_Log(L"Thread %u: theme %s, %zu rule(s), taskbar height %d", GetCurrentThreadId(), ThemeNameForIndex(themeIndex),
           g_rules.size(), g_taskbarHeight.load());
}

void InitializeForCurrentThread()
{
    if (g_initializedForThread)
    {
        return;
    }
    try
    {
        EnsureTaskbarSurfaceSubclasses();
        LoadRulesForCurrentThread();
    }
    catch (winrt::hresult_error const& e)
    {
        SP_LogError(L"Thread setup failed: 0x%08X", (unsigned)e.code());
    }
    catch (...)
    {
    }
    g_initializedForThread = true;

    std::lock_guard<std::mutex> lock(g_threadsMutex);
    const DWORD threadId = GetCurrentThreadId();
    if (std::find(g_initializedThreads.begin(), g_initializedThreads.end(), threadId) == g_initializedThreads.end())
    {
        g_initializedThreads.push_back(threadId);
    }
}

void UninitializeForCurrentThread()
{
    if (!g_initializedForThread)
    {
        return;
    }
    g_initializedForThread = false;

    try
    {
        RemoveTaskbarSurfaceSubclasses();

        for (auto& [object, op] : g_delayedBackgroundFill)
        {
            try { op.Cancel(); } catch (...) {}
        }
        g_delayedBackgroundFill.clear();

        StopDiagnosticsReleases();
        g_pendingRematch.clear();

        // Detached before being walked: restoring a value runs XAML work that may look elements up again, and
        // an empty global makes those lookups miss, which is what teardown wants.
        auto watches = std::move(g_conditionWatches);
        g_conditionWatches.clear();
        for (auto& [elementId, entry] : watches)
        {
            if (auto element = entry.element.get())
            {
                for (const auto& watch : entry.watches)
                {
                    try { element.UnregisterPropertyChangedCallback(watch.property, watch.token); } catch (...) {}
                }
            }
        }

        auto states = std::move(g_elementStates);
        g_elementStates.clear();
        size_t restored = 0;
        for (const auto& [elementId, elementState] : states)
        {
            auto element = elementState.element.get();
            VariableState* state = GetVariableState(elementState.xamlRoot);
            RestoreCaptures(element, elementState);
            for (const auto& [groupWeak, groupState] : elementState.perVisualStateGroup)
            {
                RestoreForVisualStateGroup(state, elementId, element, groupWeak, groupState);
            }
            if (element)
            {
                ++restored;
            }
        }
        states.clear();

        g_pendingPropagations.clear();
        g_variableStates.clear();
        g_elementIds.clear();
        g_elementIdsReapThreshold = 64;
        g_rules.clear();

        SP_Log(L"Thread %u: %zu element(s) restored", GetCurrentThreadId(), restored);
    }
    catch (winrt::hresult_error const& e)
    {
        SP_LogError(L"Thread teardown hit 0x%08X", (unsigned)e.code());
    }
    catch (...)
    {
    }

    std::lock_guard<std::mutex> lock(g_threadsMutex);
    g_initializedThreads.erase(std::remove(g_initializedThreads.begin(), g_initializedThreads.end(), GetCurrentThreadId()), g_initializedThreads.end());
}

// ---------------------------------------------------------------------------------------------------------------
// Running on a window's thread
//
// XAML state belongs to its thread. A message hook on that thread plus a SendMessage of a private message runs
// the work there and waits for it.
// ---------------------------------------------------------------------------------------------------------------

using ThreadProc_t = void (*)(void*);

struct RunOnThreadParam
{
    ThreadProc_t proc;
    void* context;
};

UINT RunOnThreadMessage()
{
    static const UINT message = RegisterWindowMessageW(L"ShadePatcher_RunFromWindowThread_taskbar-styler");
    return message;
}

LRESULT CALLBACK RunOnThreadHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION)
    {
        const CWPSTRUCT* cwp = reinterpret_cast<const CWPSTRUCT*>(lParam);
        if (cwp->message == RunOnThreadMessage())
        {
            auto* param = reinterpret_cast<RunOnThreadParam*>(cwp->lParam);
            try
            {
                param->proc(param->context);
            }
            catch (...)
            {
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

bool RunOnWindowThread(HWND hWnd, ThreadProc_t proc, void* context)
{
    const DWORD threadId = GetWindowThreadProcessId(hWnd, nullptr);
    if (!threadId)
    {
        return false;
    }
    if (threadId == GetCurrentThreadId())
    {
        proc(context);
        return true;
    }

    HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, RunOnThreadHookProc, nullptr, threadId);
    if (!hook)
    {
        return false;
    }
    RunOnThreadParam param = { proc, context };
    SendMessageW(hWnd, RunOnThreadMessage(), 0, reinterpret_cast<LPARAM>(&param));
    UnhookWindowsHookEx(hook);
    return true;
}

// Any window that can carry a message to the thread; a thread with no windows cannot be reached, but then it
// has no XAML content either.
HWND AnyWindowOfThread(DWORD threadId)
{
    HWND found = nullptr;
    EnumThreadWindows(threadId, [](HWND hWnd, LPARAM lParam) -> BOOL
    {
        *reinterpret_cast<HWND*>(lParam) = hWnd;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&found));
    return found;
}

// The shell's XAML host windows: the taskbar's island, Task View / Alt-Tab hosts and the input switcher.
std::vector<HWND> FindXamlHostWindows()
{
    std::vector<HWND> windows;
    EnumWindows([](HWND hWnd, LPARAM lParam) -> BOOL
    {
        DWORD processId = 0;
        if (!GetWindowThreadProcessId(hWnd, &processId) || processId != GetCurrentProcessId())
        {
            return TRUE;
        }
        wchar_t className[64];
        if (!GetClassNameW(hWnd, className, ARRAYSIZE(className)))
        {
            return TRUE;
        }
        auto* list = reinterpret_cast<std::vector<HWND>*>(lParam);
        if (_wcsicmp(className, L"XamlExplorerHostIslandWindow") == 0 || _wcsicmp(className, L"Shell_InputSwitchTopLevelWindow") == 0)
        {
            list->push_back(hWnd);
        }
        else if (_wcsicmp(className, L"Shell_TrayWnd") == 0)
        {
            if (HWND island = FindWindowExW(hWnd, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr))
            {
                list->push_back(island);
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&windows));
    return windows;
}

// The windows to send thread work to: one per initialized thread, plus the host windows found now.
std::vector<HWND> ThreadTargets()
{
    std::vector<HWND> targets = FindXamlHostWindows();
    std::vector<DWORD> threads;
    {
        std::lock_guard<std::mutex> lock(g_threadsMutex);
        threads = g_initializedThreads;
    }
    for (DWORD threadId : threads)
    {
        bool covered = false;
        for (HWND hWnd : targets)
        {
            if (GetWindowThreadProcessId(hWnd, nullptr) == threadId)
            {
                covered = true;
                break;
            }
        }
        if (!covered)
        {
            if (HWND hWnd = AnyWindowOfThread(threadId))
            {
                targets.push_back(hWnd);
            }
        }
    }
    return targets;
}

void InitializeThreadProc(void*)
{
    InitializeForCurrentThread();
}

void UninitializeThreadProc(void*)
{
    UninitializeForCurrentThread();
}

void ReinitializeThreadProc(void*)
{
    UninitializeForCurrentThread();
    InitializeForCurrentThread();
}

// Initializes every XAML thread that exists now, then connects the diagnostics. Safe to call more than once.
void SweepExistingThreads()
{
    if (!g_active.load(std::memory_order_acquire))
    {
        return;
    }
    bool any = false;
    for (HWND hWnd : FindXamlHostWindows())
    {
        if (RunOnWindowThread(hWnd, InitializeThreadProc, nullptr))
        {
            any = true;
        }
    }
    if (any)
    {
        EnsureTapConnected();
    }
    else
    {
        SP_LogDebug(L"No XAML host window yet; waiting for the taskbar");
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------------------------------------------

void OnWindowCreated(HWND hWnd, HWND hWndParent, LPCWSTR lpClassName)
{
    if (!g_active.load(std::memory_order_acquire))
    {
        return;
    }

    const bool textualClassName = (reinterpret_cast<ULONG_PTR>(lpClassName) & ~static_cast<ULONG_PTR>(0xFFFF)) != 0;

    try
    {
        if (g_initializedForThread && IsTaskbarTopLevelWindow(hWnd))
        {
            EnsureTaskbarSurfaceSubclass(hWnd);
        }

        wchar_t className[64];
        if (hWndParent && GetClassNameW(hWnd, className, ARRAYSIZE(className)) &&
            _wcsicmp(className, L"Windows.UI.Composition.DesktopWindowContentBridge") == 0 &&
            GetClassNameW(hWndParent, className, ARRAYSIZE(className)) &&
            _wcsicmp(className, L"Shell_TrayWnd") == 0)
        {
            SP_Log(L"The taskbar's XAML island was created");
            InitializeForCurrentThread();
            EnsureTapConnected();
            return;
        }

        if (textualClassName &&
            (_wcsicmp(lpClassName, L"XamlExplorerHostIslandWindow") == 0 || _wcsicmp(lpClassName, L"Shell_InputSwitchTopLevelWindow") == 0))
        {
            SP_LogDebug(L"A XAML host window was created: %s", lpClassName);
            InitializeForCurrentThread();
            EnsureTapConnected();
        }
    }
    catch (...)
    {
    }
}

using CreateWindowExW_t = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
CreateWindowExW_t g_origCreateWindowExW = nullptr;

HWND WINAPI CreateWindowExW_Hook(DWORD dwExStyle, LPCWSTR lpClassName, LPCWSTR lpWindowName, DWORD dwStyle, int X, int Y,
                                 int nWidth, int nHeight, HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam)
{
    HWND hWnd = g_origCreateWindowExW(dwExStyle, lpClassName, lpWindowName, dwStyle, X, Y, nWidth, nHeight, hWndParent, hMenu, hInstance, lpParam);
    if (hWnd)
    {
        OnWindowCreated(hWnd, hWndParent, lpClassName);
    }
    return hWnd;
}

using CreateWindowInBand_t = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID, DWORD);
CreateWindowInBand_t g_origCreateWindowInBand = nullptr;

HWND WINAPI CreateWindowInBand_Hook(DWORD dwExStyle, LPCWSTR lpClassName, LPCWSTR lpWindowName, DWORD dwStyle, int X, int Y,
                                    int nWidth, int nHeight, HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam, DWORD dwBand)
{
    HWND hWnd = g_origCreateWindowInBand(dwExStyle, lpClassName, lpWindowName, dwStyle, X, Y, nWidth, nHeight, hWndParent, hMenu, hInstance, lpParam, dwBand);
    if (hWnd)
    {
        OnWindowCreated(hWnd, hWndParent, lpClassName);
    }
    return hWnd;
}

using CreateWindowInBandEx_t = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID, DWORD, DWORD);
CreateWindowInBandEx_t g_origCreateWindowInBandEx = nullptr;

HWND WINAPI CreateWindowInBandEx_Hook(DWORD dwExStyle, LPCWSTR lpClassName, LPCWSTR lpWindowName, DWORD dwStyle, int X, int Y,
                                      int nWidth, int nHeight, HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam, DWORD dwBand, DWORD dwTypeFlags)
{
    HWND hWnd = g_origCreateWindowInBandEx(dwExStyle, lpClassName, lpWindowName, dwStyle, X, Y, nWidth, nHeight, hWndParent, hMenu, hInstance, lpParam, dwBand, dwTypeFlags);
    if (hWnd)
    {
        OnWindowCreated(hWnd, hWndParent, lpClassName);
    }
    return hWnd;
}

// The two registry hooks make Windows.UI.Xaml.dll believe DisableCompositionDiag is set while the advise thread
// is inside AdviseVisualTreeChange, the one place the value is read. The key usually does not exist, so a key
// that does is handed out in its place.
using RegOpenKeyExW_t = LSTATUS(WINAPI*)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
RegOpenKeyExW_t g_origRegOpenKeyExW = nullptr;

LSTATUS WINAPI RegOpenKeyExW_Hook(HKEY hKey, LPCWSTR lpSubKey, DWORD ulOptions, REGSAM samDesired, PHKEY phkResult)
{
    LSTATUS result = g_origRegOpenKeyExW(hKey, lpSubKey, ulOptions, samDesired, phkResult);
    if (result == ERROR_SUCCESS || !g_reportCompositionDiagDisabled || hKey != HKEY_LOCAL_MACHINE ||
        !lpSubKey || _wcsicmp(lpSubKey, L"Software\\Microsoft\\XAML\\Debug") != 0)
    {
        return result;
    }
    return g_origRegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft", ulOptions, samDesired, phkResult);
}

using RegQueryValueExW_t = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
RegQueryValueExW_t g_origRegQueryValueExW = nullptr;

LSTATUS WINAPI RegQueryValueExW_Hook(HKEY hKey, LPCWSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData)
{
    if (!g_reportCompositionDiagDisabled || !lpValueName || _wcsicmp(lpValueName, L"DisableCompositionDiag") != 0)
    {
        return g_origRegQueryValueExW(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);
    }
    if (lpType)
    {
        *lpType = REG_DWORD;
    }
    if (lpData && (!lpcbData || *lpcbData < sizeof(DWORD)))
    {
        if (lpcbData)
        {
            *lpcbData = sizeof(DWORD);
        }
        return ERROR_MORE_DATA;
    }
    if (lpData)
    {
        *reinterpret_cast<DWORD*>(lpData) = 1;
    }
    if (lpcbData)
    {
        *lpcbData = sizeof(DWORD);
    }
    return ERROR_SUCCESS;
}

// ---------------------------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------------------------

void OnTaskbarViewLoaded(HMODULE, void*)
{
    SP_LogDebug(L"Taskbar.View.dll is loaded");
    SweepExistingThreads();
}

BOOL Init()
{
    LoadSettings();

    if (!SP_HookBegin())
    {
        return FALSE;
    }

    bool ok = SP_SetExportHook(L"user32.dll", "CreateWindowExW", CreateWindowExW_Hook, &g_origCreateWindowExW);
    ok = SP_SetExportHook(L"kernelbase.dll", "RegOpenKeyExW", RegOpenKeyExW_Hook, &g_origRegOpenKeyExW) && ok;
    ok = SP_SetExportHook(L"kernelbase.dll", "RegQueryValueExW", RegQueryValueExW_Hook, &g_origRegQueryValueExW) && ok;

    // The in-band variants are what the shell uses for its own windows; they are exported, but not on every build.
    bool inBand = false;
    bool inBandEx = false;
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll"))
    {
        if (GetProcAddress(user32, "CreateWindowInBand"))
        {
            inBand = SP_SetExportHook(L"user32.dll", "CreateWindowInBand", CreateWindowInBand_Hook, &g_origCreateWindowInBand);
        }
        if (GetProcAddress(user32, "CreateWindowInBandEx"))
        {
            inBandEx = SP_SetExportHook(L"user32.dll", "CreateWindowInBandEx", CreateWindowInBandEx_Hook, &g_origCreateWindowInBandEx);
        }
    }

    if (!ok)
    {
        SP_HookAbort();
        SP_LogError(L"The window creation and registry hooks could not be installed");
        return FALSE;
    }
    if (!SP_HookCommit())
    {
        SP_LogError(L"The hooks could not be applied");
        return FALSE;
    }

    g_active.store(true, std::memory_order_release);

    SP_Log(L"Loaded: theme %s; hooked CreateWindowExW, RegOpenKeyExW, RegQueryValueExW; CreateWindowInBand %s, CreateWindowInBandEx %s",
           ThemeNameForIndex(g_themeIndex.load()), inBand ? L"yes" : L"no", inBandEx ? L"yes" : L"no");

    // The taskbar's library is what brings the XAML island; when it is there the existing threads are swept.
    SP_WaitForModule(L"Taskbar.View.dll", 60000, OnTaskbarViewLoaded, nullptr);
    return TRUE;
}

void AfterInit()
{
    SweepExistingThreads();
}

void SettingsChanged()
{
    DisconnectTap();
    LoadSettings();

    bool any = false;
    for (HWND hWnd : ThreadTargets())
    {
        if (RunOnWindowThread(hWnd, ReinitializeThreadProc, nullptr))
        {
            any = true;
        }
    }
    if (any)
    {
        EnsureTapConnected();
    }
    SP_Log(L"Settings applied: theme %s", ThemeNameForIndex(g_themeIndex.load()));
}

void BeforeUninit()
{
    g_active.store(false, std::memory_order_release);
    DisconnectTap();

    for (HWND hWnd : ThreadTargets())
    {
        RunOnWindowThread(hWnd, UninitializeThreadProc, nullptr);
    }

    std::vector<DWORD> leftover;
    {
        std::lock_guard<std::mutex> lock(g_threadsMutex);
        leftover = g_initializedThreads;
    }
    if (!leftover.empty())
    {
        SP_LogError(L"%zu XAML thread(s) could not be reached for restore", leftover.size());
    }
}

void Uninit()
{
    std::lock_guard<std::mutex> lock(g_threadsMutex);
    g_initializedThreads.clear();
}

}   // namespace

// ---------------------------------------------------------------------------------------------------------------
// COM entry points: how InitializeXamlDiagnosticsEx obtains the TAP object from this DLL
// ---------------------------------------------------------------------------------------------------------------

// The SDK headers already declare both with plain C linkage, so they are exported through the linker rather
// than with __declspec(dllexport), which the compiler would reject as a different linkage.
#pragma comment(linker, "/EXPORT:DllGetClassObject,PRIVATE")
#pragma comment(linker, "/EXPORT:DllCanUnloadNow,PRIVATE")

extern "C" HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv)
{
    if (!ppv)
    {
        return E_POINTER;
    }
    *ppv = nullptr;
    if (rclsid != CLSID_StylerTap)
    {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    try
    {
        return winrt::make_self<StylerTapFactory>()->QueryInterface(riid, ppv);
    }
    catch (...)
    {
        return winrt::to_hresult();
    }
}

// This DLL is the mod engine; it never unloads.
extern "C" HRESULT STDAPICALLTYPE DllCanUnloadNow(void)
{
    return S_FALSE;
}

SP_MOD_DEFINE(g_modTaskbarStyler) =
{
    /* id             */ SP_MOD_ID,
    /* name           */ L"Style the taskbar with a theme (Windows 11 Taskbar Styler)",
    /* basedOn        */ "windows-11-taskbar-styler",
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
