// Expose on Spawn for HorizonCode classes, the editor half (Thema 138, Schritt 3;
// docs/hc-class-expose-on-spawn-design.md §5).
//
// The class editor ticks a variable; every Create Object of that class — and of
// every class deriving from it — mirrors an input for it, behind Location and
// Rotation, from what the class ASSET holds right now. A rename carries the pin,
// its wire and its typed value; deleting, unticking or retyping the variable
// takes the wire away, and the log says so. Create Widget's mirror (Thema 119)
// is the model and stays as it was.
#include "doctest.h"
#include "TestFsUtil.h"
#include "HcGraphHost.h"
#include "HcRename.h"
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <Diagnostics/Log.h>
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HcClassResolve.h>
#include <filesystem>
#include <mutex>
#include <utility>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace HorizonCode;

namespace
{
struct TempDir
{
    fs::path path;
    explicit TempDir(const char* name)
    {
        path = fs::temp_directory_path() / name;
        he_test::removeAllQuiet(path);
        fs::create_directories(path);
    }
    ~TempDir() { he_test::removeAllQuiet(path); }
};

// The mirror caches per class path for the whole process, so every case uses
// class names of its own.
std::string writeClass(ContentManager& cm, const std::string& name, const std::string& baseClass,
                       const Graph& g)
{
    HorizonCodeClassAsset a;
    a.type      = HE::AssetType::HorizonCodeClass;
    a.name      = name;
    a.path      = name + ".hasset";
    a.baseClass = baseClass;
    a.graphJson = toJson(g);
    REQUIRE(cm.saveAsset(a));
    return a.path;
}

// What the class editor's save does to the live asset: the mirror reads that.
void editClass(ContentManager& cm, const std::string& path, const Graph& g)
{
    HorizonCodeClassAsset* a = cm.getHorizonCodeClassMutable(cm.loadAsset(path));
    REQUIRE(a != nullptr);
    a->graphJson = toJson(g);
}

Variable intVar(const char* name, int def, bool spawn, int access = 0)
{
    Variable v; v.name = name; v.type = PinType::Int; v.f[0] = (float)def;
    v.exposeOnSpawn = spawn; v.access = access;
    return v;
}
Variable floatVar(const char* name, float def, bool spawn)
{
    Variable v; v.name = name; v.type = PinType::Float; v.f[0] = def;
    v.exposeOnSpawn = spawn;
    return v;
}

int addCreateObject(Graph& g, const std::string& cls)
{
    Node co; co.type = NodeType::CreateObject; co.s = cls;
    return g.addNode(co);
}

std::vector<std::string> pinNames(const Graph& g, int id)
{
    std::vector<std::string> out;
    for (const FuncParam& p : g.findNode(id)->params) out.push_back(p.name);
    return out;
}

const Link* linkTo(const Graph& g, int dstNode, int dstPin)
{
    for (const Link& l : g.links)
        if (l.dstNode == dstNode && l.dstPin == dstPin) return &l;
    return nullptr;
}

// Every Editor-category warning while alive.
class EditorLogSpy
{
public:
    EditorLogSpy() { m_handle = HE::Log::addSink(&EditorLogSpy::onRecord, this); }
    ~EditorLogSpy() { HE::Log::removeSink(m_handle); }
    EditorLogSpy(const EditorLogSpy&)            = delete;
    EditorLogSpy& operator=(const EditorLogSpy&) = delete;

    bool mentions(const std::string& a, const std::string& b = {}) const
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        for (const std::string& line : m_lines)
            if (line.find(a) != std::string::npos && line.find(b) != std::string::npos) return true;
        return false;
    }
    std::size_t count() const { std::lock_guard<std::mutex> lk(m_mutex); return m_lines.size(); }

private:
    static void onRecord(const HE::Log::Record& rec, void* user)
    {
        if (rec.category != HE::Log::Cat::Editor || rec.level != HE::LogLevel::Warning) return;
        auto* self = static_cast<EditorLogSpy*>(user);
        std::lock_guard<std::mutex> lk(self->m_mutex);
        self->m_lines.emplace_back(rec.message ? rec.message : "");
    }

    mutable std::mutex       m_mutex;
    std::vector<std::string> m_lines;
    int                      m_handle = 0;
};
} // namespace

TEST_CASE("Create Object mirror: the class asset's ticked variables become inputs behind Location/Rotation")
{
    TempDir dir("he_test_class_eos_editor_follow");
    ContentManager cm(dir.path.string());
    Graph cg;
    cg.variables.push_back(intVar("score", 42, true));
    cg.variables.push_back(intVar("secret", 1, true, /*access=*/1));   // private: no pin
    cg.variables.push_back(intVar("hidden", 3, false));                // not ticked
    cg.variables.push_back(intVar("Location", 0, true));              // clashes: no pin
    const std::string path = writeClass(cm, "EosFollow", "", cg);

    Graph g;
    const int c = addCreateObject(g, path);
    REQUIRE(HcGraphHost::syncCreateObjectPins(&cm, g, /*force=*/true));
    CHECK(pinNames(g, c) == std::vector<std::string>{ "score" });
    CHECK(g.findNode(c)->pinDefaults.at(2).i == 42);                   // data-in 2, behind the fixed two
    CHECK_FALSE(HcGraphHost::syncCreateObjectPins(&cm, g, true));      // nothing new

    // The class is saved with a new default and a second ticked variable: the
    // untouched pin follows the default, the new one appears.
    cg.variables[0].f[0] = 50.0f;
    cg.variables.push_back(floatVar("speed", 2.5f, true));
    editClass(cm, path, cg);
    REQUIRE(HcGraphHost::syncCreateObjectPins(&cm, g, true));
    CHECK(pinNames(g, c) == std::vector<std::string>{ "score", "speed" });
    CHECK(g.findNode(c)->pinDefaults.at(2).i == 50);
    CHECK(g.findNode(c)->pinDefaults.at(3).f == doctest::Approx(2.5f));

    // A class that cannot be found leaves the node exactly as it is.
    g.findNode(c)->s = "EosMissing.hasset";
    CHECK_FALSE(HcGraphHost::syncCreateObjectPins(&cm, g, true));
    CHECK(g.findNode(c)->params.size() == 2);

    // An old Create Object of a class with nothing ticked keeps the old layout.
    const std::string plain = writeClass(cm, "EosPlain", "", Graph{});
    Graph old;
    const int o = addCreateObject(old, plain);
    CHECK_FALSE(HcGraphHost::syncCreateObjectPins(&cm, old, true));
    CHECK(old.findNode(o)->params.empty());
    // Create Widget nodes are not this mirror's business.
    Node cw; cw.type = NodeType::CreateWidget; cw.s = path;
    const int w = old.addNode(cw);
    CHECK_FALSE(HcGraphHost::syncCreateObjectPins(&cm, old, true));
    CHECK(old.findNode(w)->params.empty());
}

TEST_CASE("Create Object mirror: a tick on the BASE class reaches the derived class's Create Object")
{
    TempDir dir("he_test_class_eos_editor_chain");
    ContentManager cm(dir.path.string());
    Graph base;
    base.variables.push_back(intVar("hp", 100, true));
    const std::string basePath = writeClass(cm, "EosChainBase", "Entity", base);
    Graph derived;
    derived.variables.push_back(intVar("score", 7, true));
    const std::string derivedPath = writeClass(cm, "EosChainDerived", basePath, derived);

    Graph g;
    const int c = addCreateObject(g, derivedPath);
    REQUIRE(HcGraphHost::syncCreateObjectPins(&cm, g, true));
    CHECK(pinNames(g, c) == std::vector<std::string>{ "hp", "score" });   // root first

    // Only the base changes. The derived asset's own JSON is byte-identical, so
    // a cache keyed on it alone would miss this.
    base.variables.push_back(intVar("armor", 5, true));
    editClass(cm, basePath, base);
    REQUIRE(HcGraphHost::syncCreateObjectPins(&cm, g, true));
    CHECK(pinNames(g, c) == std::vector<std::string>{ "hp", "armor", "score" });
    CHECK(g.findNode(c)->pinDefaults.at(3).i == 5);
}

TEST_CASE("Create Object mirror: wires follow by name; a cut wire is logged with its reason")
{
    TempDir dir("he_test_class_eos_editor_wires");
    ContentManager cm(dir.path.string());
    Graph cg;
    cg.variables.push_back(intVar("a", 1, true));
    cg.variables.push_back(intVar("b", 2, true));
    cg.variables.push_back(intVar("c", 3, true));
    const std::string path = writeClass(cm, "EosWires", "", cg);

    Graph g;
    const int c = addCreateObject(g, path);
    Node mv; mv.type = NodeType::MakeVector3;
    const int loc = g.addNode(mv);
    REQUIRE(g.connect(loc, 3, c, 2));                 // → Location
    REQUIRE(HcGraphHost::syncCreateObjectPins(&cm, g, true));
    Node k; k.type = NodeType::ConstInt; k.f[0] = 9.0f;
    const int ka = g.addNode(k), kb = g.addNode(k), kc = g.addNode(k);
    REQUIRE(g.connect(ka, 0, c, 4));                  // → a
    REQUIRE(g.connect(kb, 0, c, 5));                  // → b
    REQUIRE(g.connect(kc, 0, c, 6));                  // → c

    EditorLogSpy spy;

    // Reorder (c first): every wire follows its name, Location's stays. Nothing
    // was cut, so nothing is logged.
    std::swap(cg.variables[0], cg.variables[2]);      // c, b, a
    editClass(cm, path, cg);
    REQUIRE(HcGraphHost::syncCreateObjectPins(&cm, g, true));
    CHECK(pinNames(g, c) == std::vector<std::string>{ "c", "b", "a" });
    REQUIRE(linkTo(g, c, 4) != nullptr); CHECK(linkTo(g, c, 4)->srcNode == kc);
    REQUIRE(linkTo(g, c, 6) != nullptr); CHECK(linkTo(g, c, 6)->srcNode == ka);
    CHECK(linkTo(g, c, 2) != nullptr);
    CHECK(spy.count() == 0);

    // Delete `b` and untick `c`: both pins go, their wires with them — and the
    // log names each one, with the path.
    cg.variables.erase(cg.variables.begin() + 1);     // c, a
    cg.variables[0].exposeOnSpawn = false;
    editClass(cm, path, cg);
    REQUIRE(HcGraphHost::syncCreateObjectPins(&cm, g, true));
    CHECK(pinNames(g, c) == std::vector<std::string>{ "a" });
    REQUIRE(linkTo(g, c, 4) != nullptr); CHECK(linkTo(g, c, 4)->srcNode == ka);
    CHECK(linkTo(g, c, 5) == nullptr);
    CHECK(spy.mentions("'b'", "no longer offers it"));
    CHECK(spy.mentions("'c'", "no longer offers it"));
    CHECK(spy.mentions(path));
    CHECK_FALSE(spy.mentions("'a'"));

    // Retype `a` (Int → Float): Create Object drops the wire of the old type,
    // and the log says why.
    cg.variables[1] = floatVar("a", 0.5f, true);
    editClass(cm, path, cg);
    REQUIRE(HcGraphHost::syncCreateObjectPins(&cm, g, true));
    CHECK(pinNames(g, c) == std::vector<std::string>{ "a" });
    CHECK(linkTo(g, c, 4) == nullptr);
    CHECK(g.findNode(c)->pinDefaults.at(2).f == doctest::Approx(0.5f));
    CHECK(spy.mentions("'a'", "changed type"));
    CHECK(linkTo(g, c, 2) != nullptr);                // Location never touched
}

TEST_CASE("Create Object mirror: picking another class keeps same-named, same-typed pins")
{
    TempDir dir("he_test_class_eos_editor_pick");
    ContentManager cm(dir.path.string());
    Graph one;
    one.variables.push_back(intVar("score", 1, true));
    one.variables.push_back(intVar("lives", 3, true));
    const std::string first = writeClass(cm, "EosPickOne", "", one);
    Graph two;
    two.variables.push_back(intVar("score", 2, true));
    two.variables.push_back(floatVar("speed", 4.0f, true));
    const std::string second = writeClass(cm, "EosPickTwo", "", two);

    Graph g;
    const int c = addCreateObject(g, first);
    REQUIRE(HcGraphHost::syncCreateObjectPins(&cm, g, true));
    Node k; k.type = NodeType::ConstInt; k.f[0] = 9.0f;
    const int ks = g.addNode(k), kl = g.addNode(k);
    REQUIRE(g.connect(ks, 0, c, 4));                  // → score
    REQUIRE(g.connect(kl, 0, c, 5));                  // → lives

    // What the picker does: write the path, mirror with force.
    EditorLogSpy spy;
    g.findNode(c)->s = second;
    REQUIRE(HcGraphHost::syncCreateObjectPins(&cm, g, true));
    CHECK(pinNames(g, c) == std::vector<std::string>{ "score", "speed" });
    REQUIRE(linkTo(g, c, 4) != nullptr); CHECK(linkTo(g, c, 4)->srcNode == ks);
    CHECK(linkTo(g, c, 5) == nullptr);
    CHECK(g.findNode(c)->pinDefaults.at(3).f == doctest::Approx(4.0f));
    CHECK(spy.mentions("'lives'", second));
}

TEST_CASE("syncSpawnPins on Create Object: a wire never slides onto a pin of another name")
{
    // One variable deleted and another ticked in the same save: the pin count
    // stays, and the generic remap would keep the wire on the index.
    SpawnPin lives; lives.param.name = "lives"; lives.param.type = PinType::Int; lives.def = Value::ofInt(3);
    SpawnPin coins; coins.param.name = "coins"; coins.param.type = PinType::Int; coins.def = Value::ofInt(0);
    for (const NodeType t : { NodeType::CreateObject, NodeType::CreateWidget })
    {
        Graph g;
        Node cn; cn.type = t; cn.s = "X.hasset";
        const int c = g.addNode(cn);
        const bool strict = t == NodeType::CreateObject;
        REQUIRE(syncSpawnPins(g, c, { lives }, nullptr, strict));
        Node k; k.type = NodeType::ConstInt; k.f[0] = 9.0f;
        const int kId = g.addNode(k);
        const int pin = 2 + firstSpawnDataIn(t);
        REQUIRE(g.connect(kId, 0, c, pin));
        REQUIRE(syncSpawnPins(g, c, { coins }, nullptr, strict));
        if (strict) CHECK(linkTo(g, c, pin) == nullptr);   // Create Object: dropped
        else        CHECK(linkTo(g, c, pin) != nullptr);   // Create Widget: as it always was
    }
}

TEST_CASE("Renaming a class variable carries its pin on the Create Object of the class and its children")
{
    const std::string base = "Content/EosRenameBase.hasset";
    const std::string child = "Content/EosRenameChild.hasset";
    Graph g;
    SpawnPin hp; hp.param.name = "hp"; hp.param.type = PinType::Int; hp.def = Value::ofInt(100);
    SpawnPin score; score.param.name = "score"; score.param.type = PinType::Int; score.def = Value::ofInt(0);
    const int ofBase  = addCreateObject(g, base);
    const int ofChild = addCreateObject(g, child);
    const int other   = addCreateObject(g, "Content/Unrelated.hasset");
    REQUIRE(syncSpawnPins(g, ofBase, { hp }, nullptr, true));
    REQUIRE(syncSpawnPins(g, ofChild, { hp, score }, nullptr, true));
    REQUIRE(syncSpawnPins(g, other, { hp }, nullptr, true));
    g.findNode(ofChild)->pinDefaults[2] = Value::ofInt(5);    // typed on the node
    Node k; k.type = NodeType::ConstInt; k.f[0] = 9.0f;
    const int kId = g.addNode(k);
    REQUIRE(g.connect(kId, 0, ofBase, 4));                    // → hp

    // The base's `hp` is renamed: targetKeys are the base AND every class
    // deriving from it (HcRenameSweep::classAndDescendants in the editor).
    const HcRename::Target t{ base, HcRename::Member::Variable, "hp", "health" };
    const HcRename::Plan p = HcRename::planGraph(g, HcRename::Role::Other, { base, child },
                                                 "Content/Caller.hasset", {}, t);
    REQUIRE(p.rename.size() == 2);
    CHECK(HcRename::apply(g, p, t));
    CHECK(g.findNode(ofBase)->s == base);                     // the path stays
    CHECK(pinNames(g, ofBase) == std::vector<std::string>{ "health" });
    CHECK(pinNames(g, ofChild) == std::vector<std::string>{ "health", "score" });
    CHECK(pinNames(g, other) == std::vector<std::string>{ "hp" });   // another class: not ours

    // The class now offers "health": the mirror finds the pins by name, so the
    // wire and the typed 5 stay where they were instead of starting over.
    SpawnPin health = hp; health.param.name = "health";
    CHECK_FALSE(syncSpawnPins(g, ofBase, { health }, nullptr, true));
    CHECK_FALSE(syncSpawnPins(g, ofChild, { health, score }, nullptr, true));
    CHECK(g.findNode(ofChild)->pinDefaults.at(2).i == 5);
    REQUIRE(linkTo(g, ofBase, 4) != nullptr);
    CHECK(linkTo(g, ofBase, 4)->srcNode == kId);

    // The class's OWN graph (Role::Declares, what the class editor runs on a
    // rename) carries a Create Object of itself the same way.
    Graph self;
    const int selfMade = addCreateObject(self, base);
    REQUIRE(syncSpawnPins(self, selfMade, { hp }, nullptr, true));
    self.variables.push_back(intVar("hp", 100, true));
    const HcRename::Plan sp = HcRename::planGraph(self, HcRename::Role::Declares, { base, child },
                                                  base, {}, t);
    CHECK(HcRename::apply(self, sp, t));
    CHECK(pinNames(self, selfMade) == std::vector<std::string>{ "health" });
    CHECK(self.findVariable("health") != nullptr);
}
