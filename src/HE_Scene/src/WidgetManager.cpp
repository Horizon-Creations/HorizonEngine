#include <HorizonScene/WidgetManager.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <Renderer/UIFont.h>
#include <Diagnostics/Logger.h>
#include <algorithm>

namespace
{
    // Sort key inside one widget: layer (major) + nesting depth (minor), the
    // same rule the entity UI path uses — children draw over their parents.
    int nodeSortKey(const HE::UIWidgetTree& tree, const HE::UIWidgetNode& n)
    {
        int depth = 0;
        for (const HE::UIWidgetNode* c = &n; c->parentId != 0 && depth < 255; ++depth)
        {
            const HE::UIWidgetNode* p = tree.findNode(c->parentId);
            if (!p) break;
            c = p;
        }
        return n.layer * 256 + depth;
    }

    glm::vec4 toVec4(const float c[4]) { return { c[0], c[1], c[2], c[3] }; }
}

WidgetManager::Instance* WidgetManager::find(int id)
{
    for (auto& w : m_instances) if (w.id == id) return &w;
    return nullptr;
}
const WidgetManager::Instance* WidgetManager::find(int id) const
{
    for (const auto& w : m_instances) if (w.id == id) return &w;
    return nullptr;
}

int WidgetManager::createWidget(ContentManager& content, const std::string& assetPath)
{
    const HE::UUID assetId = content.loadAsset(assetPath);
    const UIWidgetAsset* asset = content.getWidget(assetId);
    if (!asset)
    {
        Logger::Log(Logger::LogLevel::Warning,
            ("WidgetManager: widget asset not found: " + assetPath).c_str());
        return 0;
    }

    Instance w;
    if (!HE::uiWidgetTreeFromJson(asset->treeJson, w.tree))
    {
        Logger::Log(Logger::LogLevel::Error,
            ("WidgetManager: invalid widget tree JSON in " + assetPath).c_str());
        return 0;
    }
    if (!asset->graphJson.empty())
        HE::uiWidgetGraphFromJson(asset->graphJson, w.graph); // absent/broken → no logic

    // Resolve per-node material references once (paths → UUIDs).
    for (const auto& n : w.tree.nodes)
        if (!n.materialPath.empty())
        {
            const HE::UUID mid = content.loadAsset(n.materialPath);
            if (mid != HE::UUID{}) w.materials[n.id] = mid;
        }

    w.id = m_nextId++;
    m_instances.push_back(std::move(w));

    Instance& stored = m_instances.back();
    HE::UIWidgetGraphRunner runner(stored.graph, stored.tree, stored.self);
    runner.fireEvent(HE::UIWidgetEvent::Construct);
    stored.constructed = true;
    return stored.id;
}

void WidgetManager::destroyWidget(int id)
{
    m_instances.erase(std::remove_if(m_instances.begin(), m_instances.end(),
        [&](const Instance& w){ return w.id == id; }), m_instances.end());
}

void WidgetManager::showWidget(int id)  { if (Instance* w = find(id)) w->self.visible = true; }
void WidgetManager::hideWidget(int id)  { if (Instance* w = find(id)) w->self.visible = false; }
void WidgetManager::setZOrder(int id, int z) { if (Instance* w = find(id)) w->zOrder = z; }

bool WidgetManager::isAlive(int id) const   { return find(id) != nullptr; }
bool WidgetManager::isVisible(int id) const
{
    const Instance* w = find(id);
    return w && w->self.visible;
}
int WidgetManager::zOrder(int id) const
{
    const Instance* w = find(id);
    return w ? w->zOrder : 0;
}

bool WidgetManager::callFunction(int id, const std::string& name)
{
    Instance* w = find(id);
    if (!w) return false;
    HE::UIWidgetGraphRunner runner(w->graph, w->tree, w->self);
    return runner.callFunction(name, /*requirePublic=*/true);
}

void WidgetManager::tick(float dt)
{
    for (auto& w : m_instances)
    {
        if (!w.self.visible) continue;
        HE::UIWidgetGraphRunner runner(w.graph, w.tree, w.self);
        runner.fireEvent(HE::UIWidgetEvent::Tick, 0, dt);
    }
}

bool WidgetManager::isInteractive(const Instance& w, const HE::UIWidgetNode& n)
{
    if (n.type == HE::UIWidgetType::Button) return true;
    for (const auto& gn : w.graph.nodes)
    {
        const bool pointerEvent =
            gn.type == HE::UIGraphNodeType::EventClick ||
            gn.type == HE::UIGraphNodeType::EventHoverEnter ||
            gn.type == HE::UIGraphNodeType::EventHoverExit;
        if (pointerEvent && (gn.elem == 0 || gn.elem == n.id))
            return true;
    }
    return false;
}

bool WidgetManager::processPointer(float vpWidth, float vpHeight,
                                   float mouseX, float mouseY,
                                   bool primaryDown, bool valid)
{
    // Topmost interactive element under the pointer, across all visible
    // widgets: highest (widget zOrder, element sort key) wins.
    Instance* topW = nullptr;
    int topNode = 0;
    long topKey = 0;
    if (valid)
    {
        for (auto& w : m_instances)
        {
            if (!w.self.visible) continue;
            const float sx = vpWidth  / w.tree.canvasWidth;
            const float sy = vpHeight / w.tree.canvasHeight;
            for (const auto& n : w.tree.nodes)
            {
                if (!HE::uiWidgetNodeEffectiveVisible(w.tree, n)) continue;
                if (!isInteractive(w, n)) continue;
                const HE::UIWidgetRect r = HE::uiWidgetNodeRect(w.tree, n);
                const float x0 = r.x * sx, y0 = r.y * sy;
                const float x1 = (r.x + r.w) * sx, y1 = (r.y + r.h) * sy;
                if (mouseX < x0 || mouseX > x1 || mouseY < y0 || mouseY > y1)
                    continue;
                const long key = (long)w.zOrder * 1000000 + nodeSortKey(w.tree, n);
                if (!topW || key >= topKey)
                {
                    topW = &w; topNode = n.id; topKey = key;
                }
            }
        }
    }

    const bool pressEdge   = primaryDown && !m_wasDown;
    const bool releaseEdge = !primaryDown && m_wasDown;

    for (auto& w : m_instances)
    {
        const bool isTop = topW == &w;
        const int  hot   = isTop ? topNode : 0;

        // Hover transitions per instance.
        if (w.hoveredNode != hot)
        {
            HE::UIWidgetGraphRunner runner(w.graph, w.tree, w.self);
            if (w.hoveredNode != 0)
                runner.fireEvent(HE::UIWidgetEvent::HoverExit, w.hoveredNode);
            if (hot != 0)
                runner.fireEvent(HE::UIWidgetEvent::HoverEnter, hot);
            w.hoveredNode = hot;
        }

        if (pressEdge) w.pressedNode = hot;
        if (releaseEdge)
        {
            if (w.pressedNode != 0 && w.pressedNode == hot)
            {
                HE::UIWidgetGraphRunner runner(w.graph, w.tree, w.self);
                runner.fireEvent(HE::UIWidgetEvent::Click, hot);
            }
            w.pressedNode = 0;
        }

        // Button visual states.
        w.buttonState.clear();
        if (w.hoveredNode != 0)
            w.buttonState[w.hoveredNode] =
                (w.pressedNode == w.hoveredNode && primaryDown) ? 2 : 1;
    }

    m_wasDown = primaryDown;
    return topW != nullptr;
}

void WidgetManager::extract(float vpWidth, float vpHeight, std::vector<UIRenderObject>& out)
{
    // Widgets sorted by zOrder (stable: creation order breaks ties).
    std::vector<Instance*> sorted;
    sorted.reserve(m_instances.size());
    for (auto& w : m_instances)
        if (w.self.visible) sorted.push_back(&w);
    std::stable_sort(sorted.begin(), sorted.end(),
        [](const Instance* a, const Instance* b){ return a->zOrder < b->zOrder; });

    for (Instance* wp : sorted)
    {
        Instance& w = *wp;
        const float sx = vpWidth  / w.tree.canvasWidth;
        const float sy = vpHeight / w.tree.canvasHeight;

        // Draw items of this widget, painter-ordered by (layer, depth).
        struct Item { const HE::UIWidgetNode* n; int key; HE::UIWidgetRect r; };
        std::vector<Item> items;
        for (const auto& n : w.tree.nodes)
        {
            if (!HE::uiWidgetNodeEffectiveVisible(w.tree, n)) continue;
            items.push_back({ &n, nodeSortKey(w.tree, n), HE::uiWidgetNodeRect(w.tree, n) });
        }
        std::stable_sort(items.begin(), items.end(),
            [](const Item& a, const Item& b){ return a.key < b.key; });

        for (const Item& it : items)
        {
            const HE::UIWidgetNode& n = *it.n;
            const glm::vec2 pos { it.r.x * sx, it.r.y * sy };
            const glm::vec2 size{ it.r.w * sx, it.r.h * sy };

            const auto matIt = w.materials.find(n.id);
            const HE::UUID matId = matIt != w.materials.end() ? matIt->second : HE::UUID{};

            switch (n.type)
            {
            case HE::UIWidgetType::Panel:
            case HE::UIWidgetType::Image:
            {
                UIRenderObject ro;
                ro.position        = pos;
                ro.size            = size;
                ro.color           = toVec4(n.color);
                ro.materialAssetId = matId;
                ro.type            = 0;
                out.push_back(std::move(ro));
                break;
            }
            case HE::UIWidgetType::Button:
            {
                const uint8_t state = [&]{
                    auto s = w.buttonState.find(n.id);
                    return s != w.buttonState.end() ? s->second : uint8_t(0);
                }();
                glm::vec4 col = toVec4(n.color);
                if (state == 1) col = toVec4(n.hoveredColor);
                if (state == 2) col = toVec4(n.pressedColor);

                UIRenderObject ro;
                ro.position        = pos;
                ro.size            = size;
                ro.color           = col;
                ro.materialAssetId = matId;
                ro.type            = 0;
                out.push_back(std::move(ro));

                if (!n.text.empty())
                    HE::emitUITextGlyphs(n.text, pos, size, n.fontSize * sy,
                                         toVec4(n.textColor), 0, /*centerH=*/true, out);
                break;
            }
            case HE::UIWidgetType::Text:
                HE::emitUITextGlyphs(n.text, pos, size, n.fontSize * sy,
                                     toVec4(n.color), 0, /*centerH=*/false, out);
                break;
            }
        }
    }
}
