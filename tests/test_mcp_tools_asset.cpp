#include "doctest.h"

#include "McpToolRegistry.h"
#include "AssetStubWriter.h"
#include "EditorAssetTypeCache.h"
#include "TestFsUtil.h"

#include <ContentManager/AssetRefScan.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/HAsset.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// ─── What an external client can do to a project's files ─────────────────────
// These tools are the ones with the least margin for a polite failure: an entity
// put in the wrong place is visible on screen, an asset deleted or renamed
// wrongly is a project that no longer opens. So the questions here are the ones
// where a shortcut would be invisible until much later:
//
//   • does a READER really read — asset_resolve and asset_list must not load,
//     because loading moves the dense asset pool and invalidates every pointer
//     the editor holds at that moment,
//   • is the content root actually a boundary, or only a convention '..' walks
//     straight through,
//   • does a move carry the REFERRERS with it, or only the file,
//   • does delete ask what still points at the asset before it is too late —
//     and is "the scan broke down" kept apart from "nothing references this",
//   • does every refusal arrive under a code a client can branch on.

using HE::Ed::McpAssetHooks;
using HE::Ed::McpTool;
using HE::Ed::McpToolRegistry;
using HE::Ed::ToolResult;
using nlohmann::json;

namespace fs = std::filesystem;

namespace {

// A content root on disk plus the registry over it, wired the way
// EditorApplication wires it minus the editor: the hooks that would reach a
// collaboration session are counters, the rest are absent, which is exactly the
// "no session, no project restrictions" state the tools have to work in.
struct Fixture
{
	fs::path        root;
	ContentManager  content;
	McpToolRegistry registry;

	// The session half, as flags a test sets.
	bool        playing = false;
	std::string lockedKey;          // non-empty = a peer holds it
	bool        sessionTakesDelete = false;
	bool        sessionTakesMove   = false;

	int         publishedCreates = 0;
	int         goneCallbacks    = 0;
	int         movedCallbacks   = 0;

	// Absent = every creatable type is allowed. Set to model a project's gates.
	std::vector<HE::AssetType> allowedTypes;
	bool                       restrictTypes = false;

	explicit Fixture(const std::string& name)
	{
		root = fs::temp_directory_path() /
		       ("he_test_mcp_asset_" + name + "_" + std::to_string(::rand()));
		fs::create_directories(root);
		content.setContentRoot(root.string());
		// A stale entry from a previous test's temp root would answer for a path
		// this one is about to reuse.
		EditorAssetTypeCache::invalidateAll();

		McpAssetHooks h;
		h.isPlaying     = [this] { return playing; };
		h.lockedByOther = [this](const std::string& k) {
			return !lockedKey.empty() && k == lockedKey;
		};
		h.requestDelete = [this](const std::string&, bool) { return sessionTakesDelete; };
		h.requestMove   = [this](const std::string&, const std::string&, bool) {
			return sessionTakesMove;
		};
		h.publishCreate = [this](const std::string&, const std::string&) {
			++publishedCreates;
		};
		h.onAssetGone   = [this](const std::string&) { ++goneCallbacks; };
		h.onAssetMoved  = [this](const std::string&, const std::string&, bool) {
			++movedCallbacks;
		};
		h.contentDirName = [] { return std::string("Content"); };
		// enqueueRetarget deliberately ABSENT: the tools then run the on-disk walk
		// inline, which is what makes "did the referrer follow" answerable in the
		// same call rather than on the editor's queue.
		h.creatableTypes = [this]() -> std::vector<HE::AssetType> {
			if (restrictTypes) return allowedTypes;
			std::vector<HE::AssetType> all;
			for (std::uint32_t i = 1; i <= static_cast<std::uint32_t>(HE::AssetType::BlendSpace); ++i)
			{
				const auto t = static_cast<HE::AssetType>(i);
				if (HE::Ed::isCreatableAssetType(t)) all.push_back(t);
			}
			return all;
		};

		HE::Ed::registerAssetTools(registry, content, std::move(h));
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

	// A file straight onto disk, bypassing the tools — the state a test starts
	// FROM, so that what the tools then do to it is the only thing under test.
	void writeStub(const std::string& rel, HE::AssetType type)
	{
		const fs::path abs = root / rel;
		fs::create_directories(abs.parent_path());
		REQUIRE(HE::Ed::writeAssetStub(abs.string(), rel,
		                               fs::path(rel).stem().string(), type));
		EditorAssetTypeCache::invalidate(abs.string());
	}

	// A widget whose tree JSON names `texturePath` — the referrer a delete has to
	// find and a move has to carry over. A widget tree is the referrer shape a
	// test can write honestly: CHUNK_UIWT is a raw-JSON chunk, so it is the same
	// document AssetRefScan reads and AssetRefRetarget rewrites, with no MTRL
	// binary layout to mirror (and therefore nothing to get out of step with the
	// FIELD-SYNCHRONISED writer).
	void writeWidgetNaming(const std::string& rel, const std::string& texturePath)
	{
		const fs::path abs = root / rel;
		fs::create_directories(abs.parent_path());
		HAsset::Writer w;
		std::vector<uint8_t> meta;
		const HE::UUID id = HE::UUID::generate();
		HAsset::Writer::appendPOD(meta, static_cast<uint16_t>(HE::AssetType::Widget));
		HAsset::Writer::appendPOD(meta, id.hi);
		HAsset::Writer::appendPOD(meta, id.lo);
		HAsset::Writer::appendString(meta, fs::path(rel).stem().string());
		HAsset::Writer::appendString(meta, rel);
		w.addChunk(HAsset::CHUNK_META, meta.data(), meta.size());

		const std::string tree =
			std::string("{\"root\":{\"type\":\"Image\",\"texture\":\"") +
			texturePath + "\"}}";
		w.addChunk(HAsset::CHUNK_UIWT, tree.data(), tree.size());
		REQUIRE(w.write(abs.string(), static_cast<uint16_t>(HE::AssetType::Widget)));
		EditorAssetTypeCache::invalidate(abs.string());
	}

	std::string read(const std::string& rel) const
	{
		std::ifstream in(root / rel, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(in), {});
	}

	bool exists(const std::string& rel) const { return fs::exists(root / rel); }
};

} // namespace

// ─── Reading ─────────────────────────────────────────────────────────────────

TEST_CASE("asset_resolve reports a file without loading it")
{
	Fixture f("resolve");
	f.writeStub("Materials/Rock.hasset", HE::AssetType::Material);

	const std::size_t before = f.content.assetCount();
	const ToolResult r = f.call("asset_resolve", json{ { "path", "Materials/Rock.hasset" } });
	REQUIRE_FALSE(r.isError);

	CHECK(r.content["path"].get<std::string>() == "Materials/Rock.hasset");
	CHECK(r.content["type"].get<std::string>() == "Material");
	CHECK(r.content["isFolder"].get<bool>() == false);
	CHECK(r.content["loaded"].get<bool>() == false);

	// The uuid comes from the file's own META chunk, and in the [hi, lo] shape a
	// component field takes.
	REQUIRE(r.content["uuid"].is_array());
	REQUIRE(r.content["uuid"].size() == 2);
	CHECK(r.content["uuid"][0].get<std::uint64_t>() != 0);

	// The claim that matters: asking did not register anything. A reader that
	// loaded would move the dense asset pool and invalidate every pointer the
	// editor was holding.
	CHECK(f.content.assetCount() == before);
	CHECK(f.content.isLoaded(std::string("Materials/Rock.hasset")) == false);
}

TEST_CASE("asset_resolve refuses a path that is not there")
{
	Fixture f("resolve_missing");
	const ToolResult r = f.call("asset_resolve", json{ { "path", "Nope.hasset" } });
	CHECK(r.isError);
	CHECK(r.errorCode == "not_found");
}

TEST_CASE("asset_list walks a folder, filters by type and can recurse")
{
	Fixture f("list");
	f.writeStub("Materials/Rock.hasset",   HE::AssetType::Material);
	f.writeStub("Materials/Wood.hasset",   HE::AssetType::Material);
	f.writeStub("Materials/UI/Menu.hasset", HE::AssetType::Widget);

	SUBCASE("one level, folders included")
	{
		const ToolResult r = f.call("asset_list", json{ { "path", "Materials" } });
		REQUIRE_FALSE(r.isError);
		const json& e = r.content["entries"];
		CHECK(e.size() == 3);   // Rock, Wood, and the UI folder
		bool sawFolder = false;
		for (const json& x : e) if (x["isFolder"].get<bool>()) sawFolder = true;
		CHECK(sawFolder);
	}
	SUBCASE("recursive reaches the nested asset")
	{
		const ToolResult r = f.call("asset_list",
			json{ { "path", "Materials" }, { "recursive", true } });
		REQUIRE_FALSE(r.isError);
		CHECK(r.content["entries"].size() == 4);
	}
	SUBCASE("a type filter drops the folders with the other types")
	{
		const ToolResult r = f.call("asset_list",
			json{ { "path", "Materials" }, { "recursive", true }, { "type", "Material" } });
		REQUIRE_FALSE(r.isError);
		REQUIRE(r.content["entries"].size() == 2);
		for (const json& x : r.content["entries"])
			CHECK(x["type"].get<std::string>() == "Material");
	}
	SUBCASE("an invented type name is a mistake, not an empty list")
	{
		const ToolResult r = f.call("asset_list",
			json{ { "path", "Materials" }, { "type", "material" } });
		CHECK(r.isError);
		CHECK(r.errorCode == "invalid_payload");
	}
	SUBCASE("limit truncates and says so")
	{
		const ToolResult r = f.call("asset_list",
			json{ { "path", "Materials" }, { "recursive", true }, { "limit", 2 } });
		REQUIRE_FALSE(r.isError);
		CHECK(r.content["entries"].size() == 2);
		CHECK(r.content.value("truncated", false) == true);
	}
	SUBCASE("listing loads nothing")
	{
		const std::size_t before = f.content.assetCount();
		f.call("asset_list", json{ { "recursive", true } });
		CHECK(f.content.assetCount() == before);
	}
}

// ─── The boundary ────────────────────────────────────────────────────────────

TEST_CASE("no tool reaches outside the content root")
{
	Fixture f("confine");
	f.writeStub("Rock.hasset", HE::AssetType::Material);

	// A file OUTSIDE the root, next to it, that '..' would reach.
	const fs::path outside = f.root.parent_path() /
		("he_test_mcp_outside_" + std::to_string(::rand()) + ".txt");
	{ std::ofstream o(outside); o << "secret"; }

	const std::string escape = std::string("../") + outside.filename().string();

	SUBCASE("resolve")
	{
		const ToolResult r = f.call("asset_resolve", json{ { "path", escape } });
		CHECK(r.isError);
		CHECK(r.errorCode == "invalid_path");
	}
	SUBCASE("delete")
	{
		const ToolResult r = f.call("asset_delete",
			json{ { "path", escape }, { "force", true } });
		CHECK(r.isError);
		CHECK(r.errorCode == "invalid_path");
		CHECK(fs::exists(outside));   // and it is still there
	}
	SUBCASE("move out")
	{
		const ToolResult r = f.call("asset_move",
			json{ { "path", "Rock.hasset" }, { "newPath", "../Escaped.hasset" } });
		CHECK(r.isError);
		CHECK(r.errorCode == "invalid_path");
		CHECK(f.exists("Rock.hasset"));
	}
	SUBCASE("an absolute path is refused as such")
	{
		const ToolResult r = f.call("asset_resolve",
			json{ { "path", outside.string() } });
		CHECK(r.isError);
		CHECK(r.errorCode == "invalid_path");
	}

	he_test::removeQuiet(outside);
}

// ─── Creating ────────────────────────────────────────────────────────────────

TEST_CASE("asset_create writes an asset the content manager can load back")
{
	Fixture f("create");

	const ToolResult r = f.call("asset_create",
		json{ { "path", "Input/Jump.hasset" }, { "type", "InputAction" } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["created"].get<bool>() == true);
	CHECK(f.exists("Input/Jump.hasset"));
	CHECK(f.publishedCreates == 1);

	// The point of sharing the panel's stub writer: what came out is a real
	// asset, not a file with the right name.
	const HE::UUID id = f.content.loadAsset("Input/Jump.hasset");
	CHECK_FALSE(id == HE::UUID{});
	CHECK(f.content.assetType(id) == HE::AssetType::InputAction);
	CHECK(f.content.getInputAction(id) != nullptr);
}

TEST_CASE("asset_create covers every authored type it offers")
{
	Fixture f("create_all");
	int made = 0;
	for (std::uint32_t i = 1; i <= static_cast<std::uint32_t>(HE::AssetType::BlendSpace); ++i)
	{
		const auto t = static_cast<HE::AssetType>(i);
		if (!HE::Ed::isCreatableAssetType(t)) continue;
		const std::string name = HE::assetTypeName(t);
		REQUIRE_FALSE(name.empty());   // the switch in Types/Enums.h must name it
		const ToolResult r = f.call("asset_create",
			json{ { "path", "Made/" + name + ".hasset" }, { "type", name } });
		INFO("type = ", name);
		REQUIRE_FALSE(r.isError);
		const HE::UUID id = f.content.loadAsset("Made/" + name + ".hasset");
		CHECK_FALSE(id == HE::UUID{});
		CHECK(f.content.assetType(id) == t);
		++made;
	}
	CHECK(made >= 15);
}

TEST_CASE("asset_create refuses rather than guessing")
{
	Fixture f("create_refuse");
	f.writeStub("Taken.hasset", HE::AssetType::Material);

	SUBCASE("an occupied path is not renamed around")
	{
		const ToolResult r = f.call("asset_create",
			json{ { "path", "Taken.hasset" }, { "type", "Material" } });
		CHECK(r.isError);
		CHECK(r.errorCode == "already_exists");
		// And the file that was there is untouched.
		CHECK(f.content.loadAsset("Taken.hasset") != HE::UUID{});
	}
	SUBCASE("an imported type cannot be made from nothing")
	{
		const ToolResult r = f.call("asset_create",
			json{ { "path", "Rock.hasset" }, { "type", "StaticMesh" } });
		CHECK(r.isError);
		CHECK(r.errorCode == "invalid_payload");
		CHECK_FALSE(f.exists("Rock.hasset"));
	}
	SUBCASE("a scene is not an asset stub")
	{
		const ToolResult r = f.call("asset_create",
			json{ { "path", "Levels/Main.hescene" }, { "type", "Scene" } });
		CHECK(r.isError);
		CHECK(r.errorCode == "invalid_payload");
	}
	SUBCASE("what the project does not offer is not created")
	{
		f.restrictTypes = true;
		f.allowedTypes  = { HE::AssetType::Widget };
		const ToolResult r = f.call("asset_create",
			json{ { "path", "Mat.hasset" }, { "type", "Material" } });
		CHECK(r.isError);
		CHECK(r.errorCode == "invalid_payload");
		CHECK_FALSE(f.exists("Mat.hasset"));
	}
	SUBCASE("play mode is a refusal, not a queue")
	{
		f.playing = true;
		const ToolResult r = f.call("asset_create",
			json{ { "path", "Later.hasset" }, { "type", "Widget" } });
		CHECK(r.isError);
		CHECK(r.errorCode == "play_mode");
		CHECK_FALSE(f.exists("Later.hasset"));
	}
}

TEST_CASE("a reused path answers for the file that is there NOW")
{
	// The flow this interface steers a client into: asset_create refuses an
	// occupied path and says "delete that asset first". A delete mirrors the
	// Content Browser and does NOT unload, so the old asset stays resident under
	// the vacated path — and an answer taken from the path index would report the
	// dead one's uuid for the live file, and call it loaded.
	Fixture f("reused_path");
	REQUIRE_FALSE(f.call("asset_create",
		json{ { "path", "Thing.hasset" }, { "type", "Widget" } }).isError);

	const HE::UUID first = f.content.loadAsset("Thing.hasset");
	REQUIRE_FALSE(first == HE::UUID{});
	REQUIRE(f.content.isLoaded(first));

	REQUIRE_FALSE(f.call("asset_delete",
		json{ { "path", "Thing.hasset" }, { "force", true } }).isError);
	REQUIRE_FALSE(f.call("asset_create",
		json{ { "path", "Thing.hasset" }, { "type", "Theme" } }).isError);

	const ToolResult r = f.call("asset_resolve", json{ { "path", "Thing.hasset" } });
	REQUIRE_FALSE(r.isError);
	// The new file's own uuid, not the resident stranger's.
	const std::uint64_t hi = r.content["uuid"][0].get<std::uint64_t>();
	const std::uint64_t lo = r.content["uuid"][1].get<std::uint64_t>();
	CHECK_FALSE((hi == first.hi && lo == first.lo));
	CHECK(r.content["type"].get<std::string>() == "Theme");
	CHECK(r.content["loaded"].get<bool>() == false);
}

TEST_CASE("asset_resolve types a scene by its extension")
{
	// A .hescene is JSON, not an HAsset, so the header sniff finds nothing —
	// and every scene in the project, including the example this tool's own
	// description gives, would come back untyped.
	Fixture f("scene_type");
	fs::create_directories(f.root / "Levels");
	{ std::ofstream o(f.root / "Levels/Main.hescene"); o << "{\"entities\":[]}"; }

	const ToolResult r = f.call("asset_resolve", json{ { "path", "Levels/Main.hescene" } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["type"].get<std::string>() == "Scene");
}

TEST_CASE("asset_create checks the base class against the taxonomy")
{
	Fixture f("base_class");
	SUBCASE("a real one goes through")
	{
		const ToolResult r = f.call("asset_create", json{
			{ "path", "Gameplay/Hero.hasset" },
			{ "type", "HorizonCodeClass" },
			{ "baseClass", "PlayerCharacter" },
		});
		REQUIRE_FALSE(r.isError);
		CHECK(f.exists("Gameplay/Hero.hasset"));
	}
	SUBCASE("an invented one is refused rather than written")
	{
		// It would land in CHUNK_HCBC, decide the event catalog, and resolve to
		// nothing — a class whose events silently do not exist.
		const ToolResult r = f.call("asset_create", json{
			{ "path", "Gameplay/Ghost.hasset" },
			{ "type", "HorizonCodeClass" },
			{ "baseClass", "Pawn" },
		});
		CHECK(r.isError);
		CHECK(r.errorCode == "invalid_payload");
		CHECK_FALSE(f.exists("Gameplay/Ghost.hasset"));
	}
	SUBCASE("omitting it is the plain Object class")
	{
		const ToolResult r = f.call("asset_create", json{
			{ "path", "Gameplay/Plain.hasset" }, { "type", "HorizonCodeClass" },
		});
		REQUIRE_FALSE(r.isError);
	}
}

TEST_CASE("a scan that could not finish is not read as 'nothing references this'")
{
	Fixture f("scan_incomplete");
	// An .hasset that CLAIMS to be one — the magic is right — and then stops.
	// That is the case assetUuidOfFile calls unreadable, and it has to be, because
	// the scan then has no uuid to look for and scenes reference meshes and
	// materials by id alone: an empty result would be an answer the scan never
	// actually gave. (A file WITHOUT the magic is a legitimate null instead — a
	// JSON scene is shorter than the header, and calling that unreadable would
	// fire the warning on every delete until it meant nothing.)
	{
		std::ofstream o(f.root / "Broken.hasset", std::ios::binary);
		o.write("HAST", 4);
	}

	const ToolResult r = f.call("asset_delete", json{ { "path", "Broken.hasset" } });
	CHECK(r.isError);
	CHECK(r.errorCode == "scan_incomplete");
	CHECK(f.exists("Broken.hasset"));

	// force is the way through, and it says nothing about references.
	const ToolResult forced = f.call("asset_delete",
		json{ { "path", "Broken.hasset" }, { "force", true } });
	CHECK_FALSE(forced.isError);
	CHECK_FALSE(f.exists("Broken.hasset"));
}

TEST_CASE("asset_create appends the suffix and makes the folder")
{
	Fixture f("create_suffix");
	const ToolResult r = f.call("asset_create",
		json{ { "path", "Deep/Deeper/Thing" }, { "type", "Theme" } });
	REQUIRE_FALSE(r.isError);
	CHECK(f.exists("Deep/Deeper/Thing.hasset"));
	CHECK(r.content["path"].get<std::string>() == "Deep/Deeper/Thing.hasset");
}

// ─── Deleting ────────────────────────────────────────────────────────────────

TEST_CASE("asset_delete removes an unreferenced asset")
{
	Fixture f("delete");
	f.writeStub("Junk.hasset", HE::AssetType::Widget);

	const ToolResult r = f.call("asset_delete", json{ { "path", "Junk.hasset" } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["applied"].get<bool>() == true);
	CHECK_FALSE(f.exists("Junk.hasset"));
	CHECK(f.goneCallbacks == 1);
}

TEST_CASE("asset_delete asks what still points at the asset")
{
	Fixture f("delete_referenced");
	f.writeStub("Textures/Rock.hasset", HE::AssetType::Texture);
	f.writeWidgetNaming("UI/Card.hasset", "Textures/Rock.hasset");

	SUBCASE("a referrer is a refusal, and it is named")
	{
		const ToolResult r = f.call("asset_delete", json{ { "path", "Textures/Rock.hasset" } });
		REQUIRE(r.isError);
		CHECK(r.errorCode == "has_referrers");
		REQUIRE(r.content["referrers"].is_array());
		REQUIRE(r.content["referrers"].size() >= 1);
		// Named, not counted: a client that cannot see the screen has to be able
		// to go and fix the referrer.
		bool namedTheMaterial = false;
		for (const json& x : r.content["referrers"])
			if (x["path"].get<std::string>().find("UI/Card") != std::string::npos)
				namedTheMaterial = true;
		CHECK(namedTheMaterial);
		// And nothing happened.
		CHECK(f.exists("Textures/Rock.hasset"));
		CHECK(f.goneCallbacks == 0);
	}
	SUBCASE("force goes through with it")
	{
		const ToolResult r = f.call("asset_delete",
			json{ { "path", "Textures/Rock.hasset" }, { "force", true } });
		REQUIRE_FALSE(r.isError);
		CHECK_FALSE(f.exists("Textures/Rock.hasset"));
	}
}

TEST_CASE("asset_delete refuses a folder rather than taking the subtree")
{
	Fixture f("delete_folder");
	f.writeStub("Stuff/A.hasset", HE::AssetType::Widget);

	const ToolResult r = f.call("asset_delete", json{ { "path", "Stuff" } });
	CHECK(r.isError);
	CHECK(r.errorCode == "invalid_path");
	CHECK(f.exists("Stuff/A.hasset"));
}

TEST_CASE("in a session a delete is a request, and says so")
{
	Fixture f("delete_session");
	f.writeStub("Junk.hasset", HE::AssetType::Widget);
	f.sessionTakesDelete = true;

	const ToolResult r = f.call("asset_delete", json{ { "path", "Junk.hasset" } });
	REQUIRE_FALSE(r.isError);
	// The whole point: a success that does NOT claim the asset is gone.
	CHECK(r.content["applied"].get<bool>() == false);
	CHECK(r.content["requested"].get<bool>() == true);
	CHECK(f.exists("Junk.hasset"));
	CHECK(f.goneCallbacks == 0);
}

TEST_CASE("a peer's lock is a refusal a client can branch on")
{
	Fixture f("locked");
	f.writeStub("Held.hasset", HE::AssetType::Widget);
	f.lockedKey = "Held.hasset";

	const ToolResult r = f.call("asset_delete",
		json{ { "path", "Held.hasset" }, { "force", true } });
	CHECK(r.isError);
	CHECK(r.errorCode == "locked_by_other");
	CHECK(f.exists("Held.hasset"));
}

// ─── Moving ──────────────────────────────────────────────────────────────────

TEST_CASE("asset_move takes the referrers with it")
{
	Fixture f("move");
	f.writeStub("Textures/Rock.hasset", HE::AssetType::Texture);
	f.writeWidgetNaming("UI/Card.hasset", "Textures/Rock.hasset");

	const ToolResult r = f.call("asset_move", json{
		{ "path",    "Textures/Rock.hasset" },
		{ "newPath", "Textures/Stone/Granite.hasset" },
	});
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["applied"].get<bool>() == true);
	CHECK(r.content["previousPath"].get<std::string>() == "Textures/Rock.hasset");

	CHECK_FALSE(f.exists("Textures/Rock.hasset"));
	CHECK(f.exists("Textures/Stone/Granite.hasset"));
	CHECK(f.movedCallbacks == 1);

	// The half a move that only renamed the file would silently skip: the
	// widget still names the OLD path, and nothing would say so until the
	// texture failed to resolve at some later load.
	const std::string mat = f.read("UI/Card.hasset");
	CHECK(mat.find("Textures/Stone/Granite.hasset") != std::string::npos);
	CHECK(mat.find("Textures/Rock.hasset") == std::string::npos);
}

TEST_CASE("asset_move keeps the extension when the new path has none")
{
	Fixture f("move_ext");
	f.writeStub("Rock.hasset", HE::AssetType::Material);

	const ToolResult r = f.call("asset_move",
		json{ { "path", "Rock.hasset" }, { "newPath", "Stone" } });
	REQUIRE_FALSE(r.isError);
	CHECK(f.exists("Stone.hasset"));
}

TEST_CASE("asset_move does not overwrite and does not no-op silently")
{
	Fixture f("move_refuse");
	f.writeStub("A.hasset", HE::AssetType::Material);
	f.writeStub("B.hasset", HE::AssetType::Material);

	SUBCASE("onto an existing asset")
	{
		const ToolResult r = f.call("asset_move",
			json{ { "path", "A.hasset" }, { "newPath", "B.hasset" } });
		CHECK(r.isError);
		CHECK(r.errorCode == "already_exists");
		CHECK(f.exists("A.hasset"));
	}
	SUBCASE("onto itself")
	{
		const ToolResult r = f.call("asset_move",
			json{ { "path", "A.hasset" }, { "newPath", "A.hasset" } });
		CHECK(r.isError);
		CHECK(r.errorCode == "invalid_payload");
	}
	SUBCASE("while playing")
	{
		f.playing = true;
		const ToolResult r = f.call("asset_move",
			json{ { "path", "A.hasset" }, { "newPath", "C.hasset" } });
		CHECK(r.isError);
		CHECK(r.errorCode == "play_mode");
		CHECK(f.exists("A.hasset"));
	}
}

TEST_CASE("in a session a move is a request, and nothing moves yet")
{
	Fixture f("move_session");
	f.writeStub("A.hasset", HE::AssetType::Material);
	f.sessionTakesMove = true;

	const ToolResult r = f.call("asset_move",
		json{ { "path", "A.hasset" }, { "newPath", "B.hasset" } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["applied"].get<bool>() == false);
	CHECK(r.content["requested"].get<bool>() == true);
	CHECK(f.exists("A.hasset"));
	CHECK_FALSE(f.exists("B.hasset"));
	CHECK(f.movedCallbacks == 0);
}

// ─── The registration itself ─────────────────────────────────────────────────

TEST_CASE("the asset tools register under legal names and carry schemas")
{
	Fixture f("registration");
	for (const char* name : { "asset_resolve", "asset_list", "asset_create",
	                          "asset_delete", "asset_move" })
	{
		const McpTool* t = f.registry.find(name);
		INFO("tool = ", name);
		REQUIRE(t != nullptr);
		CHECK(McpToolRegistry::enforceNameRule(t->name));
		CHECK(t->inputSchema.is_object());
		CHECK_FALSE(t->description.empty());
	}
	// The three that change files are the three the bridge refuses in play mode
	// and writes to the console log.
	CHECK(f.registry.find("asset_resolve")->mutates == false);
	CHECK(f.registry.find("asset_list")->mutates    == false);
	CHECK(f.registry.find("asset_create")->mutates  == true);
	CHECK(f.registry.find("asset_delete")->mutates  == true);
	CHECK(f.registry.find("asset_move")->mutates    == true);
}

TEST_CASE("assetTypeFromName is the exact inverse of assetTypeName")
{
	// The pair these tools' `type` argument rides on. A type the forward
	// direction cannot name is one no client can ever ask for — which is how
	// BoneMask and BlendSpace went missing from the switch in Types/Enums.h.
	for (std::uint32_t i = 1; i <= static_cast<std::uint32_t>(HE::AssetType::BlendSpace); ++i)
	{
		const auto t = static_cast<HE::AssetType>(i);
		const std::string name = HE::assetTypeName(t);
		INFO("value = ", i);
		REQUIRE_FALSE(name.empty());
		CHECK(HE::Ed::assetTypeFromName(name) == t);
	}
	CHECK(HE::Ed::assetTypeFromName("")         == HE::AssetType::Unknown);
	CHECK(HE::Ed::assetTypeFromName("material") == HE::AssetType::Unknown);
}
