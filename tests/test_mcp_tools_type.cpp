#include "doctest.h"

#include "AssetStubWriter.h"
#include "EditorAssetTypeCache.h"
#include "McpToolRegistry.h"
#include "TestFsUtil.h"

#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <Types/TypeRegistry.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// ─── Authoring a project's own types from outside the editor ─────────────────
// The claim these tools make that nothing else can make for them: THE
// DEFINITION THE REST OF THE ENGINE SEES AFTERWARDS IS THE ONE THAT WAS ASKED
// FOR. Writing the file is only half of a save — TypeAssetPanel::saveState also
// re-registers the definition in HE::TypeRegistry, and every type dropdown, the
// script bootstrap, the savegame seeder and the C++ codegen read it from there.
// A tool that wrote the file and skipped that leaves the editor offering
// yesterday's fields with nothing on screen to suggest it.
//
// So the load-bearing tests here ask the REGISTRY, not just the file. The rest
// are the places where a shortcut would look green and be wrong later:
//
//   • the four coupled container fields (isArray, container, keyType,
//     keyTypeName) — the loader repairs an illegal combination in silence, so a
//     tool that writes half of them produces a field that is not what was asked
//     for and no error either,
//   • an update that mentions one thing must not reset the others,
//   • a cycle is refused, exactly as the panel's Save refuses it,
//   • a field pointing at a definition that does not exist is refused rather
//     than written,
//   • a default is read in the shape its own type has, and a wrong shape is a
//     refusal instead of a coercion,
//   • a refusal leaves the file byte for byte as it was.

using HE::Ed::McpTool;
using HE::Ed::McpToolRegistry;
using HE::Ed::McpTypeHooks;
using HE::Ed::ToolResult;
using nlohmann::json;

namespace fs = std::filesystem;

namespace {

std::string codeOf(const ToolResult& r)
{
	return r.isError ? r.errorCode : std::string("<ok>");
}

struct Fixture
{
	fs::path        root;
	ContentManager  content;
	McpToolRegistry registry;

	bool        playing = false;
	std::string lockedRel;
	std::string dirtyRel;
	std::string openRel;
	int         reloadCalls = 0;
	int         typesChanged = 0;

	explicit Fixture(const std::string& name)
	{
		root = fs::temp_directory_path() /
		       ("he_test_mcp_type_" + name + "_" + std::to_string(::rand()));
		fs::create_directories(root);
		content.setContentRoot(root.string());
		EditorAssetTypeCache::invalidateAll();
		// The registry is process-global, so a leftover definition from another
		// test file would answer this one's questions.
		HE::TypeRegistry::instance().clear();

		McpTypeHooks h;
		h.isPlaying     = [this] { return playing; };
		h.lockedByOther = [this](const std::string& rel) {
			return !lockedRel.empty() && rel == lockedRel;
		};
		h.isDirty = [this](const std::string& rel) {
			return !dirtyRel.empty() && rel == dirtyRel;
		};
		h.reloadFromDisk = [this](const std::string& rel) {
			if (openRel.empty() || rel != openRel) return false;
			++reloadCalls;
			return true;
		};
		h.onTypesChanged = [this] { ++typesChanged; };
		HE::Ed::registerTypeTools(registry, content, std::move(h));
	}

	~Fixture()
	{
		HE::TypeRegistry::instance().clear();
		EditorAssetTypeCache::invalidateAll();
		he_test::removeAllQuiet(root);
	}

	ToolResult call(const std::string& name, const json& args = json::object())
	{
		const McpTool* t = registry.find(name);
		REQUIRE_MESSAGE(t != nullptr, "no such tool registered: " << name);
		return t->handler(args);
	}

	// A definition asset as the Content Browser's create menu makes one: the
	// editor's own stub writer, so a newborn file here is the newborn file there.
	void writeStub(const std::string& rel, HE::AssetType type)
	{
		const fs::path abs = root / rel;
		fs::create_directories(abs.parent_path());
		REQUIRE(HE::Ed::writeAssetStub(abs.string(), rel, fs::path(rel).stem().string(), type));
		EditorAssetTypeCache::invalidate(abs.string());
	}

	std::string bytes(const std::string& rel) const
	{
		std::ifstream f(root / rel, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	}

	// What the registry — not the file — says about a struct now.
	HE::StructDef registered(const std::string& rel) const
	{
		HE::StructDef def;
		HE::TypeRegistry::instance().getStruct(rel, def);
		return def;
	}
	HE::EnumDef registeredEnum(const std::string& rel) const
	{
		HE::EnumDef def;
		HE::TypeRegistry::instance().getEnum(rel, def);
		return def;
	}
};

const json* findField(const json& fields, const std::string& name)
{
	for (const json& f : fields)
		if (f.value("name", std::string()) == name) return &f;
	return nullptr;
}

} // namespace

// ─── Reading ─────────────────────────────────────────────────────────────────

TEST_CASE("type_info lists the project's definitions and reads one")
{
	Fixture f("info");
	f.writeStub("Types/Loadout.hasset", HE::AssetType::StructType);
	f.writeStub("Types/Rarity.hasset", HE::AssetType::EnumType);
	f.writeStub("Saves/Slot.hasset", HE::AssetType::SaveGameTemplate);
	f.writeStub("Types/NotAType.hasset", HE::AssetType::Material);

	const ToolResult all = f.call("type_info");
	REQUIRE_MESSAGE(!all.isError, codeOf(all));
	REQUIRE(all.content["types"].size() == 3);
	// Sorted by path, so two calls on an unchanged project answer identically.
	CHECK(all.content["types"][0]["path"] == "Saves/Slot.hasset");
	CHECK(all.content["types"][0]["kind"] == "template");
	CHECK(all.content["types"][1]["kind"] == "struct");
	CHECK(all.content["types"][2]["kind"] == "enum");
	// A newborn definition really is empty — that is the whole reason these
	// tools exist.
	CHECK(all.content["types"][1]["fieldCount"] == 0);
	CHECK(all.content["types"][2]["entryCount"] == 0);

	const ToolResult one = f.call("type_info", json{ { "path", "Types/Loadout.hasset" } });
	REQUIRE_MESSAGE(!one.isError, codeOf(one));
	CHECK(one.content["kind"] == "struct");
	CHECK(one.content["name"] == "Loadout");
	CHECK(one.content["fields"].empty());
}

TEST_CASE("type_info refuses what is not a definition, and says which tool is")
{
	Fixture f("info_refuse");
	f.writeStub("Materials/Rock.hasset", HE::AssetType::Material);
	const ToolResult wrong = f.call("type_info", json{ { "path", "Materials/Rock.hasset" } });
	CHECK(wrong.isError);
	CHECK(wrong.errorCode == "invalid_path");

	const ToolResult missing = f.call("type_info", json{ { "path", "Types/Ghost.hasset" } });
	CHECK(missing.isError);
	CHECK(missing.errorCode == "not_found");

	// A struct addressed with the enum tool, and the other way round: two
	// different mistakes, each pointed at the tool that does the job.
	f.writeStub("Types/Loadout.hasset", HE::AssetType::StructType);
	f.writeStub("Types/Rarity.hasset", HE::AssetType::EnumType);
	const ToolResult a = f.call("type_enum_set", json{
		{ "path", "Types/Loadout.hasset" }, { "name", "Common" } });
	CHECK(a.isError);
	CHECK(a.errorCode == "invalid_path");
	const ToolResult b = f.call("type_field_set", json{
		{ "path", "Types/Rarity.hasset" }, { "name", "slots" }, { "type", "Int" } });
	CHECK(b.isError);
	CHECK(b.errorCode == "invalid_path");
}

// ─── Struct fields ───────────────────────────────────────────────────────────

TEST_CASE("A field written by the tool is the field the whole engine then sees")
{
	Fixture f("field");
	f.writeStub("Types/Loadout.hasset", HE::AssetType::StructType);

	const ToolResult r = f.call("type_field_set", json{
		{ "path",    "Types/Loadout.hasset" },
		{ "name",    "slots" },
		{ "type",    "Int" },
		{ "default", 4 },
	});
	REQUIRE_MESSAGE(!r.isError, codeOf(r));
	CHECK(r.content["created"] == true);
	CHECK(r.content["fieldCount"] == 1);
	CHECK(r.content["field"]["type"] == "Int");
	CHECK(r.content["field"]["default"] == 4);

	// The FILE round-trips…
	const ToolResult read = f.call("type_info", json{ { "path", "Types/Loadout.hasset" } });
	REQUIRE_MESSAGE(!read.isError, codeOf(read));
	const json* field = findField(read.content["fields"], "slots");
	REQUIRE(field != nullptr);
	CHECK((*field)["default"] == 4);

	// …and so does the REGISTRY, which is what every dropdown, the script
	// bootstrap and the savegame seeder actually read. Writing the file and
	// forgetting this is the mistake with nothing on screen to show it.
	const HE::StructDef def = f.registered("Types/Loadout.hasset");
	REQUIRE(def.fields.size() == 1);
	CHECK(def.fields[0].name == "slots");
	CHECK(def.fields[0].type == HorizonCode::PinType::Int);
	CHECK(def.fields[0].defaultValue.i == 4);
	// And the seeded default of a whole instance, which is the answer the
	// savegame path and every HorizonCode variable of this type starts from.
	// A struct Value keeps its field values in `items`, in definition order.
	const HorizonCode::Value seeded =
		HE::TypeRegistry::instance().makeDefaultValue("Types/Loadout.hasset");
	REQUIRE(seeded.items.size() == 1);
	CHECK(seeded.items[0].i == 4);

	// The C++ header hook fired — in a C++ project that is the difference
	// between gameplay code compiling against this struct and against the last.
	CHECK(f.typesChanged == 1);
}

TEST_CASE("An update keeps everything the call did not mention")
{
	Fixture f("update");
	f.writeStub("Types/Loadout.hasset", HE::AssetType::StructType);
	REQUIRE_FALSE(f.call("type_field_set", json{
		{ "path", "Types/Loadout.hasset" }, { "name", "label" },
		{ "type", "String" }, { "default", "empty" } }).isError);

	// Only the default. The type has to survive.
	const ToolResult r = f.call("type_field_set", json{
		{ "path", "Types/Loadout.hasset" }, { "name", "label" },
		{ "default", "starter" } });
	REQUIRE_MESSAGE(!r.isError, codeOf(r));
	CHECK(r.content["created"] == false);
	CHECK(r.content["field"]["type"] == "String");
	CHECK(r.content["field"]["default"] == "starter");

	const HE::StructDef def = f.registered("Types/Loadout.hasset");
	REQUIRE(def.fields.size() == 1);
	CHECK(def.fields[0].type == HorizonCode::PinType::String);
	CHECK(def.fields[0].defaultValue.s == "starter");
}

TEST_CASE("A field keeps its position on an update, and index places a new one")
{
	Fixture f("order");
	f.writeStub("Types/Loadout.hasset", HE::AssetType::StructType);
	for (const char* n : { "a", "b", "c" })
		REQUIRE_FALSE(f.call("type_field_set", json{
			{ "path", "Types/Loadout.hasset" }, { "name", n }, { "type", "Float" } }).isError);

	const ToolResult upd = f.call("type_field_set", json{
		{ "path", "Types/Loadout.hasset" }, { "name", "b" }, { "type", "Int" } });
	REQUIRE_MESSAGE(!upd.isError, codeOf(upd));
	CHECK(upd.content["index"] == 1);

	const ToolResult ins = f.call("type_field_set", json{
		{ "path", "Types/Loadout.hasset" }, { "name", "first" },
		{ "type", "Bool" }, { "index", 0 } });
	REQUIRE_MESSAGE(!ins.isError, codeOf(ins));
	CHECK(ins.content["index"] == 0);

	const HE::StructDef def = f.registered("Types/Loadout.hasset");
	REQUIRE(def.fields.size() == 4);
	CHECK(def.fields[0].name == "first");
	CHECK(def.fields[2].name == "b");
	CHECK(def.fields[2].type == HorizonCode::PinType::Int);
}

TEST_CASE("The four coupled container fields are written as one")
{
	Fixture f("container");
	f.writeStub("Types/Loadout.hasset", HE::AssetType::StructType);
	f.writeStub("Types/Rarity.hasset", HE::AssetType::EnumType);
	REQUIRE_FALSE(f.call("type_enum_set", json{
		{ "path", "Types/Rarity.hasset" }, { "name", "Common" } }).isError);

	SUBCASE("an array")
	{
		const ToolResult r = f.call("type_field_set", json{
			{ "path", "Types/Loadout.hasset" }, { "name", "items" },
			{ "type", "String" }, { "container", "array" } });
		REQUIRE_MESSAGE(!r.isError, codeOf(r));
		CHECK(r.content["field"]["container"] == "array");
		const HE::StructDef def = f.registered("Types/Loadout.hasset");
		REQUIRE(def.fields.size() == 1);
		// The legacy row: isArray true, container None, which containerKindOf
		// resolves to Array. Written that way so the bytes match every file
		// authored before Set/Map existed.
		CHECK(def.fields[0].isArray);
		CHECK(def.fields[0].container == HorizonCode::ContainerKind::None);
		CHECK(def.fields[0].kind() == HorizonCode::ContainerKind::Array);
	}

	SUBCASE("a set")
	{
		REQUIRE_FALSE(f.call("type_field_set", json{
			{ "path", "Types/Loadout.hasset" }, { "name", "tags" },
			{ "type", "String" }, { "container", "set" } }).isError);
		const HE::StructDef def = f.registered("Types/Loadout.hasset");
		REQUIRE(def.fields.size() == 1);
		// isArray must be true as well — "Set but not a container" is a state the
		// loader repairs in silence, so writing only the kind would produce a
		// field that reads back as something else.
		CHECK(def.fields[0].isArray);
		CHECK(def.fields[0].kind() == HorizonCode::ContainerKind::Set);
	}

	SUBCASE("a map keyed by an enum")
	{
		const ToolResult r = f.call("type_field_set", json{
			{ "path", "Types/Loadout.hasset" }, { "name", "byRarity" },
			{ "type", "Int" }, { "container", "map" },
			{ "keyType", "Enum" }, { "keyTypeName", "Types/Rarity.hasset" } });
		REQUIRE_MESSAGE(!r.isError, codeOf(r));
		CHECK(r.content["field"]["keyType"] == "Enum");
		const HE::StructDef def = f.registered("Types/Loadout.hasset");
		REQUIRE(def.fields.size() == 1);
		CHECK(def.fields[0].kind() == HorizonCode::ContainerKind::Map);
		CHECK(def.fields[0].keyType == HorizonCode::PinType::Enum);
		CHECK(def.fields[0].keyTypeName == "Types/Rarity.hasset");
	}

	SUBCASE("a map keyed by a float is refused")
	{
		const ToolResult r = f.call("type_field_set", json{
			{ "path", "Types/Loadout.hasset" }, { "name", "byWeight" },
			{ "type", "Int" }, { "container", "map" }, { "keyType", "Float" } });
		CHECK(r.isError);
		CHECK(r.errorCode == "invalid_payload");
		CHECK(f.registered("Types/Loadout.hasset").fields.empty());
	}

	SUBCASE("back to a single value")
	{
		REQUIRE_FALSE(f.call("type_field_set", json{
			{ "path", "Types/Loadout.hasset" }, { "name", "tags" },
			{ "type", "String" }, { "container", "set" } }).isError);
		REQUIRE_FALSE(f.call("type_field_set", json{
			{ "path", "Types/Loadout.hasset" }, { "name", "tags" },
			{ "container", "none" } }).isError);
		const HE::StructDef def = f.registered("Types/Loadout.hasset");
		REQUIRE(def.fields.size() == 1);
		CHECK_FALSE(def.fields[0].isArray);
		CHECK(def.fields[0].kind() == HorizonCode::ContainerKind::None);
	}
}

TEST_CASE("A field has to point at a definition that exists")
{
	Fixture f("ref");
	f.writeStub("Types/Loadout.hasset", HE::AssetType::StructType);
	f.writeStub("Types/Rarity.hasset", HE::AssetType::EnumType);

	SUBCASE("no typeName at all")
	{
		const ToolResult r = f.call("type_field_set", json{
			{ "path", "Types/Loadout.hasset" }, { "name", "rarity" }, { "type", "Enum" } });
		CHECK(r.isError);
		CHECK(r.errorCode == "invalid_payload");
	}
	SUBCASE("a path with nothing behind it")
	{
		const ToolResult r = f.call("type_field_set", json{
			{ "path", "Types/Loadout.hasset" }, { "name", "rarity" },
			{ "type", "Enum" }, { "typeName", "Types/Ghost.hasset" } });
		CHECK(r.isError);
		CHECK(r.errorCode == "invalid_payload");
	}
	SUBCASE("an enum field pointing at a struct")
	{
		const ToolResult r = f.call("type_field_set", json{
			{ "path", "Types/Loadout.hasset" }, { "name", "rarity" },
			{ "type", "Enum" }, { "typeName", "Types/Loadout.hasset" } });
		CHECK(r.isError);
		CHECK(r.errorCode == "invalid_payload");
	}
	SUBCASE("the real thing")
	{
		const ToolResult r = f.call("type_field_set", json{
			{ "path", "Types/Loadout.hasset" }, { "name", "rarity" },
			{ "type", "Enum" }, { "typeName", "Types/Rarity.hasset" },
			{ "default", "Common" } });
		REQUIRE_MESSAGE(!r.isError, codeOf(r));
		const HE::StructDef def = f.registered("Types/Loadout.hasset");
		REQUIRE(def.fields.size() == 1);
		CHECK(def.fields[0].typeName == "Types/Rarity.hasset");
		CHECK(def.fields[0].defaultValue.s == "Common");
	}

	// Every refusal above left the definition empty.
	if (f.registered("Types/Loadout.hasset").fields.size() != 1)
		CHECK(f.registered("Types/Loadout.hasset").fields.empty());
}

TEST_CASE("A struct that would contain itself is refused, like the panel's Save")
{
	Fixture f("cycle");
	f.writeStub("Types/Loadout.hasset", HE::AssetType::StructType);
	const std::string before = f.bytes("Types/Loadout.hasset");

	const ToolResult r = f.call("type_field_set", json{
		{ "path", "Types/Loadout.hasset" }, { "name", "self" },
		{ "type", "Struct" }, { "typeName", "Types/Loadout.hasset" } });
	CHECK(r.isError);
	CHECK(r.errorCode == "invalid_payload");
	// Nothing written, nothing registered, no header regenerated.
	CHECK(f.bytes("Types/Loadout.hasset") == before);
	CHECK(f.registered("Types/Loadout.hasset").fields.empty());
	CHECK(f.typesChanged == 0);
}

TEST_CASE("A default is read in the shape its own type has")
{
	Fixture f("default");
	f.writeStub("Types/Loadout.hasset", HE::AssetType::StructType);

	SUBCASE("a Vec3 wants three numbers")
	{
		const ToolResult ok = f.call("type_field_set", json{
			{ "path", "Types/Loadout.hasset" }, { "name", "offset" },
			{ "type", "Vec3" }, { "default", json::array({ 1.0, 2.0, 3.0 }) } });
		REQUIRE_MESSAGE(!ok.isError, codeOf(ok));
		const HE::StructDef def = f.registered("Types/Loadout.hasset");
		REQUIRE(def.fields.size() == 1);
		CHECK(def.fields[0].defaultValue.v3.z == doctest::Approx(3.0f));

		const ToolResult bad = f.call("type_field_set", json{
			{ "path", "Types/Loadout.hasset" }, { "name", "offset" },
			{ "default", json::array({ 1.0, 2.0 }) } });
		CHECK(bad.isError);
		CHECK(bad.errorCode == "invalid_payload");
		// And the good value from before is still there — a refusal is a no-op.
		CHECK(f.registered("Types/Loadout.hasset").fields[0].defaultValue.v3.z
		      == doctest::Approx(3.0f));
	}

	SUBCASE("an Int wants a whole number")
	{
		const ToolResult bad = f.call("type_field_set", json{
			{ "path", "Types/Loadout.hasset" }, { "name", "slots" },
			{ "type", "Int" }, { "default", 2.5 } });
		CHECK(bad.isError);
		CHECK(bad.errorCode == "invalid_payload");
	}

	SUBCASE("a container field takes no default here")
	{
		const ToolResult bad = f.call("type_field_set", json{
			{ "path", "Types/Loadout.hasset" }, { "name", "items" },
			{ "type", "String" }, { "container", "array" }, { "default", "x" } });
		CHECK(bad.isError);
		CHECK(bad.errorCode == "invalid_payload");
	}

	SUBCASE("an unknown type name is refused with the list")
	{
		const ToolResult bad = f.call("type_field_set", json{
			{ "path", "Types/Loadout.hasset" }, { "name", "x" }, { "type", "Quaternion" } });
		CHECK(bad.isError);
		CHECK(bad.errorMessage.find("Float") != std::string::npos);
	}

	SUBCASE("a new field without a type is refused")
	{
		const ToolResult bad = f.call("type_field_set", json{
			{ "path", "Types/Loadout.hasset" }, { "name", "x" } });
		CHECK(bad.isError);
		CHECK(bad.errorCode == "invalid_payload");
	}
}

TEST_CASE("type_field_remove takes a field out, and names what is there when it misses")
{
	Fixture f("remove");
	f.writeStub("Types/Loadout.hasset", HE::AssetType::StructType);
	REQUIRE_FALSE(f.call("type_field_set", json{
		{ "path", "Types/Loadout.hasset" }, { "name", "slots" }, { "type", "Int" } }).isError);
	REQUIRE_FALSE(f.call("type_field_set", json{
		{ "path", "Types/Loadout.hasset" }, { "name", "label" }, { "type", "String" } }).isError);

	const ToolResult miss = f.call("type_field_remove", json{
		{ "path", "Types/Loadout.hasset" }, { "name", "nope" } });
	CHECK(miss.isError);
	CHECK(miss.errorCode == "not_found");
	CHECK(miss.errorMessage.find("slots") != std::string::npos);

	const ToolResult r = f.call("type_field_remove", json{
		{ "path", "Types/Loadout.hasset" }, { "name", "slots" } });
	REQUIRE_MESSAGE(!r.isError, codeOf(r));
	CHECK(r.content["fieldCount"] == 1);
	const HE::StructDef def = f.registered("Types/Loadout.hasset");
	REQUIRE(def.fields.size() == 1);
	CHECK(def.fields[0].name == "label");
}

// ─── Enum entries ────────────────────────────────────────────────────────────

TEST_CASE("Enum entries are added, renumbered and removed by name")
{
	Fixture f("enum");
	f.writeStub("Types/Rarity.hasset", HE::AssetType::EnumType);

	// Omitting the value picks the next free number, which is what the editor's
	// own "+ Add Entry" does — 0 for the first.
	const ToolResult a = f.call("type_enum_set", json{
		{ "path", "Types/Rarity.hasset" }, { "name", "Common" } });
	REQUIRE_MESSAGE(!a.isError, codeOf(a));
	CHECK(a.content["value"] == 0);
	const ToolResult b = f.call("type_enum_set", json{
		{ "path", "Types/Rarity.hasset" }, { "name", "Rare" } });
	REQUIRE_MESSAGE(!b.isError, codeOf(b));
	CHECK(b.content["value"] == 1);
	const ToolResult c = f.call("type_enum_set", json{
		{ "path", "Types/Rarity.hasset" }, { "name", "Legendary" }, { "value", 10 } });
	REQUIRE_MESSAGE(!c.isError, codeOf(c));
	const ToolResult d = f.call("type_enum_set", json{
		{ "path", "Types/Rarity.hasset" }, { "name", "Mythic" } });
	REQUIRE_MESSAGE(!d.isError, codeOf(d));
	CHECK(d.content["value"] == 11);

	// The registry is what HorizonCode, the generated constants and the savegame
	// path read — not the file.
	HE::EnumDef def = f.registeredEnum("Types/Rarity.hasset");
	REQUIRE(def.entries.size() == 4);
	CHECK(def.entries[0].name == "Common");
	REQUIRE(def.findEntry("Legendary") != nullptr);
	CHECK(def.findEntry("Legendary")->value == 10);

	// Addressed by NAME, so a second call with the same name is an update rather
	// than a duplicate — and a duplicate name would make the generated constants
	// ambiguous, which is what the panel paints red.
	const ToolResult again = f.call("type_enum_set", json{
		{ "path", "Types/Rarity.hasset" }, { "name", "Rare" }, { "value", 5 } });
	REQUIRE_MESSAGE(!again.isError, codeOf(again));
	CHECK(again.content["created"] == false);
	def = f.registeredEnum("Types/Rarity.hasset");
	CHECK(def.entries.size() == 4);
	CHECK(def.findEntry("Rare")->value == 5);

	const ToolResult gone = f.call("type_enum_remove", json{
		{ "path", "Types/Rarity.hasset" }, { "name", "Mythic" } });
	REQUIRE_MESSAGE(!gone.isError, codeOf(gone));
	CHECK(gone.content["entryCount"] == 3);
	CHECK(f.registeredEnum("Types/Rarity.hasset").findEntry("Mythic") == nullptr);

	const ToolResult miss = f.call("type_enum_remove", json{
		{ "path", "Types/Rarity.hasset" }, { "name", "Mythic" } });
	CHECK(miss.isError);
	CHECK(miss.errorCode == "not_found");

	const ToolResult fraction = f.call("type_enum_set", json{
		{ "path", "Types/Rarity.hasset" }, { "name", "Odd" }, { "value", 1.5 } });
	CHECK(fraction.isError);
	CHECK(fraction.errorCode == "invalid_payload");
}

// ─── A savegame template is a struct on disk ─────────────────────────────────

TEST_CASE("A SaveGame Template takes the field tools, and is not registered as a type")
{
	Fixture f("template");
	f.writeStub("Saves/Slot.hasset", HE::AssetType::SaveGameTemplate);

	const ToolResult r = f.call("type_field_set", json{
		{ "path", "Saves/Slot.hasset" }, { "name", "coins" },
		{ "type", "Int" }, { "default", 100 } });
	REQUIRE_MESSAGE(!r.isError, codeOf(r));

	const ToolResult read = f.call("type_info", json{ { "path", "Saves/Slot.hasset" } });
	REQUIRE_MESSAGE(!read.isError, codeOf(read));
	CHECK(read.content["kind"] == "template");
	REQUIRE(findField(read.content["fields"], "coins") != nullptr);

	// A template is a field SCHEMA, not a type: registering it would put it in
	// every type dropdown in the editor (TypeAssetPanel::saveState makes the same
	// distinction).
	CHECK_FALSE(HE::TypeRegistry::instance().hasStruct("Saves/Slot.hasset"));
}

// ─── The gates ───────────────────────────────────────────────────────────────

TEST_CASE("Every refusal leaves the file and the registry exactly as they were")
{
	Fixture f("gates");
	f.writeStub("Types/Loadout.hasset", HE::AssetType::StructType);
	const std::string before = f.bytes("Types/Loadout.hasset");
	const json add{ { "path", "Types/Loadout.hasset" }, { "name", "slots" },
	                { "type", "Int" } };

	SUBCASE("play mode")
	{
		f.playing = true;
		const ToolResult r = f.call("type_field_set", add);
		CHECK(r.isError);
		CHECK(r.errorCode == "play_mode");
	}
	SUBCASE("a peer holds the asset")
	{
		f.lockedRel = "Types/Loadout.hasset";
		const ToolResult r = f.call("type_field_set", add);
		CHECK(r.isError);
		CHECK(r.errorCode == "locked_by_other");
	}
	SUBCASE("an open tab with unsaved edits")
	{
		f.dirtyRel = "Types/Loadout.hasset";
		const ToolResult r = f.call("type_field_set", add);
		CHECK(r.isError);
		CHECK(r.errorCode == "dirty");
		// A read still works and says so, rather than refusing.
		const ToolResult read = f.call("type_info", json{ { "path", "Types/Loadout.hasset" } });
		REQUIRE_FALSE(read.isError);
		CHECK(read.content["openInEditorUnsaved"] == true);
	}
	SUBCASE("the reserved Engine namespace")
	{
		const ToolResult r = f.call("type_field_set", json{
			{ "path", "Engine/Types/Thing.hasset" }, { "name", "x" }, { "type", "Int" } });
		CHECK(r.isError);
	}

	CHECK(f.bytes("Types/Loadout.hasset") == before);
	CHECK(f.registered("Types/Loadout.hasset").fields.empty());
	CHECK(f.typesChanged == 0);
}

TEST_CASE("A clean open tab is told to re-read the file instead of being left stale")
{
	Fixture f("reload");
	f.writeStub("Types/Loadout.hasset", HE::AssetType::StructType);
	f.openRel = "Types/Loadout.hasset";

	const ToolResult r = f.call("type_field_set", json{
		{ "path", "Types/Loadout.hasset" }, { "name", "slots" }, { "type", "Int" } });
	REQUIRE_MESSAGE(!r.isError, codeOf(r));
	CHECK(r.content["reloadedInEditor"] == true);
	CHECK(f.reloadCalls == 1);
}

TEST_CASE("Two definitions with one display name are reported, not refused")
{
	Fixture f("collision");
	f.writeStub("Types/Loadout.hasset", HE::AssetType::StructType);
	f.writeStub("Other/Loadout.hasset", HE::AssetType::StructType);

	const ToolResult a = f.call("type_field_set", json{
		{ "path", "Types/Loadout.hasset" }, { "name", "slots" }, { "type", "Int" } });
	REQUIRE_MESSAGE(!a.isError, codeOf(a));
	CHECK_FALSE(a.content.contains("nameCollision"));

	// The second one generates the same `horizon.enums.Loadout` / C++ symbol.
	// The panel warns and saves anyway — a file has to be nameable before it can
	// be renamed — so this reports rather than refuses.
	const ToolResult b = f.call("type_field_set", json{
		{ "path", "Other/Loadout.hasset" }, { "name", "slots" }, { "type", "Int" } });
	REQUIRE_MESSAGE(!b.isError, codeOf(b));
	CHECK(b.content["nameCollision"] == true);
	CHECK(f.registered("Other/Loadout.hasset").fields.size() == 1);
}

TEST_CASE("Every type tool is registered with a schema a client can call")
{
	Fixture f("schema");
	for (const char* name : { "type_info", "type_field_set", "type_field_remove",
	                          "type_enum_set", "type_enum_remove" })
	{
		const McpTool* t = f.registry.find(name);
		REQUIRE_MESSAGE(t != nullptr, name);
		CHECK(t->inputSchema.value("type", std::string()) == "object");
		CHECK_FALSE(t->description.empty());
	}
	CHECK_FALSE(f.registry.find("type_info")->mutates);
	CHECK(f.registry.find("type_field_set")->mutates);
	CHECK(f.registry.find("type_enum_remove")->mutates);
}
