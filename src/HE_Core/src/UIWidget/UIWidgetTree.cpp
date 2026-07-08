#include <UIWidget/UIWidgetTree.h>
#include <UIWidget/UIElements.h>
#include <nlohmann/json.hpp>
#include <algorithm>

namespace HE {

UIElement* UIWidgetTree::find(int id)
{
    for (auto& e : elements) if (e->id == id) return e.get();
    return nullptr;
}
const UIElement* UIWidgetTree::find(int id) const
{
    for (const auto& e : elements) if (e->id == id) return e.get();
    return nullptr;
}

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
    e->id = nextId++;
    const int id = e->id;
    elements.push_back(std::move(e));
    return id;
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
    void anchorPoint01(uint8_t a, float& ax, float& ay)
    {
        static const float pts[9][2] = {
            {0.0f,0.0f},{0.5f,0.0f},{1.0f,0.0f},
            {0.0f,0.5f},{0.5f,0.5f},{1.0f,0.5f},
            {0.0f,1.0f},{0.5f,1.0f},{1.0f,1.0f} };
        const int i = a > 8 ? 0 : a;
        ax = pts[i][0]; ay = pts[i][1];
    }
}

UIWidgetRect uiElementRect(const UIWidgetTree& tree, const UIElement& e)
{
    UIWidgetRect parent{ 0.0f, 0.0f, tree.canvasWidth, tree.canvasHeight };
    if (e.parentId != 0)
        if (const UIElement* p = tree.find(e.parentId))
            parent = uiElementRect(tree, *p);

    float ax, ay;
    anchorPoint01(e.anchor, ax, ay);
    UIWidgetRect r;
    r.x = parent.x + ax * parent.w + e.posX - e.pivotX * e.sizeX;
    r.y = parent.y + ay * parent.h + e.posY - e.pivotY * e.sizeY;
    r.w = e.sizeX;
    r.h = e.sizeY;
    return r;
}

bool uiElementEffectiveVisible(const UIWidgetTree& tree, const UIElement& e)
{
    if (!e.visible) return false;
    if (e.parentId == 0) return true;
    const UIElement* p = tree.find(e.parentId);
    return p ? uiElementEffectiveVisible(tree, *p) : true;
}

// ── JSON ─────────────────────────────────────────────────────────────────────

std::string uiWidgetTreeToJson(const UIWidgetTree& tree)
{
    nlohmann::json j;
    j["canvasWidth"]  = tree.canvasWidth;
    j["canvasHeight"] = tree.canvasHeight;
    j["nextId"]       = tree.nextId;

    nlohmann::json je = nlohmann::json::array();
    for (const auto& e : tree.elements)
    {
        nlohmann::json o = {
            { "id",      e->id },
            { "parent",  e->parentId },
            { "type",    e->typeName() },  // by name → schema-evolution safe
            { "name",    e->name },
            { "pos",     { e->posX, e->posY } },
            { "size",    { e->sizeX, e->sizeY } },
            { "pivot",   { e->pivotX, e->pivotY } },
            { "anchor",  e->anchor },
            { "layer",   e->layer },
            { "visible", e->visible },
        };
        if (!e->material.empty()) o["material"] = e->material;
        if (!e->font.empty())     o["font"]     = e->font;
        e->writeJson(o); // type-specific fields
        je.push_back(std::move(o));
    }
    j["elements"] = std::move(je);
    return j.dump(2);
}

bool uiWidgetTreeFromJson(const std::string& json, UIWidgetTree& out)
{
    nlohmann::json j = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return false;

    UIWidgetTree t;
    t.canvasWidth  = j.value("canvasWidth",  1920.0f);
    t.canvasHeight = j.value("canvasHeight", 1080.0f);
    t.nextId       = j.value("nextId", 1);

    for (const auto& o : j.value("elements", nlohmann::json::array()))
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
        e->anchor   = static_cast<uint8_t>(o.value("anchor", 0));
        e->layer    = o.value("layer", 0);
        e->visible  = o.value("visible", true);
        e->material = o.value("material", std::string());
        e->font     = o.value("font", std::string());
        e->readJson(o); // type-specific fields

        if (e->id >= t.nextId) t.nextId = e->id + 1;
        t.elements.push_back(std::move(e));
    }

    out = std::move(t);
    return true;
}

} // namespace HE
