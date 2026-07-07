#include <UIWidget/UIWidgetTree.h>
#include <nlohmann/json.hpp>
#include <algorithm>

namespace HE {

UIWidgetNode* UIWidgetTree::findNode(int id)
{
    for (auto& n : nodes) if (n.id == id) return &n;
    return nullptr;
}

const UIWidgetNode* UIWidgetTree::findNode(int id) const
{
    for (const auto& n : nodes) if (n.id == id) return &n;
    return nullptr;
}

std::vector<int> UIWidgetTree::childrenOf(int parentId) const
{
    std::vector<int> out;
    for (const auto& n : nodes) if (n.parentId == parentId) out.push_back(n.id);
    return out;
}

bool UIWidgetTree::isDescendantOf(int id, int ancestorId) const
{
    // Walk the parent chain; depth-capped so a corrupt (cyclic) tree can't hang us.
    int cur = id;
    for (size_t guard = 0; guard <= nodes.size(); ++guard)
    {
        if (cur == ancestorId) return true;
        const UIWidgetNode* n = findNode(cur);
        if (!n) return false;
        cur = n->parentId;
        if (cur == 0) return ancestorId == 0;
    }
    return false;
}

void UIWidgetTree::removeSubtree(int id)
{
    // Collect the subtree via repeated child expansion, then erase in one pass.
    std::vector<int> doomed{ id };
    for (size_t i = 0; i < doomed.size(); ++i)
        for (int c : childrenOf(doomed[i]))
            doomed.push_back(c);
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
        [&](const UIWidgetNode& n){
            return std::find(doomed.begin(), doomed.end(), n.id) != doomed.end();
        }), nodes.end());
}

int UIWidgetTree::addNode(UIWidgetNode node)
{
    node.id = nextId++;
    nodes.push_back(std::move(node));
    return nodes.back().id;
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

UIWidgetRect uiWidgetNodeRect(const UIWidgetTree& tree, const UIWidgetNode& n)
{
    UIWidgetRect parent{ 0.0f, 0.0f, tree.canvasWidth, tree.canvasHeight };
    if (n.parentId != 0)
        if (const UIWidgetNode* p = tree.findNode(n.parentId))
            parent = uiWidgetNodeRect(tree, *p);

    float ax, ay;
    anchorPoint01(n.anchor, ax, ay);
    UIWidgetRect r;
    r.x = parent.x + ax * parent.w + n.posX - n.pivotX * n.sizeX;
    r.y = parent.y + ay * parent.h + n.posY - n.pivotY * n.sizeY;
    r.w = n.sizeX;
    r.h = n.sizeY;
    return r;
}

bool uiWidgetNodeEffectiveVisible(const UIWidgetTree& tree, const UIWidgetNode& n)
{
    if (!n.visible) return false;
    if (n.parentId == 0) return true;
    const UIWidgetNode* p = tree.findNode(n.parentId);
    return p ? uiWidgetNodeEffectiveVisible(tree, *p) : true;
}

// ── JSON ─────────────────────────────────────────────────────────────────────

namespace
{
    const char* widgetTypeName(UIWidgetType t)
    {
        switch (t)
        {
            case UIWidgetType::Panel:  return "Panel";
            case UIWidgetType::Image:  return "Image";
            case UIWidgetType::Text:   return "Text";
            case UIWidgetType::Button: return "Button";
        }
        return "Panel";
    }

    UIWidgetType widgetTypeFromName(const std::string& s)
    {
        if (s == "Image")  return UIWidgetType::Image;
        if (s == "Text")   return UIWidgetType::Text;
        if (s == "Button") return UIWidgetType::Button;
        return UIWidgetType::Panel;
    }

    nlohmann::json colorJson(const float c[4])
    {
        return nlohmann::json::array({ c[0], c[1], c[2], c[3] });
    }

    void colorFromJson(const nlohmann::json& j, float out[4])
    {
        if (!j.is_array() || j.size() < 4) return;
        for (int i = 0; i < 4; ++i)
            if (j[i].is_number()) out[i] = j[i].get<float>();
    }
}

std::string uiWidgetTreeToJson(const UIWidgetTree& tree)
{
    nlohmann::json j;
    j["canvasWidth"]  = tree.canvasWidth;
    j["canvasHeight"] = tree.canvasHeight;
    j["nextId"]       = tree.nextId;

    nlohmann::json jn = nlohmann::json::array();
    for (const auto& n : tree.nodes)
    {
        nlohmann::json e = {
            { "id",       n.id },
            { "parent",   n.parentId },
            { "type",     widgetTypeName(n.type) },  // by name → schema-evolution safe
            { "name",     n.name },
            { "pos",      { n.posX, n.posY } },
            { "size",     { n.sizeX, n.sizeY } },
            { "pivot",    { n.pivotX, n.pivotY } },
            { "anchor",   n.anchor },
            { "layer",    n.layer },
            { "visible",  n.visible },
            { "color",    colorJson(n.color) },
        };
        // Type-specific fields only when meaningful (keeps files small + readable).
        if (n.type == UIWidgetType::Text || n.type == UIWidgetType::Button)
        {
            e["text"]     = n.text;
            e["fontSize"] = n.fontSize;
        }
        if (n.type == UIWidgetType::Button)
        {
            e["hoveredColor"] = colorJson(n.hoveredColor);
            e["pressedColor"] = colorJson(n.pressedColor);
            e["textColor"]    = colorJson(n.textColor);
        }
        if (!n.materialPath.empty()) e["material"] = n.materialPath;
        if (!n.scriptPath.empty())   e["script"]   = n.scriptPath;
        jn.push_back(std::move(e));
    }
    j["nodes"] = std::move(jn);
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

    for (const auto& e : j.value("nodes", nlohmann::json::array()))
    {
        UIWidgetNode n;
        n.id       = e.value("id", 0);
        n.parentId = e.value("parent", 0);
        n.type     = widgetTypeFromName(e.value("type", std::string("Panel")));
        n.name     = e.value("name", std::string());
        if (const auto& p = e.value("pos",   nlohmann::json::array()); p.size() >= 2)
        { n.posX = p[0].get<float>(); n.posY = p[1].get<float>(); }
        if (const auto& s = e.value("size",  nlohmann::json::array()); s.size() >= 2)
        { n.sizeX = s[0].get<float>(); n.sizeY = s[1].get<float>(); }
        if (const auto& p = e.value("pivot", nlohmann::json::array()); p.size() >= 2)
        { n.pivotX = p[0].get<float>(); n.pivotY = p[1].get<float>(); }
        n.anchor   = static_cast<uint8_t>(e.value("anchor", 0));
        n.layer    = e.value("layer", 0);
        n.visible  = e.value("visible", true);
        colorFromJson(e.value("color", nlohmann::json()), n.color);
        n.text     = e.value("text", std::string());
        n.fontSize = e.value("fontSize", 14.0f);
        colorFromJson(e.value("hoveredColor", nlohmann::json()), n.hoveredColor);
        colorFromJson(e.value("pressedColor", nlohmann::json()), n.pressedColor);
        colorFromJson(e.value("textColor",    nlohmann::json()), n.textColor);
        n.materialPath = e.value("material", std::string());
        n.scriptPath   = e.value("script", std::string());

        // Keep nextId ahead of every stored id even if the field is stale.
        if (n.id >= t.nextId) t.nextId = n.id + 1;
        t.nodes.push_back(std::move(n));
    }

    out = std::move(t);
    return true;
}

} // namespace HE
