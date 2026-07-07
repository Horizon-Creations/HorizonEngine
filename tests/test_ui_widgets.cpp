#include "doctest.h"
#include <UIWidget/UIWidgetTree.h>
#include <Renderer/UIFont.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <Scripting/ScriptEngine.h>
#include <HorizonScene/ScriptContext.h>
#include <HorizonScene/ScriptApi.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/UISystem.h>
#include <HorizonScene/UIInputSystem.h>
#include <HorizonScene/WidgetManager.h>
#include <UIWidget/UIWidgetGraph.h>
#include <HorizonScene/SceneSerializer.h>
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
HE::UUID registerTestWidget(ContentManager& cm, const HE::UIWidgetTree& tree,
                            const HE::UIWidgetGraph* graph = nullptr,
                            const char* path = "mem://test-widget.hasset")
{
    UIWidgetAsset a;
    a.treeJson = HE::uiWidgetTreeToJson(tree);
    if (graph) a.graphJson = HE::uiWidgetGraphToJson(*graph);
    a.path = path;
    return cm.registerWidget(std::move(a));
}
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

// ── Logic graph model ────────────────────────────────────────────────────────

TEST_CASE("UIWidgetGraph JSON round-trip and typed connect")
{
    HE::UIWidgetGraph g;

    HE::UIGraphNode ev;
    ev.type = HE::UIGraphNodeType::EventClick;
    ev.elem = 7;
    const int evId = g.addNode(ev);

    HE::UIGraphNode lit;
    lit.type = HE::UIGraphNodeType::ConstString;
    lit.s = "Hello";
    const int litId = g.addNode(lit);

    HE::UIGraphNode set;
    set.type = HE::UIGraphNodeType::SetProperty;
    set.elem = 7;
    set.prop = (int)HE::UIWidgetProp::Text;
    const int setId = g.addNode(set);

    // Pin space: EventClick has 1 exec-out at index 0.
    // SetProperty: execIn 0, execOut 1, dataIn(Value) 2.
    CHECK(g.connect(evId, 0, setId, 0));   // exec → exec
    CHECK(g.connect(litId, 0, setId, 2));  // String → String (retyped by prop)

    // Type mismatch is rejected: Float literal → String input.
    HE::UIGraphNode f;
    f.type = HE::UIGraphNodeType::ConstFloat;
    const int fId = g.addNode(f);
    CHECK(!g.connect(fId, 0, setId, 2));

    const std::string json = HE::uiWidgetGraphToJson(g);
    HE::UIWidgetGraph r;
    REQUIRE(HE::uiWidgetGraphFromJson(json, r));
    CHECK(r.nodes.size() == 4);
    CHECK(r.links.size() == 2);
    const HE::UIGraphNode* rev = r.findNode(evId);
    REQUIRE(rev);
    CHECK(rev->type == HE::UIGraphNodeType::EventClick);
    CHECK(rev->elem == 7);
}

// ── Interpreter ──────────────────────────────────────────────────────────────

namespace
{
// A tree with one text element (id 1) and one button (id 2).
HE::UIWidgetTree makeGraphTestTree()
{
    HE::UIWidgetTree t;
    HE::UIWidgetNode txt;
    txt.type = HE::UIWidgetType::Text;
    txt.text = "initial";
    t.addNode(txt); // id 1
    HE::UIWidgetNode btn;
    btn.type = HE::UIWidgetType::Button;
    btn.sizeX = 200.0f; btn.sizeY = 50.0f;
    btn.anchor = 0; btn.pivotX = 0.0f; btn.pivotY = 0.0f;
    t.addNode(btn); // id 2
    return t;
}
}

TEST_CASE("Graph runner: OnClick sets a property")
{
    HE::UIWidgetTree tree = makeGraphTestTree();
    HE::UIWidgetGraph g;

    HE::UIGraphNode ev;  ev.type = HE::UIGraphNodeType::EventClick; ev.elem = 2;
    const int evId = g.addNode(ev);
    HE::UIGraphNode lit; lit.type = HE::UIGraphNodeType::ConstString; lit.s = "clicked!";
    const int litId = g.addNode(lit);
    HE::UIGraphNode set; set.type = HE::UIGraphNodeType::SetProperty;
    set.elem = 1; set.prop = (int)HE::UIWidgetProp::Text;
    const int setId = g.addNode(set);
    REQUIRE(g.connect(evId, 0, setId, 0));
    REQUIRE(g.connect(litId, 0, setId, 2));

    HE::UIWidgetSelfState self;
    HE::UIWidgetGraphRunner runner(g, tree, self);

    runner.fireEvent(HE::UIWidgetEvent::Click, /*elem=*/1); // wrong element
    CHECK(tree.findNode(1)->text == "initial");
    runner.fireEvent(HE::UIWidgetEvent::Click, /*elem=*/2);
    CHECK(tree.findNode(1)->text == "clicked!");
}

TEST_CASE("Graph runner: Branch + math + GetProperty")
{
    HE::UIWidgetTree tree = makeGraphTestTree();
    tree.findNode(1)->fontSize = 10.0f;
    HE::UIWidgetGraph g;

    // OnConstruct → Branch(fontSize > 5) → True: set text "big", False: "small"
    HE::UIGraphNode ev; ev.type = HE::UIGraphNodeType::EventConstruct;
    const int evId = g.addNode(ev);

    HE::UIGraphNode get; get.type = HE::UIGraphNodeType::GetProperty;
    get.elem = 1; get.prop = (int)HE::UIWidgetProp::FontSize;
    const int getId = g.addNode(get);

    HE::UIGraphNode five; five.type = HE::UIGraphNodeType::ConstFloat; five.f[0] = 5.0f;
    const int fiveId = g.addNode(five);

    HE::UIGraphNode gt; gt.type = HE::UIGraphNodeType::Greater;
    const int gtId = g.addNode(gt);

    HE::UIGraphNode br; br.type = HE::UIGraphNodeType::Branch;
    const int brId = g.addNode(br);

    HE::UIGraphNode sBig; sBig.type = HE::UIGraphNodeType::ConstString; sBig.s = "big";
    const int sBigId = g.addNode(sBig);
    HE::UIGraphNode setBig; setBig.type = HE::UIGraphNodeType::SetProperty;
    setBig.elem = 1; setBig.prop = (int)HE::UIWidgetProp::Text;
    const int setBigId = g.addNode(setBig);

    HE::UIGraphNode sSmall; sSmall.type = HE::UIGraphNodeType::ConstString; sSmall.s = "small";
    const int sSmallId = g.addNode(sSmall);
    HE::UIGraphNode setSmall; setSmall.type = HE::UIGraphNodeType::SetProperty;
    setSmall.elem = 1; setSmall.prop = (int)HE::UIWidgetProp::Text;
    const int setSmallId = g.addNode(setSmall);

    // Wiring. GetProperty: dataOut pin index 0. Greater: dataIns at 0/1, out 2.
    // Branch: execIn 0, True 1, False 2, Cond 3.
    REQUIRE(g.connect(evId, 0, brId, 0));       // exec
    REQUIRE(g.connect(getId, 0, gtId, 0));      // fontSize → A
    REQUIRE(g.connect(fiveId, 0, gtId, 1));     // 5 → B
    REQUIRE(g.connect(gtId, 2, brId, 3));       // Bool → Cond
    REQUIRE(g.connect(brId, 1, setBigId, 0));   // True → set "big"
    REQUIRE(g.connect(brId, 2, setSmallId, 0)); // False → set "small"
    REQUIRE(g.connect(sBigId, 0, setBigId, 2));
    REQUIRE(g.connect(sSmallId, 0, setSmallId, 2));

    HE::UIWidgetSelfState self;
    HE::UIWidgetGraphRunner runner(g, tree, self);
    runner.fireEvent(HE::UIWidgetEvent::Construct);
    CHECK(tree.findNode(1)->text == "big");

    tree.findNode(1)->fontSize = 2.0f;
    runner.fireEvent(HE::UIWidgetEvent::Construct);
    CHECK(tree.findNode(1)->text == "small");
}

TEST_CASE("Graph runner: functions honor the access modifier for script calls")
{
    HE::UIWidgetTree tree = makeGraphTestTree();
    HE::UIWidgetGraph g;

    HE::UIGraphNode fnPub; fnPub.type = HE::UIGraphNodeType::FunctionEntry;
    fnPub.s = "SetIt"; fnPub.access = 0; // public
    const int fnPubId = g.addNode(fnPub);
    HE::UIGraphNode fnPriv; fnPriv.type = HE::UIGraphNodeType::FunctionEntry;
    fnPriv.s = "Hidden"; fnPriv.access = 1; // private
    g.addNode(fnPriv);

    HE::UIGraphNode lit; lit.type = HE::UIGraphNodeType::ConstString; lit.s = "from-fn";
    const int litId = g.addNode(lit);
    HE::UIGraphNode set; set.type = HE::UIGraphNodeType::SetProperty;
    set.elem = 1; set.prop = (int)HE::UIWidgetProp::Text;
    const int setId = g.addNode(set);
    REQUIRE(g.connect(fnPubId, 0, setId, 0));
    REQUIRE(g.connect(litId, 0, setId, 2));

    HE::UIWidgetSelfState self;
    HE::UIWidgetGraphRunner runner(g, tree, self);

    CHECK(!runner.callFunction("Hidden", /*requirePublic=*/true));  // gated
    CHECK(runner.callFunction("Hidden", /*requirePublic=*/false));  // internal ok
    CHECK(!runner.callFunction("Nope", true));                      // missing
    CHECK(runner.callFunction("SetIt", true));
    CHECK(tree.findNode(1)->text == "from-fn");
}

TEST_CASE("Graph runner: ShowSelf/HideSelf flip the widget flag")
{
    HE::UIWidgetTree tree = makeGraphTestTree();
    HE::UIWidgetGraph g;
    HE::UIGraphNode fn; fn.type = HE::UIGraphNodeType::FunctionEntry;
    fn.s = "Hide"; fn.access = 0;
    const int fnId = g.addNode(fn);
    HE::UIGraphNode hide; hide.type = HE::UIGraphNodeType::HideSelf;
    const int hideId = g.addNode(hide);
    REQUIRE(g.connect(fnId, 0, hideId, 0));

    HE::UIWidgetSelfState self;
    HE::UIWidgetGraphRunner runner(g, tree, self);
    CHECK(self.visible);
    CHECK(runner.callFunction("Hide", true));
    CHECK(!self.visible);
}

// ── WidgetManager (widgets live OUTSIDE the entity world) ────────────────────

TEST_CASE("WidgetManager create/show/hide/zOrder/destroy lifecycle")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree t = makeGraphTestTree();
    registerTestWidget(cm, t);

    WidgetManager wm;
    const int id = wm.createWidget(cm, "mem://test-widget.hasset");
    REQUIRE(id != 0);
    CHECK(wm.isAlive(id));
    CHECK(wm.isVisible(id));
    CHECK(wm.count() == 1);

    wm.hideWidget(id);
    CHECK(!wm.isVisible(id));
    wm.showWidget(id);
    CHECK(wm.isVisible(id));

    wm.setZOrder(id, 42);
    CHECK(wm.zOrder(id) == 42);

    // Hidden widgets produce no draw quads.
    std::vector<UIRenderObject> out;
    wm.extract(1920.0f, 1080.0f, out);
    CHECK(!out.empty());
    wm.hideWidget(id);
    out.clear();
    wm.extract(1920.0f, 1080.0f, out);
    CHECK(out.empty());

    wm.destroyWidget(id);
    CHECK(!wm.isAlive(id));
    CHECK(wm.count() == 0);

    // Missing asset → id 0.
    CHECK(wm.createWidget(cm, "mem://does-not-exist.hasset") == 0);
}

TEST_CASE("WidgetManager fires Construct and routes public function calls")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree t = makeGraphTestTree();
    HE::UIWidgetGraph g;
    // OnConstruct sets text; a public function overwrites it later.
    HE::UIGraphNode ev; ev.type = HE::UIGraphNodeType::EventConstruct;
    const int evId = g.addNode(ev);
    HE::UIGraphNode lit; lit.type = HE::UIGraphNodeType::ConstString; lit.s = "constructed";
    const int litId = g.addNode(lit);
    HE::UIGraphNode set; set.type = HE::UIGraphNodeType::SetProperty;
    set.elem = 1; set.prop = (int)HE::UIWidgetProp::Text;
    const int setId = g.addNode(set);
    REQUIRE(g.connect(evId, 0, setId, 0));
    REQUIRE(g.connect(litId, 0, setId, 2));

    HE::UIGraphNode fn; fn.type = HE::UIGraphNodeType::FunctionEntry;
    fn.s = "Reset"; fn.access = 0;
    const int fnId = g.addNode(fn);
    HE::UIGraphNode lit2; lit2.type = HE::UIGraphNodeType::ConstString; lit2.s = "reset";
    const int lit2Id = g.addNode(lit2);
    HE::UIGraphNode set2; set2.type = HE::UIGraphNodeType::SetProperty;
    set2.elem = 1; set2.prop = (int)HE::UIWidgetProp::Text;
    const int set2Id = g.addNode(set2);
    REQUIRE(g.connect(fnId, 0, set2Id, 0));
    REQUIRE(g.connect(lit2Id, 0, set2Id, 2));

    registerTestWidget(cm, t, &g);

    WidgetManager wm;
    const int id = wm.createWidget(cm, "mem://test-widget.hasset");
    REQUIRE(id != 0);

    // Construct ran → the text glyphs spell "constructed" (11 glyph quads +
    // 1 button quad).
    std::vector<UIRenderObject> out;
    wm.extract(1920.0f, 1080.0f, out);
    int glyphs = 0;
    for (const auto& ro : out) if (ro.type == 2) ++glyphs;
    CHECK(glyphs == (int)std::string("constructed").size());

    CHECK(wm.callFunction(id, "Reset"));
    CHECK(!wm.callFunction(id, "Missing"));
    out.clear();
    wm.extract(1920.0f, 1080.0f, out);
    glyphs = 0;
    for (const auto& ro : out) if (ro.type == 2) ++glyphs;
    CHECK(glyphs == (int)std::string("reset").size());
}

TEST_CASE("WidgetManager pointer input drives clicks and hover on the graph")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    // Button (id 2) at canvas top-left, 200×50 (see makeGraphTestTree).
    HE::UIWidgetTree t = makeGraphTestTree();
    HE::UIWidgetGraph g;
    HE::UIGraphNode ev; ev.type = HE::UIGraphNodeType::EventClick; ev.elem = 2;
    const int evId = g.addNode(ev);
    HE::UIGraphNode lit; lit.type = HE::UIGraphNodeType::ConstString; lit.s = "hit";
    const int litId = g.addNode(lit);
    HE::UIGraphNode set; set.type = HE::UIGraphNodeType::SetProperty;
    set.elem = 1; set.prop = (int)HE::UIWidgetProp::Text;
    const int setId = g.addNode(set);
    REQUIRE(g.connect(evId, 0, setId, 0));
    REQUIRE(g.connect(litId, 0, setId, 2));

    registerTestWidget(cm, t, &g);

    WidgetManager wm;
    const int id = wm.createWidget(cm, "mem://test-widget.hasset");
    REQUIRE(id != 0);

    // Press + release inside the button (canvas 1920×1080 at same viewport →
    // scale 1; the button rect is 0,0..200,50).
    CHECK(wm.processPointer(1920.0f, 1080.0f, 100.0f, 25.0f, true,  true));
    CHECK(wm.processPointer(1920.0f, 1080.0f, 100.0f, 25.0f, false, true));

    std::vector<UIRenderObject> out;
    wm.extract(1920.0f, 1080.0f, out);
    int glyphs = 0;
    for (const auto& ro : out) if (ro.type == 2) ++glyphs;
    CHECK(glyphs == 3); // "hit"

    // Press on the button, release off it → no click.
    HE::UIWidgetGraph g2 = g; // unchanged; reuse widget — set text back first
    (void)g2;
    CHECK(wm.processPointer(1920.0f, 1080.0f, 100.0f, 25.0f, true, true));
    CHECK(!wm.processPointer(1920.0f, 1080.0f, 900.0f, 900.0f, false, true));

    // Hidden widget is not hit-testable.
    wm.hideWidget(id);
    CHECK(!wm.processPointer(1920.0f, 1080.0f, 100.0f, 25.0f, false, true));
}

// ── Scripting round-trip (Lua drives the widget lifecycle + graph functions) ──

TEST_CASE("Lua scripts create, drive and destroy widgets through horizon")
{
    TempWidgetDir dir;
    ContentManager cm(dir.path.string());

    // Widget with a public function that changes its text element.
    HE::UIWidgetTree t = makeGraphTestTree();
    HE::UIWidgetGraph g;
    HE::UIGraphNode fn; fn.type = HE::UIGraphNodeType::FunctionEntry;
    fn.s = "Refresh"; fn.access = 0;
    const int fnId = g.addNode(fn);
    HE::UIGraphNode priv; priv.type = HE::UIGraphNodeType::FunctionEntry;
    priv.s = "Secret"; priv.access = 1;
    g.addNode(priv);
    HE::UIGraphNode lit; lit.type = HE::UIGraphNodeType::ConstString; lit.s = "ok";
    const int litId = g.addNode(lit);
    HE::UIGraphNode set; set.type = HE::UIGraphNodeType::SetProperty;
    set.elem = 1; set.prop = (int)HE::UIWidgetProp::Text;
    const int setId = g.addNode(set);
    REQUIRE(g.connect(fnId, 0, setId, 0));
    REQUIRE(g.connect(litId, 0, setId, 2));
    registerTestWidget(cm, t, &g, "mem://hud.hasset");

    HorizonWorld world;
    ScriptContext ctx(world);
    ctx.setContentManager(&cm);

    REQUIRE(ctx.loadScript("hud", R"lua(
local M = {}
function M.onStart(self)
    self.widget = horizon.createWidget("mem://hud.hasset")
    horizon.setWidgetZOrder(self.widget, 5)
    callOk    = horizon.callWidgetFunction(self.widget, "Refresh")
    callPriv  = horizon.callWidgetFunction(self.widget, "Secret")
    callNone  = horizon.callWidgetFunction(self.widget, "Missing")
    horizon.hideWidget(self.widget)
    hiddenNow = horizon.isWidgetVisible(self.widget)
    horizon.showWidget(self.widget)
    shownNow  = horizon.isWidgetVisible(self.widget)
    widgetId  = self.widget
end
return M
)lua", ScriptLanguage::Lua));

    auto e = world.createEntity("driver");
    const auto inst = ctx.createInstance("hud", e, ScriptLanguage::Lua);
    REQUIRE(inst != ScriptEngine::kInvalidInstance);
    REQUIRE(ctx.callOnStart(inst));

    ScriptEngine& lua = ctx.engine();
    const int widgetId = (int)lua.getGlobalNumber("widgetId");
    CHECK(widgetId != 0);
    CHECK(world.widgets().isAlive(widgetId));
    CHECK(world.widgets().zOrder(widgetId) == 5);

    // Public routed, private + missing rejected.
    CHECK(lua.exec("assert(callOk == true)"));
    CHECK(lua.exec("assert(callPriv == false)"));
    CHECK(lua.exec("assert(callNone == false)"));
    CHECK(lua.exec("assert(hiddenNow == false)"));
    CHECK(lua.exec("assert(shownNow == true)"));

    // The public function actually ran: the text element now spells "ok".
    std::vector<UIRenderObject> out;
    world.widgets().extract(1920.0f, 1080.0f, out);
    int glyphs = 0;
    for (const auto& ro : out) if (ro.type == 2) ++glyphs;
    CHECK(glyphs == 2);

    CHECK(lua.exec("horizon.destroyWidget(widgetId)"));
    CHECK(!world.widgets().isAlive(widgetId));

    // World clear (PIE stop / scene load) drops surviving widgets.
    lua.exec("leftover = horizon.createWidget('mem://hud.hasset')");
    CHECK(world.widgets().count() == 1);
    world.clear();
    CHECK(world.widgets().count() == 0);
}

TEST_CASE("showCursor/hideCursor route through the host-app hook")
{
    bool visible = false;
    int calls = 0;
    ScriptApi::setCursorHook([&](bool show){ visible = show; ++calls; });

    HorizonWorld world;
    ScriptContext ctx(world);
    REQUIRE(ctx.loadScript("cur", R"lua(
local M = {}
function M.onStart(self)
    horizon.showCursor()
end
function M.onUpdate(self, dt)
    horizon.hideCursor()
end
return M
)lua", ScriptLanguage::Lua));
    auto e = world.createEntity("driver");
    const auto inst = ctx.createInstance("cur", e, ScriptLanguage::Lua);
    REQUIRE(ctx.callOnStart(inst));
    CHECK(visible);
    CHECK(calls == 1);
    REQUIRE(ctx.callOnUpdate(inst, 0.016f));
    CHECK(!visible);
    CHECK(calls == 2);

    ScriptApi::setCursorHook(nullptr); // never leak the hook into other tests
    ScriptApi::setCursorVisible(true); // no-op without a hook
    CHECK(calls == 2);
}
