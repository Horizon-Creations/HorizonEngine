#include "doctest.h"

#include "McpToolRegistry.h"
#include "AssetStubWriter.h"
#include "EditorAssetTypeCache.h"
#include "EditorCommands.h"   // entityByUuid / uuidOf — how a test addresses what a client addresses
#include "TestFsUtil.h"

#include <ContentManager/ContentManager.h>
#include <HorizonScene/Components/NameComponent.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSerializer.h>

#include <filesystem>
#include <fstream>
#include <string>

// ─── Making a scene outlast the session, from outside the editor ─────────────
// Everything the entity tools do lives in memory. These three are what turns
// that into a project, and the questions worth asking of them are the ones where
// a shortcut is invisible until much later:
//
//   • does the round trip actually round-trip — is what scene_save wrote what
//     scene_open reads back, with the same entities under the SAME uuids (a
//     client's addresses are uuids, so a save that renamed them silently would
//     break every reference the client is holding),
//   • is a scene the tools create a scene the editor can OPEN — the trap the
//     Content Browser was in until now: it wrote an HAsset stub at a .hescene
//     path, which SceneSerializer reads as "not valid JSON",
//   • is the save prompt a human gets actually replicated, or does an unsaved
//     scene quietly disappear when a client opens another one,
//   • is a refused compound call (create + open) left half-done,
//   • is the content root a boundary here too, or only in the asset tools.

using HE::Ed::McpSceneHooks;
using HE::Ed::McpTool;
using HE::Ed::McpToolRegistry;
using HE::Ed::ToolResult;
using nlohmann::json;

namespace fs = std::filesystem;

namespace {

// The editor's scene bookkeeping, as the four pieces of state the tools ask
// about, over a REAL world and a REAL serializer: the hooks do what
// EditorApplication::saveSceneToPath and ::openScene do, minus the thumbnail and
// the material warmup (which need a renderer and answer no question here).
struct Fixture
{
	fs::path        root;
	ContentManager  content;
	McpToolRegistry registry;

	HorizonWorld    world;
	std::string     currentScenePath;    // absolute, "" = never saved
	bool            dirty   = false;
	bool            playing = false;
	bool            session = false;

	int         publishedCreates = 0;
	int         appearedCallbacks = 0;

	explicit Fixture(const std::string& name)
	{
		root = fs::temp_directory_path() /
		       ("he_test_mcp_scene_" + name + "_" + std::to_string(::rand()));
		fs::create_directories(root);
		content.setContentRoot(root.string());
		EditorAssetTypeCache::invalidateAll();

		McpSceneHooks h;
		h.currentScenePath = [this] { return currentScenePath; };
		h.sceneDirty       = [this] { return dirty; };
		h.isPlaying        = [this] { return playing; };
		h.inSession        = [this] { return session; };
		h.entityCount      = [this] {
			int n = 0;
			world.registry().view<entt::entity>().each([&](auto) { ++n; });
			return n;
		};
		h.rootUuid = [this] { return HE::Ed::uuidOf(world, world.rootEntity()); };
		h.saveScene = [this](const std::string& abs) {
			SceneSerializer s;
			if (!s.save(world, abs, SerializeFormat::JSON)) return false;
			currentScenePath = abs;
			dirty            = false;
			return true;
		};
		// Deliberately the same ORDER as EditorApplication::openScene: clear
		// first, then load, and a failed load leaves an empty world with no path.
		// A test that cleared only on success would be testing a kinder editor
		// than the one that ships.
		h.openScene = [this](const std::string& abs) {
			SceneSerializer s;
			world.clear();
			if (!s.load(world, abs, SerializeFormat::JSON))
			{
				currentScenePath.clear();
				return false;
			}
			currentScenePath = abs;
			dirty            = false;
			return true;
		};
		h.publishCreate   = [this](const std::string&, const std::string&) { ++publishedCreates; };
		h.onAssetAppeared = [this](const std::string&) { ++appearedCallbacks; };

		HE::Ed::registerSceneTools(registry, content, std::move(h));
	}

	~Fixture()
	{
		EditorAssetTypeCache::invalidateAll();
		he_test::removeAllQuiet(root);
	}

	ToolResult call(const char* name, json args)
	{
		const McpTool* t = registry.find(name);
		REQUIRE(t != nullptr);
		return t->handler(args);
	}

	// An entity under the world root, with a name — the thing that has to come
	// back out of the file.
	Entity addNamed(const char* name)
	{
		const Entity e = world.createEntity(name);
		dirty = true;
		return e;
	}

	std::string uuidOf(Entity e) { return HE::Ed::uuidOf(world, e); }

	bool exists(const std::string& rel) const { return fs::exists(root / rel); }
};

} // namespace

// ─── The round trip ──────────────────────────────────────────────────────────

TEST_CASE("mcp scene: save then open returns the same entities under the same uuids")
{
	Fixture f("roundtrip");
	const Entity  crate    = f.addNamed("Crate");
	const std::string crateId = f.uuidOf(crate);
	REQUIRE_FALSE(crateId.empty());

	const ToolResult saved = f.call("scene_save", json{ { "path", "Levels/Main.hescene" } });
	REQUIRE_FALSE(saved.isError);
	CHECK(saved.content["path"] == "Levels/Main.hescene");
	CHECK(saved.content["created"] == true);
	// The state block: a save leaves the scene clean and names the file it is now on.
	CHECK(saved.content["dirty"] == false);
	CHECK(saved.content["scenePath"] == "Levels/Main.hescene");
	CHECK(f.exists("Levels/Main.hescene"));
	// A create is published and the editor is told a file appeared — the same
	// pair asset_create fires, and without them the Content Browser would not
	// show the scene until its next poll.
	CHECK(f.publishedCreates == 1);
	CHECK(f.appearedCallbacks == 1);

	// It is JSON, not an HAsset container. The whole bug this file guards.
	{
		std::ifstream in(f.root / "Levels/Main.hescene");
		REQUIRE(in.is_open());
		char first = 0;
		in >> first;
		CHECK(first == '{');
	}

	// Somewhere else entirely, then back.
	f.call("scene_create", json{ { "path", "Levels/Other.hescene" }, { "open", true } });
	CHECK((HE::Ed::entityByUuid(f.world, crateId) == entt::null));

	const ToolResult opened = f.call("scene_open", json{ { "path", "Levels/Main.hescene" } });
	REQUIRE_FALSE(opened.isError);
	CHECK(opened.content["path"] == "Levels/Main.hescene");
	CHECK(opened.content["dirty"] == false);
	// The identity survived, which is what makes a uuid a client learned before
	// the save still worth holding.
	const Entity back = HE::Ed::entityByUuid(f.world, crateId);
	REQUIRE((back != entt::null));
	CHECK(f.world.registry().get<NameComponent>(back).name == "Crate");
}

TEST_CASE("mcp scene: save without a path writes over the file the scene came from")
{
	Fixture f("resave");
	f.addNamed("First");
	REQUIRE_FALSE(f.call("scene_save", json{ { "path", "Main.hescene" } }).isError);

	f.addNamed("Second");
	const ToolResult again = f.call("scene_save", json::object());
	REQUIRE_FALSE(again.isError);
	CHECK(again.content["path"] == "Main.hescene");
	// Not a new file this time — so nothing is announced twice.
	CHECK(again.content["created"] == false);
	CHECK(f.publishedCreates == 1);

	// And the second entity is really in the file: a fresh world reads both back.
	HorizonWorld    other;
	SceneSerializer s;
	REQUIRE(s.load(other, (f.root / "Main.hescene").string(), SerializeFormat::JSON));
	int named = 0;
	other.registry().view<NameComponent>().each([&](auto, const NameComponent& n) {
		if (n.name == "First" || n.name == "Second") ++named;
	});
	CHECK(named == 2);
}

TEST_CASE("mcp scene: a scene that was never saved has no file to save over")
{
	Fixture f("noscene");
	f.addNamed("Crate");
	const ToolResult r = f.call("scene_save", json::object());
	REQUIRE(r.isError);
	CHECK(r.errorCode == "no_scene");
	// The refusal has to say what to do instead, or a client can only retry it.
	CHECK(r.errorMessage.find("path") != std::string::npos);
}

// ─── Creating one ────────────────────────────────────────────────────────────

TEST_CASE("mcp scene: a created scene is one the editor can actually open")
{
	Fixture f("create");
	const ToolResult made = f.call("scene_create", json{ { "path", "Levels/Arena.hescene" } });
	REQUIRE_FALSE(made.isError);
	CHECK(made.content["opened"] == false);
	CHECK(f.exists("Levels/Arena.hescene"));
	CHECK(f.publishedCreates == 1);
	CHECK(f.appearedCallbacks == 1);

	// Creating does NOT switch the editor: the scene it had is still the one open.
	CHECK(made.content["scenePath"] == "");

	// The claim that matters. Until the scene tools existed, the Content
	// Browser's own "New Scene" wrote an HAsset stub here and this load failed.
	const ToolResult opened = f.call("scene_open", json{ { "path", "Levels/Arena.hescene" } });
	REQUIRE_FALSE(opened.isError);
	// Empty means the World root and nothing else — no sky, no light, no camera,
	// exactly like File > New Scene.
	CHECK(opened.content["entityCount"] == 1);
}

TEST_CASE("mcp scene: an HAsset stub at a .hescene path is refused as a scene, not loaded as one")
{
	Fixture f("stubfile");
	// Precisely what the create menu used to write. Kept as a test rather than
	// only as a fix, because the two writers live in different files and the next
	// person to add a create row will reach for the stub writer first.
	fs::create_directories(f.root / "Levels");
	REQUIRE(HE::Ed::writeAssetStub((f.root / "Levels/Broken.hescene").string(),
	                               "Levels/Broken.hescene", "Broken", HE::AssetType::Scene));

	const ToolResult r = f.call("scene_open", json{ { "path", "Levels/Broken.hescene" } });
	REQUIRE(r.isError);
	CHECK(r.errorCode == "failed");

	// And the same path written by the shared scene writer opens.
	REQUIRE(HE::Ed::writeEmptySceneFile((f.root / "Levels/Good.hescene").string()));
	CHECK_FALSE(f.call("scene_open", json{ { "path", "Levels/Good.hescene" } }).isError);
}

TEST_CASE("mcp scene: create never writes over an existing scene")
{
	Fixture f("exists");
	REQUIRE_FALSE(f.call("scene_create", json{ { "path", "Main.hescene" } }).isError);
	f.call("scene_open", json{ { "path", "Main.hescene" } });
	f.addNamed("Crate");
	REQUIRE_FALSE(f.call("scene_save", json::object()).isError);

	const ToolResult again = f.call("scene_create", json{ { "path", "Main.hescene" } });
	REQUIRE(again.isError);
	CHECK(again.errorCode == "already_exists");
	// The entity is still in the file: the refusal happened before anything was
	// written, not after a truncate.
	HorizonWorld    other;
	SceneSerializer s;
	REQUIRE(s.load(other, (f.root / "Main.hescene").string(), SerializeFormat::JSON));
	int crates = 0;
	other.registry().view<NameComponent>().each([&](auto, const NameComponent& n) {
		if (n.name == "Crate") ++crates;
	});
	CHECK(crates == 1);
}

TEST_CASE("mcp scene: a refused create+open leaves no half-made file behind")
{
	Fixture f("compound");
	const std::string unsavedId = f.uuidOf(f.addNamed("Unsaved"));   // addNamed sets dirty

	const ToolResult r = f.call("scene_create",
	                            json{ { "path", "Levels/New.hescene" }, { "open", true } });
	REQUIRE(r.isError);
	CHECK(r.errorCode == "dirty");
	// The whole call was refused, so there is nothing for the client to reason
	// about afterwards — no file, and nothing announced.
	CHECK_FALSE(f.exists("Levels/New.hescene"));
	CHECK(f.publishedCreates == 0);
	// And the unsaved work is untouched.
	CHECK((HE::Ed::entityByUuid(f.world, unsavedId) != entt::null));

	// Without open=true the same call is fine: it writes a file and touches nothing.
	REQUIRE_FALSE(f.call("scene_create", json{ { "path", "Levels/New.hescene" } }).isError);
	CHECK(f.exists("Levels/New.hescene"));
	CHECK(f.dirty);
}

// ─── The prompt a client cannot see ──────────────────────────────────────────

TEST_CASE("mcp scene: opening another scene over unsaved changes is refused, then allowed")
{
	Fixture f("dirty");
	REQUIRE_FALSE(f.call("scene_create", json{ { "path", "Other.hescene" } }).isError);
	const Entity  work   = f.addNamed("Work");
	const std::string workId = f.uuidOf(work);

	const ToolResult refused = f.call("scene_open", json{ { "path", "Other.hescene" } });
	REQUIRE(refused.isError);
	CHECK(refused.errorCode == "dirty");
	// Refused means refused: the world still holds the unsaved entity.
	CHECK((HE::Ed::entityByUuid(f.world, workId) != entt::null));

	const ToolResult forced = f.call("scene_open",
	                                 json{ { "path", "Other.hescene" },
	                                       { "discard_changes", true } });
	REQUIRE_FALSE(forced.isError);
	CHECK((HE::Ed::entityByUuid(f.world, workId) == entt::null));
	CHECK(forced.content["entityCount"] == 1);
}

TEST_CASE("mcp scene: saving first is the other way out of a dirty scene")
{
	Fixture f("saveout");
	REQUIRE_FALSE(f.call("scene_create", json{ { "path", "Other.hescene" } }).isError);
	f.addNamed("Work");

	REQUIRE_FALSE(f.call("scene_save", json{ { "path", "Work.hescene" } }).isError);
	// Clean now, so no discard_changes is needed.
	CHECK_FALSE(f.call("scene_open", json{ { "path", "Other.hescene" } }).isError);
}

// ─── Play mode ───────────────────────────────────────────────────────────────

TEST_CASE("mcp scene: none of the three run while play-in-editor does")
{
	Fixture f("play");
	REQUIRE_FALSE(f.call("scene_save", json{ { "path", "Main.hescene" } }).isError);
	f.playing = true;

	for (const char* tool : { "scene_save", "scene_create", "scene_open" })
	{
		const ToolResult r = f.call(tool, json{ { "path", "Play.hescene" } });
		REQUIRE(r.isError);
		CHECK(r.errorCode == "play_mode");
	}
	CHECK_FALSE(f.exists("Play.hescene"));
}

// ─── The boundary ────────────────────────────────────────────────────────────

TEST_CASE("mcp scene: the content root is a boundary, not a convention")
{
	Fixture f("confine");

	// '..' out of the root.
	const ToolResult up = f.call("scene_create", json{ { "path", "../escape.hescene" } });
	REQUIRE(up.isError);
	CHECK(up.errorCode == "invalid_path");

	// An absolute path is refused AS one — not silently shortened, or
	// '/Users/someone/secret.hescene' would come back as a plain "not found".
	const ToolResult abs = f.call("scene_open", json{ { "path", "/etc/passwd" } });
	REQUIRE(abs.isError);
	CHECK(abs.errorCode == "invalid_path");

	// The shipped engine content is read-only from a project's perspective.
	const ToolResult eng = f.call("scene_create", json{ { "path", "Engine/Levels/X.hescene" } });
	REQUIRE(eng.isError);
	CHECK(eng.errorCode == "read_only");

	CHECK_FALSE(fs::exists(f.root.parent_path() / "escape.hescene"));
}

TEST_CASE("mcp scene: the extension is added when missing and refused when wrong")
{
	Fixture f("ext");

	// A client that said 'Levels/Main' meant 'Levels/Main.hescene'.
	const ToolResult made = f.call("scene_create", json{ { "path", "Levels/Main" } });
	REQUIRE_FALSE(made.isError);
	CHECK(made.content["path"] == "Levels/Main.hescene");
	CHECK(f.exists("Levels/Main.hescene"));

	// A DIFFERENT extension is a different intent, and correcting it silently
	// would put scene JSON in a file every panel misreads.
	const ToolResult wrong = f.call("scene_create", json{ { "path", "Levels/Main.hasset" } });
	REQUIRE(wrong.isError);
	CHECK(wrong.errorCode == "invalid_path");
	CHECK_FALSE(f.exists("Levels/Main.hasset"));
}

TEST_CASE("mcp scene: opening a scene that is not there is not-found, not a wiped world")
{
	Fixture f("missing");
	f.addNamed("Work");
	REQUIRE_FALSE(f.call("scene_save", json{ { "path", "Main.hescene" } }).isError);
	const int before = static_cast<int>(f.world.registry().storage<entt::entity>().in_use());

	const ToolResult r = f.call("scene_open", json{ { "path", "Levels/Nope.hescene" } });
	REQUIRE(r.isError);
	CHECK(r.errorCode == "not_found");
	// Refused before the world was cleared — the check is on the path, not on the
	// load, so nothing was thrown away to find out the file is missing.
	CHECK(static_cast<int>(f.world.registry().storage<entt::entity>().in_use()) == before);
}

// ─── The interface itself ────────────────────────────────────────────────────

TEST_CASE("mcp scene: all three are registered, mutating and documented")
{
	Fixture f("iface");
	for (const char* name : { "scene_save", "scene_create", "scene_open" })
	{
		const McpTool* t = f.registry.find(name);
		REQUIRE(t != nullptr);
		CHECK(McpToolRegistry::enforceNameRule(t->name));
		// Every one of them changes the project, so every one is refused in play
		// mode by the bridge and written to the console log.
		CHECK(t->mutates);
		CHECK_FALSE(t->description.empty());
		REQUIRE(t->inputSchema.is_object());
		CHECK(t->inputSchema["type"] == "object");
		CHECK(t->inputSchema.contains("properties"));
	}
	// The one required argument each, and scene_save's deliberate lack of one.
	CHECK_FALSE(f.registry.find("scene_save")->inputSchema.contains("required"));
	CHECK(f.registry.find("scene_open")->inputSchema["required"][0] == "path");
	CHECK(f.registry.find("scene_create")->inputSchema["required"][0] == "path");
}
