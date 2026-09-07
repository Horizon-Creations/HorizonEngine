#include "UIWidget/UIWindowFrame.h"
#include <algorithm>

namespace HE
{

const char* uiWindowHitName(UIWindowHit h)
{
    switch (h)
    {
        case UIWindowHit::Normal:            return "Normal";
        case UIWindowHit::Drag:              return "Drag";
        case UIWindowHit::ResizeTopLeft:     return "Resize Top Left";
        case UIWindowHit::ResizeTop:         return "Resize Top";
        case UIWindowHit::ResizeTopRight:    return "Resize Top Right";
        case UIWindowHit::ResizeRight:       return "Resize Right";
        case UIWindowHit::ResizeBottomRight: return "Resize Bottom Right";
        case UIWindowHit::ResizeBottom:      return "Resize Bottom";
        case UIWindowHit::ResizeBottomLeft:  return "Resize Bottom Left";
        case UIWindowHit::ResizeLeft:        return "Resize Left";
    }
    return "Normal";
}

bool uiWindowHitIsResize(UIWindowHit h)
{
    return h >= UIWindowHit::ResizeTopLeft && h <= UIWindowHit::ResizeLeft;
}

UICursor uiWindowHitCursor(UIWindowHit h)
{
    switch (h)
    {
        case UIWindowHit::ResizeLeft:
        case UIWindowHit::ResizeRight:       return UICursor::ResizeWE;
        case UIWindowHit::ResizeTop:
        case UIWindowHit::ResizeBottom:      return UICursor::ResizeNS;
        case UIWindowHit::ResizeTopLeft:
        case UIWindowHit::ResizeBottomRight: return UICursor::ResizeNWSE;
        case UIWindowHit::ResizeTopRight:
        case UIWindowHit::ResizeBottomLeft:  return UICursor::ResizeNESW;
        default:                             return UICursor::Default;
    }
}

UIWindowHit uiWindowEdgeAt(float w, float h, float x, float y,
                           float border, float corner)
{
    if (border <= 0.0f) return UIWindowHit::Normal;
    if (w <= 0.0f || h <= 0.0f) return UIWindowHit::Normal;
    // Outside the window is not an edge of it. The hit test is only ever asked
    // about points inside, but a manual resize asks with a stale pointer often
    // enough that answering "left edge" for x = -40 would be a real bug.
    if (x < 0.0f || y < 0.0f || x > w || y > h) return UIWindowHit::Normal;

    // A window narrower than two borders would have a left edge that is also its
    // right one. Half the dimension is the honest cut: each edge keeps its own
    // side, and the middle disappears rather than the answer becoming arbitrary.
    const float bx = std::min(border, w * 0.5f);
    const float by = std::min(border, h * 0.5f);
    if (corner <= 0.0f) corner = border * 2.0f;
    const float cx = std::min(corner, w * 0.5f);
    const float cy = std::min(corner, h * 0.5f);

    const bool left   = x <= bx;
    const bool right  = x >= w - bx;
    const bool top    = y <= by;
    const bool bottom = y >= h - by;
    if (!left && !right && !top && !bottom) return UIWindowHit::Normal;

    // Corners first, and they reach FURTHER along the edge than the edge reaches
    // inwards: a 6px corner is a 6x6 square nobody can hit on purpose.
    const bool nearLeft   = x <= cx;
    const bool nearRight  = x >= w - cx;
    const bool nearTop    = y <= cy;
    const bool nearBottom = y >= h - cy;
    if (top    && nearLeft)  return UIWindowHit::ResizeTopLeft;
    if (top    && nearRight) return UIWindowHit::ResizeTopRight;
    if (bottom && nearLeft)  return UIWindowHit::ResizeBottomLeft;
    if (bottom && nearRight) return UIWindowHit::ResizeBottomRight;
    if (left   && nearTop)   return UIWindowHit::ResizeTopLeft;
    if (right  && nearTop)   return UIWindowHit::ResizeTopRight;
    if (left   && nearBottom)return UIWindowHit::ResizeBottomLeft;
    if (right  && nearBottom)return UIWindowHit::ResizeBottomRight;

    if (top)    return UIWindowHit::ResizeTop;
    if (bottom) return UIWindowHit::ResizeBottom;
    if (left)   return UIWindowHit::ResizeLeft;
    return UIWindowHit::ResizeRight;
}

void UIWindowResizer::begin(UIWindowHit edge, const UIWindowRect& win,
                            int globalX, int globalY)
{
    if (!uiWindowHitIsResize(edge)) { end(); return; }
    m_edge  = edge;
    m_start = win;
    m_grabX = globalX;
    m_grabY = globalY;
}

void UIWindowResizer::end()
{
    m_edge = UIWindowHit::Normal;
    m_start = UIWindowRect{};
    m_grabX = m_grabY = 0;
}

UIWindowRect UIWindowResizer::update(int globalX, int globalY, int minW, int minH) const
{
    if (!active()) return m_start;
    if (minW < 1) minW = 1;
    if (minH < 1) minH = 1;

    const int dx = globalX - m_grabX;
    const int dy = globalY - m_grabY;

    bool west = false, east = false, north = false, south = false;
    switch (m_edge)
    {
        case UIWindowHit::ResizeLeft:        west = true; break;
        case UIWindowHit::ResizeRight:       east = true; break;
        case UIWindowHit::ResizeTop:         north = true; break;
        case UIWindowHit::ResizeBottom:      south = true; break;
        case UIWindowHit::ResizeTopLeft:     north = west = true; break;
        case UIWindowHit::ResizeTopRight:    north = east = true; break;
        case UIWindowHit::ResizeBottomLeft:  south = west = true; break;
        case UIWindowHit::ResizeBottomRight: south = east = true; break;
        default: break;
    }

    UIWindowRect r = m_start;
    if (west)
    {
        // The right edge is the one that stays put, so it is the one the
        // arithmetic is written around: the minimum bites into x, not into w.
        const int rightEdge = m_start.x + m_start.w;
        r.x = std::min(m_start.x + dx, rightEdge - minW);
        r.w = rightEdge - r.x;
    }
    else if (east)
        r.w = std::max(m_start.w + dx, minW);

    if (north)
    {
        const int bottomEdge = m_start.y + m_start.h;
        r.y = std::min(m_start.y + dy, bottomEdge - minH);
        r.h = bottomEdge - r.y;
    }
    else if (south)
        r.h = std::max(m_start.h + dy, minH);

    return r;
}

} // namespace HE
