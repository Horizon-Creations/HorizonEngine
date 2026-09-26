#include "doctest.h"

#include "AssetStubWriter.h"
#include "EditorAssetTypeCache.h"
#include "McpToolRegistry.h"
#include "TestFsUtil.h"

#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/HAsset.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// ─── Authoring a cutscene from outside the editor ────────────────────────────
// The claim these tools make that nothing else can make for them: THE CUTSCENE
// A FRESH ContentManager LOADS AFTERWARDS IS THE ONE THAT WAS WRITTEN — with the
// translated references pointing where the client said (an entity uuid as
// entity_list prints it, a clip and a sound by path, a target and a curve by
// name). So the load-bearing test reads the file back the way the runtime does
// rather than believing the tool's own answer.
//
// The rest are the places a shortcut would look green and be wrong later:
//   • what sequence_info reports writes back unchanged,
//   • every refusal leaves the file byte for byte as it was,
//   • a clean open tab is told to re-read, a dirty one refuses the write,
//   • a binding is looked up in the scene by uuid and a cut to a non-camera is
//     called out.

using HE::Ed::McpSequenceHooks;
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

std::string hexOf(const HE::UUID& id)
{
	char buf[33];
	std::snprintf(buf, sizeof(buf), "%016llx%016llx",
	              static_cast<unsigned long long>(id.hi), static_cast<unsigned long long>(id.lo));
	return buf;
}

struct Fixture
{
	fs::path        root;
	ContentManager  content;
	McpToolRegistry registry;

	bool        playing = false;
	std::string lockedRel;
	std::string dirtyRel;
	std::vector<std::string> reloaded;
	bool        withScene = true;

	// The "scene": uuid → (name, is it a camera).
	struct Actor { HE::UUID id; std::string name; bool camera; };
	std::vector<Actor> scene;

	explicit Fixture(const std::string& name, bool scene = true)
		: withScene(scene)
	{
		root = fs::temp_directory_path() /
		       ("he_test_mcp_seq_" + name + "_" + std::to_string(::rand()));
		fs::create_directories(root);
		content.setContentRoot(root.string());
		EditorAssetTypeCache::invalidateAll();
		registerTools();
	}

	void registerTools()
	{
		McpSequenceHooks h;
		h.isPlaying     = [this] { return playing; };
		h.lockedByOther = [this](const std::string& rel) { return !lockedRel.empty() && rel == lockedRel; };
		h.isDirty       = [this](const std::string& rel) { return !dirtyRel.empty() && rel == dirtyRel; };
		h.reloadFromDisk = [this](const std::string& rel) { reloaded.push_back(rel); return true; };
		if (withScene)
			h.findActor = [this](const HE::UUID& id, std::string& n, bool& cam) {
				for (const Actor& a : scene)
					if (a.id == id) { n = a.name; cam = a.camera; return true; }
				return false;
			};
		HE::Ed::registerSequenceTools(registry, content, std::move(h));
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

	void writeStub(const std::string& rel)
	{
		const fs::path abs = root / rel;
		fs::create_directories(abs.parent_path());
		REQUIRE(HE::Ed::writeAssetStub(abs.string(), rel, fs::path(rel).stem().string(),
		                               HE::AssetType::Sequence));
		EditorAssetTypeCache::invalidate(abs.string());
	}

	// An imported asset as far as a sequence can tell: the header's type and a
	// META chunk carrying its uuid. Nothing here loads it, which is the point.
	HE::UUID writeRef(const std::string& rel, HE::AssetType type)
	{
		const fs::path abs = root / rel;
		fs::create_directories(abs.parent_path());
		const HE::UUID id = HE::UUID::generate();
		std::vector<uint8_t> meta;
		HAsset::Writer::appendPOD(meta, static_cast<uint16_t>(type));
		HAsset::Writer::appendPOD(meta, id.hi);
		HAsset::Writer::appendPOD(meta, id.lo);
		HAsset::Writer::appendString(meta, fs::path(rel).stem().string());
		HAsset::Writer::appendString(meta, rel);
		HAsset::Writer w;
		w.addChunk(HAsset::CHUNK_META, meta.data(), meta.size());
		REQUIRE(w.write(abs.string(), static_cast<uint16_t>(type)));
		EditorAssetTypeCache::invalidate(abs.string());
		return id;
	}

	std::string bytes(const std::string& rel) const
	{
		std::ifstream f(root / rel, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	}

	// What the FILE holds, read the way the runtime reads it.
	SequenceAsset loadFresh(const std::string& rel) const
	{
		ContentManager fresh;
		fresh.setContentRoot(root.string());
		const HE::UUID id = fresh.loadAsset(rel);
		REQUIRE(id != HE::UUID{});
		const SequenceAsset* s = fresh.getSequence(id);
		REQUIRE(s != nullptr);
		return *s;
	}
};

// A cutscene touching every kind of track, in the tool vocabulary.
json sampleDoc(const HE::UUID& hero, const HE::UUID& cam)
{
	return json{
		{ "duration", 6.0 }, { "frameRate", 24.0 },
		{ "bindings", json::array({
			json{ { "slot", 0 }, { "name", "Hero" },   { "entity", hexOf(hero) } },
			json{ { "slot", 1 }, { "name", "CamA" },   { "entity", hexOf(cam) } } }) },
		{ "tracks", json::array({
			json{ { "kind", "cameraCut" }, { "cuts", json::array({
				json{ { "time", 2.0 }, { "binding", 1 }, { "blendIn", 0.5 }, { "curve", "easeOut" } },
				json{ { "time", 0.0 }, { "binding", 1 }, { "curve", "linear" } } }) } },
			json{ { "kind", "property" }, { "binding", 0 }, { "target", "Position Y" },
			      { "times", json::array({ 0.0, 3.0 }) }, { "values", json::array({ 0.0, 2.5 }) } },
			json{ { "kind", "property" }, { "binding", 1 }, { "target", "Field of View" },
			      { "times", json::array({ 1.0 }) }, { "values", json::array({ 40.0 }) } },
			json{ { "kind", "skeletal" }, { "binding", 0 }, { "sections", json::array({
				json{ { "clip", "Anim/Wave.hasset" }, { "start", 0.5 }, { "end", 4.0 },
				      { "playRate", 1.5 }, { "loop", true } } }) } },
			json{ { "kind", "event" }, { "events", json::array({
				json{ { "name", "Door" }, { "time", 5.0 } } }) } },
			json{ { "kind", "audio" }, { "binding", 0 }, { "sections", json::array({
				json{ { "sound", "Sfx/Boom.hasset" }, { "start", 5.5 }, { "volume", 0.8 } } }) } } }) },
	};
}

} // namespace

TEST_CASE("sequence_write lands the cutscene a fresh ContentManager loads, references translated")
{
	Fixture f("roundtrip");
	f.writeStub("Cine/Intro.hasset");
	const HE::UUID clip  = f.writeRef("Anim/Wave.hasset", HE::AssetType::AnimationClip);
	const HE::UUID sound = f.writeRef("Sfx/Boom.hasset", HE::AssetType::Audio);
	const HE::UUID hero  = HE::UUID::generate();
	const HE::UUID cam   = HE::UUID::generate();

	const ToolResult r = f.call("sequence_write",
		json{ { "path", "Cine/Intro.hasset" }, { "sequence", sampleDoc(hero, cam) } });
	REQUIRE_MESSAGE(codeOf(r) == "<ok>", r.content.dump());
	CHECK(r.content["written"] == true);

	const SequenceAsset s = f.loadFresh("Cine/Intro.hasset");
	CHECK(s.duration == doctest::Approx(6.0f));
	CHECK(s.frameRate == doctest::Approx(24.0f));
	REQUIRE(s.bindings.size() == 2);
	CHECK(s.bindings[0].entityId == hero);
	CHECK(s.bindings[1].entityId == cam);
	REQUIRE(s.tracks.size() == 6);

	// The cuts come back in time order (the loader sorts), curves by enum.
	REQUIRE(s.tracks[0].kind == SequenceTrackKind::CameraCut);
	REQUIRE(s.tracks[0].cuts.size() == 2);
	CHECK(s.tracks[0].cuts[0].time == doctest::Approx(0.0f));
	CHECK(s.tracks[0].cuts[0].curve == SequenceBlendCurve::Linear);
	CHECK(s.tracks[0].cuts[1].curve == SequenceBlendCurve::EaseOut);
	CHECK(s.tracks[0].cuts[1].blendIn == doctest::Approx(0.5f));

	CHECK(s.tracks[1].channel.target == PropTarget::PosY);
	CHECK(s.tracks[2].channel.target == PropTarget::CameraFov);
	REQUIRE(s.tracks[3].sections.size() == 1);
	CHECK(s.tracks[3].sections[0].clipId == clip);
	CHECK(s.tracks[3].sections[0].playRate == doctest::Approx(1.5f));
	CHECK(s.tracks[3].sections[0].loop);
	CHECK(s.tracks[4].binding == kSequenceNoBinding);
	REQUIRE(s.tracks[5].audio.size() == 1);
	CHECK(s.tracks[5].audio[0].assetId == sound);
	CHECK(s.tracks[5].audio[0].volume == doctest::Approx(0.8f));
}

TEST_CASE("sequence_info reports the tool form, and writing it back changes nothing")
{
	Fixture f("readback");
	f.writeStub("Cine/Intro.hasset");
	f.writeRef("Anim/Wave.hasset", HE::AssetType::AnimationClip);
	f.writeRef("Sfx/Boom.hasset", HE::AssetType::Audio);
	const HE::UUID hero = HE::UUID::generate();
	const HE::UUID cam  = HE::UUID::generate();
	REQUIRE(codeOf(f.call("sequence_write",
		json{ { "path", "Cine/Intro.hasset" }, { "sequence", sampleDoc(hero, cam) } })) == "<ok>");

	const ToolResult info = f.call("sequence_info", json{ { "path", "Cine/Intro.hasset" } });
	REQUIRE(codeOf(info) == "<ok>");
	const json& doc = info.content["sequence"];
	CHECK(doc["bindings"][0]["entity"] == hexOf(hero));
	CHECK_FALSE(doc["bindings"][0].contains("entityId"));
	CHECK(doc["tracks"][1]["target"] == "Position Y");
	CHECK(doc["tracks"][0]["cuts"][1]["curve"] == "easeOut");
	CHECK(doc["tracks"][3]["sections"][0]["clip"] == "Anim/Wave.hasset");
	CHECK(doc["tracks"][5]["sections"][0]["sound"] == "Sfx/Boom.hasset");
	CHECK_FALSE(info.content.contains("unresolvedAssets"));
	CHECK(info.content["propertyTargets"].size() == static_cast<std::size_t>(kLastPropTarget) + 1);

	// Read → write unchanged → read: the same document.
	REQUIRE(codeOf(f.call("sequence_write",
		json{ { "path", "Cine/Intro.hasset" }, { "sequence", doc } })) == "<ok>");
	const ToolResult again = f.call("sequence_info", json{ { "path", "Cine/Intro.hasset" } });
	CHECK(again.content["sequence"] == doc);

	// A clip whose file is gone stays writable: reported as its uuid, taken back.
	fs::remove(f.root / "Anim/Wave.hasset");
	EditorAssetTypeCache::invalidateAll();
	const ToolResult orphan = f.call("sequence_info", json{ { "path", "Cine/Intro.hasset" } });
	REQUIRE(orphan.content.contains("unresolvedAssets"));
	CHECK(orphan.content["unresolvedAssets"].size() == 1);
	const json od = orphan.content["sequence"];
	CHECK(od["tracks"][3]["sections"][0].contains("clipId"));
	CHECK(codeOf(f.call("sequence_write",
		json{ { "path", "Cine/Intro.hasset" }, { "sequence", od } })) == "<ok>");
}

TEST_CASE("sequence_info reads a fresh stub as empty and lists the catalogue")
{
	Fixture f("stub");
	f.writeStub("Cine/A.hasset");
	f.writeStub("Cine/B.hasset");
	f.writeRef("Anim/Wave.hasset", HE::AssetType::AnimationClip);

	const ToolResult one = f.call("sequence_info", json{ { "path", "Cine/A.hasset" } });
	REQUIRE(codeOf(one) == "<ok>");
	CHECK(one.content["sequence"]["bindings"].empty());
	CHECK(one.content["sequence"]["tracks"].empty());

	const ToolResult all = f.call("sequence_info");
	REQUIRE(codeOf(all) == "<ok>");
	REQUIRE(all.content["sequences"].size() == 2);
	CHECK(all.content["sequences"][0]["path"] == "Cine/A.hasset");
	CHECK(all.content["sequences"][1]["trackCount"] == 0);

	// Not a sequence at all.
	CHECK(codeOf(f.call("sequence_info", json{ { "path", "Anim/Wave.hasset" } })) == "invalid_path");
}

TEST_CASE("sequence_write refuses what the runtime would get silently wrong, and writes nothing")
{
	Fixture f("refuse");
	f.writeStub("Cine/Intro.hasset");
	f.writeRef("Anim/Wave.hasset", HE::AssetType::AnimationClip);
	f.writeRef("Sfx/Boom.hasset", HE::AssetType::Audio);
	const HE::UUID hero = HE::UUID::generate();
	const HE::UUID cam  = HE::UUID::generate();
	REQUIRE(codeOf(f.call("sequence_write",
		json{ { "path", "Cine/Intro.hasset" }, { "sequence", sampleDoc(hero, cam) } })) == "<ok>");
	const std::string before = f.bytes("Cine/Intro.hasset");
	f.reloaded.clear();

	auto refused = [&](json doc, const char* why) {
		CAPTURE(why);
		const ToolResult r = f.call("sequence_write",
			json{ { "path", "Cine/Intro.hasset" }, { "sequence", std::move(doc) } });
		CHECK(codeOf(r) == "invalid_args");
		CHECK_FALSE(r.errorMessage.empty());
	};

	json d = sampleDoc(hero, cam);
	d["bindings"][1]["slot"] = 0;
	refused(d, "two bindings share a slot");

	d = sampleDoc(hero, cam);
	d["tracks"][1]["binding"] = 7;
	refused(d, "a track names a slot nobody has");

	d = sampleDoc(hero, cam);
	d["tracks"][1]["times"] = json::array({ 3.0, 1.0 });
	refused(d, "keys out of order");

	d = sampleDoc(hero, cam);
	d["tracks"].push_back(json{ { "kind", "cameraCut" }, { "cuts", json::array() } });
	refused(d, "a second camera-cut track");

	d = sampleDoc(hero, cam);
	d["duration"] = 4.0;
	refused(d, "content after the end (event at 5, sound at 5.5)");

	d = sampleDoc(hero, cam);
	d["tracks"][1]["target"] = "Position W";
	refused(d, "unknown target name");

	d = sampleDoc(hero, cam);
	d["tracks"][3]["sections"][0]["clip"] = "Sfx/Boom.hasset";
	refused(d, "a sound where a clip belongs");

	d = sampleDoc(hero, cam);
	d["tracks"][3]["sections"][0]["clip"] = "Anim/Missing.hasset";
	refused(d, "a clip path with no file");

	d = sampleDoc(hero, cam);
	d["bindings"][0]["entity"] = "1234";
	refused(d, "an entity uuid with digits missing");

	d = sampleDoc(hero, cam);
	d["tracks"].push_back(json{ { "kind", "particles" } });
	refused(d, "a track kind this build does not know (the loader would drop it)");

	d = sampleDoc(hero, cam);
	d["tracks"][1]["values"] = json::array({ 1.0 });
	refused(d, "times and values of different length (the loader would drop it)");

	d = sampleDoc(hero, cam);
	d["tracks"][1].erase("binding");
	refused(d, "a property track without an actor");

	CHECK(f.bytes("Cine/Intro.hasset") == before);
	CHECK(f.reloaded.empty());
}

TEST_CASE("sequence_write asks the four gates and tells a clean tab to re-read")
{
	Fixture f("gates");
	f.writeStub("Cine/Intro.hasset");
	const json add{ { "path", "Cine/Intro.hasset" },
	                { "sequence", json{ { "duration", 2.0 }, { "tracks", json::array({
	                    json{ { "kind", "event" }, { "events", json::array({
	                        json{ { "name", "Go" }, { "time", 1.0 } } }) } } }) } } } };
	const std::string before = f.bytes("Cine/Intro.hasset");

	f.playing = true;
	CHECK(codeOf(f.call("sequence_write", add)) == "play_mode");
	f.playing = false;

	f.lockedRel = "Cine/Intro.hasset";
	CHECK(codeOf(f.call("sequence_write", add)) == "locked_by_other");
	f.lockedRel.clear();

	f.dirtyRel = "Cine/Intro.hasset";
	CHECK(codeOf(f.call("sequence_write", add)) == "dirty");
	f.dirtyRel.clear();

	CHECK(f.bytes("Cine/Intro.hasset") == before);
	CHECK(f.reloaded.empty());

	CHECK(codeOf(f.call("sequence_write", json{ { "path", "Engine/Cine/X.hasset" },
	                                          { "sequence", json::object() } })) != "<ok>");

	const ToolResult ok = f.call("sequence_write", add);
	REQUIRE(codeOf(ok) == "<ok>");
	REQUIRE(f.reloaded.size() == 1);
	CHECK(f.reloaded[0] == "Cine/Intro.hasset");
	CHECK(ok.content["tabReloaded"] == true);
	CHECK(f.loadFresh("Cine/Intro.hasset").tracks.size() == 1);
}

TEST_CASE("sequence_info looks each binding up in the open scene by uuid")
{
	Fixture f("actors");
	f.writeStub("Cine/Intro.hasset");
	f.writeRef("Anim/Wave.hasset", HE::AssetType::AnimationClip);
	f.writeRef("Sfx/Boom.hasset", HE::AssetType::Audio);
	const HE::UUID hero = HE::UUID::generate();
	const HE::UUID cam  = HE::UUID::generate();
	REQUIRE(codeOf(f.call("sequence_write",
		json{ { "path", "Cine/Intro.hasset" }, { "sequence", sampleDoc(hero, cam) } })) == "<ok>");

	// The cut camera is in the scene but is no camera; the hero is missing.
	f.scene.push_back({ cam, "Not A Camera", false });
	json actors = f.call("sequence_info", json{ { "path", "Cine/Intro.hasset" } }).content["actors"];
	REQUIRE(actors.size() == 2);
	CHECK(actors[0]["found"] == false);
	CHECK(actors[1]["found"] == true);
	CHECK(actors[1]["sceneName"] == "Not A Camera");
	CHECK(actors[1].contains("warning"));

	f.scene.back().camera = true;
	actors = f.call("sequence_info", json{ { "path", "Cine/Intro.hasset" } }).content["actors"];
	CHECK_FALSE(actors[1].contains("warning"));
	CHECK(actors[1]["isCamera"] == true);
}

TEST_CASE("sequence_info without a scene says it could not look, not that actors are missing")
{
	Fixture f("noscene", /*scene=*/false);
	f.writeStub("Cine/Intro.hasset");
	const ToolResult r = f.call("sequence_info", json{ { "path", "Cine/Intro.hasset" } });
	REQUIRE(codeOf(r) == "<ok>");
	CHECK_FALSE(r.content.contains("actors"));
	CHECK(r.content.contains("scene"));
}
