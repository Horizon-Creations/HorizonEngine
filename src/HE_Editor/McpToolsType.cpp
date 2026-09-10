#include "McpToolRegistry.h"

#include "EditorAssetTypeCache.h"     // what a path holds, without loading it
#include "McpToolCommon.h"            // the argument readers, the confinement rule, the walk

#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/HAsset.h>
#include <Types/TypeRegistry.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// ─── Authoring a project's own types from outside the editor ─────────────────
// Why struct and enum definitions need tools of their own, which two gates the
// Type Editor has and what a save does besides writing the file:
// McpToolRegistry.h, beside McpTypeHooks. What is worth stating HERE is what the
// handlers promise.
//
//   • A TYPE IS NAMED THE WAY THE PANEL NAMES IT. "Float", "Vec3", "Enum" — the
//     labels of the Type Editor's own dropdown, not the integers the file
//     stores. An unknown name is refused WITH the list, because a client cannot
//     see the enum and a silently wrong type would be a field that reads back as
//     something else.
//
//   • AN ENUM OR STRUCT FIELD MUST NAME A DEFINITION THAT EXISTS. `typeName` is
//     the referenced asset's project-relative path, which is what a
//     HorizonCode::Value carries in its own `typeName` — a field pointing at a
//     path with nothing behind it seeds every instance with an empty value and
//     nothing anywhere says why.
//
//   • THE FOUR CONTAINER FIELDS ARE WRITTEN AS ONE. `isArray` (is it a container
//     at all), `container` (which kind), `keyType` and `keyTypeName` are coupled,
//     and the loader repairs illegal combinations in SILENCE — so a client that
//     set two of them and forgot the third would get a field that is not what it
//     asked for and no error either. One `container` argument sets all of them.
//
//   • A DEFAULT IS WRITTEN IN THE SHAPE ITS OWN TYPE HAS. A number for Float, a
//     three-element array for Vec3, the ENTRY NAME for an Enum. Refused with the
//     expected shape rather than coerced.
//
//   • THE WHOLE DEFINITION IS RE-WRITTEN AND RE-REGISTERED ON EVERY EDIT, which
//     is what the panel's Save does. A file written without the re-registration
//     is a definition every type dropdown in the editor still shows the old
//     version of, until something reloads the project.
//
//   • A REFUSAL IS A NO-OP. Nothing is written, nothing is registered, no header
//     is regenerated and no tab is told to re-read anything.

namespace HE::Ed
{

using nlohmann::json;
using HorizonCode::ContainerKind;
using HorizonCode::PinType;

namespace
{

// ── The vocabulary, as the Type Editor spells it ─────────────────────────────
// Exec is control flow and Ref is a runtime instance handle; neither is data a
// definition can hold, so neither is in this table (the panel's kFieldTypes list
// is the same eleven).
struct TypeName { const char* name; PinType type; };
constexpr TypeName kFieldTypes[] = {
	{ "Float", PinType::Float }, { "Int", PinType::Int }, { "Bool", PinType::Bool },
	{ "String", PinType::String }, { "Vec2", PinType::Vec2 }, { "Vec3", PinType::Vec3 },
	{ "Vec4", PinType::Vec4 }, { "Color", PinType::Color },
	{ "Transform", PinType::Transform }, { "Enum", PinType::Enum },
	{ "Struct", PinType::Struct },
};

const char* pinTypeName(PinType t)
{
	for (const TypeName& n : kFieldTypes)
		if (n.type == t) return n.name;
	return "Float";
}

bool pinTypeFromName(const std::string& s, PinType& out)
{
	for (const TypeName& n : kFieldTypes)
		if (s == n.name) { out = n.type; return true; }
	return false;
}

std::string allTypeNames()
{
	std::string s;
	for (const TypeName& n : kFieldTypes) s += (s.empty() ? "" : ", ") + std::string(n.name);
	return s;
}

const char* containerName(ContainerKind k)
{
	switch (k)
	{
	case ContainerKind::Array: return "array";
	case ContainerKind::Set:   return "set";
	case ContainerKind::Map:   return "map";
	case ContainerKind::None:  break;
	}
	return "none";
}

bool containerFromName(const std::string& s, ContainerKind& out)
{
	if (s == "none"  || s.empty()) { out = ContainerKind::None;  return true; }
	if (s == "array")              { out = ContainerKind::Array; return true; }
	if (s == "set")                { out = ContainerKind::Set;   return true; }
	if (s == "map")                { out = ContainerKind::Map;   return true; }
	return false;
}

// ── The addressed definition ─────────────────────────────────────────────────
// One struct for all three asset types, because the path check, the play-mode
// gate, the lock gate and the unsaved-tab gate are the same four questions for a
// struct, an enum and a savegame template. `kind` is what separates them
// afterwards — and a template is deliberately in here: a SaveGameTemplate IS a
// StructDef on disk (TypeRegistry.h says so), so the field tools serve it too,
// and being able to author the schema of a save without a human is worth more
// than the tidiness of a fourth tool.
struct Def
{
	std::string   rel;
	std::string   abs;
	HE::AssetType type = HE::AssetType::Unknown;
	HE::StructDef structDef;
	HE::EnumDef   enumDef;
	bool          ok = false;
	ToolResult    failure = ToolResult::ok(json::object());

	bool isEnum()     const { return type == HE::AssetType::EnumType; }
	bool isTemplate() const { return type == HE::AssetType::SaveGameTemplate; }
};

std::uint32_t chunkFor(HE::AssetType t)
{
	if (t == HE::AssetType::EnumType)         return HAsset::CHUNK_ENDF;
	if (t == HE::AssetType::SaveGameTemplate) return HAsset::CHUNK_SGTP;
	return HAsset::CHUNK_STDF;
}

// The definition JSON of a type asset, read out of the FILE. Nothing is loaded
// to answer a question — the same rule the input and material readers follow,
// and here it has a second edge: loading a definition REGISTERS it in the
// TypeRegistry as a side effect, so a mere read would change the process's idea
// of what types exist.
bool readPayload(const std::string& abs, HE::AssetType type, std::string& out)
{
	HAsset::Reader r;
	if (!r.open(abs)) return false;
	if (const HAsset::Reader::Chunk* c = r.findChunk(chunkFor(type)))
		out.assign(reinterpret_cast<const char*>(c->data.data()), c->data.size());
	// An absent chunk is not a failure: a freshly created definition asset has no
	// fields and no entries, which is exactly what the panel shows for one.
	return true;
}

Def openDef(ContentManager& content, const McpTypeHooks& h, const json& args,
            bool forWrite, bool wantEnum, bool wantStruct)
{
	Def d;
	const PathCheck p = checkPath(content, strArg(args, "path"), /*mustExist=*/true, "path");
	if (!p.ok) { d.failure = p.failure; return d; }
	d.rel  = p.rel;
	d.abs  = p.abs;
	d.type = EditorAssetTypeCache::assetTypeOf(p.abs);

	const bool isEnum     = d.type == HE::AssetType::EnumType;
	const bool isStruct   = d.type == HE::AssetType::StructType;
	const bool isTemplate = d.type == HE::AssetType::SaveGameTemplate;
	if (!isEnum && !isStruct && !isTemplate)
	{
		d.failure = ToolResult::fail("invalid_path",
			"'" + p.rel + "' is not a Struct, Enum or SaveGame Template asset. "
			"asset_resolve reports what a path holds, type_info without arguments lists "
			"every definition in the project, and asset_create with type 'StructType', "
			"'EnumType' or 'SaveGameTemplate' makes a new one.");
		return d;
	}
	if (wantEnum && !isEnum)
	{
		d.failure = ToolResult::fail("invalid_path",
			"'" + p.rel + "' is a " + std::string(isTemplate ? "SaveGame Template" : "Struct") +
			", which has FIELDS rather than entries — type_field_set is the tool for it.");
		return d;
	}
	if (wantStruct && isEnum)
	{
		d.failure = ToolResult::fail("invalid_path",
			"'" + p.rel + "' is an Enum, which has ENTRIES rather than fields — "
			"type_enum_set is the tool for it.");
		return d;
	}

	std::string payload;
	if (!readPayload(p.abs, d.type, payload))
	{
		d.failure = ToolResult::fail("failed",
			"'" + p.rel + "' could not be read. The editor log carries the reason.");
		return d;
	}
	const std::string name = std::filesystem::path(p.rel).stem().string();
	if (isEnum)
	{
		HE::TypeRegistry::enumFromJson(payload, d.enumDef);
		d.enumDef.name      = name;
		d.enumDef.assetPath = p.rel;
	}
	else
	{
		HE::TypeRegistry::structFromJson(payload, d.structDef);
		d.structDef.name      = name;
		d.structDef.assetPath = p.rel;
	}

	if (forWrite)
	{
		if (p.engine) { d.failure = failEngineReadOnly(p.rel); return d; }
		if (h.isPlaying && h.isPlaying())
		{
			d.failure = ToolResult::fail("play_mode",
				"Play-in-editor is running, and the running session has already bound "
				"this definition — its scripts hold values of it and its dropdowns are "
				"already built. A change now would be half in effect, so it is refused "
				"rather than half-applied. Ask the user to stop play mode.");
			return d;
		}
		if (h.lockedByOther && h.lockedByOther(p.rel))
		{
			d.failure = ToolResult::fail("locked_by_other",
				"Another participant in the collaboration session holds '" + p.rel +
				"' right now. Wait until they let go, or work on something else.");
			return d;
		}
		if (h.isDirty && h.isDirty(p.rel))
		{
			d.failure = ToolResult::fail("dirty",
				"'" + p.rel + "' is open in the Type Editor with unsaved changes. That "
				"tab's own copy is the truth while it is dirty: writing the file would be "
				"reverted by the human's next Save, and there is no way to land an edit "
				"in the tab that they could take back. Ask the user to save or close that "
				"tab, then call again.");
			return d;
		}
	}
	d.ok = true;
	return d;
}

// ── Writing one back ─────────────────────────────────────────────────────────
// Everything TypeAssetPanel::saveState does, in the same order: the cycle gate,
// the file, the registry, the C++ header. The load is the ONLY one in this path
// and nothing is loaded after it, so the pointer taken here cannot be moved out
// from under us by a second asset registering (ContentManager.h: the pool is a
// dense vector).
ToolResult writeDef(ContentManager& content, const McpTypeHooks& h, Def& d, json out)
{
	auto& reg = HE::TypeRegistry::instance();
	if (!d.isEnum() && !d.isTemplate() && reg.structWouldCycle(d.structDef))
		return ToolResult::fail("invalid_payload",
			"That field would make '" + d.rel + "' contain itself, directly or through "
			"another struct — seeding a default value of it would never finish. The Type "
			"Editor refuses the same save. Nothing was written.");

	const std::string payload = d.isEnum() ? HE::TypeRegistry::enumToJson(d.enumDef)
	                                       : HE::TypeRegistry::structToJson(d.structDef);
	const HE::UUID id = content.loadAsset(d.rel);
	bool wrote = false;
	if (!(id == HE::UUID{}))
	{
		if (d.isEnum())
		{
			if (EnumTypeAsset* a = content.getEnumTypeMutable(id))
			{
				a->json = payload;
				wrote = content.saveAsset(*a);
			}
		}
		else if (d.isTemplate())
		{
			if (SaveGameTemplateAsset* a = content.getSaveGameTemplateMutable(id))
			{
				a->json = payload;
				wrote = content.saveAsset(*a);
			}
		}
		else if (StructTypeAsset* a = content.getStructTypeMutable(id))
		{
			a->json = payload;
			wrote = content.saveAsset(*a);
		}
	}
	if (!wrote)
		return ToolResult::fail("failed",
			"Could not write '" + d.rel + "'. A read-only file or a full disk is the "
			"usual cause; the editor log carries the reason.");

	// The registry, or every type dropdown in the editor keeps offering the
	// version before this call. A TEMPLATE is deliberately not registered — it is
	// not a type, just a field schema on disk (TypeAssetPanel::saveState).
	if (d.isEnum())            reg.registerEnum(d.enumDef);
	else if (!d.isTemplate())  reg.registerStruct(d.structDef);

	// In a C++ project the definitions ARE C++ types, so the generated header has
	// to follow or gameplay code compiles against yesterday's struct.
	if (h.onTypesChanged) h.onTypesChanged();

	out["path"] = d.rel;
	// Warned about, not refused — the panel warns and saves too. Two definitions
	// with one display name generate colliding symbols.
	if (!d.isTemplate())
	{
		const std::string name = d.isEnum() ? d.enumDef.name : d.structDef.name;
		const std::string path = d.isEnum() ? d.enumDef.assetPath : d.structDef.assetPath;
		if (reg.nameCollides(name, path))
			out["nameCollision"] = true;
	}
	out["reloadedInEditor"] = h.reloadFromDisk ? h.reloadFromDisk(d.rel) : false;
	return ToolResult::ok(std::move(out));
}

// ── Reporting ────────────────────────────────────────────────────────────────

json fieldJson(const HE::StructField& f)
{
	json j{
		{ "name", f.name },
		{ "type", pinTypeName(f.type) },
	};
	if (!f.typeName.empty()) j["typeName"] = f.typeName;
	const ContainerKind k = f.kind();
	if (k != ContainerKind::None)
	{
		j["container"] = containerName(k);
		if (k == ContainerKind::Map)
		{
			j["keyType"] = pinTypeName(f.keyType);
			if (!f.keyTypeName.empty()) j["keyTypeName"] = f.keyTypeName;
		}
	}
	// The authored default, in the shape this field's type has — the same
	// encoding `type_field_set` takes back. A container's authored starting
	// elements are reported as a count rather than element by element: setting
	// them is not something these tools offer, and a wall of values would read
	// like something a client could send back.
	if (k == ContainerKind::None && f.type != PinType::Struct)
	{
		const HorizonCode::Value& v = f.defaultValue;
		switch (f.type)
		{
		case PinType::Float:  j["default"] = v.f; break;
		case PinType::Int:    j["default"] = v.i; break;
		case PinType::Bool:   j["default"] = v.b; break;
		case PinType::String:
		case PinType::Enum:   j["default"] = v.s; break;
		case PinType::Vec2:   j["default"] = json::array({ v.v2.x, v.v2.y }); break;
		case PinType::Vec3:   j["default"] = json::array({ v.v3.x, v.v3.y, v.v3.z }); break;
		case PinType::Vec4:   j["default"] = json::array({ v.v4.x, v.v4.y, v.v4.z, v.v4.w }); break;
		case PinType::Color:  j["default"] = json::array({ v.col.x, v.col.y, v.col.z, v.col.w }); break;
		default: break;   // Transform: an object, reported by shape below
		}
		if (f.type == PinType::Transform)
			j["default"] = json{
				{ "pos", json::array({ v.tpos.x, v.tpos.y, v.tpos.z }) },
				{ "rot", json::array({ v.trot.x, v.trot.y, v.trot.z }) },
				{ "scl", json::array({ v.tscl.x, v.tscl.y, v.tscl.z }) },
			};
	}
	else if (k != ContainerKind::None && !f.defaultValue.items.empty())
	{
		j["defaultElements"] = static_cast<int>(f.defaultValue.items.size());
	}
	return j;
}

json defJson(const Def& d)
{
	json j{
		{ "path", d.rel },
		{ "name", d.isEnum() ? d.enumDef.name : d.structDef.name },
		{ "kind", d.isEnum() ? "enum" : (d.isTemplate() ? "template" : "struct") },
	};
	if (d.isEnum())
	{
		json entries = json::array();
		for (const HE::EnumEntry& e : d.enumDef.entries)
			entries.push_back(json{ { "name", e.name }, { "value", e.value } });
		j["entries"] = std::move(entries);
	}
	else
	{
		json fields = json::array();
		for (const HE::StructField& f : d.structDef.fields) fields.push_back(fieldJson(f));
		j["fields"] = std::move(fields);
	}
	return j;
}

// ── Reading a default out of a client's JSON ─────────────────────────────────
// The refusal names the shape that was expected, because the client cannot see
// the type table. `ok` false means the argument was there and wrong; an absent
// argument never reaches here.
bool readDefault(const json& v, PinType t, HorizonCode::Value& out, std::string& want)
{
	auto nums = [&v](int n, float* dst) {
		if (!v.is_array() || static_cast<int>(v.size()) != n) return false;
		for (int i = 0; i < n; ++i)
		{
			if (!v[i].is_number()) return false;
			dst[i] = v[i].get<float>();
		}
		return true;
	};
	out.type = t;
	switch (t)
	{
	case PinType::Float:
		want = "a number";
		if (!v.is_number()) return false;
		out.f = v.get<float>();
		return true;
	case PinType::Int:
		want = "a whole number";
		if (!v.is_number_integer()) return false;
		out.i = v.get<int>();
		return true;
	case PinType::Bool:
		want = "true or false";
		if (!v.is_boolean()) return false;
		out.b = v.get<bool>();
		return true;
	case PinType::String:
		want = "a string";
		if (!v.is_string()) return false;
		out.s = v.get<std::string>();
		return true;
	case PinType::Enum:
		want = "the NAME of one of the referenced enum's entries, as a string";
		if (!v.is_string()) return false;
		out.s = v.get<std::string>();
		return true;
	case PinType::Vec2:
		want = "two numbers [x, y]";
		return nums(2, &out.v2.x);
	case PinType::Vec3:
		want = "three numbers [x, y, z]";
		return nums(3, &out.v3.x);
	case PinType::Vec4:
		want = "four numbers [x, y, z, w]";
		return nums(4, &out.v4.x);
	case PinType::Color:
		want = "four numbers [r, g, b, a]";
		return nums(4, &out.col.x);
	case PinType::Transform:
		want = "an object {\"pos\": [x,y,z], \"rot\": [x,y,z], \"scl\": [x,y,z]}";
		if (!v.is_object()) return false;
		{
			const json p = v.value("pos", json::array({ 0.0, 0.0, 0.0 }));
			const json r = v.value("rot", json::array({ 0.0, 0.0, 0.0 }));
			const json s = v.value("scl", json::array({ 1.0, 1.0, 1.0 }));
			auto three = [](const json& a, glm::vec3& dst) {
				if (!a.is_array() || a.size() != 3) return false;
				for (int i = 0; i < 3; ++i)
				{
					if (!a[i].is_number()) return false;
					dst[i] = a[i].get<float>();
				}
				return true;
			};
			return three(p, out.tpos) && three(r, out.trot) && three(s, out.tscl);
		}
	default:
		want = "nothing — a Struct field takes its default from its own definition";
		return false;
	}
}

// ── type_info ────────────────────────────────────────────────────────────────

void addInfo(McpToolRegistry& registry, ContentManager& content,
             const std::shared_ptr<McpTypeHooks>& h)
{
	ContentManager* cm = &content;
	McpTool t;
	t.name        = "type_info";
	t.description =
		"List the project's own types, or read one. Without 'path': every Struct, Enum "
		"and SaveGame Template asset, sorted, with what kind it is and how many fields "
		"or entries it has. With 'path': the whole definition — every field with its "
		"type, container and default, or every enum entry with its value. These are the "
		"types HorizonCode pins, Lua tables, Python dicts, the generated C++ header and "
		"savegame fields all speak.";
	t.inputSchema = objectSchema(json{
		{ "path",  stringProp("Content-relative path of one definition, e.g. "
		                      "'Types/Loadout.hasset'. Omit for the catalogue.") },
		{ "limit", numberProp("Catalogue only: how many at most (default 200).") },
	}, {});
	t.handler = [cm, h](const json& args) -> ToolResult {
		if (!strArg(args, "path").empty())
		{
			Def d = openDef(*cm, *h, args, /*forWrite=*/false, false, false);
			if (!d.ok) return d.failure;
			json out = defJson(d);
			if (h->isDirty && h->isDirty(d.rel)) out["openInEditorUnsaved"] = true;
			return ToolResult::ok(std::move(out));
		}

		int limit = intArg(args, "limit", 200);
		if (limit <= 0) limit = 200;
		bool truncated = false;
		json list = json::array();
		for (const ContentAsset& a : walkContentAssets(
			     *cm, { HE::AssetType::StructType, HE::AssetType::EnumType,
			            HE::AssetType::SaveGameTemplate }, limit, truncated))
		{
			std::string payload;
			readPayload(a.abs, a.type, payload);
			json j{
				{ "path", a.rel },
				{ "name", std::filesystem::path(a.rel).stem().string() },
				{ "kind", a.type == HE::AssetType::EnumType ? "enum"
				          : a.type == HE::AssetType::SaveGameTemplate ? "template" : "struct" },
			};
			if (a.type == HE::AssetType::EnumType)
			{
				HE::EnumDef def;
				HE::TypeRegistry::enumFromJson(payload, def);
				j["entryCount"] = static_cast<int>(def.entries.size());
			}
			else
			{
				HE::StructDef def;
				HE::TypeRegistry::structFromJson(payload, def);
				j["fieldCount"] = static_cast<int>(def.fields.size());
			}
			if (h->isDirty && h->isDirty(a.rel)) j["openInEditorUnsaved"] = true;
			list.push_back(std::move(j));
		}
		json out{ { "types", std::move(list) } };
		if (truncated) out["truncated"] = true;
		return ToolResult::ok(std::move(out));
	};
	registry.add(std::move(t));
}

// ── type_field_set ───────────────────────────────────────────────────────────

void addFieldSet(McpToolRegistry& registry, ContentManager& content,
                 const std::shared_ptr<McpTypeHooks>& h)
{
	ContentManager* cm = &content;
	McpTool t;
	t.name        = "type_field_set";
	t.description =
		"Add a field to a Struct or SaveGame Template, or change one that is already "
		"there. Addressed by NAME: an existing name is updated in place (and keeps its "
		"position), a new one is appended unless 'index' says otherwise. Everything not "
		"named in the call keeps its current value on an update, so changing only a "
		"default does not silently reset the type. A field that would make the struct "
		"contain itself is refused.";
	t.inputSchema = objectSchema(json{
		{ "path", stringProp("Content-relative path of the Struct or SaveGame Template, "
		                     "e.g. 'Types/Loadout.hasset'.") },
		{ "name", stringProp("Field name. An existing one is updated, a new one added.") },
		{ "type", stringProp("Field type, one of: Float, Int, Bool, String, Vec2, Vec3, "
		                     "Vec4, Color, Transform, Enum, Struct. Required for a NEW "
		                     "field.") },
		{ "typeName", stringProp("For an Enum or Struct field: the content-relative path "
		                         "of the definition it refers to, e.g. "
		                         "'Types/Rarity.hasset'. type_info lists them.") },
		{ "container", stringProp("'none' (a single value, the default), 'array', 'set' "
		                          "or 'map'.") },
		{ "keyType", stringProp("For a map: the key's type — Int, String or Enum. "
		                        "Default String.") },
		{ "keyTypeName", stringProp("For a map keyed by Enum: the enum definition's "
		                            "path.") },
		{ "default", json{ { "description",
		                     "The field's starting value, in the shape its type has: a "
		                     "number for Float, [x, y, z] for Vec3, the entry NAME for an "
		                     "Enum. Not for containers or Struct fields — those seed from "
		                     "their own definition." } } },
		{ "index", numberProp("Where to put a NEW field (0 = first). Omit to append; "
		                      "ignored when the field already exists.") },
	}, { "path", "name" });
	t.mutates = true;
	t.handler = [cm, h](const json& args) -> ToolResult {
		Def d = openDef(*cm, *h, args, /*forWrite=*/true, /*wantEnum=*/false,
		                /*wantStruct=*/true);
		if (!d.ok) return d.failure;

		const std::string name = strArg(args, "name");
		if (name.empty())
			return ToolResult::fail("invalid_payload",
				"'name' is required and must not be empty — a field with no name is "
				"dropped by the loader without a word.");

		auto& fields = d.structDef.fields;
		const auto at = std::find_if(fields.begin(), fields.end(),
		                             [&name](const HE::StructField& f) { return f.name == name; });
		const bool existing = at != fields.end();
		// Updated in place: everything the call does not mention keeps its value,
		// so setting a default cannot reset a type and vice versa.
		HE::StructField f = existing ? *at : HE::StructField{};
		f.name = name;

		const std::string typeArg = strArg(args, "type");
		if (!typeArg.empty())
		{
			if (!pinTypeFromName(typeArg, f.type))
				return ToolResult::fail("invalid_payload",
					"'" + typeArg + "' is not a field type. The types a definition can "
					"hold are: " + allTypeNames() + ".");
		}
		else if (!existing)
		{
			return ToolResult::fail("invalid_payload",
				"'type' is required for a new field. One of: " + allTypeNames() + ".");
		}

		if (hasArg(args, "typeName")) f.typeName = strArg(args, "typeName");
		if (hasArg(args, "container"))
		{
			ContainerKind k;
			if (!containerFromName(strArg(args, "container"), k))
				return ToolResult::fail("invalid_payload",
					"'" + strArg(args, "container") + "' is not a container kind. One of: "
					"none, array, set, map.");
			// The four coupled fields, written as one — the loader repairs an
			// inconsistent pair in silence, so half of them is worse than none.
			f.container = (k == ContainerKind::Array) ? ContainerKind::None : k;
			f.isArray   = (k != ContainerKind::None);
		}
		if (hasArg(args, "keyType"))
		{
			PinType kt;
			if (!pinTypeFromName(strArg(args, "keyType"), kt))
				return ToolResult::fail("invalid_payload",
					"'" + strArg(args, "keyType") + "' is not a type. One of: " +
					allTypeNames() + ".");
			f.keyType = kt;
		}
		if (hasArg(args, "keyTypeName")) f.keyTypeName = strArg(args, "keyTypeName");

		// The referenced definition has to exist. A field pointing at nothing
		// seeds every instance with an empty value and says so nowhere.
		auto checkRef = [cm](PinType type, const std::string& path, const char* what,
		                     ToolResult& fail) -> bool {
			if (type != PinType::Enum && type != PinType::Struct) return true;
			if (path.empty())
			{
				fail = ToolResult::fail("invalid_payload",
					std::string("A ") + (type == PinType::Enum ? "n Enum" : " Struct") +
					" " + what + " has to name the definition it refers to in '" +
					(std::string(what) == "field" ? "typeName" : "keyTypeName") +
					"' — type_info lists them.");
				return false;
			}
			const std::string abs = cm->resolveAbsolutePath(path);
			const HE::AssetType t = abs.empty() ? HE::AssetType::Unknown
			                                    : EditorAssetTypeCache::assetTypeOf(abs);
			const bool okType = (type == PinType::Enum) ? t == HE::AssetType::EnumType
			                                            : t == HE::AssetType::StructType;
			if (!okType)
			{
				fail = ToolResult::fail("invalid_payload",
					"'" + path + "' is not " +
					(type == PinType::Enum ? "an Enum" : "a Struct") + " definition in "
					"this project. type_info lists what there is; a field pointing at "
					"nothing seeds every instance with an empty value and nothing "
					"anywhere says why.");
				return false;
			}
			return true;
		};
		ToolResult refFail = ToolResult::ok(json::object());
		if (!checkRef(f.type, f.typeName, "field", refFail)) return refFail;
		if (f.kind() == ContainerKind::Map)
		{
			if (!HorizonCode::isValidMapKeyType(f.keyType))
				return ToolResult::fail("invalid_payload",
					"A map cannot be keyed by " + std::string(pinTypeName(f.keyType)) +
					". The key types with a cheap, exact identity are Int, String and "
					"Enum — a float key rests on float equality, and a two-slot map is a "
					"struct.");
			if (!checkRef(f.keyType, f.keyTypeName, "key", refFail)) return refFail;
		}

		if (hasArg(args, "default"))
		{
			if (f.kind() != ContainerKind::None || f.type == PinType::Struct)
				return ToolResult::fail("invalid_payload",
					"A " + std::string(f.type == PinType::Struct ? "Struct field takes its "
					"default from its own definition" : "container field starts empty "
					"here") + ", so 'default' has nothing to write. Remove it from the "
					"call.");
			HorizonCode::Value v;
			std::string want;
			if (!readDefault(args["default"], f.type, v, want))
				return ToolResult::fail("invalid_payload",
					"'default' for a " + std::string(pinTypeName(f.type)) +
					" field has to be " + want + ".");
			f.defaultValue = v;
		}
		// The Value carries its own idea of what it is, and the seeder reads it —
		// a field whose type changed while its default did not would otherwise
		// keep answering as the old type.
		f.defaultValue.type        = f.type;
		f.defaultValue.typeName    = f.typeName;
		f.defaultValue.isArray     = f.isArray;
		f.defaultValue.container   = f.container;
		f.defaultValue.keyType     = f.keyType;
		f.defaultValue.keyTypeName = f.keyTypeName;

		int index = -1;
		if (existing) *at = std::move(f);
		else
		{
			index = intArg(args, "index", -1);
			if (index < 0 || index > static_cast<int>(fields.size()))
				fields.push_back(std::move(f));
			else
				fields.insert(fields.begin() + index, std::move(f));
		}

		json out{
			{ "name",       name },
			{ "created",    !existing },
			{ "fieldCount", static_cast<int>(fields.size()) },
		};
		const auto now = std::find_if(fields.begin(), fields.end(),
		                              [&name](const HE::StructField& x) { return x.name == name; });
		if (now != fields.end())
		{
			out["field"] = fieldJson(*now);
			out["index"] = static_cast<int>(now - fields.begin());
		}
		return writeDef(*cm, *h, d, std::move(out));
	};
	registry.add(std::move(t));
}

// ── type_field_remove ────────────────────────────────────────────────────────

void addFieldRemove(McpToolRegistry& registry, ContentManager& content,
                    const std::shared_ptr<McpTypeHooks>& h)
{
	ContentManager* cm = &content;
	McpTool t;
	t.name        = "type_field_remove";
	t.description =
		"Remove a field from a Struct or SaveGame Template. Every value of that type "
		"that was already saved keeps the removed key on disk and simply stops being "
		"read — definitions persist name-keyed, so nothing shifts.";
	t.inputSchema = objectSchema(json{
		{ "path", stringProp("Content-relative path of the Struct or SaveGame Template.") },
		{ "name", stringProp("Name of the field to remove.") },
	}, { "path", "name" });
	t.mutates = true;
	t.handler = [cm, h](const json& args) -> ToolResult {
		Def d = openDef(*cm, *h, args, /*forWrite=*/true, false, /*wantStruct=*/true);
		if (!d.ok) return d.failure;

		const std::string name = strArg(args, "name");
		auto& fields = d.structDef.fields;
		const auto at = std::find_if(fields.begin(), fields.end(),
		                             [&name](const HE::StructField& f) { return f.name == name; });
		if (at == fields.end())
		{
			std::string have;
			for (const HE::StructField& f : fields) have += (have.empty() ? "" : ", ") + f.name;
			return ToolResult::fail("not_found",
				"'" + d.rel + "' has no field called '" + name + "'. It has: " +
				(have.empty() ? "no fields at all" : have) + ".");
		}
		fields.erase(at);

		return writeDef(*cm, *h, d, json{
			{ "removed",    name },
			{ "fieldCount", static_cast<int>(fields.size()) },
		});
	};
	registry.add(std::move(t));
}

// ── type_enum_set / type_enum_remove ─────────────────────────────────────────

void addEnumSet(McpToolRegistry& registry, ContentManager& content,
                const std::shared_ptr<McpTypeHooks>& h)
{
	ContentManager* cm = &content;
	McpTool t;
	t.name        = "type_enum_set";
	t.description =
		"Add an entry to an Enum, or change the value of one that is there. Addressed "
		"by NAME, which is what everything else refers to an entry by — a saved value "
		"that no longer matches an entry falls back to the first one, so renumbering an "
		"existing entry moves data. Omit 'value' for the next free number, which is "
		"what the editor's own '+ Add Entry' picks.";
	t.inputSchema = objectSchema(json{
		{ "path",  stringProp("Content-relative path of the Enum asset, e.g. "
		                      "'Types/Rarity.hasset'.") },
		{ "name",  stringProp("Entry name. An existing one is updated, a new one added.") },
		{ "value", numberProp("The whole number behind the name. Omit for the next free "
		                      "one (highest + 1).") },
	}, { "path", "name" });
	t.mutates = true;
	t.handler = [cm, h](const json& args) -> ToolResult {
		Def d = openDef(*cm, *h, args, /*forWrite=*/true, /*wantEnum=*/true, false);
		if (!d.ok) return d.failure;

		const std::string name = strArg(args, "name");
		if (name.empty())
			return ToolResult::fail("invalid_payload",
				"'name' is required and must not be empty — an entry with no name is a "
				"constant nothing can spell.");
		if (hasArg(args, "value") && !args["value"].is_number_integer())
			return ToolResult::fail("invalid_payload",
				"'value' has to be a whole number — an enum entry is an int, and 2.5 "
				"would arrive as 2 with nothing to say so.");

		auto& entries = d.enumDef.entries;
		const auto at = std::find_if(entries.begin(), entries.end(),
		                             [&name](const HE::EnumEntry& e) { return e.name == name; });
		const bool existing = at != entries.end();

		int value;
		if (hasArg(args, "value")) value = intArg(args, "value", 0);
		else if (existing)         value = at->value;
		else
		{
			// The panel's "+ Add Entry": highest + 1, 0 for the first.
			value = 0;
			for (const HE::EnumEntry& e : entries) value = (std::max)(value, e.value + 1);
		}

		// A value two entries share is legal (aliases exist) but a NAME two share
		// is not: findEntry takes the first and the generated constants collide.
		// The name is the address here, so a duplicate cannot arise from an update.
		if (existing) at->value = value;
		else          entries.push_back(HE::EnumEntry{ name, value });

		return writeDef(*cm, *h, d, json{
			{ "name",       name },
			{ "value",      value },
			{ "created",    !existing },
			{ "entryCount", static_cast<int>(entries.size()) },
		});
	};
	registry.add(std::move(t));
}

void addEnumRemove(McpToolRegistry& registry, ContentManager& content,
                   const std::shared_ptr<McpTypeHooks>& h)
{
	ContentManager* cm = &content;
	McpTool t;
	t.name        = "type_enum_remove";
	t.description =
		"Remove an entry from an Enum. Anything that had this entry saved falls back to "
		"the enum's FIRST entry when it is read again — the lookup answers with the "
		"fallback rather than failing, so a removal is a quiet data change, not an "
		"error later.";
	t.inputSchema = objectSchema(json{
		{ "path", stringProp("Content-relative path of the Enum asset.") },
		{ "name", stringProp("Name of the entry to remove.") },
	}, { "path", "name" });
	t.mutates = true;
	t.handler = [cm, h](const json& args) -> ToolResult {
		Def d = openDef(*cm, *h, args, /*forWrite=*/true, /*wantEnum=*/true, false);
		if (!d.ok) return d.failure;

		const std::string name = strArg(args, "name");
		auto& entries = d.enumDef.entries;
		const auto at = std::find_if(entries.begin(), entries.end(),
		                             [&name](const HE::EnumEntry& e) { return e.name == name; });
		if (at == entries.end())
		{
			std::string have;
			for (const HE::EnumEntry& e : entries) have += (have.empty() ? "" : ", ") + e.name;
			return ToolResult::fail("not_found",
				"'" + d.rel + "' has no entry called '" + name + "'. It has: " +
				(have.empty() ? "no entries at all" : have) + ".");
		}
		entries.erase(at);

		return writeDef(*cm, *h, d, json{
			{ "removed",    name },
			{ "entryCount", static_cast<int>(entries.size()) },
		});
	};
	registry.add(std::move(t));
}

} // namespace

void registerTypeTools(McpToolRegistry& registry, ContentManager& content,
                       McpTypeHooks hooks)
{
	// Shared rather than copied into each handler, like the material and prefab
	// tools: the hooks hold std::functions that capture the editor, and one copy
	// per handler would be one chance per handler to let one go stale.
	auto h = std::make_shared<McpTypeHooks>(std::move(hooks));
	addInfo(registry, content, h);
	addFieldSet(registry, content, h);
	addFieldRemove(registry, content, h);
	addEnumSet(registry, content, h);
	addEnumRemove(registry, content, h);
}

} // namespace HE::Ed
