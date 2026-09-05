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

UIWidgetCanvas uiResolveCanvas(const UIWidgetTree& tree, float vpWidth, float vpHeight,
                               float displayScale)
{
    return uiResolveCanvasFor(tree.canvasWidth, tree.canvasHeight, tree.scaleMode,
                              vpWidth, vpHeight, displayScale);
}

UIWidgetCanvas uiResolveCanvasFor(float authoredW, float authoredH, UICanvasScaleMode mode,
                                  float vpWidth, float vpHeight, float displayScale)
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
    // One unit is one device-independent pixel: on an unscaled display that is
    // one real pixel, and on a 200% one it is two, which is what keeps a
    // 14-unit label 14 units TALL to the eye instead of shrinking it by half.
    case UICanvasScaleMode::ConstantPixel: s = std::max(0.05f, displayScale); break;
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

    // ── The Grid, solved in one go ───────────────────────────────────────────
    // Where every child sits, and how wide and tall every track came out. One
    // function because the two answers depend on each other: an `auto` track is
    // as big as the children IN it, and which children those are is the
    // placement. Splitting them would mean placing twice.
    //
    // Stateless like every other container's walk — nothing is cached on the
    // grid, so nothing can be stale. It costs one pass over the siblings per
    // rect query, which is the same bargain boxSlotRect already makes.
    struct GridSolved
    {
        std::vector<float> colPos, colSize;   // host units, relative to the inner rect
        std::vector<float> rowPos, rowSize;
        std::vector<int>   who;               // element id per cell, 0 = empty
        int cols = 1, rows = 1;
        // What the fixed and `auto` tracks add up to, gaps included. Weighted
        // tracks count as nothing here, exactly as a filling child counts as
        // nothing when a box measures itself.
        float contentW = 0.0f, contentH = 0.0f;
        int cellOf(int elemId, int& col, int& row, int& cs, int& rs) const
        {
            for (int r = 0; r < rows; ++r)
                for (int c = 0; c < cols; ++c)
                    if (who[static_cast<std::size_t>(r) * cols + c] == elemId)
                    {
                        col = c; row = r;
                        // Walk the run to recover the span it was placed with.
                        cs = 1; while (col + cs < cols &&
                                       who[static_cast<std::size_t>(r) * cols + col + cs] == elemId) ++cs;
                        rs = 1; while (row + rs < rows &&
                                       who[static_cast<std::size_t>(row + rs) * cols + col] == elemId) ++rs;
                        return 1;
                    }
            return 0;
        }
    };

    GridSolved solveGrid(const UIWidgetTree& tree, const UIGrid& grid,
                         const UIWidgetCanvas* canvas)
    {
        GridSolved g;
        float us = 1.0f, vs = 1.0f;
        uiElementUnitScale(tree, grid, us, vs, canvas);

        std::vector<const UIElement*> kids;
        for (const auto& sp : tree.elements)
            if (sp && sp->parentId == grid.id && sp->visible) kids.push_back(sp.get());

        g.cols = static_cast<int>(grid.colTracks.size());
        if (g.cols < 1) g.cols = 1;

        // How many rows there have to be. Declared ones first; if the children
        // need more, the LAST declared token is repeated — a settings form with
        // twenty rows should not have to declare twenty of them.
        int needed = static_cast<int>(grid.rowTracks.size());
        if (needed < 1) needed = 1;
        {
            int cells = 0;
            for (const UIElement* k : kids)
            {
                const int cs = std::clamp(k->gridColumnSpan, 1, g.cols);
                const int rs = std::max(1, k->gridRowSpan);
                if (k->gridRow >= 0) needed = std::max(needed, k->gridRow + rs);
                else                 cells += cs * rs;
            }
            const int autoRows = (cells + g.cols - 1) / g.cols;
            needed = std::max(needed, autoRows > 0 ? autoRows : 1);
        }
        g.rows = needed;
        g.who.assign(static_cast<std::size_t>(g.rows) * g.cols, 0);

        auto fits = [&](int c, int r, int cs, int rs)
        {
            if (c < 0 || r < 0 || c + cs > g.cols || r + rs > g.rows) return false;
            for (int y = r; y < r + rs; ++y)
                for (int x = c; x < c + cs; ++x)
                    if (g.who[static_cast<std::size_t>(y) * g.cols + x] != 0) return false;
            return true;
        };
        auto mark = [&](int c, int r, int cs, int rs, int id)
        {
            for (int y = r; y < r + rs; ++y)
                for (int x = c; x < c + cs; ++x)
                    if (y < g.rows && x < g.cols)
                        g.who[static_cast<std::size_t>(y) * g.cols + x] = id;
        };

        // PINNED children first, whatever their order in the tree: an element
        // that names its cell must get it, and an automatic one that happens to
        // come earlier must not be able to take it.
        for (const UIElement* k : kids)
        {
            if (k->gridColumn < 0 || k->gridRow < 0) continue;
            const int cs = std::clamp(k->gridColumnSpan, 1, g.cols);
            const int rs = std::max(1, k->gridRowSpan);
            mark(std::min(k->gridColumn, g.cols - 1), std::min(k->gridRow, g.rows - 1),
                 cs, rs, k->id);
        }
        // …then the rest, into the next free cell, reading order.
        int cursor = 0;
        for (const UIElement* k : kids)
        {
            if (k->gridColumn >= 0 && k->gridRow >= 0) continue;
            const int cs = std::clamp(k->gridColumnSpan, 1, g.cols);
            const int rs = std::max(1, k->gridRowSpan);
            bool placed = false;
            for (int i = cursor; i < g.rows * g.cols && !placed; ++i)
            {
                const int c = i % g.cols, r = i / g.cols;
                if (!fits(c, r, cs, rs)) continue;
                mark(c, r, cs, rs, k->id);
                cursor = i + 1;
                placed = true;
            }
            // No room left: it takes no cell and gets a zero-height slot below,
            // which is visible and fixable rather than drawn on top of a
            // neighbour.
        }

        // ── Track sizes ──────────────────────────────────────────────────────
        const UIWidgetRect b = uiElementRect(tree, grid, canvas);
        const float pad  = std::max(0.0f, grid.padding);
        const float padX = pad * us, padY = pad * vs;
        const float gapX = std::max(0.0f, grid.spacing)    * us;
        const float gapY = std::max(0.0f, grid.rowSpacing) * vs;
        const float innerW = std::max(0.0f, b.w - 2.0f * padX);
        const float innerH = std::max(0.0f, b.h - 2.0f * padY);

        auto solveAxisTracks = [&](const std::vector<UIGridTrack>& tracks, int count,
                                   float avail, float gap, float unit, bool horizontal,
                                   std::vector<float>& size, std::vector<float>& pos,
                                   float& content)
        {
            size.assign(static_cast<std::size_t>(count), 0.0f);
            float fixedSum = 0.0f, weightSum = 0.0f;
            for (int i = 0; i < count; ++i)
            {
                // Rows past the declared ones repeat the last token.
                const UIGridTrack& t = tracks[static_cast<std::size_t>(
                    std::min<int>(i, static_cast<int>(tracks.size()) - 1))];
                if (t.kind == UIGridTrack::Kind::Fixed)
                { size[i] = t.value * unit; fixedSum += size[i]; }
                else if (t.kind == UIGridTrack::Kind::Auto)
                {
                    // As big as the biggest thing IN it. Only children that span
                    // ONE track contribute: a child stretched over three columns
                    // says nothing about how wide any one of them has to be.
                    float m = 0.0f;
                    for (const UIElement* k : kids)
                    {
                        int c = 0, r = 0, cs = 1, rs = 1;
                        if (!g.cellOf(k->id, c, r, cs, rs)) continue;
                        if (horizontal) { if (c != i || cs != 1) continue; m = std::max(m, k->sizeX * unit); }
                        else            { if (r != i || rs != 1) continue; m = std::max(m, k->sizeY * unit); }
                    }
                    size[i] = m; fixedSum += m;
                }
                else weightSum += t.value;
            }
            const float gaps = count > 1 ? gap * static_cast<float>(count - 1) : 0.0f;
            content = fixedSum + gaps;
            const float leftover = std::max(0.0f, avail - fixedSum - gaps);
            if (weightSum > 0.0f)
                for (int i = 0; i < count; ++i)
                {
                    const UIGridTrack& t = tracks[static_cast<std::size_t>(
                        std::min<int>(i, static_cast<int>(tracks.size()) - 1))];
                    if (t.kind == UIGridTrack::Kind::Weight)
                        size[i] = leftover * (t.value / weightSum);
                }
            pos.assign(static_cast<std::size_t>(count), 0.0f);
            float at = 0.0f;
            for (int i = 0; i < count; ++i) { pos[i] = at; at += size[i] + gap; }
        };

        solveAxisTracks(grid.colTracks, g.cols, innerW, gapX, us, true,
                        g.colSize, g.colPos, g.contentW);
        solveAxisTracks(grid.rowTracks, g.rows, innerH, gapY, vs, false,
                        g.rowSize, g.rowPos, g.contentH);
        return g;
    }

    // ── A tab's page gets everything under the strip ─────────────────────────
    // Every page, not only the active one: a page that is not showing still has
    // a real rect, so switching to it does not have to lay it out from nothing
    // and an auto-sized element inside it has a width to measure against. What
    // makes it invisible is hidesChild, and that is the only thing that does.
    UIWidgetRect tabSlotRect(const UIWidgetTree& tree, const UITabBox& tabs,
                             const UIElement& child, const UIWidgetCanvas* canvas)
    {
        const UIWidgetRect b = uiElementRect(tree, tabs, canvas);
        float bus = 1.0f, bvs = 1.0f;
        uiElementUnitScale(tree, tabs, bus, bvs, canvas);
        const float strip = std::min(b.h, tabs.tabHeight * bvs);
        (void)child;
        return { b.x, b.y + strip, b.w, std::max(0.0f, b.h - strip) };
    }

    // ── Two panes and the gap between them ───────────────────────────────────
    UIWidgetRect splitSlotRect(const UIWidgetTree& tree, const UISplitter& sp,
                               const UIElement& child, const UIWidgetCanvas* canvas)
    {
        const UIWidgetRect b = uiElementRect(tree, sp, canvas);
        float bus = 1.0f, bvs = 1.0f;
        uiElementUnitScale(tree, sp, bus, bvs, canvas);

        const float len = sp.vertical ? b.h : b.w;
        const float div = std::min(sp.dividerSize * (sp.vertical ? bvs : bus), len);
        // The SAME clamp the drag and the divider's own drawing use, in the same
        // units — a ratio the layout honoured but the divider drew elsewhere is
        // a splitter you cannot grab where you see it.
        const float first = sp.clampedRatio(len) * std::max(0.0f, len - div);

        const int idx = uiChildIndexOf(tree, sp.id, child.id);
        if (idx == 0)
            return sp.vertical ? UIWidgetRect{ b.x, b.y, b.w, first }
                               : UIWidgetRect{ b.x, b.y, first, b.h };
        if (idx == 1)
            return sp.vertical
                ? UIWidgetRect{ b.x, b.y + first + div, b.w, std::max(0.0f, len - first - div) }
                : UIWidgetRect{ b.x + first + div, b.y, std::max(0.0f, len - first - div), b.h };
        // A third child and beyond: nothing. hidesChild already keeps it off the
        // screen and out of the hit test; an empty rect is what stops it from
        // quietly influencing anything that measures.
        return { b.x, b.y, 0.0f, 0.0f };
    }

    UIWidgetRect gridSlotRect(const UIWidgetTree& tree, const UIGrid& grid,
                              const UIElement& child, const UIWidgetCanvas* canvas)
    {
        const UIWidgetRect b = uiElementRect(tree, grid, canvas);
        float us = 1.0f, vs = 1.0f;
        uiElementUnitScale(tree, grid, us, vs, canvas);
        const float pad = std::max(0.0f, grid.padding);
        const UIWidgetRect inner{ b.x + pad * us, b.y + pad * vs, 0.0f, 0.0f };

        const GridSolved g = solveGrid(tree, grid, canvas);
        int c = 0, r = 0, cs = 1, rs = 1;
        if (!g.cellOf(child.id, c, r, cs, rs)) return { inner.x, inner.y, 0.0f, 0.0f };

        const float gapX = std::max(0.0f, grid.spacing)    * us;
        const float gapY = std::max(0.0f, grid.rowSpacing) * vs;
        float w = 0.0f, h = 0.0f;
        for (int i = c; i < c + cs && i < g.cols; ++i) w += g.colSize[i];
        for (int i = r; i < r + rs && i < g.rows; ++i) h += g.rowSize[i];
        // The gaps a span swallows belong to the span: two columns plus the gap
        // between them is what "spans two" means.
        w += gapX * static_cast<float>(std::min(cs, g.cols - c) - 1);
        h += gapY * static_cast<float>(std::min(rs, g.rows - r) - 1);
        return { inner.x + g.colPos[c], inner.y + g.rowPos[r],
                 std::max(0.0f, w), std::max(0.0f, h) };
    }

    // The slot a WrapBox hands one of its children.
    //
    // A row until it cannot be one: children run along X and break to a new line
    // when the next one would not fit. Line height is the tallest child ON THAT
    // LINE, so a row of chips with one tall entry does not push every other line
    // apart as well.
    //
    // Stateless like the box's walk, and for the same reason: no cached line
    // table to invalidate, and the lists are short. `outLines` is an optional
    // by-product for the caller that wants the total height (size-to-content)
    // rather than one child's rect.
    UIWidgetRect wrapSlotRect(const UIWidgetTree& tree, const UIWrapBox& box,
                              const UIElement* child, const UIWidgetCanvas* canvas,
                              float* outContentHeight = nullptr)
    {
        const UIWidgetRect b = uiElementRect(tree, box, canvas);
        float us = 1.0f, vs = 1.0f;
        uiElementUnitScale(tree, box, us, vs, canvas);

        const float pad  = std::max(0.0f, box.padding);
        const float gapX = std::max(0.0f, box.spacing)     * us;
        const float gapY = std::max(0.0f, box.lineSpacing) * vs;
        const float padX = pad * us, padY = pad * vs;
        const UIWidgetRect inner{ b.x + padX, b.y + padY,
                                  std::max(0.0f, b.w - 2.0f * padX),
                                  std::max(0.0f, b.h - 2.0f * padY) };

        float x = inner.x, y = inner.y, lineH = 0.0f;
        UIWidgetRect found{ inner.x, inner.y, 0.0f, 0.0f };
        bool got = false;
        for (const auto& sp : tree.elements)
        {
            if (!sp || sp->parentId != box.id || !sp->visible) continue;
            const float w = sp->sizeX * us;
            const float h = sp->sizeY * vs;
            // A break only when something is already on this line: a child wider
            // than the whole box still gets its own line rather than an empty
            // one above it.
            if (x > inner.x + 0.001f && x + w > inner.x + inner.w + 0.001f)
            {
                x = inner.x;
                y += lineH + gapY;
                lineH = 0.0f;
            }
            if (child && sp->id == child->id)
            {
                found = { x, y, w, h };
                got = true;
                if (!outContentHeight) break;   // nothing left to measure
            }
            x += w + gapX;
            lineH = std::max(lineH, h);
        }
        if (outContentHeight) *outContentHeight = (y + lineH) - inner.y;
        if (!got && child) found.h = 0.0f;      // not a child of this box
        return found;
    }

    // The slot a ListView hands one of its rows.
    //
    // Deliberately NOT the walk above. A box finds a child's place by adding up
    // everything before it, which is exactly what a virtualized list cannot do:
    // the rows before this one do not exist. A row's place comes from the ITEM
    // it shows — index times step — so ten realized rows can stand anywhere in
    // ten thousand, and scrolling is one subtraction.
    UIWidgetRect listSlotRect(const UIWidgetTree& tree, const UIListView& list,
                              const UIElement& child, const UIWidgetCanvas* canvas)
    {
        const UIWidgetRect b = uiElementRect(tree, list, canvas);
        float us = 1.0f, vs = 1.0f;
        uiElementUnitScale(tree, list, us, vs, canvas);

        const float padX = std::max(0.0f, list.padding) * us;
        const float padY = std::max(0.0f, list.padding) * vs;
        UIWidgetRect out{ b.x + padX, b.y + padY,
                          std::max(0.0f, b.w - 2.0f * padX),
                          std::max(0.0f, b.h - 2.0f * padY) };

        const auto* ref = dynamic_cast<const UIWidgetRef*>(&child);
        const int   idx = ref ? ref->rowIndex : -1;
        // Anything that is not a realized row gets a zero-height slot at the top
        // rather than the whole inner rect: an element that ended up in here by
        // hand should be invisible, not a full-size sheet over the list.
        if (idx < 0) { out.h = 0.0f; return out; }

        out.y += (idx * list.rowStep() - list.scrollOffset) * vs;
        out.h  = list.rowHeight * vs;
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
            // No display scale here on purpose: r is in the HOST's canvas
            // units, and the host's own factor already carries it (see the
            // note on uiResolveCanvasFor).
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

void uiGridTracks(const UIWidgetTree& tree, const UIGrid& grid,
                  const UIWidgetCanvas* canvas,
                  std::vector<float>& outColumns, std::vector<float>& outRows)
{
    const GridSolved g = solveGrid(tree, grid, canvas);
    outColumns = g.colSize;
    outRows    = g.rowSize;
}

namespace
{
    // The floor and the ceiling, applied to the FINISHED rectangle. Every way an
    // element can get a rect runs through here, which is the point: the bound has
    // to hold for a box's child (whose size field the box ignores) and for a
    // stretched anchor (whose size field is a negative inset) just as much as for
    // an element that places itself.
    //
    // The bounds are in the element's own units, the rect is in canvas pixels, so
    // one conversion each. Where a bound bites, the element keeps its PIVOT where
    // it was — a centred element stays centred, a left-pinned one keeps its left
    // edge. And the floor is applied after the ceiling, so a max below a min
    // loses: "never smaller than this" is the promise a layout can keep.
    void applySizeBounds(const UIWidgetTree& tree, const UIElement& e,
                         const UIWidgetCanvas* canvas, UIWidgetRect& r)
    {
        if (e.minSizeX <= 0.0f && e.minSizeY <= 0.0f &&
            e.maxSizeX <= 0.0f && e.maxSizeY <= 0.0f) return;
        float us = 1.0f, vs = 1.0f;
        uiElementUnitScale(tree, e, us, vs, canvas);
        const auto bound = [](float extent, float lo, float hi)
        {
            if (hi > 0.0f && extent > hi) extent = hi;
            if (lo > 0.0f && extent < lo) extent = lo;
            return extent;
        };
        const float w = bound(r.w, e.minSizeX * us, e.maxSizeX * us);
        const float h = bound(r.h, e.minSizeY * vs, e.maxSizeY * vs);
        r.x += (r.w - w) * e.pivotX;
        r.y += (r.h - h) * e.pivotY;
        r.w = w;
        r.h = h;
    }
}

UIWidgetRect uiElementRect(const UIWidgetTree& tree, const UIElement& e,
                           const UIWidgetCanvas* canvas)
{
    UIWidgetRect r = uiElementRectUnbounded(tree, e, canvas);
    applySizeBounds(tree, e, canvas, r);
    return r;
}

// Split out so the bound is applied in exactly one place instead of at each of
// the seven ways out of this walk.
UIWidgetRect uiElementRectUnbounded(const UIWidgetTree& tree, const UIElement& e,
                                    const UIWidgetCanvas* canvas)
{
    // A child of a layout container does not place itself: the box does, and
    // the child's own anchors and position are not consulted at all.
    if (e.parentId != 0)
        if (const UIElement* p = tree.find(e.parentId); p && p->laysOutChildren())
        {
            // Three walks, because they are three different questions. A box
            // stacks along one axis; a list places by ITEM INDEX (the rows
            // before this one do not exist); a wrap box runs and breaks.
            if (const auto* lv = dynamic_cast<const UIListView*>(p))
                return listSlotRect(tree, *lv, e, canvas);
            if (const auto* wb = dynamic_cast<const UIWrapBox*>(p))
                return wrapSlotRect(tree, *wb, &e, canvas);
            if (const auto* gr = dynamic_cast<const UIGrid*>(p))
                return gridSlotRect(tree, *gr, e, canvas);
            if (const auto* tb = dynamic_cast<const UITabBox*>(p))
                return tabSlotRect(tree, *tb, e, canvas);
            if (const auto* sp = dynamic_cast<const UISplitter*>(p))
                return splitSlotRect(tree, *sp, e, canvas);
            return boxSlotRect(tree, *p, e, canvas);
        }

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

std::vector<const UIThemeStyle*> uiThemeStylesFor(const UIElement& e, const UITheme& theme)
{
    std::vector<const UIThemeStyle*> out;
    if (!e.themeStyled) return out;
    // CSS's specificity, and in CSS's order:
    //
    //   Button           a type selector      (0,0,1)   weakest
    //   Card             a class selector     (0,1,0)
    //   Button.success   both at once         (0,1,1)   strongest
    //
    // Which is why the tag comes LAST and not the name an element points at: a
    // variant of this very type says more about it than a look shared with
    // everything else that carries the same class. Getting this order from CSS
    // rather than inventing one is the point — an author who knows CSS already
    // knows what wins here.
    //
    // They LAYER, all three: a "Card" that names one colour leaves the rest to
    // the type's style, which is what a class means everywhere it exists. A name
    // that no longer resolves therefore costs the element nothing.
    const std::string type = e.typeName();
    if (const UIThemeStyle* s = theme.styleFor(type)) out.push_back(s);
    if (!e.themeStyle.empty())
        if (const UIThemeStyle* s = theme.styleFor(e.themeStyle)) out.push_back(s);
    if (!e.themeTag.empty())
        if (const UIThemeStyle* s = theme.styleFor(uiThemeSelector(type, e.themeTag)))
            out.push_back(s);
    return out;
}

std::vector<std::string> uiThemeDecidedProps(const UIElement& e, const UITheme& theme)
{
    std::vector<std::string> out;
    for (const UIThemeStyle* s : uiThemeStylesFor(e, theme))
        for (const auto& [prop, v] : s->values)
        {
            bool seen = false;
            for (const std::string& have : out) if (have == prop) seen = true;
            if (!seen) out.push_back(prop);
        }
    return out;
}

bool uiThemeValueFor(const UIElement& e, const UITheme& theme, UIThemeMode mode,
                     const std::string& prop, UIPropValue& out)
{
    const UIPropValue cur = e.getPropAny(prop);
    const std::string& bound = e.themeRoleFor(prop);

    // Decided somewhere else and locked against the theme — a component
    // parameter, most of the time. Not the same as no binding: no binding lets
    // the style answer, this one does not.
    if (bound == kUIThemeLiteral) return false;

    if (!bound.empty())
    {
        // WHAT a binding means is decided by the property's TYPE, not by the
        // stored name. One map carries all of them, so a colour, a text size and
        // a rounding are bound, saved and edited the same way.
        if (cur.type == UIPropType::Color)
        {
            const UIThemeRole role = uiThemeRoleFromName(bound);
            // A name that no longer resolves leaves the property alone rather
            // than painting it white: an element bound to a renamed role keeps
            // the last value it had, which is visible and fixable, instead of
            // vanishing. It does NOT fall through to the style either — the
            // binding is still there, it is only broken, and repainting it from
            // somewhere else would hide that.
            if (role == UIThemeRole::COUNT) return false;
            out = UIPropValue::ofColor(theme.colorFor(role, mode));
            return true;
        }
        if (cur.type == UIPropType::Float)
        {
            // WHICH scale the step is read from is the property's decision, not
            // the stored name's: all three vocabularies overlap ("Small" is in
            // two of them), so the name alone could never say. uiThemeScaleFor
            // is that rule, and the designer's role button asks the same one.
            const UIThemeScale scale = uiThemeScaleFor(prop);
            if (scale == UIThemeScale::Text)
            {
                const UIThemeTextLevel lvl = uiThemeTextLevelFromName(bound);
                if (lvl == UIThemeTextLevel::COUNT) return false;
                out = UIPropValue::ofFloat(theme.textSize[static_cast<int>(lvl)]);
                return true;
            }
            const UIThemeSize step = uiThemeSizeFromName(bound);
            if (step == UIThemeSize::COUNT) return false;
            const float* steps = scale == UIThemeScale::Spacing ? theme.spacing
                                                                : theme.radius;
            out = UIPropValue::ofFloat(steps[static_cast<int>(step)]);
            return true;
        }
        return false;
    }

    // The cascade, most specific LAST — so the last style that names this
    // property is the one that answers, and everything it does not name falls
    // through to the base underneath it.
    const UIThemeStyleValue* v = nullptr;
    for (const UIThemeStyle* s : uiThemeStylesFor(e, theme))
        if (const UIThemeStyleValue* hit = s->find(prop)) v = hit;
    if (!v) return false;
    // A style may name a property this type does not have — that is what lets
    // one named style cover a Panel and a Button at once. The type has to match
    // as well: a colour entry cannot decide a number.
    if (v->isColor && cur.type == UIPropType::Color)
    {
        const int mi = static_cast<int>(mode) < static_cast<int>(UIThemeMode::COUNT)
            ? static_cast<int>(mode) : 0;
        out = UIPropValue::ofColor(v->color[mi]);
        return true;
    }
    if (!v->isColor && cur.type == UIPropType::Float)
    {
        out = UIPropValue::ofFloat(v->number);
        return true;
    }
    return false;
}

int uiApplyTheme(UIElement& e, const UITheme& theme, UIThemeMode mode)
{
    int written = 0;
    auto write = [&](const std::string& prop)
    {
        UIPropValue v;
        if (!uiThemeValueFor(e, theme, mode, prop, v)) return;
        e.setPropAny(prop, v);
        ++written;
    };
    // Everything the element bound by hand…
    for (const auto& [prop, boundName] : e.themeRoles) write(prop);
    // …and then everything its styles decide that it did not bind itself. Each
    // property once, however many styles of the cascade named it — write() asks
    // uiThemeValueFor, which already knows which of them wins. The themeRoleFor
    // check is what keeps a bound property from being written twice, and it is
    // also why a binding beats the styles rather than the other way round.
    for (const std::string& prop : uiThemeDecidedProps(e, theme))
        if (e.themeRoleFor(prop).empty()) write(prop);
    return written;
}

int uiApplyTheme(UIWidgetTree& tree, const UITheme& theme, UIThemeMode mode)
{
    int written = 0;
    for (auto& ep : tree.elements)
        if (ep) written += uiApplyTheme(*ep, theme, mode);
    return written;
}

void uiApplyAutoSize(UIWidgetTree& tree, const UIWidgetCanvas* canvas)
{
    // What each Tab Box's strip will SAY, refreshed before anything draws. The
    // labels are the page names, so renaming a page in the designer shows up on
    // its tab without a second field to keep in step.
    //
    // A cache, and only for the drawing: which page shows and which tab a click
    // hit are both answered from the tree itself (hidesChild, tabAtPoint), so
    // the worst a stale entry can cost is a label one frame old.
    for (auto& e : tree.elements)
    {
        auto* tabs = dynamic_cast<UITabBox*>(e.get());
        if (!tabs) continue;
        tabs->tabLabels.clear();
        for (const auto& c : tree.elements)
            if (c && c->parentId == tabs->id)
                // An unnamed page still needs something on its tab, or the strip
                // grows a gap nobody can click with any confidence.
                tabs->tabLabels.push_back(c->name.empty() ? std::string("Page") : c->name);
    }

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

    // The measured extent, held between the element's floor and its ceiling. The
    // rect is bounded anyway (applySizeBounds), but the SIZE FIELD has to be too:
    // it is what a parent box adds up when it stacks its children, so a box that
    // measured itself twice as tall as its Max would push its siblings down by a
    // height it does not have.
    const auto held = [](const UIElement& e, float v, bool xAxis)
    {
        const float hi = xAxis ? e.maxSizeX : e.maxSizeY;
        const float lo = xAxis ? e.minSizeX : e.minSizeY;
        if (hi > 0.0f && v > hi) v = hi;
        return lo > 0.0f ? std::max(lo, v) : v;
    };

    for (auto& [depth, box] : boxes)
    {
        // A wrap box is measured by WRAPPING, not by stacking — and only on the
        // height. Its width is what the children are wrapped against, so
        // measuring that from them would be the question answering itself.
        if (auto* wb = dynamic_cast<UIWrapBox*>(box))
        {
            float content = 0.0f;
            wrapSlotRect(tree, *wb, nullptr, canvas, &content);
            float wus = 1.0f, wvs = 1.0f;
            uiElementUnitScale(tree, *wb, wus, wvs, canvas);
            const float pad = std::max(0.0f, wb->padding);
            wb->sizeY = held(*wb, content / std::max(1e-4f, wvs) + 2.0f * pad, false);
            continue;
        }
        // A grid is measured by its TRACKS. Fixed and `auto` ones add up;
        // weighted ones count as nothing, exactly as a filling child counts as
        // nothing when a box measures itself — a share of the leftover is what
        // this is trying to compute.
        if (auto* gr = dynamic_cast<UIGrid*>(box))
        {
            const GridSolved sg = solveGrid(tree, *gr, canvas);
            float gus = 1.0f, gvs = 1.0f;
            uiElementUnitScale(tree, *gr, gus, gvs, canvas);
            const float pad = std::max(0.0f, gr->padding);
            gr->sizeX = held(*gr, sg.contentW / std::max(1e-4f, gus) + 2.0f * pad, true);
            gr->sizeY = held(*gr, sg.contentH / std::max(1e-4f, gvs) + 2.0f * pad, false);
            continue;
        }
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
        box->sizeX = held(*box, w, true);
        box->sizeY = held(*box, h, false);
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

int uiChildIndexOf(const UIWidgetTree& tree, int parentId, int childId)
{
    int i = 0;
    for (const auto& ep : tree.elements)
    {
        if (!ep || ep->parentId != parentId) continue;
        if (ep->id == childId) return i;
        ++i;
    }
    return -1;
}

int uiChildCountOf(const UIWidgetTree& tree, int parentId)
{
    int n = 0;
    for (const auto& ep : tree.elements)
        if (ep && ep->parentId == parentId) ++n;
    return n;
}

bool uiElementEffectiveVisible(const UIWidgetTree& tree, const UIElement& e)
{
    if (!e.visible) return false;
    if (e.parentId == 0) return true;
    const UIElement* p = tree.find(e.parentId);
    if (!p) return true;
    // …and whether the PARENT is showing it. A Tab Box hides every page but
    // one, and this is the single place that has to know, because everything
    // that asks "is this on screen" — the extract, the hit test, the designer —
    // already comes through here.
    if (p->hidesChild(tree, e)) return false;
    return uiElementEffectiveVisible(tree, *p);
}

void uiUpdateScrollExtents(UIWidgetTree& tree)
{
    for (auto& bp : tree.elements)
    {
        // A list measures itself from its item COUNT, not from its children —
        // the whole point is that most of them are not there. Its clamp is the
        // scroll box's, though, so both end up at the same last line.
        if (auto* lv = dynamic_cast<UIListView*>(bp.get()))
        {
            lv->contentExtent = lv->measuredExtent();
            lv->scrollOffset  = std::clamp(lv->scrollOffset, 0.0f, lv->maxScroll());
            continue;
        }
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

// Any element that says it scrolls — the scroll box and the list view today.
// Asking the element rather than testing its type is what keeps the wheel, the
// clamp and the layout from each growing their own list of the types that move.
bool uiScrollBy(UIWidgetTree& tree, int id, float delta)
{
    UIElement* e = tree.find(id);
    float* off = e ? e->scrollOffsetPtr() : nullptr;
    if (!off) return false;
    const float maxOff = e->maxScrollAmount();
    if (maxOff <= 0.0f) return false;
    const float before = *off;
    *off = std::clamp(before + delta, 0.0f, maxOff);
    // At either end the wheel belongs to whatever is behind this box.
    return *off != before;
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
    // Written only when it says something, so every widget authored before
    // tooltips existed saves byte-identical.
    if (!e.tooltip.empty())  o["tooltip"] = e.tooltip;
    if (e.clipChildren)      o["clipChildren"] = true;
    if (e.focusFrame)        o["focusFrame"] = true;
    if (e.acceptsDrop)       o["acceptsDrop"] = true;
    if (e.draggable)         o["draggable"] = true;
    if (!e.dragPayload.empty()) o["dragPayload"] = e.dragPayload;
    if (e.renderOpacity < 1.0f) o["renderOpacity"] = e.renderOpacity;
    // Written only when there IS one, like every other addition to this list:
    // 0 is the old behaviour, so a widget that never asked for a transition
    // saves byte-identical.
    if (e.transition > 0.0f) o["transition"] = e.transition;
    // Same rule, and here it matters twice over: 0 means "take my place from the
    // tree", which is what every element authored before Tab Index existed does.
    if (e.tabIndex != 0)     o["tabIndex"] = e.tabIndex;
    if (!e.enabled)          o["enabled"] = false;
    if (e.slotFill > 0.0f)   o["slotFill"] = e.slotFill;
    // The floor and the ceiling. "minSize" is the key the four container types
    // already wrote when it lived on them, written under the same condition and
    // into the same object, so a box that carries one saves byte-identically;
    // "maxSize" is new and follows the same "only once set" rule.
    if (e.minSizeX > 0.0f || e.minSizeY > 0.0f) o["minSize"] = { e.minSizeX, e.minSizeY };
    if (e.maxSizeX > 0.0f || e.maxSizeY > 0.0f) o["maxSize"] = { e.maxSizeX, e.maxSizeY };
    // Written only when the element really sits in a named cell, so every widget
    // authored before grids existed saves byte-identical.
    if (e.gridColumn >= 0)     o["gridColumn"] = e.gridColumn;
    if (e.gridRow >= 0)        o["gridRow"]    = e.gridRow;
    if (e.gridColumnSpan > 1)  o["gridColSpan"] = e.gridColumnSpan;
    if (e.gridRowSpan > 1)     o["gridRowSpan"] = e.gridRowSpan;
    if (e.rotation != 0.0f)  o["rotation"] = e.rotation;
    if (e.hoverCursor != HE::UICursor::Default)
        o["hoverCursor"] = static_cast<int>(e.hoverCursor);
    // Only once set, like every optional field above, so an element authored
    // before borders existed saves byte-identically.
    // Only when it differs from what this TYPE starts with. Writing every
    // non-zero radius would add a `cornerRadius: 6` to every Button ever saved,
    // for a value that is already the default — the same "only once set" rule
    // every other optional field above follows.
    //
    // Three shapes, in order of how ordinary they are: unchanged writes nothing,
    // one number for all four corners keeps the ORIGINAL scalar key (so a widget
    // that only ever had one rounding still saves and loads exactly as it did),
    // and only genuinely different corners cost the four-element array.
    if (const std::unique_ptr<UIElement> proto = makeUIElement(e.type());
        !proto || e.cornerRadius != proto->cornerRadius)
    {
        if (e.uniformCornerRadius()) o["cornerRadius"] = e.cornerRadius.x;
        else o["cornerRadii"] = { e.cornerRadius.x, e.cornerRadius.y,
                                  e.cornerRadius.z, e.cornerRadius.w };
    }
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
        // Only the radial one is written: linear is what every gradient authored
        // so far is, and absent has to keep meaning exactly that.
        if (e.gradientShape == 1) o["gradientShape"] = 1;
    }
    // Both shadows follow the same "only once switched on" rule as everything
    // else here, so a widget without one saves byte-identically to before.
    if (e.shadow)
    {
        o["shadow"]        = true;
        o["shadowColor"]   = { e.shadowColor.r, e.shadowColor.g,
                               e.shadowColor.b, e.shadowColor.a };
        o["shadowBlur"]    = e.shadowBlur;
        o["shadowOffset"]  = { e.shadowOffsetX, e.shadowOffsetY };
    }
    // Only when something IS bound, like every optional field here — an element
    // that decided its own colours saves byte-identically to before themes.
    if (!e.themeRoles.empty())
    {
        nlohmann::json roles = nlohmann::json::object();
        for (const auto& [prop, role] : e.themeRoles) roles[prop] = role;
        o["themeRoles"] = std::move(roles);
    }
    // Same rule again: an element whose text is written out in one language
    // saves byte-identically to before catalogues existed.
    if (!e.textKeys.empty())
    {
        nlohmann::json keys = nlohmann::json::object();
        for (const auto& [prop, key] : e.textKeys) keys[prop] = key;
        o["textKeys"] = std::move(keys);
    }
    // Only when switched ON, like everything else optional here — so a widget
    // authored before styles existed still saves byte-identically. That the key
    // then cannot tell "predates styles" from "switched off" is fine: both mean
    // the same thing, that the theme's styles leave this element alone.
    if (e.themeStyled)
    {
        o["themeStyled"] = true;
        if (!e.themeStyle.empty()) o["themeStyle"] = e.themeStyle;
        if (!e.themeTag.empty())   o["themeTag"]   = e.themeTag;
    }
    if (e.innerShadow)
    {
        o["innerShadow"]      = true;
        o["innerShadowColor"] = { e.innerShadowColor.r, e.innerShadowColor.g,
                                  e.innerShadowColor.b, e.innerShadowColor.a };
        o["innerShadowBlur"]  = e.innerShadowBlur;
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
    e->tooltip       = o.value("tooltip", std::string());
    e->clipChildren  = o.value("clipChildren", false);
    e->focusFrame    = o.value("focusFrame", false);
    e->acceptsDrop   = o.value("acceptsDrop", false);
    e->draggable     = o.value("draggable", false);
    e->dragPayload   = o.value("dragPayload", std::string{});
    e->renderOpacity = o.value("renderOpacity", 1.0f);
    e->transition    = std::max(0.0f, o.value("transition", 0.0f));
    e->tabIndex      = o.value("tabIndex", 0);
    e->enabled       = o.value("enabled", true);
    e->slotFill      = o.value("slotFill", 0.0f);
    if (const auto& m = o.value("minSize", nlohmann::json::array()); m.size() >= 2)
    { e->minSizeX = std::max(0.0f, m[0].get<float>()); e->minSizeY = std::max(0.0f, m[1].get<float>()); }
    if (const auto& m = o.value("maxSize", nlohmann::json::array()); m.size() >= 2)
    { e->maxSizeX = std::max(0.0f, m[0].get<float>()); e->maxSizeY = std::max(0.0f, m[1].get<float>()); }
    e->gridColumn     = o.value("gridColumn", -1);
    e->gridRow        = o.value("gridRow", -1);
    e->gridColumnSpan = std::max(1, o.value("gridColSpan", 1));
    e->gridRowSpan    = std::max(1, o.value("gridRowSpan", 1));
    e->rotation      = o.value("rotation", 0.0f);
    e->hoverCursor = static_cast<HE::UICursor>(
        o.value("hoverCursor", static_cast<int>(HE::UICursor::Default)));
    // Absent = keep the TYPE's default (a Button's 6, a ComboBox's 4), not zero:
    // every widget authored before the radius was a property must still look the
    // way it looked. The element was constructed with its default just above.
    // The scalar key is the one every widget saved so far carries, and it means
    // all four corners — which is why it is still READ first and still written
    // whenever the four agree. The array is the newer, rarer shape and wins when
    // both are somehow present.
    if (const auto cr = o.find("cornerRadius"); cr != o.end() && cr->is_number())
        e->cornerRadius = glm::vec4(cr->get<float>());
    if (const auto cs = o.find("cornerRadii");
        cs != o.end() && cs->is_array() && cs->size() == 4)
        e->cornerRadius = { (*cs)[0].get<float>(), (*cs)[1].get<float>(),
                            (*cs)[2].get<float>(), (*cs)[3].get<float>() };
    e->borderWidth = o.value("borderWidth", 0.0f);
    if (const auto bc = o.find("borderColor");
        bc != o.end() && bc->is_array() && bc->size() == 4)
        e->borderColor = { (*bc)[0].get<float>(), (*bc)[1].get<float>(),
                           (*bc)[2].get<float>(), (*bc)[3].get<float>() };
    e->gradient      = o.value("gradient", false);
    e->gradientAngle = o.value("gradientAngle", 0.0f);
    e->gradientShape = o.value("gradientShape", 0) == 1 ? 1 : 0;
    e->shadow     = o.value("shadow", false);
    e->shadowBlur = o.value("shadowBlur", e->shadowBlur);
    if (const auto sc = o.find("shadowColor");
        sc != o.end() && sc->is_array() && sc->size() == 4)
        e->shadowColor = { (*sc)[0].get<float>(), (*sc)[1].get<float>(),
                           (*sc)[2].get<float>(), (*sc)[3].get<float>() };
    if (const auto so = o.find("shadowOffset");
        so != o.end() && so->is_array() && so->size() == 2)
    { e->shadowOffsetX = (*so)[0].get<float>(); e->shadowOffsetY = (*so)[1].get<float>(); }
    if (const auto roles = o.find("themeRoles"); roles != o.end() && roles->is_object())
        for (auto it = roles->begin(); it != roles->end(); ++it)
            if (it.value().is_string())
                e->setThemeRole(it.key(), it.value().get<std::string>());
    if (const auto keys = o.find("textKeys"); keys != o.end() && keys->is_object())
        for (auto it = keys->begin(); it != keys->end(); ++it)
            if (it.value().is_string())
                e->setTextKey(it.key(), it.value().get<std::string>());
    // Missing means FALSE here, against the field's own default of true: an
    // element that predates styles kept the colours somebody typed into it, and
    // opening that widget must not repaint it. New elements are constructed, not
    // read, which is where the true comes from.
    e->themeStyled = o.value("themeStyled", false);
    e->themeStyle  = o.value("themeStyle", std::string());
    e->themeTag    = o.value("themeTag", std::string());
    e->innerShadow     = o.value("innerShadow", false);
    e->innerShadowBlur = o.value("innerShadowBlur", e->innerShadowBlur);
    if (const auto ic = o.find("innerShadowColor");
        ic != o.end() && ic->is_array() && ic->size() == 4)
        e->innerShadowColor = { (*ic)[0].get<float>(), (*ic)[1].get<float>(),
                                (*ic)[2].get<float>(), (*ic)[3].get<float>() };
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
    // Only once written, so a widget that says nothing about itself saves
    // byte-identical to before descriptions existed.
    if (!tree.description.empty()) j["description"] = tree.description;
    if (!tree.themeAsset.empty())  j["themeAsset"]  = tree.themeAsset;

    // The knobs this widget offers its hosts. Only once it declares one, so a
    // page (which is every widget authored before components existed) saves
    // byte-identical. An array because the declaration order is the order a
    // host's details panel shows them in.
    if (!tree.params.empty())
    {
        nlohmann::json jp = nlohmann::json::array();
        for (const UIWidgetParam& p : tree.params)
        {
            nlohmann::json o;
            o["name"]    = p.name;
            o["element"] = p.elementId;
            o["prop"]    = p.property;
            if (!p.help.empty()) o["help"] = p.help;
            jp.push_back(std::move(o));
        }
        j["params"] = std::move(jp);
    }

    // Authored animations, and only when there are any — a widget without a
    // timeline saves byte-identically to before they existed.
    if (!tree.animations.empty())
    {
        nlohmann::json ja = nlohmann::json::array();
        for (const UIAnimClip& c : tree.animations)
        {
            nlohmann::json jc;
            jc["name"]     = c.name;
            jc["duration"] = c.duration;
            if (c.loop) jc["loop"] = true;
            nlohmann::json jt = nlohmann::json::array();
            for (const UIAnimTrack& tr : c.tracks)
            {
                nlohmann::json jtr;
                jtr["elem"] = tr.element;
                jtr["prop"] = tr.prop;
                nlohmann::json jk = nlohmann::json::array();
                for (const UIAnimKey& k : tr.keys)
                {
                    nlohmann::json jkey;
                    jkey["t"] = k.time;
                    // The value goes through the same writer every property
                    // value uses, so a key of a new type never needs a second
                    // encoder that learns about it later.
                    uiPropValueToJson(jkey["v"], k.value);
                    if (k.ease != UIEase::Linear) jkey["ease"] = uiEaseName(k.ease);
                    jk.push_back(std::move(jkey));
                }
                jtr["keys"] = std::move(jk);
                jt.push_back(std::move(jtr));
            }
            jc["tracks"] = std::move(jt);
            ja.push_back(std::move(jc));
        }
        j["animations"] = std::move(ja);
    }

    nlohmann::json je = nlohmann::json::array();
    for (const auto& e : tree.elements) je.push_back(uiElementToJsonObj(*e));
    j["elements"] = std::move(je);
    return j.dump(2);
}

int uiApplyWidgetParams(UIWidgetTree& tree,
                        const std::vector<std::pair<std::string, UIPropValue>>& values)
{
    int written = 0;
    for (const auto& [name, value] : values)
    {
        // Which declaration this value belongs to. Nothing found = the
        // component's author renamed or dropped that knob; see the header for
        // why that is a drop and not a guess.
        const UIWidgetParam* decl = nullptr;
        for (const UIWidgetParam& p : tree.params)
            if (p.name == name) { decl = &p; break; }
        if (!decl) continue;

        UIElement* e = tree.find(decl->elementId);
        if (!e) continue;

        // Written through the same setPropAny every other writer uses, so a
        // property with a custom slot (a Grid's track list re-parses on write)
        // does its work here too — a parameter must not be a second, quieter
        // way into a field.
        //
        // The stored value carries its own type; the property has one too, and
        // they can disagree once a component's author changes a Text into a
        // number. The property's type wins, because it is the one the element
        // actually reads.
        const UIPropValue cur = e->getPropAny(decl->property);
        e->setPropAny(decl->property, uiPropValueCoerce(value, cur.type));
        // A parameter and the theme would otherwise fight every time the theme
        // changes, with the theme winning silently a frame later. Saying it
        // once, here, is the whole resolution: telling a component what colour
        // to be takes that colour out of the theme's hands.
        //
        // LOCKED, not merely un-bound. Un-binding was enough while only a role
        // could answer for a property; a style answers for properties nobody
        // bound, so an un-bound parameter would be painted over by the style of
        // the element's type at the next uiApplyTheme.
        e->setThemeRole(decl->property, kUIThemeLiteral);
        ++written;
    }
    return written;
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
    t.description  = j.value("description", std::string());
    t.themeAsset   = j.value("themeAsset", std::string());

    if (const auto jp = j.find("params"); jp != j.end() && jp->is_array())
        for (const auto& o : *jp)
        {
            if (!o.is_object()) continue;
            UIWidgetParam p;
            p.name      = o.value("name", std::string());
            p.elementId = o.value("element", 0);
            p.property  = o.value("prop", std::string());
            p.help      = o.value("help", std::string());
            // A declaration missing either half names nothing and would be a
            // row in the host's panel that writes into the void.
            if (p.name.empty() || p.property.empty() || p.elementId <= 0) continue;
            t.params.push_back(std::move(p));
        }

    if (const auto ja = j.find("animations"); ja != j.end() && ja->is_array())
        for (const auto& jc : *ja)
        {
            if (!jc.is_object()) continue;
            UIAnimClip c;
            c.name     = jc.value("name", std::string());
            c.duration = jc.value("duration", 1.0f);
            c.loop     = jc.value("loop", false);
            // A clip with no name cannot be played — the name IS the identity —
            // and a duration of zero is a timeline with no length.
            if (c.name.empty() || c.duration <= 0.0f) continue;
            if (const auto jt = jc.find("tracks"); jt != jc.end() && jt->is_array())
                for (const auto& jtr : *jt)
                {
                    if (!jtr.is_object()) continue;
                    UIAnimTrack tr;
                    tr.element = jtr.value("elem", 0);
                    tr.prop    = jtr.value("prop", std::string());
                    if (tr.element <= 0 || tr.prop.empty()) continue;
                    if (const auto jk = jtr.find("keys"); jk != jtr.end() && jk->is_array())
                        for (const auto& jkey : *jk)
                        {
                            if (!jkey.is_object() || !jkey.contains("v")) continue;
                            UIAnimKey k;
                            k.time  = jkey.value("t", 0.0f);
                            k.value = uiPropValueFromJson(jkey["v"]);
                            k.ease  = uiEaseFromName(jkey.value("ease", std::string()));
                            tr.keys.push_back(std::move(k));
                        }
                    // A track with no keys says nothing; dropping it here keeps
                    // the editor from showing a row that cannot be evaluated.
                    if (!tr.keys.empty()) c.tracks.push_back(std::move(tr));
                }
            t.animations.push_back(std::move(c));
        }

    // ── Migration: a Button's caption becomes a Text child ───────────────────
    // A Button used to draw its own centred string. It is a surface now, and
    // what sits on it is made of children — so a caption authored under the old
    // rule is turned into one here rather than being dropped.
    //
    // Collected first and added after the loop: appending to t.elements while
    // iterating the JSON is fine, but the new children need ids from t.nextId,
    // which is only final once every stored id has been seen.
    struct LegacyCaption
    {
        int         buttonId;
        std::string text;
        float       fontSize;
        glm::vec4   color;
    };
    std::vector<LegacyCaption> captions;

    for (const auto& o : j.value("elements", nlohmann::json::array()))
    {
        std::unique_ptr<UIElement> e = uiElementFromJsonObj(o);
        HE::graph::bumpNextId(t.nextId, e->id);
        // Keyed on the legacy JSON KEY, not on any field: once this widget has
        // been saved again the key is gone and the migration is inert, which is
        // what stops it from running twice and stacking up labels.
        if (e->type() == UIWidgetType::Button)
            if (const auto txt = o.find("text");
                txt != o.end() && txt->is_string() && !txt->get<std::string>().empty())
            {
                glm::vec4 tc{ 1.0f, 1.0f, 1.0f, 1.0f };
                if (const auto c = o.find("textColor");
                    c != o.end() && c->is_array() && c->size() == 4)
                    tc = { (*c)[0].get<float>(), (*c)[1].get<float>(),
                           (*c)[2].get<float>(), (*c)[3].get<float>() };
                captions.push_back({ e->id, txt->get<std::string>(),
                                     o.value("fontSize", 20.0f), tc });
            }
        t.elements.push_back(std::move(e));
    }

    for (const LegacyCaption& lc : captions)
    {
        auto label = makeUIElement(UIWidgetType::Text);
        label->id       = t.nextId++;
        label->parentId = lc.buttonId;
        label->name     = "Label";
        // Centred and stretched across the button, which is exactly where the
        // built-in caption used to be drawn.
        uiSetAnchorPreset(*label, 15);
        label->posX = label->posY = 0.0f;
        label->sizeX = label->sizeY = 0.0f;
        // A caption must never swallow the click meant for the button under it.
        label->hitTestable = false;
        if (auto* txt = dynamic_cast<UIText*>(label.get()))
        {
            txt->text     = lc.text;
            txt->fontSize = lc.fontSize;
            txt->color    = lc.color;
            // Centred both ways — exactly where the built-in caption was drawn.
            txt->alignH   = 1;
            txt->alignV   = 1;
            // Auto-size would shrink the label to its own text and undo the
            // stretch that centres it in the button.
            txt->autoSize = false;
        }
        t.elements.push_back(std::move(label));
    }

    out = std::move(t);
    return true;
}

// ── The text catalog ─────────────────────────────────────────────────────────
const std::string* UITextCatalog::find(const std::string& lang,
                                       const std::string& key) const
{
    const auto look = [&](const std::string& l) -> const std::string*
    {
        for (const auto& [name, entries] : languages)
        {
            if (name != l) continue;
            for (const auto& [k, text] : entries) if (k == key) return &text;
            return nullptr;   // the language is here and does not have the key
        }
        return nullptr;
    };
    if (const std::string* s = look(lang)) return s;
    return lang == fallback ? nullptr : look(fallback);
}

void UITextCatalog::set(const std::string& lang, const std::string& key,
                        const std::string& text)
{
    for (auto& [name, entries] : languages)
        if (name == lang)
        {
            for (auto& [k, v] : entries) if (k == key) { v = text; return; }
            entries.emplace_back(key, text);
            return;
        }
    languages.push_back({ lang, { { key, text } } });
}

std::vector<std::string> UITextCatalog::languageNames() const
{
    std::vector<std::string> out;
    out.reserve(languages.size());
    for (const auto& [name, entries] : languages) { (void)entries; out.push_back(name); }
    return out;
}

std::string uiTextCatalogToJson(const UITextCatalog& c)
{
    nlohmann::json o = nlohmann::json::object();
    o["fallback"] = c.fallback;
    // An ARRAY of languages, each an array of pairs: nlohmann sorts the keys of
    // an object, and this file is read by people who translate it — losing the
    // author's order would reshuffle every language on every save.
    nlohmann::json langs = nlohmann::json::array();
    for (const auto& [name, entries] : c.languages)
    {
        nlohmann::json l = nlohmann::json::object();
        l["language"] = name;
        nlohmann::json items = nlohmann::json::array();
        for (const auto& [k, text] : entries)
            items.push_back(nlohmann::json::object({ { "key", k }, { "text", text } }));
        l["entries"] = std::move(items);
        langs.push_back(std::move(l));
    }
    o["languages"] = std::move(langs);
    return o.dump(2);
}

bool uiTextCatalogFromJson(const std::string& json, UITextCatalog& out)
{
    nlohmann::json j;
    if (!HE::graph::parseGraphObject(json, j)) return false;
    UITextCatalog c;
    c.fallback = j.value("fallback", std::string("en"));
    if (const auto langs = j.find("languages"); langs != j.end() && langs->is_array())
        for (const auto& l : *langs)
        {
            if (!l.is_object()) continue;
            const std::string name = l.value("language", std::string());
            if (name.empty()) continue;
            const auto entries = l.find("entries");
            if (entries == l.end() || !entries->is_array()) continue;
            for (const auto& item : *entries)
            {
                if (!item.is_object()) continue;
                const std::string k = item.value("key", std::string());
                if (k.empty()) continue;
                c.set(name, k, item.value("text", std::string()));
            }
        }
    out = std::move(c);
    return true;
}

int uiApplyTextCatalog(UIElement& e, const UITextCatalog& c, const std::string& lang)
{
    int written = 0;
    for (const auto& [prop, key] : e.textKeys)
    {
        const std::string* text = c.find(lang, key);
        if (!text) continue;
        // Only String properties: a key bound to a colour is an authoring
        // mistake, and writing the translation into it through a coercion would
        // turn that mistake into a black label nobody can explain.
        const UIPropValue cur = e.getPropAny(prop);
        if (cur.type != UIPropType::String) continue;
        if (cur.s == *text) continue;   // already says it; not a change to redraw
        e.setPropAny(prop, UIPropValue::ofString(*text));
        ++written;
    }
    return written;
}

int uiApplyTextCatalog(UIWidgetTree& tree, const UITextCatalog& c, const std::string& lang)
{
    int written = 0;
    for (auto& ep : tree.elements)
        if (ep) written += uiApplyTextCatalog(*ep, c, lang);
    return written;
}

// ── Contrast (WCAG 2.1) ──────────────────────────────────────────────────────
namespace
{
float srgbToLinear(float c)
{
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// Which property of an element is its SURFACE — the thing anything drawn on it
// stands on. Null for the types that paint no pixel: a vertical box, a spacer,
// a label. They are transparent, so the question passes straight through them
// to whatever is behind, which is exactly what the walk below wants.
//
// A per-type answer rather than a list of likely names, because the wrong guess
// is silent: a Slider's "Track Color" and a CheckBox's "Box Color" are surfaces
// of a PART, not of the element, and measuring a label against one of them
// would report a number about a place no text ever appears.
const char* surfacePropOf(const UIElement& e)
{
    switch (e.type())
    {
    case UIWidgetType::Panel:       return "Color";
    case UIWidgetType::Image:       return "Tint";
    case UIWidgetType::Button:      return "Normal Color";
    case UIWidgetType::TextInput:   return "Back Color";
    case UIWidgetType::ComboBox:    return "Back Color";
    case UIWidgetType::ListView:    return "Back Color";
    case UIWidgetType::ProgressBar: return "Back Color";
    case UIWidgetType::TabBox:      return "Tab Color";
    default:                        return nullptr;
    }
}

// …and which one carries text that has to be read.
const char* textPropOf(const UIElement& e)
{
    switch (e.type())
    {
    case UIWidgetType::Text:      return "Color";
    case UIWidgetType::CheckBox:  return "Text Color";
    case UIWidgetType::TextInput: return "Text Color";
    case UIWidgetType::ComboBox:  return "Text Color";
    case UIWidgetType::TabBox:    return "Text Color";
    default:                      return nullptr;
    }
}

glm::vec4 colorProp(const UIElement& e, const char* name)
{
    const UIPropValue v = e.getPropAny(name);
    return v.type == UIPropType::Color ? v.col : glm::vec4(0.0f);
}

// `src` laid over `dst`. Both sRGB, and the mix is done in sRGB on purpose:
// this asks what an 8-bit framebuffer ends up holding, and that is what every
// backend's "over" blend writes into it.
glm::vec4 over(const glm::vec4& src, const glm::vec4& dst)
{
    const float a = std::clamp(src.a, 0.0f, 1.0f);
    return glm::vec4(glm::mix(glm::vec3(dst), glm::vec3(src), a), 1.0f);
}
} // namespace

float uiRelativeLuminance(const glm::vec4& srgb)
{
    return 0.2126f * srgbToLinear(srgb.r)
         + 0.7152f * srgbToLinear(srgb.g)
         + 0.0722f * srgbToLinear(srgb.b);
}

float uiContrastRatio(const glm::vec4& a, const glm::vec4& b)
{
    const float la = uiRelativeLuminance(a), lb = uiRelativeLuminance(b);
    const float hi = std::max(la, lb), lo = std::min(la, lb);
    return (hi + 0.05f) / (lo + 0.05f);
}

std::vector<UIContrastFinding> uiCheckContrast(const UIWidgetTree& tree,
                                               const glm::vec4& backdrop,
                                               float minRatio)
{
    std::vector<UIContrastFinding> out;
    for (const auto& ep : tree.elements)
    {
        if (!ep) continue;
        const UIElement& e = *ep;
        const char* tp = textPropOf(e);
        if (!tp) continue;

        // What this text stands on. Its own surface when it has one (a field
        // paints its own background), else the nearest ancestor that has one,
        // else the page. Everything between is transparent and composited in
        // order, so a tinted overlay over a dark panel is measured as what the
        // eye gets and not as either of the two.
        int againstId = 0;
        const char* againstProp = nullptr;
        std::vector<const UIElement*> chain;
        int guard = 0;
        for (const UIElement* c = &e;
             c && guard++ <= static_cast<int>(tree.elements.size());)
        {
            if (const char* sp = surfacePropOf(*c))
            {
                chain.push_back(c);
                // The lowest one is what the finding names: the surface the
                // text is actually written on, whatever is stacked behind it.
                if (!againstProp) { againstId = c->id; againstProp = sp; }
                // Opaque: nothing behind it can show through, so stop.
                if (colorProp(*c, sp).a >= 0.999f) break;
            }
            c = c->parentId != 0 ? tree.find(c->parentId) : nullptr;
        }
        glm::vec4 back = backdrop;
        back.a = 1.0f;
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
            back = over(colorProp(**it, surfacePropOf(**it)), back);

        const glm::vec4 authored = colorProp(e, tp);
        const glm::vec4 text = over(authored, back);
        const float ratio = uiContrastRatio(text, back);

        // WCAG's own exception: large text is legible at a lower ratio, and 24
        // px is where it puts the line. Scaled rather than hard-coded to 3, so
        // a project that asks for AAA (7) gets AAA's large-text bar (4.5) too.
        const UIPropValue fs = e.getPropAny("FontSize");
        const float fontSize = fs.type == UIPropType::Float ? fs.f : 0.0f;
        const float required = fontSize >= 24.0f ? minRatio * (3.0f / 4.5f) : minRatio;
        if (ratio >= required) continue;

        out.push_back({ e.id, tp, againstId, againstProp ? againstProp : "",
                        text, back, ratio, required, fontSize });
    }
    return out;
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
