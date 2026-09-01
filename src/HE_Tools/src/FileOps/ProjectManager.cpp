#include "ProjectManager.h"
#include "CppScaffoldTemplates.h"
#include <ContentManager/DefaultAssets.h> // well-known UUIDs seeded into the tutorial scene
#include <ContentManager/HAsset.h>        // the third-person template writes .hasset containers
#include <Types/Enums.h>                  // HE::LightType
#include <Types/UUID.h>                   // stable project identity
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <Diagnostics/Log.h>
#include <ContentManager/HAsset.h>        // application starter content (root widget)
#include <UIWidget/UIWidgetTree.h>
#include <UIWidget/UIElements.h>
#include <HorizonCode/HorizonCode.h>      // …and the GameInstance graph that shows it

namespace fs = std::filesystem;
using json   = nlohmann::json;

// ─── Application starter content ──────────────────────────────────────────────
// An application project with empty folders has nothing to show: no world, no
// scene, and therefore a black panel where its UI should be. So the template
// lays down the two things that make it an app — a ROOT WIDGET, and a
// GameInstance whose OnInit creates it. That is also exactly the structure the
// packaged build runs, so the preview practises the product rather than a
// stand-in for it. See docs/he-apps-plan.md E1/E2.
namespace
{
// Content-relative path of the widget the template writes, and the name the
// GameInstance node refers to it by. One constant, because a mismatch between
// the file and the reference is a silently empty window.
constexpr const char* kRootWidgetRel = "UI/RootWidget.hasset";

// A root widget: a panel filling the canvas, with the project's name on it. Not
// empty — an empty canvas looks exactly like a broken one, and the first thing
// anybody does is change this text, which teaches where the text lives.
std::string rootWidgetTreeJson(const std::string& projectName)
{
	HE::UIWidgetTree tree;
	tree.canvasWidth  = 1280.0f;
	tree.canvasHeight = 720.0f;

	const int panelId = tree.add(HE::UIWidgetType::Panel);
	if (auto* panel = tree.find(panelId))
	{
		panel->name = "Root";
		HE::uiSetAnchorPreset(*panel, 15);   // stretched to all four edges
		panel->posX = panel->posY = 0.0f;
		panel->sizeX = panel->sizeY = 0.0f;  // on a stretched axis these are insets
		if (auto* p = dynamic_cast<HE::UIPanel*>(panel))
			p->color = glm::vec4(0.12f, 0.12f, 0.14f, 1.0f);
	}

	const int labelId = tree.add(HE::UIWidgetType::Text);
	if (auto* label = tree.find(labelId))
	{
		label->name     = "Title";
		label->parentId = panelId;
		HE::uiSetAnchorPreset(*label, 5);    // centred
		label->posX = label->posY = 0.0f;
		label->sizeX = 600.0f; label->sizeY = 60.0f;
		if (auto* t = dynamic_cast<HE::UIText*>(label))
		{
			t->text     = projectName;
			t->fontSize = 32.0f;
		}
	}
	return HE::uiWidgetTreeToJson(tree);
}

// Write the root widget as a real .hasset: META (so the content browser and every
// picker can see what it is) plus the tree chunk. Same shape the editor's own
// "New UI Widget" writes, because a template asset that differs from a
// hand-made one is a second format nobody maintains.
bool writeRootWidgetAsset(const fs::path& root, const std::string& projectName)
{
	HAsset::Writer w;
	const HE::UUID assetId = HE::UUID::generate();
	std::vector<uint8_t> meta;
	HAsset::Writer::appendPOD(meta, static_cast<uint16_t>(HE::AssetType::Widget));
	HAsset::Writer::appendPOD(meta, assetId.hi);
	HAsset::Writer::appendPOD(meta, assetId.lo);
	HAsset::Writer::appendString(meta, std::string("RootWidget"));
	HAsset::Writer::appendString(meta, std::string(kRootWidgetRel));
	w.addChunk(HAsset::CHUNK_META, meta.data(), meta.size());

	const std::string treeJson = rootWidgetTreeJson(projectName);
	w.addChunk(HAsset::CHUNK_UIWT, treeJson.data(), treeJson.size());

	return w.write((root / "Content" / kRootWidgetRel).string(),
	               static_cast<uint16_t>(HE::AssetType::Widget));
}

// The GameInstance: OnInit → Create Widget(RootWidget) → Show Widget. Built as a
// Graph and serialised with the engine's own toJson rather than hand-written
// JSON — the node/link format is internal and would drift out from under a
// literal.
//
// Three nodes, not two: Create Widget makes an INSTANCE of the widget class and
// leaves it hidden; Show Widget is what puts it on screen.
bool writeAppGameInstance(const fs::path& root)
{
	HorizonCode::Graph g;

	// Laid out left to right along one row. Positions are not decoration: nodes
	// all default to (0, 0), and a graph written without them opens as a single
	// pile in which only the last node drawn is visible.
	constexpr float kY      = 0.0f;
	constexpr float kColGap = 260.0f;

	HorizonCode::Node ev;
	ev.id   = g.nextId++;
	ev.type = HorizonCode::NodeType::Event;
	ev.s    = "OnInit";
	ev.x    = 0.0f;  ev.y = kY;
	g.nodes.push_back(ev);

	HorizonCode::Node create;
	create.id   = g.nextId++;
	create.type = HorizonCode::NodeType::CreateWidget;
	create.s    = kRootWidgetRel;
	create.x    = kColGap;  create.y = kY;
	g.nodes.push_back(create);

	HorizonCode::Node show;
	show.id   = g.nextId++;
	show.type = HorizonCode::NodeType::ShowWidget;
	show.x    = kColGap * 2.0f;  show.y = kY;
	g.nodes.push_back(show);

	// A node's pins live in ONE flat index space, in the order exec-in, exec-out,
	// data-in, data-out. Computed from the signature rather than written as
	// literals: the numbers depend on how many pins each node type happens to
	// have, and a literal would be silently wrong the day one of them gains an
	// input.
	auto execOutPin = [](const HorizonCode::Node& n, int k)
	{
		const HorizonCode::NodeSig s = HorizonCode::signatureOf(n);
		return static_cast<int>(s.execIns.size()) + k;
	};
	auto execInPin  = [](const HorizonCode::Node&, int k) { return k; };
	auto dataOutPin = [](const HorizonCode::Node& n, int k)
	{
		const HorizonCode::NodeSig s = HorizonCode::signatureOf(n);
		return static_cast<int>(s.execIns.size() + s.execOuts.size() + s.dataIns.size()) + k;
	};
	auto dataInPin  = [](const HorizonCode::Node& n, int k)
	{
		const HorizonCode::NodeSig s = HorizonCode::signatureOf(n);
		return static_cast<int>(s.execIns.size() + s.execOuts.size()) + k;
	};

	// Exec: event → create → show. Data: the widget id Create Widget hands out
	// goes into Show Widget's input. connect() validates, so a refused link is a
	// bug in this function and says so instead of shipping a graph that does
	// nothing.
	const bool wired =
		g.connect(ev.id,     execOutPin(ev, 0),     create.id, execInPin(create, 0)) &&
		g.connect(create.id, execOutPin(create, 0), show.id,   execInPin(show, 0))   &&
		g.connect(create.id, dataOutPin(create, 0), show.id,   dataInPin(show, 0));
	if (!wired)
		HE_LOG_WARN(Config, "%s", "Application template: could not wire the GameInstance graph "
		                          "— the preview will start empty");

	std::ofstream out(root / "GameInstance.hcode", std::ios::trunc);
	if (!out.is_open()) return false;
	out << HorizonCode::toJson(g);
	return out.good();
}
} // namespace

// ─── Export profiles ──────────────────────────────────────────────────────────

std::vector<ExportProfile> defaultExportProfiles()
{
	ExportProfile dev;
	dev.name             = "Development";
	dev.compress         = false;  // fast iteration: store, no crypto
	dev.encrypt          = false;
	dev.enableModSupport = true;

	ExportProfile ship;
	ship.name             = "Shipping";
	ship.compress         = true;  // zstd + AES for distribution
	ship.encrypt          = true;
	ship.enableModSupport = false;

	return { dev, ship };
}

// Type-checked json getters. .heproj manifests are user-editable text; nlohmann's
// value()/get<> throw type_error on a wrong-typed value, which would escape
// loadProject and (because the editor auto-loads the last project on startup)
// crash-loop the editor until the file is hand-fixed. Wrong types → default.
static std::string jsonString(const json& j, const char* key, const std::string& def = {})
{
	auto it = j.find(key);
	return (it != j.end() && it->is_string()) ? it->get<std::string>() : def;
}
// A fresh project identity, hex, lower case. Written into the manifest as "id".
static std::string newProjectId()
{
	const HE::UUID u = HE::UUID::generate();
	char buf[33];
	std::snprintf(buf, sizeof(buf), "%016llx%016llx",
	              static_cast<unsigned long long>(u.hi),
	              static_cast<unsigned long long>(u.lo));
	return buf;
}

static bool jsonBool(const json& j, const char* key, bool def)
{
	auto it = j.find(key);
	return (it != j.end() && it->is_boolean()) ? it->get<bool>() : def;
}

static json profileToJson(const ExportProfile& p)
{
	json j;
	j["name"]             = p.name;
	j["compress"]         = p.compress;
	j["encrypt"]          = p.encrypt;
	j["enableModSupport"] = p.enableModSupport;
	j["startupScene"]     = p.startupScene;
	j["outputDir"]        = p.outputDir;
	j["excludePatterns"]  = p.excludePatterns;
	j["incremental"]      = p.incremental;
	j["targetPlatform"]   = p.targetPlatform;
	j["appBundle"]        = p.appBundle;
	j["shaderBackends"]   = p.shaderBackends;
	j["compileHorizonCode"] = p.compileHorizonCode;
	j["hcStopOnFailure"]    = p.hcStopOnFailure;
	return j;
}

static ExportProfile profileFromJson(const json& j)
{
	ExportProfile p;
	p.name             = jsonString(j, "name");
	p.compress         = jsonBool(j, "compress", true);
	p.encrypt          = jsonBool(j, "encrypt", false);
	p.enableModSupport = jsonBool(j, "enableModSupport", false);
	p.startupScene     = jsonString(j, "startupScene");
	p.outputDir        = jsonString(j, "outputDir");
	p.incremental      = jsonBool(j, "incremental", true);
	p.targetPlatform   = jsonString(j, "targetPlatform", "Host");
	if (p.targetPlatform.empty()) p.targetPlatform = "Host";
	p.appBundle        = jsonBool(j, "appBundle", false);
	p.shaderBackends   = j.contains("shaderBackends") && j["shaderBackends"].is_number_unsigned()
	                     ? j["shaderBackends"].get<uint32_t>() : ((1u << 4) | (1u << 0));
	p.compileHorizonCode = jsonBool(j, "compileHorizonCode", false);
	p.hcStopOnFailure    = jsonBool(j, "hcStopOnFailure", false);
	if (auto it = j.find("excludePatterns"); it != j.end() && it->is_array())
		for (const auto& e : *it)
			if (e.is_string()) p.excludePatterns.push_back(e.get<std::string>());
	return p;
}

// Read profiles from a .heproj json; seeds the defaults when absent/empty so
// callers can rely on exportProfiles never being empty.
static void readProfiles(const json& j, ProjectData& data)
{
	data.exportProfiles.clear();
	if (auto it = j.find("exportProfiles"); it != j.end() && it->is_array())
		for (const auto& e : *it)
		{
			if (!e.is_object()) continue; // malformed entry: skip, don't throw
			ExportProfile p = profileFromJson(e);
			if (!p.name.empty()) data.exportProfiles.push_back(std::move(p));
		}
	if (data.exportProfiles.empty())
		data.exportProfiles = defaultExportProfiles();

	data.activeExportProfile = jsonString(j, "activeExportProfile");
	const bool known = std::any_of(data.exportProfiles.begin(), data.exportProfiles.end(),
		[&](const ExportProfile& p) { return p.name == data.activeExportProfile; });
	if (!known) data.activeExportProfile = data.exportProfiles.front().name;
}

namespace HE::tools
{

const char* toString(ProjectScriptLanguage lang)
{
	switch (lang)
	{
	case ProjectScriptLanguage::Lua:    return "Lua";
	case ProjectScriptLanguage::Python: return "Python";
	case ProjectScriptLanguage::Cpp:    return "Cpp";
	case ProjectScriptLanguage::HorizonCode:
	default:                            return "HorizonCode";
	}
}

ProjectScriptLanguage projectScriptLanguageFromString(const std::string& s)
{
	if (s == "Lua")    return ProjectScriptLanguage::Lua;
	if (s == "Python") return ProjectScriptLanguage::Python;
	if (s == "Cpp")    return ProjectScriptLanguage::Cpp;
	return ProjectScriptLanguage::HorizonCode; // unknown/missing → default
}

} // namespace HE::tools

// ─── Native C++ gameplay scaffolding ─────────────────────────────────────────

std::string HE::tools::cppIdentifier(const std::string& name)
{
	std::string id;
	id.reserve(name.size());
	for (char c : name)
	{
		unsigned char u = static_cast<unsigned char>(c);
		id.push_back((std::isalnum(u) || c == '_') ? c : '_');
	}
	if (id.empty()) return "Unnamed";
	if (std::isdigit(static_cast<unsigned char>(id.front()))) id.insert(id.begin(), '_');
	return id;
}

namespace
{
// Write text to a file, creating parent folders. Overwrites.
bool writeTextFile(const fs::path& path, const std::string& content)
{
	std::error_code ec;
	fs::create_directories(path.parent_path(), ec);
	std::ofstream out(path, std::ios::trunc | std::ios::binary);
	if (!out.is_open()) return false;
	out << content;
	return out.good();
}

// Write only if the file does not already exist (idempotent scaffolding). Missing
// → written; present → left as-is. Returns false only on a write failure.
bool writeTextFileIfAbsent(const fs::path& path, const std::string& content)
{
	if (fs::exists(path)) return true;
	return writeTextFile(path, content);
}
} // namespace

bool writeCppLevelScript(const std::string& projectRoot, const std::string& sceneName)
{
	const fs::path source = fs::path(projectRoot) / "Source";
	const std::string className = HE::tools::cppIdentifier(sceneName) + "LevelScript";
	const fs::path header = source / (className + ".h");
	const fs::path body   = source / (className + ".cpp");
	// Idempotent: an existing level script for this scene is left untouched.
	if (fs::exists(header) && fs::exists(body)) return true;
	if (!writeTextFileIfAbsent(header, CppScaffold::levelScriptHeader(className, sceneName))) return false;
	if (!writeTextFileIfAbsent(body,   CppScaffold::levelScriptSource(className, sceneName))) return false;
	return true;
}

bool writeCppClass(const std::string& projectRoot, const std::string& className,
                   std::string* outCreatedHeaderPath)
{
	const fs::path source = fs::path(projectRoot) / "Source";
	const std::string base = HE::tools::cppIdentifier(className);
	std::string name = base;
	int counter = 1;
	while (fs::exists(source / (name + ".h")) || fs::exists(source / (name + ".cpp")))
		name = base + std::to_string(counter++);
	const fs::path header = source / (name + ".h");
	const fs::path body   = source / (name + ".cpp");
	if (!writeTextFile(header, CppScaffold::gameplayClassHeader(name))) return false;
	if (!writeTextFile(body,   CppScaffold::gameplayClassSource(name))) return false;
	if (outCreatedHeaderPath) *outCreatedHeaderPath = header.string();
	return true;
}

bool scaffoldTutorialProject(const std::string& projectRoot,
                             const std::string& projectName)
{
	const std::string readme =
		"# " + projectName + "\n"
		"\n"
		"A sandbox created by the Horizon Engine editor for the interactive tutorial.\n"
		"Nothing in here is special — it is an ordinary project, and every change you\n"
		"make while following the tour is a change to a real project you can keep.\n"
		"\n"
		"## What is already in it\n"
		"\n"
		"`Content/StartupScene.hescene` opens with:\n"
		"\n"
		"- **Sky** and **Weather** entities (atmosphere, clouds, wind — all scene data)\n"
		"- **Ground**, a wide flattened cube to stand things on\n"
		"- **Cube**, a single box above the ground to select, move and re-material\n"
		"- **Point Light**, a local light next to it\n"
		"\n"
		"## Reopening the tour\n"
		"\n"
		"Help - Interactive Tutorial. Your place in it is remembered between sessions,\n"
		"and the Tutorial template in the Project Hub recreates this sandbox at any\n"
		"time if you want to start over without touching the project you built here.\n";

	std::error_code ec;
	fs::create_directories(fs::path(projectRoot), ec);
	return writeTextFileIfAbsent(fs::path(projectRoot) / "TUTORIAL.md", readme);
}

// ─── Third-person starter ────────────────────────────────────────────────────
namespace
{
// One .hasset container: the META chunk every asset needs to be discoverable,
// then whatever payload chunks the type carries. Written by hand rather than
// through ContentManager because HorizonTools is a build-time library and does
// not link the scene/content runtime — and because a project has to be complete
// on disk before anything opens it.
bool writeChunkedAsset(const fs::path& file, const std::string& contentRelative,
                       const std::string& name, HE::AssetType type,
                       const std::vector<std::pair<uint32_t, std::string>>& chunks)
{
	std::error_code ec;
	if (fs::exists(file, ec)) return true;   // never clobber
	fs::create_directories(file.parent_path(), ec);

	HAsset::Writer w;
	std::vector<uint8_t> meta;
	HAsset::Writer::appendPOD(meta, static_cast<uint16_t>(type));
	const HE::UUID id = HE::UUID::generate();
	HAsset::Writer::appendPOD(meta, id.hi);
	HAsset::Writer::appendPOD(meta, id.lo);
	HAsset::Writer::appendString(meta, name);
	// Relative to <project>/Content, NOT to the project root — that is the path
	// scheme the content manager and Create Object both speak. (The .heproj
	// manifest's startupScene uses the other one, which is a trap worth naming.)
	HAsset::Writer::appendString(meta, contentRelative);
	w.addChunk(HAsset::CHUNK_META, meta.data(), meta.size());
	for (const auto& [chunkId, payload] : chunks)
		w.addChunk(chunkId, payload.data(), payload.size());
	return w.write(file.string(), static_cast<uint16_t>(type));
}

// The controller's graph: Begin Play → Create Object(PlayerCharacter) → Possess.
//
// Written as JSON text rather than built through the graph API for the same
// reason the scene above is: HorizonTools does not link HorizonCode. Three
// things about this format are easy to get wrong and silent when wrong, so they
// are named here rather than discovered again:
//
//  * Node "type" is the DISPLAY NAME, and it is an on-disk key. A misspelling
//    drops the node AND every link touching it, without an error.
//  * Link pin indices are ABSOLUTE in the unified pin space
//    [execIns][execOuts][dataIns][dataOuts]. An exec Engine Call therefore has
//    execIn 0, execOut 1, and its first argument at 2.
//  * An exec Engine Call MUST carry "hasArg": true. Without it the node loads
//    as a pure data node, every pin shifts by two, and the exec chain stops
//    there in silence.
//
// Create Object rather than the row displayed as "Spawn Class": Possess takes
// object references, Spawn Class answers an entity id, and there is no
// conversion between the two. The Location pin is wired from a Make Vector 3
// (rather than given an inline default) because Create Object decides placement
// by whether the pin is WIRED — an unwired pin means "leave it where the class
// authored it", so a default alone would be ignored.
constexpr const char* kControllerGraph = R"JSON({
  "nextId": 6,
  "nodes": [
    { "id": 1, "type": "Event",         "pos": [ -280.0, 0.0 ], "s": "BeginPlay" },
    { "id": 3, "type": "Make Vector 3", "pos": [ -280.0, 160.0 ],
      "pinDefaults": [ { "i": 0, "t": 1, "v": 0.0 },
                       { "i": 1, "t": 1, "v": 1.2 },
                       { "i": 2, "t": 1, "v": 0.0 } ] },
    { "id": 2, "type": "Create Object", "pos": [ 20.0, 0.0 ],
      "s": "Gameplay/PlayerCharacter.hasset" },
    { "id": 5, "type": "Get Self",      "pos": [ 280.0, 150.0 ] },
    { "id": 4, "type": "Engine Call",   "pos": [ 460.0, 0.0 ],
      "s": "player.possess", "hasArg": true,
      "params": [ { "name": "controller", "type": 7 },
                  { "name": "character",  "type": 7 } ] }
  ],
  "links": [
    [ 1, 0, 2, 0 ],
    [ 3, 3, 2, 2 ],
    [ 2, 1, 4, 0 ],
    [ 5, 0, 4, 2 ],
    [ 2, 4, 4, 3 ]
  ],
  "variables": []
})JSON";

// The character's graph: the Move axis drives Move, the Jump button drives Jump.
//
// The Entity pins of Move and Jump are deliberately UNWIRED. A leading parameter
// named "entity" means "the entity of the class making the call" when it is left
// empty, which is exactly what a character doing something to itself means. That
// is also the shape a reader should copy, so the template must not model the
// old, noisier way.
//
// Break Vector 2 → Make Vector 3 is mandatory, not decoration: the axis arrives
// as a Vec2 and Move wants a Vec3, and the graph deliberately refuses to convert
// between them. X maps to X and Y maps to Z, because the pair is a direction on
// the GROUND plane; the height component stays unwired at zero.
constexpr const char* kCharacterGraph = R"JSON({
  "nextId": 7,
  "nodes": [
    { "id": 1, "type": "Input Action",   "pos": [ -360.0, 0.0 ],
      "s": "Move", "propType": 5, "hasArg": true },
    { "id": 2, "type": "Break Vector 2", "pos": [ -140.0, 110.0 ] },
    { "id": 3, "type": "Make Vector 3",  "pos": [ 60.0, 110.0 ] },
    { "id": 4, "type": "Engine Call",    "pos": [ 280.0, 0.0 ],
      "s": "locomotion.move", "hasArg": true,
      "params": [ { "name": "entity",    "type": 3 },
                  { "name": "direction", "type": 11 } ] },
    { "id": 5, "type": "Input Action",   "pos": [ -360.0, 300.0 ], "s": "Jump" },
    { "id": 6, "type": "Engine Call",    "pos": [ 280.0, 300.0 ],
      "s": "locomotion.jump", "hasArg": true,
      "params":  [ { "name": "entity", "type": 3 } ],
      "results": [ { "name": "jumped", "type": 2 } ] }
  ],
  "links": [
    [ 1, 0, 4, 0 ],
    [ 1, 1, 2, 0 ],
    [ 2, 1, 3, 0 ],
    [ 2, 2, 3, 2 ],
    [ 3, 3, 4, 3 ],
    [ 5, 0, 6, 0 ]
  ],
  "variables": []
})JSON";
} // namespace

bool scaffoldThirdPersonProject(const std::string& projectRoot)
{
	const fs::path content = fs::path(projectRoot) / "Content";
	bool ok = true;

	// The two classes. No component chunk on purpose: a class without one
	// inherits its engine base's list at spawn time, so this character gets its
	// character controller, collider, movement component and camera child from
	// whatever those are TODAY rather than from what they were when this
	// template was written.
	ok &= writeChunkedAsset(content / "Gameplay" / "PlayerController.hasset",
	                        "Gameplay/PlayerController.hasset", "PlayerController",
	                        HE::AssetType::HorizonCodeClass,
	                        { { HAsset::CHUNK_HCGR, kControllerGraph },
	                          { HAsset::CHUNK_HCBC, "PlayerController" } });
	ok &= writeChunkedAsset(content / "Gameplay" / "PlayerCharacter.hasset",
	                        "Gameplay/PlayerCharacter.hasset", "PlayerCharacter",
	                        HE::AssetType::HorizonCodeClass,
	                        { { HAsset::CHUNK_HCGR, kCharacterGraph },
	                          { HAsset::CHUNK_HCBC, "PlayerCharacter" } });

	// The actions. Their FILE STEM is the logical name a graph binds to, so
	// renaming one of these files quietly stops the action firing.
	ok &= writeChunkedAsset(content / "Input" / "Move.hasset", "Input/Move.hasset", "Move",
	                        HE::AssetType::InputAction,
	                        { { HAsset::CHUNK_IACT, R"({"valueType":"Axis2D","runWhilePaused":false})" } });
	ok &= writeChunkedAsset(content / "Input" / "Look.hasset", "Input/Look.hasset", "Look",
	                        HE::AssetType::InputAction,
	                        { { HAsset::CHUNK_IACT, R"({"valueType":"Axis2D","runWhilePaused":false})" } });
	ok &= writeChunkedAsset(content / "Input" / "Jump.hasset", "Input/Jump.hasset", "Jump",
	                        HE::AssetType::InputAction,
	                        { { HAsset::CHUNK_IACT, R"({"valueType":"Button","runWhilePaused":false})" } });

	// …and what they are bound to. Keyboard and gamepad both, because "it does
	// not work with a controller" is otherwise the second thing anyone reports.
	// A mapping entry names its action by content-relative PATH, unlike the
	// graph node above, which names it by stem — the one inconsistency in this
	// file that cannot be papered over here.
	//
	// Look is bound but unused by the shipped graphs: the camera rig already
	// turns with the mouse on its own, and an axis wired to nothing is a smaller
	// surprise than an action that exists in the mapping list and nowhere else.
	// The Move axis IS a direction on the ground plane in the engine's own axes:
	// its X is world X and its Y is world Z, which the character graph wires
	// straight through. So FORWARD IS NEGATIVE — -Z is forward everywhere else in
	// this engine (the camera rig's own forward vector, orientToMovement's atan2),
	// and an axis that disagreed would make W walk backwards. It did, until this
	// comment existed.
	//
	// That also makes the gamepad binding the plain one: SDL's left stick already
	// reports negative when pushed up, which is forward, so it needs no flip.
	constexpr const char* kMappings = R"JSON({"entries":[
 {"action":"Input/Move.hasset",
  "axesX":[{"source":"Key","positive":"D","negative":"A","scale":1.0},
           {"source":"GamepadAxis","axis":"leftx","scale":1.0}],
  "axesY":[{"source":"Key","positive":"W","negative":"S","scale":-1.0},
           {"source":"GamepadAxis","axis":"lefty","scale":1.0}]},
 {"action":"Input/Look.hasset",
  "axesX":[{"source":"MouseDeltaX","scale":1.0},
           {"source":"GamepadAxis","axis":"rightx","scale":1.0}],
  "axesY":[{"source":"MouseDeltaY","scale":1.0},
           {"source":"GamepadAxis","axis":"righty","scale":1.0}]},
 {"action":"Input/Jump.hasset","keys":["Space"],"gamepadButtons":["a"]}
]})JSON";
	ok &= writeChunkedAsset(content / "Input" / "DefaultMappings.hasset",
	                        "Input/DefaultMappings.hasset", "DefaultMappings",
	                        HE::AssetType::InputMappingContext,
	                        { { HAsset::CHUNK_IMAP, kMappings } });
	return ok;
}

bool scaffoldCppProject(const std::string& projectRoot,
                        const std::string& projectName,
                        const std::string& startupSceneName)
{
	const fs::path source = fs::path(projectRoot) / "Source";
	std::error_code ec;
	fs::create_directories(source, ec);

	bool ok = true;
	ok &= writeTextFileIfAbsent(source / "GameLogicRuntime.h",  CppScaffold::runtimeHeader());
	ok &= writeTextFileIfAbsent(source / "GameLogicRuntime.cpp", CppScaffold::runtimeSource());
	ok &= writeTextFileIfAbsent(source / "GameInstance.h",      CppScaffold::gameInstanceHeader());
	ok &= writeTextFileIfAbsent(source / "GameInstance.cpp",    CppScaffold::gameInstanceSource());
	ok &= writeTextFileIfAbsent(source / "GameLogic.cpp",       CppScaffold::gameLogicSource(startupSceneName));
	ok &= writeTextFileIfAbsent(source / "CMakeLists.txt",      CppScaffold::cmakeLists(HE::tools::cppIdentifier(projectName)));
	ok &= writeTextFileIfAbsent(source / "README.md",          CppScaffold::readme(projectName));
	ok &= writeCppLevelScript(projectRoot, startupSceneName);
	return ok;
}

// ─── Startup scene ────────────────────────────────────────────────────────────
// The Content/StartupScene.hescene every new project gets. Root entity "World"
// (no parent); Game/Simulation/Tutorial additionally get the dedicated
// "Sky" (EnvironmentComponent) and "Weather" (WeatherComponent) entities, and
// Tutorial gets a furnished sandbox on top of that — the guided tour's first
// chapters are about selecting, moving and re-materialling something, which needs
// there to *be* something.
//
// An empty component object lets the scene loader fill every field from the
// struct defaults, so this stays decoupled from the components' exact field lists;
// the tutorial entities only spell out the fields whose defaults are wrong for
// them. Empty/Tool projects start with a bare world (add a Sky via the editor's
// Environment window).
static json startupSceneJson(ProjectPreset preset)
{
	const bool seedEnvironment = (preset == ProjectPreset::Game ||
	                              preset == ProjectPreset::Simulation ||
	                              preset == ProjectPreset::Tutorial ||
	                              preset == ProjectPreset::ThirdPerson);
	const bool furnish  = (preset == ProjectPreset::Tutorial);
	// The third-person starter's scene: somewhere to stand and something to see
	// it by. Deliberately NOT the tutorial's furniture — the tour's cube and its
	// point light are props for the tour's steps, and a template you build a game
	// on should start with the floor and nothing to delete.
	const bool playable = (preset == ProjectPreset::ThirdPerson);

	auto vec3 = [](float x, float y, float z) { return json::array({ x, y, z }); };
	// Matches SceneSerializer's uuidToJson: [hi, lo].
	auto uuid = [](const HE::UUID& id) { return json::array({ id.hi, id.lo }); };
	auto transform = [&](json position, json scale)
	{
		return json{ { "position", std::move(position) },
		             { "rotation", vec3(0.0f, 0.0f, 0.0f) },
		             { "scale",    std::move(scale) } };
	};

	json entities = json::array();
	json rootChildren = json::array();

	// Entities are addressed by stable UUID, matching what SceneSerializer writes —
	// see EntityIdComponent.h for why a handle would make the file merge-hostile.
	// Minting them here (rather than emitting the old uint32 handles and letting
	// the loader's legacy path pick them up) means a new project's scene is in the
	// current format from the first commit instead of the first save.
	const HE::UUID rootId = HE::UUID::generate();

	json rootEntity;
	rootEntity["uuid"]   = uuid(rootId);
	rootEntity["name"]   = "World";
	rootEntity["parent"] = nullptr;   // null parent is how the loader finds the root
	// Filled in below once the children are known — pushed last so the array is
	// complete, but kept at index 0 of `entities` for readability of the file.
	entities.push_back(rootEntity);

	auto addChild = [&](const char* name, json components)
	{
		const HE::UUID id = HE::UUID::generate();
		rootChildren.push_back(uuid(id));
		json e;
		e["uuid"]       = uuid(id);
		e["name"]       = name;
		e["parent"]     = uuid(rootId);
		e["children"]   = json::array();
		e["components"] = std::move(components);
		entities.push_back(std::move(e));
	};

	if (seedEnvironment)
	{
		// The tutorial's Sky is the one seeded environment that is not left at the
		// struct defaults. Those are a dome sky with the day-night cycle OFF, which
		// means timeOfDay does nothing at all (the renderer then takes the sun from
		// the scene's own directional light — see EnvironmentSettings.h) and the
		// frame is flat and shadowless. Turning the cycle on is what makes the
		// tour's "drag the time-of-day slider" step actually move the sun.
		// Keys are EnvironmentComponent member names (HE_ENV_FIELDS_* /
		// SceneSerializer); anything left out falls back to the struct default.
		addChild("Sky", json{ { "environment", (furnish || playable)
			? json{ { "dayNightCycle", true  },   // without this timeOfDay is inert
			        { "timeOfDay",     0.32f },   // mid-morning: raking light, long shadows
			        { "cloudMode",     1     },   // volumetric clouds rather than the dome
			        { "cloudCoverage", 0.4f  } }
			: json::object() } });
		addChild("Weather", json{ { "weather", json::object() } });
	}

	if (playable)
	{
		// Ground you can actually stand on, which is the difference between this
		// and the tutorial's floor. A flattened cube rather than a quad — the
		// built-in quad is a 1x1 billboard facing +Z, and there is no ground
		// primitive.
		//
		// A Static rigid body and DELIBERATELY NO ColliderComponent. That is not
		// an omission, it is the only shape that matches: a Box collider uses its
		// AUTHORED half extents and ignores the entity's scale entirely (see the
		// KNOWN LIMITATION note in PhysicsWorld::buildColliderShape — Mesh, Hull
		// and Height Field scale, the three primitives do not). An authored box
		// under a 60x0.5x60 floor was therefore a 1 m cube in the middle of it,
		// and the player walked off it and fell through the world. Left without
		// one, the fallback builds the box from the composed world scale, and
		// since the built-in cube spans exactly ±0.5 that box IS the visible
		// geometry, to the millimetre.
		addChild("Ground", json{
			{ "transform", transform(vec3(0.0f, -0.25f, 0.0f), vec3(60.0f, 0.5f, 60.0f)) },
			{ "mesh",      json{ { "asset", uuid(HE::kDefaultCubeMeshId) } } },
			{ "material",  json{ { "asset", uuid(HE::kDefaultTerrainMaterialId) } } },
			{ "rigidbody", json{ { "type", static_cast<uint8_t>(HE::RigidBodyType::Static) } } },
		});
		// Two boxes to walk around and jump onto — the smallest thing that makes
		// movement legible. Without something at a fixed place, a player on an
		// empty plane cannot tell whether they are moving at all. Same reason as
		// the ground for having no explicit collider.
		addChild("Block", json{
			{ "transform", transform(vec3(4.0f, 0.5f, -3.0f), vec3(2.0f, 1.0f, 2.0f)) },
			{ "mesh",      json{ { "asset", uuid(HE::kDefaultCubeMeshId) } } },
			{ "material",  json{ { "asset", uuid(HE::kDefaultMaterialId) } } },
			{ "rigidbody", json{ { "type", static_cast<uint8_t>(HE::RigidBodyType::Static) } } },
		});
		addChild("Step", json{
			{ "transform", transform(vec3(-3.5f, 0.25f, -4.5f), vec3(3.0f, 0.5f, 3.0f)) },
			{ "mesh",      json{ { "asset", uuid(HE::kDefaultCubeMeshId) } } },
			{ "material",  json{ { "asset", uuid(HE::kDefaultMaterialId) } } },
			{ "rigidbody", json{ { "type", static_cast<uint8_t>(HE::RigidBodyType::Static) } } },
		});
		// No camera entity here, and that is deliberate: the character class
		// brings its own camera as a child, aimed by a rig that follows whoever
		// is possessed. A second main camera in the scene would win or lose the
		// coin toss depending on iteration order.
	}

	if (furnish)
	{
		// A wide, flattened cube rather than the Plane primitive: it has thickness,
		// so a box collider added during the physics chapter behaves sensibly and
		// the tutorial's falling cube lands on something instead of through it.
		// The neutral-grey terrain material, not the white default one — on the
		// default material the cube would be white geometry on a white floor.
		addChild("Ground", json{
			{ "transform", transform(vec3(0.0f, -0.25f, 0.0f), vec3(24.0f, 0.5f, 24.0f)) },
			{ "mesh",      json{ { "asset", uuid(HE::kDefaultCubeMeshId) } } },
			{ "material",  json{ { "asset", uuid(HE::kDefaultTerrainMaterialId) } } },
		});
		addChild("Cube", json{
			{ "transform", transform(vec3(0.0f, 1.0f, 0.0f), vec3(1.0f, 1.0f, 1.0f)) },
			{ "mesh",      json{ { "asset", uuid(HE::kDefaultCubeMeshId) } } },
			{ "material",  json{ { "asset", uuid(HE::kDefaultMaterialId) } } },
		});
		addChild("Point Light", json{
			{ "transform", transform(vec3(2.5f, 3.0f, 2.5f), vec3(1.0f, 1.0f, 1.0f)) },
			{ "light",     json{ { "type",      static_cast<uint8_t>(HE::LightType::Point) },
			                     { "color",     vec3(1.0f, 0.85f, 0.7f) },
			                     { "intensity", 4.0f },
			                     { "range",     12.0f } } },
		});
	}

	entities[0]["children"] = std::move(rootChildren);

	json scene;
	scene["version"]  = "1.1";
	scene["entities"] = std::move(entities);
	return scene;
}

bool ProjectManager::createNewProject(const std::string& projectDir,
									  const std::string& projectName,
									  ProjectPreset preset,
									  ProjectScriptLanguage scriptLanguage,
									  bool appProject,
									  bool advancedShaderEffects)
{
	fs::path root(projectDir);
	if (!fs::exists(root))
	{
		if (!fs::create_directories(root))
			return false;
	}

	// The third-person starter IS two HorizonCode classes, and PlayerHost only
	// ever scans HorizonCode class assets for a controller. Handing this template
	// to a Lua or Python project would produce a project that says "playable" and
	// has no player — the exact silent hole this template exists to close. So the
	// language follows the template rather than the picker, and says so.
	if (preset == ProjectPreset::ThirdPerson &&
	    scriptLanguage != ProjectScriptLanguage::HorizonCode)
	{
		HE_LOG_INFO(Config, "%s",
			"Third Person template: scripting language set to HorizonCode - the "
			"controller and character it ships are visual-script classes, and a "
			"player cannot be discovered in another language yet");
		scriptLanguage = ProjectScriptLanguage::HorizonCode;
	}

	// ── Common sub-folders ────────────────────────────────────────────────────
	fs::create_directories(root / "Content");
	fs::create_directories(root / "Config");
	fs::create_directories(root / "Shaders");

	// ── Preset-specific sub-folders ───────────────────────────────────────────
	switch (preset)
	{
	// The tutorial sandbox is a Game project with a furnished scene — the tour
	// walks through scripts, audio, models, materials, prefabs and UI, so every
	// folder it mentions has to already be there to put things in.
	// Named explicitly rather than left to `default:` below — that default
	// suppresses -Wswitch, so a new preset would compile clean and quietly get
	// the Empty folder layout.
	case ProjectPreset::ThirdPerson:
		// Where the template's own assets go. Gameplay and Input are the two
		// folders a player is actually made of, and having them already there is
		// half of what makes the layout readable.
		fs::create_directories(root / "Content" / "Gameplay");
		fs::create_directories(root / "Content" / "Input");
		[[fallthrough]];
	case ProjectPreset::Tutorial:
	case ProjectPreset::Game:
		fs::create_directories(root / "Content" / "Scripts");
		fs::create_directories(root / "Content" / "Audio");
		fs::create_directories(root / "Content" / "Scenes");
		fs::create_directories(root / "Content" / "Models");
		fs::create_directories(root / "Content" / "Textures");
		fs::create_directories(root / "Content" / "Materials");
		fs::create_directories(root / "Content" / "Prefabs");
		fs::create_directories(root / "Content" / "UI");
		break;
	case ProjectPreset::Simulation:
		fs::create_directories(root / "Content" / "Data");
		fs::create_directories(root / "Content" / "Materials");
		break;
	case ProjectPreset::Tool:
		fs::create_directories(root / "Content" / "Source");
		fs::create_directories(root / "Content" / "UI");
		break;
	// An application is UI and the things UI draws with. No Models, no Scenes,
	// no Materials — the last of those only when Advanced Shader Effects are on,
	// and even then a folder can be made when the first one is.
	case ProjectPreset::Application:
		fs::create_directories(root / "Content" / "UI");
		fs::create_directories(root / "Content" / "Textures");
		fs::create_directories(root / "Content" / "Fonts");
		break;
	case ProjectPreset::Empty:
	default:
		break;
	}

	// Text-scripting languages get a Scripts folder regardless of preset — the
	// natural home for the project's first .hasset script assets.
	if (scriptLanguage == ProjectScriptLanguage::Lua ||
	    scriptLanguage == ProjectScriptLanguage::Python)
		fs::create_directories(root / "Content" / "Scripts");

	// ── Write .heproj manifest ─────────────────────────────────────────────────
	// Choosing the Application preset IS the decision that this is an app, so the
	// caller's flag and the preset are merged here rather than left able to
	// disagree.
	const bool isApp = appProject || preset == ProjectPreset::Application;

	// Default startup scene: Content/StartupScene.hescene — except for an
	// application, which has no world to load one into. Its startupScene stays
	// empty, and the runtime's app mode never looks for one.
	fs::path scenePath;
	if (!isApp)
	{
		scenePath = root / "Content" / "StartupScene.hescene";
		std::ofstream sceneOut(scenePath);
		if (sceneOut.is_open())
			sceneOut << startupSceneJson(preset).dump(4);
	}

	json j;
	j["name"]           = projectName;
	j["id"]             = newProjectId();
	j["version"]        = "1.0";
	j["preset"]         = static_cast<int>(preset);
	j["startupScene"]   = isApp ? std::string() : std::string("Content/StartupScene.hescene");
	j["scriptLanguage"] = HE::tools::toString(scriptLanguage);
	j["appProject"]            = isApp;
	j["advancedShaderEffects"] = advancedShaderEffects;

	// Seed the default packaging profiles so Build > Export works out of the box.
	const auto profiles = defaultExportProfiles();
	json jp = json::array();
	for (const auto& p : profiles) jp.push_back(profileToJson(p));
	j["exportProfiles"]      = std::move(jp);
	j["activeExportProfile"] = profiles.front().name;

	fs::path heprojPath = root / (projectName + ".heproj");
	std::ofstream out(heprojPath);
	if (!out.is_open())
		return false;
	out << j.dump(4);
	out.close();

	// A C++ project authors gameplay as a native GameLogic library, not engine
	// assets: lay down a compilable Source/ tree (runtime + GameInstance + a level
	// script for the startup scene + CMakeLists). Non-fatal on failure — the
	// project is still usable; the user can regenerate the scaffold.
	if (scriptLanguage == ProjectScriptLanguage::Cpp)
		scaffoldCppProject(root.string(), projectName, scenePath.stem().string());

	// Same deal for the tutorial sandbox's TUTORIAL.md: nice to have, never a
	// reason to fail a project that is otherwise complete on disk.
	if (preset == ProjectPreset::Tutorial)
		scaffoldTutorialProject(root.string(), projectName);

	// The third-person starter's six assets. NOT non-fatal like the two above: a
	// project created from this template and missing its controller opens on a
	// scene with no player in it, which is indistinguishable from the template
	// never having worked. Better to fail the creation and say so.
	if (preset == ProjectPreset::ThirdPerson && !scaffoldThirdPersonProject(root.string()))
	{
		HE_LOG_ERROR(Config, "%s",
			"Third Person template: could not write its gameplay and input assets - "
			"the project would open without a player");
		return false;
	}

	// An application's starter content, on the SOFT terms the two above the
	// third-person block use, and deliberately not on its hard one: a project
	// that opens with an empty preview is worse than one that opens with a label
	// on a panel, but neither is a reason to refuse a project whose folders and
	// manifest are already on disk. The difference is that a missing controller
	// leaves a scene that looks finished and is not, while a missing root widget
	// leaves a blank window nobody could mistake for working.
	if (isApp)
	{
		if (!writeRootWidgetAsset(root, projectName))
			HE_LOG_WARN(Config, "%s", "Application template: could not write the root widget "
			                          "asset — the preview will start empty");
		if (!writeAppGameInstance(root))
			HE_LOG_WARN(Config, "%s", "Application template: could not write GameInstance.hcode "
			                          "— nothing will create the root widget");
	}

	m_currentProject.name                = projectName;
	m_currentProject.path                = heprojPath.string();
	m_currentProject.startupScene        = scenePath.string();   // empty for an application
	m_currentProject.exportProfiles      = profiles;
	m_currentProject.activeExportProfile = profiles.front().name;
	m_currentProject.scriptLanguage      = scriptLanguage;
	m_currentProject.appProject            = isApp;
	m_currentProject.advancedShaderEffects = advancedShaderEffects;
	HE_LOG_INFO(Config, "Created project '%s' at '%s': language %s, preset %d, "
	                    "%zu export profile(s), startup scene '%s', kind %s, "
	                    "advanced shader effects %s",
	            m_currentProject.name.c_str(), m_currentProject.path.c_str(),
	            HE::tools::toString(m_currentProject.scriptLanguage),
	            static_cast<int>(preset), m_currentProject.exportProfiles.size(),
	            m_currentProject.startupScene.c_str(),
	            isApp ? "application" : "game",
	            advancedShaderEffects ? "on" : "off");

	if (m_onProjectLoaded)
		m_onProjectLoaded(m_currentProject.startupScene);
	return true;
}

bool ProjectManager::loadProject(const std::string& projectPath)
{
	// Opening a project is the first thing a user does; "nothing happened" with no
	// reason is the worst possible answer, so each rejection says which it was.
	if (!fs::exists(projectPath))
	{
		HE_LOG_ERROR(Config, "Cannot open project: '%s' does not exist", projectPath.c_str());
		return false;
	}

	std::ifstream in(projectPath);
	if (!in.is_open())
	{
		HE_LOG_ERROR(Config, "Cannot open project '%s' for reading (permissions?)",
		             projectPath.c_str());
		return false;
	}

	json j = json::parse(in, nullptr, false);
	if (j.is_discarded())
	{
		HE_LOG_ERROR(Config, "Project file '%s' is not valid JSON — it is corrupt or was "
		                     "written by a different tool", projectPath.c_str());
		return false;
	}

	m_currentProject.name = jsonString(j, "name", fs::path(projectPath).stem().string());
	m_currentProject.path = projectPath;

	// Projects created before the manifest had an "id" get one now, and it is
	// written back immediately: an identity that is re-minted on every load
	// identifies nothing, and collaboration would reject two editors that DO
	// have the same project open. Failing to write it back is not fatal — the
	// project still opens, it just cannot be matched against a peer's.
	m_currentProject.id = jsonString(j, "id");
	bool mintedId = false;
	if (m_currentProject.id.empty())
	{
		m_currentProject.id = newProjectId();
		mintedId = true;
	}

	// Resolve startup scene relative to the project root
	fs::path projectRoot = fs::path(projectPath).parent_path();
	std::string relScene = jsonString(j, "startupScene");
	if (!relScene.empty())
	{
		fs::path absScene = projectRoot / relScene;
		m_currentProject.startupScene = fs::exists(absScene) ? absScene.string() : "";
		if (m_currentProject.startupScene.empty())
			HE_LOG_WARN(Config, "Project '%s' names startup scene '%s', which does not "
			                    "exist — the editor will open with an empty scene",
			            m_currentProject.name.c_str(), relScene.c_str());
	}
	else
	{
		m_currentProject.startupScene = "";
	}

	readProfiles(j, m_currentProject);
	m_currentProject.scriptLanguage =
		HE::tools::projectScriptLanguageFromString(jsonString(j, "scriptLanguage"));
	m_currentProject.defaultSaveTemplate = jsonString(j, "defaultSaveTemplate");
	m_currentProject.theme     = jsonString(j, "theme");
	m_currentProject.themeMode = jsonString(j, "themeMode");
	// Absent keys mean a project written before applications existed: a game,
	// with materials. Both defaults therefore have to be the game's answer.
	m_currentProject.appProject            = jsonBool(j, "appProject", false);
	m_currentProject.advancedShaderEffects = jsonBool(j, "advancedShaderEffects", true);

	// The id is in here because collaboration compares it and nothing else shows
	// it. A joiner refused for "a different project" otherwise has no way to see
	// what the two sides actually hold — and comparing the .heproj files by hand
	// does not settle it, since an id minted in memory because the file was not
	// writable differs from the one on disk.
	HE_LOG_INFO(Config, "Project '%s' loaded from '%s': id %s%s, language %s, "
	                    "%zu export profile(s), startup scene '%s'",
	            m_currentProject.name.c_str(), projectPath.c_str(),
	            m_currentProject.id.empty() ? "(none)" : m_currentProject.id.c_str(),
	            mintedId ? " (freshly minted, NOT the value in the file)" : "",
	            HE::tools::toString(m_currentProject.scriptLanguage),
	            m_currentProject.exportProfiles.size(),
	            m_currentProject.startupScene.empty() ? "(none)"
	                                                  : m_currentProject.startupScene.c_str());

	if (mintedId && !saveProject(projectPath))
		HE_LOG_WARN(Config, "Project '%s' had no id and it could not be written back — "
		                    "collaboration cannot verify that peers share this project",
		            m_currentProject.name.c_str());

	if (m_onProjectLoaded)
		m_onProjectLoaded(m_currentProject.startupScene);
	return true;
}

bool ProjectManager::saveProject(const std::string& projectPath)
{
	// Read-modify-write: preserve every key the manifest already has (preset,
	// startupScene, future fields) and only overwrite what ProjectData owns.
	json j;
	if (fs::exists(projectPath))
	{
		// An existing manifest we cannot read or parse must NOT be clobbered
		// with a fresh skeleton — it may be hand-recoverable.
		std::ifstream in(projectPath);
		if (!in.is_open())
		{
			HE_LOG_ERROR(Config, "Project save aborted: '%s' exists but cannot be read",
			             projectPath.c_str());
			return false;
		}
		json existing = json::parse(in, nullptr, false);
		if (existing.is_discarded() || !existing.is_object())
		{
			HE_LOG_ERROR(Config, "Project save aborted: '%s' is corrupt and would be "
			                     "overwritten — fix or remove it first, it may still be "
			                     "recoverable by hand", projectPath.c_str());
			return false;
		}
		j = std::move(existing);
	}

	j["name"]    = m_currentProject.name;
	if (!m_currentProject.id.empty()) j["id"] = m_currentProject.id;
	if (!j.contains("version")) j["version"] = "1.0";

	json jp = json::array();
	for (const auto& p : m_currentProject.exportProfiles)
		jp.push_back(profileToJson(p));
	j["exportProfiles"]      = std::move(jp);
	j["activeExportProfile"] = m_currentProject.activeExportProfile;
	j["scriptLanguage"]      = HE::tools::toString(m_currentProject.scriptLanguage);
	j["defaultSaveTemplate"] = m_currentProject.defaultSaveTemplate;
	// Only once chosen, like every other optional key here: a project that never
	// touched a theme keeps a .heproj that looks exactly as it did.
	if (!m_currentProject.theme.empty())     j["theme"]     = m_currentProject.theme;
	if (!m_currentProject.themeMode.empty()) j["themeMode"] = m_currentProject.themeMode;
	// Application flags. Written unconditionally so the file always states what
	// it is, rather than a missing key having to mean "game" forever.
	j["appProject"]            = m_currentProject.appProject;
	j["advancedShaderEffects"] = m_currentProject.advancedShaderEffects;

	// Write temp + rename: an in-place ofstream truncates the only copy before
	// the new content is durable, so disk-full/kill mid-write would leave an
	// empty manifest. rename() swaps atomically on POSIX.
	const std::string tmpPath = projectPath + ".tmp";
	{
		std::ofstream out(tmpPath, std::ios::trunc);
		if (!out.is_open())
		{
			HE_LOG_ERROR(Config, "Project save failed: cannot create '%s'", tmpPath.c_str());
			return false;
		}
		// "replace" error handler: default dump() throws type_error.316 on invalid
		// UTF-8 (e.g. a project name), which would abort the app instead of saving.
		out << j.dump(4, ' ', false, json::error_handler_t::replace);
		out.flush();
		if (!out.good())
		{
			out.close();
			std::error_code ec;
			fs::remove(tmpPath, ec);
			HE_LOG_ERROR(Config, "Project save failed while writing '%s' (disk full?) — "
			                     "the previous file is intact", tmpPath.c_str());
			return false;
		}
	}
	std::error_code ec;
	fs::rename(tmpPath, projectPath, ec);
	if (ec)
	{
		std::error_code ec2;
		fs::remove(tmpPath, ec2);
		HE_LOG_ERROR(Config, "Project save failed: could not replace '%s' (%s) — "
		                     "the previous file is intact",
		             projectPath.c_str(), ec.message().c_str());
		return false;
	}
	HE_LOG_INFO(Config, "Project saved to '%s'", projectPath.c_str());
	return true;
}

void ProjectManager::closeProject()
{
	if (!m_currentProject.name.empty())
		HE_LOG_INFO(Config, "Project '%s' closed", m_currentProject.name.c_str());
	m_currentProject = {};

}
