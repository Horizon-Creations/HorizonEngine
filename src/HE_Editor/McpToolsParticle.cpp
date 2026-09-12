#include "McpToolRegistry.h"

#include "EditorAssetTypeCache.h"     // what a path holds, without loading it
#include "McpToolCommon.h"            // the argument readers, the confinement rule, the walk

#include <ContentManager/AssetRefScan.h>
#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/HAsset.h>
#include <ParticleGraph/ParticleGraph.h>

#include <algorithm>
#include <memory>
#include <random>
#include <string>
#include <vector>

// ─── Authoring an emitter from outside the editor ────────────────────────────
// Why a particle graph needs tools of its own, why a PIN is the address and
// where the boundary between "a value" and "the graph" runs: McpToolRegistry.h,
// beside McpParticleHooks. What is worth stating HERE is what the handlers
// promise.
//
//   • A PIN IS NAMED THE WAY THE EDITOR NAMES IT. "Emit Rate", "Start Color" —
//     the strings in the node registry, which are the labels of the Emitter
//     Output node's own slots. An unknown name is refused WITH the list, because
//     a client cannot see the registry and a near-miss would otherwise write
//     nothing and report success.
//
//   • A VALUE IS WRITTEN IN THE SHAPE ITS PIN HAS. A number for a float pin,
//     [r, g, b] for a Vec3 one, `true`/`false` for the three pins the evaluator
//     reads as a flag. A wrong shape is refused with the expected one rather
//     than coerced.
//
//   • THE SMALLEST EDIT THAT MAKES THE VALUE TRUE. An unconnected pin gets a new
//     Const node, wired; a Const node that feeds only this pin is changed in
//     place; anything else is refused by name. `reset` puts a pin back on its
//     registry default and takes the Const node with it when nothing else reads
//     it.
//
//   • THE ANSWER CARRIES THE EVALUATED EMITTER. Setting a pin is a graph edit,
//     but what the client wanted is a particle behaviour, so every result
//     reports the config `evaluateParticleGraph` produces — the same POD the
//     simulation consumes.
//
//   • A REFUSAL IS A NO-OP. Nothing is written, no live emitter is invalidated
//     and no tab is told to re-read anything.

namespace HE::Ed
{

using nlohmann::json;

namespace
{

// ── The pin vocabulary ───────────────────────────────────────────────────────
// The Emitter Output's inputs, in registry order. The INDEX is on-disk format
// (ParticleGraph.h), which is exactly why nothing here hardcodes one: the names
// come out of the registry, and the index is wherever the name was found.
const std::vector<ParticlePinDesc>& emitterPins()
{
	return particleNodeDesc(ParticleNodeType::EmitterOutput).inputs;
}

int pinIndexByName(const std::string& name)
{
	const std::vector<ParticlePinDesc>& pins = emitterPins();
	for (std::size_t i = 0; i < pins.size(); ++i)
		if (name == pins[i].name) return static_cast<int>(i);
	return -1;
}

std::string allPinNames()
{
	std::string s;
	for (const ParticlePinDesc& p : emitterPins())
		s += (s.empty() ? "" : ", ") + std::string(p.name);
	return s;
}

// The three pins the evaluator reads as "> 0.5", i.e. the ones an author thinks
// of as checkboxes. Reported as booleans and settable with `true`/`false`, so a
// client never has to know they are floats on disk.
bool isFlagPin(int index)
{
	return index == kParticleLoopingPin || index == kParticleCollisionEnabledPin ||
	       index == kParticleKillOnCollisionPin;
}

// Which Const node an author would put on this pin. The two colour pins get a
// Const Color, which is the same node with a colour picker in the panel — a
// Const Vec3 there would be an emitter whose colour cannot be picked.
ParticleNodeType constNodeFor(int pinIndex)
{
	if (pinIndex == kParticleStartColorPin || pinIndex == kParticleEndColorPin)
		return ParticleNodeType::ConstColor;
	return emitterPins()[static_cast<std::size_t>(pinIndex)].type == ParticlePinType::Vec3
	           ? ParticleNodeType::ConstVec3
	           : ParticleNodeType::ConstFloat;
}

bool isConstNode(ParticleNodeType t)
{
	return t == ParticleNodeType::ConstFloat || t == ParticleNodeType::ConstVec3 ||
	       t == ParticleNodeType::ConstColor;
}

// ── The addressed emitter ────────────────────────────────────────────────────
// The path check, the play-mode gate, the lock gate and the unsaved-tab gate,
// once — the same four questions the type tools ask, in the same order.
struct Emitter
{
	std::string   rel;
	std::string   abs;
	ParticleGraph graph;
	bool          ok      = false;
	ToolResult    failure = ToolResult::ok(json::object());
};

// The graph JSON out of the FILE. Nothing is loaded to answer a question — the
// same rule the input, material and type readers follow.
bool readPayload(const std::string& abs, std::string& out)
{
	HAsset::Reader r;
	if (!r.open(abs)) return false;
	if (const HAsset::Reader::Chunk* c = r.findChunk(HAsset::CHUNK_PTGR))
		out.assign(reinterpret_cast<const char*>(c->data.data()), c->data.size());
	// An absent chunk is not a failure: a freshly created ParticleSystem asset
	// has no graph chunk at all (AssetStubWriter writes none), and what the panel
	// shows for one is makeDefault() — so that is what this reads as.
	return true;
}

Emitter openEmitter(ContentManager& content, const McpParticleHooks& h, const json& args,
                    bool forWrite)
{
	Emitter e;
	const PathCheck p = checkPath(content, strArg(args, "path"), /*mustExist=*/true, "path");
	if (!p.ok) { e.failure = p.failure; return e; }
	e.rel = p.rel;
	e.abs = p.abs;

	if (EditorAssetTypeCache::assetTypeOf(p.abs) != HE::AssetType::ParticleSystem)
	{
		e.failure = ToolResult::fail("invalid_path",
			"'" + p.rel + "' is not a Particle System asset. asset_resolve reports what a "
			"path holds, particle_info without arguments lists every emitter in the "
			"project, and asset_create with type 'ParticleSystem' makes a new one.");
		return e;
	}

	std::string payload;
	if (!readPayload(p.abs, payload))
	{
		e.failure = ToolResult::fail("failed",
			"'" + p.rel + "' could not be read. The editor log carries the reason.");
		return e;
	}
	// Exactly what ParticleGraphEditorPanel::stateFor does, in the same order: the
	// default graph, replaced only when the file holds one that parses.
	e.graph = ParticleGraph::makeDefault();
	if (!payload.empty())
	{
		ParticleGraph parsed;
		if (particleGraphFromJson(payload, parsed)) e.graph = std::move(parsed);
	}

	if (forWrite)
	{
		if (p.engine) { e.failure = failEngineReadOnly(p.rel); return e; }
		if (h.isPlaying && h.isPlaying())
		{
			e.failure = ToolResult::fail("play_mode",
				"Play-in-editor is running, and the running session's emitters have "
				"already resolved this graph. A change now would be half in effect, so it "
				"is refused rather than half-applied. Ask the user to stop play mode.");
			return e;
		}
		if (h.lockedByOther && h.lockedByOther(p.rel))
		{
			e.failure = ToolResult::fail("locked_by_other",
				"Another participant in the collaboration session holds '" + p.rel +
				"' right now. Wait until they let go, or work on something else.");
			return e;
		}
		if (h.isDirty && h.isDirty(p.rel))
		{
			e.failure = ToolResult::fail("dirty",
				"'" + p.rel + "' is open in the Particle Graph Editor with unsaved "
				"changes. That tab's own copy is the truth while it is dirty: writing the "
				"file would be reverted by the human's next Save, and there is no way to "
				"land an edit in the tab that they could take back. Ask the user to save "
				"or close that tab, then call again.");
			return e;
		}
	}
	e.ok = true;
	return e;
}

// ── Writing one back ─────────────────────────────────────────────────────────
// Everything ParticleGraphEditorPanel::saveToDisk does: the file, then the live
// emitters. The load is the ONLY one in this path and nothing is loaded after
// it, so the pointer taken here cannot be moved out from under us by a second
// asset registering (ContentManager.h: the pool is a dense vector).
ToolResult writeEmitter(ContentManager& content, const McpParticleHooks& h, Emitter& e, json out)
{
	const std::string payload = particleGraphToJson(e.graph);
	const HE::UUID id = content.loadAsset(e.rel);
	bool wrote = false;
	if (!(id == HE::UUID{}))
	{
		if (ParticleGraphAsset* a = content.getParticleGraphMutable(id))
		{
			a->nodeGraphJson = payload;
			wrote = content.saveAsset(*a);
		}
	}
	if (!wrote)
		return ToolResult::fail("failed",
			"Could not write '" + e.rel + "'. A read-only file or a full disk is the "
			"usual cause; the editor log carries the reason.");

	// Live emitters, or the edit only shows up the next time an entity's own
	// particleAssetId changes — the panel's Save does exactly this.
	if (h.onGraphChanged) h.onGraphChanged(e.rel);

	out["path"] = e.rel;
	out["reloadedInEditor"] = h.reloadFromDisk ? h.reloadFromDisk(e.rel) : false;
	return ToolResult::ok(std::move(out));
}

// ── Reporting ────────────────────────────────────────────────────────────────

json uuidJson(const HE::UUID& id)
{
	return json::array({ static_cast<std::uint64_t>(id.hi), static_cast<std::uint64_t>(id.lo) });
}

const ParticleGraphNode* outputNode(const ParticleGraph& g)
{
	for (const ParticleGraphNode& n : g.nodes)
		if (n.type == ParticleNodeType::EmitterOutput) return &n;
	return nullptr;
}

// The link feeding one input pin, or nullptr. An input pin holds at most one
// (ParticleGraph::connect disconnects first), so this answers completely.
const ParticleGraphLink* linkInto(const ParticleGraph& g, int dstNode, int dstPin)
{
	for (const ParticleGraphLink& l : g.links)
		if (l.dstNode == dstNode && l.dstPin == dstPin) return &l;
	return nullptr;
}

int consumersOf(const ParticleGraph& g, int srcNode)
{
	int n = 0;
	for (const ParticleGraphLink& l : g.links)
		if (l.srcNode == srcNode) ++n;
	return n;
}

// One pin, as a client reads it: what drives it and what it is worth right now.
// A pin nobody drives reports its registry default, which is the value the
// evaluator uses — "unconnected" and "no value" are not the same thing here.
json pinJson(const ParticleGraph& g, const ParticleGraphNode& out, int index)
{
	const ParticlePinDesc& desc = emitterPins()[static_cast<std::size_t>(index)];
	json j{
		{ "pin",  desc.name },
		{ "type", isFlagPin(index) ? "flag"
		                           : (desc.type == ParticlePinType::Vec3 ? "vec3" : "float") },
	};
	const ParticleGraphLink* l = linkInto(g, out.id, index);
	const ParticleGraphNode* src = l ? g.findNode(l->srcNode) : nullptr;
	if (src)
	{
		j["drivenBy"] = json{
			{ "node", src->id },
			{ "type", particleNodeDesc(src->type).name },
		};
		if (isConstNode(src->type))
		{
			if (desc.type == ParticlePinType::Vec3)
				j["value"] = json::array({ src->p[0], src->p[1], src->p[2] });
			else if (isFlagPin(index)) j["value"] = src->p[0] > 0.5f;
			else                       j["value"] = src->p[0];
			// What `particle_set` will do with this pin, said outright rather than
			// left for the client to work out from the shape of the graph.
			j["settable"] = consumersOf(g, src->id) == 1;
		}
		else
		{
			// A Random Range or a piece of math: there is no single value to report,
			// and particle_set will refuse it.
			j["settable"] = false;
		}
	}
	else
	{
		j["drivenBy"] = "default";
		if (desc.type == ParticlePinType::Vec3)
			j["value"] = json::array({ desc.def, desc.def, desc.def });
		else if (isFlagPin(index)) j["value"] = desc.def > 0.5f;
		else                       j["value"] = desc.def;
		j["settable"] = true;
	}
	return j;
}

// The emitter the simulation would actually get. RandomRange resolves once per
// evaluate (ParticleGraph.h's KNOWN LIMITATION), so a graph holding one answers
// differently on two calls — said in the result rather than left as a surprise.
json configJson(const ParticleGraph& g, bool& randomised)
{
	randomised = false;
	for (const ParticleGraphNode& n : g.nodes)
		if (n.type == ParticleNodeType::RandomRange) randomised = true;

	std::mt19937 rng{ 1337 };
	const ParticleEmitterConfig c = evaluateParticleGraph(g, rng);
	return json{
		{ "emitRate",        c.emitRate },
		{ "lifetimeMin",     c.lifetimeMin },
		{ "lifetimeMax",     c.lifetimeMax },
		{ "startSize",       c.startSize },
		{ "endSize",         c.endSize },
		{ "startColor",      json::array({ c.startColor[0], c.startColor[1], c.startColor[2] }) },
		{ "endColor",        json::array({ c.endColor[0], c.endColor[1], c.endColor[2] }) },
		{ "startAlpha",      c.startAlpha },
		{ "endAlpha",        c.endAlpha },
		{ "initialVelocity", json::array({ c.initialVelocity[0], c.initialVelocity[1],
		                                   c.initialVelocity[2] }) },
		{ "velocitySpread",  c.velocitySpread },
		{ "gravity",         json::array({ c.gravity[0], c.gravity[1], c.gravity[2] }) },
		{ "maxParticles",    c.maxParticles },
		{ "looping",         c.looping },
		{ "collisionEnabled", c.collisionEnabled },
		{ "restitution",     c.restitution },
		{ "killOnCollision", c.killOnCollision },
	};
}

// ── Reading a value out of a client's JSON ───────────────────────────────────
// The refusal names the shape that was expected, because the client cannot see
// the registry. `false` means the argument was there and wrong.
bool readValue(const json& v, int pinIndex, float* p, std::string& want)
{
	const ParticlePinDesc& desc = emitterPins()[static_cast<std::size_t>(pinIndex)];
	if (isFlagPin(pinIndex))
	{
		// A number is taken too: the pin IS a float on disk, and refusing 1 where
		// true works would be a refusal nobody can debug from the schema.
		if (v.is_boolean()) { p[0] = v.get<bool>() ? 1.0f : 0.0f; return true; }
		if (v.is_number())  { p[0] = v.get<float>(); return true; }
		want = "true or false";
		return false;
	}
	if (desc.type == ParticlePinType::Vec3)
	{
		if (!v.is_array() || v.size() != 3)
		{
			want = "an array of three numbers, [x, y, z]";
			return false;
		}
		for (int i = 0; i < 3; ++i)
		{
			if (!v[i].is_number()) { want = "an array of three numbers, [x, y, z]"; return false; }
			p[i] = v[i].get<float>();
		}
		return true;
	}
	if (!v.is_number()) { want = "a number"; return false; }
	p[0] = v.get<float>();
	return true;
}

// ── particle_info ────────────────────────────────────────────────────────────

void addInfo(McpToolRegistry& registry, ContentManager& content,
             const std::shared_ptr<McpParticleHooks>& h)
{
	ContentManager* cm = &content;
	McpTool t;
	t.name        = "particle_info";
	t.description =
		"Without arguments: every Particle System asset in the project. With 'path': "
		"that emitter in full — each of the Emitter Output's inputs with what drives it "
		"and what it is worth, the mesh and material it draws with, the graph's nodes "
		"and links, and the emitter configuration the simulation would actually get.";
	t.inputSchema = objectSchema(json{
		{ "path",  stringProp("Content-relative path of a Particle System asset, e.g. "
		                      "'Effects/Smoke.hasset'. Omit for the catalogue.") },
		{ "limit", numberProp("Maximum number of assets in the catalogue (default 200).") },
	}, {});
	t.handler = [cm, h](const json& args) -> ToolResult {
		if (!hasArg(args, "path"))
		{
			bool truncated = false;
			const std::vector<ContentAsset> found = walkContentAssets(
				*cm, { HE::AssetType::ParticleSystem }, intArg(args, "limit", 200), truncated);
			json list = json::array();
			for (const ContentAsset& a : found)
			{
				std::string payload;
				ParticleGraph g = ParticleGraph::makeDefault();
				if (readPayload(a.abs, payload) && !payload.empty())
				{
					ParticleGraph parsed;
					if (particleGraphFromJson(payload, parsed)) g = std::move(parsed);
				}
				list.push_back(json{
					{ "path",      a.rel },
					{ "nodeCount", static_cast<int>(g.nodes.size()) },
					{ "linkCount", static_cast<int>(g.links.size()) },
				});
			}
			json out{ { "emitters", std::move(list) } };
			if (truncated) out["truncated"] = true;
			return ToolResult::ok(std::move(out));
		}

		Emitter e = openEmitter(*cm, *h, args, /*forWrite=*/false);
		if (!e.ok) return e.failure;

		const ParticleGraphNode* out = outputNode(e.graph);
		json pins = json::array();
		if (out)
			for (int i = 0; i < static_cast<int>(emitterPins().size()); ++i)
				pins.push_back(pinJson(e.graph, *out, i));

		json nodes = json::array();
		for (const ParticleGraphNode& n : e.graph.nodes)
		{
			json jn{
				{ "id",   n.id },
				{ "type", particleNodeDesc(n.type).name },
			};
			const int params = particleNodeDesc(n.type).paramCount;
			if (params > 0)
			{
				json p = json::array();
				for (int i = 0; i < params; ++i) p.push_back(n.p[i]);
				jn["params"] = std::move(p);
			}
			nodes.push_back(std::move(jn));
		}
		json links = json::array();
		for (const ParticleGraphLink& l : e.graph.links)
			links.push_back(json{ { "srcNode", l.srcNode }, { "srcPin", l.srcPin },
			                      { "dstNode", l.dstNode }, { "dstPin", l.dstPin } });

		bool randomised = false;
		json cfg = configJson(e.graph, randomised);

		json result{
			{ "path",     e.rel },
			{ "pins",     std::move(pins) },
			{ "nodes",    std::move(nodes) },
			{ "links",    std::move(links) },
			{ "emitter",  std::move(cfg) },
		};
		if (out)
		{
			result["mesh"]     = uuidJson(out->meshAssetId);
			result["material"] = uuidJson(out->materialAssetId);
		}
		else
		{
			// A graph without its fixed sink: a hand-edit or a file from elsewhere.
			// Said outright, because every value above is then the registry default
			// and nothing else would explain why setting a pin is impossible.
			result["note"] =
				"This graph has no Emitter Output node, which every particle graph the "
				"editor writes has. The simulation falls back to the default emitter; "
				"particle_set has no pin to write to until the graph is repaired in the "
				"Particle Graph Editor.";
		}
		if (randomised)
			result["note"] =
				"A Random Range node resolves ONCE per evaluation, so the 'emitter' block "
				"above is one roll of it and the running game rolls its own.";
		return ToolResult::ok(std::move(result));
	};
	registry.add(std::move(t));
}

// ── particle_set ─────────────────────────────────────────────────────────────

void addSet(McpToolRegistry& registry, ContentManager& content,
            const std::shared_ptr<McpParticleHooks>& h)
{
	ContentManager* cm = &content;
	McpTool t;
	t.name        = "particle_set";
	t.description =
		"Set one input of the emitter, addressed by the name the editor shows for it "
		"('Emit Rate', 'Start Color'). An input nothing drives gets a Const node wired "
		"to it; one already driven by a Const node that feeds nothing else is changed in "
		"place. An input driven by a Random Range or by math is refused rather than "
		"quietly detached — particle_info says which is which ('settable'). 'reset' puts "
		"an input back on its default.";
	t.inputSchema = objectSchema(json{
		{ "path",  stringProp("Content-relative path of the Particle System asset.") },
		{ "pin",   stringProp("Which input: one of Emit Rate, Lifetime Min, Lifetime Max, "
		                      "Start Size, End Size, Start Color, End Color, Start Alpha, "
		                      "End Alpha, Initial Velocity, Velocity Spread, Gravity, Max "
		                      "Particles, Looping, Collision Enabled, Restitution, Kill On "
		                      "Collision.") },
		{ "value", json{ { "description",
		                   "The value, in the shape the input has: a number, [x, y, z] for "
		                   "a colour/vector input, true or false for Looping, Collision "
		                   "Enabled and Kill On Collision." } } },
		{ "reset", json{ { "type", "boolean" },
		                 { "description",
		                   "Put this input back on its default instead of setting it. The "
		                   "Const node that drove it is removed when nothing else reads "
		                   "it." } } },
	}, { "path", "pin" });
	t.mutates = true;
	t.handler = [cm, h](const json& args) -> ToolResult {
		Emitter e = openEmitter(*cm, *h, args, /*forWrite=*/true);
		if (!e.ok) return e.failure;

		const std::string pinName = strArg(args, "pin");
		const int index = pinIndexByName(pinName);
		if (index < 0)
			return ToolResult::fail("invalid_payload",
				"'" + pinName + "' is not an input of the Emitter Output. The inputs are: " +
				allPinNames() + ". (Mesh and Material are not inputs but slots on the node "
				"itself — particle_slot_set writes those.)");

		const bool reset = boolArg(args, "reset");
		if (!reset && !hasArg(args, "value"))
			return ToolResult::fail("invalid_payload",
				"Either 'value' or 'reset' is required — a call with neither would report "
				"success without changing anything.");

		ParticleGraphNode* out = nullptr;
		for (ParticleGraphNode& n : e.graph.nodes)
			if (n.type == ParticleNodeType::EmitterOutput) { out = &n; break; }
		if (!out)
			return ToolResult::fail("invalid_payload",
				"'" + e.rel + "' has no Emitter Output node, which every particle graph the "
				"editor writes has — there is no input to write to. Open it in the Particle "
				"Graph Editor, or create a new asset with asset_create.");
		const int outId = out->id;

		const ParticleGraphLink* existing = linkInto(e.graph, outId, index);
		const ParticleGraphNode* driver = existing ? e.graph.findNode(existing->srcNode) : nullptr;

		if (reset)
		{
			json result{ { "pin", pinName }, { "reset", true } };
			if (!driver)
			{
				result["changed"] = false;
				result["note"]    = "Nothing drove this input; it was already on its default.";
			}
			else
			{
				const int driverId = driver->id;
				const bool onlyConsumer = consumersOf(e.graph, driverId) == 1;
				const bool wasConst     = isConstNode(driver->type);
				e.graph.disconnectInput(outId, index);
				// The node goes only when it was a plain constant that nothing else
				// reads: removing an author's Random Range because a pin was reset
				// would be an edit nobody asked for.
				if (wasConst && onlyConsumer) e.graph.removeNode(driverId);
				result["changed"]     = true;
				result["removedNode"] = wasConst && onlyConsumer;
			}
			ParticleGraphNode* after = nullptr;
			for (ParticleGraphNode& n : e.graph.nodes)
				if (n.type == ParticleNodeType::EmitterOutput) { after = &n; break; }
			if (after) result["state"] = pinJson(e.graph, *after, index);
			bool randomised = false;
			result["emitter"] = configJson(e.graph, randomised);
			return writeEmitter(*cm, *h, e, std::move(result));
		}

		float p[4] = { 0, 0, 0, 0 };
		std::string want;
		if (!readValue(args["value"], index, p, want))
			return ToolResult::fail("invalid_payload",
				"'value' for the input '" + pinName + "' has to be " + want + ".");

		bool created = false;
		int  nodeId  = 0;
		if (driver)
		{
			if (!isConstNode(driver->type))
				return ToolResult::fail("driven_by_graph",
					"'" + pinName + "' is driven by the " +
					std::string(particleNodeDesc(driver->type).name) + " node " +
					std::to_string(driver->id) + ", not by a constant. Setting a value here "
					"would mean detaching that node, which is a change to the graph rather "
					"than to a value — these tools do not make it. Use 'reset' to take the "
					"input back to its default first if that is what you want, or edit the "
					"graph in the Particle Graph Editor.");
			if (consumersOf(e.graph, driver->id) != 1)
				return ToolResult::fail("driven_by_graph",
					"'" + pinName + "' is driven by node " + std::to_string(driver->id) +
					", which also feeds " + std::to_string(consumersOf(e.graph, driver->id) - 1) +
					" other input(s). Changing its value would change those too, so it is "
					"refused rather than applied wider than asked. Use 'reset' on this input "
					"first to give it a constant of its own.");
			nodeId = driver->id;
		}
		else
		{
			// Placed to the LEFT of the sink and spread down by pin, or the panel
			// opens on a pile of nodes stacked in one spot.
			const float x = out->x - 260.0f;
			const float y = out->y + static_cast<float>(index) * 46.0f;
			nodeId  = e.graph.addNode(constNodeFor(index), x, y);
			created = true;
			if (!e.graph.connect(nodeId, 0, outId, index))
			{
				e.graph.removeNode(nodeId);
				return ToolResult::fail("failed",
					"The new constant could not be wired to '" + pinName + "'. Nothing was "
					"written.");
			}
		}

		ParticleGraphNode* node = e.graph.findNode(nodeId);
		if (!node) return ToolResult::fail("failed", "The node to write disappeared. Nothing was written.");
		const int params = particleNodeDesc(node->type).paramCount;
		for (int i = 0; i < params; ++i) node->p[i] = p[i];

		ParticleGraphNode* after = nullptr;
		for (ParticleGraphNode& n : e.graph.nodes)
			if (n.type == ParticleNodeType::EmitterOutput) { after = &n; break; }

		json result{
			{ "pin",         pinName },
			{ "node",        nodeId },
			{ "createdNode", created },
		};
		if (after) result["state"] = pinJson(e.graph, *after, index);
		bool randomised = false;
		result["emitter"] = configJson(e.graph, randomised);
		if (randomised)
			result["note"] =
				"A Random Range node resolves ONCE per evaluation, so the 'emitter' block "
				"above is one roll of it and the running game rolls its own.";
		return writeEmitter(*cm, *h, e, std::move(result));
	};
	registry.add(std::move(t));
}

// ── particle_slot_set ────────────────────────────────────────────────────────

void addSlotSet(McpToolRegistry& registry, ContentManager& content,
                const std::shared_ptr<McpParticleHooks>& h)
{
	ContentManager* cm = &content;
	McpTool t;
	t.name        = "particle_slot_set";
	t.description =
		"Set the mesh or the material the emitter draws its particles with. These are "
		"slots on the Emitter Output node itself rather than inputs (a particle's look "
		"is not something the graph computes), so they are addressed by asset path and "
		"stored as a UUID — exactly what a drag-drop onto the node body does. An empty "
		"path clears the slot.";
	t.inputSchema = objectSchema(json{
		{ "path",      stringProp("Content-relative path of the Particle System asset.") },
		{ "slot",      stringProp("'mesh' or 'material'.") },
		{ "assetPath", stringProp("Content-relative path of the Static Mesh or Material "
		                          "to use. Empty or omitted clears the slot.") },
	}, { "path", "slot" });
	t.mutates = true;
	t.handler = [cm, h](const json& args) -> ToolResult {
		Emitter e = openEmitter(*cm, *h, args, /*forWrite=*/true);
		if (!e.ok) return e.failure;

		const std::string slot = strArg(args, "slot");
		const bool isMesh = slot == "mesh";
		if (!isMesh && slot != "material")
			return ToolResult::fail("invalid_payload",
				"'" + slot + "' is not a slot of the Emitter Output. The slots are: mesh, "
				"material. Everything else an emitter has is an input — particle_set writes "
				"those.");

		// Resolved BEFORE anything loads the emitter itself: the referenced asset's
		// own META is read off disk (no load, so a question cannot change what is
		// resident), and the answer is a plain UUID rather than a pointer, which is
		// the half of ContentManager.h's lifetime rule that gets missed.
		HE::UUID ref{};
		const std::string refPath = strArg(args, "assetPath");
		if (!refPath.empty())
		{
			const PathCheck rp = checkPath(*cm, refPath, /*mustExist=*/true, "assetPath");
			if (!rp.ok) return rp.failure;
			const HE::AssetType rt = EditorAssetTypeCache::assetTypeOf(rp.abs);
			const bool okType = isMesh ? (rt == HE::AssetType::StaticMesh ||
			                              rt == HE::AssetType::SkeletalMesh)
			                           : rt == HE::AssetType::Material;
			if (!okType)
				return ToolResult::fail("invalid_payload",
					"'" + rp.rel + "' is not a " + (isMesh ? "mesh" : "Material") +
					" asset, so the emitter would reference something it cannot draw with. "
					"asset_list with a type filter says what there is.");
			ref = HE::AssetRefs::assetUuidOfFile(rp.abs);
			if (ref == HE::UUID{})
				return ToolResult::fail("failed",
					"'" + rp.rel + "' has no readable asset id of its own, so nothing can "
					"reference it. The editor log carries the reason.");
		}

		ParticleGraphNode* out = nullptr;
		for (ParticleGraphNode& n : e.graph.nodes)
			if (n.type == ParticleNodeType::EmitterOutput) { out = &n; break; }
		if (!out)
			return ToolResult::fail("invalid_payload",
				"'" + e.rel + "' has no Emitter Output node, which every particle graph the "
				"editor writes has — there is no slot to write to.");

		if (isMesh) out->meshAssetId     = ref;
		else        out->materialAssetId = ref;

		json result{
			{ "slot",    slot },
			{ "cleared", ref == HE::UUID{} },
			{ "uuid",    uuidJson(ref) },
		};
		if (!refPath.empty()) result["assetPath"] = refPath;
		return writeEmitter(*cm, *h, e, std::move(result));
	};
	registry.add(std::move(t));
}

} // namespace

void registerParticleTools(McpToolRegistry& registry, ContentManager& content,
                           McpParticleHooks hooks)
{
	// Shared rather than copied into each handler, like the material, prefab and
	// type tools: the hooks hold std::functions that capture the editor, and one
	// copy per handler would be one chance per handler to let one go stale.
	auto h = std::make_shared<McpParticleHooks>(std::move(hooks));
	addInfo(registry, content, h);
	addSet(registry, content, h);
	addSlotSet(registry, content, h);
}

} // namespace HE::Ed
