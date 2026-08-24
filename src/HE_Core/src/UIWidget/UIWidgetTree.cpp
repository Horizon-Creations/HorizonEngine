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
    const float vw = std::max(1.0f, vpWidth), vh = std::max(1.0f, vpHeight);
    const float rw = std::max(1.0f, tree.canvasWidth), rh = std::max(1.0f, tree.canvasHeight);
    const float sx = vw / rw, sy = vh / rh;

    UIWidgetCanvas c;
    if (tree.scaleMode == UICanvasScaleMode::Stretch)
    {
        // The authored canvas IS the layout canvas; the two axes take whatever
        // factor they need to cover the viewport.
        c.scaleX = sx; c.scaleY = sy;
        c.width  = rw; c.height = rh;
        return c;
    }

    float s = 1.0f;
    switch (tree.scaleMode)
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

UIWidgetRect uiElementRect(const UIWidgetTree& tree, const UIElement& e,
                           const UIWidgetCanvas* canvas)
{
    const UIWidgetRect parent = parentRectOf(tree, e, canvas);
    const float lox = parent.x + e.anchorMinX * parent.w;
    const float hix = parent.x + e.anchorMaxX * parent.w;
    const float loy = parent.y + e.anchorMinY * parent.h;
    const float hiy = parent.y + e.anchorMaxY * parent.h;

    UIWidgetRect r;
    solveAxis(lox, hix, e.posX, e.sizeX, e.pivotX, r.x, r.w);
    solveAxis(loy, hiy, e.posY, e.sizeY, e.pivotY, r.y, r.h);
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
        if (e) e->applyAutoSize(uiElementRect(tree, *e, canvas).w);
}

bool uiElementClipRect(const UIWidgetTree& tree, const UIElement& e,
                       UIWidgetRect& out, const UIWidgetCanvas* canvas)
{
    bool any = false;
    UIWidgetRect acc{};
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
    if (e.hoverCursor != HE::UICursor::Default)
        o["hoverCursor"] = static_cast<int>(e.hoverCursor);
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
    e->hitTestable  = o.value("hitTestable", true);
    e->clipChildren = o.value("clipChildren", false);
    e->hoverCursor = static_cast<HE::UICursor>(
        o.value("hoverCursor", static_cast<int>(HE::UICursor::Default)));
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
