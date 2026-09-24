//
// backdrop.cpp - see backdrop.h.
//
#include <unknwn.h>

#include "backdrop.h"

#include <DispatcherQueue.h>
#include <dwmapi.h>
#include <windows.ui.composition.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>

#include "engine/log.h"

#pragma comment(lib, "CoreMessaging.lib")
#pragma comment(lib, "dwmapi.lib")

namespace sm {

namespace {

constexpr char kTag[] = "custom-start-menu";

namespace wuc = winrt::Windows::UI::Composition;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;

#ifndef DWMWA_USE_HOSTBACKDROPBRUSH
constexpr DWORD DWMWA_USE_HOSTBACKDROPBRUSH = 17;
#endif

} // namespace

struct Backdrop::Impl
{
    winrt::Windows::System::DispatcherQueueController queue{ nullptr };
    wuc::Compositor compositor{ nullptr };
    wuc::Desktop::DesktopWindowTarget target{ nullptr };
    wuc::ContainerVisual root{ nullptr };
    wuc::ContainerVisual layer{ nullptr };   // animated: opacity, scale, offset
    wuc::SpriteVisual sprite{ nullptr };     // the host backdrop, clipped to the panel's rounded rectangle
    wuc::CompositionRoundedRectangleGeometry clipShape{ nullptr };
};

bool TransparencyEnabled()
{
    DWORD value = 1, size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"EnableTransparency", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value != 0;
}

Backdrop::Backdrop() = default;

Backdrop::~Backdrop()
{
    Destroy();
}

bool Backdrop::Ready() const
{
    return m_impl && m_impl->sprite;
}

bool Backdrop::Create(HWND window)
{
    if (m_impl)
    {
        return Ready();
    }
    auto impl = std::make_unique<Impl>();
    try
    {
        if (!winrt::Windows::System::DispatcherQueue::GetForCurrentThread())
        {
            DispatcherQueueOptions options = { sizeof(options), DQTYPE_THREAD_CURRENT, DQTAT_COM_STA };
            winrt::check_hresult(CreateDispatcherQueueController(
                options, reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(
                             winrt::put_abi(impl->queue))));
        }
        BOOL on = TRUE;
        winrt::check_hresult(DwmSetWindowAttribute(window, DWMWA_USE_HOSTBACKDROPBRUSH, &on, sizeof(on)));

        impl->compositor = wuc::Compositor();
        auto interop = impl->compositor.as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>();
        winrt::check_hresult(interop->CreateDesktopWindowTarget(
            window, FALSE,   // not topmost: under the DirectComposition tree, which draws the panel itself
            reinterpret_cast<ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget**>(
                winrt::put_abi(impl->target))));

        impl->root = impl->compositor.CreateContainerVisual();
        impl->layer = impl->compositor.CreateContainerVisual();
        impl->sprite = impl->compositor.CreateSpriteVisual();
        impl->sprite.Brush(impl->compositor.CreateHostBackdropBrush());
        impl->clipShape = impl->compositor.CreateRoundedRectangleGeometry();
        impl->sprite.Clip(impl->compositor.CreateGeometricClip(impl->clipShape));
        impl->layer.Children().InsertAtTop(impl->sprite);
        impl->root.Children().InsertAtTop(impl->layer);
        impl->root.IsVisible(false);
        impl->target.Root(impl->root);
    }
    catch (const winrt::hresult_error& error)
    {
        SP_LOG_ERR(kTag, L"Acrylic is not available (0x%08X); the opaque background is used",
                   (unsigned)error.code().value);
        BOOL off = FALSE;
        DwmSetWindowAttribute(window, DWMWA_USE_HOSTBACKDROPBRUSH, &off, sizeof(off));
        return false;
    }
    m_impl = std::move(impl);
    return true;
}

void Backdrop::Destroy()
{
    if (!m_impl)
    {
        return;
    }
    try
    {
        if (m_impl->target)
        {
            m_impl->target.Root(nullptr);
        }
        m_impl->sprite = nullptr;
        m_impl->layer = nullptr;
        m_impl->root = nullptr;
        m_impl->target = nullptr;
        m_impl->compositor = nullptr;
        if (m_impl->queue)
        {
            // The queue belongs to this thread; ShutdownQueueAsync completes as the thread keeps pumping, and the
            // menu thread is about to stop pumping, so it is only released here.
            m_impl->queue = nullptr;
        }
    }
    catch (...)
    {
    }
    m_impl.reset();
}

void Backdrop::Layout(float x, float y, float width, float height, float radius)
{
    if (!Ready())
    {
        return;
    }
    try
    {
        m_impl->sprite.Offset(float3{ x, y, 0 });
        m_impl->sprite.Size(float2{ width, height });
        m_impl->clipShape.Size(float2{ width, height });
        m_impl->clipShape.CornerRadius(float2{ radius, radius });
    }
    catch (...)
    {
    }
}

void Backdrop::SetVisible(bool visible)
{
    if (!Ready())
    {
        return;
    }
    try
    {
        m_impl->root.IsVisible(visible);
    }
    catch (...)
    {
    }
}

void Backdrop::Animate(const float from[3], const float to[3], const double durations[3],
                       const motion::Bezier curves[3], float centerX, float centerY, bool animate)
{
    if (!Ready())
    {
        return;
    }
    try
    {
        wuc::Compositor& c = m_impl->compositor;
        wuc::ContainerVisual& layer = m_impl->layer;
        layer.CenterPoint(float3{ centerX, centerY, 0 });
        if (!animate)
        {
            layer.StopAnimation(L"Opacity");
            layer.StopAnimation(L"Offset");
            layer.StopAnimation(L"Scale");
            layer.Opacity(to[0]);
            layer.Offset(float3{ 0, to[1], 0 });
            layer.Scale(float3{ to[2], to[2], 1 });
            return;
        }
        auto ease = [&](int i) {
            return c.CreateCubicBezierEasingFunction(float2{ curves[i].x1, curves[i].y1 },
                                                     float2{ curves[i].x2, curves[i].y2 });
        };
        auto duration = [&](int i) {
            return winrt::Windows::Foundation::TimeSpan(std::chrono::microseconds((long long)(durations[i] * 1e6)));
        };

        auto opacity = c.CreateScalarKeyFrameAnimation();
        opacity.InsertKeyFrame(0.f, from[0]);
        opacity.InsertKeyFrame(1.f, to[0], ease(0));
        opacity.Duration(duration(0));
        layer.StartAnimation(L"Opacity", opacity);

        auto offset = c.CreateVector3KeyFrameAnimation();
        offset.InsertKeyFrame(0.f, float3{ 0, from[1], 0 });
        offset.InsertKeyFrame(1.f, float3{ 0, to[1], 0 }, ease(1));
        offset.Duration(duration(1));
        layer.StartAnimation(L"Offset", offset);

        auto scale = c.CreateVector3KeyFrameAnimation();
        scale.InsertKeyFrame(0.f, float3{ from[2], from[2], 1 });
        scale.InsertKeyFrame(1.f, float3{ to[2], to[2], 1 }, ease(2));
        scale.Duration(duration(2));
        layer.StartAnimation(L"Scale", scale);
    }
    catch (const winrt::hresult_error& error)
    {
        SP_LOG_ERR(kTag, L"Acrylic animation failed (0x%08X)", (unsigned)error.code().value);
    }
}

} // namespace sm
