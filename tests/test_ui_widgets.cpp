#include "doctest.h"
#include <UIWidget/UIWidgetTree.h>
#include <Renderer/UIFont.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <Scripting/ScriptEngine.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/UISystem.h>
#include <HorizonScene/UIInputSystem.h>
#include <HorizonScene/UIWidgetInstantiator.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/Components/UIWidgetComponent.h>
#include <HorizonScene/Components/UICanvasComponent.h>
#include <HorizonScene/Components/UIElementComponent.h>
#include <HorizonScene/Components/UITextComponent.h>
#include <HorizonScene/Components/UIImageComponent.h>
#include <HorizonScene/Components/UIButtonComponent.h>
#include <HorizonScene/Components/ScriptComponent.h>
#include <HorizonScene/Components/HierarchyComponent.h>
#include <filesystem>

namespace
{
	struct TempWidgetDir
	{
		std::filesystem::path path;
		TempWidgetDir()
		{
			path = std::filesystem::temp_directory_path() / "he_test_uiwidgets";
			std::filesystem::remove_all(path);
			std::filesystem::create_directories(path);
		}
		~TempWidgetDir() { std::filesystem::remove_all(path); }
	};
}

// ── UIWidgetTree model ───────────────────────────────────────────────────────

TEST_CASE("UIWidgetTree JSON round-trip preserves nodes and hierarchy")
{
    HE::UIWidgetTree t;
    t.canvasWidth = 1280.0f; t.canvasHeight = 720.0f;

    HE::UIWidgetNode panel;
    panel.type = HE::UIWidgetType::Panel;
    panel.name = "Root";
    panel.posX = 10.0f; panel.posY = 20.0f;
    panel.sizeX = 300.0f; panel.sizeY = 200.0f;
    panel.anchor = 4; // MiddleCenter
    panel.color[0] = 0.1f; panel.color[3] = 0.5f;
    const int panelId = t.addNode(panel);

    HE::UIWidgetNode btn;
    btn.type = HE::UIWidgetType::Button;
    btn.parentId = panelId;
    btn.name = "Play";
    btn.text = "PLAY";
    btn.fontSize = 24.0f;
    btn.materialPath = "Materials/GlowButton.hasset";
    btn.scriptPath   = "Scripts/PlayButton.hasset";
    btn.hoveredColor[0] = 0.9f;
    const int btnId = t.addNode(btn);

    const std::string json = HE::uiWidgetTreeToJson(t);
    HE::UIWidgetTree r;
    REQUIRE(HE::uiWidgetTreeFromJson(json, r));

    CHECK(r.canvasWidth  == doctest::Approx(1280.0f));
    CHECK(r.canvasHeight == doctest::Approx(720.0f));
    REQUIRE(r.nodes.size() == 2);

    const HE::UIWidgetNode* rp = r.findNode(panelId);
    REQUIRE(rp);
    CHECK(rp->type == HE::UIWidgetType::Panel);
    CHECK(rp->name == "Root");
    CHECK(rp->posX == doctest::Approx(10.0f));
    CHECK(rp->anchor == 4);
    CHECK(rp->color[3] == doctest::Approx(0.5f));

    const HE::UIWidgetNode* rb = r.findNode(btnId);
    REQUIRE(rb);
    CHECK(rb->parentId == panelId);
    CHECK(rb->text == "PLAY");
    CHECK(rb->materialPath == "Materials/GlowButton.hasset");
    CHECK(rb->scriptPath   == "Scripts/PlayButton.hasset");
    CHECK(rb->hoveredColor[0] == doctest::Approx(0.9f));

    // nextId stays ahead of loaded ids.
    CHECK(r.nextId > btnId);
}

TEST_CASE("UIWidgetTree invalid JSON is rejected without touching the output")
{
    HE::UIWidgetTree t;
    t.canvasWidth = 111.0f;
    CHECK(!HE::uiWidgetTreeFromJson("not json {", t));
    CHECK(t.canvasWidth == doctest::Approx(111.0f));
}

TEST_CASE("UIWidgetTree subtree helpers")
{
    HE::UIWidgetTree t;
    HE::UIWidgetNode n;
    const int a = t.addNode(n);
    n.parentId = a;
    const int b = t.addNode(n);
    n.parentId = b;
    const int c = t.addNode(n);
    n.parentId = 0;
    const int d = t.addNode(n);

    CHECK(t.childrenOf(0).size() == 2);       // a, d
    CHECK(t.childrenOf(a) == std::vector<int>{ b });
    CHECK(t.isDescendantOf(c, a));
    CHECK(t.isDescendantOf(c, c));
    CHECK(!t.isDescendantOf(a, c));
    CHECK(!t.isDescendantOf(d, a));

    t.removeSubtree(a);
    CHECK(t.findNode(a) == nullptr);
    CHECK(t.findNode(b) == nullptr);
    CHECK(t.findNode(c) == nullptr);
    CHECK(t.findNode(d) != nullptr);
}

// ── Instantiation ────────────────────────────────────────────────────────────

namespace
{
HE::UUID registerTestWidget(ContentManager& cm, const HE::UIWidgetTree& tree)
{
    UIWidgetAsset a;
    a.treeJson = HE::uiWidgetTreeToJson(tree);
    a.path = "mem://test-widget.hasset";
    return cm.registerWidget(std::move(a));
}
}

TEST_CASE("UIWidgetInstantiator expands a tree into UI entities")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree t;
    t.canvasWidth = 800.0f; t.canvasHeight = 600.0f;

    HE::UIWidgetNode panel;
    panel.type = HE::UIWidgetType::Panel;
    panel.name = "HUD";
    const int panelId = t.addNode(panel);

    HE::UIWidgetNode label;
    label.type = HE::UIWidgetType::Text;
    label.parentId = panelId;
    label.text = "Score";
    t.addNode(label);

    HE::UIWidgetNode btn;
    btn.type = HE::UIWidgetType::Button;
    btn.parentId = panelId;
    btn.text = "OK";
    t.addNode(btn);

    const HE::UUID widgetId = registerTestWidget(cm, t);

    HorizonWorld world;
    auto& reg = world.registry();
    auto host = world.createEntity("HudHost");
    reg.emplace<UIWidgetComponent>(host, UIWidgetComponent{ widgetId, true });

    const auto spawned = UIWidgetInstantiator::instantiateAll(world, cm);
    REQUIRE(spawned.size() == 3);

    // Host got a canvas sized from the tree.
    const auto* canvas = reg.try_get<UICanvasComponent>(host);
    REQUIRE(canvas);
    CHECK(canvas->width  == doctest::Approx(800.0f));
    CHECK(canvas->height == doctest::Approx(600.0f));

    int panels = 0, texts = 0, buttons = 0, buttonLabels = 0;
    for (Entity e : spawned)
    {
        REQUIRE(reg.all_of<UIElementComponent>(e));
        if (reg.all_of<UIButtonComponent>(e))
        {
            ++buttons;
            if (reg.all_of<UITextComponent>(e)) ++buttonLabels;
        }
        else if (reg.all_of<UITextComponent>(e)) ++texts;
        else if (reg.all_of<UIImageComponent>(e)) ++panels;
    }
    CHECK(panels == 1);
    CHECK(texts == 1);
    CHECK(buttons == 1);
    CHECK(buttonLabels == 1); // the button label rides on the button entity

    // Children are parented under the panel entity, panel under the host.
    int rootChildren = 0;
    for (Entity e : spawned)
    {
        const auto* h = reg.try_get<HierarchyComponent>(e);
        REQUIRE(h);
        if (h->parent == host) ++rootChildren;
    }
    CHECK(rootChildren == 1);
}

TEST_CASE("UIWidgetInstantiator attaches ScriptComponents from scriptPath")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    ScriptAsset script;
    script.sourceCode = "local M = {}\nreturn M\n";
    script.path = "mem://Behavior.hasset";
    const HE::UUID scriptId = cm.registerScript(std::move(script));
    (void)scriptId;

    HE::UIWidgetTree t;
    HE::UIWidgetNode n;
    n.type = HE::UIWidgetType::Button;
    n.scriptPath = "mem://Behavior.hasset";
    t.addNode(n);

    const HE::UUID widgetId = registerTestWidget(cm, t);

    HorizonWorld world;
    auto& reg = world.registry();
    auto host = world.createEntity("host");
    reg.emplace<UIWidgetComponent>(host, UIWidgetComponent{ widgetId, true });

    const auto spawned = UIWidgetInstantiator::instantiate(world, cm, host);
    REQUIRE(spawned.size() == 1);
    const auto* sc = reg.try_get<ScriptComponent>(spawned[0]);
    REQUIRE(sc);
    CHECK(sc->moduleName == "Behavior");
}

TEST_CASE("UIWidgetInstantiator skips inactive components and missing assets")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());
    HorizonWorld world;
    auto& reg = world.registry();

    auto e1 = world.createEntity("inactive");
    reg.emplace<UIWidgetComponent>(e1, UIWidgetComponent{ HE::UUID::generate(), false });
    auto e2 = world.createEntity("missing");
    reg.emplace<UIWidgetComponent>(e2, UIWidgetComponent{ HE::UUID::generate(), true });

    CHECK(UIWidgetInstantiator::instantiateAll(world, cm).empty());
}

// ── Parent-relative layout ───────────────────────────────────────────────────

TEST_CASE("UISystem child elements anchor inside their parent rect")
{
    HorizonWorld world;
    auto& reg = world.registry();

    auto ce = world.createEntity("canvas");
    reg.emplace<UICanvasComponent>(ce); // 1920×1080

    // Parent panel: TopLeft at (100, 100), 400×300.
    auto pe = world.createEntity("panel");
    UIElementComponent pel;
    pel.position = { 100.0f, 100.0f };
    pel.size     = { 400.0f, 300.0f };
    pel.pivot    = { 0.0f, 0.0f };
    pel.anchor   = UIAnchor::TopLeft;
    reg.emplace<UIElementComponent>(pe, pel);
    reg.emplace<UIImageComponent>(pe);

    // Child: centered in the parent, 100×50.
    auto che = world.createEntity("child");
    world.reparentEntity(che, pe);
    UIElementComponent cel;
    cel.position = { 0.0f, 0.0f };
    cel.size     = { 100.0f, 50.0f };
    cel.pivot    = { 0.5f, 0.5f };
    cel.anchor   = UIAnchor::MiddleCenter;
    reg.emplace<UIElementComponent>(che, cel);
    reg.emplace<UIImageComponent>(che);

    glm::vec2 pos, size;
    REQUIRE(UISystem::computeScreenRect(reg, che, 1920.0f, 1080.0f, 1.0f, 1.0f, pos, size));
    // Parent center = (100+200, 100+150) = (300, 250); minus pivot (50, 25).
    CHECK(pos.x == doctest::Approx(250.0f));
    CHECK(pos.y == doctest::Approx(225.0f));

    // Hiding the parent hides the child.
    reg.get<UIElementComponent>(pe).active = false;
    CHECK(!UISystem::computeScreenRect(reg, che, 1920.0f, 1080.0f, 1.0f, 1.0f, pos, size));
}

TEST_CASE("UISystem children draw over their parents at equal layer")
{
    HorizonWorld world;
    auto& reg = world.registry();

    auto ce = world.createEntity("canvas");
    reg.emplace<UICanvasComponent>(ce);

    auto pe = world.createEntity("panel");
    UIElementComponent pel;
    pel.pivot = { 0.0f, 0.0f };
    reg.emplace<UIElementComponent>(pe, pel);
    UIImageComponent pimg; pimg.tint = { 1.0f, 0.0f, 0.0f, 1.0f };
    reg.emplace<UIImageComponent>(pe, pimg);

    auto che = world.createEntity("child");
    world.reparentEntity(che, pe);
    UIElementComponent cel;
    cel.pivot = { 0.0f, 0.0f };
    reg.emplace<UIElementComponent>(che, cel);
    UIImageComponent cimg; cimg.tint = { 0.0f, 1.0f, 0.0f, 1.0f };
    reg.emplace<UIImageComponent>(che, cimg);

    std::vector<UIRenderObject> out;
    UISystem::extract(world, 1920.0f, 1080.0f, out);
    REQUIRE(out.size() == 2);
    // Painter's order: parent (red) first, child (green) second.
    CHECK(out[0].color.r == doctest::Approx(1.0f));
    CHECK(out[1].color.g == doctest::Approx(1.0f));
}

// ── Glyph emission ───────────────────────────────────────────────────────────

TEST_CASE("emitUITextGlyphs produces one atlas quad per printable character")
{
    std::vector<UIRenderObject> out;
    HE::emitUITextGlyphs("AB c", { 0.0f, 0.0f }, { 200.0f, 40.0f }, 20.0f,
                         { 1.0f, 1.0f, 1.0f, 1.0f }, 3, false, out);
    REQUIRE(out.size() == 4); // 'A','B',' ','c' — space bakes as an (empty) quad
    for (const auto& g : out)
    {
        CHECK(g.type == 2);
        CHECK(g.layer == 3);
    }
    // Glyphs advance left to right.
    CHECK(out[3].position.x > out[0].position.x);
}

// ── Pointer input ────────────────────────────────────────────────────────────

namespace
{
Entity makeButton(HorizonWorld& world, glm::vec2 pos, glm::vec2 size, int layer = 0)
{
    auto& reg = world.registry();
    auto e = world.createEntity("btn");
    UIElementComponent el;
    el.position = pos;
    el.size     = size;
    el.pivot    = { 0.0f, 0.0f };
    el.anchor   = UIAnchor::TopLeft;
    el.layer    = layer;
    reg.emplace<UIElementComponent>(e, el);
    reg.emplace<UIButtonComponent>(e);
    return e;
}
}

TEST_CASE("UIInputSystem hover, press and click lifecycle")
{
    HorizonWorld world;
    auto& reg = world.registry();
    auto ce = world.createEntity("canvas");
    reg.emplace<UICanvasComponent>(ce); // 1920×1080

    auto btn = makeButton(world, { 100.0f, 100.0f }, { 200.0f, 50.0f });
    const uint32_t btnId = static_cast<uint32_t>(btn);

    UIInputSystem::InputState st;
    std::vector<UIInputSystem::PointerEvent> ev;

    // Frame 1: mouse over the button → HoverEnter + Hovered state.
    UIInputSystem::update(world, st, 1920.0f, 1080.0f, 150.0f, 120.0f, false, true, ev);
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].type == UIInputSystem::PointerEvent::Type::HoverEnter);
    CHECK(ev[0].entity == btnId);
    CHECK(reg.get<UIButtonComponent>(btn).state == UIButtonState::Hovered);

    // Frame 2: press → Pressed state, no event yet.
    ev.clear();
    UIInputSystem::update(world, st, 1920.0f, 1080.0f, 150.0f, 120.0f, true, true, ev);
    CHECK(ev.empty());
    CHECK(reg.get<UIButtonComponent>(btn).state == UIButtonState::Pressed);

    // Frame 3: release on the same element → Click.
    ev.clear();
    UIInputSystem::update(world, st, 1920.0f, 1080.0f, 150.0f, 120.0f, false, true, ev);
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].type == UIInputSystem::PointerEvent::Type::Click);
    CHECK(ev[0].entity == btnId);

    // Frame 4: mouse leaves → HoverExit + Normal state.
    ev.clear();
    UIInputSystem::update(world, st, 1920.0f, 1080.0f, 5.0f, 5.0f, false, true, ev);
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].type == UIInputSystem::PointerEvent::Type::HoverExit);
    CHECK(reg.get<UIButtonComponent>(btn).state == UIButtonState::Normal);
}

TEST_CASE("UIInputSystem press-drag-away-release is not a click")
{
    HorizonWorld world;
    auto& reg = world.registry();
    auto ce = world.createEntity("canvas");
    reg.emplace<UICanvasComponent>(ce);

    makeButton(world, { 100.0f, 100.0f }, { 200.0f, 50.0f });

    UIInputSystem::InputState st;
    std::vector<UIInputSystem::PointerEvent> ev;
    UIInputSystem::update(world, st, 1920.0f, 1080.0f, 150.0f, 120.0f, true,  true, ev); // press on btn
    ev.clear();
    UIInputSystem::update(world, st, 1920.0f, 1080.0f, 5.0f,   5.0f,   false, true, ev); // release off btn
    for (const auto& e : ev)
        CHECK(e.type != UIInputSystem::PointerEvent::Type::Click);
}

TEST_CASE("UIInputSystem topmost element wins the hit-test")
{
    HorizonWorld world;
    auto& reg = world.registry();
    auto ce = world.createEntity("canvas");
    reg.emplace<UICanvasComponent>(ce);

    makeButton(world, { 0.0f, 0.0f }, { 400.0f, 400.0f }, /*layer=*/0);
    auto top = makeButton(world, { 100.0f, 100.0f }, { 100.0f, 100.0f }, /*layer=*/5);

    UIInputSystem::InputState st;
    std::vector<UIInputSystem::PointerEvent> ev;
    UIInputSystem::update(world, st, 1920.0f, 1080.0f, 150.0f, 150.0f, false, true, ev);
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].entity == static_cast<uint32_t>(top));
}

TEST_CASE("UIInputSystem invalid pointer clears hover")
{
    HorizonWorld world;
    auto& reg = world.registry();
    auto ce = world.createEntity("canvas");
    reg.emplace<UICanvasComponent>(ce);

    auto btn = makeButton(world, { 100.0f, 100.0f }, { 200.0f, 50.0f });

    UIInputSystem::InputState st;
    std::vector<UIInputSystem::PointerEvent> ev;
    UIInputSystem::update(world, st, 1920.0f, 1080.0f, 150.0f, 120.0f, false, true, ev);
    ev.clear();
    // Mouse captured (fly-look) → pointer invalid → hover exits.
    UIInputSystem::update(world, st, 1920.0f, 1080.0f, 150.0f, 120.0f, false, false, ev);
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].type == UIInputSystem::PointerEvent::Type::HoverExit);
    CHECK(world.registry().get<UIButtonComponent>(btn).state == UIButtonState::Normal);
}

// ── Script UI events (Lua backend) ───────────────────────────────────────────

TEST_CASE("ScriptEngine dispatches onClick / onHoverEnter / onHoverExit")
{
    ScriptEngine engine;
    REQUIRE(engine.loadScript("uiButton", R"lua(
local M = {}
function M.onClick(self)      clicks = (clicks or 0) + 1 end
function M.onHoverEnter(self) hovers = (hovers or 0) + 1 end
function M.onHoverExit(self)  exits  = (exits  or 0) + 1 end
return M
)lua"));
    const auto inst = engine.createInstance("uiButton", 7);
    REQUIRE(inst != ScriptEngine::kInvalidInstance);

    CHECK(engine.callOnUIEvent(inst, UIScriptEvent::Click));
    CHECK(engine.callOnUIEvent(inst, UIScriptEvent::Click));
    CHECK(engine.callOnUIEvent(inst, UIScriptEvent::HoverEnter));
    CHECK(engine.callOnUIEvent(inst, UIScriptEvent::HoverExit));

    CHECK(engine.getGlobalNumber("clicks") == doctest::Approx(2.0));
    CHECK(engine.getGlobalNumber("hovers") == doctest::Approx(1.0));
    CHECK(engine.getGlobalNumber("exits")  == doctest::Approx(1.0));
}

TEST_CASE("ScriptEngine UI events on a script without handlers are no-ops")
{
    ScriptEngine engine;
    REQUIRE(engine.loadScript("plain", "local M = {}\nreturn M\n"));
    const auto inst = engine.createInstance("plain", 1);
    CHECK(engine.callOnUIEvent(inst, UIScriptEvent::Click));
    CHECK(engine.callOnUIEvent(inst, UIScriptEvent::HoverEnter));
}

// ── Serialization ────────────────────────────────────────────────────────────

TEST_CASE("UIWidgetComponent round-trips through SceneSerializer")
{
    HorizonWorld w1;
    auto& r1 = w1.registry();
    auto e1  = w1.createEntity("widgetHost");
    UIWidgetComponent wc;
    wc.widgetAssetId = HE::UUID::generate();
    wc.active = false;
    r1.emplace<UIWidgetComponent>(e1, wc);

    SceneSerializer ser;
    std::vector<uint8_t> blob;
    REQUIRE(ser.saveToMemory(w1, blob));

    HorizonWorld w2;
    REQUIRE(ser.loadFromMemory(w2, blob));

    bool found = false;
    for (auto [e, c] : w2.registry().view<UIWidgetComponent>().each())
    {
        found = true;
        CHECK(c.widgetAssetId == wc.widgetAssetId);
        CHECK(!c.active);
    }
    CHECK(found);
}

// ── Widget asset through ContentManager save/load ────────────────────────────

TEST_CASE("UIWidgetAsset persists through HAsset save/load")
{
    TempWidgetDir dir;

    HE::UIWidgetTree t;
    t.canvasWidth = 640.0f;
    HE::UIWidgetNode n;
    n.type = HE::UIWidgetType::Image;
    n.materialPath = "Materials/M.hasset";
    t.addNode(n);

    {
        ContentManager cm(dir.path.string());
        UIWidgetAsset a;
        a.treeJson = HE::uiWidgetTreeToJson(t);
        a.path = "TestWidget.hasset";
        const HE::UUID id = cm.registerWidget(std::move(a));
        UIWidgetAsset* mut = cm.getWidgetMutable(id);
        REQUIRE(mut);
        CHECK(cm.saveAsset(*mut));
    }
    {
        ContentManager cm(dir.path.string());
        const HE::UUID id = cm.loadAsset("TestWidget.hasset");
        REQUIRE(id != HE::UUID{});
        const UIWidgetAsset* a = cm.getWidget(id);
        REQUIRE(a);
        HE::UIWidgetTree r;
        REQUIRE(HE::uiWidgetTreeFromJson(a->treeJson, r));
        CHECK(r.canvasWidth == doctest::Approx(640.0f));
        REQUIRE(r.nodes.size() == 1);
        CHECK(r.nodes[0].materialPath == "Materials/M.hasset");
    }
}
