#include "doctest.h"
#include "TestFsUtil.h"
#include "ProjectManager.h"

#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HorizonCodeRuntime.h>
#include <HorizonScene/GameInstanceHost.h>
#include <HorizonScene/HorizonWorld.h>
#include <UIWidget/UIElements.h>
#include <UIWidget/UIWidgetTree.h>
#include <UIWidget/WidgetManager.h>

#include <nlohmann/json.hpp>

#include <deque>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// ═══ Der App-Vorschaupfad, wirklich gelaufen ═════════════════════════════════
// docs/app-preview-outliner-diagnosis.md is the source reading behind this file:
// the Outliner's application branch (OutlinerPanel.cpp:166-226) had never been
// run end to end, only read. The diagnostic line the editor prints on
// EditorApplication.cpp:1994 ("preview holds N widget(s) after OnInit") is meant
// to tell three causes apart — no graph, a graph that creates nothing, a widget
// that fails to load — and nobody had ever seen it come out of a real run.
//
// So this rebuilds exactly that path, without the GUI: create a project the way
// the New Project dialog does, point a ContentManager at its Content folder the
// way EditorApplication.cpp:1487 does, bind the same four widget services
// (EditorApplication.cpp:1299), load GameInstance.hcode the way
// loadGameInstanceGraph does, and fire OnInit. Then ask the manager the two
// questions the log line asks, and the ones the panel asks after it.
//
// What this does NOT cover: the ImGui drawing itself and anything that needs an
// editor window. It measures what the panel READS, not what it paints.

namespace fs = std::filesystem;

namespace
{
    // Every preset that makes an application (ProjectManager.h:66). Listed by
    // hand rather than looped over the enum, so a seventh app template shows up
    // here as a compile-time absence rather than as silently untested.
    struct PresetCase { ProjectPreset preset; const char* name; };
    const PresetCase kAppPresets[] = {
        { ProjectPreset::Application,  "Application"  },
        { ProjectPreset::AppSidebar,   "AppSidebar"   },
        { ProjectPreset::AppWizard,    "AppWizard"    },
        { ProjectPreset::AppDashboard, "AppDashboard" },
        { ProjectPreset::AppForm,      "AppForm"      },
        { ProjectPreset::AppTool,      "AppTool"      },
    };

    // The editor's live preview, minus the editor. Members are in the order the
    // editor builds them, because the runtime has to exist before the world is
    // told about it and the services have to be bound before OnInit runs.
    struct Preview
    {
        fs::path         dir;
        ProjectManager   pm;
        ContentManager   cm;
        HorizonWorld     world;
        GameInstanceHost gi;
        HorizonCode::Graph graph;

        explicit Preview(const char* tmpName) : dir(fs::temp_directory_path() / tmpName)
        {
            he_test::removeAllQuiet(dir);
        }
        ~Preview() { he_test::removeAllQuiet(dir); }

        // Create the project, then open it the way the editor opens one.
        bool create(ProjectPreset preset, const char* name)
        {
            // appProject deliberately FALSE: the preset alone has to flip the
            // manifest's flag (ProjectManager.cpp:1312), and that flag is the
            // whole gate `uiLive` hangs on (EditorApplication.cpp:1972).
            return pm.createNewProject(dir.string(), name, preset,
                                       ProjectScriptLanguage::HorizonCode,
                                       /*appProject=*/false,
                                       /*advancedShaderEffects=*/false);
        }

        // EditorApplication.cpp:1455 + :1299 + :1487, in that order.
        void wire()
        {
            cm.setContentRoot((dir / "Content").string());
            world.setScriptRuntime(&gi.runtime());
            HorizonCode::Runtime::Services svc;
            svc.createWidget  = [this](const std::string& p){ return world.widgets().createWidget(cm, p); };
            svc.showWidget    = [this](int id){ world.widgets().showWidget(id); };
            svc.hideWidget    = [this](int id){ world.widgets().hideWidget(id); };
            svc.destroyWidget = [this](int id){ world.widgets().destroyWidget(id); };
            gi.runtime().setServices(std::move(svc));
        }

        // EditorApplication::loadGameInstanceGraph (EditorApplication.cpp:6823).
        void loadGameInstanceGraph()
        {
            graph = HorizonCode::Graph{};
            std::ifstream f(dir / "GameInstance.hcode");
            if (f)
            {
                const std::string content((std::istreambuf_iterator<char>(f)),
                                          std::istreambuf_iterator<char>());
                HorizonCode::fromJson(content, graph);
            }
            gi.setGraph(HorizonCode::toJson(graph));
        }

        // The line on EditorApplication.cpp:1994, verbatim in shape, so what a
        // test run prints can be compared with what a user's log prints.
        std::string diagnosticLine()
        {
            std::ostringstream os;
            os << "Application project: preview holds " << world.widgets().count()
               << " widget(s) after OnInit (GameInstance graph: "
               << graph.nodes.size() << " node(s))";
            return os.str();
        }
    };

    // What OutlinerPanel's drawElem actually reaches: start at parentId 0 and
    // follow parentId == e.id downwards (OutlinerPanel.cpp:188-212). Anything it
    // never reaches is drawn nowhere — the silent orphan the diagnosis doc names
    // as the likeliest cause of "shows the hierarchy wrong" (as opposed to
    // "shows nothing").
    std::set<int> reachableFromCanvas(const HE::UIWidgetTree& tree)
    {
        std::set<int> seen;
        std::deque<int> open{ 0 };
        while (!open.empty())
        {
            const int parent = open.front();
            open.pop_front();
            for (const auto& ep : tree.elements)
            {
                if (!ep || ep->parentId != parent) continue;
                if (!seen.insert(ep->id).second) continue;  // cycle guard
                open.push_back(ep->id);
            }
        }
        return seen;
    }

    std::string describe(const HE::UIElement& e)
    {
        return "id=" + std::to_string(e.id) + " parentId=" + std::to_string(e.parentId) +
               " type=" + std::string(e.typeName()) +
               " name='" + e.name + "'";
    }
}

// ─── Der Durchlauf, pro App-Preset ──────────────────────────────────────────

TEST_CASE("App preview: OnInit builds a widget hierarchy the Outliner can draw")
{
    for (const auto& pc : kAppPresets)
    {
        const std::string presetName = pc.name;   // CAPTURE prints a const char* as a pointer
        CAPTURE(presetName);
        Preview p("he_test_app_preview_outliner");
        REQUIRE(p.create(pc.preset, "PreviewProj"));

        // Gate 1 — the manifest. If this is false the editor never fires OnInit,
        // the diagnostic line is never printed and the Outliner shows the empty
        // ENTITY list instead of a widget tree (ProjectManager.cpp:1312/1506).
        CHECK(p.pm.currentProject().appProject);

        p.wire();
        p.loadGameInstanceGraph();

        // Gate 2 — is there a graph at all? First of the three causes.
        CHECK(p.graph.nodes.size() > 0);

        p.gi.fireInit();

        MESSAGE(pc.name << ": " << p.diagnosticLine());

        // Gate 3 — did the graph create anything? Second and third cause: a
        // graph that reaches no Create Widget node, and a Create Widget node
        // whose asset does not load, both land here as count() == 0.
        REQUIRE(p.world.widgets().count() == 1);
        const std::vector<int> ids = p.world.widgets().liveIds();
        REQUIRE(ids.size() == 1);

        // Gate 4 — the panel's own entry point. liveIds() naming an id whose
        // tree() is null would draw a header with nothing under it.
        const HE::UIWidgetTree* tree = p.world.widgets().tree(ids[0]);
        REQUIRE(tree != nullptr);
        CHECK(tree->elements.size() > 0);

        // Gate 5 — drawElem's contract. Not "some element has parentId 0" but
        // "every element is reachable from the canvas", which is the difference
        // between a hierarchy that is complete and one that quietly drops a
        // subtree.
        const std::set<int> reachable = reachableFromCanvas(*tree);
        MESSAGE(presetName << ": " << tree->elements.size() << " element(s), "
                << reachable.size() << " reachable from the canvas");
        std::vector<std::string> orphans;
        for (const auto& ep : tree->elements)
        {
            if (!ep) continue;
            if (reachable.count(ep->id) == 0) orphans.push_back(describe(*ep));
        }
        for (const auto& o : orphans) MESSAGE(pc.name << " orphan: " << o);
        CHECK(orphans.empty());

        // Gate 6 — ids are unique. drawElem finds children by `parentId == e.id`
        // and labels rows `##el<id>`; two elements sharing an id show each
        // other's children and collide in ImGui's id stack. This is what "the
        // hierarchy is shown wrong" looks like when nothing is missing.
        std::set<int> distinct;
        int duplicates = 0;
        for (const auto& ep : tree->elements)
            if (ep && !distinct.insert(ep->id).second)
            {
                ++duplicates;
                MESSAGE(pc.name << " duplicate id: " << describe(*ep));
            }
        CHECK(duplicates == 0);

        // Gate 7 — there IS a root. `parentId == 0` means "direct child of the
        // canvas" (UIElement.h:368) and is what every root-enumerating loop in
        // the engine looks for: the panel's drawElem(tree, 0), the layout pass
        // (WidgetManager.cpp:951), the row-template column reader
        // (WidgetManager.cpp:1512). A tree with no such element has no entry
        // point at all — nothing to draw a hierarchy FROM.
        int roots = 0;
        for (const auto& ep : tree->elements) if (ep && ep->parentId == 0) ++roots;
        CHECK(roots >= 1);
        CHECK(roots < static_cast<int>(tree->elements.size()));
    }
}

// ─── Dieselbe Wurzel, vom UI-Designer aus gesehen ───────────────────────────
// The Outliner is not the only panel that starts at the canvas: the Designer's
// own Hierarchy list is `st.tree.childrenOf(0)` (UIEditorPanel.cpp:5637). Asked
// on the AUTHORED asset rather than on a live instance, because that is what
// somebody opening RootWidget.hasset is looking at — and it is the version of
// this symptom a user meets first.

TEST_CASE("UI Designer: the authored root widget has a canvas-level root")
{
    for (const auto& pc : kAppPresets)
    {
        const std::string presetName = pc.name;
        CAPTURE(presetName);
        Preview p("he_test_app_preview_designer");
        REQUIRE(p.create(pc.preset, "DesignerProj"));
        p.wire();

        const HE::UUID assetId = p.cm.loadAsset("UI/RootWidget.hasset");
        REQUIRE(assetId != HE::UUID{});
        const UIWidgetAsset* a = p.cm.getWidget(assetId);
        REQUIRE(a != nullptr);

        HE::UIWidgetTree authored;
        REQUIRE(HE::uiWidgetTreeFromJson(a->treeJson, authored));
        REQUIRE(authored.elements.size() > 0);

        const std::vector<int> roots = authored.childrenOf(0);
        if (roots.empty() && !authored.elements.empty())
            MESSAGE(presetName << ": no canvas-level root; first element is "
                    << describe(*authored.elements.front()));
        CHECK(roots.size() > 0);
    }
}

// ─── Der Neustartpfad ───────────────────────────────────────────────────────
// Every asset edit in the UI editor calls restartAppPreview
// (UIEditorPanel.cpp:5817 → EditorApplication.cpp:6775). "I added a child and
// the Outliner still shows the old thing" is the likeliest user story behind
// the report, and it only shows up on the SECOND OnInit — so run one.

TEST_CASE("App preview: a restart after an asset edit shows the edited hierarchy")
{
    Preview p("he_test_app_preview_restart");
    REQUIRE(p.create(ProjectPreset::AppForm, "RestartProj"));
    p.wire();
    p.loadGameInstanceGraph();
    p.gi.fireInit();

    REQUIRE(p.world.widgets().count() == 1);
    const int firstId = p.world.widgets().liveIds().front();
    const size_t before = p.world.widgets().tree(firstId)->elements.size();

    // Edit the asset exactly the way the UI editor does: through the
    // ContentManager's live copy (UIEditorPanel.cpp:463 saveState / :481
    // applyToAsset), then write it to disk. Not by rewriting the .hasset behind
    // the manager's back — loadAsset returns the cached entry for a path it
    // already knows, so a disk-only edit would test a path the editor never
    // takes.
    const HE::UUID assetId = p.cm.loadAsset("UI/RootWidget.hasset");
    REQUIRE(assetId != HE::UUID{});
    {
        // The pointer is taken AFTER the last loadAsset and dropped before the
        // next one: ContentManager getters point into a dense vector.
        UIWidgetAsset* a = p.cm.getWidgetMutable(assetId);
        REQUIRE(a != nullptr);

        HE::UIWidgetTree edited;
        REQUIRE(HE::uiWidgetTreeFromJson(a->treeJson, edited));
        const int added = edited.add(HE::UIWidgetType::Text);
        REQUIRE(added != 0);
        HE::UIElement* e = edited.find(added);
        REQUIRE(e != nullptr);
        e->name     = "AddedByEdit";
        e->parentId = 0;

        a->treeJson = HE::uiWidgetTreeToJson(edited);
        CHECK(p.cm.saveAsset(*a));
    }

    // restartAppPreview, without the editor: shutdown, clear, re-register the
    // graph, OnInit again (EditorApplication.cpp:6786-6796).
    p.gi.fireShutdown();
    p.world.widgets().clear();
    p.gi.setGraph(HorizonCode::toJson(p.graph));
    p.gi.fireInit();

    MESSAGE("after restart: " << p.diagnosticLine());
    REQUIRE(p.world.widgets().count() == 1);
    const int secondId = p.world.widgets().liveIds().front();
    const HE::UIWidgetTree* after = p.world.widgets().tree(secondId);
    REQUIRE(after != nullptr);

    // The question the whole restart path exists to answer: does the rebuilt
    // preview read the file again, or hand back the ContentManager's cached
    // copy? A cached copy means the Outliner keeps showing yesterday's tree.
    bool foundAdded = false;
    for (const auto& ep : after->elements)
        if (ep && ep->name == "AddedByEdit") { foundAdded = true; break; }
    MESSAGE("elements before=" << before << " after=" << after->elements.size()
                               << " edit visible=" << (foundAdded ? "yes" : "NO"));
    CHECK(foundAdded);
}

// ─── Der Randbefund aus der Diagnose ────────────────────────────────────────
// ProjectManager.cpp:1506 reads "appProject" and nothing else; ProjectData has
// no `preset` field to fall back on. A manifest that names an app preset but
// carries no appProject key therefore opens as a GAME — no OnInit, no
// diagnostic line, an empty entity list where a widget tree belongs. Recorded
// rather than asserted: this documents what the code does today.

TEST_CASE("App preview: a manifest without the appProject key opens as a game")
{
    const auto dir = fs::temp_directory_path() / "he_test_app_preview_manifest";
    he_test::removeAllQuiet(dir);
    fs::create_directories(dir);

    nlohmann::json j;
    j["projectName"] = "HandWritten";
    j["preset"]      = static_cast<int>(ProjectPreset::AppDashboard);
    // deliberately no "appProject" key
    {
        std::ofstream f(dir / "HandWritten.heproj", std::ios::trunc);
        f << j.dump(2);
    }

    ProjectManager pm;
    const bool loaded = pm.loadProject((dir / "HandWritten.heproj").string());
    MESSAGE("hand-written manifest: loaded=" << loaded
            << " appProject=" << pm.currentProject().appProject);
    CHECK(pm.currentProject().appProject == false);   // today's behaviour

    he_test::removeAllQuiet(dir);
}

// ─── Der Fix, an seinen zwei Stellen einzeln festgenagelt ───────────────────
// The repair has two halves and they cover for each other: the templates no
// longer WRITE a negative parent, and the loader no longer PASSES one through.
// Asked together, either half alone would make the four cases above green — so
// each gets a case that can only be answered by its own half.

// Half one, at the source. Read out of the stored JSON rather than out of a
// loaded tree, because the loader would have repaired it on the way in and the
// template could keep writing -1 forever without anybody noticing.
TEST_CASE("App templates: no authored element carries a negative parent")
{
    for (const auto& pc : kAppPresets)
    {
        const std::string presetName = pc.name;
        CAPTURE(presetName);
        Preview p("he_test_app_preview_authored_parent");
        REQUIRE(p.create(pc.preset, "AuthoredProj"));
        p.wire();

        const HE::UUID assetId = p.cm.loadAsset("UI/RootWidget.hasset");
        REQUIRE(assetId != HE::UUID{});
        const UIWidgetAsset* a = p.cm.getWidget(assetId);
        REQUIRE(a != nullptr);

        const nlohmann::json j = nlohmann::json::parse(a->treeJson, nullptr, false);
        REQUIRE_FALSE(j.is_discarded());
        const auto elements = j.value("elements", nlohmann::json::array());
        REQUIRE(elements.size() > 0);

        // "parent" is the stored key for parentId (UIWidgetTree.cpp:1658), and
        // ids start at 1, so anything below zero names nothing.
        int negatives = 0, storedRoots = 0;
        for (const auto& o : elements)
        {
            const int parent = o.value("parent", 0);
            if (parent < 0)
            {
                ++negatives;
                MESSAGE(presetName << ": stored parent " << parent << " on '"
                        << o.value("name", std::string()) << "'");
            }
            if (parent == 0) ++storedRoots;
        }
        CHECK(negatives == 0);
        CHECK(storedRoots >= 1);
    }
}

// Half two, on the way in. A widget already saved with -1 — every app project
// created before the template fix — has to come back with a hierarchy without
// the user rebuilding anything.
TEST_CASE("Widget load: a stored negative parent is read as the canvas")
{
    nlohmann::json j;
    j["canvasWidth"]  = 1280.0f;
    j["canvasHeight"] = 720.0f;
    j["nextId"]       = 3;
    j["elements"]     = nlohmann::json::array({
        nlohmann::json::object({ { "type", "Panel" }, { "id", 1 },
                                 { "parent", -1 }, { "name", "Root" } }),
        nlohmann::json::object({ { "type", "VerticalBox" }, { "id", 2 },
                                 { "parent", 1 }, { "name", "Content" } }),
    });

    HE::UIWidgetTree tree;
    REQUIRE(HE::uiWidgetTreeFromJson(j.dump(), tree));
    REQUIRE(tree.elements.size() == 2);

    const HE::UIElement* root = tree.find(1);
    REQUIRE(root != nullptr);
    CHECK(root->parentId == 0);

    // The two questions the panels ask, on the repaired tree.
    const std::vector<int> canvasChildren = tree.childrenOf(0);
    REQUIRE(canvasChildren.size() == 1);
    CHECK(canvasChildren.front() == 1);
    CHECK(reachableFromCanvas(tree).size() == 2);

    // A positive parent is left alone even when it resolves to nothing: an id
    // that names a missing element is a broken reference, not the old spelling
    // of "no parent", and silently reparenting it to the canvas would move a
    // subtree the user never asked to move.
    nlohmann::json k = j;
    k["elements"][0]["parent"] = 99;
    HE::UIWidgetTree dangling;
    REQUIRE(HE::uiWidgetTreeFromJson(k.dump(), dangling));
    REQUIRE(dangling.find(1) != nullptr);
    CHECK(dangling.find(1)->parentId == 99);
}
