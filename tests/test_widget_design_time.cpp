// PreConstruct at DESIGN time (Thema 119, Schritt 4;
// docs/widget-pre-construct-design.md §5).
//
// The widget designer runs a widget's PreConstruct to show what it sets, on a
// throwaway WidgetManager whose runtime only has the design-time Services: the
// pure math/string/json/datetime rows, Is Design Time (true there) and Get
// Child Widget. Everything else — files, saves, the network, Create Widget —
// is refused and listed. Only PreConstruct runs, never Construct.
#include "doctest.h"
#include <UIWidget/UIElements.h>
#include <UIWidget/UIWidgetTree.h>
#include <UIWidget/WidgetManager.h>
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HorizonCodeRuntime.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <HorizonScene/EngineApi.h>
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

using HorizonCode::NodeType;
using HorizonCode::PinType;

namespace
{
struct TempDir
{
    std::filesystem::path path;
    TempDir()
    {
        path = std::filesystem::temp_directory_path() / "he_test_widget_design_time";
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() { std::filesystem::remove_all(path); }
};

void registerWidget(ContentManager& cm, const HE::UIWidgetTree& tree,
                    const HorizonCode::Graph* graph, const char* path)
{
    UIWidgetAsset a;
    a.treeJson = HE::uiWidgetTreeToJson(tree);
    if (graph) a.graphJson = HorizonCode::toJson(*graph);
    a.path = path;
    cm.registerWidget(std::move(a));
}

int addEvent(HorizonCode::Graph& g, const char* name)
{
    HorizonCode::Node e; e.type = NodeType::Event; e.s = name;
    return g.addNode(e);
}

// An Engine Call node mirroring registry row `id`. Pins: exec rows
// [exec in][exec out][params…][results…], pure rows [params…][results…].
int addEngineCall(HorizonCode::Graph& g, const char* id)
{
    const HE::api::ApiFn* fn = HE::api::find(id);
    REQUIRE(fn != nullptr);
    HorizonCode::Node call; call.type = NodeType::EngineCall; call.s = fn->id;
    call.hasArg = fn->isExec;
    for (const auto& p : fn->params)  call.params.push_back({ p.name, p.type, p.isArray });
    for (const auto& r : fn->results) call.results.push_back({ r.name, r.type, r.isArray });
    return g.addNode(std::move(call));
}

// Set Property `prop` (String) = `text` on element `elem`, run from exec pin
// `fromPin` of `from`. Returns the Set node.
int setTextAfter(HorizonCode::Graph& g, int from, int fromPin, int elem, const char* text)
{
    HorizonCode::Node lit; lit.type = NodeType::ConstString; lit.s = text;
    const int litId = g.addNode(lit);
    HorizonCode::Node set; set.type = NodeType::SetProperty; set.elem = elem;
    set.s = "Text"; set.propType = PinType::String;
    const int setId = g.addNode(set);
    REQUIRE(g.connect(from, fromPin, setId, 0));
    REQUIRE(g.connect(litId, 0, setId, 2));
    return setId;
}

// A Text element at the top left, the only thing these widgets need.
int addLabel(HE::UIWidgetTree& t, const char* text)
{
    const int id = t.add(HE::UIWidgetType::Text);
    HE::UIElement& e = *t.find(id);
    HE::uiSetAnchorPreset(e, 0); e.pivotX = e.pivotY = 0.0f;
    e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 160.0f; e.sizeY = 30.0f;
    e.setProp("Text", HE::UIPropValue::ofString(text));
    return id;
}

// The sandbox exactly as the designer builds it (UIEditorPanel
// refreshDesignTimeRun): own runtime, design-time callApi, nothing else.
struct DesignSandbox
{
    std::vector<std::string> refused;
    HorizonCode::Runtime     rt;
    WidgetManager            wm;
    DesignSandbox()
    {
        wm.setRuntime(&rt);
        HorizonCode::Runtime::Services svc;
        svc.callApi = HE::api::designTimeCallApi(&wm, &refused);
        rt.setServices(std::move(svc));
    }
};

const WidgetManager::DesignTimeRun::Write* findWrite(const WidgetManager::DesignTimeRun& r,
                                                     int elem, const char* prop)
{
    for (const auto& w : r.writes)
        if (w.elem == elem && w.prop == prop) return &w;
    return nullptr;
}
} // namespace

TEST_CASE("Is Design Time: a pure Widget row, false unless the Ctx says design time")
{
    const HE::api::ApiFn* fn = HE::api::find("widget.isDesignTime");
    REQUIRE(fn != nullptr);
    CHECK_FALSE(fn->isExec);
    CHECK(std::string(fn->category) == "Widget");
    REQUIRE(fn->displayName != nullptr);
    CHECK(std::string(fn->displayName) == "Is Design Time");
    CHECK(fn->params.empty());
    REQUIRE(fn->results.size() == 1);
    CHECK(fn->results[0].type == PinType::Bool);

    HE::api::Ctx game;   // the game, PIE, Lua, Python: never set
    REQUIRE(fn->invoke(game, {}).size() == 1);
    CHECK_FALSE(fn->invoke(game, {})[0].b);
    HE::api::Ctx designer;
    designer.designTime = true;
    CHECK(fn->invoke(designer, {})[0].b);
}

TEST_CASE("Design-time sandbox: an allow-list of pure groups, nothing that touches the machine")
{
    using HE::api::designTimeAllows;
    CHECK(designTimeAllows("math.clamp"));
    CHECK(designTimeAllows("string.length"));
    CHECK(designTimeAllows("json.getString"));
    CHECK(designTimeAllows("datetime.format"));
    CHECK(designTimeAllows("widget.isDesignTime"));
    CHECK(designTimeAllows("widget.childRef"));

    // Pure is not harmless: fs.exists is a data row and reads the disk.
    REQUIRE(HE::api::find("fs.exists") != nullptr);
    REQUIRE_FALSE(HE::api::find("fs.exists")->isExec);
    CHECK_FALSE(designTimeAllows("fs.exists"));
    CHECK_FALSE(designTimeAllows("fs.writeText"));
    CHECK_FALSE(designTimeAllows("fs.readText"));
    CHECK_FALSE(designTimeAllows("random.value"));     // turns the process-wide generator
    CHECK_FALSE(designTimeAllows("print.file"));
    CHECK_FALSE(designTimeAllows("widget.addChild"));
    CHECK_FALSE(designTimeAllows("widget.isVisible"));
    CHECK_FALSE(designTimeAllows("no.suchRow"));
    CHECK_FALSE(designTimeAllows("mathx.clamp"));      // the group, not a prefix

    // The invariant over the whole table: whatever is allowed is either one of
    // the two named widget rows or a PURE row of one of the four groups. A row
    // added to one of those groups as exec stays out by itself.
    int allowed = 0;
    for (const HE::api::ApiFn& fn : HE::api::registry())
    {
        if (!designTimeAllows(fn.id)) continue;
        ++allowed;
        const std::string id = fn.id;
        const std::string group = id.substr(0, id.find('.'));
        INFO(id);
        const bool named = id == "widget.isDesignTime" || id == "widget.childRef";
        CHECK((named || ((group == "math" || group == "string" || group == "json" ||
                          group == "datetime") && !fn.isExec)));
    }
    CHECK(allowed > 20);   // the math library alone is more than that
}

TEST_CASE("Design-time callApi: allowed rows answer, refused ones are listed once")
{
    std::vector<std::string> refused;
    const auto call = HE::api::designTimeCallApi(nullptr, &refused);

    const auto r = call(0, "math.clamp", { HorizonCode::Value::ofFloat(5.0f),
                                           HorizonCode::Value::ofFloat(0.0f),
                                           HorizonCode::Value::ofFloat(1.0f) });
    REQUIRE(r.size() == 1);
    CHECK(r[0].f == doctest::Approx(1.0f));
    const auto dt = call(0, "widget.isDesignTime", {});
    REQUIRE(dt.size() == 1);
    CHECK(dt[0].b);
    // No manager given: Get Child Widget finds nothing, and says so as a Ref 0.
    const auto child = call(0, "widget.childRef", { HorizonCode::Value::ofRef(1),
                                                    HorizonCode::Value::ofString("Card") });
    REQUIRE(child.size() == 1);
    CHECK(child[0].ref == 0u);
    CHECK(refused.empty());

    CHECK(call(0, "fs.writeText", {}).empty());
    CHECK(call(0, "http.get", {}).empty());
    CHECK(call(0, "fs.writeText", {}).empty());   // twice
    REQUIRE(refused.size() == 2);
    CHECK(refused[0] == "fs.writeText");
    CHECK(refused[1] == "http.get");
}

// The whole thing on one widget, built from the DOCUMENT: PreConstruct sets a
// placeholder text only on the Is Design Time branch, writes a file (refused),
// and the Construct that would overwrite the text never runs.
TEST_CASE("Design-time run: PreConstruct only, sandboxed, from the unsaved document")
{
    TempDir dir;
    ContentManager cm(dir.path.string());
    const std::string oldRoot = HE::api::fs::sandboxRoot();
    HE::api::fs::setSandboxRoot((dir.path / "saved").string());

    HE::UIWidgetTree t;
    t.canvasWidth = 200.0f; t.canvasHeight = 100.0f;
    const int label = addLabel(t, "designed");
    const int other = addLabel(t, "untouched");

    // What is SAVED for this path: a graph that writes "saved". The run must
    // not see it — the designer shows what is being edited.
    {
        HorizonCode::Graph saved;
        setTextAfter(saved, addEvent(saved, "PreConstruct"), 0, label, "saved");
        registerWidget(cm, t, &saved, "mem://status.hasset");
    }

    HorizonCode::Graph g;
    {
        const int ev = addEvent(g, "PreConstruct");
        // Write Text File first: refused, outputs default, chain goes on.
        const int fsw = addEngineCall(g, "fs.writeText");
        // Keyed by DATA-IN index, past the exec pins.
        g.findNode(fsw)->pinDefaults[0] = HorizonCode::Value::ofString("dt.txt");
        g.findNode(fsw)->pinDefaults[1] = HorizonCode::Value::ofString("x");
        REQUIRE(g.connect(ev, 0, fsw, 0));
        // Branch(Is Design Time): placeholder on true, "real" on false.
        HorizonCode::Node br; br.type = NodeType::Branch;
        const int brId = g.addNode(br);
        const int isDt = addEngineCall(g, "widget.isDesignTime");
        REQUIRE(g.connect(fsw, 1, brId, 0));
        REQUIRE(g.connect(isDt, 0, brId, 3));
        setTextAfter(g, brId, 1, label, "Placeholder");
        setTextAfter(g, brId, 2, label, "real data");
        // Construct would overwrite it — and must not run at design time.
        setTextAfter(g, addEvent(g, "Construct"), 0, label, "constructed");
    }

    const std::string treeBefore = HE::uiWidgetTreeToJson(t);
    DesignSandbox sb;
    const WidgetManager::DesignTimeRun run =
        sb.wm.runDesignTimePreConstruct(cm, "mem://status.hasset", t, g);
    REQUIRE(run.created);

    const auto* w = findWrite(run, label, "Text");
    REQUIRE(w != nullptr);
    CHECK(w->value.s == "Placeholder");               // design-time branch, no Construct
    CHECK(findWrite(run, other, "Text") == nullptr);  // only what the graph changed
    CHECK(run.writes.size() == 1);
    CHECK(run.embeds.empty());

    // The file write was refused and nothing reached the disk…
    REQUIRE(sb.refused.size() == 1);
    CHECK(sb.refused[0] == "fs.writeText");
    CHECK_FALSE(std::filesystem::exists(dir.path / "saved" / "dt.txt"));
    // …although the same row, called for real, would have written it.
    {
        HE::api::Ctx c;
        const auto ok = HE::api::find("fs.writeText")->invoke(
            c, { HorizonCode::Value::ofString("dt.txt"), HorizonCode::Value::ofString("x") });
        REQUIRE(ok.size() == 1);
        CHECK(ok[0].b);
        CHECK(std::filesystem::exists(dir.path / "saved" / "dt.txt"));
    }

    // The document handed in is untouched, and the throwaway family is gone:
    // a second run starts from scratch and gives the same answer.
    CHECK(HE::uiWidgetTreeToJson(t) == treeBefore);
    const WidgetManager::DesignTimeRun again =
        sb.wm.runDesignTimePreConstruct(cm, "mem://status.hasset", t, g);
    REQUIRE(again.writes.size() == 1);
    CHECK(again.writes[0].value.s == "Placeholder");

    // In the game the same graph takes the other branch — and Construct runs.
    {
        registerWidget(cm, t, &g, "mem://status_game.hasset");
        WidgetManager game;
        const int id = game.createWidget(cm, "mem://status_game.hasset");
        REQUIRE(id != 0);
        CHECK(game.tree(id)->find(label)->getProp("Text").s == "constructed");
    }

    HE::api::fs::setSandboxRoot(oldRoot);
}

// Theme and language are already in the tree before any graph runs, so a
// bound colour the theme rewrote is NOT reported as something PreConstruct did.
TEST_CASE("Design-time run: what the theme writes is not a PreConstruct write")
{
    TempDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree t;
    t.canvasWidth = 200.0f; t.canvasHeight = 100.0f;
    const int panel = t.add(HE::UIWidgetType::Panel);
    t.find(panel)->setProp("Color", HE::UIPropValue::ofColor({ 1.0f, 0.0f, 1.0f, 1.0f }));
    t.find(panel)->setThemeRole("Color", "Surface");
    const int label = addLabel(t, "designed");
    registerWidget(cm, t, nullptr, "mem://themed.hasset");

    // Positive control: the theme does rewrite that colour on creation.
    {
        WidgetManager plain;
        const int id = plain.createWidget(cm, "mem://themed.hasset");
        REQUIRE(id != 0);
        CHECK(plain.tree(id)->find(panel)->getProp("Color").col.g != doctest::Approx(0.0f));
    }

    HorizonCode::Graph g;
    setTextAfter(g, addEvent(g, "PreConstruct"), 0, label, "set");
    DesignSandbox sb;
    const auto run = sb.wm.runDesignTimePreConstruct(cm, "mem://themed.hasset", t, g);
    REQUIRE(run.created);
    CHECK(findWrite(run, panel, "Color") == nullptr);
    REQUIRE(run.writes.size() == 1);
    CHECK(run.writes[0].elem == label);
}

// An embedded widget runs its own PreConstruct too, and what it sets is
// reported under its instance ids, with the offset that maps them back to its
// own (local) ids — what the designer needs to draw it inside the ref. The
// page reaches it through Get Child Widget, answered by the sandbox.
TEST_CASE("Design-time run: an embed's PreConstruct lands under its ref, by offset")
{
    TempDir dir;
    ContentManager cm(dir.path.string());

    HE::UIWidgetTree card;
    card.canvasWidth = 200.0f; card.canvasHeight = 100.0f;
    const int cardLabel = addLabel(card, "card");
    HorizonCode::Graph cardG;
    {
        HorizonCode::Variable v; v.name = "label"; v.type = PinType::String; v.s = "default";
        cardG.variables.push_back(v);   // public
        const int ev = addEvent(cardG, "PreConstruct");
        const int set = setTextAfter(cardG, ev, 0, cardLabel, "from card");
        HorizonCode::Node lit; lit.type = NodeType::ConstString; lit.s = "card says hi";
        const int litId = cardG.addNode(lit);
        HorizonCode::Node sv; sv.type = NodeType::SetVariable; sv.s = "label";
        sv.propType = PinType::String;
        const int svId = cardG.addNode(sv);
        REQUIRE(cardG.connect(set, 1, svId, 0));
        REQUIRE(cardG.connect(litId, 0, svId, 2));
    }
    registerWidget(cm, card, &cardG, "mem://card.hasset");

    HE::UIWidgetTree page;
    page.canvasWidth = 400.0f; page.canvasHeight = 300.0f;
    const int pageLabel = addLabel(page, "page");
    const int slot = page.add(HE::UIWidgetType::WidgetRef);
    {
        auto* r = dynamic_cast<HE::UIWidgetRef*>(page.find(slot));
        r->widgetPath = "mem://card.hasset";
        r->name = "Card";
        HE::uiSetAnchorPreset(*r, 0); r->pivotX = r->pivotY = 0.0f;
        r->posX = 0.0f; r->posY = 40.0f; r->sizeX = 200.0f; r->sizeY = 100.0f;
    }
    // The page's PreConstruct reads the card through Get Child Widget. It runs
    // BEFORE the card's own PreConstruct (host first, as at runtime), so it
    // sees the card's default — proof that the ref resolves in the sandbox.
    HorizonCode::Graph pageG;
    {
        const int ev = addEvent(pageG, "PreConstruct");
        const int child = addEngineCall(pageG, "widget.childRef");
        pageG.findNode(child)->pinDefaults[1] = HorizonCode::Value::ofString("Card");
        HorizonCode::Node self; self.type = NodeType::GetSelf;
        const int selfId = pageG.addNode(self);
        REQUIRE(pageG.connect(selfId, 0, child, 0));
        HorizonCode::Node get; get.type = NodeType::GetExternal; get.s = "label";
        get.propType = PinType::String;
        const int getId = pageG.addNode(get);
        REQUIRE(pageG.connect(child, 2, getId, 0));
        HorizonCode::Node set; set.type = NodeType::SetProperty; set.elem = pageLabel;
        set.s = "Text"; set.propType = PinType::String;
        const int setId = pageG.addNode(set);
        REQUIRE(pageG.connect(ev, 0, setId, 0));
        REQUIRE(pageG.connect(getId, 1, setId, 2));
    }
    registerWidget(cm, page, nullptr, "mem://page.hasset");

    DesignSandbox sb;
    const auto run = sb.wm.runDesignTimePreConstruct(cm, "mem://page.hasset", page, pageG);
    REQUIRE(run.created);
    CHECK(sb.refused.empty());

    REQUIRE(run.embeds.size() == 1);
    CHECK(run.embeds[0].refElem == slot);
    const int off = run.embeds[0].idOffset;
    CHECK(off >= page.nextId - 1);   // above every id the page itself uses

    const auto* inCard = findWrite(run, off + cardLabel, "Text");
    REQUIRE(inCard != nullptr);
    CHECK(inCard->value.s == "from card");
    const auto* onPage = findWrite(run, pageLabel, "Text");
    REQUIRE(onPage != nullptr);
    CHECK(onPage->value.s == "default");   // reached the card, before its PreConstruct
}
