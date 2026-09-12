#include "doctest.h"

#include "AssetStubWriter.h"
#include "EditorAssetTypeCache.h"
#include "McpToolRegistry.h"
#include "TestFsUtil.h"

#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/HAsset.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// ─── Authoring what an animation announces, from outside the editor ──────────
// The claim these tools make that nothing else can make for them: THE CLIP THE
// SIMULATION READS AFTERWARDS CARRIES THE EVENTS THAT WERE ASKED FOR — and still
// carries its keyframes. That second half is the whole risk of this family:
// `saveAsset` writes CHUNK_ANIM back out of the LOADED clip, so every write here
// rewrites the import too, and a notify that cost a walk cycle its samples would
// be a far worse bug than no notify tool at all.
//
// So the load-bearing tests load the file back through a FRESH ContentManager —
// the path the runtime takes, channels and all — rather than believing the tool's
// own answer. The rest are the places where a shortcut would look green and be
// wrong later:
//
//   • a clip imported before notifies existed has no ANOT chunk at all, and has
//     to read as an empty timeline rather than as a broken file,
//   • an index shifts when something before it is removed, so the answer has to
//     carry the new ones,
//   • a time outside the clip is refused, not clamped — a notify past the end
//     never fires,
//   • only the fields that were sent change,
//   • a refusal leaves the file byte for byte as it was.

using HE::Ed::McpClipHooks;
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

	explicit Fixture(const std::string& name)
	{
		root = fs::temp_directory_path() /
		       ("he_test_mcp_clip_" + name + "_" + std::to_string(::rand()));
		fs::create_directories(root);
		content.setContentRoot(root.string());
		EditorAssetTypeCache::invalidateAll();

		McpClipHooks h;
		h.isPlaying     = [this] { return playing; };
		h.lockedByOther = [this](const std::string& rel) {
			return !lockedRel.empty() && rel == lockedRel;
		};
		h.isDirty = [this](const std::string& rel) {
			return !dirtyRel.empty() && rel == dirtyRel;
		};
		HE::Ed::registerClipTools(registry, content, std::move(h));
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

	// A clip as an IMPORT makes one — by hand, because there is no other way: a
	// clip is the one asset in this family that `writeAssetStub` refuses, which is
	// the very fact §16.2 of the plan doc read as "not editable".
	//
	// `withNotifyChunk=false` writes the file an older build wrote: CHUNK_ANIM and
	// no CHUNK_ANOT at all.
	void writeClip(const std::string& rel, float duration, int channels,
	               bool rootMotion, const std::vector<AnimationNotify>& notifies,
	               bool withNotifyChunk = true)
	{
		const fs::path abs = root / rel;
		fs::create_directories(abs.parent_path());

		const HE::UUID id = HE::UUID::generate();
		HAsset::Writer w;

		std::vector<uint8_t> meta;
		HAsset::Writer::appendPOD(meta, static_cast<uint16_t>(HE::AssetType::AnimationClip));
		HAsset::Writer::appendPOD(meta, id.hi);
		HAsset::Writer::appendPOD(meta, id.lo);
		HAsset::Writer::appendString(meta, fs::path(rel).stem().string());
		HAsset::Writer::appendString(meta, rel);
		w.addChunk(HAsset::CHUNK_META, meta.data(), meta.size());

		std::vector<uint8_t> anim;
		HAsset::Writer::appendPOD(anim, duration);
		HAsset::Writer::appendPOD(anim, static_cast<uint32_t>(channels));
		for (int c = 0; c < channels; ++c)
		{
			// One joint, one key: enough that "the samples survived a notify edit"
			// is a question with an answer.
			HAsset::Writer::appendPOD(anim, static_cast<int32_t>(c));
			HAsset::Writer::appendPOD(anim, static_cast<uint8_t>(AnimPathType::Translation));
			HAsset::Writer::appendVec(anim, std::vector<float>{ 0.0f, duration });
			HAsset::Writer::appendVec(anim,
				std::vector<float>{ 0.0f, 0.0f, 0.0f, 1.0f, 2.0f, 3.0f });
		}
		w.addChunk(HAsset::CHUNK_ANIM, anim.data(), anim.size());

		if (withNotifyChunk)
		{
			std::vector<uint8_t> anot;
			HAsset::Writer::appendPOD(anot, static_cast<uint8_t>(rootMotion ? 1 : 0));
			HAsset::Writer::appendPOD(anot, static_cast<uint32_t>(notifies.size()));
			for (const AnimationNotify& n : notifies)
			{
				HAsset::Writer::appendString(anot, n.name);
				HAsset::Writer::appendPOD(anot, n.time);
				HAsset::Writer::appendPOD(anot, n.duration);
			}
			w.addChunk(HAsset::CHUNK_ANOT, anot.data(), anot.size());
		}

		REQUIRE(w.write(abs.string(), static_cast<uint16_t>(HE::AssetType::AnimationClip)));
		EditorAssetTypeCache::invalidate(abs.string());
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
};

// What the FILE holds, read the way the runtime reads it: a ContentManager that
// has never seen this asset, so nothing in-memory can answer for it. Returned by
// value because the pool is a dense vector and the manager dies with the call.
AnimationClipAsset clipOnDisk(const fs::path& root, const std::string& rel)
{
	ContentManager fresh;
	fresh.setContentRoot(root.string());
	const HE::UUID id = fresh.loadAsset(rel);
	REQUIRE(!(id == HE::UUID{}));
	const AnimationClipAsset* c = fresh.getAnimationClip(id);
	REQUIRE(c != nullptr);
	return *c;
}

const json* notifyAt(const json& list, int index)
{
	for (const json& n : list)
		if (n.value("index", -1) == index) return &n;
	return nullptr;
}

} // namespace

// ─── Reading ─────────────────────────────────────────────────────────────────

TEST_CASE("clip_info lists the project's clips and reads one")
{
	Fixture f("info");
	f.writeClip("Anim/Run.hasset", 1.5f, 2, /*rootMotion=*/true,
	            { { "Footstep_L", 0.25f, 0.0f }, { "Window", 0.5f, 0.2f } });
	f.writeClip("Anim/Idle.hasset", 3.0f, 1, /*rootMotion=*/false, {});

	const ToolResult all = f.call("clip_info");
	REQUIRE(codeOf(all) == "<ok>");
	REQUIRE(all.content["clips"].size() == 2);
	// Sorted, so the catalogue of an unchanged project answers identically twice.
	CHECK(all.content["clips"][0]["path"] == "Anim/Idle.hasset");
	CHECK(all.content["clips"][1]["notifyCount"] == 2);
	CHECK(all.content["clips"][1]["rootMotion"] == true);
	CHECK(all.content["clips"][0]["duration"].get<float>() == doctest::Approx(3.0f));

	const ToolResult one = f.call("clip_info", json{ { "path", "Anim/Run.hasset" } });
	REQUIRE(codeOf(one) == "<ok>");
	CHECK(one.content["duration"].get<float>() == doctest::Approx(1.5f));
	CHECK(one.content["channelCount"] == 2);
	CHECK(one.content["rootMotion"] == true);
	REQUIRE(one.content["notifies"].size() == 2);

	const json* first = notifyAt(one.content["notifies"], 0);
	REQUIRE(first != nullptr);
	CHECK((*first)["name"] == "Footstep_L");
	CHECK((*first)["time"].get<float>() == doctest::Approx(0.25f));
	// The form is reported rather than left for a client to infer from a number.
	CHECK((*first)["kind"] == "notify");
	const json* second = notifyAt(one.content["notifies"], 1);
	REQUIRE(second != nullptr);
	CHECK((*second)["kind"] == "notifyState");

	// Said outright, so a client does not discover by a failing write that the
	// keyframes are the import.
	REQUIRE(one.content.contains("editable"));
	CHECK(one.content["editable"].size() == 2);
}

TEST_CASE("a clip from before notifies existed reads as an empty timeline")
{
	Fixture f("nochunk");
	f.writeClip("Anim/Old.hasset", 2.0f, 1, /*rootMotion=*/false, {},
	            /*withNotifyChunk=*/false);

	const ToolResult r = f.call("clip_info", json{ { "path", "Anim/Old.hasset" } });
	REQUIRE(codeOf(r) == "<ok>");
	CHECK(r.content["notifies"].empty());
	CHECK(r.content["rootMotion"] == false);
	CHECK(r.content["duration"].get<float>() == doctest::Approx(2.0f));

	// And it is editable: the chunk is written on the first save, not required
	// to be there before it.
	const ToolResult set = f.call("clip_notify_set",
		json{ { "path", "Anim/Old.hasset" }, { "name", "Hit" }, { "time", 1.0f } });
	REQUIRE(codeOf(set) == "<ok>");
	CHECK(clipOnDisk(f.root, "Anim/Old.hasset").notifies.size() == 1);
}

TEST_CASE("clip_info refuses what is not a clip, and says a clip cannot be made")
{
	Fixture f("wrongtype");
	f.writeStub("Effects/Smoke.hasset", HE::AssetType::ParticleSystem);

	const ToolResult r = f.call("clip_info", json{ { "path", "Effects/Smoke.hasset" } });
	CHECK(codeOf(r) == "invalid_path");
	CHECK(r.errorMessage.find("asset_resolve") != std::string::npos);
	CHECK(r.errorMessage.find("import") != std::string::npos);

	// A path that holds nothing is a different mistake from a path that holds the
	// wrong thing, and the confinement rule keeps them apart: `not_found` for the
	// first, `invalid_path` for the second and for an escape attempt.
	CHECK(codeOf(f.call("clip_info", json{ { "path", "Anim/Nope.hasset" } })) == "not_found");
	CHECK(codeOf(f.call("clip_info", json{ { "path", "../Outside.hasset" } })) == "invalid_path");
}

// ─── Writing events ──────────────────────────────────────────────────────────

TEST_CASE("clip_notify_set appends an event and the clip keeps its samples")
{
	Fixture f("append");
	f.writeClip("Anim/Run.hasset", 1.0f, 3, /*rootMotion=*/false, {});

	const ToolResult r = f.call("clip_notify_set",
		json{ { "path", "Anim/Run.hasset" }, { "name", "Footstep_R" }, { "time", 0.5f } });
	REQUIRE(codeOf(r) == "<ok>");
	CHECK(r.content["added"] == true);
	CHECK(r.content["index"] == 0);

	// The file, through the runtime's own loader. Both halves of the claim: the
	// event arrived AND the import survived being rewritten around it.
	const AnimationClipAsset on = clipOnDisk(f.root, "Anim/Run.hasset");
	REQUIRE(on.notifies.size() == 1);
	CHECK(on.notifies[0].name == "Footstep_R");
	CHECK(on.notifies[0].time == doctest::Approx(0.5f));
	CHECK(on.notifies[0].duration == doctest::Approx(0.0f));
	CHECK(on.duration == doctest::Approx(1.0f));
	REQUIRE(on.channels.size() == 3);
	CHECK(on.channels[2].times.size() == 2);
	CHECK(on.channels[2].values.size() == 6);

	// A second append lands beside the first rather than replacing it — two
	// footsteps with the same name are legitimate, which is why the index is the
	// address in the first place.
	const ToolResult again = f.call("clip_notify_set",
		json{ { "path", "Anim/Run.hasset" }, { "name", "Footstep_R" }, { "time", 0.9f } });
	REQUIRE(codeOf(again) == "<ok>");
	CHECK(again.content["index"] == 1);
	CHECK(clipOnDisk(f.root, "Anim/Run.hasset").notifies.size() == 2);
}

TEST_CASE("clip_notify_set changes only the fields it was sent")
{
	Fixture f("update");
	f.writeClip("Anim/Swing.hasset", 2.0f, 1, /*rootMotion=*/false,
	            { { "Damage", 0.4f, 0.3f } });

	const ToolResult r = f.call("clip_notify_set",
		json{ { "path", "Anim/Swing.hasset" }, { "index", 0 }, { "time", 0.8f } });
	REQUIRE(codeOf(r) == "<ok>");
	CHECK(r.content["added"] == false);

	const AnimationClipAsset on = clipOnDisk(f.root, "Anim/Swing.hasset");
	REQUIRE(on.notifies.size() == 1);
	CHECK(on.notifies[0].time == doctest::Approx(0.8f));
	// Neither of these was sent, so neither may have moved.
	CHECK(on.notifies[0].name == "Damage");
	CHECK(on.notifies[0].duration == doctest::Approx(0.3f));

	// A duration back to zero turns a state into a one-shot, and that is a value
	// somebody sent rather than a field left alone.
	const ToolResult z = f.call("clip_notify_set",
		json{ { "path", "Anim/Swing.hasset" }, { "index", 0 }, { "duration", 0.0f } });
	REQUIRE(codeOf(z) == "<ok>");
	CHECK(notifyAt(z.content["notifies"], 0)->at("kind") == "notify");
	CHECK(clipOnDisk(f.root, "Anim/Swing.hasset").notifies[0].duration == doctest::Approx(0.0f));
}

TEST_CASE("clip_notify_set refuses a time outside the clip rather than clamping it")
{
	Fixture f("outside");
	f.writeClip("Anim/Run.hasset", 1.0f, 1, /*rootMotion=*/false, { { "Step", 0.5f, 0.0f } });
	const std::string before = f.bytes("Anim/Run.hasset");

	const ToolResult past = f.call("clip_notify_set",
		json{ { "path", "Anim/Run.hasset" }, { "name", "Late" }, { "time", 1.4f } });
	CHECK(codeOf(past) == "invalid_args");
	// The number the client was missing is in the refusal.
	CHECK(past.errorMessage.find("1.0") != std::string::npos);

	CHECK(codeOf(f.call("clip_notify_set",
		json{ { "path", "Anim/Run.hasset" }, { "name", "Early" }, { "time", -0.1f } })) ==
		"invalid_args");

	// A refusal is a no-op, all the way down to the bytes.
	CHECK(f.bytes("Anim/Run.hasset") == before);
	CHECK(clipOnDisk(f.root, "Anim/Run.hasset").notifies.size() == 1);
}

TEST_CASE("clip_notify_set refuses what would be an event nobody can use")
{
	Fixture f("badargs");
	f.writeClip("Anim/Run.hasset", 1.0f, 1, /*rootMotion=*/false, { { "Step", 0.5f, 0.0f } });
	const std::string before = f.bytes("Anim/Run.hasset");

	// Appending without the two things an event cannot be invented without.
	CHECK(codeOf(f.call("clip_notify_set",
		json{ { "path", "Anim/Run.hasset" }, { "name", "Step" } })) == "invalid_args");
	CHECK(codeOf(f.call("clip_notify_set",
		json{ { "path", "Anim/Run.hasset" }, { "time", 0.2f } })) == "invalid_args");
	// Clearing the name of one that exists: the name IS the payload.
	CHECK(codeOf(f.call("clip_notify_set",
		json{ { "path", "Anim/Run.hasset" }, { "index", 0 }, { "name", "" } })) ==
		"invalid_args");
	// A negative duration has no meaning between "once" and "open this long".
	CHECK(codeOf(f.call("clip_notify_set",
		json{ { "path", "Anim/Run.hasset" }, { "index", 0 }, { "duration", -1.0f } })) ==
		"invalid_args");
	// An index nothing is at, with the count in the refusal.
	const ToolResult oob = f.call("clip_notify_set",
		json{ { "path", "Anim/Run.hasset" }, { "index", 7 }, { "time", 0.2f } });
	CHECK(codeOf(oob) == "invalid_args");
	CHECK(oob.errorMessage.find("1 event") != std::string::npos);

	CHECK(f.bytes("Anim/Run.hasset") == before);
}

TEST_CASE("clip_notify_remove shifts the indices and says so")
{
	Fixture f("remove");
	f.writeClip("Anim/Combo.hasset", 2.0f, 1, /*rootMotion=*/false,
	            { { "A", 0.1f, 0.0f }, { "B", 0.2f, 0.0f }, { "C", 0.3f, 0.0f } });

	const ToolResult r = f.call("clip_notify_remove",
		json{ { "path", "Anim/Combo.hasset" }, { "index", 1 } });
	REQUIRE(codeOf(r) == "<ok>");
	CHECK(r.content["removed"] == "B");
	// The answer is what the client needs next: C is index 1 NOW, and nobody
	// should have to call clip_info to learn that.
	REQUIRE(r.content["notifies"].size() == 2);
	CHECK(notifyAt(r.content["notifies"], 0)->at("name") == "A");
	CHECK(notifyAt(r.content["notifies"], 1)->at("name") == "C");

	const AnimationClipAsset on = clipOnDisk(f.root, "Anim/Combo.hasset");
	REQUIRE(on.notifies.size() == 2);
	CHECK(on.notifies[1].name == "C");
	CHECK(on.channels.size() == 1);

	const ToolResult oob = f.call("clip_notify_remove",
		json{ { "path", "Anim/Combo.hasset" }, { "index", 2 } });
	CHECK(codeOf(oob) == "invalid_args");
	CHECK(clipOnDisk(f.root, "Anim/Combo.hasset").notifies.size() == 2);
}

// ─── The per-clip switch ─────────────────────────────────────────────────────

TEST_CASE("clip_root_motion_set flips the switch and keeps the events")
{
	Fixture f("rootmotion");
	f.writeClip("Anim/Roll.hasset", 1.0f, 2, /*rootMotion=*/false, { { "Land", 0.8f, 0.0f } });

	const ToolResult on = f.call("clip_root_motion_set",
		json{ { "path", "Anim/Roll.hasset" }, { "enabled", true } });
	REQUIRE(codeOf(on) == "<ok>");
	CHECK(on.content["rootMotion"] == true);
	// The other half of the same chunk must come through the write untouched.
	REQUIRE(on.content["notifies"].size() == 1);

	const AnimationClipAsset disk = clipOnDisk(f.root, "Anim/Roll.hasset");
	CHECK(disk.hasRootMotion == true);
	REQUIRE(disk.notifies.size() == 1);
	CHECK(disk.notifies[0].name == "Land");
	CHECK(disk.channels.size() == 2);

	const ToolResult off = f.call("clip_root_motion_set",
		json{ { "path", "Anim/Roll.hasset" }, { "enabled", false } });
	REQUIRE(codeOf(off) == "<ok>");
	CHECK(clipOnDisk(f.root, "Anim/Roll.hasset").hasRootMotion == false);

	// Not optional: a call without it would be a call that did nothing and said
	// it worked.
	CHECK(codeOf(f.call("clip_root_motion_set", json{ { "path", "Anim/Roll.hasset" } })) ==
	      "invalid_args");
}

// ─── The gates ───────────────────────────────────────────────────────────────

TEST_CASE("clip writes refuse while play runs, while a peer holds it, and while a tab is dirty")
{
	Fixture f("gates");
	f.writeClip("Anim/Run.hasset", 1.0f, 1, /*rootMotion=*/false, { { "Step", 0.5f, 0.0f } });
	const std::string before = f.bytes("Anim/Run.hasset");
	const json add{ { "path", "Anim/Run.hasset" }, { "name", "New" }, { "time", 0.2f } };

	f.playing = true;
	CHECK(codeOf(f.call("clip_notify_set", add)) == "play_mode");
	// Reading is not a write: a question about a clip is answerable in play mode.
	CHECK(codeOf(f.call("clip_info", json{ { "path", "Anim/Run.hasset" } })) == "<ok>");
	f.playing = false;

	f.lockedRel = "Anim/Run.hasset";
	CHECK(codeOf(f.call("clip_notify_set", add)) == "locked_by_other");
	f.lockedRel.clear();

	// The tab question is asked with the CLIP's path, not a mesh tab's: that is
	// how SkeletalMeshEditorPanel keys its unsaved clip edits.
	f.dirtyRel = "Anim/Run.hasset";
	const ToolResult dirty = f.call("clip_notify_set", add);
	CHECK(codeOf(dirty) == "dirty");
	CHECK(dirty.errorMessage.find("Skeletal Mesh Editor") != std::string::npos);
	CHECK(codeOf(f.call("clip_root_motion_set",
		json{ { "path", "Anim/Run.hasset" }, { "enabled", true } })) == "dirty");
	CHECK(codeOf(f.call("clip_notify_remove",
		json{ { "path", "Anim/Run.hasset" }, { "index", 0 } })) == "dirty");
	f.dirtyRel.clear();

	// Every one of those refusals left the file exactly as it was.
	CHECK(f.bytes("Anim/Run.hasset") == before);

	// …and with the gates down the same call goes through.
	CHECK(codeOf(f.call("clip_notify_set", add)) == "<ok>");
}
