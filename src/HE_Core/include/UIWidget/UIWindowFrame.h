#pragma once
#include "Types/Defines.h"
#include "UIWidget/UIElement.h"   // UICursor
#include <cstdint>

// ── The frame a borderless window does not have (docs/he-apps-plan.md F3) ────
// An application with its own title bar asks the OS for a window without one,
// and then owes the OS the answers the frame used to give: where the window can
// be picked up, and where its edges are. Both answers are geometry, so they live
// here — in HorizonCore, where a test can pin them — and not in the executable
// that happens to hold the SDL window.

namespace HE
{

// Which part of a borderless window a point lands on.
//
// The order is SDL_HitTestResult's order, value for value (NORMAL, DRAGGABLE,
// RESIZE_TOPLEFT … RESIZE_LEFT), so the SDL callback is a cast rather than a
// switch. Window.cpp static_asserts that pairing; nothing here may be reordered.
enum class UIWindowHit : uint8_t
{
    Normal = 0,
    Drag,
    ResizeTopLeft,
    ResizeTop,
    ResizeTopRight,
    ResizeRight,
    ResizeBottomRight,
    ResizeBottom,
    ResizeBottomLeft,
    ResizeLeft
};

HE_API const char* uiWindowHitName(UIWindowHit h);
HE_API bool        uiWindowHitIsResize(UIWindowHit h);
// The cursor an edge asks for. Only macOS needs it: Windows, X11 and Wayland set
// the cursor themselves once the hit test names an edge (see F3 in the plan).
HE_API UICursor    uiWindowHitCursor(UIWindowHit h);

// Which band of the frame (x, y) falls in, for a window of w x h. Every value is
// in ONE unit — whatever the caller passes in, points or pixels — and mixing the
// two is the bug this signature exists to make visible.
//
// border <= 0 turns edge resizing off entirely and always answers Normal.
// corner = how far along each edge the diagonal grip reaches; 0 = twice border,
// because a corner that is only as big as the edge is a corner nobody hits.
HE_API UIWindowHit uiWindowEdgeAt(float w, float h, float x, float y,
                                  float border, float corner = 0.0f);

// A window rectangle in the space SDL_GetWindowPosition / SDL_GetWindowSize
// speak: window points, top-left origin, desktop coordinates.
struct UIWindowRect
{
    int x = 0, y = 0, w = 0, h = 0;
    bool operator==(const UIWindowRect& o) const
    { return x == o.x && y == o.y && w == o.w && h == o.h; }
};

// ── The resize macOS does not do for us ──────────────────────────────────────
// SDL's Cocoa backend reads exactly one hit-test answer, SDL_HITTEST_DRAGGABLE
// (SDL_cocoawindow.m, processHitTest); every RESIZE_* result is ignored, and a
// borderless NSWindow has no system grips either. Windows (WM_NCHITTEST), X11
// (_NET_WM_MOVERESIZE) and Wayland all take the edge and resize the window
// themselves — and they swallow the press while doing it, which is why this
// state machine can be driven straight off the mouse events that DO arrive
// without a platform switch anywhere: on the three platforms that handle it,
// the press never comes.
class HE_API UIWindowResizer
{
public:
    // Start dragging `edge`, with the window at `win` and the pointer at the
    // given DESKTOP position (SDL_GetGlobalMouseState). A Normal or Drag edge
    // starts nothing — moving the window is the window manager's job even here.
    void begin(UIWindowHit edge, const UIWindowRect& win, int globalX, int globalY);
    void end();

    bool        active() const { return m_edge != UIWindowHit::Normal; }
    UIWindowHit edge()   const { return m_edge; }

    // Where the window belongs with the pointer at (globalX, globalY).
    // Both minimums are enforced by moving the FIXED edge back, never by letting
    // the dragged one push past it: shrinking a window from the left below its
    // minimum must pin the left edge, not walk the whole window leftwards.
    UIWindowRect update(int globalX, int globalY, int minW, int minH) const;

private:
    UIWindowHit  m_edge = UIWindowHit::Normal;
    UIWindowRect m_start{};
    int          m_grabX = 0, m_grabY = 0;
};

} // namespace HE
