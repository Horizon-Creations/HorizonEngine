#include "doctest.h"

#include "AssetStubWriter.h"
#include "EditorAssetTypeCache.h"
#include "McpToolRegistry.h"
#include "TestFsUtil.h"

#include <AnimatorStateMachine/AnimatorStateMachineGraph.h>
#include <BlendSpace/BlendSpace.h>
#include <ContentManager/AssetRefScan.h>
#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/HAsset.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// ─── Authoring the animation assets from outside the editor ──────────────────
// The claim these tools make that nothing else can make for them: A NAME THAT
// MOVES TAKES EVERYTHING THAT POINTED AT IT ALONG. A transition names its
// endpoint states by NAME and its parameter by NAME, and nothing in the format
// ties either back — so every reference is a reference only as long as somebody
// maintains it. That somebody was the editor panel; now it is these tools too.
//
// So the load-bearing tests read the FILE back through
// HE::animatorStateMachineFromJson — the runtime's own parser — and ask what the
// transitions say afterwards. The rest are the places where a shortcut would
// look green and be wrong later:
//
//   • a fresh asset has NO payload chunk at all (AssetStubWriter writes none),
//   • a parameter a transition names cannot vanish by accident: the removal is
//     refused, because a transition whose parameter the live map does not hold
//     compares as false every frame with nothing logged,
//   • a state's pose source is ONE thing: setting a blend space clears the clip,
//     because the blend space wins and a clip left behind is never read,
//   • a sync graph (CHUNK_ASSY) the tools never touch survives their writes,
//   • a refusal leaves the file byte for byte as it was.

using HE::Ed::McpAnimatorHooks;
using HE::Ed::McpTool;
using HE::Ed::McpToolRegistry;
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
	int         reloadCalls  = 0;
	int         graphChanged = 0;

	explicit Fixture(const std::string& name)
	{
		root = fs::temp_directory_path() /
		       ("he_test_mcp_anim_" + name + "_" + std::to_string(::rand()));
		fs::create_directories(root);
		content.setContentRoot(root.string());
		EditorAssetTypeCache::invalidateAll();

		McpAnimatorHooks h;
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
		h.onGraphChanged = [this](const std::string&) { ++graphChanged; };
		HE::Ed::registerAnimatorTools(registry, content, std::move(h));
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

	std::string chunkOnDisk(const std::string& rel, std::uint32_t chunk) const
	{
		HAsset::Reader r;
		if (!r.open((root / rel).string())) return {};
		const HAsset::Reader::Chunk* c = r.findChunk(chunk);
		if (!c) return {};
		return std::string(reinterpret_cast<const char*>(c->data.data()), c->data.size());
	}

	// What the FILE says, through the same parser the runtime uses.
	HE::AnimatorStateMachineGraph graphOnDisk(const std::string& rel) const
	{
		HE::AnimatorStateMachineGraph g;
		const std::string payload = chunkOnDisk(rel, HAsset::CHUNK_ASMG);
		REQUIRE(!payload.empty());
		REQUIRE(HE::animatorStateMachineFromJson(payload, g));
		return g;
	}

	HE::BlendSpace spaceOnDisk(const std::string& rel) const
	{
		HE::BlendSpace s;
		const std::string payload = chunkOnDisk(rel, HAsset::CHUNK_BLSP);
		REQUIRE(!payload.empty());
		REQUIRE(HE::blendSpaceFromJson(payload, s));
		return s;
	}

	// Give an asset a sync graph, which these tools never write and must never
	// drop: the panel's Save keeps it, and so must a tool's.
	void putSyncGraph(const std::string& rel, const std::string& json)
	{
		const HE::UUID id = content.loadAsset(rel);
		REQUIRE(!(id == HE::UUID{}));
		AnimatorStateMachineAsset* a = content.getAnimatorStateMachineMutable(id);
		REQUIRE(a != nullptr);
		a->syncGraphJson = json;
		REQUIRE(content.saveAsset(*a));
	}

	// A machine with two states and one transition, made with the tools
	// themselves — every test below starts from a machine, not from a file.
	std::string twoStates(const std::string& rel = "Animation/Player.hasset")
	{
		writeStub(rel, HE::AssetType::AnimatorStateMachine);
		REQUIRE(!call("animator_state_set", json{ { "path", rel }, { "name", "Idle" } }).isError);
		REQUIRE(!call("animator_state_set", json{ { "path", rel }, { "name", "Run" } }).isError);
		REQUIRE(!call("animator_param_set",
		              json{ { "path", rel }, { "name", "Speed" }, { "value", 0.0 } }).isError);
		REQUIRE(!call("animator_transition_set",
		              json{ { "path", rel }, { "from", "Idle" }, { "to", "Run" },
		                    { "param", "Speed" }, { "threshold", 0.2 } }).isError);
		return rel;
	}
};

} // namespace

// ─── Reading ─────────────────────────────────────────────────────────────────

TEST_CASE("animator_info lists the project's machines and reads one")
{
	Fixture f("info");
	const std::string p = f.twoStates();
	f.writeStub("Animation/Empty.hasset", HE::AssetType::AnimatorStateMachine);
	f.writeStub("Animation/NotAMachine.hasset", HE::AssetType::Material);

	const ToolResult all = f.call("animator_info");
	REQUIRE_MESSAGE(!all.isError, codeOf(all));
	REQUIRE(all.content["stateMachines"].size() == 2);
	// Sorted by path, so two calls on an unchanged project answer identically.
	CHECK(all.content["stateMachines"][0]["path"] == "Animation/Empty.hasset");
	CHECK(all.content["stateMachines"][0]["stateCount"] == 0);
	CHECK(all.content["stateMachines"][1]["stateCount"] == 2);

	const ToolResult one = f.call("animator_info", json{ { "path", p } });
	REQUIRE_MESSAGE(!one.isError, codeOf(one));
	CHECK(one.content["states"].size() == 2);
	CHECK(one.content["transitions"].size() == 1);
	CHECK(one.content["transitions"][0]["op"] == "greater");
	CHECK(one.content["params"][0]["name"] == "Speed");
	// The name the runtime will actually enter, not just the field — an empty
	// startState means the first state.
	CHECK(one.content["startState"] == "Idle");
	CHECK(one.content["startStateExplicit"] == false);
}

// ─── The rename fix-up ───────────────────────────────────────────────────────

TEST_CASE("animator_state_set renames a state and takes its transitions with it")
{
	Fixture f("rename");
	const std::string p = f.twoStates();
	REQUIRE(!f.call("animator_state_set",
	                json{ { "path", p }, { "name", "Idle" }, { "start", true } }).isError);

	const ToolResult r = f.call("animator_state_set",
	                            json{ { "path", p }, { "name", "Idle" }, { "newName", "Stand" } });
	REQUIRE_MESSAGE(!r.isError, codeOf(r));
	CHECK(r.content["renamedFrom"] == "Idle");
	CHECK(r.content["retargetedEndpoints"] == 1);

	// The claim, asked of the FILE: the transition still connects two states that
	// exist, and the start state moved with the name.
	const HE::AnimatorStateMachineGraph g = f.graphOnDisk(p);
	REQUIRE(g.transitions.size() == 1);
	CHECK(g.transitions[0].fromState == "Stand");
	CHECK(g.transitions[0].toState == "Run");
	CHECK(g.startState == "Stand");
	// …and the state kept its identity rather than being replaced.
	REQUIRE(g.states.size() == 2);

	// A rename onto a name that is taken is refused: two states with one name
	// make every transition naming it ambiguous.
	const ToolResult clash = f.call("animator_state_set",
	                                json{ { "path", p }, { "name", "Run" }, { "newName", "Stand" } });
	CHECK(clash.isError);
	CHECK(clash.errorCode == "invalid_payload");
	CHECK(f.graphOnDisk(p).states.size() == 2);
}

TEST_CASE("animator_state_remove cascades the transitions and the start state")
{
	Fixture f("state_remove");
	const std::string p = f.twoStates();
	REQUIRE(!f.call("animator_state_set",
	                json{ { "path", p }, { "name", "Idle" }, { "start", true } }).isError);

	const ToolResult r = f.call("animator_state_remove", json{ { "path", p }, { "name", "Idle" } });
	REQUIRE_MESSAGE(!r.isError, codeOf(r));
	CHECK(r.content["removedTransitions"] == 1);
	CHECK(r.content["clearedStartState"] == true);

	const HE::AnimatorStateMachineGraph g = f.graphOnDisk(p);
	CHECK(g.states.size() == 1);
	CHECK(g.transitions.empty());   // no endpoint left naming a state that is gone
	CHECK(g.startState.empty());

	const ToolResult missing = f.call("animator_state_remove", json{ { "path", p }, { "name", "Nope" } });
	CHECK(missing.isError);
	CHECK(missing.errorCode == "not_found");
	CHECK(missing.errorMessage.find("Run") != std::string::npos);   // refused WITH the list
}

// ─── Transitions ─────────────────────────────────────────────────────────────

TEST_CASE("animator_transition_set addresses a transition by its triple")
{
	Fixture f("transitions");
	const std::string p = f.twoStates();

	// The same triple: an update, not a second transition.
	const ToolResult upd = f.call("animator_transition_set",
	                              json{ { "path", p }, { "from", "Idle" }, { "to", "Run" },
	                                    { "param", "Speed" }, { "op", "less" } });
	REQUIRE_MESSAGE(!upd.isError, codeOf(upd));
	CHECK(upd.content["created"] == false);
	{
		const HE::AnimatorStateMachineGraph g = f.graphOnDisk(p);
		REQUIRE(g.transitions.size() == 1);
		CHECK(g.transitions[0].op == HE::TransitionOp::Less);
		// Everything the call did not mention kept its value.
		CHECK(g.transitions[0].threshold == doctest::Approx(0.2f));
	}

	// A different parameter between the same two states is a SECOND transition —
	// which is the whole reason the triple is the address.
	REQUIRE(!f.call("animator_param_set", json{ { "path", p }, { "name", "Jump" } }).isError);
	const ToolResult second = f.call("animator_transition_set",
	                                 json{ { "path", p }, { "from", "Idle" }, { "to", "Run" },
	                                       { "param", "Jump" } });
	REQUIRE_MESSAGE(!second.isError, codeOf(second));
	CHECK(second.content["created"] == true);
	CHECK(f.graphOnDisk(p).transitions.size() == 2);

	// An endpoint that does not exist is refused: the runtime skips such a
	// transition without a word.
	const ToolResult dangling = f.call("animator_transition_set",
	                                   json{ { "path", p }, { "from", "Idle" }, { "to", "Fly" },
	                                         { "param", "Speed" } });
	CHECK(dangling.isError);
	CHECK(dangling.errorCode == "not_found");

	const ToolResult badOp = f.call("animator_transition_set",
	                                json{ { "path", p }, { "from", "Idle" }, { "to", "Run" },
	                                      { "param", "Speed" }, { "op", "greaterthan" } });
	CHECK(badOp.isError);
	CHECK(badOp.errorCode == "invalid_payload");

	const ToolResult rm = f.call("animator_transition_remove",
	                             json{ { "path", p }, { "from", "Idle" }, { "to", "Run" },
	                                   { "param", "Jump" } });
	REQUIRE_MESSAGE(!rm.isError, codeOf(rm));
	CHECK(f.graphOnDisk(p).transitions.size() == 1);
}

TEST_CASE("a transition on an undeclared parameter is reported, not refused")
{
	Fixture f("undeclared");
	const std::string p = f.twoStates();

	// Not refused: a script or the sync graph may write a parameter that was
	// never declared as a default — the live map is open. What would be wrong is
	// letting it pass in silence.
	const ToolResult r = f.call("animator_transition_set",
	                            json{ { "path", p }, { "from", "Run" }, { "to", "Idle" },
	                                  { "param", "Tired" } });
	REQUIRE_MESSAGE(!r.isError, codeOf(r));
	CHECK(r.content.contains("note"));
	CHECK(r.content["transition"]["undeclaredParam"] == true);

	const ToolResult info = f.call("animator_info", json{ { "path", p } });
	bool marked = false;
	for (const json& t : info.content["transitions"])
		if (t.value("param", std::string()) == "Tired") marked = t.value("undeclaredParam", false);
	CHECK(marked);
}

// ─── The parameter question ──────────────────────────────────────────────────

TEST_CASE("animator_param_remove refuses while a transition names the parameter")
{
	Fixture f("param_remove");
	const std::string p = f.twoStates();
	const std::string before = f.bytes(p);

	const ToolResult refused = f.call("animator_param_remove", json{ { "path", p }, { "name", "Speed" } });
	CHECK(refused.isError);
	CHECK(refused.errorCode == "in_use");
	REQUIRE(refused.content["usedBy"].size() == 1);
	CHECK(refused.content["usedBy"][0]["from"] == "Idle");
	CHECK(f.bytes(p) == before);   // a refusal is a no-op

	// force is the deliberate way through, and it says that it was forced.
	const ToolResult forced = f.call("animator_param_remove",
	                                 json{ { "path", p }, { "name", "Speed" }, { "force", true } });
	REQUIRE_MESSAGE(!forced.isError, codeOf(forced));
	CHECK(forced.content["forced"] == true);
	CHECK(f.graphOnDisk(p).defaultParams.empty());
	// The transition survives — force means "I know", not "clean up after me".
	CHECK(f.graphOnDisk(p).transitions.size() == 1);

	const ToolResult missing = f.call("animator_param_remove", json{ { "path", p }, { "name", "Speed" } });
	CHECK(missing.isError);
	CHECK(missing.errorCode == "not_found");
}

TEST_CASE("animator_param_set declares a parameter and keeps its value on a re-set")
{
	Fixture f("param_set");
	const std::string p = f.twoStates();

	const ToolResult r = f.call("animator_param_set",
	                            json{ { "path", p }, { "name", "Speed" }, { "value", 3.5 } });
	REQUIRE_MESSAGE(!r.isError, codeOf(r));
	CHECK(r.content["created"] == false);
	CHECK(r.content["usedBy"].size() == 1);
	CHECK(f.graphOnDisk(p).defaultParams.at("Speed") == doctest::Approx(3.5f));

	// A call without a value keeps the one that is there rather than resetting it
	// to zero behind the caller's back.
	REQUIRE(!f.call("animator_param_set", json{ { "path", p }, { "name", "Speed" } }).isError);
	CHECK(f.graphOnDisk(p).defaultParams.at("Speed") == doctest::Approx(3.5f));
}

// ─── The pose source ─────────────────────────────────────────────────────────

TEST_CASE("a state poses from one thing: a blend space clears the clip")
{
	Fixture f("pose_source");
	const std::string p = f.twoStates();
	f.writeStub("Animation/Idle.hasset", HE::AssetType::AnimationClip);
	f.writeStub("Animation/Locomotion.hasset", HE::AssetType::BlendSpace);
	f.writeStub("Materials/Rock.hasset", HE::AssetType::Material);

	REQUIRE(!f.call("animator_state_set",
	                json{ { "path", p }, { "name", "Idle" },
	                      { "clip", "Animation/Idle.hasset" } }).isError);
	const HE::UUID clipId =
		HE::AssetRefs::assetUuidOfFile((f.root / "Animation/Idle.hasset").string());
	{
		const HE::AnimatorStateMachineGraph g = f.graphOnDisk(p);
		CHECK(g.states[0].clipId == clipId);
	}

	const ToolResult space = f.call("animator_state_set",
	                                json{ { "path", p }, { "name", "Idle" },
	                                      { "blendSpace", "Animation/Locomotion.hasset" } });
	REQUIRE_MESSAGE(!space.isError, codeOf(space));
	CHECK(space.content["state"]["poseSource"] == "blendSpace");
	// The clip is CLEARED rather than left behind: a set blend space wins, so a
	// clip that stays is a value on disk that nothing ever reads.
	const HE::AnimatorStateMachineGraph g = f.graphOnDisk(p);
	CHECK(g.states[0].clipId == HE::UUID{});
	CHECK(!(g.states[0].blendSpaceId == HE::UUID{}));
	// …and the info tool names the reference back, not just its id.
	const ToolResult info = f.call("animator_info", json{ { "path", p } });
	CHECK(info.content["states"][0]["blendSpace"]["path"] == "Animation/Locomotion.hasset");

	// A material is not something a state can pose from.
	const ToolResult wrong = f.call("animator_state_set",
	                                json{ { "path", p }, { "name", "Idle" },
	                                      { "clip", "Materials/Rock.hasset" } });
	CHECK(wrong.isError);
	CHECK(wrong.errorCode == "invalid_payload");
	// Both at once is refused rather than silently resolved.
	const ToolResult both = f.call("animator_state_set",
	                               json{ { "path", p }, { "name", "Idle" },
	                                     { "clip", "Animation/Idle.hasset" },
	                                     { "blendSpace", "Animation/Locomotion.hasset" } });
	CHECK(both.isError);
}

// ─── The chunk nobody here writes ────────────────────────────────────────────

TEST_CASE("a write keeps the sync graph the tools never touch")
{
	Fixture f("sync_graph");
	const std::string p = f.twoStates();
	f.putSyncGraph(p, "{\"nodes\":[],\"links\":[],\"marker\":42}");

	REQUIRE(!f.call("animator_param_set", json{ { "path", p }, { "name", "Fear" } }).isError);

	// Going through the loaded asset rather than writing the file directly is
	// what keeps it — saveAsset rewrites both chunks out of the asset.
	CHECK(f.chunkOnDisk(p, HAsset::CHUNK_ASSY).find("42") != std::string::npos);
}

// ─── The four gates ──────────────────────────────────────────────────────────

TEST_CASE("animator writes refuse while play runs, while a peer holds it, and while a tab is dirty")
{
	Fixture f("gates");
	const std::string p = f.twoStates();
	const std::string before = f.bytes(p);
	const json args{ { "path", p }, { "name", "Fear" } };
	const int changedBefore = f.graphChanged;

	f.playing = true;
	CHECK(f.call("animator_param_set", args).errorCode == "play_mode");
	f.playing = false;

	f.lockedRel = p;
	CHECK(f.call("animator_param_set", args).errorCode == "locked_by_other");
	f.lockedRel.clear();

	f.dirtyRel = p;
	CHECK(f.call("animator_param_set", args).errorCode == "dirty");
	f.dirtyRel.clear();

	CHECK(f.bytes(p) == before);
	CHECK(f.graphChanged == changedBefore);

	f.openRel = p;
	const ToolResult ok = f.call("animator_param_set", args);
	REQUIRE_MESSAGE(!ok.isError, codeOf(ok));
	CHECK(ok.content["reloadedInEditor"] == true);
	CHECK(f.reloadCalls == 1);
	CHECK(f.graphChanged == changedBefore + 1);   // the live animators were told
}

// ─── The blend space ─────────────────────────────────────────────────────────

TEST_CASE("blendspace_set and the samples")
{
	Fixture f("blendspace");
	const std::string p = "Animation/Locomotion.hasset";
	f.writeStub(p, HE::AssetType::BlendSpace);
	f.writeStub("Animation/Walk.hasset", HE::AssetType::AnimationClip);
	f.writeStub("Animation/Run.hasset", HE::AssetType::AnimationClip);

	const ToolResult set = f.call("blendspace_set", json{
		{ "path", p }, { "kind", "1d" }, { "paramX", "Speed" }, { "maxX", 6.0 } });
	REQUIRE_MESSAGE(!set.isError, codeOf(set));
	{
		const HE::BlendSpace s = f.spaceOnDisk(p);
		CHECK(s.paramX == "Speed");
		CHECK(s.maxX == doctest::Approx(6.0f));
		CHECK(s.kind == HE::BlendSpaceKind::OneD);
	}

	// A new sample needs a clip — one without places nothing and is still weighed
	// against the ones that do.
	const ToolResult noClip = f.call("blendspace_sample_set", json{ { "path", p }, { "x", 1.0 } });
	CHECK(noClip.isError);

	REQUIRE(!f.call("blendspace_sample_set",
	                json{ { "path", p }, { "clip", "Animation/Walk.hasset" }, { "x", 1.0 } }).isError);
	const ToolResult second = f.call("blendspace_sample_set",
	                                 json{ { "path", p }, { "clip", "Animation/Run.hasset" },
	                                       { "x", 5.0 }, { "speedScale", 1.5 } });
	REQUIRE_MESSAGE(!second.isError, codeOf(second));
	CHECK(second.content["index"] == 1);

	{
		const HE::BlendSpace s = f.spaceOnDisk(p);
		REQUIRE(s.samples.size() == 2);
		CHECK(s.samples[1].x == doctest::Approx(5.0f));
		CHECK(s.samples[1].speedScale == doctest::Approx(1.5f));
		CHECK(s.samples[1].clipId ==
		      HE::AssetRefs::assetUuidOfFile((f.root / "Animation/Run.hasset").string()));
	}

	// Addressed by index, and everything the call does not mention keeps its
	// value.
	REQUIRE(!f.call("blendspace_sample_set",
	                json{ { "path", p }, { "index", 0 }, { "x", 2.0 } }).isError);
	{
		const HE::BlendSpace s = f.spaceOnDisk(p);
		CHECK(s.samples[0].x == doctest::Approx(2.0f));
		CHECK(s.samples[0].clipId ==
		      HE::AssetRefs::assetUuidOfFile((f.root / "Animation/Walk.hasset").string()));
	}

	const ToolResult ghost = f.call("blendspace_sample_set", json{ { "path", p }, { "index", 7 } });
	CHECK(ghost.isError);
	CHECK(ghost.errorCode == "not_found");

	const ToolResult info = f.call("blendspace_info", json{ { "path", p } });
	REQUIRE_MESSAGE(!info.isError, codeOf(info));
	CHECK(info.content["samples"].size() == 2);
	CHECK(info.content["samples"][0]["clip"]["path"] == "Animation/Walk.hasset");
	// A 1D space does not report a y a client could not use.
	CHECK(!info.content["samples"][0].contains("y"));

	REQUIRE(!f.call("blendspace_sample_remove", json{ { "path", p }, { "index", 0 } }).isError);
	{
		const HE::BlendSpace s = f.spaceOnDisk(p);
		REQUIRE(s.samples.size() == 1);
		// The one that was second moved down — the index is a position, nothing more.
		CHECK(s.samples[0].x == doctest::Approx(5.0f));
	}

	// A blend space is NOT a state machine, and the refusal says which tool is.
	const ToolResult wrongFamily = f.call("animator_info", json{ { "path", p } });
	CHECK(wrongFamily.isError);
	CHECK(wrongFamily.errorCode == "invalid_path");
	// A blend space edit deliberately does not invalidate live entities — the
	// panel's own Save does not either.
	CHECK(f.graphChanged == 0);
}
