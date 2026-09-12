#include "doctest.h"

#include "AssetStubWriter.h"
#include "EditorAssetTypeCache.h"
#include "McpToolRegistry.h"
#include "TestFsUtil.h"

#include <ContentManager/AssetRefScan.h>
#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/HAsset.h>
#include <ParticleGraph/ParticleGraph.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

// ─── Authoring an emitter from outside the editor ────────────────────────────
// The claim these tools make that nothing else can make for them: THE EMITTER
// THE SIMULATION GETS AFTERWARDS IS THE ONE THAT WAS ASKED FOR. A particle asset
// is a node graph, and "Emit Rate = 25" is a Const node wired to pin 0 — so a
// test that only re-read the tool's own answer would be testing the answer, not
// the emitter.
//
// So the load-bearing tests here read the FILE back, parse it with
// HE::particleGraphFromJson and run HE::evaluateParticleGraph over it: the exact
// path ParticleSystem takes. The rest are the places where a shortcut would look
// green and be wrong later:
//
//   • a fresh asset has NO graph chunk at all (AssetStubWriter writes none), so
//     everything has to start from makeDefault() the way the panel does,
//   • the boundary between "a value" and "the graph": a Random Range or a shared
//     constant is refused rather than quietly detached,
//   • a value is read in the shape its pin has, and a wrong shape is a refusal
//     instead of a coercion,
//   • a refusal leaves the file byte for byte as it was,
//   • the live emitters are invalidated, which is the half of the panel's Save
//     that is not the file.

using HE::Ed::McpParticleHooks;
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
		       ("he_test_mcp_particle_" + name + "_" + std::to_string(::rand()));
		fs::create_directories(root);
		content.setContentRoot(root.string());
		EditorAssetTypeCache::invalidateAll();

		McpParticleHooks h;
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
		HE::Ed::registerParticleTools(registry, content, std::move(h));
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

	// An asset as the Content Browser's create menu makes one: the editor's own
	// stub writer, so a newborn file here is the newborn file there — including
	// the part that matters, that a ParticleSystem stub carries NO graph chunk.
	void writeStub(const std::string& rel, HE::AssetType type)
	{
		const fs::path abs = root / rel;
		fs::create_directories(abs.parent_path());
		REQUIRE(HE::Ed::writeAssetStub(abs.string(), rel, fs::path(rel).stem().string(), type));
		EditorAssetTypeCache::invalidate(abs.string());
	}

	// Put an authored graph into an existing asset, the same way the panel's Save
	// does — for the shapes these tools must REFUSE and could not create.
	void putGraph(const std::string& rel, const HE::ParticleGraph& g)
	{
		const HE::UUID id = content.loadAsset(rel);
		REQUIRE(!(id == HE::UUID{}));
		ParticleGraphAsset* a = content.getParticleGraphMutable(id);
		REQUIRE(a != nullptr);
		a->nodeGraphJson = HE::particleGraphToJson(g);
		REQUIRE(content.saveAsset(*a));
	}

	std::string bytes(const std::string& rel) const
	{
		std::ifstream f(root / rel, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	}

	// What the FILE says, through the same parser the runtime uses.
	HE::ParticleGraph graphOnDisk(const std::string& rel) const
	{
		HAsset::Reader r;
		REQUIRE(r.open((root / rel).string()));
		const HAsset::Reader::Chunk* c = r.findChunk(HAsset::CHUNK_PTGR);
		REQUIRE(c != nullptr);
		const std::string payload(reinterpret_cast<const char*>(c->data.data()), c->data.size());
		HE::ParticleGraph g;
		REQUIRE(HE::particleGraphFromJson(payload, g));
		return g;
	}

	// …and what the simulation would emit from it.
	HE::ParticleEmitterConfig emitterOnDisk(const std::string& rel) const
	{
		std::mt19937 rng{ 7 };
		return HE::evaluateParticleGraph(graphOnDisk(rel), rng);
	}
};

const json* findPin(const json& pins, const std::string& name)
{
	for (const json& p : pins)
		if (p.value("pin", std::string()) == name) return &p;
	return nullptr;
}

} // namespace

// ─── Reading ─────────────────────────────────────────────────────────────────

TEST_CASE("particle_info lists the project's emitters and reads one")
{
	Fixture f("info");
	f.writeStub("Effects/Smoke.hasset", HE::AssetType::ParticleSystem);
	f.writeStub("Effects/Fire.hasset", HE::AssetType::ParticleSystem);
	f.writeStub("Effects/NotAnEmitter.hasset", HE::AssetType::Material);

	const ToolResult all = f.call("particle_info");
	REQUIRE_MESSAGE(!all.isError, codeOf(all));
	REQUIRE(all.content["emitters"].size() == 2);
	// Sorted by path, so two calls on an unchanged project answer identically.
	CHECK(all.content["emitters"][0]["path"] == "Effects/Fire.hasset");
	// A newborn asset has no graph chunk at all — what the panel shows for one is
	// makeDefault(), a lone Emitter Output, and that is what this must report.
	CHECK(all.content["emitters"][0]["nodeCount"] == 1);
	CHECK(all.content["emitters"][0]["linkCount"] == 0);

	const ToolResult one = f.call("particle_info", json{ { "path", "Effects/Smoke.hasset" } });
	REQUIRE_MESSAGE(!one.isError, codeOf(one));
	CHECK(one.content["pins"].size() == HE::kParticleEmitterPinCount);

	const json* rate = findPin(one.content["pins"], "Emit Rate");
	REQUIRE(rate != nullptr);
	CHECK((*rate)["drivenBy"] == "default");
	CHECK((*rate)["settable"] == true);
	CHECK((*rate)["value"].get<float>() == doctest::Approx(10.0f));
	// A flag pin reads as a boolean, not as the float it is on disk.
	const json* loop = findPin(one.content["pins"], "Looping");
	REQUIRE(loop != nullptr);
	CHECK((*loop)["type"] == "flag");
	CHECK((*loop)["value"] == true);
	// The evaluated emitter — the POD the simulation consumes, not the graph.
	CHECK(one.content["emitter"]["emitRate"].get<float>() == doctest::Approx(10.0f));
	CHECK(one.content["emitter"]["maxParticles"] == 100);
}

TEST_CASE("particle_info refuses what is not an emitter, and says which tool is")
{
	Fixture f("info_refuse");
	f.writeStub("Materials/Rock.hasset", HE::AssetType::Material);
	const ToolResult wrong = f.call("particle_info", json{ { "path", "Materials/Rock.hasset" } });
	CHECK(wrong.isError);
	CHECK(wrong.errorCode == "invalid_path");

	const ToolResult missing = f.call("particle_info", json{ { "path", "Effects/Ghost.hasset" } });
	CHECK(missing.isError);
	CHECK(missing.errorCode == "not_found");
}

// ─── Setting a value ─────────────────────────────────────────────────────────

TEST_CASE("particle_set wires a constant into an unconnected pin, and the emitter follows")
{
	Fixture f("set_new");
	f.writeStub("Effects/Smoke.hasset", HE::AssetType::ParticleSystem);

	const ToolResult r = f.call("particle_set", json{
		{ "path", "Effects/Smoke.hasset" }, { "pin", "Emit Rate" }, { "value", 25.0 } });
	REQUIRE_MESSAGE(!r.isError, codeOf(r));
	CHECK(r.content["createdNode"] == true);

	// The claim, asked of the FILE through the runtime's own parser and evaluator.
	const HE::ParticleEmitterConfig cfg = f.emitterOnDisk("Effects/Smoke.hasset");
	CHECK(cfg.emitRate == doctest::Approx(25.0f));
	const HE::ParticleGraph g = f.graphOnDisk("Effects/Smoke.hasset");
	CHECK(g.nodes.size() == 2);
	CHECK(g.links.size() == 1);
	// The live emitters were invalidated — the half of the panel's Save that is
	// not the file.
	CHECK(f.graphChanged == 1);
}

TEST_CASE("particle_set changes a constant it already owns in place")
{
	Fixture f("set_again");
	f.writeStub("Effects/Smoke.hasset", HE::AssetType::ParticleSystem);
	f.call("particle_set", json{
		{ "path", "Effects/Smoke.hasset" }, { "pin", "Emit Rate" }, { "value", 25.0 } });

	const ToolResult r = f.call("particle_set", json{
		{ "path", "Effects/Smoke.hasset" }, { "pin", "Emit Rate" }, { "value", 4.0 } });
	REQUIRE_MESSAGE(!r.isError, codeOf(r));
	CHECK(r.content["createdNode"] == false);

	const HE::ParticleGraph g = f.graphOnDisk("Effects/Smoke.hasset");
	CHECK(g.nodes.size() == 2);   // no second constant piled onto the same pin
	CHECK(g.links.size() == 1);
	CHECK(f.emitterOnDisk("Effects/Smoke.hasset").emitRate == doctest::Approx(4.0f));
}

TEST_CASE("particle_set writes each value in the shape its pin has")
{
	Fixture f("set_shapes");
	f.writeStub("Effects/Smoke.hasset", HE::AssetType::ParticleSystem);
	const std::string p = "Effects/Smoke.hasset";

	REQUIRE(!f.call("particle_set", json{ { "path", p }, { "pin", "Start Color" },
	                                      { "value", json::array({ 1.0, 0.0, 0.0 }) } }).isError);
	REQUIRE(!f.call("particle_set", json{ { "path", p }, { "pin", "Gravity" },
	                                      { "value", json::array({ 0.0, -9.0, 0.0 }) } }).isError);
	REQUIRE(!f.call("particle_set", json{ { "path", p }, { "pin", "Looping" },
	                                      { "value", false } }).isError);

	const HE::ParticleEmitterConfig cfg = f.emitterOnDisk(p);
	CHECK(cfg.startColor[0] == doctest::Approx(1.0f));
	CHECK(cfg.startColor[1] == doctest::Approx(0.0f));
	CHECK(cfg.gravity[1] == doctest::Approx(-9.0f));
	CHECK(cfg.looping == false);

	// A colour pin gets the node the panel puts there — a Const Vec3 would be an
	// emitter whose colour cannot be picked.
	const HE::ParticleGraph g = f.graphOnDisk(p);
	bool haveColorNode = false;
	for (const HE::ParticleGraphNode& n : g.nodes)
		if (n.type == HE::ParticleNodeType::ConstColor) haveColorNode = true;
	CHECK(haveColorNode);
}

TEST_CASE("particle_set refuses a wrong shape rather than coercing it, and writes nothing")
{
	Fixture f("set_shape_refuse");
	f.writeStub("Effects/Smoke.hasset", HE::AssetType::ParticleSystem);
	const std::string p = "Effects/Smoke.hasset";
	const std::string before = f.bytes(p);

	const ToolResult vec = f.call("particle_set", json{
		{ "path", p }, { "pin", "Gravity" }, { "value", 3.0 } });
	CHECK(vec.isError);
	CHECK(vec.errorCode == "invalid_payload");

	const ToolResult num = f.call("particle_set", json{
		{ "path", p }, { "pin", "Emit Rate" }, { "value", "fast" } });
	CHECK(num.isError);
	CHECK(num.errorCode == "invalid_payload");

	const ToolResult pin = f.call("particle_set", json{
		{ "path", p }, { "pin", "Emit Rat" }, { "value", 3.0 } });
	CHECK(pin.isError);
	CHECK(pin.errorCode == "invalid_payload");
	CHECK(pin.errorMessage.find("Emit Rate") != std::string::npos);  // refused WITH the list

	const ToolResult nothing = f.call("particle_set", json{ { "path", p }, { "pin", "Emit Rate" } });
	CHECK(nothing.isError);

	CHECK(f.bytes(p) == before);   // a refusal is a no-op
	CHECK(f.graphChanged == 0);
}

TEST_CASE("particle_set refuses a pin the graph itself drives")
{
	Fixture f("set_driven");
	f.writeStub("Effects/Smoke.hasset", HE::AssetType::ParticleSystem);
	const std::string p = "Effects/Smoke.hasset";

	// A Random Range on Emit Rate, and ONE constant feeding two pins — the two
	// shapes these tools must not silently rewire.
	HE::ParticleGraph g = HE::ParticleGraph::makeDefault();
	const int out = g.nodes.front().id;
	const int rnd = g.addNode(HE::ParticleNodeType::RandomRange, 0, 0);
	REQUIRE(g.connect(rnd, 0, out, HE::kParticleEmitRatePin));
	const int shared = g.addNode(HE::ParticleNodeType::ConstFloat, 0, 0);
	REQUIRE(g.connect(shared, 0, out, HE::kParticleStartSizePin));
	REQUIRE(g.connect(shared, 0, out, HE::kParticleEndSizePin));
	f.putGraph(p, g);
	const std::string before = f.bytes(p);

	const ToolResult rnge = f.call("particle_set", json{
		{ "path", p }, { "pin", "Emit Rate" }, { "value", 5.0 } });
	CHECK(rnge.isError);
	CHECK(rnge.errorCode == "driven_by_graph");

	const ToolResult sh = f.call("particle_set", json{
		{ "path", p }, { "pin", "Start Size" }, { "value", 5.0 } });
	CHECK(sh.isError);
	CHECK(sh.errorCode == "driven_by_graph");

	CHECK(f.bytes(p) == before);

	// And the reading tool says so BEFORE the client tries.
	const ToolResult info = f.call("particle_info", json{ { "path", p } });
	REQUIRE_MESSAGE(!info.isError, codeOf(info));
	CHECK((*findPin(info.content["pins"], "Emit Rate"))["settable"] == false);
	CHECK((*findPin(info.content["pins"], "Start Size"))["settable"] == false);
	CHECK((*findPin(info.content["pins"], "Lifetime Min"))["settable"] == true);
}

TEST_CASE("particle_set reset takes a pin back to its default and removes the constant")
{
	Fixture f("reset");
	f.writeStub("Effects/Smoke.hasset", HE::AssetType::ParticleSystem);
	const std::string p = "Effects/Smoke.hasset";
	f.call("particle_set", json{ { "path", p }, { "pin", "Emit Rate" }, { "value", 25.0 } });

	const ToolResult r = f.call("particle_set", json{
		{ "path", p }, { "pin", "Emit Rate" }, { "reset", true } });
	REQUIRE_MESSAGE(!r.isError, codeOf(r));
	CHECK(r.content["removedNode"] == true);

	const HE::ParticleGraph g = f.graphOnDisk(p);
	CHECK(g.nodes.size() == 1);
	CHECK(g.links.empty());
	CHECK(f.emitterOnDisk(p).emitRate == doctest::Approx(10.0f));   // the registry default

	// A Random Range is NOT removed by a reset — only unwired. Removing an
	// author's node because a pin was reset would be an edit nobody asked for.
	HE::ParticleGraph g2 = HE::ParticleGraph::makeDefault();
	const int out = g2.nodes.front().id;
	const int rnd = g2.addNode(HE::ParticleNodeType::RandomRange, 0, 0);
	REQUIRE(g2.connect(rnd, 0, out, HE::kParticleEmitRatePin));
	f.putGraph(p, g2);
	const ToolResult r2 = f.call("particle_set", json{
		{ "path", p }, { "pin", "Emit Rate" }, { "reset", true } });
	REQUIRE_MESSAGE(!r2.isError, codeOf(r2));
	CHECK(r2.content["removedNode"] == false);
	CHECK(f.graphOnDisk(p).nodes.size() == 2);
}

// ─── The mesh and material slots ─────────────────────────────────────────────

TEST_CASE("particle_slot_set stores the referenced asset's own id")
{
	Fixture f("slot");
	f.writeStub("Effects/Smoke.hasset", HE::AssetType::ParticleSystem);
	f.writeStub("Meshes/Quad.hasset", HE::AssetType::StaticMesh);
	f.writeStub("Materials/Smoke.hasset", HE::AssetType::Material);
	const std::string p = "Effects/Smoke.hasset";

	const ToolResult mesh = f.call("particle_slot_set", json{
		{ "path", p }, { "slot", "mesh" }, { "assetPath", "Meshes/Quad.hasset" } });
	REQUIRE_MESSAGE(!mesh.isError, codeOf(mesh));

	const HE::UUID want = HE::AssetRefs::assetUuidOfFile((f.root / "Meshes/Quad.hasset").string());
	const HE::ParticleGraph g = f.graphOnDisk(p);
	REQUIRE(!g.nodes.empty());
	CHECK(g.nodes.front().meshAssetId == want);

	// A material where a mesh belongs is a reference the emitter cannot draw with.
	const ToolResult wrong = f.call("particle_slot_set", json{
		{ "path", p }, { "slot", "mesh" }, { "assetPath", "Materials/Smoke.hasset" } });
	CHECK(wrong.isError);
	CHECK(wrong.errorCode == "invalid_payload");

	const ToolResult badSlot = f.call("particle_slot_set", json{
		{ "path", p }, { "slot", "texture" }, { "assetPath", "Materials/Smoke.hasset" } });
	CHECK(badSlot.isError);

	// An empty path clears it.
	const ToolResult clear = f.call("particle_slot_set", json{ { "path", p }, { "slot", "mesh" } });
	REQUIRE_MESSAGE(!clear.isError, codeOf(clear));
	CHECK(clear.content["cleared"] == true);
	CHECK(f.graphOnDisk(p).nodes.front().meshAssetId == HE::UUID{});
}

// ─── The four gates ──────────────────────────────────────────────────────────

TEST_CASE("particle_set refuses while play runs, while a peer holds it, and while a tab is dirty")
{
	Fixture f("gates");
	f.writeStub("Effects/Smoke.hasset", HE::AssetType::ParticleSystem);
	const std::string p = "Effects/Smoke.hasset";
	const std::string before = f.bytes(p);
	const json args{ { "path", p }, { "pin", "Emit Rate" }, { "value", 25.0 } };

	f.playing = true;
	CHECK(f.call("particle_set", args).errorCode == "play_mode");
	f.playing = false;

	f.lockedRel = p;
	CHECK(f.call("particle_set", args).errorCode == "locked_by_other");
	f.lockedRel.clear();

	f.dirtyRel = p;
	CHECK(f.call("particle_set", args).errorCode == "dirty");
	f.dirtyRel.clear();

	CHECK(f.bytes(p) == before);
	CHECK(f.graphChanged == 0);

	// A CLEAN tab is told to re-read instead — the same reload a peer's change
	// goes through.
	f.openRel = p;
	const ToolResult ok = f.call("particle_set", args);
	REQUIRE_MESSAGE(!ok.isError, codeOf(ok));
	CHECK(ok.content["reloadedInEditor"] == true);
	CHECK(f.reloadCalls == 1);
}
