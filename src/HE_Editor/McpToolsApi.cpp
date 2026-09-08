#include "McpToolsApi.h"

#include "HcNodeDocs.h"          // what each engine call does, in words

#include <Types/TypeRegistry.h>   // struct field layout for a Struct-typed value

#include <algorithm>
#include <cctype>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

// See McpToolsApi.h for what this file is and which rows it refuses.

namespace HE::Ed
{

using nlohmann::json;
using HC = HorizonCode::PinType;
using HorizonCode::Value;

namespace
{

// ── Refused groups ───────────────────────────────────────────────────────────
// The pure rows that read something outside the project. `clipboard.getText`
// hands over whatever the human last copied — a password, a private URL — and
// `process.which` probes the machine for installed binaries. Neither is needed
// to place an object or author a graph, and both would be a strange thing for a
// scene editor's remote control to be able to do.
//
// Everything else stays in by default, on purpose: a group added to the engine
// later should become reachable without an edit here, and the exec/pure split
// already keeps the writing half out.
constexpr const char* kDeniedGroups[] = { "clipboard", "process" };

std::string groupOf(const std::string& id)
{
	const auto dot = id.find('.');
	return dot == std::string::npos ? std::string() : id.substr(0, dot);
}

std::string lowered(std::string v)
{
	std::transform(v.begin(), v.end(), v.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return v;
}

const char* pinTypeName(HC t)
{
	switch (t)
	{
	case HC::Exec:      return "Exec";
	case HC::Float:     return "Float";
	case HC::Bool:      return "Bool";
	case HC::Int:       return "Int";
	case HC::String:    return "String";
	case HC::Vec2:      return "Vec2";
	case HC::Color:     return "Color";
	case HC::Ref:       return "Object";
	case HC::Transform: return "Transform";
	case HC::Enum:      return "Enum";
	case HC::Struct:    return "Struct";
	case HC::Vec3:      return "Vec3";
	case HC::Vec4:      return "Vec4";
	}
	return "?";
}

// "Does this Int mean an entity?" — the registry's own marker, set in the
// post-pass at the bottom of EngineApi.cpp for the leading `entity` parameter of
// every row that acts on one. Asking the flag rather than keeping a list here is
// the point of it existing: a row added later is covered without an edit.
bool looksLikeEntity(const HE::api::ApiParam& p)
{
	return p.type == HC::Int &&
	       (p.selfDefault || (p.name && std::strcmp(p.name, "entity") == 0));
}

// ── Schema per pin type ──────────────────────────────────────────────────────
// The vectors are ARRAYS of numbers, exactly as the entity tools already spell a
// position ([x, y, z]) — one shape for the whole interface, so a client that
// learned it at `entity_set_transform` does not have to learn a second one here.
json schemaFor(const HE::api::ApiParam& p)
{
	json s = json::object();
	auto fixed = [](int n, const char* what) {
		return json{ { "type", "array" },
		             { "items", json{ { "type", "number" } } },
		             { "minItems", n }, { "maxItems", n },
		             { "description", what } };
	};
	switch (p.type)
	{
	case HC::Float: s = json{ { "type", "number" } }; break;
	case HC::Bool:  s = json{ { "type", "boolean" } }; break;
	case HC::Int:
		if (looksLikeEntity(p))
			s = json{ { "type", json::array({ "integer", "string" }) },
			          { "description", "An entity: either its uuid (as returned by "
			                           "entity_list) or a raw engine handle from an "
			                           "earlier call on this interface." } };
		else
			s = json{ { "type", "integer" } };
		break;
	case HC::String: s = json{ { "type", "string" } }; break;
	case HC::Enum:
		s = json{ { "type", "integer" },
		          { "description", "Enum entry, as its zero-based index." } };
		break;
	case HC::Vec2:      s = fixed(2, "[x, y]"); break;
	case HC::Vec3:      s = fixed(3, "[x, y, z]"); break;
	case HC::Color:     s = fixed(4, "[r, g, b, a], each 0..1"); break;
	case HC::Vec4:      s = fixed(4, "[x, y, z, w]"); break;
	case HC::Ref:
		s = json{ { "type", "integer" },
		          { "description", "A runtime object handle (0 = none). Objects only "
		                           "exist while the game runs, so this is normally 0 "
		                           "outside play mode." } };
		break;
	case HC::Transform:
		s = json{ { "type", "object" },
		          { "description", "{ pos: [x,y,z], rot: [x,y,z] in degrees, "
		                           "scl: [x,y,z] }" } };
		break;
	case HC::Struct:
		s = json{ { "type", "object" },
		          { "description", "A project struct: its fields by name, plus "
		                           "'__type' naming the definition asset." } };
		break;
	case HC::Exec:
		s = json{ { "type", "null" } };
		break;
	}
	if (p.isArray) return json{ { "type", "array" }, { "items", std::move(s) } };
	return s;
}

// ── JSON → Value ─────────────────────────────────────────────────────────────
// `outError` is a sentence for the model, not a log line: it is the entire
// explanation it gets for why its call did not run.
bool numbers(const json& j, int n, float* out)
{
	if (!j.is_array() || static_cast<int>(j.size()) != n) return false;
	for (int i = 0; i < n; ++i)
	{
		if (!j[i].is_number()) return false;
		out[i] = j[i].get<float>();
	}
	return true;
}

bool scalarFromJson(const json& j, const HE::api::ApiParam& p, const McpApiHooks& hooks,
                    Value& out, std::string& outError, std::string& outCode)
{
	const std::string name = p.name ? p.name : "";
	auto wrong = [&](const char* want) {
		outCode  = "invalid_argument";
		outError = "'" + name + "' has to be " + want + ".";
		return false;
	};
	float v[4] = { 0, 0, 0, 0 };
	switch (p.type)
	{
	case HC::Float:
		if (!j.is_number()) return wrong("a number");
		out = Value::ofFloat(j.get<float>());
		return true;
	case HC::Bool:
		if (!j.is_boolean()) return wrong("true or false");
		out = Value::ofBool(j.get<bool>());
		return true;
	case HC::Int:
	case HC::Enum:
		// An Enum entry is an index and never an entity, so a string there is a
		// client naming the entry ("Idle") — which has to read as "that is not a
		// number", not as "no entity has that uuid".
		if (j.is_string() && p.type == HC::Int)
		{
			// A uuid. Deliberately allowed for EVERY Int and not just for the
			// ones the registry marks (`entity.distance` takes two plain `a`/`b`
			// handles), because a client that has a uuid and no handle would
			// otherwise be unable to call half of the surface.
			if (!hooks.entityByUuid)
			{
				outCode  = "no_context";
				outError = "No scene is open, so the uuid in '" + name +
				           "' cannot be resolved.";
				return false;
			}
			const std::int64_t e = hooks.entityByUuid(j.get<std::string>());
			if (e < 0)
			{
				// Never 0 here: 0 is a valid handle, so a miss that fell through
				// to it would act on whichever entity happens to be first.
				outCode  = "not_found";
				outError = "No entity with uuid '" + j.get<std::string>() +
				           "' in the open scene (parameter '" + name +
				           "'). entity_list shows what exists.";
				return false;
			}
			Value r = Value::ofInt(static_cast<int>(static_cast<std::uint32_t>(e)));
			r.type  = p.type;
			out     = r;
			return true;
		}
		if (!j.is_number_integer()) return wrong("a whole number (or an entity uuid)");
		{
			Value r = Value::ofInt(static_cast<int>(
				static_cast<std::uint32_t>(j.get<std::int64_t>())));
			r.type = p.type;
			out    = r;
		}
		return true;
	case HC::String:
		if (!j.is_string()) return wrong("a string");
		out = Value::ofString(j.get<std::string>());
		return true;
	case HC::Ref:
		if (!j.is_number_integer()) return wrong("an object handle (a whole number)");
		out = Value::ofRef(static_cast<std::uint32_t>(j.get<std::int64_t>()));
		return true;
	case HC::Vec2:
		if (!numbers(j, 2, v)) return wrong("an array of 2 numbers [x, y]");
		out = Value::ofVec2({ v[0], v[1] });
		return true;
	case HC::Vec3:
		if (!numbers(j, 3, v)) return wrong("an array of 3 numbers [x, y, z]");
		out = Value::ofVec3({ v[0], v[1], v[2] });
		return true;
	case HC::Color:
		if (!numbers(j, 4, v)) return wrong("an array of 4 numbers [r, g, b, a]");
		out = Value::ofColor({ v[0], v[1], v[2], v[3] });
		return true;
	case HC::Vec4:
		if (!numbers(j, 4, v)) return wrong("an array of 4 numbers [x, y, z, w]");
		out = Value::ofVec4({ v[0], v[1], v[2], v[3] });
		return true;
	case HC::Transform:
	{
		if (!j.is_object()) return wrong("an object with pos/rot/scl");
		glm::vec3 pos(0.0f), rot(0.0f), scl(1.0f);
		auto part = [&](const char* key, glm::vec3& dst) {
			const auto it = j.find(key);
			if (it == j.end()) return true;                       // keep the default
			float t[3];
			if (!numbers(*it, 3, t)) return false;
			dst = { t[0], t[1], t[2] };
			return true;
		};
		if (!part("pos", pos) || !part("rot", rot) || !part("scl", scl))
			return wrong("an object whose pos/rot/scl are arrays of 3 numbers");
		out = Value::ofTransform(pos, rot, scl);
		return true;
	}
	case HC::Struct:
	{
		// The only two Struct rows are save.setStruct/getStruct, and only the
		// getter is pure — so this side is reached by an array element or by a
		// row added later. `__type` has to travel with the value: a struct
		// without its definition cannot be laid out in field order.
		if (!j.is_object()) return wrong("an object with a '__type' and its fields");
		const auto tn = j.find("__type");
		if (tn == j.end() || !tn->is_string())
			return wrong("an object naming its definition in '__type'");
		Value r = HE::TypeRegistry::instance().makeDefaultValue(tn->get<std::string>());
		HE::StructDef def;
		if (!HE::TypeRegistry::instance().getStruct(tn->get<std::string>(), def))
		{
			outCode  = "not_found";
			outError = "'" + tn->get<std::string>() + "' is not a struct this project "
			           "defines (parameter '" + name + "').";
			return false;
		}
		for (std::size_t i = 0; i < def.fields.size() && i < r.items.size(); ++i)
		{
			const auto it = j.find(def.fields[i].name);
			if (it == j.end()) continue;                          // keep the default
			HE::api::ApiParam fp;
			fp.name = def.fields[i].name.c_str();
			fp.type = def.fields[i].type;
			Value fv;
			if (!scalarFromJson(*it, fp, hooks, fv, outError, outCode)) return false;
			fv.typeName = def.fields[i].typeName;
			r.items[i]  = std::move(fv);
		}
		out = std::move(r);
		return true;
	}
	case HC::Exec:
		outCode  = "unsupported_signature";
		outError = "'" + name + "' is an exec pin and carries no value.";
		return false;
	}
	return wrong("a value");
}

bool valueFromJson(const json& j, const HE::api::ApiParam& p, const McpApiHooks& hooks,
                   Value& out, std::string& outError, std::string& outCode)
{
	if (!p.isArray) return scalarFromJson(j, p, hooks, out, outError, outCode);
	if (!j.is_array())
	{
		outCode  = "invalid_argument";
		outError = "'" + std::string(p.name ? p.name : "") + "' has to be an array.";
		return false;
	}
	HE::api::ApiParam elem = p;
	elem.isArray = false;
	Value r = Value::ofArray(p.type);
	for (const json& e : j)
	{
		Value item;
		if (!scalarFromJson(e, elem, hooks, item, outError, outCode)) return false;
		r.items.push_back(std::move(item));
	}
	out = std::move(r);
	return true;
}

// ── Value → JSON ─────────────────────────────────────────────────────────────
// The mirror of the block above, and it reads the field the value's own type
// says: a Vec3 keeps its own storage (`v3`) rather than borrowing `col`, and
// mixing the two would hand every zero vector the alpha-1 of a colour.
json scalarToJson(const Value& v, HC t)
{
	switch (t)
	{
	case HC::Float:     return v.f;
	case HC::Bool:      return v.b;
	case HC::Int:
	case HC::Enum:      return v.i;
	case HC::String:    return v.s;
	case HC::Ref:       return v.ref;
	case HC::Vec2:      return json::array({ v.v2.x, v.v2.y });
	case HC::Vec3:      return json::array({ v.v3.x, v.v3.y, v.v3.z });
	case HC::Color:     return json::array({ v.col.x, v.col.y, v.col.z, v.col.w });
	case HC::Vec4:      return json::array({ v.v4.x, v.v4.y, v.v4.z, v.v4.w });
	case HC::Transform:
		return json{ { "pos", json::array({ v.tpos.x, v.tpos.y, v.tpos.z }) },
		             { "rot", json::array({ v.trot.x, v.trot.y, v.trot.z }) },
		             { "scl", json::array({ v.tscl.x, v.tscl.y, v.tscl.z }) } };
	case HC::Struct:
	{
		json o = json::object();
		o["__type"] = v.typeName;
		HE::StructDef def;
		if (HE::TypeRegistry::instance().getStruct(v.typeName, def))
			for (std::size_t i = 0; i < def.fields.size() && i < v.items.size(); ++i)
				o[def.fields[i].name] = scalarToJson(v.items[i], def.fields[i].type);
		return o;
	}
	case HC::Exec: break;
	}
	return nullptr;
}

json valueToJson(const Value& v, const HE::api::ApiParam& p)
{
	if (!p.isArray) return scalarToJson(v, p.type);
	json a = json::array();
	for (const Value& item : v.items) a.push_back(scalarToJson(item, p.type));
	return a;
}

json paramList(const std::vector<HE::api::ApiParam>& ps)
{
	json a = json::array();
	for (const auto& p : ps)
	{
		json e{ { "name", p.name ? p.name : "" },
		        { "type", pinTypeName(p.type) },
		        { "isArray", p.isArray } };
		if (p.selfDefault) e["isEntity"] = true;
		a.push_back(std::move(e));
	}
	return a;
}

} // namespace

// ── The name ─────────────────────────────────────────────────────────────────

std::string apiToolName(const std::string& id)
{
	std::string out = "api_" + id;
	for (char& c : out)
		if (c == '.') c = '_';
	return out;
}

// ── The policy ───────────────────────────────────────────────────────────────

bool apiRowCallable(const HE::api::ApiFn& fn, std::string* outReason)
{
	auto no = [&](const char* why) { if (outReason) *outReason = why; return false; };
	if (fn.isExec)
		// The rule from the plan, and the reason the gateway exists: an exec row
		// writes into the world without undo, without the lock gate and without
		// telling a collaboration session. The mutations a client may make are
		// the entity_* and hc_* tools, and those go through EditorCommands.
		return no("exec");
	const std::string group = groupOf(fn.id ? fn.id : "");
	for (const char* denied : kDeniedGroups)
		if (group == denied) return no("denied_group");
	for (const auto& p : fn.params)
		if (p.type == HC::Exec) return no("unsupported_signature");
	if (outReason) outReason->clear();
	return true;
}

// ── The row, as a tool ───────────────────────────────────────────────────────

McpTool apiRowTool(const HE::api::ApiFn& fn, const McpApiHooks& hooks)
{
	McpTool t;
	t.name = apiToolName(fn.id ? fn.id : "");
	// Pure by construction (apiRowCallable refuses the rest), so it neither
	// counts as a mutation nor is refused while play-in-editor runs — reading
	// the running world is exactly what this half of the surface is for.
	t.mutates = false;

	const std::string shown = fn.displayName ? fn.displayName : (fn.id ? fn.id : "");
	std::string desc = NodeDocs::engineCall(fn.id ? fn.id : "");
	if (desc.empty()) desc = shown;
	// The id and the category travel in the description because the tool NAME is
	// all the client sees in a list, and `api_env_fogDensity` does not say which
	// engine call it is nor where it belongs.
	t.description = shown + " (" + std::string(fn.id ? fn.id : "") + ", " +
	                std::string(fn.category ? fn.category : "") + "). " + desc;

	json props = json::object();
	std::vector<std::string> required;
	for (const auto& p : fn.params)
	{
		const std::string name = p.name ? p.name : "";
		if (name.empty()) continue;
		props[name] = schemaFor(p);
		// Everything is required. The registry's thunks tolerate a missing
		// argument and fill in a zero, which is right for a graph with an
		// unwired pin and wrong here: a client that forgot the entity would act
		// on handle 0 instead of being told it forgot.
		required.push_back(name);
	}
	t.inputSchema = json{ { "type", "object" },
	                      { "properties", std::move(props) },
	                      { "additionalProperties", false } };
	if (!required.empty()) t.inputSchema["required"] = required;

	// `registry()` builds its table once into a function-local static, so the
	// row's address outlives every tool that points at it.
	const HE::api::ApiFn* row = &fn;
	t.handler = [row, hooks](const json& args) -> ToolResult {
		if (!hooks.makeCtx)
			return ToolResult::fail("no_context",
			                        "The editor has no engine context — no project is "
			                        "loaded yet. Call scene_info to see what is open.");
		std::vector<Value> in;
		in.reserve(row->params.size());
		for (const auto& p : row->params)
		{
			const std::string name = p.name ? p.name : "";
			const json* given = nullptr;
			if (args.is_object())
				if (const auto it = args.find(name); it != args.end()) given = &(*it);
			if (!given)
				return ToolResult::fail("invalid_argument",
				                        "'" + name + "' is missing. Every parameter of "
				                        "an engine call has to be given — there are no "
				                        "defaults on this interface.");
			Value v;
			std::string err, code;
			if (!valueFromJson(*given, p, hooks, v, err, code))
				return ToolResult::fail(code, err);
			in.push_back(std::move(v));
		}

		HE::api::Ctx ctx = hooks.makeCtx();
		std::vector<Value> out;
		try
		{
			out = row->invoke(ctx, in);
		}
		catch (const std::exception& e)
		{
			// The one catch site on this path, and not belt-and-braces: `invoke`
			// lands in engine code that was written for a graph, where a throw
			// takes the frame down. Here it would take the editor down on
			// somebody else's behalf.
			return ToolResult::fail("engine_error",
			                        std::string("The engine call failed: ") + e.what());
		}
		catch (...)
		{
			return ToolResult::fail("engine_error", "The engine call failed.");
		}

		json result = json::object();
		for (std::size_t i = 0; i < row->results.size(); ++i)
		{
			const auto& r = row->results[i];
			const std::string name = r.name ? r.name : ("result" + std::to_string(i));
			const Value v = i < out.size() ? out[i] : Value{};
			result[name] = valueToJson(v, r);
			// An entity comes back as a raw handle, which is not the address the
			// rest of this interface speaks. The uuid alongside is what lets a
			// raycast hit be handed straight to entity_get.
			if (!r.isArray && looksLikeEntity(r) && hooks.uuidOf)
			{
				const std::string uuid = hooks.uuidOf(static_cast<std::uint32_t>(v.i));
				if (!uuid.empty()) result[name + "Uuid"] = uuid;
			}
		}
		return ToolResult::ok(std::move(result));
	};
	return t;
}

// ── Registration ─────────────────────────────────────────────────────────────

void registerApiTools(McpToolRegistry& registry, McpApiHooks hooks)
{
	// ── api_list ─────────────────────────────────────────────────────────────
	// The whole registry, callable or not. The refused rows are in here on
	// purpose: a client looking for "set position" finds `transform.setPosition`
	// with `callable: false`, the reason and the pointer to the gateway tool that
	// does it properly — where an absence would only look like a gap.
	{
		McpTool t;
		t.name        = "api_list";
		t.description =
			"The engine's gameplay API: every function the scripting languages and "
			"HorizonCode can call. The pure ones are also tools of this server, named "
			"api_<group>_<function>; the ones that CHANGE the world are listed with "
			"callable=false, because a change has to go through the editor's own tools "
			"(entity_*, hc_*) to be undoable. Also the catalogue for wiring an "
			"'Engine Call' node with hc_add_node.";
		t.inputSchema = json{
			{ "type", "object" },
			{ "properties", json{
				{ "query", json{ { "type", "string" },
				                 { "description", "Case-insensitive substring of the id, "
				                                  "the display name or the category." } } },
				{ "callableOnly", json{ { "type", "boolean" },
				                        { "description", "Only rows that are tools of "
				                                         "this server (default false)." } } },
				{ "limit", json{ { "type", "integer" }, { "minimum", 1 },
				                 { "description", "At most this many rows (default 60)." } } },
			} },
			{ "additionalProperties", false },
		};
		t.handler = [](const json& args) -> ToolResult {
			std::string q;
			bool callableOnly = false;
			int  limit        = 60;
			if (args.is_object())
			{
				if (const auto it = args.find("query"); it != args.end() && it->is_string())
					q = lowered(it->get<std::string>());
				if (const auto it = args.find("callableOnly");
				    it != args.end() && it->is_boolean())
					callableOnly = it->get<bool>();
				if (const auto it = args.find("limit");
				    it != args.end() && it->is_number_integer())
					limit = std::max(1, it->get<int>());
			}

			json rows      = json::array();
			int  matched   = 0;
			for (const HE::api::ApiFn& fn : HE::api::registry())
			{
				const std::string id    = fn.id ? fn.id : "";
				const std::string shown = fn.displayName ? fn.displayName : id;
				std::string reason;
				const bool callable = apiRowCallable(fn, &reason);
				if (callableOnly && !callable) continue;
				if (!q.empty() &&
				    lowered(id).find(q)    == std::string::npos &&
				    lowered(shown).find(q) == std::string::npos &&
				    lowered(fn.category ? fn.category : "").find(q) == std::string::npos)
					continue;
				++matched;
				if (static_cast<int>(rows.size()) >= limit) continue;

				json j{
					{ "id",          id },
					{ "displayName", shown },
					{ "category",    fn.category ? fn.category : "" },
					{ "isExec",      fn.isExec },
					{ "callable",    callable },
					{ "params",      paramList(fn.params) },
					{ "results",     paramList(fn.results) },
					{ "description", NodeDocs::engineCall(id) },
				};
				if (callable) j["tool"] = apiToolName(id);
				else
				{
					j["reason"] = reason;
					if (reason == "exec")
						j["note"] = "Changes the world, so it is not callable from here. "
						            "Use the editor's own tools (entity_create, "
						            "entity_set_transform, entity_set_components, hc_*) — "
						            "those are undoable and reach a collaboration session.";
					else if (reason == "denied_group")
						j["note"] = "Reads something outside the project (the clipboard, "
						            "the machine's PATH). Not exposed on this interface.";
				}
				rows.push_back(std::move(j));
			}
			return ToolResult::ok(json{
				{ "functions", std::move(rows) },
				{ "matched",   matched },
				{ "truncated", matched - static_cast<int>(rows.size()) },
			});
		};
		registry.add(std::move(t));
	}

	// ── One tool per pure row ────────────────────────────────────────────────
	// No hand-written list: what the engine registers, a client can call. The
	// `add` result is ignored deliberately — a name that collides or breaks the
	// spelling rule is refused by the registry and caught by the test that walks
	// this loop, rather than crashing an editor somebody is working in.
	for (const HE::api::ApiFn& fn : HE::api::registry())
	{
		if (!apiRowCallable(fn)) continue;
		registry.add(apiRowTool(fn, hooks));
	}
}

} // namespace HE::Ed
