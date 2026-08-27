#include <UIWidget/UIWidgetTree.h>
#include <cstdint>
#include <GraphCommon/GraphJson.h>
#include <GraphCommon/GraphModel.h>
#include <UIWidget/UIElements.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>

namespace HE {

// A widget tree is a parent-id TREE, not a link graph: it shares find/add id
// handling with the node graphs (GraphModel.h) but has no links, hence no
// connect/disconnect and a removeSubtree instead of removeNode.
UIElement* UIWidgetTree::find(int id)
{ return HE::graph::findNodeById(elements, id); }
const UIElement* UIWidgetTree::find(int id) const
{ return HE::graph::findNodeById(elements, id); }

std::vector<int> UIWidgetTree::childrenOf(int parentId) const
{
    std::vector<int> out;
    for (const auto& e : elements) if (e->parentId == parentId) out.push_back(e->id);
    return out;
}

bool UIWidgetTree::isDescendantOf(int id, int ancestorId) const
{
    int cur = id;
    for (size_t guard = 0; guard <= elements.size(); ++guard)
    {
        if (cur == ancestorId) return true;
        const UIElement* n = find(cur);
        if (!n) return false;
        cur = n->parentId;
        if (cur == 0) return ancestorId == 0;
    }
    return false;
}

void UIWidgetTree::removeSubtree(int id)
{
    std::vector<int> doomed{ id };
    for (size_t i = 0; i < doomed.size(); ++i)
        for (int c : childrenOf(doomed[i]))
            doomed.push_back(c);
    elements.erase(std::remove_if(elements.begin(), elements.end(),
        [&](const std::unique_ptr<UIElement>& e){
            return std::find(doomed.begin(), doomed.end(), e->id) != doomed.end();
        }), elements.end());
}

int UIWidgetTree::add(std::unique_ptr<UIElement> e)
{
    return HE::graph::appendNode(elements, nextId, std::move(e));
}

int UIWidgetTree::add(UIWidgetType type)
{
    auto e = makeUIElement(type);
    e->name = e->typeName();
    return add(std::move(e));
}

// ── Layout ───────────────────────────────────────────────────────────────────

namespace
{
    // The 3×3 point anchors, in the order the legacy "anchor" field numbered
    // them: 0 = TopLeft … 8 = BottomRight.
    void anchorPoint01(int a, float& ax, float& ay)
    {
        static const float pts[9][2] = {
            {0.0f,0.0f},{0.5f,0.0f},{1.0f,0.0f},
            {0.0f,0.5f},{0.5f,0.5f},{1.0f,0.5f},
            {0.0f,1.0f},{0.5f,1.0f},{1.0f,1.0f} };
        const int i = (a < 0 || a > 8) ? 0 : a;
        ax = pts[i][0]; ay = pts[i][1];
    }

    bool nearly(float a, float b) { return std::fabs(a - b) < 1e-4f; }

    // One axis of the layout, shared by both: `lo`/`hi` are the anchored span
    // inside the parent, `pos`/`size` the element's two fields, `pivot` its
    // pivot on this axis. Where the span is zero this is exactly the old
    // point-anchor formula, which is why every existing asset lays out
    // unchanged.
    void solveAxis(float lo, float hi, float pos, float size, float pivot,
                   float& outMin, float& outSize)
    {
        const float span = hi - lo;
        outSize = span + size;
        const float ref = lo + pivot * span;      // the anchor point at the pivot
        outMin = ref + pos - pivot * outSize;
    }

    UIWidgetRect parentRectOf(const UIWidgetTree& tree, const UIElement& e,
                              const UIWidgetCanvas* canvas)
    {
        UIWidgetRect parent{ 0.0f, 0.0f,
                             canvas ? canvas->width  : tree.canvasWidth,
                             canvas ? canvas->height : tree.canvasHeight };
        if (e.parentId != 0)
            if (const UIElement* p = tree.find(e.parentId))
                parent = uiElementRect(tree, *p, canvas);
        return parent;
    }
}

const char* uiCanvasScaleModeName(UICanvasScaleMode m)
{
    switch (m)
    {
    case UICanvasScaleMode::Stretch:       return "Stretch";
    case UICanvasScaleMode::FitInside:     return "Fit Inside";
    case UICanvasScaleMode::FillOutside:   return "Fill Outside";
    case UICanvasScaleMode::MatchWidth:    return "Match Width";
    case UICanvasScaleMode::MatchHeight:   return "Match Height";
    case UICanvasScaleMode::ConstantPixel: return "Constant Pixel";
    }
    return "Stretch";
}

UIWidgetCanvas uiResolveCanvas(const UIWidgetTree& tree, float vpWidth, float vpHeight)
{
    return uiResolveCanvasFor(tree.canvasWidth, tree.canvasHeight, tree.scaleMode,
                              vpWidth, vpHeight);
}

UIWidgetCanvas uiResolveCanvasFor(float authoredW, float authoredH, UICanvasScaleMode mode,
                                  float vpWidth, float vpHeight)
{
    const float vw = std::max(1.0f, vpWidth), vh = std::max(1.0f, vpHeight);
    const float rw = std::max(1.0f, authoredW), rh = std::max(1.0f, authoredH);
    const float sx = vw / rw, sy = vh / rh;

    UIWidgetCanvas c;
    if (mode == UICanvasScaleMode::Stretch)
    {
        // The authored canvas IS the layout canvas; the two axes take whatever
        // factor they need to cover the viewport.
        c.scaleX = sx; c.scaleY = sy;
        c.width  = rw; c.height = rh;
        return c;
    }

    float s = 1.0f;
    switch (mode)
    {
    case UICanvasScaleMode::FitInside:     s = std::min(sx, sy); break;
    case UICanvasScaleMode::FillOutside:   s = std::max(sx, sy); break;
    case UICanvasScaleMode::MatchWidth:    s = sx; break;
    case UICanvasScaleMode::MatchHeight:   s = sy; break;
    case UICanvasScaleMode::ConstantPixel: s = 1.0f; break;
    case UICanvasScaleMode::Stretch:       break; // handled above
    }
    s = std::max(s, 1e-4f);
    // One factor for both axes, and a canvas as big as the screen really is in
    // those units. Nothing is distorted and nothing is letterboxed: a wider
    // screen simply HAS more canvas, which is exactly what an element anchored
    // to its right edge needs.
    c.scaleX = c.scaleY = s;
    c.width  = vw / s;
    c.height = vh / s;
    return c;
}

void uiAnchorPresetRect(int preset, float& minX, float& minY, float& maxX, float& maxY)
{
    // col/row 3 is the stretched one; 0/1/2 are the three point positions.
    static const float lo[4] = { 0.0f, 0.5f, 1.0f, 0.0f };
    static const float hi[4] = { 0.0f, 0.5f, 1.0f, 1.0f };
    const int p   = (preset < 0 || preset >= kUIAnchorPresetCount) ? 0 : preset;
    const int row = p / 4, col = p % 4;
    minX = lo[col]; maxX = hi[col];
    minY = lo[row]; maxY = hi[row];
}

int uiAnchorPresetOf(const UIElement& e)
{
    for (int p = 0; p < kUIAnchorPresetCount; ++p)
    {
        float x0, y0, x1, y1;
        uiAnchorPresetRect(p, x0, y0, x1, y1);
        if (nearly(x0, e.anchorMinX) && nearly(y0, e.anchorMinY) &&
            nearly(x1, e.anchorMaxX) && nearly(y1, e.anchorMaxY))
            return p;
    }
    return -1;
}

void uiSetAnchorPreset(UIElement& e, int preset)
{
    uiAnchorPresetRect(preset, e.anchorMinX, e.anchorMinY, e.anchorMaxX, e.anchorMaxY);
}

void uiReanchorKeepingRect(const UIWidgetTree& tree, UIElement& e, int preset)
{
    const UIWidgetRect keep   = uiElementRect(tree, e);
    const UIWidgetRect parent = parentRectOf(tree, e, nullptr);
    uiSetAnchorPreset(e, preset);

    // Invert solveAxis for the new span: size becomes the difference to the
    // anchored span, pos the offset of the pivot from the anchor reference.
    auto rebuild = [](float parentLo, float parentSize, float aMin, float aMax,
                      float keepMin, float keepSize, float pivot,
                      float& pos, float& size)
    {
        const float lo   = parentLo + aMin * parentSize;
        const float hi   = parentLo + aMax * parentSize;
        const float span = hi - lo;
        size = keepSize - span;
        const float ref = lo + pivot * span;
        pos = keepMin + pivot * keepSize - ref;
    };
    rebuild(parent.x, parent.w, e.anchorMinX, e.anchorMaxX, keep.x, keep.w, e.pivotX,
            e.posX, e.sizeX);
    rebuild(parent.y, parent.h, e.anchorMinY, e.anchorMaxY, keep.y, keep.h, e.pivotY,
            e.posY, e.sizeY);
}

void uiAnchorFromLegacyPoint(UIElement& e, int ninePoint)
{
    float ax, ay;
    anchorPoint01(ninePoint, ax, ay);
    e.anchorMinX = e.anchorMaxX = ax;
    e.anchorMinY = e.anchorMaxY = ay;
}

int uiAnchorLegacyPointOf(const UIElement& e)
{
    if (!nearly(e.anchorMinX, e.anchorMaxX) || !nearly(e.anchorMinY, e.anchorMaxY))
        return -1;
    for (int i = 0; i < 9; ++i)
    {
        float ax, ay;
        anchorPoint01(i, ax, ay);
        if (nearly(ax, e.anchorMinX) && nearly(ay, e.anchorMinY)) return i;
    }
    return -1;
}

UIWidgetRect uiElementAnchorRect(const UIWidgetTree& tree, const UIElement& e,
                                 const UIWidgetCanvas* canvas)
{
    const UIWidgetRect parent = parentRectOf(tree, e, canvas);
    UIWidgetRect a;
    a.x = parent.x + e.anchorMinX * parent.w;
    a.y = parent.y + e.anchorMinY * parent.h;
    a.w = (e.anchorMaxX - e.anchorMinX) * parent.w;
    a.h = (e.anchorMaxY - e.anchorMinY) * parent.h;
    return a;
}

namespace
{
    // The slot a layout container hands one of its children.
    //
    // Along the box's axis every visible child takes its own size, unless its
    // slotFill is > 0: those share what is left after the fixed ones and the
    // gaps, in proportion. Across the axis each child gets the full inner
    // extent. An invisible child takes no space, so hiding one closes the gap
    // instead of leaving a hole where it used to be.
    UIWidgetRect boxSlotRect(const UIWidgetTree& tree, const UIElement& box,
                             const UIElement& child, const UIWidgetCanvas* canvas)
    {
        const UIWidgetRect b = uiElementRect(tree, box, canvas);
        // Read through the property table rather than a cast: every container
        // type declares these two by name, so a Grid or a scroll box added
        // later is laid out by this same code without touching it.
        // The box's own numbers are in its widget's units too (a box inside an
        // embedded widget is padded in that widget's terms).
        float bus = 1.0f, bvs = 1.0f;
        uiElementUnitScale(tree, box, bus, bvs, canvas);
        const bool  vert = box.stacksVertically();
        const float axisScale = vert ? bvs : bus;
        const float pad  = std::max(0.0f, box.getProp("Padding").f);
        const float gap  = std::max(0.0f, box.getProp("Spacing").f) * axisScale;

        const float padX = pad * bus, padY = pad * bvs;
        UIWidgetRect inner{ b.x + padX, b.y + padY,
                            std::max(0.0f, b.w - 2.0f * padX),
                            std::max(0.0f, b.h - 2.0f * padY) };
        const float axisSpace = vert ? inner.h : inner.w;

        // One pass over the siblings for the totals, a second to walk to this
        // child. The lists are short (a box holds what fits on a screen), and
        // this keeps the layout stateless — no cached slot table to invalidate.
        float fixed = 0.0f, fillSum = 0.0f;
        int   count = 0;
        for (const auto& sp : tree.elements)
        {
            if (!sp || sp->parentId != box.id || !sp->visible) continue;
            ++count;
            // A child's own size is in the same units as the box's (they are in
            // the same widget), so one axis factor converts both.
            if (sp->slotFill > 0.0f) fillSum += sp->slotFill;
            else                     fixed += (vert ? sp->sizeY : sp->sizeX) * axisScale;
        }
        const float gaps = count > 1 ? gap * static_cast<float>(count - 1) : 0.0f;
        const float leftover = std::max(0.0f, axisSpace - fixed - gaps);

        // A scroll box shifts its whole stack along the axis. It is the only
        // thing that distinguishes it from a plain vertical box — the clipping
        // that makes the shift readable is the ordinary clipChildren.
        float cursor = vert ? inner.y : inner.x;
        if (const auto* sb = dynamic_cast<const UIScrollBox*>(&box))
            cursor -= sb->scrollOffset * axisScale;
        UIWidgetRect out{ inner.x, inner.y, inner.w, inner.h };
        for (const auto& sp : tree.elements)
        {
            if (!sp || sp->parentId != box.id || !sp->visible) continue;
            const float extent = (sp->slotFill > 0.0f && fillSum > 0.0f)
                ? leftover * (sp->slotFill / fillSum)
                : (vert ? sp->sizeY : sp->sizeX) * axisScale;
            if (sp->id == child.id)
            {
                if (vert) { out.x = inner.x; out.w = inner.w; out.y = cursor; out.h = extent; }
                else      { out.y = inner.y; out.h = inner.h; out.x = cursor; out.w = extent; }
                break;
            }
            cursor += extent + gap;
        }
        return out;
    }
}

void uiElementUnitScale(const UIWidgetTree& tree, const UIElement& e,
                        float& outScaleX, float& outScaleY, const UIWidgetCanvas* canvas)
{
    outScaleX = outScaleY = 1.0f;
    // Nearest WidgetRef above this element, if any.
    int guard = 0;
    const UIElement* cur = &e;
    while (cur->parentId != 0 && guard++ < static_cast<int>(tree.elements.size()) + 1)
    {
        const UIElement* p = tree.find(cur->parentId);
        if (!p) return;
        if (const auto* ref = dynamic_cast<const UIWidgetRef*>(p))
        {
            if (ref->contentW <= 0.0f || ref->contentH <= 0.0f) return;
            // The ref's rect is that widget's screen, and its own scale mode
            // decides how its canvas meets it — the same call the real screen
            // goes through. Its rect already carries any scaling from a
            // WidgetRef further up, so this one factor is the whole story.
            const UIWidgetRect r = uiElementRect(tree, *ref, canvas);
            const UIWidgetCanvas sub = uiResolveCanvasFor(ref->contentW, ref->contentH,
                                                          ref->contentMode, r.w, r.h);
            outScaleX = sub.scaleX;
            outScaleY = sub.scaleY;
            return;
        }
        cur = p;
    }
}

UIWidgetRect uiElementRect(const UIWidgetTree& tree, const UIElement& e,
                           const UIWidgetCanvas* canvas)
{
    // A child of a layout container does not place itself: the box does, and
    // the child's own anchors and position are not consulted at all.
    if (e.parentId != 0)
        if (const UIElement* p = tree.find(e.parentId); p && p->laysOutChildren())
            return boxSlotRect(tree, *p, e, canvas);

    const UIWidgetRect parent = parentRectOf(tree, e, canvas);
    const float lox = parent.x + e.anchorMinX * parent.w;
    const float hix = parent.x + e.anchorMaxX * parent.w;
    const float loy = parent.y + e.anchorMinY * parent.h;
    const float hiy = parent.y + e.anchorMaxY * parent.h;

    // Inside an embedded widget the element's own numbers are in THAT widget's
    // units; the anchors are fractions and need no conversion.
    float us = 1.0f, vs = 1.0f;
    uiElementUnitScale(tree, e, us, vs, canvas);

    UIWidgetRect r;
    solveAxis(lox, hix, e.posX * us, e.sizeX * us, e.pivotX, r.x, r.w);
    solveAxis(loy, hiy, e.posY * vs, e.sizeY * vs, e.pivotY, r.y, r.h);
    return r;
}

// left = pos - pivot*size and right = -left - size, from solveAxis with the
// span divided out. Both are the distance from the anchored edge inward, so a
// margin of 20 on each side is (20, 20) whatever the parent's width is.
void uiAnchorInsetsX(const UIElement& e, float& left, float& right)
{
    left  = e.posX - e.pivotX * e.sizeX;
    right = -left - e.sizeX;
}
void uiAnchorInsetsY(const UIElement& e, float& top, float& bottom)
{
    top    = e.posY - e.pivotY * e.sizeY;
    bottom = -top - e.sizeY;
}
void uiSetAnchorInsetsX(UIElement& e, float left, float right)
{
    e.sizeX = -(left + right);
    e.posX  = left + e.pivotX * e.sizeX;
}
void uiSetAnchorInsetsY(UIElement& e, float top, float bottom)
{
    e.sizeY = -(top + bottom);
    e.posY  = top + e.pivotY * e.sizeY;
}

void uiApplyAutoSize(UIWidgetTree& tree, const UIWidgetCanvas* canvas)
{
    // The width handed over is the one the anchor already decides. Where the
    // anchor is a point that is just the element's own size (nothing changes);
    // where it stretches, it is the span the parent gives it, which is what a
    // wrapping text has to be measured against.
    for (auto& e : tree.elements)
    {
        if (!e) continue;
        // The width is handed over in the element's OWN units: it measures its
        // content in those (a font size is authored there), and inside an
        // embedded widget the resolved rect is in the host's units instead.
        float us = 1.0f, vs = 1.0f;
        uiElementUnitScale(tree, *e, us, vs, canvas);
        e->applyAutoSize(uiElementRect(tree, *e, canvas).w / std::max(1e-4f, us));
    }

    // Then the containers that size themselves to what is in them, INNERMOST
    // FIRST: a box that holds a box has to be measured after the one inside it
    // has found its own size, or it would measure last frame's.
    std::vector<std::pair<int, UIBoxBase*>> boxes;   // (depth, box)
    for (auto& ep : tree.elements)
    {
        auto* box = dynamic_cast<UIBoxBase*>(ep.get());
        if (!box || !box->sizeToContent) continue;
        int depth = 0, guard = 0;
        for (const UIElement* c = box;
             c->parentId != 0 && guard++ < static_cast<int>(tree.elements.size()) + 1;)
        {
            const UIElement* p = tree.find(c->parentId);
            if (!p) break;
            ++depth; c = p;
        }
        boxes.emplace_back(depth, box);
    }
    std::sort(boxes.begin(), boxes.end(),
              [](const auto& a, const auto& b){ return a.first > b.first; });

    for (auto& [depth, box] : boxes)
    {
        const bool  vert = box->stacksVertically();
        const float pad  = std::max(0.0f, box->padding);
        const float gap  = std::max(0.0f, box->spacing);
        float along = 0.0f, across = 0.0f;
        int   count = 0;
        for (const auto& cp : tree.elements)
        {
            if (!cp || cp->parentId != box->id || !cp->visible) continue;
            ++count;
            // A filling child's size IS the leftover this is computing, so it
            // contributes nothing along the axis — across it still counts.
            if (cp->slotFill <= 0.0f) along += vert ? cp->sizeY : cp->sizeX;
            across = std::max(across, vert ? cp->sizeX : cp->sizeY);
        }
        if (count > 1) along += gap * static_cast<float>(count - 1);
        const float w = (vert ? across : along) + 2.0f * pad;
        const float h = (vert ? along : across) + 2.0f * pad;
        box->sizeX = std::max(box->minSizeX, w);
        box->sizeY = std::max(box->minSizeY, h);
    }
}

bool uiElementClipRect(const UIWidgetTree& tree, const UIElement& e,
                       UIWidgetRect& out, const UIWidgetCanvas* canvas)
{
    bool any = false;
    UIWidgetRect acc{};
    // A text field clips its OWN content, without anyone having to tick a box:
    // text longer than the field scrolls sideways under the caret, and glyphs
    // that scrolled out have to stop at the edge instead of spilling across
    // whatever sits next to the field.
    if (e.type() == UIWidgetType::TextInput)
    {
        acc = uiElementRect(tree, e, canvas);
        any = true;
    }
    // Walk up. The chain is short (a UI tree is shallow) and bounded by the
    // element count, so a cycle in a hand-edited file cannot hang this.
    int guard = 0;
    const UIElement* cur = &e;
    while (cur->parentId != 0 && guard++ < static_cast<int>(tree.elements.size()) + 1)
    {
        const UIElement* p = tree.find(cur->parentId);
        if (!p) break;
        if (p->clipChildren)
        {
            const UIWidgetRect r = uiElementRect(tree, *p, canvas);
            if (!any) { acc = r; any = true; }
            else
            {
                const float x0 = std::max(acc.x, r.x), y0 = std::max(acc.y, r.y);
                const float x1 = std::min(acc.x + acc.w, r.x + r.w);
                const float y1 = std::min(acc.y + acc.h, r.y + r.h);
                acc.x = x0; acc.y = y0; acc.w = x1 - x0; acc.h = y1 - y0;
            }
        }
        cur = p;
    }
    if (any) out = acc;
    return any;
}

bool uiElementEffectiveVisible(const UIWidgetTree& tree, const UIElement& e)
{
    if (!e.visible) return false;
    if (e.parentId == 0) return true;
    const UIElement* p = tree.find(e.parentId);
    return p ? uiElementEffectiveVisible(tree, *p) : true;
}

void uiUpdateScrollExtents(UIWidgetTree& tree)
{
    for (auto& bp : tree.elements)
    {
        auto* sb = dynamic_cast<UIScrollBox*>(bp.get());
        if (!sb) continue;
        // The stacked height of the visible children, in the same terms the
        // slot algorithm uses: own size unless the slot fills, plus the gaps.
        // A filling child cannot make the content taller than the box — that is
        // what filling MEANS — so it contributes nothing to the overflow.
        float total = 0.0f;
        int   count = 0;
        for (const auto& cp : tree.elements)
        {
            if (!cp || cp->parentId != sb->id || !cp->visible) continue;
            ++count;
            if (cp->slotFill <= 0.0f) total += cp->sizeY;
        }
        if (count > 1) total += std::max(0.0f, sb->spacing) * static_cast<float>(count - 1);
        sb->contentExtent = total;
        sb->scrollOffset = std::clamp(sb->scrollOffset, 0.0f, sb->maxScroll());
    }
}

bool uiScrollBy(UIWidgetTree& tree, int id, float delta)
{
    auto* sb = dynamic_cast<UIScrollBox*>(tree.find(id));
    if (!sb) return false;
    const float maxOff = sb->maxScroll();
    if (maxOff <= 0.0f) return false;
    const float before = sb->scrollOffset;
    sb->scrollOffset = std::clamp(before + delta, 0.0f, maxOff);
    // At either end the wheel belongs to whatever is behind this box.
    return sb->scrollOffset != before;
}

float uiElementEffectiveOpacity(const UIWidgetTree& tree, const UIElement& e)
{
    float a = std::clamp(e.renderOpacity, 0.0f, 1.0f);
    // Iterative, not recursive: this runs per element per frame, and the guard
    // bounds a parent chain that a hand-edited file could have made cyclic.
    int guard = 0;
    const UIElement* cur = &e;
    while (cur->parentId != 0 && guard++ < static_cast<int>(tree.elements.size()) + 1)
    {
        const UIElement* p = tree.find(cur->parentId);
        if (!p) break;
        a *= std::clamp(p->renderOpacity, 0.0f, 1.0f);
        cur = p;
    }
    return a;
}

bool uiElementRotation(const UIWidgetTree& tree, const UIElement& e,
                       UIRotation& out, const UIWidgetCanvas* canvas)
{
    // Anything to do at all? Walk the chain once looking for a non-zero angle;
    // almost every element in almost every widget leaves here.
    float total = e.rotation;
    {
        int guard = 0;
        const UIElement* cur = &e;
        while (cur->parentId != 0 && guard++ < static_cast<int>(tree.elements.size()) + 1)
        {
            const UIElement* p = tree.find(cur->parentId);
            if (!p) break;
            total += p->rotation;
            cur = p;
        }
        if (std::fabs(total) < 1e-4f && std::fabs(e.rotation) < 1e-4f)
        {
            // A chain that cancels out to zero degrees still MOVES the element
            // (two opposite rotations about different points are a
            // translation), so only a chain with no rotation at all is free.
            bool any = false;
            int g2 = 0;
            const UIElement* c2 = &e;
            while (c2->parentId != 0 && g2++ < static_cast<int>(tree.elements.size()) + 1)
            {
                const UIElement* p = tree.find(c2->parentId);
                if (!p) break;
                if (std::fabs(p->rotation) > 1e-4f) { any = true; break; }
                c2 = p;
            }
            if (!any) return false;
        }
    }

    const UIWidgetRect r = uiElementRect(tree, e, canvas);
    out.degrees = total;
    out.srcX = r.x + e.pivotX * r.w;
    out.srcY = r.y + e.pivotY * r.h;

    // Carry that pivot up through every rotating ancestor, innermost first —
    // each one turns it about its OWN pivot in the unrotated layout.
    float px = out.srcX, py = out.srcY;
    int guard = 0;
    const UIElement* cur = &e;
    while (cur->parentId != 0 && guard++ < static_cast<int>(tree.elements.size()) + 1)
    {
        const UIElement* p = tree.find(cur->parentId);
        if (!p) break;
        if (std::fabs(p->rotation) > 1e-4f)
        {
            const UIWidgetRect pr = uiElementRect(tree, *p, canvas);
            const float ax = pr.x + p->pivotX * pr.w;
            const float ay = pr.y + p->pivotY * pr.h;
            const float a  = p->rotation * 3.14159265358979323846f / 180.0f;
            const float c = std::cos(a), s = std::sin(a);
            const float dx = px - ax, dy = py - ay;
            px = ax + dx * c - dy * s;
            py = ay + dx * s + dy * c;
        }
        cur = p;
    }
    out.dstX = px;
    out.dstY = py;
    return true;
}

void uiUnrotatePoint(const UIRotation& r, float x, float y, float& outX, float& outY)
{
    const float a = -r.degrees * 3.14159265358979323846f / 180.0f;
    const float c = std::cos(a), s = std::sin(a);
    const float dx = x - r.dstX, dy = y - r.dstY;
    outX = r.srcX + dx * c - dy * s;
    outY = r.srcY + dx * s + dy * c;
}

bool uiElementEffectiveEnabled(const UIWidgetTree& tree, const UIElement& e)
{
    if (!e.enabled) return false;
    int guard = 0;
    const UIElement* cur = &e;
    while (cur->parentId != 0 && guard++ < static_cast<int>(tree.elements.size()) + 1)
    {
        const UIElement* p = tree.find(cur->parentId);
        if (!p) break;
        if (!p->enabled) return false;
        cur = p;
    }
    return true;
}

// ── JSON ─────────────────────────────────────────────────────────────────────
// The element writer/reader comes first; the tree serializers are assembled from
// it, so the per-element form collaboration sends (CollabDocSync) and the on-disk
// form cannot drift apart.

namespace
{
nlohmann::json uiElementToJsonObj(const UIElement& e)
{
    nlohmann::json o = {
        { "id",      e.id },
        { "parent",  e.parentId },
        { "type",    e.typeName() },  // by name → schema-evolution safe
        { "name",    e.name },
        { "pos",     { e.posX, e.posY } },
        { "size",    { e.sizeX, e.sizeY } },
        { "pivot",   { e.pivotX, e.pivotY } },
        // The 9-point field an anchor POINT has always been written as, so a
        // document full of them is byte-identical to what earlier versions
        // wrote. An anchor that spans a side (or the whole parent) has no
        // 9-point name and carries its rectangle instead; "anchor" then stays
        // at 0 for a reader that only knows the old field.
        { "anchor",  std::max(0, uiAnchorLegacyPointOf(e)) },
        { "layer",   e.layer },
        { "visible", e.visible },
    };
    if (uiAnchorLegacyPointOf(e) < 0)
    {
        o["anchorMin"] = { e.anchorMinX, e.anchorMinY };
        o["anchorMax"] = { e.anchorMaxX, e.anchorMaxY };
    }
    if (!e.material.empty()) o["material"] = e.material;
    if (!e.texture.empty())  o["texture"]  = e.texture;
    if (!e.font.empty())     o["font"]     = e.font;
    if (!e.hitTestable)      o["hitTestable"] = false;
    if (e.clipChildren)      o["clipChildren"] = true;
    if (e.renderOpacity < 1.0f) o["renderOpacity"] = e.renderOpacity;
    if (!e.enabled)          o["enabled"] = false;
    if (e.slotFill > 0.0f)   o["slotFill"] = e.slotFill;
    if (e.rotation != 0.0f)  o["rotation"] = e.rotation;
    if (e.hoverCursor != HE::UICursor::Default)
        o["hoverCursor"] = static_cast<int>(e.hoverCursor);
    // Only once set, like every optional field above, so an element authored
    // before borders existed saves byte-identically.
    // Only when it differs from what this TYPE starts with. Writing every
    // non-zero radius would add a `cornerRadius: 6` to every Button ever saved,
    // for a value that is already the default — the same "only once set" rule
    // every other optional field above follows.
    if (const std::unique_ptr<UIElement> proto = makeUIElement(e.type());
        !proto || e.cornerRadius != proto->cornerRadius)
        o["cornerRadius"] = e.cornerRadius;
    if (e.borderWidth > 0.0f)
    {
        o["borderWidth"] = e.borderWidth;
        o["borderColor"] = { e.borderColor.r, e.borderColor.g,
                             e.borderColor.b, e.borderColor.a };
    }
    if (e.gradient)
    {
        o["gradient"]      = true;
        o["gradientColor"] = { e.gradientColor.r, e.gradientColor.g,
                               e.gradientColor.b, e.gradientColor.a };
        o["gradientAngle"] = e.gradientAngle;
    }
    e.writeJson(o); // type-specific fields
    return o;
}

// The element type is part of its identity: a Button cannot be turned into a
// Slider in place, so the caller gets a freshly constructed element of the
// stored type rather than a mutation of an existing one.
std::unique_ptr<UIElement> uiElementFromJsonObj(const nlohmann::json& o)
{
    const UIWidgetType type = uiWidgetTypeFromName(o.value("type", std::string("Panel")));
    std::unique_ptr<UIElement> e = makeUIElement(type);
    e->id       = o.value("id", 0);
    e->parentId = o.value("parent", 0);
    e->name     = o.value("name", std::string());
    if (const auto& p = o.value("pos",   nlohmann::json::array()); p.size() >= 2)
    { e->posX = p[0].get<float>(); e->posY = p[1].get<float>(); }
    if (const auto& s = o.value("size",  nlohmann::json::array()); s.size() >= 2)
    { e->sizeX = s[0].get<float>(); e->sizeY = s[1].get<float>(); }
    if (const auto& p = o.value("pivot", nlohmann::json::array()); p.size() >= 2)
    { e->pivotX = p[0].get<float>(); e->pivotY = p[1].get<float>(); }
    // An anchor rectangle wins when present; otherwise the 9-point field is
    // read as the point anchor it always named.
    uiAnchorFromLegacyPoint(*e, o.value("anchor", 0));
    if (const auto& a = o.value("anchorMin", nlohmann::json::array()); a.size() >= 2)
    { e->anchorMinX = a[0].get<float>(); e->anchorMinY = a[1].get<float>(); }
    if (const auto& a = o.value("anchorMax", nlohmann::json::array()); a.size() >= 2)
    { e->anchorMaxX = a[0].get<float>(); e->anchorMaxY = a[1].get<float>(); }
    e->layer    = o.value("layer", 0);
    e->visible  = o.value("visible", true);
    e->material = o.value("material", std::string());
    e->texture  = o.value("texture", std::string());
    e->font     = o.value("font", std::string());
    e->hitTestable   = o.value("hitTestable", true);
    e->clipChildren  = o.value("clipChildren", false);
    e->renderOpacity = o.value("renderOpacity", 1.0f);
    e->enabled       = o.value("enabled", true);
    e->slotFill      = o.value("slotFill", 0.0f);
    e->rotation      = o.value("rotation", 0.0f);
    e->hoverCursor = static_cast<HE::UICursor>(
        o.value("hoverCursor", static_cast<int>(HE::UICursor::Default)));
    // Absent = keep the TYPE's default (a Button's 6, a ComboBox's 4), not zero:
    // every widget authored before the radius was a property must still look the
    // way it looked. The element was constructed with its default just above.
    e->cornerRadius = o.value("cornerRadius", e->cornerRadius);
    e->borderWidth = o.value("borderWidth", 0.0f);
    if (const auto bc = o.find("borderColor");
        bc != o.end() && bc->is_array() && bc->size() == 4)
        e->borderColor = { (*bc)[0].get<float>(), (*bc)[1].get<float>(),
                           (*bc)[2].get<float>(), (*bc)[3].get<float>() };
    e->gradient      = o.value("gradient", false);
    e->gradientAngle = o.value("gradientAngle", 0.0f);
    if (const auto gc = o.find("gradientColor");
        gc != o.end() && gc->is_array() && gc->size() == 4)
        e->gradientColor = { (*gc)[0].get<float>(), (*gc)[1].get<float>(),
                             (*gc)[2].get<float>(), (*gc)[3].get<float>() };
    e->readJson(o); // type-specific fields
    return e;
}
} // namespace

std::string uiWidgetTreeToJson(const UIWidgetTree& tree)
{
    nlohmann::json j;
    j["canvasWidth"]  = tree.canvasWidth;
    j["canvasHeight"] = tree.canvasHeight;
    // Only written once it is not the default, so every widget authored before
    // scale modes existed saves byte-identical.
    if (tree.scaleMode != UICanvasScaleMode::Stretch)
        j["scaleMode"] = static_cast<int>(tree.scaleMode);
    j["nextId"]       = tree.nextId;

    nlohmann::json je = nlohmann::json::array();
    for (const auto& e : tree.elements) je.push_back(uiElementToJsonObj(*e));
    j["elements"] = std::move(je);
    return j.dump(2);
}

bool uiWidgetTreeFromJson(const std::string& json, UIWidgetTree& out)
{
    nlohmann::json j;
    if (!HE::graph::parseGraphObject(json, j)) return false;

    UIWidgetTree t;
    t.canvasWidth  = j.value("canvasWidth",  1920.0f);
    t.canvasHeight = j.value("canvasHeight", 1080.0f);
    {
        const int sm = j.value("scaleMode", 0);
        t.scaleMode = (sm >= 0 && sm <= static_cast<int>(UICanvasScaleMode::ConstantPixel))
            ? static_cast<UICanvasScaleMode>(sm) : UICanvasScaleMode::Stretch;
    }
    t.nextId       = j.value("nextId", 1);

    for (const auto& o : j.value("elements", nlohmann::json::array()))
    {
        std::unique_ptr<UIElement> e = uiElementFromJsonObj(o);
        HE::graph::bumpNextId(t.nextId, e->id);
        t.elements.push_back(std::move(e));
    }

    out = std::move(t);
    return true;
}

// ── Item-level JSON, public (collaboration addresses single elements) ───────
std::string uiElementToJson(const UIElement& e) { return uiElementToJsonObj(e).dump(); }

std::unique_ptr<UIElement> uiElementFromJson(const std::string& json)
{
    nlohmann::json j;
    if (!HE::graph::parseGraphObject(json, j)) return nullptr;
    return uiElementFromJsonObj(j);
}

} // namespace HE
