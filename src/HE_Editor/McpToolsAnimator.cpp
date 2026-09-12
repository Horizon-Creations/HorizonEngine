#include "McpToolRegistry.h"

#include "EditorAssetTypeCache.h"     // what a path holds, without loading it
#include "McpToolCommon.h"            // the argument readers, the confinement rule, the walk

#include <AnimatorStateMachine/AnimatorStateMachineGraph.h>
#include <BlendSpace/BlendSpace.h>
#include <ContentManager/AssetRefScan.h>
#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/HAsset.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

// ─── Authoring the animation assets from outside the editor ──────────────────
// Why they need tools of their own, what happens to a transition whose parameter
// or endpoint state disappears, and why a transition is addressed by a triple:
// McpToolRegistry.h, beside McpAnimatorHooks. What is worth stating HERE is what
// the handlers promise.
//
//   • A STATE IS ADDRESSED BY NAME, AND SO IS EVERYTHING THAT POINTS AT IT. That
//     is the format's own wart (AnimatorStateMachineGraph.h): a transition names
//     its endpoints by state name. So a rename here rewrites every transition
//     and `startState` that named the old one — the fix-up the editor panel does
//     by hand in its name field, and the one thing a caller cannot do for
//     itself, because "remove and add again" loses the transitions.
//
//   • A PARAMETER THAT GOES AWAY IS A TRANSITION THAT NEVER FIRES AGAIN.
//     `evalTransition` returns false for a parameter the live map does not hold
//     — no error, no log line. So removing one that a transition names is
//     refused, with the transitions listed, and needs `force` to happen anyway.
//
//   • A POSE SOURCE IS ONE THING, NOT TWO. A state's blend space WINS over its
//     clip (AnimatorStateMachineGraph.h), so setting one clears the other rather
//     than leaving a value that is on disk and never read.
//
//   • A REFERENCE IS GIVEN AS A PATH AND STORED AS A UUID, which is exactly what
//     dropping an asset on the slot does. The referenced file's own id is read
//     off disk, without loading it — a question must not change what is
//     resident.
//
//   • A REFUSAL IS A NO-OP. Nothing is written, no live animator is invalidated
//     and no tab is told to re-read anything.

namespace HE::Ed
{

using nlohmann::json;

namespace
{

// ── The operator vocabulary, as the panel spells it ──────────────────────────
struct OpName { const char* name; TransitionOp op; };
constexpr OpName kOps[] = {
	{ "greater", TransitionOp::Greater },
	{ "less",    TransitionOp::Less },
	{ "equal",   TransitionOp::Equal },
};

const char* opName(TransitionOp op)
{
	for (const OpName& o : kOps)
		if (o.op == op) return o.name;
	return "greater";
}

bool opFromName(const std::string& s, TransitionOp& out)
{
	for (const OpName& o : kOps)
		if (s == o.name) { out = o.op; return true; }
	return false;
}

json uuidJson(const HE::UUID& id)
{
	return json::array({ static_cast<std::uint64_t>(id.hi), static_cast<std::uint64_t>(id.lo) });
}

// ── Naming a referenced asset back ───────────────────────────────────────────
// A state stores its clip and its blend space as UUIDs, which is the wrong end
// of the question a client asks ("which clip is on Idle?"). There is no
// path-for-id lookup that does not load, so the answer is the same walk
// `asset_list` does, filtered to the two types that can be referenced here and
// built ONCE per call. Detail views only: the catalogue would pay for it per
// asset and does not report references at all.
class RefNames
{
public:
	explicit RefNames(ContentManager& content) : m_content(&content) {}

	std::string pathOf(const HE::UUID& id)
	{
		if (id == HE::UUID{}) return {};
		if (!m_built) build();
		const auto it = m_byId.find(std::make_pair(id.hi, id.lo));
		return it == m_byId.end() ? std::string() : it->second;
	}

private:
	void build()
	{
		m_built = true;
		bool truncated = false;
		for (const ContentAsset& a : walkContentAssets(
		         *m_content, { HE::AssetType::AnimationClip, HE::AssetType::BlendSpace },
		         2000, truncated))
		{
			const HE::UUID id = HE::AssetRefs::assetUuidOfFile(a.abs);
			if (!(id == HE::UUID{})) m_byId.emplace(std::make_pair(id.hi, id.lo), a.rel);
		}
	}

	ContentManager* m_content;
	bool            m_built = false;
	std::map<std::pair<std::uint64_t, std::uint64_t>, std::string> m_byId;
};

// A reference as a client reads it: the id it really is, plus the path when the
// project still holds a file with that id. A reference whose file is gone
// reports the id alone, which is the honest answer — the state machine will
// find nothing either.
json refJson(RefNames& names, const HE::UUID& id)
{
	json j{ { "uuid", uuidJson(id) } };
	if (id == HE::UUID{}) return j;
	const std::string p = names.pathOf(id);
	if (!p.empty()) j["path"] = p;
	else            j["missing"] = true;
	return j;
}

// ── The addressed asset ──────────────────────────────────────────────────────
// One struct for the state machine and the blend space, because the path check,
// the play-mode gate, the lock gate and the unsaved-tab gate are the same four
// questions for both, asked in the same order as everywhere else.
struct Doc
{
	std::string   rel;
	std::string   abs;
	HE::AssetType type = HE::AssetType::Unknown;

	AnimatorStateMachineGraph graph;
	BlendSpace                space;

	bool       ok      = false;
	ToolResult failure = ToolResult::ok(json::object());
};

bool readPayload(const std::string& abs, std::uint32_t chunk, std::string& out)
{
	HAsset::Reader r;
	if (!r.open(abs)) return false;
	if (const HAsset::Reader::Chunk* c = r.findChunk(chunk))
		out.assign(reinterpret_cast<const char*>(c->data.data()), c->data.size());
	// An absent chunk is not a failure: a freshly created asset of either type
	// has none (AssetStubWriter writes no payload for them), and what the panels
	// show for one is the empty document — which is what this reads as.
	return true;
}

Doc openDoc(ContentManager& content, const McpAnimatorHooks& h, const json& args,
            bool forWrite, HE::AssetType want)
{
	Doc d;
	const PathCheck p = checkPath(content, strArg(args, "path"), /*mustExist=*/true, "path");
	if (!p.ok) { d.failure = p.failure; return d; }
	d.rel  = p.rel;
	d.abs  = p.abs;
	d.type = EditorAssetTypeCache::assetTypeOf(p.abs);

	if (d.type != want)
	{
		const bool wantMachine = want == HE::AssetType::AnimatorStateMachine;
		d.failure = ToolResult::fail("invalid_path",
			"'" + p.rel + "' is not " +
			(wantMachine ? "an Animator State Machine" : "a Blend Space") + " asset. " +
			(wantMachine ? "animator_info" : "blendspace_info") +
			" without arguments lists every one in the project, asset_resolve reports what "
			"a path holds, and asset_create with type '" +
			(wantMachine ? "AnimatorStateMachine" : "BlendSpace") + "' makes a new one.");
		return d;
	}

	std::string payload;
	const std::uint32_t chunk = want == HE::AssetType::AnimatorStateMachine
	                                ? HAsset::CHUNK_ASMG : HAsset::CHUNK_BLSP;
	if (!readPayload(p.abs, chunk, payload))
	{
		d.failure = ToolResult::fail("failed",
			"'" + p.rel + "' could not be read. The editor log carries the reason.");
		return d;
	}
	if (!payload.empty())
	{
		if (want == HE::AssetType::AnimatorStateMachine)
			animatorStateMachineFromJson(payload, d.graph);
		else
			blendSpaceFromJson(payload, d.space);
	}

	if (forWrite)
	{
		if (p.engine) { d.failure = failEngineReadOnly(p.rel); return d; }
		if (h.isPlaying && h.isPlaying())
		{
			d.failure = ToolResult::fail("play_mode",
				"Play-in-editor is running, and the running session's animators have "
				"already resolved this asset — their live parameters and current states "
				"came from it. A change now would be half in effect, so it is refused "
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
				"'" + p.rel + "' is open in the editor with unsaved changes. That tab's own "
				"copy is the truth while it is dirty: writing the file would be reverted by "
				"the human's next Save, and there is no way to land an edit in the tab that "
				"they could take back. Ask the user to save or close that tab, then call "
				"again.");
			return d;
		}
	}
	d.ok = true;
	return d;
}

// ── Writing one back ─────────────────────────────────────────────────────────
// The panels' own saves, each of them: the file, and for a state machine the
// live animators too. A state machine asset also carries a SYNC GRAPH chunk
// (CHUNK_ASSY) this file never touches — going through the loaded asset rather
// than writing the file directly is what keeps it: `saveAsset` rewrites both
// chunks out of the asset, and the loader put the sync graph there.
//
// The load is the ONLY one in this path and nothing is loaded after it, so the
// pointer taken here cannot be moved out from under us by a second asset
// registering (ContentManager.h: the pool is a dense vector).
ToolResult writeDoc(ContentManager& content, const McpAnimatorHooks& h, Doc& d, json out)
{
	const bool machine = d.type == HE::AssetType::AnimatorStateMachine;
	const std::string payload = machine ? animatorStateMachineToJson(d.graph)
	                                    : blendSpaceToJson(d.space);
	const HE::UUID id = content.loadAsset(d.rel);
	bool wrote = false;
	if (!(id == HE::UUID{}))
	{
		if (machine)
		{
			if (AnimatorStateMachineAsset* a = content.getAnimatorStateMachineMutable(id))
			{
				a->graphJson = payload;
				wrote = content.saveAsset(*a);
			}
		}
		else if (BlendSpaceAsset* a = content.getBlendSpaceMutable(id))
		{
			a->json = payload;
			wrote = content.saveAsset(*a);
		}
	}
	if (!wrote)
		return ToolResult::fail("failed",
			"Could not write '" + d.rel + "'. A read-only file or a full disk is the "
			"usual cause; the editor log carries the reason.");

	// Only for the state machine: BlendSpacePanel's Save does not invalidate live
	// entities either, and a tool that invalidated more than the panel would be a
	// second, quietly different save path.
	if (machine && h.onGraphChanged) h.onGraphChanged(d.rel);

	out["path"] = d.rel;
	out["reloadedInEditor"] = h.reloadFromDisk ? h.reloadFromDisk(d.rel) : false;
	return ToolResult::ok(std::move(out));
}

// ── Reading the graph ────────────────────────────────────────────────────────

AnimationState* findState(AnimatorStateMachineGraph& g, const std::string& name)
{
	for (AnimationState& s : g.states)
		if (s.name == name) return &s;
	return nullptr;
}

std::string stateNames(const AnimatorStateMachineGraph& g)
{
	std::string s;
	for (const AnimationState& st : g.states) s += (s.empty() ? "" : ", ") + st.name;
	return s.empty() ? "no states at all" : s;
}

AnimationTransition* findTransition(AnimatorStateMachineGraph& g, const std::string& from,
                                    const std::string& to, const std::string& param)
{
	for (AnimationTransition& t : g.transitions)
		if (t.fromState == from && t.toState == to && t.paramName == param) return &t;
	return nullptr;
}

json stateJson(RefNames& names, const AnimatorStateMachineGraph& g, const AnimationState& s)
{
	json j{
		{ "name",    s.name },
		{ "id",      s.id },
		{ "looping", s.looping },
	};
	// The pose source, resolved the way the runtime resolves it: a set blend
	// space wins, so reporting both as equals would misdescribe the state.
	if (!(s.blendSpaceId == HE::UUID{}))
	{
		j["poseSource"] = "blendSpace";
		j["blendSpace"] = refJson(names, s.blendSpaceId);
		if (!(s.clipId == HE::UUID{}))
			j["clip"] = refJson(names, s.clipId);
	}
	else if (!(s.clipId == HE::UUID{}))
	{
		j["poseSource"] = "clip";
		j["clip"]       = refJson(names, s.clipId);
	}
	else
	{
		j["poseSource"] = "none";
	}
	j["isStart"] = (!g.startState.empty() && g.startState == s.name) ||
	               (g.startState.empty() && !g.states.empty() && g.states.front().name == s.name);
	return j;
}

json transitionJson(const AnimatorStateMachineGraph& g, const AnimationTransition& t)
{
	json j{
		{ "from",      t.fromState },
		{ "to",        t.toState },
		{ "param",     t.paramName },
		{ "op",        opName(t.op) },
		{ "threshold", t.threshold },
		{ "duration",  t.duration },
	};
	// The three ways a transition can be dead without anything saying so. Named
	// here because this is the only place a client can see them at all.
	if (g.defaultParams.find(t.paramName) == g.defaultParams.end())
		j["undeclaredParam"] = true;
	bool haveFrom = false, haveTo = false;
	for (const AnimationState& s : g.states)
	{
		if (s.name == t.fromState) haveFrom = true;
		if (s.name == t.toState)   haveTo   = true;
	}
	if (!haveFrom || !haveTo) j["danglingEndpoint"] = true;
	return j;
}

// Every transition that names a parameter — the list a removal has to show.
json transitionsUsing(const AnimatorStateMachineGraph& g, const std::string& param)
{
	json out = json::array();
	for (const AnimationTransition& t : g.transitions)
		if (t.paramName == param)
			out.push_back(json{ { "from", t.fromState }, { "to", t.toState } });
	return out;
}

// ── Resolving a referenced asset ─────────────────────────────────────────────
// Path in, UUID out, read from the file's own META — no load, so asking cannot
// change what is resident, and the answer is a plain value rather than a pointer
// into a pool the next load may move (ContentManager.h).
bool resolveRef(ContentManager& cm, const json& args, const char* argName,
                HE::AssetType want, HE::UUID& out, ToolResult& fail)
{
	const std::string raw = strArg(args, argName);
	if (raw.empty()) { out = HE::UUID{}; return true; }   // explicit clear
	const PathCheck p = checkPath(cm, raw, /*mustExist=*/true, argName);
	if (!p.ok) { fail = p.failure; return false; }
	if (EditorAssetTypeCache::assetTypeOf(p.abs) != want)
	{
		fail = ToolResult::fail("invalid_payload",
			"'" + p.rel + "' is not " +
			(want == HE::AssetType::AnimationClip ? "an Animation Clip" : "a Blend Space") +
			" asset, so the state would reference something it cannot pose from. asset_list "
			"with a type filter says what there is.");
		return false;
	}
	out = HE::AssetRefs::assetUuidOfFile(p.abs);
	if (out == HE::UUID{})
	{
		fail = ToolResult::fail("failed",
			"'" + p.rel + "' has no readable asset id of its own, so nothing can reference "
			"it. The editor log carries the reason.");
		return false;
	}
	return true;
}

// ── animator_info ────────────────────────────────────────────────────────────

void addAnimatorInfo(McpToolRegistry& registry, ContentManager& content,
                     const std::shared_ptr<McpAnimatorHooks>& h)
{
	ContentManager* cm = &content;
	McpTool t;
	t.name        = "animator_info";
	t.description =
		"Without arguments: every Animator State Machine asset in the project. With "
		"'path': that machine in full — its states with what each poses from, its "
		"transitions with the parameter each watches, and its parameters with their "
		"starting values. A transition whose parameter is not declared, or whose "
		"endpoint state does not exist, is marked: both are silently dead at runtime.";
	t.inputSchema = objectSchema(json{
		{ "path",  stringProp("Content-relative path of an Animator State Machine asset, "
		                      "e.g. 'Animation/Player.hasset'. Omit for the catalogue.") },
		{ "limit", numberProp("Maximum number of assets in the catalogue (default 200).") },
	}, {});
	t.handler = [cm, h](const json& args) -> ToolResult {
		if (!hasArg(args, "path"))
		{
			bool truncated = false;
			const std::vector<ContentAsset> found = walkContentAssets(
				*cm, { HE::AssetType::AnimatorStateMachine }, intArg(args, "limit", 200),
				truncated);
			json list = json::array();
			for (const ContentAsset& a : found)
			{
				std::string payload;
				AnimatorStateMachineGraph g;
				if (readPayload(a.abs, HAsset::CHUNK_ASMG, payload) && !payload.empty())
					animatorStateMachineFromJson(payload, g);
				list.push_back(json{
					{ "path",            a.rel },
					{ "stateCount",      static_cast<int>(g.states.size()) },
					{ "transitionCount", static_cast<int>(g.transitions.size()) },
					{ "paramCount",      static_cast<int>(g.defaultParams.size()) },
				});
			}
			json out{ { "stateMachines", std::move(list) } };
			if (truncated) out["truncated"] = true;
			return ToolResult::ok(std::move(out));
		}

		Doc d = openDoc(*cm, *h, args, /*forWrite=*/false,
		                HE::AssetType::AnimatorStateMachine);
		if (!d.ok) return d.failure;

		RefNames names(*cm);
		json states = json::array();
		for (const AnimationState& s : d.graph.states) states.push_back(stateJson(names, d.graph, s));
		json transitions = json::array();
		for (const AnimationTransition& tr : d.graph.transitions)
			transitions.push_back(transitionJson(d.graph, tr));
		// Sorted, so two calls on an unchanged asset answer identically — the
		// defaults live in an unordered_map, whose order is nobody's decision.
		std::vector<std::pair<std::string, float>> params(d.graph.defaultParams.begin(),
		                                                  d.graph.defaultParams.end());
		std::sort(params.begin(), params.end(),
		          [](const auto& a, const auto& b) { return a.first < b.first; });
		json jparams = json::array();
		for (const auto& [name, value] : params)
			jparams.push_back(json{ { "name", name }, { "value", value } });

		json out{
			{ "path",        d.rel },
			{ "states",      std::move(states) },
			{ "transitions", std::move(transitions) },
			{ "params",      std::move(jparams) },
		};
		// The name the runtime will actually enter, not just the field: an empty
		// startState means the first state, and a client reading the field alone
		// would report "none" for a machine that starts fine.
		out["startState"] = !d.graph.startState.empty()
		                        ? d.graph.startState
		                        : (d.graph.states.empty() ? std::string()
		                                                  : d.graph.states.front().name);
		out["startStateExplicit"] = !d.graph.startState.empty();
		return ToolResult::ok(std::move(out));
	};
	registry.add(std::move(t));
}

// ── animator_state_set / animator_state_remove ───────────────────────────────

void addStateSet(McpToolRegistry& registry, ContentManager& content,
                 const std::shared_ptr<McpAnimatorHooks>& h)
{
	ContentManager* cm = &content;
	McpTool t;
	t.name        = "animator_state_set";
	t.description =
		"Add a state to an Animator State Machine, or change one that is there. "
		"Addressed by NAME. 'newName' renames it AND re-points every transition and the "
		"start state that named the old one — a rename done as remove-and-add would "
		"lose those. A state poses from a clip or from a blend space, never both: "
		"setting one clears the other.";
	t.inputSchema = objectSchema(json{
		{ "path",       stringProp("Content-relative path of the Animator State Machine.") },
		{ "name",       stringProp("State name. An existing one is updated, a new one "
		                           "added.") },
		{ "newName",    stringProp("Rename the state to this. Every transition endpoint "
		                           "and the start state follow.") },
		{ "clip",       stringProp("Content-relative path of the Animation Clip this state "
		                           "poses from. Empty clears it.") },
		{ "blendSpace", stringProp("Content-relative path of the Blend Space this state "
		                           "poses from instead of a clip. Empty clears it.") },
		{ "looping",    json{ { "type", "boolean" },
		                      { "description", "Whether the state's pose loops." } } },
		{ "start",      json{ { "type", "boolean" },
		                      { "description",
		                        "Make this the state the machine starts in." } } },
	}, { "path", "name" });
	t.mutates = true;
	t.handler = [cm, h](const json& args) -> ToolResult {
		Doc d = openDoc(*cm, *h, args, /*forWrite=*/true, HE::AssetType::AnimatorStateMachine);
		if (!d.ok) return d.failure;

		const std::string name = strArg(args, "name");
		if (name.empty())
			return ToolResult::fail("invalid_payload",
				"'name' is required and must not be empty — a transition names its "
				"endpoints by state name, so a nameless state is one nothing can reach.");

		// Both references resolved BEFORE the graph is touched, so a bad path is a
		// refusal that changed nothing rather than a half-edited state.
		HE::UUID clipId{}, spaceId{};
		ToolResult fail = ToolResult::ok(json::object());
		const bool wantClip  = hasArg(args, "clip");
		const bool wantSpace = hasArg(args, "blendSpace");
		if (wantClip && !resolveRef(*cm, args, "clip", HE::AssetType::AnimationClip, clipId, fail))
			return fail;
		if (wantSpace && !resolveRef(*cm, args, "blendSpace", HE::AssetType::BlendSpace, spaceId, fail))
			return fail;
		if (wantClip && wantSpace && !(clipId == HE::UUID{}) && !(spaceId == HE::UUID{}))
			return ToolResult::fail("invalid_payload",
				"A state has ONE pose source: a set blend space wins over a clip, so "
				"passing both would leave a clip on disk that is never read. Pass one of "
				"them.");

		AnimationState* existing = findState(d.graph, name);
		const bool created = existing == nullptr;
		if (created)
		{
			AnimationState s;
			int maxId = 0;
			for (const AnimationState& x : d.graph.states) maxId = (std::max)(maxId, x.id);
			// The panel's "Add State": the next free id, and a canvas spot that is
			// not on top of the last one.
			s.id   = maxId + 1;
			s.name = name;
			s.x    = 80.0f + static_cast<float>(d.graph.states.size() % 4) * 220.0f;
			s.y    = 80.0f + static_cast<float>(d.graph.states.size() / 4) * 160.0f;
			d.graph.states.push_back(s);
			existing = &d.graph.states.back();
		}

		if (wantClip)
		{
			existing->clipId = clipId;
			// A blend space still set would win over the clip that was just asked
			// for, which is the one thing a caller of this argument cannot mean.
			if (!(clipId == HE::UUID{})) existing->blendSpaceId = HE::UUID{};
		}
		if (wantSpace)
		{
			existing->blendSpaceId = spaceId;
			if (!(spaceId == HE::UUID{})) existing->clipId = HE::UUID{};
		}
		if (hasArg(args, "looping")) existing->looping = boolArg(args, "looping", true);

		std::string finalName = name;
		int retargeted = 0;
		if (hasArg(args, "newName"))
		{
			const std::string newName = strArg(args, "newName");
			if (newName.empty())
				return ToolResult::fail("invalid_payload",
					"'newName' must not be empty — see 'name'.");
			if (newName != name)
			{
				if (findState(d.graph, newName))
					return ToolResult::fail("invalid_payload",
						"'" + d.rel + "' already has a state called '" + newName + "'. Two "
						"states with one name make every transition naming it ambiguous, and "
						"the runtime takes the first — so it is refused rather than written.");
				existing->name = newName;
				// The wart, handled: endpoints and the start state are NAMES.
				for (AnimationTransition& tr : d.graph.transitions)
				{
					if (tr.fromState == name) { tr.fromState = newName; ++retargeted; }
					if (tr.toState   == name) { tr.toState   = newName; ++retargeted; }
				}
				if (d.graph.startState == name) d.graph.startState = newName;
				finalName = newName;
			}
		}

		if (boolArg(args, "start")) d.graph.startState = finalName;

		RefNames names(*cm);
		const AnimationState* now = findState(d.graph, finalName);
		json out{
			{ "name",       finalName },
			{ "created",    created },
			{ "stateCount", static_cast<int>(d.graph.states.size()) },
		};
		if (finalName != name)
		{
			out["renamedFrom"]        = name;
			out["retargetedEndpoints"] = retargeted;
		}
		if (now) out["state"] = stateJson(names, d.graph, *now);
		return writeDoc(*cm, *h, d, std::move(out));
	};
	registry.add(std::move(t));
}

void addStateRemove(McpToolRegistry& registry, ContentManager& content,
                    const std::shared_ptr<McpAnimatorHooks>& h)
{
	ContentManager* cm = &content;
	McpTool t;
	t.name        = "animator_state_remove";
	t.description =
		"Remove a state from an Animator State Machine. Every transition that starts or "
		"ends at it goes with it — leaving them would leave endpoints naming a state "
		"that no longer exists, which the runtime skips without a word — and the start "
		"state is cleared if it pointed there. The same cascade the editor performs when "
		"a state node is deleted.";
	t.inputSchema = objectSchema(json{
		{ "path", stringProp("Content-relative path of the Animator State Machine.") },
		{ "name", stringProp("Name of the state to remove.") },
	}, { "path", "name" });
	t.mutates = true;
	t.handler = [cm, h](const json& args) -> ToolResult {
		Doc d = openDoc(*cm, *h, args, /*forWrite=*/true, HE::AssetType::AnimatorStateMachine);
		if (!d.ok) return d.failure;

		const std::string name = strArg(args, "name");
		const auto at = std::find_if(d.graph.states.begin(), d.graph.states.end(),
		                             [&name](const AnimationState& s) { return s.name == name; });
		if (at == d.graph.states.end())
			return ToolResult::fail("not_found",
				"'" + d.rel + "' has no state called '" + name + "'. It has: " +
				stateNames(d.graph) + ".");

		d.graph.states.erase(at);
		const std::size_t before = d.graph.transitions.size();
		d.graph.transitions.erase(
			std::remove_if(d.graph.transitions.begin(), d.graph.transitions.end(),
			               [&name](const AnimationTransition& t) {
				               return t.fromState == name || t.toState == name;
			               }),
			d.graph.transitions.end());
		const int dropped = static_cast<int>(before - d.graph.transitions.size());
		const bool wasStart = d.graph.startState == name;
		if (wasStart) d.graph.startState.clear();

		json out{
			{ "removed",             name },
			{ "removedTransitions",  dropped },
			{ "clearedStartState",   wasStart },
			{ "stateCount",          static_cast<int>(d.graph.states.size()) },
		};
		if (wasStart && !d.graph.states.empty())
			out["startState"] = d.graph.states.front().name;   // what the runtime falls back to
		return writeDoc(*cm, *h, d, std::move(out));
	};
	registry.add(std::move(t));
}

// ── animator_transition_set / animator_transition_remove ─────────────────────

void addTransitionSet(McpToolRegistry& registry, ContentManager& content,
                      const std::shared_ptr<McpAnimatorHooks>& h)
{
	ContentManager* cm = &content;
	McpTool t;
	t.name        = "animator_transition_set";
	t.description =
		"Add a transition between two states, or change one that is there. A transition "
		"has no id of its own, so it is addressed by the triple (from, to, param) — that "
		"triple existing means an update, otherwise one is appended, which is what lets "
		"two states be connected twice on different parameters. Everything the call does "
		"not mention keeps its value.";
	t.inputSchema = objectSchema(json{
		{ "path",      stringProp("Content-relative path of the Animator State Machine.") },
		{ "from",      stringProp("Name of the state the transition leaves.") },
		{ "to",        stringProp("Name of the state it enters.") },
		{ "param",     stringProp("Name of the parameter it watches. animator_param_set "
		                          "declares one.") },
		{ "op",        stringProp("How the parameter is compared: 'greater', 'less' or "
		                          "'equal'. Default greater.") },
		{ "threshold", numberProp("The value it is compared against. Default 0.5.") },
		{ "duration",  numberProp("Crossfade length in seconds. Default 0.2.") },
	}, { "path", "from", "to", "param" });
	t.mutates = true;
	t.handler = [cm, h](const json& args) -> ToolResult {
		Doc d = openDoc(*cm, *h, args, /*forWrite=*/true, HE::AssetType::AnimatorStateMachine);
		if (!d.ok) return d.failure;

		const std::string from  = strArg(args, "from");
		const std::string to    = strArg(args, "to");
		const std::string param = strArg(args, "param");
		if (param.empty())
			return ToolResult::fail("invalid_payload",
				"'param' is required and must not be empty — a transition with no parameter "
				"is compared against a value nothing writes, so it never fires.");
		for (const std::string* end : { &from, &to })
			if (!findState(d.graph, *end))
				return ToolResult::fail("not_found",
					"'" + d.rel + "' has no state called '" + *end + "'. It has: " +
					stateNames(d.graph) + ". An endpoint naming a state that does not exist "
					"is skipped by the runtime without a word, so it is refused here.");

		TransitionOp op = TransitionOp::Greater;
		if (hasArg(args, "op") && !opFromName(strArg(args, "op"), op))
			return ToolResult::fail("invalid_payload",
				"'" + strArg(args, "op") + "' is not a comparison. One of: greater, less, "
				"equal.");

		AnimationTransition* existing = findTransition(d.graph, from, to, param);
		const bool created = existing == nullptr;
		if (created)
		{
			AnimationTransition tr;
			tr.fromState = from;
			tr.toState   = to;
			tr.paramName = param;
			d.graph.transitions.push_back(tr);
			existing = &d.graph.transitions.back();
		}
		if (hasArg(args, "op"))        existing->op        = op;
		if (hasArg(args, "threshold")) existing->threshold = static_cast<float>(numArg(args, "threshold", 0.5));
		if (hasArg(args, "duration"))  existing->duration  = static_cast<float>(numArg(args, "duration", 0.2));

		json out{
			{ "created",         created },
			{ "transitionCount", static_cast<int>(d.graph.transitions.size()) },
			{ "transition",      transitionJson(d.graph, *existing) },
		};
		// Reported, not refused: a sync graph or a script may write a parameter at
		// runtime that was never declared as a default — the live map is open. What
		// would be wrong is letting it pass in silence.
		if (d.graph.defaultParams.find(param) == d.graph.defaultParams.end())
			out["note"] =
				"No parameter called '" + param + "' is declared on this machine. Unless "
				"something writes it at runtime, this transition can never fire: a "
				"parameter the live map does not hold compares as false, every frame, "
				"with nothing logged. animator_param_set declares it.";
		return writeDoc(*cm, *h, d, std::move(out));
	};
	registry.add(std::move(t));
}

void addTransitionRemove(McpToolRegistry& registry, ContentManager& content,
                         const std::shared_ptr<McpAnimatorHooks>& h)
{
	ContentManager* cm = &content;
	McpTool t;
	t.name        = "animator_transition_remove";
	t.description =
		"Remove a transition, addressed by the same (from, to, param) triple that "
		"animator_transition_set writes it with.";
	t.inputSchema = objectSchema(json{
		{ "path",  stringProp("Content-relative path of the Animator State Machine.") },
		{ "from",  stringProp("Name of the state the transition leaves.") },
		{ "to",    stringProp("Name of the state it enters.") },
		{ "param", stringProp("Name of the parameter it watches.") },
	}, { "path", "from", "to", "param" });
	t.mutates = true;
	t.handler = [cm, h](const json& args) -> ToolResult {
		Doc d = openDoc(*cm, *h, args, /*forWrite=*/true, HE::AssetType::AnimatorStateMachine);
		if (!d.ok) return d.failure;

		const std::string from  = strArg(args, "from");
		const std::string to    = strArg(args, "to");
		const std::string param = strArg(args, "param");
		const auto at = std::find_if(
			d.graph.transitions.begin(), d.graph.transitions.end(),
			[&](const AnimationTransition& t) {
				return t.fromState == from && t.toState == to && t.paramName == param;
			});
		if (at == d.graph.transitions.end())
		{
			std::string have;
			for (const AnimationTransition& tr : d.graph.transitions)
				have += (have.empty() ? "" : ", ") + tr.fromState + " -> " + tr.toState +
				        " on " + tr.paramName;
			return ToolResult::fail("not_found",
				"'" + d.rel + "' has no transition from '" + from + "' to '" + to + "' on '" +
				param + "'. It has: " + (have.empty() ? "no transitions at all" : have) + ".");
		}
		d.graph.transitions.erase(at);

		return writeDoc(*cm, *h, d, json{
			{ "removed",         json{ { "from", from }, { "to", to }, { "param", param } } },
			{ "transitionCount", static_cast<int>(d.graph.transitions.size()) },
		});
	};
	registry.add(std::move(t));
}

// ── animator_param_set / animator_param_remove ───────────────────────────────

void addParamSet(McpToolRegistry& registry, ContentManager& content,
                 const std::shared_ptr<McpAnimatorHooks>& h)
{
	ContentManager* cm = &content;
	McpTool t;
	t.name        = "animator_param_set";
	t.description =
		"Declare a parameter of an Animator State Machine, or change its starting value. "
		"Parameters are numbers — the only thing a transition compares. The value here "
		"is the one a fresh entity starts with; a running one keeps whatever a script or "
		"the sync graph has written since.";
	t.inputSchema = objectSchema(json{
		{ "path",  stringProp("Content-relative path of the Animator State Machine.") },
		{ "name",  stringProp("Parameter name — the name transitions refer to it by.") },
		{ "value", numberProp("Its starting value. Default 0.") },
	}, { "path", "name" });
	t.mutates = true;
	t.handler = [cm, h](const json& args) -> ToolResult {
		Doc d = openDoc(*cm, *h, args, /*forWrite=*/true, HE::AssetType::AnimatorStateMachine);
		if (!d.ok) return d.failure;

		const std::string name = strArg(args, "name");
		if (name.empty())
			return ToolResult::fail("invalid_payload",
				"'name' is required and must not be empty — a parameter is addressed by "
				"name and nothing else.");

		const bool created = d.graph.defaultParams.find(name) == d.graph.defaultParams.end();
		const float value = hasArg(args, "value")
		                        ? static_cast<float>(numArg(args, "value", 0.0))
		                        : (created ? 0.0f : d.graph.defaultParams[name]);
		d.graph.defaultParams[name] = value;

		return writeDoc(*cm, *h, d, json{
			{ "name",       name },
			{ "value",      value },
			{ "created",    created },
			{ "paramCount", static_cast<int>(d.graph.defaultParams.size()) },
			{ "usedBy",     transitionsUsing(d.graph, name) },
		});
	};
	registry.add(std::move(t));
}

void addParamRemove(McpToolRegistry& registry, ContentManager& content,
                    const std::shared_ptr<McpAnimatorHooks>& h)
{
	ContentManager* cm = &content;
	McpTool t;
	t.name        = "animator_param_remove";
	t.description =
		"Remove a parameter's declaration. REFUSED while a transition names it, with "
		"those transitions listed: a transition whose parameter the live map does not "
		"hold compares as false every frame, forever, with nothing logged — a machine "
		"that quietly stops moving. 'force' does it anyway, which is right when a script "
		"or the sync graph writes that parameter itself.";
	t.inputSchema = objectSchema(json{
		{ "path",  stringProp("Content-relative path of the Animator State Machine.") },
		{ "name",  stringProp("Name of the parameter to remove.") },
		{ "force", json{ { "type", "boolean" },
		                 { "description",
		                   "Remove it even though transitions name it." } } },
	}, { "path", "name" });
	t.mutates = true;
	t.handler = [cm, h](const json& args) -> ToolResult {
		Doc d = openDoc(*cm, *h, args, /*forWrite=*/true, HE::AssetType::AnimatorStateMachine);
		if (!d.ok) return d.failure;

		const std::string name = strArg(args, "name");
		const auto at = d.graph.defaultParams.find(name);
		if (at == d.graph.defaultParams.end())
		{
			std::string have;
			for (const auto& [k, v] : d.graph.defaultParams) have += (have.empty() ? "" : ", ") + k;
			return ToolResult::fail("not_found",
				"'" + d.rel + "' declares no parameter called '" + name + "'. It declares: " +
				(have.empty() ? "no parameters at all" : have) + ".");
		}

		const json used = transitionsUsing(d.graph, name);
		if (!used.empty() && !boolArg(args, "force"))
		{
			ToolResult r = ToolResult::fail("in_use",
				"'" + name + "' is named by " + std::to_string(used.size()) +
				" transition(s) of '" + d.rel + "'. Removing it would leave them comparing "
				"a value the live map does not hold, which is false every frame with "
				"nothing logged. Remove those transitions first, or pass force to take the "
				"declaration out anyway — which is right when a script or the sync graph "
				"writes this parameter itself.");
			r.content = json{ { "usedBy", used } };
			return r;
		}
		d.graph.defaultParams.erase(at);

		return writeDoc(*cm, *h, d, json{
			{ "removed",    name },
			{ "forced",     !used.empty() },
			{ "usedBy",     used },
			{ "paramCount", static_cast<int>(d.graph.defaultParams.size()) },
		});
	};
	registry.add(std::move(t));
}

// ── blendspace_info ──────────────────────────────────────────────────────────

const char* kindName(BlendSpaceKind k) { return k == BlendSpaceKind::TwoD ? "2d" : "1d"; }

json sampleJson(RefNames& names, const BlendSpace& s, std::size_t index)
{
	const BlendSpaceSample& sm = s.samples[index];
	json j{
		{ "index",      static_cast<int>(index) },
		{ "x",          sm.x },
		{ "speedScale", sm.speedScale },
		{ "clip",       refJson(names, sm.clipId) },
	};
	// Only for a 2D space: `y` is not read on a 1D one, and reporting it would
	// invite a client to set a coordinate that does nothing.
	if (s.kind == BlendSpaceKind::TwoD) j["y"] = sm.y;
	return j;
}

void addBlendSpaceInfo(McpToolRegistry& registry, ContentManager& content,
                       const std::shared_ptr<McpAnimatorHooks>& h)
{
	ContentManager* cm = &content;
	McpTool t;
	t.name        = "blendspace_info";
	t.description =
		"Without arguments: every Blend Space asset in the project. With 'path': that "
		"space in full — its kind, the parameters its axes read, the axis ranges and "
		"every sample with the clip it places and where. Samples are addressed by INDEX, "
		"which this reports: they have no id and two of them may name one clip.";
	t.inputSchema = objectSchema(json{
		{ "path",  stringProp("Content-relative path of a Blend Space asset, e.g. "
		                      "'Animation/Locomotion.hasset'. Omit for the catalogue.") },
		{ "limit", numberProp("Maximum number of assets in the catalogue (default 200).") },
	}, {});
	t.handler = [cm, h](const json& args) -> ToolResult {
		if (!hasArg(args, "path"))
		{
			bool truncated = false;
			const std::vector<ContentAsset> found = walkContentAssets(
				*cm, { HE::AssetType::BlendSpace }, intArg(args, "limit", 200), truncated);
			json list = json::array();
			for (const ContentAsset& a : found)
			{
				std::string payload;
				BlendSpace s;
				if (readPayload(a.abs, HAsset::CHUNK_BLSP, payload) && !payload.empty())
					blendSpaceFromJson(payload, s);
				list.push_back(json{
					{ "path",        a.rel },
					{ "kind",        kindName(s.kind) },
					{ "sampleCount", static_cast<int>(s.samples.size()) },
				});
			}
			json out{ { "blendSpaces", std::move(list) } };
			if (truncated) out["truncated"] = true;
			return ToolResult::ok(std::move(out));
		}

		Doc d = openDoc(*cm, *h, args, /*forWrite=*/false, HE::AssetType::BlendSpace);
		if (!d.ok) return d.failure;

		RefNames names(*cm);
		json samples = json::array();
		for (std::size_t i = 0; i < d.space.samples.size(); ++i)
			samples.push_back(sampleJson(names, d.space, i));

		json out{
			{ "path",    d.rel },
			{ "name",    d.space.name },
			{ "kind",    kindName(d.space.kind) },
			{ "paramX",  d.space.paramX },
			{ "minX",    d.space.minX },
			{ "maxX",    d.space.maxX },
			{ "looping", d.space.looping },
			{ "samples", std::move(samples) },
		};
		if (d.space.kind == BlendSpaceKind::TwoD)
		{
			out["paramY"] = d.space.paramY;
			out["minY"]   = d.space.minY;
			out["maxY"]   = d.space.maxY;
		}
		// The ranges are the EDITOR's axes and not a clamp — the sampler clamps to
		// the outermost sample instead (BlendSpace.h). Said here because a client
		// reading a range would otherwise take it for the domain.
		out["note"] =
			"minX/maxX (and minY/maxY) are the diagram's axes only. The sampler clamps to "
			"the outermost SAMPLE, so a parameter outside the range still poses.";
		return ToolResult::ok(std::move(out));
	};
	registry.add(std::move(t));
}

void addBlendSpaceSet(McpToolRegistry& registry, ContentManager& content,
                      const std::shared_ptr<McpAnimatorHooks>& h)
{
	ContentManager* cm = &content;
	McpTool t;
	t.name        = "blendspace_set";
	t.description =
		"Change a Blend Space's own settings: whether it is 1D or 2D, which parameters "
		"its axes read, the axis ranges of the diagram and whether the shared phase "
		"loops. Everything the call does not mention keeps its value. The samples "
		"themselves are blendspace_sample_set.";
	t.inputSchema = objectSchema(json{
		{ "path",    stringProp("Content-relative path of the Blend Space asset.") },
		{ "name",    stringProp("Display name, shown in the inspector.") },
		{ "kind",    stringProp("'1d' (only the x axis is read) or '2d'.") },
		{ "paramX",  stringProp("Name of the state machine parameter the x axis reads. "
		                        "Empty = the axis reads 0.") },
		{ "paramY",  stringProp("Name of the parameter the y axis reads (2D only).") },
		{ "minX",    numberProp("Left end of the diagram's x axis.") },
		{ "maxX",    numberProp("Right end of the diagram's x axis.") },
		{ "minY",    numberProp("Bottom end of the y axis (2D only).") },
		{ "maxY",    numberProp("Top end of the y axis (2D only).") },
		{ "looping", json{ { "type", "boolean" },
		                   { "description", "Whether the shared phase wraps at 1." } } },
	}, { "path" });
	t.mutates = true;
	t.handler = [cm, h](const json& args) -> ToolResult {
		Doc d = openDoc(*cm, *h, args, /*forWrite=*/true, HE::AssetType::BlendSpace);
		if (!d.ok) return d.failure;

		if (hasArg(args, "kind"))
		{
			const std::string k = strArg(args, "kind");
			if (k == "1d")      d.space.kind = BlendSpaceKind::OneD;
			else if (k == "2d") d.space.kind = BlendSpaceKind::TwoD;
			else return ToolResult::fail("invalid_payload",
				"'" + k + "' is not a blend space kind. One of: 1d, 2d.");
		}
		if (hasArg(args, "name"))    d.space.name   = strArg(args, "name");
		if (hasArg(args, "paramX"))  d.space.paramX = strArg(args, "paramX");
		if (hasArg(args, "paramY"))  d.space.paramY = strArg(args, "paramY");
		if (hasArg(args, "minX"))    d.space.minX = static_cast<float>(numArg(args, "minX", 0.0));
		if (hasArg(args, "maxX"))    d.space.maxX = static_cast<float>(numArg(args, "maxX", 1.0));
		if (hasArg(args, "minY"))    d.space.minY = static_cast<float>(numArg(args, "minY", 0.0));
		if (hasArg(args, "maxY"))    d.space.maxY = static_cast<float>(numArg(args, "maxY", 1.0));
		if (hasArg(args, "looping")) d.space.looping = boolArg(args, "looping", true);

		json out{
			{ "kind",        kindName(d.space.kind) },
			{ "paramX",      d.space.paramX },
			{ "sampleCount", static_cast<int>(d.space.samples.size()) },
		};
		if (d.space.kind == BlendSpaceKind::TwoD) out["paramY"] = d.space.paramY;
		return writeDoc(*cm, *h, d, std::move(out));
	};
	registry.add(std::move(t));
}

void addBlendSpaceSampleSet(McpToolRegistry& registry, ContentManager& content,
                            const std::shared_ptr<McpAnimatorHooks>& h)
{
	ContentManager* cm = &content;
	McpTool t;
	t.name        = "blendspace_sample_set";
	t.description =
		"Place a clip in a Blend Space, or move one that is already placed. Without "
		"'index' a new sample is appended; with one, that sample is changed and "
		"everything the call does not mention keeps its value. A sample has no id — two "
		"of them may name the same clip at different speeds — so the index blendspace_"
		"info reports is the address.";
	t.inputSchema = objectSchema(json{
		{ "path",       stringProp("Content-relative path of the Blend Space asset.") },
		{ "index",      numberProp("Which sample to change (0 = first). Omit to append a "
		                           "new one.") },
		{ "clip",       stringProp("Content-relative path of the Animation Clip to place. "
		                           "Required for a NEW sample.") },
		{ "x",          numberProp("Where on the x axis it sits.") },
		{ "y",          numberProp("Where on the y axis (2D spaces only).") },
		{ "speedScale", numberProp("Per-sample time scale, 1 = as authored. It scales this "
		                           "sample's share of the blended duration, not its own "
		                           "playhead — every sample runs on one shared phase.") },
	}, { "path" });
	t.mutates = true;
	t.handler = [cm, h](const json& args) -> ToolResult {
		Doc d = openDoc(*cm, *h, args, /*forWrite=*/true, HE::AssetType::BlendSpace);
		if (!d.ok) return d.failure;

		// Resolved before the space is touched — a bad path must change nothing.
		HE::UUID clipId{};
		ToolResult fail = ToolResult::ok(json::object());
		const bool wantClip = hasArg(args, "clip");
		if (wantClip && !resolveRef(*cm, args, "clip", HE::AssetType::AnimationClip, clipId, fail))
			return fail;

		const bool append = !hasArg(args, "index");
		int index = intArg(args, "index", -1);
		if (!append)
		{
			if (index < 0 || index >= static_cast<int>(d.space.samples.size()))
				return ToolResult::fail("not_found",
					"'" + d.rel + "' has " + std::to_string(d.space.samples.size()) +
					" sample(s), so there is no sample " + std::to_string(index) +
					". blendspace_info reports the indices.");
		}
		else
		{
			if (!wantClip || clipId == HE::UUID{})
				return ToolResult::fail("invalid_payload",
					"'clip' is required for a new sample — a sample without a clip places "
					"nothing and is weighed against the ones that do.");
			d.space.samples.push_back(BlendSpaceSample{});
			index = static_cast<int>(d.space.samples.size()) - 1;
		}

		BlendSpaceSample& sm = d.space.samples[static_cast<std::size_t>(index)];
		if (wantClip)                    sm.clipId     = clipId;
		if (hasArg(args, "x"))           sm.x          = static_cast<float>(numArg(args, "x", 0.0));
		if (hasArg(args, "y"))           sm.y          = static_cast<float>(numArg(args, "y", 0.0));
		if (hasArg(args, "speedScale"))  sm.speedScale = static_cast<float>(numArg(args, "speedScale", 1.0));

		RefNames names(*cm);
		json out{
			{ "index",       index },
			{ "created",     append },
			{ "sampleCount", static_cast<int>(d.space.samples.size()) },
			{ "sample",      sampleJson(names, d.space, static_cast<std::size_t>(index)) },
		};
		if (hasArg(args, "y") && d.space.kind != BlendSpaceKind::TwoD)
			out["note"] =
				"This space is 1D, so 'y' was written but is never read. blendspace_set with "
				"kind '2d' makes the second axis count.";
		return writeDoc(*cm, *h, d, std::move(out));
	};
	registry.add(std::move(t));
}

void addBlendSpaceSampleRemove(McpToolRegistry& registry, ContentManager& content,
                               const std::shared_ptr<McpAnimatorHooks>& h)
{
	ContentManager* cm = &content;
	McpTool t;
	t.name        = "blendspace_sample_remove";
	t.description =
		"Take a clip back out of a Blend Space. The samples after it MOVE DOWN one "
		"index, because the index is a position in a list and nothing else — read "
		"blendspace_info again before addressing another one.";
	t.inputSchema = objectSchema(json{
		{ "path",  stringProp("Content-relative path of the Blend Space asset.") },
		{ "index", numberProp("Which sample to remove (0 = first).") },
	}, { "path", "index" });
	t.mutates = true;
	t.handler = [cm, h](const json& args) -> ToolResult {
		Doc d = openDoc(*cm, *h, args, /*forWrite=*/true, HE::AssetType::BlendSpace);
		if (!d.ok) return d.failure;

		const int index = intArg(args, "index", -1);
		if (index < 0 || index >= static_cast<int>(d.space.samples.size()))
			return ToolResult::fail("not_found",
				"'" + d.rel + "' has " + std::to_string(d.space.samples.size()) +
				" sample(s), so there is no sample " + std::to_string(index) +
				". blendspace_info reports the indices.");
		d.space.samples.erase(d.space.samples.begin() + index);

		return writeDoc(*cm, *h, d, json{
			{ "removed",     index },
			{ "sampleCount", static_cast<int>(d.space.samples.size()) },
		});
	};
	registry.add(std::move(t));
}

} // namespace

void registerAnimatorTools(McpToolRegistry& registry, ContentManager& content,
                           McpAnimatorHooks hooks)
{
	// Shared rather than copied into each handler, like the material, prefab and
	// type tools: the hooks hold std::functions that capture the editor, and one
	// copy per handler would be one chance per handler to let one go stale.
	auto h = std::make_shared<McpAnimatorHooks>(std::move(hooks));
	addAnimatorInfo(registry, content, h);
	addStateSet(registry, content, h);
	addStateRemove(registry, content, h);
	addTransitionSet(registry, content, h);
	addTransitionRemove(registry, content, h);
	addParamSet(registry, content, h);
	addParamRemove(registry, content, h);
	addBlendSpaceInfo(registry, content, h);
	addBlendSpaceSet(registry, content, h);
	addBlendSpaceSampleSet(registry, content, h);
	addBlendSpaceSampleRemove(registry, content, h);
}

} // namespace HE::Ed
