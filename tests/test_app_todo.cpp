#include "doctest.h"

#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <Application/GameLoop.h>
#include <IGameLogic.h>
#include <Hpak/ProjectExporter.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <thread>
#include <HorizonCode/HorizonCode.h>
#include <HorizonScene/EngineApi.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/WidgetManager.h>
#include <UIWidget/UIElements.h>
#include <UIWidget/UIWidgetTree.h>

#include <filesystem>
#include <string>
#include <vector>

// ═══ Welle 1, die Abnahme: eine Todo-App ═════════════════════════════════════
// docs/he-apps-plan.md names a todo app as what Wave 1 has to be able to build.
// It is the acceptance because it is ordinary: a text field, a button, a list
// that grows and shrinks, and a row that can delete itself. Nothing exotic, and
// every part of it goes through the paths a real application would use — a
// widget asset per screen, a widget asset per ROW, HorizonCode graphs, and the
// runtime API those graphs call.
//
// What this file proves: the VOCABULARY is sufficient. It builds the app the way
// an author would, drives it with real pointer input, and asks what is on
// screen. What it does not prove is the packaged build's idle cost — that needs
// an exported binary and a clock, and it is measured outside the test suite.

using namespace HorizonCode;
namespace NT_ = HorizonCode;
using P = HorizonCode::PinType;
using NT = HorizonCode::NodeType;

namespace
{
    struct TempDir
    {
        std::filesystem::path path;
        explicit TempDir(const char* name)
        {
            path = std::filesystem::temp_directory_path() / name;
            std::filesystem::remove_all(path);
            std::filesystem::create_directories(path);
        }
        ~TempDir() { std::filesystem::remove_all(path); }
    };

    HE::UUID putWidget(ContentManager& cm, const HE::UIWidgetTree& tree,
                       const HorizonCode::Graph& graph, const char* path)
    {
        UIWidgetAsset a;
        a.treeJson  = HE::uiWidgetTreeToJson(tree);
        a.graphJson = HorizonCode::toJson(graph);
        a.path      = path;
        return cm.registerWidget(std::move(a));
    }

    int countGlyphs(const std::vector<UIRenderObject>& out)
    {
        int n = 0;
        for (const auto& ro : out) if (ro.type == 2) ++n;
        return n;
    }

    // Place an element at an exact spot, so a test can click it by coordinate.
    void placeAt(HE::UIElement& e, float x, float y, float w, float h)
    {
        HE::uiSetAnchorPreset(e, 0);
        e.pivotX = e.pivotY = 0.0f;
        e.posX = x; e.posY = y; e.sizeX = w; e.sizeY = h;
    }

    void click(WidgetManager& wm, float w, float h, float x, float y)
    {
        wm.processPointer(w, h, x, y, true,  true);
        wm.processPointer(w, h, x, y, false, true);
    }
}

TEST_CASE("Todo app: rows appear, carry their text, and delete themselves")
{
    TempDir dir("he_todo_app");
    ContentManager cm(dir.path.string());
    HorizonWorld world;

    // The runtime the widgets' graphs run on, wired to the engine API the way
    // the game runtime wires it at startup. Without callApi an EngineCall node
    // is silently a no-op, which would make this test pass by doing nothing.
    HorizonCode::Runtime runtime;
    {
        HorizonCode::Runtime::Services svc;
        svc.callApi = [&](HorizonCode::InstanceId, const std::string& id,
                          const std::vector<HorizonCode::Value>& args)
            -> std::vector<HorizonCode::Value>
        {
            const HE::api::ApiFn* fn = HE::api::find(id);
            if (!fn) return {};
            HE::api::Ctx c{ &world, nullptr, &cm };
            return fn->invoke(c, args);
        };
        runtime.setServices(std::move(svc));
    }
    world.widgets().setRuntime(&runtime);

    // ── The row asset ────────────────────────────────────────────────────────
    // One label and one delete button, authored once. Its logic knows two
    // things: how to be told its text, and how to take itself out of the list
    // it was put into.
    HE::UIWidgetTree row;
    row.canvasWidth = 400.0f; row.canvasHeight = 40.0f;
    row.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int label = row.add(HE::UIWidgetType::Text);
    {
        HE::UIElement& e = *row.find(label);
        e.name = "Label";
        placeAt(e, 0.0f, 0.0f, 320.0f, 40.0f);
        e.setProp("Text", HE::UIPropValue::ofString(""));
        e.setProp("AutoSize", HE::UIPropValue::ofBool(false));
    }
    const int del = row.add(HE::UIWidgetType::Button);
    {
        HE::UIElement& e = *row.find(del);
        e.name = "Delete";
        placeAt(e, 330.0f, 4.0f, 60.0f, 32.0f);
    }

    HorizonCode::Graph rowGraph;
    {
        // Public function SetText(Text) → the label shows it.
        Node fn; fn.type = NT::FunctionEntry; fn.s = "SetText"; fn.access = 0;
        fn.params = { { "Text", P::String } };
        const int fnId = rowGraph.addNode(fn);
        Node set; set.type = NT::SetProperty; set.elem = label; set.s = "Text";
        set.propType = P::String;
        const int setId = rowGraph.addNode(set);
        REQUIRE(rowGraph.connect(fnId, 0, setId, 0));   // exec
        REQUIRE(rowGraph.connect(fnId, 1, setId, 2));   // the parameter → the value

        // The delete button takes the row out of the list it lives in. Which
        // list that is arrives in a public variable when the row is added — a
        // row has no other way to know its host, and being told beats guessing.
        Node ev; ev.type = NT::Event; ev.s = "OnClicked"; ev.elem = del;
        const int evId = rowGraph.addNode(ev);
        Node owner; owner.type = NT::GetVariable; owner.s = "Owner"; owner.propType = P::Ref;
        const int ownerId = rowGraph.addNode(owner);
        Node self; self.type = NT::GetSelf;
        const int selfId = rowGraph.addNode(self);
        Node call; call.type = NT::EngineCall; call.s = "widget.removeChild"; call.hasArg = true;
        call.params  = { { "widget", P::Ref }, { "child", P::Ref } };
        call.results = { { "ok", P::Bool } };
        const int callId = rowGraph.addNode(call);
        REQUIRE(rowGraph.connect(evId, 0, callId, 0));
        REQUIRE(rowGraph.connect(ownerId, 0, callId, 2));
        REQUIRE(rowGraph.connect(selfId, 0, callId, 3));
        rowGraph.variables.push_back({ "Owner", P::Ref, {}, /*isPublic=*/true });
    }
    putWidget(cm, row, rowGraph, "mem://TodoRow.hasset");

    // ── The page ─────────────────────────────────────────────────────────────
    HE::UIWidgetTree page;
    page.canvasWidth = 400.0f; page.canvasHeight = 600.0f;
    page.scaleMode = HE::UICanvasScaleMode::ConstantPixel;
    const int field = page.add(HE::UIWidgetType::TextInput);
    {
        HE::UIElement& e = *page.find(field);
        e.name = "Entry";
        placeAt(e, 0.0f, 0.0f, 300.0f, 40.0f);
    }
    const int addBtn = page.add(HE::UIWidgetType::Button);
    {
        HE::UIElement& e = *page.find(addBtn);
        e.name = "Add";
        placeAt(e, 310.0f, 0.0f, 90.0f, 40.0f);
    }
    const int list = page.add(HE::UIWidgetType::VerticalBox);
    {
        HE::UIElement& e = *page.find(list);
        e.name = "List";
        placeAt(e, 0.0f, 50.0f, 400.0f, 550.0f);
    }

    HorizonCode::Graph pageGraph;
    {
        // Add clicked → graft a row into the list, tell it who owns it, and hand
        // it the text that is in the field.
        Node ev; ev.type = NT::Event; ev.s = "OnClicked"; ev.elem = addBtn;
        const int evId = pageGraph.addNode(ev);
        Node self; self.type = NT::GetSelf;
        const int selfId = pageGraph.addNode(self);
        Node parent; parent.type = NT::ConstString; parent.s = "List";
        const int parentId = pageGraph.addNode(parent);
        Node asset; asset.type = NT::ConstString; asset.s = "mem://TodoRow.hasset";
        const int assetId = pageGraph.addNode(asset);
        Node add; add.type = NT::EngineCall; add.s = "widget.addChild"; add.hasArg = true;
        add.params  = { { "widget", P::Ref }, { "parent", P::String },
                        { "widgetAsset", P::String } };
        add.results = { { "child", P::Ref } };
        const int addId = pageGraph.addNode(add);
        REQUIRE(pageGraph.connect(evId, 0, addId, 0));
        REQUIRE(pageGraph.connect(selfId, 0, addId, 2));
        REQUIRE(pageGraph.connect(parentId, 0, addId, 3));
        REQUIRE(pageGraph.connect(assetId, 0, addId, 4));

        // Owner ← this page, so the row's delete button has something to call.
        Node setOwner; setOwner.type = NT::SetExternal; setOwner.s = "Owner";
        setOwner.propType = P::Ref;
        const int setOwnerId = pageGraph.addNode(setOwner);
        REQUIRE(pageGraph.connect(addId, 1, setOwnerId, 0));      // exec
        REQUIRE(pageGraph.connect(addId, 5, setOwnerId, 2));      // child → Target
        REQUIRE(pageGraph.connect(selfId, 0, setOwnerId, 3));     // this page → Value

        // …and the text, straight out of the field.
        Node text; text.type = NT::GetProperty; text.elem = field; text.s = "Text";
        text.propType = P::String;
        const int textId = pageGraph.addNode(text);
        Node callRow; callRow.type = NT::CallExternal; callRow.s = "SetText";
        callRow.params = { { "Text", P::String } };
        const int callRowId = pageGraph.addNode(callRow);
        REQUIRE(pageGraph.connect(setOwnerId, 1, callRowId, 0));
        REQUIRE(pageGraph.connect(addId, 5, callRowId, 2));       // child → Target
        REQUIRE(pageGraph.connect(textId, 0, callRowId, 3));      // field text → param

        // …and empty the field, the way every add-something form does. Not
        // decoration: without it the next add reads what is still standing
        // there, which is exactly what this test caught on its first run.
        Node empty; empty.type = NT::ConstString; empty.s = "";
        const int emptyId = pageGraph.addNode(empty);
        Node clear; clear.type = NT::SetProperty; clear.elem = field; clear.s = "Text";
        clear.propType = P::String;
        const int clearId = pageGraph.addNode(clear);
        REQUIRE(pageGraph.connect(callRowId, 1, clearId, 0));
        REQUIRE(pageGraph.connect(emptyId, 0, clearId, 2));
    }
    putWidget(cm, page, pageGraph, "mem://TodoPage.hasset");

    // ── Run it ───────────────────────────────────────────────────────────────
    WidgetManager& wm = world.widgets();
    const int app = wm.createWidget(cm, "mem://TodoPage.hasset");
    REQUIRE(app != 0);
    wm.showWidget(app);

    std::vector<UIRenderObject> out;
    wm.extract(400.0f, 600.0f, out);
    const int emptyGlyphs = countGlyphs(out);

    // Type a todo and press Add. Twice.
    wm.processPointer(400.0f, 600.0f, 100.0f, 20.0f, true, true);
    wm.processPointer(400.0f, 600.0f, 100.0f, 20.0f, false, true);
    wm.inputText("Milch");
    click(wm, 400.0f, 600.0f, 350.0f, 20.0f);

    wm.processPointer(400.0f, 600.0f, 100.0f, 20.0f, true, true);
    wm.processPointer(400.0f, 600.0f, 100.0f, 20.0f, false, true);
    wm.inputText("Brot");
    click(wm, 400.0f, 600.0f, 350.0f, 20.0f);

    out.clear();
    wm.extract(400.0f, 600.0f, out);
    CHECK(countGlyphs(out) > emptyGlyphs);   // there is more on screen than before

    // The rows are where a vertical box puts them: first at the top of the list,
    // second below it.
    const HE::UIWidgetTree* live = wm.tree(app);
    REQUIRE(live);
    int rowLabels = 0;
    float firstY = -1.0f, secondY = -1.0f;
    for (const auto& ep : live->elements)
    {
        if (!ep || ep->name != "Label") continue;
        ++rowLabels;
        const HE::UIWidgetRect r = HE::uiElementRect(*live, *ep, nullptr);
        (firstY < 0.0f ? firstY : secondY) = r.y;
    }
    CHECK(rowLabels == 2);
    CHECK(firstY >= 50.0f);          // inside the list box
    CHECK(secondY > firstY);         // stacked, not on top of each other

    // Each carries ITS OWN text — the whole point of a row being an instance.
    std::vector<std::string> texts;
    for (const auto& ep : live->elements)
        if (ep && ep->name == "Label")
            texts.push_back(ep->getProp("Text").s);
    REQUIRE(texts.size() == 2);
    CHECK(texts[0] == "Milch");
    CHECK(texts[1] == "Brot");

    // Click the first row's delete button. It removes ITSELF, which is a graph
    // in the ROW calling the runtime with a reference to its own instance.
    click(wm, 400.0f, 600.0f, 360.0f, firstY + 16.0f);

    live = wm.tree(app);
    REQUIRE(live);
    std::vector<std::string> left;
    for (const auto& ep : live->elements)
        if (ep && ep->name == "Label") left.push_back(ep->getProp("Text").s);
    REQUIRE(left.size() == 1);
    CHECK(left[0] == "Brot");        // the right one went
}

// ── The packaged half ────────────────────────────────────────────────────────
// The acceptance is not "it works in the editor", it is "it ships". This builds
// the same two widgets as REAL asset files, exports them the way the export
// dialog does, and asks the shipped configuration what the game will boot with.
//
// It leaves the export behind on purpose: the idle-CPU half of the acceptance
// needs a running binary and a clock, which is not a unit test's job. The path
// is printed so it can be launched by hand or by a script.
// Export the same little application in one of its two flavours. Both go through
// the SAME code, because "the second one rots" is exactly what the plan's risk
// list warns about and two copies of this would be how it happens.
//
// Returns the export directory, or an empty path when there is no deployed
// runtime to package (a configure without the deploy step).
static std::filesystem::path exportTodoApp(bool advancedShaderEffects)
{
    const std::filesystem::path runtimeDir = HE_TEST_GAME_RUNTIME_DIR;
    if (!std::filesystem::is_directory(runtimeDir)) return {};

    const std::string tag = advancedShaderEffects ? "advanced" : "software";
    const auto proj = std::filesystem::temp_directory_path() / ("he_todo_project_" + tag);
    const auto out  = std::filesystem::temp_directory_path() / ("he_todo_export_" + tag);
    std::filesystem::remove_all(proj);
    std::filesystem::remove_all(out);
    std::filesystem::create_directories(proj / "UI");

    ContentManager cm(proj.string());

    // A page that fills its window: a background, a card on it, a centred title.
    // It was one unplaced label at first, which is a legitimate thing for a test
    // to build and a MISLEADING thing to launch — it lands in the top-left
    // corner because that is where an element with no anchor and no position
    // belongs, and it reads on screen as "the UI is broken". A page that is laid
    // out says what it means, and it doubles as somewhere the theme is visible.
    HE::UIWidgetTree page;
    page.canvasWidth = 640.0f; page.canvasHeight = 480.0f;
    {
        const int bg = page.add(HE::UIWidgetType::Panel);
        HE::UIElement& e = *page.find(bg);
        e.name = "Background";
        HE::uiSetAnchorPreset(e, HE::kUIAnchorFill);   // the whole window, always
        e.posX = e.posY = 0.0f; e.sizeX = e.sizeY = 0.0f;
        e.setThemeRole("Color", "Background");
    }
    {
        const int card = page.add(HE::UIWidgetType::Panel);
        HE::UIElement& e = *page.find(card);
        e.name = "Card";
        HE::uiSetAnchorPreset(e, 5);                   // middle-centre
        e.posX = 0.0f; e.posY = 0.0f; e.sizeX = 420.0f; e.sizeY = 220.0f;
        e.cornerRadius = glm::vec4(16.0f);
        e.setThemeRole("Color", "Surface");
        e.shadow = true; e.shadowBlur = 18.0f; e.shadowOffsetY = 6.0f;
    }
    const int hello = page.add(HE::UIWidgetType::Text);
    {
        HE::UIElement& e = *page.find(hello);
        e.name = "Hello";
        HE::uiSetAnchorPreset(e, HE::kUIAnchorFill);
        e.posX = e.posY = 0.0f; e.sizeX = e.sizeY = 0.0f;
        e.setProp("Text", HE::UIPropValue::ofString("Todo"));
        e.setProp("AutoSize", HE::UIPropValue::ofBool(false));
        e.setProp("Align H", HE::UIPropValue::ofInt(1));   // centred both ways
        e.setProp("Align V", HE::UIPropValue::ofInt(1));
        e.setThemeRole("Color", "Text");
    }

    UIWidgetAsset asset;
    asset.type      = HE::AssetType::Widget;
    asset.name      = "TodoPage";
    asset.path      = "UI/TodoPage.hasset";
    asset.treeJson  = HE::uiWidgetTreeToJson(page);
    REQUIRE(cm.saveAsset(asset));

    // The GameInstance: OnInit → Create Widget → Show Widget, which is what an
    // application's startup IS (there is no scene to load).
    HorizonCode::Graph gi;
    {
        Node ev; ev.type = NT::Event; ev.s = "OnInit";
        const int evId = gi.addNode(ev);
        Node create; create.type = NT::CreateWidget; create.s = "UI/TodoPage.hasset";
        const int createId = gi.addNode(create);
        Node show; show.type = NT::ShowWidget;
        const int showId = gi.addNode(show);
        REQUIRE(gi.connect(evId, 0, createId, 0));
        REQUIRE(gi.connect(createId, 1, showId, 0));
        REQUIRE(gi.connect(createId, 2, showId, 2));
    }

    // What the export dialog would have put in. Advanced OFF ships the software
    // renderer by name; Advanced ON says nothing about the backend and takes the
    // platform's own — which is the whole difference between the two flavours,
    // and the reason both have to be launched.
    //
    // Driving the exporter directly means this covers the EXPORTER, not the
    // dialog's own handoff; that stays the dialog's business.
    nlohmann::json entries = nlohmann::json::array({
        nlohmann::json{ { "Key", "GameWindowWidth" }, { "Value", "640" } },
        nlohmann::json{ { "Key", "GameWindowHeight" }, { "Value", "480" } },
    });
    if (!advancedShaderEffects)
        entries.push_back(nlohmann::json{ { "Key", "GameBackend" }, { "Value", "Software" } });
    nlohmann::json cfg;
    cfg["CustomConfig"] = std::move(entries);

    ExportSettings settings;
    // The two flags that make the shipped build an APPLICATION rather than a
    // game: no scene, no physics, no camera, no world tick, and no materials.
    // They travel in project.hcfg, not in config.json — leaving them out was the
    // first thing this test caught: the exported app started Jolt and added a
    // free-fly camera to a world it does not have.
    settings.appProject           = true;
    settings.advancedShaderEffects = advancedShaderEffects;
    settings.gameRuntimeDir = runtimeDir;
    settings.bundlePython   = false;      // a HorizonCode app carries no interpreter
    settings.compress       = true;
    settings.gameConfigJson = cfg.dump(4);

    const auto res = ProjectExporter::exportProject(
        proj, "TodoApp", /*startupSceneName=*/"", out, settings,
        /*startupSceneBinary=*/{}, /*extraScenes=*/{},
        /*gameInstanceJson=*/HorizonCode::toJson(gi));
    REQUIRE_MESSAGE(res.success, res.errorMessage);
    REQUIRE(res.assetsPacked >= 1);
    REQUIRE(res.binaryFilesCopied > 0);
    return out;
}

TEST_CASE("Todo app: it exports as a software build with no scene")
{
    const auto out = exportTodoApp(/*advancedShaderEffects=*/false);
    if (out.empty()) { MESSAGE("no deployed game runtime — skipped"); return; }

    // The shipped configuration says Software, so the packaged app needs no GPU
    // — the line that makes the checkbox at project creation mean something.
    bool foundConfig = false;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(out))
    {
        if (!entry.is_regular_file() || entry.path().filename() != "config.json") continue;
        foundConfig = true;
        std::ifstream in(entry.path());
        nlohmann::json j; in >> j;
        bool sawBackend = false;
        for (const auto& kv : j.at("CustomConfig"))
            if (kv.at("Key") == "GameBackend")
            { sawBackend = true; CHECK(kv.at("Value") == "Software"); }
        CHECK(sawBackend);
    }
    CHECK(foundConfig);

    // …and no Python travelled with it.
    for (const auto& entry : std::filesystem::recursive_directory_iterator(out))
    {
        const std::string n = entry.path().filename().string();
        CAPTURE(n);
        CHECK(n.find("python") == std::string::npos);
        CHECK(n.find("Python") == std::string::npos);
    }
    MESSAGE("software build left at " << out.string());
}

TEST_CASE("Todo app: the other flavour ships the platform's own renderer")
{
    const auto out = exportTodoApp(/*advancedShaderEffects=*/true);
    if (out.empty()) { MESSAGE("no deployed game runtime — skipped"); return; }

    // With Advanced Shader Effects ON the config names NO backend, which is how
    // "take the platform's own" is said — Metal on macOS, OpenGL elsewhere. An
    // absent key is a different answer from a named one, and this is the pair of
    // lines that keeps the two flavours apart.
    bool foundConfig = false;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(out))
    {
        if (!entry.is_regular_file() || entry.path().filename() != "config.json") continue;
        foundConfig = true;
        std::ifstream in(entry.path());
        nlohmann::json j; in >> j;
        for (const auto& kv : j.at("CustomConfig"))
            CHECK(kv.at("Key") != "GameBackend");
    }
    CHECK(foundConfig);
    MESSAGE("advanced build left at " << out.string());
}

// ── What the acceptance found ────────────────────────────────────────────────
// An event-driven application draws when something changes, so a frame at idle
// is a tenth of a second long — six fixed steps for a cap of five, every frame,
// forever. Carrying that backlog is the spiral of death; the exported todo app
// warned about it once every two seconds with a number that only grew.
TEST_CASE("A long frame does not leave a backlog that grows forever")
{
    HE::GameLoopConfig cfg;
    cfg.fixedTimestep = 1.0f / 60.0f;
    cfg.maxFixedSteps = 5;
    HE::GameLoop loop(cfg);
    HorizonWorld world;

    int ran = 0;
    struct CountingLogic final : IGameLogic
    {
        int* n = nullptr;
        void onStart(HorizonWorld&) override {}
        void onUpdate(HorizonWorld&, float) override { ++*n; }
        void onStop(HorizonWorld&) override {}
    } logic;
    logic.n = &ran;

    // Ten frames of an idle application's heartbeat: 100 ms each, which is more
    // simulation time than the cap of five steps can run.
    for (int i = 0; i < 10; ++i) CHECK(loop.tick(world, &logic, 0.1f));
    CHECK(ran == 50);            // five per frame, the cap, and not one more

    // A frame that fits again runs the steps IT is worth, not a hundred catching
    // up — which is what "the backlog is gone" means from the outside.
    ran = 0;
    loop.tick(world, &logic, 1.0f / 60.0f);
    CHECK(ran == 1);

    // And an application, which ships no game-logic module at all, is never told
    // that a simulation it does not have is running late.
    HE::GameLoop quiet(cfg);
    for (int i = 0; i < 10; ++i) CHECK(quiet.tick(world, nullptr, 0.1f));
    int after = 0;
    logic.n = &after;
    quiet.tick(world, &logic, 1.0f / 60.0f);
    CHECK(after == 1);           // no backlog was silently kept for later
}

// ── The start test the risk list has been asking for ─────────────────────────
// docs/he-apps-plan.md's risk list: "ab Tag eins ein Test, der eine
// App-Konfiguration headless hochfährt, und ab Welle 2 muss er BEIDE
// App-Ausprägungen fahren, sonst verrottet eine davon."
//
// It did not exist, and the gap was not theoretical: a theme change put a null
// dereference in OnInit, every unit test stayed green, the build was clean, and
// both exported applications crashed before their first frame. Nothing in this
// suite touches STARTING — the window, the renderer, the pak, the GameInstance's
// OnInit — because all of it happens before there is anything to assert on.
//
// So the assertion is the exit code. HE_EXIT_AFTER_FRAMES makes the application
// draw a few frames and leave; a crash on the way is a signal instead, and the
// wait below is what turns "it hung" into a failure rather than a stuck suite.
namespace
{
    // Run `exe` in its own directory with a frame budget. Returns the exit code,
    // -1 when it could not be started, -2 when it outlived the deadline.
    int bootOnce(const std::filesystem::path& exe, int frames, int timeoutSeconds)
    {
        const std::string dir = exe.parent_path().string();
        // Redirected to a log beside the binary, so a failure leaves something to
        // read rather than a bare number.
        const std::string log = (exe.parent_path() / "boot.log").string();
        const std::string cmd =
            "cd " + dir + " && HE_EXIT_AFTER_FRAMES=" + std::to_string(frames) +
            " ./" + exe.filename().string() + " > " + log + " 2>&1 & echo $!";
        std::string pid;
        if (FILE* p = popen(cmd.c_str(), "r"))
        {
            char buf[64] = {};
            if (std::fgets(buf, sizeof buf, p)) pid = buf;
            pclose(p);
        }
        while (!pid.empty() && (pid.back() == '\n' || pid.back() == ' ')) pid.pop_back();
        if (pid.empty()) return -1;

        // Poll rather than wait(): the shell above is not our child, so waitpid
        // has nothing to wait on. kill(pid, 0) answers "is it still there".
        for (int i = 0; i < timeoutSeconds * 10; ++i)
        {
            if (std::system(("kill -0 " + pid + " 2>/dev/null").c_str()) != 0)
            {
                // Gone. Its own last line says whether it left on purpose.
                std::ifstream in(log);
                const std::string text((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char>());
                return text.find("leaving cleanly") != std::string::npos ? 0 : 1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::system(("kill -9 " + pid + " 2>/dev/null").c_str());
        return -2;
    }
}

TEST_CASE("Both app flavours actually start")
{
    for (const bool advanced : { false, true })
    {
        CAPTURE(advanced);
        const auto out = exportTodoApp(advanced);
        if (out.empty()) { MESSAGE("no deployed game runtime — skipped"); return; }
        const auto exe = out / "HorizonGame";
        if (!std::filesystem::exists(exe))
        {
            MESSAGE("no runnable binary for this platform — skipped");
            return;
        }

        // Thirty frames is past everything that happens once: the window, the
        // renderer, the pak mount, the theme, OnInit and the first widget.
        const int code = bootOnce(exe, /*frames=*/30, /*timeoutSeconds=*/40);
        if (code == -1) { MESSAGE("could not launch — skipped"); return; }
        INFO("read " << (out / "boot.log").string());
        CHECK_MESSAGE(code != -2, "the application never finished 30 frames");
        CHECK_MESSAGE(code == 0, "the application did not reach a clean exit — it "
                                 "crashed or quit early during startup");
    }
}
