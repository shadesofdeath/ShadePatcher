#pragma once
//
// backdrop.h - the real acrylic behind the panel.
//
// DirectComposition cannot sample what is behind a window. Windows.UI.Composition can, through the host backdrop
// brush (the blurred desktop DWM keeps for acrylic), so the acrylic layer is a Windows.UI.Composition visual tree
// on the same window as the DirectComposition one: its target is not topmost, so it sits under the menu's own
// DirectComposition content, and the panel's translucent tint is drawn over it. The window needs
// DWMWA_USE_HOSTBACKDROPBRUSH; it works on the click-through, never-activated visual window (measured).
//
// The two trees animate separately, so the backdrop is given the same curves and durations as the panel and
// started in the same message; they finish within a frame of each other.
//
// Everything here goes through C++/WinRT, which reports errors by throwing: every entry point catches, logs
// and leaves the backdrop off. The menu then simply draws its opaque background.
//
#include <Windows.h>

#include <memory>

#include "tokens.h"

namespace sm {

class Backdrop
{
public:
    Backdrop();
    ~Backdrop();

    // Creates the Windows.UI.Composition tree on `window`. Needs a DispatcherQueue on the calling thread; one is
    // created when there is none. Returns false when composition or the host backdrop is not available.
    bool Create(HWND window);
    void Destroy();
    bool Ready() const;

    // Panel rectangle in window pixels and its corner radius.
    void Layout(float x, float y, float width, float height, float radius);
    void SetVisible(bool visible);

    // The open/close animation: [0] opacity, [1] vertical offset in pixels, [2] scale, about (centerX, centerY).
    // With `animate` false the `to` values are set at once.
    void Animate(const float from[3], const float to[3], const double durations[3], const motion::Bezier curves[3],
                 float centerX, float centerY, bool animate);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// True when Windows' "Transparency effects" are on; with them off DWM paints the host backdrop as a flat colour,
// so the menu uses its opaque background instead.
bool TransparencyEnabled();

} // namespace sm
