#include <HorizonScene/UIWidgetInstantiator.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/UIWidgetComponent.h>
#include <HorizonScene/Components/UICanvasComponent.h>
#include <HorizonScene/Components/UIElementComponent.h>
#include <HorizonScene/Components/UITextComponent.h>
#include <HorizonScene/Components/UIImageComponent.h>
#include <HorizonScene/Components/UIButtonComponent.h>
#include <HorizonScene/Components/ScriptComponent.h>
#include <UIWidget/UIWidgetTree.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <Diagnostics/Logger.h>

#include <filesystem>

namespace
{
    glm::vec4 toVec4(const float c[4]) { return { c[0], c[1], c[2], c[3] }; }

    // Resolve a content-relative asset path to a UUID (loads on demand).
    // Empty path or failed load → nil UUID.
    HE::UUID resolvePath(ContentManager& content, const std::string& path)
    {
        if (path.empty()) return HE::UUID{};
        return content.loadAsset(path);
    }

    void spawnNode(HorizonWorld& world, ContentManager& content,
                   const HE::UIWidgetTree& tree, const HE::UIWidgetNode& node,
                   Entity parent, std::vector<Entity>& spawned)
    {
        auto& reg = world.registry();

        const char* fallback =
            node.type == HE::UIWidgetType::Image  ? "Image"  :
            node.type == HE::UIWidgetType::Text   ? "Text"   :
            node.type == HE::UIWidgetType::Button ? "Button" : "Panel";
        Entity e = world.createEntity(node.name.empty() ? fallback : node.name);
        world.reparentEntity(e, parent);
        spawned.push_back(e);

        UIElementComponent elem;
        elem.position = { node.posX, node.posY };
        elem.size     = { node.sizeX, node.sizeY };
        elem.pivot    = { node.pivotX, node.pivotY };
        elem.anchor   = static_cast<UIAnchor>(node.anchor > 8 ? 0 : node.anchor);
        elem.layer    = node.layer;
        elem.active   = node.visible;
        reg.emplace<UIElementComponent>(e, elem);

        switch (node.type)
        {
        case HE::UIWidgetType::Panel:
        case HE::UIWidgetType::Image:
        {
            UIImageComponent img;
            img.tint            = toVec4(node.color);
            img.materialAssetId = resolvePath(content, node.materialPath);
            reg.emplace<UIImageComponent>(e, img);
            break;
        }
        case HE::UIWidgetType::Text:
        {
            UITextComponent txt;
            txt.text     = node.text;
            txt.fontSize = node.fontSize;
            txt.color    = toVec4(node.color);
            reg.emplace<UITextComponent>(e, txt);
            break;
        }
        case HE::UIWidgetType::Button:
        {
            UIButtonComponent btn;
            btn.normalColor  = toVec4(node.color);
            btn.hoveredColor = toVec4(node.hoveredColor);
            btn.pressedColor = toVec4(node.pressedColor);
            reg.emplace<UIButtonComponent>(e, btn);
            // A button can carry a custom material on its quad too.
            if (!node.materialPath.empty())
            {
                UIImageComponent img;
                img.tint            = toVec4(node.color);
                img.materialAssetId = resolvePath(content, node.materialPath);
                reg.emplace<UIImageComponent>(e, img);
            }
            if (!node.text.empty())
            {
                UITextComponent txt;
                txt.text     = node.text;
                txt.fontSize = node.fontSize;
                txt.color    = toVec4(node.textColor);
                reg.emplace<UITextComponent>(e, txt);
            }
            break;
        }
        }

        if (!node.scriptPath.empty())
        {
            const HE::UUID scriptId = resolvePath(content, node.scriptPath);
            if (scriptId == HE::UUID{})
            {
                Logger::Log(Logger::LogLevel::Warning,
                    ("UIWidgetInstantiator: script asset not found: " + node.scriptPath).c_str());
            }
            else
            {
                ScriptComponent sc;
                sc.scriptAssetId = scriptId;
                sc.moduleName    = std::filesystem::path(node.scriptPath).stem().string();
                reg.emplace<ScriptComponent>(e, sc);
            }
        }

        for (int childId : tree.childrenOf(node.id))
            if (const HE::UIWidgetNode* child = tree.findNode(childId))
                spawnNode(world, content, tree, *child, e, spawned);
    }
}

namespace UIWidgetInstantiator {

std::vector<Entity> instantiate(HorizonWorld& world, ContentManager& content, Entity host)
{
    std::vector<Entity> spawned;
    auto& reg = world.registry();
    auto* comp = reg.try_get<UIWidgetComponent>(host);
    if (!comp || !comp->active) return spawned;

    content.ensureResident(comp->widgetAssetId);
    const UIWidgetAsset* asset = content.getWidget(comp->widgetAssetId);
    if (!asset)
    {
        Logger::Log(Logger::LogLevel::Warning,
            "UIWidgetInstantiator: widget asset not loaded — skipping");
        return spawned;
    }

    HE::UIWidgetTree tree;
    if (!HE::uiWidgetTreeFromJson(asset->treeJson, tree))
    {
        Logger::Log(Logger::LogLevel::Error,
            ("UIWidgetInstantiator: invalid widget JSON in " + asset->path).c_str());
        return spawned;
    }

    if (!reg.all_of<UICanvasComponent>(host))
    {
        UICanvasComponent canvas;
        canvas.width  = tree.canvasWidth;
        canvas.height = tree.canvasHeight;
        reg.emplace<UICanvasComponent>(host, canvas);
    }

    for (const auto& node : tree.nodes)
        if (node.parentId == 0)
            spawnNode(world, content, tree, node, host, spawned);

    return spawned;
}

std::vector<Entity> instantiateAll(HorizonWorld& world, ContentManager& content)
{
    std::vector<Entity> spawned;
    auto& reg = world.registry();
    // Snapshot hosts first — spawning mutates the registry while a view is live.
    std::vector<Entity> hosts;
    for (auto [e, comp] : reg.view<UIWidgetComponent>().each())
        if (comp.active) hosts.push_back(e);
    for (Entity host : hosts)
    {
        auto s = instantiate(world, content, host);
        spawned.insert(spawned.end(), s.begin(), s.end());
    }
    return spawned;
}

} // namespace UIWidgetInstantiator
