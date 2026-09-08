#include "McpToolRegistry.h"

#include "CollabDocSync.h"

#include <HorizonCode/HorizonCode.h>
#include <HorizonScene/EngineApi.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// ─── Authoring HorizonCode from outside the editor ───────────────────────────
// Thirteen tools over a visual script: read it, add and remove nodes, wire and
// unwire pins, set parameters, save.
//
// The rule of this file is the one stated on `registerHcTools` in the header:
// nothing below writes into a `HorizonCode::Graph` by hand. Every node and every
// variable travels as the item-level JSON HorizonCode itself writes, and is
// applied through the `CollabDocSync` adapter — the same door a collaboration
// peer's edit uses. Two things fall out of that and neither is incidental:
//
//   • What MCP can express is exactly what the collaboration wire can express,
//     so the two cannot drift. A field added to `nodeToJson` is carried here
//     the day it is added, with no change in this file.
//   • In a live session the panel's own DocMirror diff sees the change in the
//     next frame and publishes it. This file contains the word "collab" only in
//     an include and in comments.
//
// The second bullet holds where `publishDocDeltas` already runs: the level
// script and the GameInstance graph are diffed every frame we hold their lock,
// and a class asset is diffed once its tab reports unsaved edits, which is what
// the `endEdit` hook marks. It does NOT hold for a class whose tab is CLOSED —
// nothing walks it — which is why `documents` only ever offers the tabs the
// editor holds.
//
// The other case it does not hold for is a PEER holding the document's lock: an
// edit made then lands locally, is never published, and is thrown away by the
// peer's next whole-file update. That was a deliberate compromise while a human
// was the only caller — they can see the read-only banner. A remote client
// cannot, so `McpHcHooks::lockedByOther` is now asked BEFORE every mutation and
// the answer is a `locked_by_other` refusal rather than a silent local write.
// The check sits in `openDoc`, which is the one door all ten mutating tools go
// through, and it sits there rather than in the handlers so that `beginEdit`
// never runs for a refused call — for the level script that hook is
// `m_undo.snapshotNow()`, and a refusal that dirtied the scene would be its own
// small bug.
//
// Wiring is the deliberate exception. `IDocAdapter::upsert(Kind::Link, …)` only
// checks that both endpoints exist — a peer's link was validated on the peer —
// so `hc_connect` goes through `Graph::connect` (direction, types, occupancy)
// and falls back to `connectWithConversion`, which is the pair of steps the
// canvas takes when a human drags a wire.
//
// Free of ImGui and of EditorApplication, so the interesting questions — does a
// connect by pin LABEL land on the pin the client meant, does removing a node
// take its links — are answerable against a graph on the stack.

namespace HE::Ed
{

using nlohmann::json;
namespace HC = HorizonCode;

namespace
{

// ── Refusal vocabulary ───────────────────────────────────────────────────────
// Deliberately the gateway's words where the situation is the gateway's
// (`not_found`, `play_mode`, `invalid_payload`, `failed`), so a client that has
// already learned the entity tools does not have to learn a second dialect for
// the same three answers. Two are new because the situation is new.
constexpr const char* kNotFound   = "not_found";
constexpr const char* kPlayMode   = "play_mode";
constexpr const char* kBadPayload = "invalid_payload";
constexpr const char* kFailed     = "failed";
constexpr const char* kLockedByOther = "locked_by_other";   // EditorCommands' word
constexpr const char* kRefused    = "refused_by_policy";   // the frontend hides it
constexpr const char* kSaveFailed = "save_failed";

// ── Pin types by name ────────────────────────────────────────────────────────
// Complete, unlike HcGraphHost::pinTypeName, which folds everything it does not
// draw into "Exec". That is fine for a colour swatch and wrong for a wire
// protocol: a client told a Vec3 pin is an Exec pin cannot connect anything to
// it. "Object" for Ref is kept — that IS the word the editor puts on the pin,
// and a name a human reads in the UI is the name a model should send back.
const char* pinTypeName(HC::PinType t)
{
	switch (t)
	{
		case HC::PinType::Exec:      return "Exec";
		case HC::PinType::Float:     return "Float";
		case HC::PinType::Bool:      return "Bool";
		case HC::PinType::Int:       return "Int";
		case HC::PinType::String:    return "String";
		case HC::PinType::Vec2:      return "Vec2";
		case HC::PinType::Color:     return "Color";
		case HC::PinType::Ref:       return "Object";
		case HC::PinType::Transform: return "Transform";
		case HC::PinType::Enum:      return "Enum";
		case HC::PinType::Struct:    return "Struct";
		case HC::PinType::Vec3:      return "Vec3";
		case HC::PinType::Vec4:      return "Vec4";
	}
	return "Exec";
}

bool pinTypeFromName(const std::string& name, HC::PinType& out)
{
	for (int i = 0; i <= static_cast<int>(HC::PinType::Vec4); ++i)
	{
		const auto t = static_cast<HC::PinType>(i);
		if (name == pinTypeName(t)) { out = t; return true; }
	}
	return false;
}

const char* containerName(HC::ContainerKind k)
{
	switch (k)
	{
		case HC::ContainerKind::Array: return "array";
		case HC::ContainerKind::Set:   return "set";
		case HC::ContainerKind::Map:   return "map";
		case HC::ContainerKind::None:  break;
	}
	return "none";
}

// ── The unified pin layout ───────────────────────────────────────────────────
// [execIns][execOuts][dataIns][dataOuts] — the indices `Graph::connect` and
// `Link` speak. Computed from `HC::signatureOf` here rather than borrowed from
// `HcGraphHost::pinRanges`, which computes it from the same call one layer up in
// a translation unit full of ImGui. Same switch, same answers; the parity test
// checks the two against their common source rather than against each other.
struct Ranges { int execIn0, execOut0, dataIn0, dataOut0, end; };

Ranges rangesOf(const HC::NodeSig& s)
{
	Ranges r;
	r.execIn0  = 0;
	r.execOut0 = r.execIn0  + static_cast<int>(s.execIns.size());
	r.dataIn0  = r.execOut0 + static_cast<int>(s.execOuts.size());
	r.dataOut0 = r.dataIn0  + static_cast<int>(s.dataIns.size());
	r.end      = r.dataOut0 + static_cast<int>(s.dataOuts.size());
	return r;
}

json pinJson(const HC::PinDesc& pd, int index, bool input, bool isExec)
{
	const HC::ContainerKind ck = pd.kind();
	json p{
		{ "index",     index },
		{ "label",     pd.name ? pd.name : "" },
		{ "type",      pinTypeName(pd.type) },
		{ "direction", input ? "in" : "out" },
		{ "exec",      isExec },
	};
	if (ck != HC::ContainerKind::None)
	{
		p["container"] = containerName(ck);
		// A Map pin's `type` is its VALUE type — without the key beside it
		// Map<String,Int> and Map<Int,Int> read as the same pin.
		if (ck == HC::ContainerKind::Map) p["keyType"] = pinTypeName(pd.keyType);
	}
	// The Enum/Struct definition asset behind a user-typed pin. Absent for every
	// built-in type, which is why it is added rather than always present.
	if (pd.typeName && pd.typeName[0]) p["typeName"] = pd.typeName;
	return p;
}

// Every pin of one node, in unified index order — the order the indices ARE.
json pinsJson(const HC::Node& n)
{
	const HC::NodeSig s = HC::signatureOf(n);
	json out   = json::array();
	int  index = 0;
	for (const auto& pd : s.execIns)  out.push_back(pinJson(pd, index++, true,  true));
	for (const auto& pd : s.execOuts) out.push_back(pinJson(pd, index++, false, true));
	for (const auto& pd : s.dataIns)  out.push_back(pinJson(pd, index++, true,  false));
	for (const auto& pd : s.dataOuts) out.push_back(pinJson(pd, index++, false, false));
	return out;
}

// A node as the wire carries it: its own item JSON (the exact object the saved
// file holds) with the pin list added. The pins are not stored anywhere — they
// are derived from the node's fields — but without them no client can call
// `hc_connect`, because a pin index has no other source.
json nodeJson(const HC::Node& n, bool withPins)
{
	json j = json::parse(HC::nodeToJson(n), nullptr, /*allow_exceptions=*/false);
	if (!j.is_object()) j = json{ { "id", n.id } };
	if (withPins) j["pins"] = pinsJson(n);
	return j;
}

std::string lowered(std::string v)
{
	std::transform(v.begin(), v.end(), v.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return v;
}

// ── Addressing a pin ─────────────────────────────────────────────────────────
// A client may send an index (which it can only have got from `hc_get`) or a
// label (which is what it reads in the documentation and in a screenshot).
// Labels are resolved on the SIDE the operation needs — a node with an "Exec"
// in and an "Exec" out is otherwise ambiguous, and picking the wrong one wires
// two outputs together and reports success.
bool resolvePin(const HC::Node& n, bool wantOutput, const json& spec,
                int& outIndex, std::string& err)
{
	const HC::NodeSig s = HC::signatureOf(n);
	const Ranges      r = rangesOf(s);

	if (spec.is_number_integer())
	{
		const int i = spec.get<int>();
		if (i < 0 || i >= r.end)
		{
			err = "pin index " + std::to_string(i) + " is out of range for node " +
			      std::to_string(n.id) + " (it has " + std::to_string(r.end) + " pins).";
			return false;
		}
		const bool isInput = (i < r.execOut0) || (i >= r.dataIn0 && i < r.dataOut0);
		if (isInput == wantOutput)
		{
			err = "pin " + std::to_string(i) + " of node " + std::to_string(n.id) +
			      " is an " + (isInput ? "input" : "output") + "; this end needs an " +
			      (wantOutput ? "output" : "input") + ".";
			return false;
		}
		outIndex = i;
		return true;
	}

	if (!spec.is_string())
	{
		err = "a pin is either its unified index (integer, from hc_get) or its "
		      "label (string).";
		return false;
	}

	const std::string want = spec.get<std::string>();
	// The empty string is not an address. Most exec pins and a few data pins
	// carry no label at all (`Print` has three of them), so matching "" would
	// silently pick whichever came first.
	if (want.empty())
	{
		err = "an empty pin label addresses nothing. Many pins carry no label — "
		      "the canvas draws them as a bare arrow — and those are addressed by "
		      "their index, which hc_get reports for every pin.";
		return false;
	}

	auto scan = [&](const std::vector<HC::PinDesc>& pins, int base) -> int {
		for (std::size_t i = 0; i < pins.size(); ++i)
			if (pins[i].name && want == pins[i].name) return base + static_cast<int>(i);
		return -1;
	};
	// Exec first, then data: an exec pin's label is the flow's name ("True",
	// "Then 0") and never collides with a value's, so the order only decides
	// which of two impossible collisions wins.
	int found = wantOutput ? scan(s.execOuts, r.execOut0) : scan(s.execIns, r.execIn0);
	if (found < 0)
		found = wantOutput ? scan(s.dataOuts, r.dataOut0) : scan(s.dataIns, r.dataIn0);

	// "exec" as a convenience for the pin that has no name to ask for. Exec
	// pins are unlabelled unless a node has more than one of them on a side
	// (Branch's True/False, Sequence's Then 0/Then 1), so "the exec pin" is an
	// unambiguous address exactly when there is one — and is refused, with the
	// alternatives named, when there is not. Without it the single commonest
	// wiring in the language (chain one node to the next) would need an index a
	// client can only get from a second call.
	if (found < 0 && lowered(want) == "exec")
	{
		const std::vector<HC::PinDesc>& execs = wantOutput ? s.execOuts : s.execIns;
		const int base = wantOutput ? r.execOut0 : r.execIn0;
		if (execs.size() == 1) { outIndex = base; return true; }
		if (execs.empty())
		{
			err = "node " + std::to_string(n.id) + " has no exec " +
			      (wantOutput ? "output" : "input") + " at all.";
			return false;
		}
		std::string names;
		for (std::size_t i = 0; i < execs.size(); ++i)
		{
			if (!names.empty()) names += ", ";
			names += (execs[i].name && execs[i].name[0])
			             ? std::string("'") + execs[i].name + "'"
			             : std::to_string(base + static_cast<int>(i));
		}
		err = "node " + std::to_string(n.id) + " has more than one exec " +
		      (wantOutput ? "output" : "input") + " — say which: " + names + ".";
		return false;
	}

	if (found < 0)
	{
		err = "node " + std::to_string(n.id) + " has no " +
		      (wantOutput ? "output" : "input") + " pin labelled '" + want +
		      "'. hc_get lists every pin with its index and label; an unlabelled "
		      "pin is addressed by that index, and 'exec' names the exec pin of a "
		      "node that has only one on this side.";
		return false;
	}
	outIndex = found;
	return true;
}

// ── Node types by their stored name ──────────────────────────────────────────
// `nodeDisplayName` is the key on disk (see the boxed warning at its
// definition), so it is also the name on this wire — a second spelling would be
// a second thing to migrate.
// "?" is `nodeDisplayName`'s fallback for a type it has no name for. It is not
// an address — several types could answer to it — so it is refused here and left
// out of the catalogue, rather than resolving to whichever one comes first.
bool nodeTypeByName(const std::string& name, HC::NodeType& out)
{
	if (name.empty() || name == "?") return false;
	for (const HC::NodeType t : HC::nodeRegistry())
		if (name == HC::nodeDisplayName(t)) { out = t; return true; }
	return false;
}

bool typeExcluded(const McpHcDoc& doc, HC::NodeType t)
{
	const char* n = HC::nodeDisplayName(t);
	return std::find(doc.excludedNodeTypes.begin(), doc.excludedNodeTypes.end(),
	                 std::string(n)) != doc.excludedNodeTypes.end();
}

std::vector<HC::NodeType> excludedTypes(const McpHcDoc& doc)
{
	std::vector<HC::NodeType> out;
	for (const std::string& name : doc.excludedNodeTypes)
	{
		HC::NodeType t;
		if (nodeTypeByName(name, t)) out.push_back(t);
	}
	return out;
}

// `HE::api::groupAllowed` wants a vector of raw pointers; the document carries
// strings because it crosses an interface. Borrowed, never outliving the call.
bool apiAllowed(const McpHcDoc& doc, const char* apiId)
{
	std::vector<const char*> groups;
	groups.reserve(doc.apiGroups.size());
	for (const std::string& g : doc.apiGroups) groups.push_back(g.c_str());
	return HE::api::groupAllowed(apiId, groups);
}

// ── Small readers ────────────────────────────────────────────────────────────

std::string strArg(const json& args, const char* key)
{
	if (!args.is_object()) return {};
	const auto it = args.find(key);
	return (it != args.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

const json* memberOf(const json& args, const char* key)
{
	if (!args.is_object()) return nullptr;
	const auto it = args.find(key);
	return it == args.end() ? nullptr : &(*it);
}

// ── Schemas ──────────────────────────────────────────────────────────────────

json objectSchema(json properties, std::vector<std::string> required)
{
	json s{
		{ "type",       "object" },
		{ "properties", std::move(properties) },
		{ "additionalProperties", false },
	};
	if (!required.empty()) s["required"] = required;
	return s;
}

json keyProp()
{
	return json{
		{ "type", "string" },
		{ "description", "Which document, from hc_documents: 'levelscript', "
		                 "'gameinstance', or a class asset's content-relative path." },
	};
}

json nodeIdProp(const char* what)
{
	return json{ { "type", "integer" }, { "description", what } };
}

json pinProp(const char* what)
{
	return json{
		{ "description", what },
		{ "oneOf", json::array({ json{ { "type", "integer" } },
		                         json{ { "type", "string" } } }) },
	};
}

// ── Resolving a document ─────────────────────────────────────────────────────
struct Doc
{
	McpHcDoc    desc;
	HC::Graph*  g  = nullptr;
	bool        ok = false;
	ToolResult  failure = ToolResult::ok(json::object());
};

Doc openDoc(const McpHcHooks& h, const json& args, bool mutating)
{
	Doc d;
	if (mutating && h.isPlaying && h.isPlaying())
	{
		d.failure = ToolResult::fail(
			kPlayMode,
			"Play-in-editor is running. A graph edited now would be handed to a "
			"session that is about to be thrown away, so it is refused rather than "
			"lost. Ask the user to stop play mode.");
		return d;
	}

	const std::string key = strArg(args, "key");
	if (key.empty())
	{
		d.failure = ToolResult::fail(
			kBadPayload, "'key' is required. hc_documents lists the documents this "
			             "editor can address right now.");
		return d;
	}

	// The descriptor comes from the list rather than being invented here: it
	// carries the frontend's restrictions, and a document that is not in the
	// list is one the editor is not offering.
	if (h.documents)
		for (McpHcDoc& doc : h.documents())
			if (doc.key == key) { d.desc = std::move(doc); break; }

	d.g = h.resolve ? h.resolve(key) : nullptr;
	if (!d.g || d.desc.key.empty())
	{
		d.failure = ToolResult::fail(
			kNotFound, "No HorizonCode document '" + key + "'. Call hc_documents — a "
			           "class asset the editor has never opened is not addressable, "
			           "and the human has to open its tab first.");
		return d;
	}

	// After the resolve, so an unknown key is still `not_found` rather than a
	// lock answer about a document that does not exist. Before anything else,
	// so no `beginEdit` and no partial write happens for a refused call.
	if (mutating && h.lockedByOther && h.lockedByOther(key))
	{
		d.failure = ToolResult::fail(
			kLockedByOther,
			"Another peer in the collaboration session holds '" + key + "'. Writing "
			"anyway would change only this copy: the edit is never published while "
			"someone else owns the document, and their next update overwrites it. "
			"Reading is unaffected — hc_get still works. Ask the user to have the "
			"other peer close or release it, then retry.");
		return d;
	}

	d.ok = true;
	return d;
}

// ── Applying an item ─────────────────────────────────────────────────────────
// Everything a tool changes goes through here, so there is exactly one place
// that knows the payload format and one place that runs `afterApply` (which is
// `syncFunctionSignatures`: a FunctionEntry whose interface moved leaves every
// FunctionCall of it with the old pin list until it runs).
bool upsertNode(HC::Graph& g, const HC::Node& n)
{
	const auto ad = CollabDocSync::forHorizonCodeGraph(g);
	if (!ad->upsert(CollabDocSync::Kind::Node, n.id, HC::nodeToJson(n))) return false;
	ad->afterApply();
	return true;
}

bool upsertVariable(HC::Graph& g, const HC::Variable& v)
{
	const auto ad = CollabDocSync::forHorizonCodeGraph(g);
	// The id is the adapter's own hash of the name; it is not exported, and the
	// adapter re-derives it from the payload anyway. 0 is only ever seen by the
	// switch that ignores it for variables.
	if (!ad->upsert(CollabDocSync::Kind::Variable, 0, HC::variableToJson(v))) return false;
	ad->afterApply();
	return true;
}

// The adapter keys a variable by a hash of its name that CollabDocSync keeps to
// itself. Rather than reimplementing the hash — which would be a second copy of
// an identity, the exact bug the shared payload format exists to avoid — the id
// is read back out of the adapter's own enumeration.
bool variableIdOf(const HC::Graph& g, const std::string& name, std::int64_t& out)
{
	const auto ad = CollabDocSync::forHorizonCodeGraph(const_cast<HC::Graph&>(g));
	bool found = false;
	ad->enumerate(CollabDocSync::Kind::Variable,
	              [&](std::int64_t id, std::string itemJson) {
		if (found) return;
		const json j = json::parse(itemJson, nullptr, false);
		if (j.is_object() && j.value("name", std::string()) == name)
		{ out = id; found = true; }
	});
	return found;
}

json linkJson(const HC::Link& l)
{
	return json{
		{ "srcNode", l.srcNode }, { "srcPin", l.srcPin },
		{ "dstNode", l.dstNode }, { "dstPin", l.dstPin },
	};
}

// Links that were in `before` and are not in `g` any more. `Graph::connect`
// replaces an occupied exec-out or data-in silently, which is right on a canvas
// (the old wire visibly disappears) and a lie to a caller that cannot see it.
json droppedLinks(const std::vector<HC::Link>& before, const HC::Graph& g)
{
	json out = json::array();
	for (const HC::Link& b : before)
	{
		const bool still = std::any_of(g.links.begin(), g.links.end(), [&b](const HC::Link& l) {
			return l.srcNode == b.srcNode && l.srcPin == b.srcPin &&
			       l.dstNode == b.dstNode && l.dstPin == b.dstPin;
		});
		if (!still) out.push_back(linkJson(b));
	}
	return out;
}

} // namespace

// ─── The tools ───────────────────────────────────────────────────────────────

void registerHcTools(McpToolRegistry& registry, McpHcHooks hooks)
{
	// Shared by every handler. Copied into each lambda, which is what keeps the
	// registry the only owner — the hooks close over the editor, and the editor
	// outlives the registry it built.
	auto h = std::make_shared<McpHcHooks>(std::move(hooks));

	// ── hc_documents ─────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "hc_documents";
		t.description =
			"The HorizonCode (visual scripting) documents this editor can author "
			"right now: the level script, the project's GameInstance graph and every "
			"HorizonCode class asset whose tab the editor holds. Every other hc_ tool "
			"takes the 'key' returned here. A class asset that has never been opened "
			"is deliberately absent — the class editor owns that state, and a copy "
			"loaded behind its back would be overwritten by the human's next Save.";
		t.inputSchema = objectSchema(json::object(), {});
		t.handler = [h](const json&) -> ToolResult {
			json out = json::array();
			if (h->documents)
				for (const McpHcDoc& d : h->documents())
					out.push_back(json{
						{ "key",   d.key },
						{ "label", d.label },
						{ "kind",  d.kind },
						{ "dirty", d.dirty },
						// Reported rather than hidden: a client that is refused an
						// EngineCall node otherwise has no way to learn that this
						// frontend restricts them at all.
						{ "apiGroups",         d.apiGroups },
						{ "excludedNodeTypes", d.excludedNodeTypes },
					});
			return ToolResult::ok(json{ { "documents", std::move(out) } });
		};
		registry.add(std::move(t));
	}

	// ── hc_get ───────────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "hc_get";
		t.description =
			"Read one HorizonCode document: its nodes (each in the exact JSON the "
			"saved file holds, plus the list of its pins), its links, its variables "
			"and its declared events. The pin list is the only source of the pin "
			"indices hc_connect takes — it is derived from each node's fields, not "
			"stored — so read this before wiring anything.";
		t.inputSchema = objectSchema(json{
			{ "key",      keyProp() },
			{ "subgraph", json{ { "type", "integer" },
			                    { "description", "Only nodes of this sub-graph: 0 is "
			                                     "the main event graph, otherwise the "
			                                     "id of the owning Function Entry." } } },
			{ "nodeIds",  json{ { "type", "array" }, { "items", json{ { "type", "integer" } } },
			                    { "description", "Only these nodes (with their pins). "
			                                     "Links and variables are unaffected." } } },
			{ "includePins", json{ { "type", "boolean" },
			                       { "description", "Default true. Set false for a "
			                                        "cheap structural overview of a "
			                                        "large graph." } } },
			{ "limit",    json{ { "type", "integer" }, { "minimum", 1 },
			                    { "description", "At most this many nodes (default 200)." } } },
		}, { "key" });
		t.handler = [h](const json& args) -> ToolResult {
			const Doc d = openDoc(*h, args, /*mutating=*/false);
			if (!d.ok) return d.failure;

			int  subgraph    = -1;   // -1 = every sub-graph
			bool withPins    = true;
			int  limit       = 200;
			std::vector<int> only;
			if (args.is_object())
			{
				if (const json* s = memberOf(args, "subgraph"); s && s->is_number_integer())
					subgraph = s->get<int>();
				if (const json* p = memberOf(args, "includePins"); p && p->is_boolean())
					withPins = p->get<bool>();
				if (const json* l = memberOf(args, "limit"); l && l->is_number_integer())
					limit = std::max(1, l->get<int>());
				if (const json* n = memberOf(args, "nodeIds"); n && n->is_array())
					for (const json& e : *n)
						if (e.is_number_integer()) only.push_back(e.get<int>());
			}

			json nodes     = json::array();
			int  truncated = 0;
			for (const HC::Node& n : d.g->nodes)
			{
				if (subgraph >= 0 && n.subgraph != subgraph) continue;
				if (!only.empty() &&
				    std::find(only.begin(), only.end(), n.id) == only.end()) continue;
				if (static_cast<int>(nodes.size()) >= limit) { ++truncated; continue; }
				nodes.push_back(nodeJson(n, withPins));
			}

			json links = json::array();
			for (const HC::Link& l : d.g->links) links.push_back(linkJson(l));

			json vars = json::array();
			for (const HC::Variable& v : d.g->variables)
			{
				json j = json::parse(HC::variableToJson(v), nullptr, false);
				if (j.is_object()) vars.push_back(std::move(j));
			}

			json events = json::array();
			for (const HC::EventDecl& e : d.g->events)
			{
				json j{ { "name", e.name }, { "hasArg", e.hasArg } };
				if (e.hasArg) j["argType"] = pinTypeName(e.argType);
				if (!e.typeName.empty()) j["typeName"] = e.typeName;
				events.push_back(std::move(j));
			}

			return ToolResult::ok(json{
				{ "key",       d.desc.key },
				{ "label",     d.desc.label },
				{ "kind",      d.desc.kind },
				{ "dirty",     d.desc.dirty },
				{ "nextId",    d.g->nextId },
				{ "nodes",     std::move(nodes) },
				{ "links",     std::move(links) },
				{ "variables", std::move(vars) },
				{ "events",    std::move(events) },
				// Named rather than silently cut, for the reason entity_list gives:
				// a client that gets exactly `limit` nodes cannot otherwise tell a
				// whole graph from a clipped one.
				{ "truncated", truncated },
			});
		};
		registry.add(std::move(t));
	}

	// ── hc_node_types ────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "hc_node_types";
		t.description =
			"The catalogue hc_add_node draws from: every node type, and — as a "
			"second list — every engine-API row an 'Engine Call' node can be bound "
			"to. Both are already filtered by what the given document's own add menu "
			"offers, so anything listed here can actually be inserted there.";
		t.inputSchema = objectSchema(json{
			{ "key",   keyProp() },
			{ "query", json{ { "type", "string" },
			                 { "description", "Case-insensitive substring of the name, "
			                                  "category or (for engine rows) the id." } } },
			{ "limit", json{ { "type", "integer" }, { "minimum", 1 },
			                 { "description", "At most this many engine-API rows "
			                                  "(default 80). Node types are never cut — "
			                                  "the list is short and closed." } } },
		}, { "key" });
		t.handler = [h](const json& args) -> ToolResult {
			const Doc d = openDoc(*h, args, /*mutating=*/false);
			if (!d.ok) return d.failure;

			const std::string q = lowered(strArg(args, "query"));
			int limit = 80;
			if (const json* l = memberOf(args, "limit"); l && l->is_number_integer())
				limit = std::max(1, l->get<int>());

			json types = json::array();
			for (const HC::NodeType nt : HC::nodeRegistry())
			{
				if (typeExcluded(d.desc, nt)) continue;
				const char* name     = HC::nodeDisplayName(nt);
				if (std::strcmp(name, "?") == 0) continue;   // see nodeTypeByName
				const char* category = HC::nodeCategory(nt);
				const char* aliases  = HC::nodeSearchAliases(nt);
				if (!q.empty() &&
				    lowered(name).find(q)     == std::string::npos &&
				    lowered(category).find(q) == std::string::npos &&
				    lowered(aliases).find(q)  == std::string::npos) continue;
				json j{
					{ "type",     name },
					{ "category", category },
					{ "tooltip",  HC::nodeTooltip(nt) },
				};
				if (aliases && aliases[0]) j["aliases"] = aliases;
				types.push_back(std::move(j));
			}

			json api       = json::array();
			int  truncated = 0;
			for (const HE::api::ApiFn& fn : HE::api::registry())
			{
				if (!apiAllowed(d.desc, fn.id)) continue;
				const char* shown = fn.displayName ? fn.displayName : fn.id;
				if (!q.empty() &&
				    lowered(fn.id).find(q)       == std::string::npos &&
				    lowered(shown).find(q)       == std::string::npos &&
				    lowered(fn.category).find(q) == std::string::npos) continue;
				if (static_cast<int>(api.size()) >= limit) { ++truncated; continue; }

				auto paramList = [](const std::vector<HE::api::ApiParam>& ps) {
					json a = json::array();
					for (const auto& p : ps)
						a.push_back(json{ { "name", p.name ? p.name : "" },
						                  { "type", pinTypeName(p.type) },
						                  { "isArray", p.isArray } });
					return a;
				};
				api.push_back(json{
					{ "id",          fn.id },
					{ "displayName", shown },
					{ "category",    fn.category ? fn.category : "" },
					// An exec row runs when the chain reaches it; a pure one is
					// evaluated whenever an output of it is read. It also decides
					// the node's `hasArg`, which hc_add_node sets for the client.
					{ "isExec",      fn.isExec },
					{ "params",      paramList(fn.params) },
					{ "results",     paramList(fn.results) },
				});
			}

			return ToolResult::ok(json{
				{ "nodeTypes",    std::move(types) },
				{ "engineApi",    std::move(api) },
				{ "apiTruncated", truncated },
			});
		};
		registry.add(std::move(t));
	}

	// ── hc_add_node ──────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "hc_add_node";
		t.mutates     = true;
		t.description =
			"Add one node to a document. 'type' is a name from hc_node_types. For an "
			"'Engine Call' node, 's' is the engine-API id (e.g. 'transform.setPosition') "
			"and its pins are mirrored from the registry for you. For an 'Event' or a "
			"'Function Call' node, 's' is the event or function name. The answer "
			"carries the new node's id and its full pin list, so the next call can "
			"wire it without a second hc_get.";
		t.inputSchema = objectSchema(json{
			{ "key",  keyProp() },
			{ "type", json{ { "type", "string" },
			                { "description", "Node type name, exactly as hc_node_types "
			                                 "spells it (it is also the key on disk)." } } },
			{ "s",    json{ { "type", "string" },
			                { "description", "The node's name payload: the engine-API id "
			                                 "for Engine Call, the event name for Event, "
			                                 "the function name for Function Call, the "
			                                 "variable name for Get/Set Variable, the "
			                                 "literal for a String constant." } } },
			{ "position", json{ { "type", "array" }, { "items", json{ { "type", "number" } } },
			                    { "minItems", 2 }, { "maxItems", 2 },
			                    { "description", "Canvas position [x, y]. Cosmetic, but a "
			                                     "graph where everything sits at the "
			                                     "origin is unreadable for the human." } } },
			{ "subgraph", json{ { "type", "integer" },
			                    { "description", "0 (default) = the main event graph, "
			                                     "otherwise the id of the Function Entry "
			                                     "whose body this node belongs to." } } },
			{ "propType", json{ { "type", "string" },
			                    { "description", "Value type for the node types that "
			                                     "carry one (Get/Set Variable, the array "
			                                     "operations, Event's argument): Float, "
			                                     "Bool, Int, String, Vec2, Vec3, Vec4, "
			                                     "Color, Object, Transform, Enum, Struct." } } },
		}, { "key", "type" });
		t.handler = [h](const json& args) -> ToolResult {
			Doc d = openDoc(*h, args, /*mutating=*/true);
			if (!d.ok) return d.failure;

			const std::string typeName = strArg(args, "type");
			HC::NodeType nt;
			if (!nodeTypeByName(typeName, nt))
				return ToolResult::fail(kBadPayload,
					"'" + typeName + "' is not a node type. hc_node_types lists every "
					"name this document accepts, spelled exactly as it has to be sent.");
			if (typeExcluded(d.desc, nt))
				return ToolResult::fail(kRefused,
					"'" + typeName + "' is not offered by this document's add menu, so "
					"it is not inserted here either — the restriction would otherwise "
					"be cosmetic.");

			HC::Node n;
			n.type = nt;
			n.s    = strArg(args, "s");
			if (const json* p = memberOf(args, "position");
			    p && p->is_array() && p->size() == 2 &&
			    (*p)[0].is_number() && (*p)[1].is_number())
			{ n.x = (*p)[0].get<float>(); n.y = (*p)[1].get<float>(); }
			if (const json* s = memberOf(args, "subgraph"); s && s->is_number_integer())
				n.subgraph = s->get<int>();
			if (const std::string pt = strArg(args, "propType"); !pt.empty())
			{
				if (!pinTypeFromName(pt, n.propType))
					return ToolResult::fail(kBadPayload,
						"'" + pt + "' is not a pin type. Use one of Float, Bool, Int, "
						"String, Vec2, Vec3, Vec4, Color, Object, Transform, Enum, Struct.");
			}

			// Engine Call: the pins come from the registry, not from this file.
			// `params`/`results` are mirrored onto the node so `signatureOf`
			// resolves without the registry (which lives a layer up), exactly as
			// the add menu and the node-reference page do it.
			if (nt == HC::NodeType::EngineCall)
			{
				const HE::api::ApiFn* fn = HE::api::find(n.s);
				if (!fn)
					return ToolResult::fail(kBadPayload,
						"'" + n.s + "' is not an engine-API id. hc_node_types returns the "
						"full list under 'engineApi'; an Engine Call node without a valid "
						"id has no pins and does nothing.");
				if (!apiAllowed(d.desc, fn->id))
					return ToolResult::fail(kRefused,
						"This document's add menu does not offer the engine group of '" +
						n.s + "'. hc_documents reports which groups it allows.");
				n.hasArg = fn->isExec;
				for (const auto& p : fn->params)  n.params.push_back({ p.name, p.type, p.isArray });
				for (const auto& r : fn->results) n.results.push_back({ r.name, r.type, r.isArray });
			}

			// The id is minted here rather than by `Graph::addNode`, because the
			// adapter's payload IS the node and a payload without an id has no
			// identity to upsert against.
			n.id = d.g->nextId;

			if (h->beginEdit) h->beginEdit(d.desc.key);
			if (!upsertNode(*d.g, n))
				return ToolResult::fail(kFailed,
					"The document refused the node. This is the adapter's own check — "
					"an unusable payload — and it should not be reachable from here.");
			if (h->endEdit) h->endEdit(d.desc.key);

			const HC::Node* added = d.g->findNode(n.id);
			return ToolResult::ok(json{
				{ "node", added ? nodeJson(*added, true) : json::object() },
			});
		};
		registry.add(std::move(t));
	}

	// ── hc_set_node ──────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "hc_set_node";
		t.mutates     = true;
		t.description =
			"Replace one node with the object given in 'node' — the same shape hc_get "
			"returns and the saved file holds. Read the node, change the fields you "
			"mean, send the whole object back. A node id that does not exist is "
			"created. Links whose pin no longer exists after the change are dropped "
			"and reported, because a link into a pin that is gone stalls the "
			"interpreter's chain walk.";
		t.inputSchema = objectSchema(json{
			{ "key",  keyProp() },
			{ "node", json{ { "type", "object" },
			                { "description", "A node object from hc_get, including its "
			                                 "'id' and 'type'. The 'pins' member hc_get "
			                                 "adds is derived, not stored, and ignored "
			                                 "here." } } },
		}, { "key", "node" });
		t.handler = [h](const json& args) -> ToolResult {
			Doc d = openDoc(*h, args, /*mutating=*/true);
			if (!d.ok) return d.failure;

			const json* nodeArg = memberOf(args, "node");
			if (!nodeArg || !nodeArg->is_object())
				return ToolResult::fail(kBadPayload, "'node' must be a node object.");
			json payload = *nodeArg;
			payload.erase("pins");   // derived, and nodeFromJson would ignore it anyway
			if (!payload.contains("id") || !payload["id"].is_number_integer())
				return ToolResult::fail(kBadPayload,
					"'node.id' is required and must be the integer id hc_get reported.");
			const int id = payload["id"].get<int>();

			HC::Node parsed;
			if (!HC::nodeFromJson(payload.dump(), parsed))
				return ToolResult::fail(kBadPayload,
					"The node object could not be read. The usual cause is a 'type' "
					"that is not one of the names hc_node_types spells.");
			if (typeExcluded(d.desc, parsed.type))
				return ToolResult::fail(kRefused,
					"This document's add menu does not offer '" +
					std::string(HC::nodeDisplayName(parsed.type)) + "'.");

			if (h->beginEdit) h->beginEdit(d.desc.key);
			const auto ad = CollabDocSync::forHorizonCodeGraph(*d.g);
			if (!ad->upsert(CollabDocSync::Kind::Node, id, payload.dump()))
				return ToolResult::fail(kBadPayload,
					"The document refused the node payload.");
			ad->afterApply();

			// A replaced node can have fewer pins than the one it replaced — a
			// Sequence turned into a Branch, an Engine Call re-bound to a shorter
			// row. Nothing in the collaboration path prunes those links, because a
			// peer sends the node and its link deltas together; a client sends one
			// object, so the pruning happens here.
			json dropped = json::array();
			if (const HC::Node* n = d.g->findNode(id))
			{
				const Ranges r = rangesOf(HC::signatureOf(*n));
				std::vector<HC::Link> kept;
				for (const HC::Link& l : d.g->links)
				{
					const bool bad = (l.srcNode == id && l.srcPin >= r.end) ||
					                 (l.dstNode == id && l.dstPin >= r.end);
					if (bad) dropped.push_back(linkJson(l));
					else     kept.push_back(l);
				}
				d.g->links = std::move(kept);
			}
			if (h->endEdit) h->endEdit(d.desc.key);

			const HC::Node* now = d.g->findNode(id);
			return ToolResult::ok(json{
				{ "node",         now ? nodeJson(*now, true) : json::object() },
				{ "droppedLinks", std::move(dropped) },
			});
		};
		registry.add(std::move(t));
	}

	// ── hc_remove_node ───────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "hc_remove_node";
		t.mutates     = true;
		t.description =
			"Delete one node. Its links go with it — every wire that touched it is "
			"removed too, and the answer says which, so a client does not have to "
			"re-read the graph to learn what its delete broke.";
		t.inputSchema = objectSchema(json{
			{ "key", keyProp() },
			{ "id",  nodeIdProp("Node id, from hc_get or hc_add_node.") },
		}, { "key", "id" });
		t.handler = [h](const json& args) -> ToolResult {
			Doc d = openDoc(*h, args, /*mutating=*/true);
			if (!d.ok) return d.failure;

			const json* idArg = memberOf(args, "id");
			if (!idArg || !idArg->is_number_integer())
				return ToolResult::fail(kBadPayload, "'id' is required and is an integer.");
			const int id = idArg->get<int>();
			if (!d.g->findNode(id))
				return ToolResult::fail(kNotFound,
					"No node " + std::to_string(id) + " in this document.");

			const std::vector<HC::Link> before = d.g->links;
			if (h->beginEdit) h->beginEdit(d.desc.key);
			const auto ad = CollabDocSync::forHorizonCodeGraph(*d.g);
			if (!ad->remove(CollabDocSync::Kind::Node, id))
				return ToolResult::fail(kFailed, "The document refused the delete.");
			ad->afterApply();
			if (h->endEdit) h->endEdit(d.desc.key);

			return ToolResult::ok(json{
				{ "removed",      id },
				{ "removedLinks", droppedLinks(before, *d.g) },
			});
		};
		registry.add(std::move(t));
	}

	// ── hc_connect ───────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "hc_connect";
		t.mutates     = true;
		t.description =
			"Wire one node's output pin to another node's input pin. A pin is named "
			"by its unified index (from hc_get), by its label, or by the word 'exec' "
			"when the node has exactly one exec pin on that side (most nodes do — an "
			"exec pin otherwise carries no label at all). A label is resolved on the "
			"side that end needs, so 'True' on the source means the exec OUT. Types must match; when they do not, the same conversion node "
			"the canvas would insert is inserted here and reported. An input that was "
			"already wired is replaced, and the wire that went away is reported too.";
		t.inputSchema = objectSchema(json{
			{ "key",     keyProp() },
			{ "srcNode", nodeIdProp("The node the wire leaves.") },
			{ "srcPin",  pinProp("An OUTPUT pin of srcNode: its unified index, its label, "
			                     "or 'exec' when the node has a single exec output "
			                     "(most do; exec pins otherwise carry no label).") },
			{ "dstNode", nodeIdProp("The node the wire enters.") },
			{ "dstPin",  pinProp("An INPUT pin of dstNode: its unified index, its label, "
			                     "or 'exec' when the node has a single exec input.") },
			{ "allowConversion", json{ { "type", "boolean" },
			                           { "description", "Default true. False refuses a "
			                                            "type mismatch instead of "
			                                            "spawning a conversion node." } } },
		}, { "key", "srcNode", "srcPin", "dstNode", "dstPin" });
		t.handler = [h](const json& args) -> ToolResult {
			Doc d = openDoc(*h, args, /*mutating=*/true);
			if (!d.ok) return d.failure;

			const json* srcId = memberOf(args, "srcNode");
			const json* dstId = memberOf(args, "dstNode");
			const json* srcP  = memberOf(args, "srcPin");
			const json* dstP  = memberOf(args, "dstPin");
			if (!srcId || !srcId->is_number_integer() || !dstId || !dstId->is_number_integer())
				return ToolResult::fail(kBadPayload,
					"'srcNode' and 'dstNode' are required and are node ids.");
			if (!srcP || !dstP)
				return ToolResult::fail(kBadPayload,
					"'srcPin' and 'dstPin' are required (unified index or pin label).");

			const HC::Node* src = d.g->findNode(srcId->get<int>());
			const HC::Node* dst = d.g->findNode(dstId->get<int>());
			if (!src) return ToolResult::fail(kNotFound,
				"No node " + std::to_string(srcId->get<int>()) + " in this document.");
			if (!dst) return ToolResult::fail(kNotFound,
				"No node " + std::to_string(dstId->get<int>()) + " in this document.");

			int sp = 0, dp = 0;
			std::string err;
			if (!resolvePin(*src, /*wantOutput=*/true,  *srcP, sp, err))
				return ToolResult::fail(kBadPayload, err);
			if (!resolvePin(*dst, /*wantOutput=*/false, *dstP, dp, err))
				return ToolResult::fail(kBadPayload, err);

			bool conversion = true;
			if (const json* c = memberOf(args, "allowConversion"); c && c->is_boolean())
				conversion = c->get<bool>();

			const int sn = src->id, dn = dst->id;
			const std::vector<HC::Link> before = d.g->links;
			// Node ids before the attempt, so a conversion node spawned by the
			// fallback can be named in the answer rather than left for the client
			// to discover in the next hc_get.
			std::vector<int> idsBefore;
			idsBefore.reserve(d.g->nodes.size());
			for (const HC::Node& n : d.g->nodes) idsBefore.push_back(n.id);

			if (h->beginEdit) h->beginEdit(d.desc.key);
			bool ok = d.g->connect(sn, sp, dn, dp);
			bool viaConversion = false;
			if (!ok && conversion)
			{
				// Same fallback and the same exclusion list the canvas uses: a node
				// type this frontend hides must not appear through this door either.
				ok = HC::connectWithConversion(*d.g, sn, sp, dn, dp, excludedTypes(d.desc));
				viaConversion = ok;
			}
			if (!ok)
			{
				// Nothing was applied — `connect` validates before it writes, and
				// `connectWithConversion` removes its own half-built node on
				// failure. So there is nothing to undo and nothing to publish.
				return ToolResult::fail(kFailed,
					"Those two pins cannot be connected. Either the directions are "
					"wrong (output to input), or the types do not match and no "
					"conversion node exists for the pair, or both ends are the same "
					"node. hc_get lists each pin's type.");
			}

			json spawned = json::array();
			for (const HC::Node& n : d.g->nodes)
				if (std::find(idsBefore.begin(), idsBefore.end(), n.id) == idsBefore.end())
					spawned.push_back(nodeJson(n, true));

			const auto ad = CollabDocSync::forHorizonCodeGraph(*d.g);
			ad->afterApply();
			if (h->endEdit) h->endEdit(d.desc.key);

			return ToolResult::ok(json{
				{ "connected",     true },
				{ "viaConversion", viaConversion },
				{ "spawnedNodes",  std::move(spawned) },
				// The wire an occupied input or exec-output gave up. Silent on the
				// canvas because you watch it disappear; not silent here.
				{ "replacedLinks", droppedLinks(before, *d.g) },
				{ "link", json{ { "srcNode", sn }, { "srcPin", sp },
				                { "dstNode", dn }, { "dstPin", dp } } },
			});
		};
		registry.add(std::move(t));
	}

	// ── hc_disconnect ────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "hc_disconnect";
		t.mutates     = true;
		t.description =
			"Remove one wire, named by both of its endpoints exactly as hc_connect "
			"names them. A link has no id of its own — it IS its four endpoints — so "
			"both ends have to be given.";
		t.inputSchema = objectSchema(json{
			{ "key",     keyProp() },
			{ "srcNode", nodeIdProp("The node the wire leaves.") },
			{ "srcPin",  pinProp("An OUTPUT pin of srcNode: unified index, label, or 'exec'.") },
			{ "dstNode", nodeIdProp("The node the wire enters.") },
			{ "dstPin",  pinProp("An INPUT pin of dstNode: unified index, label, or 'exec'.") },
		}, { "key", "srcNode", "srcPin", "dstNode", "dstPin" });
		t.handler = [h](const json& args) -> ToolResult {
			Doc d = openDoc(*h, args, /*mutating=*/true);
			if (!d.ok) return d.failure;

			const json* srcId = memberOf(args, "srcNode");
			const json* dstId = memberOf(args, "dstNode");
			const json* srcP  = memberOf(args, "srcPin");
			const json* dstP  = memberOf(args, "dstPin");
			if (!srcId || !srcId->is_number_integer() || !dstId || !dstId->is_number_integer() ||
			    !srcP || !dstP)
				return ToolResult::fail(kBadPayload,
					"'srcNode', 'srcPin', 'dstNode' and 'dstPin' are all required.");

			const HC::Node* src = d.g->findNode(srcId->get<int>());
			const HC::Node* dst = d.g->findNode(dstId->get<int>());
			if (!src || !dst)
				return ToolResult::fail(kNotFound,
					"One of the two nodes is not in this document.");

			int sp = 0, dp = 0;
			std::string err;
			if (!resolvePin(*src, true,  *srcP, sp, err)) return ToolResult::fail(kBadPayload, err);
			if (!resolvePin(*dst, false, *dstP, dp, err)) return ToolResult::fail(kBadPayload, err);

			if (h->beginEdit) h->beginEdit(d.desc.key);
			const auto ad = CollabDocSync::forHorizonCodeGraph(*d.g);
			const std::int64_t id = CollabDocSync::linkKey(src->id, sp, dst->id, dp);
			if (!ad->remove(CollabDocSync::Kind::Link, id))
				return ToolResult::fail(kNotFound,
					"There is no wire between those two pins. hc_get lists the links "
					"this document actually has.");
			ad->afterApply();
			if (h->endEdit) h->endEdit(d.desc.key);

			return ToolResult::ok(json{
				{ "disconnected", true },
				{ "link", json{ { "srcNode", src->id }, { "srcPin", sp },
				                { "dstNode", dst->id }, { "dstPin", dp } } },
			});
		};
		registry.add(std::move(t));
	}

	// ── hc_set_pin_default ───────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "hc_set_pin_default";
		t.mutates     = true;
		t.description =
			"Give an UNWIRED simple data input a constant value, instead of spending "
			"a literal node on it — the same thing typing into the little field on a "
			"node's pin does. Bool, Int, Float and String only. Send value: null to "
			"clear it. A pin that already has a wire is refused rather than silently "
			"ignored: the wire wins, so writing a default there would look like it "
			"worked and change nothing.";
		t.inputSchema = objectSchema(json{
			{ "key",   keyProp() },
			{ "node",  nodeIdProp("Node id.") },
			{ "pin",   pinProp("A data INPUT pin: unified index or label. Many data pins "
			                   "carry no label either — hc_get gives the index for each.") },
			{ "value", json{ { "description",
			                   "The constant: a boolean, a number or a string, matching "
			                   "the pin's type. null clears the default." } } },
		}, { "key", "node", "pin", "value" });
		t.handler = [h](const json& args) -> ToolResult {
			Doc d = openDoc(*h, args, /*mutating=*/true);
			if (!d.ok) return d.failure;

			const json* nodeId = memberOf(args, "node");
			const json* pinArg = memberOf(args, "pin");
			if (!nodeId || !nodeId->is_number_integer() || !pinArg)
				return ToolResult::fail(kBadPayload, "'node' and 'pin' are required.");
			const HC::Node* found = d.g->findNode(nodeId->get<int>());
			if (!found)
				return ToolResult::fail(kNotFound,
					"No node " + std::to_string(nodeId->get<int>()) + " in this document.");

			int unified = 0;
			std::string err;
			if (!resolvePin(*found, /*wantOutput=*/false, *pinArg, unified, err))
				return ToolResult::fail(kBadPayload, err);

			const HC::NodeSig sig = HC::signatureOf(*found);
			const Ranges      r   = rangesOf(sig);
			if (unified < r.dataIn0 || unified >= r.dataOut0)
				return ToolResult::fail(kBadPayload,
					"That is an exec pin. A default is a value, and only a data input "
					"can carry one.");
			// The key of pinDefaults is the DATA-IN index, not the unified one —
			// it has to stay stable when a node's exec prefix changes.
			const int dataIndex = unified - r.dataIn0;

			for (const HC::Link& l : d.g->links)
				if (l.dstNode == found->id && l.dstPin == unified)
					return ToolResult::fail(kFailed,
						"That pin is wired, and a wired pin ignores its default. "
						"Disconnect it first with hc_disconnect if the constant is "
						"what you want.");

			HC::PinDesc desc{};
			if (!HC::dataPinDescOf(*found, /*input=*/true, dataIndex, desc))
				return ToolResult::fail(kBadPayload, "That data input does not exist.");
			if (desc.kind() != HC::ContainerKind::None)
				return ToolResult::fail(kBadPayload,
					"Container pins (array, set, map) take no inline default. Wire a "
					"Make node into it instead.");

			HC::Node edited = *found;
			const json* valArg = memberOf(args, "value");
			const bool  clear  = !valArg || valArg->is_null();
			if (clear)
			{
				if (edited.pinDefaults.erase(dataIndex) == 0)
					return ToolResult::ok(json{ { "cleared", true }, { "changed", false } });
			}
			else
			{
				HC::Value v;
				v.type = desc.type;
				switch (desc.type)
				{
					case HC::PinType::Bool:
						if (!valArg->is_boolean())
							return ToolResult::fail(kBadPayload, "That pin is a Bool.");
						v.b = valArg->get<bool>();
						break;
					case HC::PinType::Int:
						if (!valArg->is_number())
							return ToolResult::fail(kBadPayload, "That pin is an Int.");
						v.i = valArg->get<int>();
						break;
					case HC::PinType::Float:
						if (!valArg->is_number())
							return ToolResult::fail(kBadPayload, "That pin is a Float.");
						v.f = valArg->get<float>();
						break;
					case HC::PinType::String:
						if (!valArg->is_string())
							return ToolResult::fail(kBadPayload, "That pin is a String.");
						v.s = valArg->get<std::string>();
						break;
					default:
						return ToolResult::fail(kBadPayload,
							std::string("A ") + pinTypeName(desc.type) + " pin has no "
							"inline default — the editor gives one only to Bool, Int, "
							"Float and String. Wire a literal node into it instead.");
				}
				edited.pinDefaults[dataIndex] = std::move(v);
			}

			if (h->beginEdit) h->beginEdit(d.desc.key);
			if (!upsertNode(*d.g, edited))
				return ToolResult::fail(kFailed, "The document refused the node payload.");
			if (h->endEdit) h->endEdit(d.desc.key);

			const HC::Node* now = d.g->findNode(edited.id);
			return ToolResult::ok(json{
				{ "changed",   true },
				{ "cleared",   clear },
				{ "dataIndex", dataIndex },
				{ "node",      now ? nodeJson(*now, true) : json::object() },
			});
		};
		registry.add(std::move(t));
	}

	// ── hc_add_variable ──────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "hc_add_variable";
		t.mutates     = true;
		t.description =
			"Declare a graph variable — the per-instance state Get Variable and Set "
			"Variable nodes read and write. The name is its identity everywhere in "
			"the graph, so a name that already exists is refused rather than "
			"overwritten; use hc_set_variable to change one.";
		t.inputSchema = objectSchema(json{
			{ "key",  keyProp() },
			{ "name", json{ { "type", "string" }, { "description", "Unique in this graph." } } },
			{ "type", json{ { "type", "string" },
			                { "description", "Float, Bool, Int, String, Vec2, Vec3, Vec4, "
			                                 "Color, Object, Transform, Enum or Struct." } } },
			{ "container", json{ { "type", "string" }, { "enum", json::array({ "none", "array", "set", "map" }) },
			                     { "description", "Default 'none' (a scalar)." } } },
			{ "keyType",   json{ { "type", "string" },
			                     { "description", "For a map: the KEY's type (Int, String, "
			                                      "Enum or Object). 'type' is then the "
			                                      "value type." } } },
			{ "access",    json{ { "type", "integer" }, { "enum", json::array({ 0, 1 }) },
			                     { "description", "0 = public (readable through a "
			                                      "reference), 1 = private. Default 0." } } },
			{ "typeName",  json{ { "type", "string" },
			                     { "description", "For an Enum or Struct variable: the "
			                                      "definition asset's path." } } },
		}, { "key", "name", "type" });
		t.handler = [h](const json& args) -> ToolResult {
			Doc d = openDoc(*h, args, /*mutating=*/true);
			if (!d.ok) return d.failure;

			HC::Variable v;
			v.name = strArg(args, "name");
			if (v.name.empty())
				return ToolResult::fail(kBadPayload,
					"'name' is required — a variable is identified by its name, so a "
					"nameless one has no identity.");
			if (d.g->findVariable(v.name))
				return ToolResult::fail(kFailed,
					"This graph already declares a variable called '" + v.name +
					"'. Use hc_set_variable to change it.");
			if (!pinTypeFromName(strArg(args, "type"), v.type))
				return ToolResult::fail(kBadPayload,
					"'type' must be one of Float, Bool, Int, String, Vec2, Vec3, Vec4, "
					"Color, Object, Transform, Enum, Struct.");

			const std::string container = strArg(args, "container");
			if (!container.empty() && container != "none")
			{
				if      (container == "array") v.container = HC::ContainerKind::Array;
				else if (container == "set")   v.container = HC::ContainerKind::Set;
				else if (container == "map")   v.container = HC::ContainerKind::Map;
				else return ToolResult::fail(kBadPayload,
					"'container' is 'none', 'array', 'set' or 'map'.");
				// Both flags together, always: the legacy row is `isArray` alone,
				// and the inconsistent state is not representable after a load.
				v.isArray = true;
			}
			if (const std::string kt = strArg(args, "keyType"); !kt.empty())
			{
				if (!pinTypeFromName(kt, v.keyType))
					return ToolResult::fail(kBadPayload, "'keyType' is not a pin type.");
				if (!HC::isValidMapKeyType(v.keyType))
					return ToolResult::fail(kBadPayload,
						"A " + kt + " cannot key a map — only types with an exact "
						"identity can (Int, String, Enum, Object).");
			}
			if (const json* a = memberOf(args, "access"); a && a->is_number_integer())
				v.access = a->get<int>() ? 1 : 0;
			v.typeName = strArg(args, "typeName");

			if (h->beginEdit) h->beginEdit(d.desc.key);
			if (!upsertVariable(*d.g, v))
				return ToolResult::fail(kFailed, "The document refused the variable.");
			if (h->endEdit) h->endEdit(d.desc.key);

			return ToolResult::ok(json{
				{ "variable", json::parse(HC::variableToJson(v), nullptr, false) },
			});
		};
		registry.add(std::move(t));
	}

	// ── hc_set_variable ──────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "hc_set_variable";
		t.mutates     = true;
		t.description =
			"Replace one variable declaration with the object given in 'variable' — "
			"the same shape hc_get returns. Read it, change what you mean, send the "
			"whole object back. The 'name' inside it is the identity, and a name this "
			"graph does not declare is refused rather than created: renaming is not "
			"done this way, because the Get Variable and Set Variable nodes that "
			"reference the old name would go on referencing it.";
		t.inputSchema = objectSchema(json{
			{ "key",      keyProp() },
			{ "variable", json{ { "type", "object" },
			                    { "description", "A variable object from hc_get, "
			                                     "including its 'name'." } } },
		}, { "key", "variable" });
		t.handler = [h](const json& args) -> ToolResult {
			Doc d = openDoc(*h, args, /*mutating=*/true);
			if (!d.ok) return d.failure;

			const json* varArg = memberOf(args, "variable");
			if (!varArg || !varArg->is_object())
				return ToolResult::fail(kBadPayload, "'variable' must be an object.");
			HC::Variable parsed;
			if (!HC::variableFromJson(varArg->dump(), parsed))
				return ToolResult::fail(kBadPayload,
					"The variable object could not be read — the usual cause is a "
					"missing 'name'.");
			if (!d.g->findVariable(parsed.name))
				return ToolResult::fail(kNotFound,
					"This graph declares no variable '" + parsed.name +
					"'. Use hc_add_variable to create one.");

			if (h->beginEdit) h->beginEdit(d.desc.key);
			const auto ad = CollabDocSync::forHorizonCodeGraph(*d.g);
			if (!ad->upsert(CollabDocSync::Kind::Variable, 0, varArg->dump()))
				return ToolResult::fail(kBadPayload, "The document refused the payload.");
			ad->afterApply();
			if (h->endEdit) h->endEdit(d.desc.key);

			const HC::Variable* now = d.g->findVariable(parsed.name);
			return ToolResult::ok(json{
				{ "variable", now ? json::parse(HC::variableToJson(*now), nullptr, false)
				                  : json::object() },
			});
		};
		registry.add(std::move(t));
	}

	// ── hc_remove_variable ───────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "hc_remove_variable";
		t.mutates     = true;
		t.description =
			"Delete a variable declaration. The Get Variable and Set Variable nodes "
			"that named it are LEFT IN PLACE — they are not part of the declaration, "
			"and removing them would delete work the human can still see. The answer "
			"lists them so the client can decide.";
		t.inputSchema = objectSchema(json{
			{ "key",  keyProp() },
			{ "name", json{ { "type", "string" }, { "description", "The variable's name." } } },
		}, { "key", "name" });
		t.handler = [h](const json& args) -> ToolResult {
			Doc d = openDoc(*h, args, /*mutating=*/true);
			if (!d.ok) return d.failure;

			const std::string name = strArg(args, "name");
			if (name.empty())
				return ToolResult::fail(kBadPayload, "'name' is required.");
			if (!d.g->findVariable(name))
				return ToolResult::fail(kNotFound,
					"This graph declares no variable '" + name + "'.");

			std::int64_t id = 0;
			if (!variableIdOf(*d.g, name, id))
				return ToolResult::fail(kFailed,
					"The document could not address that variable.");

			json orphaned = json::array();
			for (const HC::Node& n : d.g->nodes)
				if ((n.type == HC::NodeType::GetVariable ||
				     n.type == HC::NodeType::SetVariable) && n.s == name)
					orphaned.push_back(n.id);

			if (h->beginEdit) h->beginEdit(d.desc.key);
			const auto ad = CollabDocSync::forHorizonCodeGraph(*d.g);
			if (!ad->remove(CollabDocSync::Kind::Variable, id))
				return ToolResult::fail(kFailed, "The document refused the delete.");
			ad->afterApply();
			if (h->endEdit) h->endEdit(d.desc.key);

			return ToolResult::ok(json{
				{ "removed",       name },
				{ "orphanedNodes", std::move(orphaned) },
			});
		};
		registry.add(std::move(t));
	}

	// ── hc_save ──────────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "hc_save";
		t.mutates     = true;
		t.description =
			"Write the document to disk — the level script into the scene, the "
			"GameInstance graph and a class asset into their own files. Edits made by "
			"the other hc_ tools live in the editor until this is called, exactly as a "
			"human's do; the editor shows them as unsaved in the meantime.";
		t.inputSchema = objectSchema(json{ { "key", keyProp() } }, { "key" });
		t.handler = [h](const json& args) -> ToolResult {
			const Doc d = openDoc(*h, args, /*mutating=*/true);
			if (!d.ok) return d.failure;
			if (!h->save)
				return ToolResult::fail(kSaveFailed,
					"This editor build cannot save a HorizonCode document over MCP.");
			if (!h->save(d.desc.key))
				return ToolResult::fail(kSaveFailed,
					"The editor could not write '" + d.desc.key + "'. The scene may "
					"have no path yet, or the file is not writable.");
			return ToolResult::ok(json{ { "saved", d.desc.key } });
		};
		registry.add(std::move(t));
	}
}

} // namespace HE::Ed
