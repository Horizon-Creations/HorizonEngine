#include "doctest.h"

#include "AssetStubWriter.h"      // writeAssetStub — what the Content Browser makes
#include "TestFsUtil.h"

#include <ContentManager/AssetRefScan.h>
#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/HAsset.h>
#include <Sequence/SequenceJson.h>
#include <HorizonScene/AnimationNotify.h>
#include <HorizonScene/CameraPose.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/PropertyAnimationSystem.h>
#include <HorizonScene/SequenceEval.h>
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/LightComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/TransformComponent.h>

#include <filesystem>
#include <string>
#include <vector>

// ── Cinematic Sequence: asset and evaluation ────────────────────────────────
// Hive topic 84, plan step 2 (docs/sequencer-cinematics-plan.md §5): the asset
// round-trips through CHUNK_SEQU, a stub and a file without the chunk read as an
// empty sequence, and SequenceEval answers at keys, between keys and at cuts —
// all without an editor, the way every later step (runtime, camera, script API,
// the Cinematic tab) is going to lean on it.

namespace fs = std::filesystem;
using namespace HE::SequenceEval;

namespace
{
	struct TempContentDir
	{
		fs::path path;   // …/<name>/Content — the project root is its parent
		explicit TempContentDir(const char* name)
		{
			path = fs::temp_directory_path() / name / "Content";
			he_test::removeAllQuiet(path.parent_path());
			fs::create_directories(path);
		}
		~TempContentDir() { he_test::removeAllQuiet(path.parent_path()); }
	};

	const HE::UUID kClipId  { 7412589630741258963ull, 1597534862159753486ull };
	const HE::UUID kSoundId { 8523697410852369741ull, 2648153972648153972ull };
	const HE::UUID kCamAId  { 1111111111111111111ull, 2222222222222222222ull };
	const HE::UUID kCamBId  { 3333333333333333333ull, 4444444444444444444ull };

	SequenceTrack propertyTrack(uint16_t slot, PropTarget target,
	                            std::vector<float> times, std::vector<float> values)
	{
		SequenceTrack t;
		t.kind           = SequenceTrackKind::Property;
		t.binding        = slot;
		t.channel.target = target;
		t.channel.times  = std::move(times);
		t.channel.values = std::move(values);
		return t;
	}

	SequenceCameraCut cut(float time, uint16_t slot, float blendIn = 0.0f,
	                      SequenceBlendCurve curve = SequenceBlendCurve::Linear)
	{
		SequenceCameraCut c;
		c.time = time; c.binding = slot; c.blendIn = blendIn; c.curve = curve;
		return c;
	}

	SequenceTrack cutTrack(std::vector<SequenceCameraCut> cuts)
	{
		SequenceTrack t;
		t.kind = SequenceTrackKind::CameraCut;
		t.cuts = std::move(cuts);
		return t;
	}

	// One of each track kind, every field off its default, so a round trip that
	// drops or defaults anything shows up as a difference.
	SequenceAsset everyKind()
	{
		SequenceAsset s;
		s.duration  = 12.5f;
		s.frameRate = 24.0f;
		s.bindings  = { { 0, "CamA", kCamAId }, { 3, "Hero", kCamBId } };

		s.tracks.push_back(propertyTrack(3, PropTarget::PosY, { 0.0f, 1.5f, 4.0f }, { 0.25f, -2.0f, 7.0f }));
		s.tracks.push_back(propertyTrack(0, PropTarget::CameraFov, { 0.0f, 2.0f }, { 50.0f, 35.0f }));

		SequenceTrack skel;
		skel.kind    = SequenceTrackKind::Skeletal;
		skel.binding = 3;
		SequenceSkeletalSection sec;
		sec.clipId = kClipId; sec.start = 1.0f; sec.end = 3.5f;
		sec.clipOffset = 0.25f; sec.playRate = 1.5f; sec.loop = true;
		skel.sections.push_back(sec);
		s.tracks.push_back(skel);

		s.tracks.push_back(cutTrack({ cut(0.0f, 0), cut(5.0f, 0, 1.25f, SequenceBlendCurve::EaseOut) }));

		SequenceTrack ev;
		ev.kind    = SequenceTrackKind::Event;
		ev.binding = 3;
		ev.events  = { { "Footstep", 0.5f, 0.0f }, { "Window", 2.0f, 0.75f } };
		s.tracks.push_back(ev);

		SequenceTrack au;
		au.kind = SequenceTrackKind::Audio;   // no binding: plays at the player
		SequenceAudioSection a;
		a.assetId = kSoundId; a.start = 2.5f; a.volume = 0.6f; a.pitch = 1.1f;
		au.audio.push_back(a);
		s.tracks.push_back(au);
		return s;
	}

	void checkSame(const SequenceAsset& a, const SequenceAsset& b)
	{
		CHECK(a.duration  == b.duration);
		CHECK(a.frameRate == b.frameRate);
		REQUIRE(a.bindings.size() == b.bindings.size());
		for (size_t i = 0; i < a.bindings.size(); ++i)
		{
			CHECK(a.bindings[i].slot     == b.bindings[i].slot);
			CHECK(a.bindings[i].name     == b.bindings[i].name);
			CHECK(a.bindings[i].entityId == b.bindings[i].entityId);
		}
		REQUIRE(a.tracks.size() == b.tracks.size());
		for (size_t i = 0; i < a.tracks.size(); ++i)
		{
			const SequenceTrack& x = a.tracks[i];
			const SequenceTrack& y = b.tracks[i];
			CAPTURE(i);
			CHECK(x.kind    == y.kind);
			CHECK(x.binding == y.binding);
			CHECK(x.channel.target == y.channel.target);
			CHECK(x.channel.times  == y.channel.times);
			CHECK(x.channel.values == y.channel.values);
			REQUIRE(x.sections.size() == y.sections.size());
			for (size_t k = 0; k < x.sections.size(); ++k)
			{
				CHECK(x.sections[k].clipId     == y.sections[k].clipId);
				CHECK(x.sections[k].start      == y.sections[k].start);
				CHECK(x.sections[k].end        == y.sections[k].end);
				CHECK(x.sections[k].clipOffset == y.sections[k].clipOffset);
				CHECK(x.sections[k].playRate   == y.sections[k].playRate);
				CHECK(x.sections[k].loop       == y.sections[k].loop);
			}
			REQUIRE(x.cuts.size() == y.cuts.size());
			for (size_t k = 0; k < x.cuts.size(); ++k)
			{
				CHECK(x.cuts[k].time    == y.cuts[k].time);
				CHECK(x.cuts[k].binding == y.cuts[k].binding);
				CHECK(x.cuts[k].blendIn == y.cuts[k].blendIn);
				CHECK(x.cuts[k].curve   == y.cuts[k].curve);
			}
			REQUIRE(x.events.size() == y.events.size());
			for (size_t k = 0; k < x.events.size(); ++k)
			{
				CHECK(x.events[k].name     == y.events[k].name);
				CHECK(x.events[k].time     == y.events[k].time);
				CHECK(x.events[k].duration == y.events[k].duration);
			}
			REQUIRE(x.audio.size() == y.audio.size());
			for (size_t k = 0; k < x.audio.size(); ++k)
			{
				CHECK(x.audio[k].assetId == y.audio[k].assetId);
				CHECK(x.audio[k].start   == y.audio[k].start);
				CHECK(x.audio[k].volume  == y.audio[k].volume);
				CHECK(x.audio[k].pitch   == y.audio[k].pitch);
			}
		}
	}

	void writeSequenceFile(const fs::path& contentRoot, const std::string& relPath,
	                       const HE::UUID& id, const std::string* seqPayload)
	{
		std::vector<uint8_t> meta;
		HAsset::Writer::appendPOD(meta, static_cast<uint16_t>(HE::AssetType::Sequence));
		HAsset::Writer::appendPOD(meta, id.hi);
		HAsset::Writer::appendPOD(meta, id.lo);
		HAsset::Writer::appendString(meta, fs::path(relPath).stem().string());
		HAsset::Writer::appendString(meta, relPath);

		fs::create_directories((contentRoot / relPath).parent_path());
		HAsset::Writer w;
		w.addChunk(HAsset::CHUNK_META, meta.data(), meta.size());
		if (seqPayload) w.addChunk(HAsset::CHUNK_SEQU, seqPayload->data(), seqPayload->size());
		REQUIRE(w.write((contentRoot / relPath).string(), static_cast<uint16_t>(HE::AssetType::Sequence)));
	}
}

// ─── The asset type itself ───────────────────────────────────────────────────

TEST_CASE("sequence: the asset type is named, creatable and travels over collaboration")
{
	CHECK(std::string(HE::assetTypeName(HE::AssetType::Sequence)) == "Sequence");
	CHECK(HE::Ed::assetTypeFromName("Sequence") == HE::AssetType::Sequence);
	CHECK(HE::Ed::isCreatableAssetType(HE::AssetType::Sequence));
	CHECK(HE::isCollabSyncableAssetType(HE::AssetType::Sequence));
}

// ─── JSON ────────────────────────────────────────────────────────────────────

TEST_CASE("sequence: every track kind survives the JSON form field for field")
{
	const SequenceAsset original = everyKind();
	const std::string json = HE::sequenceToJson(original);

	SequenceAsset back;
	int dropped = -1;
	REQUIRE(HE::sequenceFromJson(json, back, &dropped));
	CHECK(dropped == 0);
	checkSame(original, back);

	// And once more: the form a second save writes is the form the first did.
	CHECK(HE::sequenceToJson(back) == json);
}

TEST_CASE("sequence: bad entries are dropped and counted, a bad document is refused whole")
{
	SequenceAsset out;
	out.duration = 99.0f;

	// Not an object, or not JSON: refused, and `out` is left exactly as it was.
	CHECK_FALSE(HE::sequenceFromJson("", out));
	CHECK_FALSE(HE::sequenceFromJson("[1,2,3]", out));
	CHECK_FALSE(HE::sequenceFromJson("{\"tracks\":[", out));
	// A key of the wrong type makes json::value() throw; that is a refusal too,
	// never an exception out of the loader.
	CHECK_FALSE(HE::sequenceFromJson(R"({"duration":"long"})", out));
	CHECK(out.duration == 99.0f);

	// One good track among four that this build cannot use.
	const std::string doc = R"({
		"duration": 3, "frameRate": 0,
		"bindings": [ {"slot": 1, "name": "A"}, {"slot": -4}, {"name": "no slot"} ],
		"tracks": [
			{"kind": "property", "binding": 1, "target": 0, "times": [0, 1], "values": [0, 2]},
			{"kind": "property", "binding": 1, "target": 250, "times": [0], "values": [0]},
			{"kind": "property", "binding": 1, "target": 1, "times": [0, 1], "values": [0]},
			{"kind": "hologram", "binding": 1},
			{"kind": "cameraCut", "cuts": [ {"time": 2, "binding": 1, "blendIn": -3, "curve": 77},
			                                {"time": 1, "binding": 1} ]}
		]})";
	int dropped = 0;
	REQUIRE(HE::sequenceFromJson(doc, out, &dropped));
	// two bindings without a usable slot, bad target, short values, unknown kind
	CHECK(dropped == 5);
	CHECK(out.duration == 3.0f);
	CHECK(out.frameRate == 30.0f);          // 0 fps is no rate; the default stands in
	REQUIRE(out.bindings.size() == 1);
	CHECK(out.bindings[0].entityId == HE::UUID{});
	REQUIRE(out.tracks.size() == 2);
	CHECK(out.tracks[0].channel.target == PropTarget::PosX);
	// Cuts come back in time order, a negative blend is no blend, and a curve this
	// build does not know is the default rather than an enum with no enumerator.
	REQUIRE(out.tracks[1].cuts.size() == 2);
	CHECK(out.tracks[1].cuts[0].time == 1.0f);
	CHECK(out.tracks[1].cuts[1].blendIn == 0.0f);
	CHECK(out.tracks[1].cuts[1].curve == SequenceBlendCurve::SmoothStep);
	CHECK(out.tracks[1].binding == kSequenceNoBinding);
}

TEST_CASE("sequence: the assets it plays are listed once each, its actors are not")
{
	SequenceAsset s = everyKind();
	SequenceTrack again;
	again.kind = SequenceTrackKind::Skeletal;
	SequenceSkeletalSection sec; sec.clipId = kClipId;
	again.sections = { sec, SequenceSkeletalSection{} };   // a repeat and an unset clip
	s.tracks.push_back(again);

	std::vector<HE::UUID> refs;
	HE::sequenceAssetRefs(s, refs);
	CHECK(refs == std::vector<HE::UUID>{ kClipId, kSoundId });
}

// ─── Through the ContentManager and onto disk ────────────────────────────────

TEST_CASE("sequence: a stub reads as an empty sequence, and an edit survives save and reload")
{
	TempContentDir dir("he_test_sequence_roundtrip");

	{
		const fs::path abs = dir.path / "Intro.hasset";
		REQUIRE(HE::Ed::writeAssetStub(abs.string(), "Intro.hasset", "Intro", HE::AssetType::Sequence));
		ContentManager cm(dir.path.string());
		const HE::UUID id = cm.loadAsset("Intro.hasset");
		REQUIRE(id != HE::UUID{});
		CHECK(cm.assetType(id) == HE::AssetType::Sequence);
		SequenceAsset* seq = cm.getSequenceMutable(id);
		REQUIRE(seq != nullptr);
		CHECK(seq->duration == 0.0f);
		CHECK(seq->bindings.empty());
		CHECK(seq->tracks.empty());

		const SequenceAsset authored = everyKind();
		seq->duration  = authored.duration;
		seq->frameRate = authored.frameRate;
		seq->bindings  = authored.bindings;
		seq->tracks    = authored.tracks;
		REQUIRE(cm.saveAsset(*seq));
	}
	{
		ContentManager cm(dir.path.string());
		const SequenceAsset* seq = cm.getSequence(cm.loadAsset("Intro.hasset"));
		REQUIRE(seq != nullptr);
		checkSame(everyKind(), *seq);
	}
}

TEST_CASE("sequence: a file without the chunk is empty, a file with a broken chunk does not load")
{
	TempContentDir dir("he_test_sequence_oldfiles");

	// What an older writer — or a stub from before this build — leaves behind.
	writeSequenceFile(dir.path, "Old.hasset", HE::UUID::generate(), nullptr);
	// A hand edit gone wrong. Loading it as empty would be worse than failing: the
	// next save from the editor would overwrite the author's file with nothing.
	const std::string broken = R"({"duration": 2, "tracks": [)";
	writeSequenceFile(dir.path, "Broken.hasset", HE::UUID::generate(), &broken);

	ContentManager cm(dir.path.string());
	const SequenceAsset* old = cm.getSequence(cm.loadAsset("Old.hasset"));
	REQUIRE(old != nullptr);
	CHECK(old->tracks.empty());
	CHECK(cm.loadAsset("Broken.hasset") == HE::UUID{});
}

TEST_CASE("sequence: an asset a sequence plays is found by the reference scan")
{
	TempContentDir dir("he_test_sequence_refscan");

	SequenceAsset s = everyKind();
	const std::string payload = HE::sequenceToJson(s);
	writeSequenceFile(dir.path, "Cine/Intro.hasset", HE::UUID::generate(), &payload);

	const SequenceAsset quiet;   // no tracks, so it names no asset
	const std::string quietPayload = HE::sequenceToJson(quiet);
	writeSequenceFile(dir.path, "Cine/Quiet.hasset", HE::UUID::generate(), &quietPayload);

	HE::AssetRefs::ScanRequest req;
	req.contentRoot    = dir.path.string();
	req.projectRoot    = dir.path.parent_path().string();
	req.contentDirName = "Content";

	for (const HE::UUID& target : { kClipId, kSoundId })
	{
		HE::AssetRefs::ScanTargets targets;
		targets.uuids.push_back(target);
		const HE::AssetRefs::ScanResult res = HE::AssetRefs::findReferrers(targets, req);
		CHECK_FALSE(res.incomplete);
		REQUIRE(res.referrers.size() == 1);
		CHECK(res.referrers[0].displayPath == "Cine/Intro.hasset");
		CHECK(res.referrers[0].kind == HE::AssetRefs::RefKind::Uuid);
	}
}

// ─── Evaluation: property tracks ─────────────────────────────────────────────

TEST_CASE("sequence eval: property tracks at keys, between keys and past both ends")
{
	SequenceAsset s;
	s.tracks.push_back(propertyTrack(0, PropTarget::PosX, { 1.0f, 3.0f }, { 10.0f, 20.0f }));
	s.tracks.push_back(propertyTrack(2, PropTarget::RotY, { 0.0f, 2.0f }, { 0.0f, 90.0f }));

	auto at = [&](float t, size_t i) { return evaluate(s, t).writes.at(i).value; };

	CHECK(at(1.0f, 0) == doctest::Approx(10.0f));   // at a key
	CHECK(at(3.0f, 0) == doctest::Approx(20.0f));
	CHECK(at(2.0f, 0) == doctest::Approx(15.0f));   // between keys
	CHECK(at(0.0f, 0) == doctest::Approx(10.0f));   // before the first: held
	CHECK(at(9.0f, 0) == doctest::Approx(20.0f));   // after the last: held
	CHECK(at(0.5f, 1) == doctest::Approx(22.5f));

	const Result r = evaluate(s, 1.0f);
	REQUIRE(r.writes.size() == 2);
	CHECK(r.writes[0].slot == 0);
	CHECK(r.writes[0].target == PropTarget::PosX);
	CHECK(r.writes[1].slot == 2);
	CHECK(r.writes[1].target == PropTarget::RotY);

	// The sampling is the Property Animator's, not a second copy of it.
	CHECK(at(2.3f, 0) == PropertyAnimationSystem::sampleChannel(s.tracks[0].channel, 2.3f));
}

TEST_CASE("sequence eval: a track with no actor or no keys writes nothing")
{
	SequenceAsset s;
	s.tracks.push_back(propertyTrack(kSequenceNoBinding, PropTarget::PosX, { 0.0f }, { 5.0f }));
	// Sampling an empty channel answers 0, and 0 in a scale is "gone", not "not animated".
	s.tracks.push_back(propertyTrack(1, PropTarget::ScaleX, {}, {}));
	CHECK(evaluate(s, 0.0f).writes.empty());
	CHECK_FALSE(evaluate(s, 0.0f).camera.active);
}

TEST_CASE("sequence eval: Visible is a switch — it steps at its keys instead of fading through 0.5")
{
	SequenceAsset s;
	s.tracks.push_back(propertyTrack(0, PropTarget::Visible, { 0.0f, 2.0f, 3.0f }, { 1.0f, 0.0f, 1.0f }));

	auto v = [&](float t) { return evaluate(s, t).writes.at(0).value; };
	CHECK(v(-1.0f) == 1.0f);
	CHECK(v(0.0f)  == 1.0f);
	CHECK(v(1.99f) == 1.0f);   // linear would say 0.005 short of the key
	CHECK(v(2.0f)  == 0.0f);   // the key's own value from its own instant on
	CHECK(v(2.5f)  == 0.0f);
	CHECK(v(3.0f)  == 1.0f);
	CHECK(v(10.0f) == 1.0f);

	CHECK(PropertyAnimationSystem::isStepTarget(PropTarget::Visible));
	CHECK_FALSE(PropertyAnimationSystem::isStepTarget(PropTarget::CameraFov));
	CHECK_FALSE(PropertyAnimationSystem::isStepTarget(PropTarget::PosX));
}

// ─── Evaluation: camera cuts ─────────────────────────────────────────────────

TEST_CASE("sequence eval: the latest cut at or before t holds the camera")
{
	SequenceAsset s;
	s.tracks.push_back(cutTrack({ cut(1.0f, 0), cut(4.0f, 1), cut(6.0f, kSequenceNoBinding) }));

	CHECK_FALSE(evaluate(s, 0.5f).camera.active);        // before the first cut: gameplay's
	CHECK(evaluate(s, 1.0f).camera.active);              // AT the cut: already cut
	CHECK(evaluate(s, 1.0f).camera.slot == 0);
	CHECK(evaluate(s, 3.99f).camera.slot == 0);
	CHECK(evaluate(s, 4.0f).camera.slot == 1);
	CHECK_FALSE(evaluate(s, 4.0f).camera.blending);      // blendIn 0: a hard cut
	CHECK(evaluate(s, 4.0f).camera.alpha == 1.0f);
	CHECK_FALSE(evaluate(s, 6.0f).camera.active);        // a cut to nobody hands back
	CHECK_FALSE(evaluate(s, 60.0f).camera.active);

	// Order in the list is not order in time, and the answer must not care.
	SequenceAsset shuffled;
	shuffled.tracks.push_back(cutTrack({ cut(4.0f, 1), cut(6.0f, kSequenceNoBinding), cut(1.0f, 0) }));
	for (float t : { 0.5f, 1.0f, 2.0f, 4.0f, 5.0f, 6.0f, 7.0f })
	{
		CAPTURE(t);
		CHECK(evaluate(shuffled, t).camera.active == evaluate(s, t).camera.active);
		CHECK(evaluate(shuffled, t).camera.slot   == evaluate(s, t).camera.slot);
	}

	// Two cuts at one instant: the later-listed one wins.
	SequenceAsset tie;
	tie.tracks.push_back(cutTrack({ cut(2.0f, 3), cut(2.0f, 5) }));
	CHECK(evaluate(tie, 2.0f).camera.slot == 5);

	// Only the first camera-cut track counts.
	SequenceAsset two;
	two.tracks.push_back(cutTrack({ cut(0.0f, 7) }));
	two.tracks.push_back(cutTrack({ cut(0.0f, 8) }));
	CHECK(evaluate(two, 1.0f).camera.slot == 7);
}

TEST_CASE("sequence eval: a cut with blendIn travels from the previous camera, shaped by its curve")
{
	SequenceAsset s;
	s.tracks.push_back(cutTrack({ cut(0.0f, 0, 2.0f, SequenceBlendCurve::Linear),
	                              cut(4.0f, 1, 1.0f, SequenceBlendCurve::SmoothStep),
	                              cut(8.0f, 1, 1.0f),
	                              cut(10.0f, 2, 0.5f, SequenceBlendCurve::EaseOut) }));

	// The first cut blends in from the gameplay camera.
	CameraState c = evaluate(s, 0.0f).camera;
	CHECK(c.active);
	CHECK(c.blending);
	CHECK(c.fromGameplay);
	CHECK(c.fromSlot == kSequenceNoBinding);
	CHECK(c.alpha == 0.0f);
	CHECK(evaluate(s, 0.5f).camera.alpha == doctest::Approx(0.25f));
	CHECK(evaluate(s, 1.0f).camera.alpha == doctest::Approx(0.5f));
	c = evaluate(s, 2.0f).camera;                    // blend over: plainly on camera 0
	CHECK_FALSE(c.blending);
	CHECK(c.alpha == 1.0f);
	CHECK(c.slot == 0);

	// Between two sequence cameras: from 0 to 1, SmoothStep.
	c = evaluate(s, 4.25f).camera;
	CHECK(c.blending);
	CHECK_FALSE(c.fromGameplay);
	CHECK(c.fromSlot == 0);
	CHECK(c.slot == 1);
	CHECK(c.alpha == doctest::Approx(HE::applyBlendCurve(0.25f, HE::BlendCurve::SmoothStep)));
	CHECK(c.alpha != doctest::Approx(0.25f));        // shaped, not linear

	// A cut to the camera that is already live does not blend.
	c = evaluate(s, 8.5f).camera;
	CHECK_FALSE(c.blending);
	CHECK(c.slot == 1);

	c = evaluate(s, 10.25f).camera;
	CHECK(c.fromSlot == 1);
	CHECK(c.alpha == doctest::Approx(HE::applyBlendCurve(0.5f, HE::BlendCurve::EaseOut)));
}

// ─── Into a world ────────────────────────────────────────────────────────────

TEST_CASE("sequence eval: bindings resolve by entity id, overrides win, missing actors are skipped")
{
	ContentManager cm;
	HorizonWorld   world;
	auto& reg = world.registry();

	const entt::entity cam = world.createEntity("Cam");
	world.addComponent(cam, TransformComponent{ .position = {}, .rotation = {}, .scale = glm::vec3(1.0f) });
	world.addComponent(cam, CameraComponent{});
	world.setEntityId(cam, kCamAId);

	const entt::entity lamp = world.createEntity("Lamp");
	world.addComponent(lamp, TransformComponent{ .position = {}, .rotation = {}, .scale = glm::vec3(1.0f) });
	world.addComponent(lamp, MeshComponent{});
	world.addComponent(lamp, LightComponent{});

	const entt::entity stranger = world.createEntity("Spawned");
	world.addComponent(stranger, TransformComponent{ .position = {}, .rotation = {}, .scale = glm::vec3(1.0f) });

	SequenceAsset s;
	s.bindings = { { 0, "Cam", kCamAId },
	               { 1, "Lamp", world.entityId(lamp) },
	               { 2, "Door_North", HE::UUID::generate() },   // deleted since
	               { 4, "Player", HE::UUID{} } };               // filled at runtime
	s.tracks.push_back(propertyTrack(0, PropTarget::CameraFov, { 0.0f, 2.0f }, { 60.0f, 30.0f }));
	s.tracks.push_back(propertyTrack(0, PropTarget::PosZ, { 0.0f, 2.0f }, { 0.0f, -8.0f }));
	s.tracks.push_back(propertyTrack(1, PropTarget::Visible, { 0.0f, 1.0f }, { 1.0f, 0.0f }));
	s.tracks.push_back(propertyTrack(2, PropTarget::PosX, { 0.0f }, { 99.0f }));
	s.tracks.push_back(propertyTrack(4, PropTarget::PosY, { 0.0f, 2.0f }, { 0.0f, 6.0f }));

	std::vector<entt::entity> slots = resolveBindings(world, s);
	REQUIRE(slots.size() == 5);
	CHECK(slots[0] == cam);
	CHECK(slots[1] == lamp);
	CHECK((slots[2] == entt::null));
	CHECK((slots[3] == entt::null));
	CHECK((slots[4] == entt::null));

	apply(world, cm, evaluate(s, 1.0f), slots);
	CHECK(reg.get<CameraComponent>(cam).fovDegrees == doctest::Approx(45.0f));
	CHECK(reg.get<TransformComponent>(cam).position.z == doctest::Approx(-4.0f));
	CHECK(reg.get<TransformComponent>(cam).dirty);
	// Visible is the draw flag of every renderable the actor has, not the
	// "switched off" tag: a hidden lamp still exists for scripts and physics.
	CHECK_FALSE(reg.get<MeshComponent>(lamp).visible);
	CHECK_FALSE(reg.get<LightComponent>(lamp).visible);
	CHECK(reg.get<TransformComponent>(stranger).position.y == 0.0f);

	// Back before the key: the switch flips on again.
	apply(world, cm, evaluate(s, 0.5f), slots);
	CHECK(reg.get<MeshComponent>(lamp).visible);
	CHECK(reg.get<LightComponent>(lamp).visible);

	// "The player" is whoever the caller says it is.
	slots = resolveBindings(world, s, { { 4, stranger } });
	apply(world, cm, evaluate(s, 1.0f), slots);
	CHECK(reg.get<TransformComponent>(stranger).position.y == doctest::Approx(3.0f));

	// An actor destroyed after resolution is skipped, not written into.
	world.destroyEntity(stranger);
	apply(world, cm, evaluate(s, 2.0f), slots);
	CHECK(reg.get<TransformComponent>(cam).position.z == doctest::Approx(-8.0f));
}

TEST_CASE("sequence eval: the Property Animator writes the new targets through the same function")
{
	ContentManager cm;
	HorizonWorld   world;
	const entt::entity e = world.createEntity();
	world.addComponent(e, TransformComponent{ .position = {}, .rotation = {}, .scale = glm::vec3(1.0f) });
	world.addComponent(e, CameraComponent{});
	world.addComponent(e, MeshComponent{});

	PropertyAnimClipAsset clip;
	clip.duration = 2.0f;
	PropertyAnimChannel fov; fov.target = PropTarget::CameraFov; fov.times = { 0.0f, 2.0f }; fov.values = { 90.0f, 50.0f };
	PropertyAnimChannel vis; vis.target = PropTarget::Visible;   vis.times = { 0.0f, 1.0f }; vis.values = { 0.0f, 1.0f };
	clip.channels = { fov, vis };

	PropertyAnimationSystem::applyAt(world, cm, e, clip, 0.5f);
	CHECK(world.registry().get<CameraComponent>(e).fovDegrees == doctest::Approx(80.0f));
	CHECK_FALSE(world.registry().get<MeshComponent>(e).visible);
	PropertyAnimationSystem::applyAt(world, cm, e, clip, 1.0f);
	CHECK(world.registry().get<MeshComponent>(e).visible);
}

// ─── The notify span rule, lifted off the clip ───────────────────────────────

TEST_CASE("sequence: the notify span rule over a bare list is the clip's rule")
{
	AnimationClipAsset clip;
	clip.duration = 2.0f;
	clip.notifies = { { "Step", 0.0f, 0.0f }, { "Swing", 0.5f, 1.0f }, { "Late", 1.8f, 0.9f } };

	const float spans[][2] = { { 0.0f, 0.1f }, { 0.4f, 1.6f }, { 1.5f, 2.5f },
	                           { 0.2f, 5.3f }, { 1.9f, 0.1f }, { 1.0f, 1.0f } };
	for (bool includeStart : { false, true })
		for (const auto& sp : spans)
		{
			CAPTURE(sp[0]); CAPTURE(sp[1]); CAPTURE(includeStart);
			HE::NotifyQueue viaClip, viaList;
			HE::collectNotifies(clip, 9, sp[0], sp[1], includeStart, viaClip);
			HE::collectNotifySpan(clip.notifies, clip.duration, 9, sp[0], sp[1], includeStart, viaList);
			REQUIRE(viaClip.size() == viaList.size());
			for (size_t i = 0; i < viaClip.size(); ++i)
			{
				CHECK(viaClip[i].entity == viaList[i].entity);
				CHECK(viaClip[i].name   == viaList[i].name);
				CHECK(viaClip[i].kind   == viaList[i].kind);
			}
		}

	// And it is usable without a clip at all — a sequence's event track.
	HE::NotifyQueue q;
	const std::vector<AnimationNotify> events = { { "Boom", 3.0f, 0.0f } };
	HE::collectNotifySpan(events, 10.0f, 4, 2.5f, 3.5f, false, q);
	REQUIRE(q.size() == 1);
	CHECK(q[0].name == "Boom");
	CHECK(q[0].entity == 4u);
}
