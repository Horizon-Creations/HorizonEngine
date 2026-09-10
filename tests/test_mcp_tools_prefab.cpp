#include "doctest.h"

#include "AssetStubWriter.h"
#include "CollabUndo.h"
#include "EditorAssetTypeCache.h"
#include "EditorCommands.h"
#include "EditorUndo.h"
#include "StructuralSync.h"
#include "McpToolRegistry.h"
#include "TestFsUtil.h"

#include <ContentManager/AssetRefScan.h>
#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/HAsset.h>

#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/TransformHierarchy.h>
#include <HorizonScene/Components/HierarchyComponent.h>
#include <HorizonScene/Components/LightComponent.h>
#include <HorizonScene/Components/NameComponent.h>
#include <HorizonScene/Components/TransformComponent.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// ─── Placing an authored subtree from outside the editor ─────────────────────
// The prefab tools sit on both halves of the editor at once, so the questions
// worth asking span both:
//
//   • is a placement ONE gateway command — a single undo has to take the whole
//     subtree AND the client's position back out, because a second command for
//     the transform would leave the prefab standing in the scene at its authored
//     spot after one Ctrl+Z,
//   • do two placements of one prefab really get two identities (the blob names
//     the entities it was captured from, and preserving those ids would make the
//     second placement claim the first one's identity),
//   • is what the client did NOT ask for left alone — the authored rotation and
//     scale are part of the prefab, and resetting them because a call named only
//     a position would silently un-author it,
//   • does prefab_save write a file the tools themselves can place again, and
//     does the uuid it reports match the one the file carries (the identity is
//     minted by the save and the registration must not mint a second one),
//   • does a refusal really leave the world and the disk exactly as they were.
//
// The blob shapes a damaged file can have get their own cases: a payload with no
// parentless record and one with two of them are both things the scene loader
// answers to by placing something other than what is in the file.

using HE::Ed::EditorCommands;
using HE::Ed::McpPrefabHooks;
using HE::Ed::McpTool;
using HE::Ed::McpToolRegistry;
using HE::Ed::Origin;
using HE::Ed::ToolResult;
using nlohmann::json;

namespace fs = std::filesystem;

namespace {

std::string codeOf(const ToolResult& r)
{
	return r.isError ? r.errorCode : std::string("<ok>");
}

// The gateway wired the way EditorApplication wires it, plus a real
// ContentManager over a temporary content root — modelled on
// tests/test_mcp_tools_entity.cpp and tests/test_mcp_tools_material.cpp, so
// there is one theory of how the editor behaves rather than three.
struct Fixture
{
	fs::path        root;
	HorizonWorld    world;
	ContentManager  content;
	EditorCommands  cmds;
	EditorUndo      snapshotUndo;
	CollabUndo      collabUndo;
	McpToolRegistry registry;

	HE::Ed::SnapshotUndoSink snapshotSink{ &snapshotUndo, [this] { return playing; } };
	HE::Ed::CollabUndoSink   collabSink{ &collabUndo };

	bool        playing = false;
	std::string lockedRel;   // non-empty = a peer holds it
	int         appeared = 0;
	std::vector<std::string> published;

	explicit Fixture(const std::string& name)
	{
		root = fs::temp_directory_path() /
		       ("he_test_mcp_prefab_" + name + "_" + std::to_string(::rand()));
		fs::create_directories(root);
		content.setContentRoot(root.string());
		EditorAssetTypeCache::invalidateAll();

		snapshotUndo.setWorld(&world);
		cmds.setWorld(&world);

		EditorCommands::Hooks gh;
		gh.isPlaying = [this] { return playing; };
		gh.inSession = [] { return false; };
		gh.nowMs     = [] { return std::uint64_t{ 0 }; };
		cmds.setHooks(std::move(gh));
		cmds.setUndoSinks(&snapshotSink, &collabSink);

		McpPrefabHooks h;
		h.isPlaying     = [this] { return playing; };
		h.lockedByOther = [this](const std::string& rel) {
			return !lockedRel.empty() && rel == lockedRel;
		};
		h.publishCreate = [this](const std::string& rel, const std::string&) {
			published.push_back(rel);
		};
		h.onAssetAppeared = [this](const std::string&) { ++appeared; };
		HE::Ed::registerPrefabTools(registry, content, cmds, std::move(h));
	}

	~Fixture()
	{
		EditorAssetTypeCache::invalidateAll();
		he_test::removeAllQuiet(root);
	}

	ToolResult call(const std::string& name, const json& args = json::object())
	{
		const McpTool* t = registry.find(name);
		REQUIRE_MESSAGE(t != nullptr, "no such tool registered: " << name);
		return t->handler(args);
	}

	std::string uuid(Entity e) { return HE::Ed::uuidOf(world, e); }

	// A lamp post: a root with a transform, one child carrying a light and a
	// grandchild, so a placement that only restores the root, or only one level,
	// is visible. The authored transform is deliberately non-identity in every
	// component — a prefab at [0,0,0] with rotation 0 and scale 1 hides exactly
	// the mistake this file is about.
	Entity makeSubtree(const char* rootName = "Lamp")
	{
		auto& reg = world.registry();
		const Entity post = world.createEntity(rootName);
		TransformComponent xf;
		xf.position = glm::vec3(3.0f, 0.0f, -2.0f);
		xf.rotation = glm::vec3(0.0f, 45.0f, 0.0f);
		xf.scale    = glm::vec3(2.0f, 2.0f, 2.0f);
		reg.emplace<TransformComponent>(post, xf);

		const Entity bulb = world.createEntity("Bulb");
		TransformComponent bxf;
		bxf.position = glm::vec3(0.0f, 4.0f, 0.0f);
		reg.emplace<TransformComponent>(bulb, bxf);
		LightComponent lc;
		lc.color     = glm::vec3(1.0f, 0.5f, 0.0f);
		lc.intensity = 7.5f;
		reg.emplace<LightComponent>(bulb, lc);
		world.reparentEntity(bulb, post);

		const Entity glass = world.createEntity("Glass");
		reg.emplace<TransformComponent>(glass, TransformComponent{});
		world.reparentEntity(glass, bulb);
		return post;
	}

	// A prefab file written the way the Outliner writes one, deliberately NOT
	// through the tools under test.
	std::string writePrefabFile(const std::string& rel, Entity subtreeRoot)
	{
		SceneSerializer ser;
		PrefabAsset a;
		a.type = HE::AssetType::Prefab;
		a.name = fs::path(rel).stem().string();
		a.path = rel;
		a.data = ser.serializeSubtree(world, subtreeRoot);
		REQUIRE(content.saveAsset(a));
		EditorAssetTypeCache::invalidate((root / rel).string());
		return rel;
	}

	// A prefab file carrying a payload this test wrote by hand — the damaged
	// shapes a hand-edited or half-merged file can have.
	std::string writeRawPrefabFile(const std::string& rel, const json& tree)
	{
		PrefabAsset a;
		a.type = HE::AssetType::Prefab;
		a.name = fs::path(rel).stem().string();
		a.path = rel;
		a.data = json::to_cbor(tree);
		REQUIRE(content.saveAsset(a));
		EditorAssetTypeCache::invalidate((root / rel).string());
		return rel;
	}

	std::string bytes(const std::string& rel) const
	{
		std::ifstream f(root / rel, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	}

	// Every entity in the world whose name matches, so a test can say "the
	// subtree really arrived" without holding handles across a snapshot undo.
	int countNamed(const char* name) const
	{
		int n = 0;
		auto& reg = const_cast<HorizonWorld&>(world).registry();
		for (auto e : reg.view<NameComponent>())
			if (reg.get<NameComponent>(e).name == name) ++n;
		return n;
	}

	Entity findNamed(const char* name) const
	{
		auto& reg = const_cast<HorizonWorld&>(world).registry();
		for (auto e : reg.view<NameComponent>())
			if (reg.get<NameComponent>(e).name == name) return e;
		return entt::null;
	}
};

// A one-record blob in the shape buildSubtreeJson writes, with `parent` set on
// as many records as the caller asks for.
json rawTree(int records, bool everyRecordHasParent)
{
	json entities = json::array();
	for (int i = 0; i < records; ++i)
	{
		json e{
			{ "id",   i },
			{ "name", "Rec" + std::to_string(i) },
			{ "components", json{ { "transform", json{
				{ "position", json::array({ 0.0, 0.0, 0.0 }) },
				{ "rotation", json::array({ 0.0, 0.0, 0.0 }) },
				{ "scale",    json::array({ 1.0, 1.0, 1.0 }) },
			} } } },
		};
		if (everyRecordHasParent) e["parent"] = json::array({ 99u, 99u });
		entities.push_back(e);
	}
	return json{ { "version", "1.1" }, { "entities", entities } };
}

} // namespace

// ─── The catalogue ───────────────────────────────────────────────────────────

TEST_CASE("prefab_info lists the project's prefabs and reads one of them")
{
	Fixture f("info");
	const Entity lamp = f.makeSubtree();
	f.writePrefabFile("Prefabs/Lamp.hasset", lamp);
	f.writePrefabFile("Prefabs/Spare.hasset", lamp);
	// A file of another type in the same folder: the catalogue is a type filter,
	// not a directory listing.
	const fs::path matAbs = f.root / "Prefabs/NotAPrefab.hasset";
	REQUIRE(HE::Ed::writeAssetStub(matAbs.string(), "Prefabs/NotAPrefab.hasset",
	                               "NotAPrefab", HE::AssetType::Material));

	const ToolResult all = f.call("prefab_info");
	REQUIRE_MESSAGE(!all.isError, codeOf(all));
	const json& list = all.content["prefabs"];
	REQUIRE(list.size() == 2);
	// Sorted, so two calls on an unchanged project answer identically.
	CHECK(list[0]["path"] == "Prefabs/Lamp.hasset");
	CHECK(list[1]["path"] == "Prefabs/Spare.hasset");
	CHECK(list[0]["name"] == "Lamp");
	// The uuid is the file's own, readable without loading anything.
	const HE::UUID onDisk =
		HE::AssetRefs::assetUuidOfFile((f.root / "Prefabs/Lamp.hasset").string());
	CHECK(list[0]["uuid"][0].get<std::uint64_t>() == onDisk.hi);
	CHECK(list[0]["uuid"][1].get<std::uint64_t>() == onDisk.lo);

	const ToolResult one = f.call("prefab_info", json{ { "path", "Prefabs/Lamp.hasset" } });
	REQUIRE_MESSAGE(!one.isError, codeOf(one));
	CHECK(one.content["entityCount"] == 3);
	CHECK(one.content["root"] == "Lamp");
	CHECK_FALSE(one.content.contains("rootCount"));   // exactly one root
	REQUIRE(one.content["entities"].size() == 3);
	// The component keys of the child are the scene format's own keys, which is
	// what makes them usable in an entity_set_components call afterwards.
	bool sawLight = false;
	for (const json& e : one.content["entities"])
		if (e["name"] == "Bulb")
			for (const json& k : e["components"])
				if (k == "light") sawLight = true;
	CHECK(sawLight);
}

TEST_CASE("prefab_info refuses what is not a prefab, and says what is")
{
	Fixture f("info_refuse");
	const fs::path abs = f.root / "Materials/Rock.hasset";
	fs::create_directories(abs.parent_path());
	REQUIRE(HE::Ed::writeAssetStub(abs.string(), "Materials/Rock.hasset", "Rock",
	                               HE::AssetType::Material));

	const ToolResult wrong = f.call("prefab_info", json{ { "path", "Materials/Rock.hasset" } });
	CHECK(wrong.isError);
	CHECK(wrong.errorCode == "invalid_path");

	const ToolResult missing = f.call("prefab_info", json{ { "path", "Prefabs/Ghost.hasset" } });
	CHECK(missing.isError);
	CHECK(missing.errorCode == "not_found");

	// Outside the content root at all — the one confinement rule, asked through
	// this family too.
	const ToolResult escape = f.call("prefab_info", json{ { "path", "../../etc/passwd" } });
	CHECK(escape.isError);
}

TEST_CASE("A prefab file with no payload is a refusal rather than an empty placement")
{
	Fixture f("empty");
	// The stub writer refuses the type, so this is what a hand-made file looks
	// like: META and nothing else.
	PrefabAsset a;
	a.type = HE::AssetType::Prefab;
	a.name = "Hollow";
	a.path = "Prefabs/Hollow.hasset";
	REQUIRE(f.content.saveAsset(a));
	EditorAssetTypeCache::invalidate((f.root / a.path).string());

	const ToolResult read = f.call("prefab_info", json{ { "path", a.path } });
	CHECK(read.isError);
	CHECK(read.errorCode == "invalid_payload");

	const ToolResult placed = f.call("prefab_instantiate", json{ { "path", a.path } });
	CHECK(placed.isError);
	CHECK(placed.errorCode == "invalid_payload");
	CHECK(f.countNamed("Hollow") == 0);
}

// ─── Placing ─────────────────────────────────────────────────────────────────

TEST_CASE("prefab_instantiate places the whole subtree under the named parent")
{
	Fixture f("place");
	const Entity source = f.makeSubtree();
	f.writePrefabFile("Prefabs/Lamp.hasset", source);
	const Entity holder = f.world.createEntity("Street");
	f.world.registry().emplace<TransformComponent>(holder, TransformComponent{});

	const ToolResult r = f.call("prefab_instantiate", json{
		{ "path",   "Prefabs/Lamp.hasset" },
		{ "parent", f.uuid(holder) },
	});
	REQUIRE_MESSAGE(!r.isError, codeOf(r));
	CHECK(r.content["entityCount"] == 3);
	CHECK(r.content["prefab"] == "Prefabs/Lamp.hasset");
	CHECK(r.content["parent"] == f.uuid(holder));

	const Entity placed = HE::Ed::entityByUuid(f.world, r.content["uuid"]);
	REQUIRE((placed != entt::null));
	CHECK(HE::Ed::structParentOf(f.world.registry(), placed) == holder);

	// Both levels came with it, and the child's own components did too.
	auto& reg = f.world.registry();
	const auto& kids = reg.get<HierarchyComponent>(placed).children;
	REQUIRE(kids.size() == 1);
	CHECK(reg.get<NameComponent>(kids[0]).name == "Bulb");
	CHECK(reg.get<LightComponent>(kids[0]).intensity == doctest::Approx(7.5f));
	REQUIRE(reg.get<HierarchyComponent>(kids[0]).children.size() == 1);

	// The source subtree is untouched — a placement is a copy, not a move.
	CHECK(f.countNamed("Lamp") == 2);
}

TEST_CASE("Two placements of one prefab are two identities")
{
	Fixture f("twice");
	const Entity source = f.makeSubtree();
	f.writePrefabFile("Prefabs/Lamp.hasset", source);

	const ToolResult a = f.call("prefab_instantiate", json{ { "path", "Prefabs/Lamp.hasset" } });
	const ToolResult b = f.call("prefab_instantiate", json{ { "path", "Prefabs/Lamp.hasset" } });
	REQUIRE_MESSAGE(!a.isError, codeOf(a));
	REQUIRE_MESSAGE(!b.isError, codeOf(b));

	const std::string ua = a.content["uuid"], ub = b.content["uuid"];
	CHECK(ua != ub);
	// And neither of them is the uuid of the entity the prefab was captured from,
	// which is what the blob actually carries.
	CHECK(ua != f.uuid(source));
	CHECK(ub != f.uuid(source));
	CHECK((HE::Ed::entityByUuid(f.world, ua) != entt::null));
	CHECK((HE::Ed::entityByUuid(f.world, ub) != entt::null));
	CHECK(f.countNamed("Bulb") == 3);   // the source plus two placements
}

TEST_CASE("Only the axes the client sent are overwritten")
{
	Fixture f("axes");
	const Entity source = f.makeSubtree();
	f.writePrefabFile("Prefabs/Lamp.hasset", source);

	SUBCASE("a position alone leaves the authored rotation and scale")
	{
		const ToolResult r = f.call("prefab_instantiate", json{
			{ "path",     "Prefabs/Lamp.hasset" },
			{ "position", json::array({ 10.0, 1.0, 5.0 }) },
		});
		REQUIRE_MESSAGE(!r.isError, codeOf(r));
		const Entity e = HE::Ed::entityByUuid(f.world, r.content["uuid"]);
		const auto& xf = f.world.registry().get<TransformComponent>(e);
		CHECK(xf.position.x == doctest::Approx(10.0f));
		CHECK(xf.position.z == doctest::Approx(5.0f));
		CHECK(xf.rotation.y == doctest::Approx(45.0f));   // authored
		CHECK(xf.scale.x    == doctest::Approx(2.0f));    // authored
	}

	SUBCASE("nothing at all keeps the prefab exactly as it was authored")
	{
		const ToolResult r = f.call("prefab_instantiate",
		                            json{ { "path", "Prefabs/Lamp.hasset" } });
		REQUIRE_MESSAGE(!r.isError, codeOf(r));
		const Entity e = HE::Ed::entityByUuid(f.world, r.content["uuid"]);
		const auto& xf = f.world.registry().get<TransformComponent>(e);
		CHECK(xf.position.x == doctest::Approx(3.0f));
		CHECK(xf.rotation.y == doctest::Approx(45.0f));
		CHECK(xf.scale.y    == doctest::Approx(2.0f));
	}

	SUBCASE("a name overrides the prefab's own")
	{
		const ToolResult r = f.call("prefab_instantiate", json{
			{ "path", "Prefabs/Lamp.hasset" },
			{ "name", "Corner lamp" },
		});
		REQUIRE_MESSAGE(!r.isError, codeOf(r));
		CHECK(r.content["name"] == "Corner lamp");
		CHECK(f.countNamed("Corner lamp") == 1);
		// The children keep theirs — the override is the root's alone.
		CHECK(f.countNamed("Bulb") == 2);
	}

	SUBCASE("a two-element position is refused, not half-applied")
	{
		// The whole call is refused by the schema-shaped reader rather than
		// applying one axis: a client thinking in 2D would otherwise leave the
		// third axis wherever the prefab left it with nothing to show for it.
		const ToolResult r = f.call("prefab_instantiate", json{
			{ "path",     "Prefabs/Lamp.hasset" },
			{ "position", json::array({ 10.0, 5.0 }) },
		});
		REQUIRE_MESSAGE(!r.isError, codeOf(r));
		const Entity e = HE::Ed::entityByUuid(f.world, r.content["uuid"]);
		// Ignored, so the authored position stands — nothing was half-written.
		CHECK(f.world.registry().get<TransformComponent>(e).position.x
		      == doctest::Approx(3.0f));
	}
}

TEST_CASE("A placement is ONE gateway command, so one undo takes all of it back")
{
	Fixture f("undo");
	const Entity source = f.makeSubtree();
	f.writePrefabFile("Prefabs/Lamp.hasset", source);
	// The source out of the way, so the counts below only see the placement.
	f.world.destroyEntity(source);
	REQUIRE(f.countNamed("Lamp") == 0);

	const ToolResult r = f.call("prefab_instantiate", json{
		{ "path",     "Prefabs/Lamp.hasset" },
		{ "position", json::array({ 12.0, 0.0, 0.0 }) },
	});
	REQUIRE_MESSAGE(!r.isError, codeOf(r));
	const std::string uuid = r.content["uuid"];
	CHECK(f.countNamed("Lamp") == 1);
	CHECK(f.countNamed("Glass") == 1);

	// ONE undo. Two commands (create, then a transform) would leave the lamp
	// standing here at its authored position — the mistake this asserts against.
	REQUIRE(f.snapshotUndo.undo());
	CHECK((HE::Ed::entityByUuid(f.world, uuid) == entt::null));
	CHECK(f.countNamed("Lamp") == 0);
	CHECK(f.countNamed("Bulb") == 0);
	CHECK(f.countNamed("Glass") == 0);
}

TEST_CASE("Placing is refused while play-in-editor runs, and nothing is placed")
{
	Fixture f("play");
	const Entity source = f.makeSubtree();
	f.writePrefabFile("Prefabs/Lamp.hasset", source);
	f.world.destroyEntity(source);

	f.playing = true;
	const ToolResult r = f.call("prefab_instantiate", json{ { "path", "Prefabs/Lamp.hasset" } });
	CHECK(r.isError);
	CHECK(r.errorCode == "play_mode");
	CHECK(f.countNamed("Lamp") == 0);
}

TEST_CASE("A damaged payload is refused rather than half-placed")
{
	SUBCASE("no record without a parent — the loader would refuse the whole thing")
	{
		Fixture f("noroot");
		f.writeRawPrefabFile("Prefabs/Broken.hasset",
		                     rawTree(2, /*everyRecordHasParent=*/true));
		const ToolResult r = f.call("prefab_instantiate",
		                            json{ { "path", "Prefabs/Broken.hasset" } });
		CHECK(r.isError);
		CHECK(r.errorCode == "invalid_payload");
		CHECK(f.countNamed("Rec0") == 0);
	}

	SUBCASE("two records without a parent — the loader would strand one of them")
	{
		Fixture f("tworoots");
		f.writeRawPrefabFile("Prefabs/Twins.hasset",
		                     rawTree(2, /*everyRecordHasParent=*/false));
		// The reader says so rather than pretending the file is fine…
		const ToolResult read = f.call("prefab_info", json{ { "path", "Prefabs/Twins.hasset" } });
		REQUIRE_MESSAGE(!read.isError, codeOf(read));
		CHECK(read.content["rootCount"] == 2);
		// …and the writer refuses it.
		const ToolResult r = f.call("prefab_instantiate",
		                            json{ { "path", "Prefabs/Twins.hasset" } });
		CHECK(r.isError);
		CHECK(r.errorCode == "invalid_payload");
		CHECK(f.countNamed("Rec0") == 0);
		CHECK(f.countNamed("Rec1") == 0);
	}
}

TEST_CASE("An unknown parent uuid is a refusal, not a placement at the top level")
{
	Fixture f("badparent");
	const Entity source = f.makeSubtree();
	f.writePrefabFile("Prefabs/Lamp.hasset", source);
	f.world.destroyEntity(source);

	const ToolResult r = f.call("prefab_instantiate", json{
		{ "path",   "Prefabs/Lamp.hasset" },
		{ "parent", "00000000-0000-0000-0000-000000000001" },
	});
	CHECK(r.isError);
	CHECK(r.errorCode == "not_found");
	CHECK(f.countNamed("Lamp") == 0);
}

// ─── Capturing ───────────────────────────────────────────────────────────────

TEST_CASE("prefab_save writes a file the tools can place again")
{
	Fixture f("save");
	const Entity source = f.makeSubtree();

	const ToolResult saved = f.call("prefab_save", json{ { "uuid", f.uuid(source) } });
	REQUIRE_MESSAGE(!saved.isError, codeOf(saved));
	// The Outliner's own default path.
	CHECK(saved.content["path"] == "Prefabs/Lamp.hasset");
	CHECK(saved.content["entityCount"] == 3);
	CHECK(fs::exists(f.root / "Prefabs/Lamp.hasset"));
	// The editor heard about it, and the session did too.
	CHECK(f.appeared == 1);
	REQUIRE(f.published.size() == 1);
	CHECK(f.published[0] == "Prefabs/Lamp.hasset");

	// The uuid in the answer is the one the FILE carries. The save mints it and
	// the registration must not mint a second one — otherwise the path→uuid entry
	// and the file disagree and a later placement resolves a stranger.
	const HE::UUID onDisk =
		HE::AssetRefs::assetUuidOfFile((f.root / "Prefabs/Lamp.hasset").string());
	CHECK(saved.content["uuid"][0].get<std::uint64_t>() == onDisk.hi);
	CHECK(saved.content["uuid"][1].get<std::uint64_t>() == onDisk.lo);
	CHECK_FALSE(onDisk == HE::UUID{});

	// And the round trip: place what was just captured and the tree comes back.
	f.world.destroyEntity(source);
	const ToolResult placed = f.call("prefab_instantiate",
	                                 json{ { "path", "Prefabs/Lamp.hasset" } });
	REQUIRE_MESSAGE(!placed.isError, codeOf(placed));
	const Entity e = HE::Ed::entityByUuid(f.world, placed.content["uuid"]);
	REQUIRE((e != entt::null));
	auto& reg = f.world.registry();
	CHECK(reg.get<TransformComponent>(e).rotation.y == doctest::Approx(45.0f));
	const auto& kids = reg.get<HierarchyComponent>(e).children;
	REQUIRE(kids.size() == 1);
	CHECK(reg.get<LightComponent>(kids[0]).intensity == doctest::Approx(7.5f));
}

TEST_CASE("prefab_save uniquifies its default name instead of overwriting")
{
	Fixture f("unique");
	const Entity source = f.makeSubtree();
	REQUIRE_FALSE(f.call("prefab_save", json{ { "uuid", f.uuid(source) } }).isError);
	const ToolResult second = f.call("prefab_save", json{ { "uuid", f.uuid(source) } });
	REQUIRE_MESSAGE(!second.isError, codeOf(second));
	CHECK(second.content["path"] == "Prefabs/Lamp1.hasset");
	CHECK(fs::exists(f.root / "Prefabs/Lamp.hasset"));
	CHECK(fs::exists(f.root / "Prefabs/Lamp1.hasset"));
}

TEST_CASE("An explicit path that is taken is refused, and the file is untouched")
{
	Fixture f("taken");
	const Entity source = f.makeSubtree();
	f.writePrefabFile("Prefabs/Lamp.hasset", source);
	const std::string before = f.bytes("Prefabs/Lamp.hasset");

	const ToolResult r = f.call("prefab_save", json{
		{ "uuid", f.uuid(source) },
		{ "path", "Prefabs/Lamp.hasset" },
	});
	CHECK(r.isError);
	CHECK(r.errorCode == "already_exists");
	CHECK(f.bytes("Prefabs/Lamp.hasset") == before);
	CHECK(f.published.empty());
}

TEST_CASE("Every refusal prefab_save can give leaves the disk alone")
{
	Fixture f("save_refusals");
	const Entity source = f.makeSubtree();
	const std::string uuid = f.uuid(source);

	SUBCASE("play mode")
	{
		f.playing = true;
		const ToolResult r = f.call("prefab_save", json{ { "uuid", uuid } });
		CHECK(r.isError);
		CHECK(r.errorCode == "play_mode");
	}
	SUBCASE("a peer holds the target path")
	{
		f.lockedRel = "Prefabs/Lamp.hasset";
		const ToolResult r = f.call("prefab_save", json{ { "uuid", uuid } });
		CHECK(r.isError);
		CHECK(r.errorCode == "locked_by_other");
	}
	SUBCASE("the reserved Engine namespace")
	{
		const ToolResult r = f.call("prefab_save", json{
			{ "uuid", uuid },
			{ "path", "Engine/Prefabs/Lamp.hasset" },
		});
		CHECK(r.isError);
	}
	SUBCASE("a suffix that is not .hasset")
	{
		const ToolResult r = f.call("prefab_save", json{
			{ "uuid", uuid },
			{ "path", "Prefabs/Lamp.prefab" },
		});
		CHECK(r.isError);
		CHECK(r.errorCode == "invalid_path");
	}
	SUBCASE("an entity that does not exist")
	{
		const ToolResult r = f.call("prefab_save", json{
			{ "uuid", "00000000-0000-0000-0000-000000000001" },
		});
		CHECK(r.isError);
		CHECK(r.errorCode == "not_found");
	}
	SUBCASE("the scene root, whose subtree is the whole scene")
	{
		const ToolResult r = f.call("prefab_save", json{
			{ "uuid", HE::Ed::uuidOf(f.world, f.world.rootEntity()) },
		});
		CHECK(r.isError);
	}

	CHECK_FALSE(fs::exists(f.root / "Prefabs/Lamp.hasset"));
	CHECK(f.published.empty());
	CHECK(f.appeared == 0);
}

TEST_CASE("A name with a slash in it cannot reach into another folder")
{
	Fixture f("slash");
	const Entity source = f.makeSubtree("Arm/Left");
	const ToolResult r = f.call("prefab_save", json{ { "uuid", f.uuid(source) } });
	REQUIRE_MESSAGE(!r.isError, codeOf(r));
	CHECK(r.content["path"] == "Prefabs/Arm_Left.hasset");
	CHECK_FALSE(fs::exists(f.root / "Prefabs/Arm"));
}

// ─── The registration ────────────────────────────────────────────────────────

TEST_CASE("Every prefab tool is registered with a schema a client can call")
{
	Fixture f("schema");
	for (const char* name : { "prefab_info", "prefab_instantiate", "prefab_save" })
	{
		const McpTool* t = f.registry.find(name);
		REQUIRE_MESSAGE(t != nullptr, name);
		CHECK(t->inputSchema.value("type", std::string()) == "object");
		CHECK_FALSE(t->description.empty());
	}
	CHECK_FALSE(f.registry.find("prefab_info")->mutates);
	CHECK(f.registry.find("prefab_instantiate")->mutates);
	CHECK(f.registry.find("prefab_save")->mutates);
}
