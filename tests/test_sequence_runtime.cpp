#include "doctest.h"

#include "TestFsUtil.h"

#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <HorizonScene/AnimationNotify.h>
#include <HorizonScene/AudioEngine.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/SceneSystems.h>
#include <HorizonScene/SequenceSystem.h>
#include <HorizonScene/CameraRigController.h>
#include <HorizonScene/Components/AnimatorComponent.h>
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/CameraRigComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/PropertyAnimatorComponent.h>
#include <HorizonScene/Components/SequencePlayerComponent.h>
#include <HorizonScene/Components/SkeletalMeshComponent.h>
#include <HorizonScene/Components/TransformComponent.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

// ── Cinematic Sequence: runtime playback ─────────────────────────────────────
// Hive topic 84, plan step 3 (docs/sequencer-cinematics-plan.md §5): the Sequence
// Player in a real world, driven through SceneSystems::tickAnimation exactly as
// both applications drive it — the clock, loop and end, events once per pass,
// the skeleton it takes over from an animator and hands back, sound started by
// the span rule, and a missing actor that is skipped rather than fatal.

namespace fs = std::filesystem;

namespace
{
	constexpr float kDt = 0.1f;

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

	SequenceTrack eventTrack(uint16_t slot, std::vector<AnimationNotify> events)
	{
		SequenceTrack t;
		t.kind    = SequenceTrackKind::Event;
		t.binding = slot;
		t.events  = std::move(events);
		return t;
	}

	entt::entity makeActor(HorizonWorld& world, const char* name)
	{
		const entt::entity e = world.createEntity(name);
		world.addComponent(e, TransformComponent{ .position = {}, .rotation = {}, .scale = glm::vec3(1.0f) });
		world.setEntityId(e, HE::UUID::generate());
		return e;
	}

	// One "cutscene": a sequence registered with the content manager and an
	// owner entity whose Sequence Player plays it.
	struct Rig
	{
		ContentManager      cm;
		HorizonWorld        world;
		HE::NotifyQueue     notifies;
		HE::SequenceContext ctx;   // no audio engine unless a test gives it one
		entt::entity        owner = entt::null;
		HE::UUID            seqId;

		SequencePlayerComponent& player() { return world.registry().get<SequencePlayerComponent>(owner); }

		void make(SequenceAsset seq, bool autoplay = true, bool loop = false, float rate = 1.0f)
		{
			seqId = cm.registerSequence(std::move(seq));
			owner = world.createEntity("CutsceneOwner");
			SequencePlayerComponent sp;
			sp.sequenceId = seqId;
			sp.autoplay   = autoplay;
			sp.loop       = loop;
			sp.playRate   = rate;
			world.registry().emplace<SequencePlayerComponent>(owner, sp);
		}

		// One frame of a play session: the animation phase, then the drain.
		// Returns the events fired this frame, by name.
		std::vector<std::string> frame(float dt = kDt, bool session = true)
		{
			notifies.clear();
			SceneSystems::tickAnimation(world, cm, dt, nullptr, nullptr,
			                            session ? &notifies : nullptr,
			                            session ? &ctx : nullptr);
			std::vector<std::string> names;
			for (const auto& ev : notifies) names.push_back(ev.name);
			return names;
		}
	};

	long countOf(const std::vector<std::string>& v, const char* name)
	{
		return std::count(v.begin(), v.end(), std::string(name));
	}

	SkeletalMeshAsset oneBoneMesh()
	{
		SkeletalMeshAsset sma;
		sma.name = "seqSkel";
		SkeletonJoint root;
		root.name   = "Root";
		root.parent = -1;
		root.inverseBindMatrix = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
		sma.skeleton.push_back(root);
		return sma;
	}

	// Root translation X goes from 0 to `to` over `duration`.
	AnimationClipAsset translationClip(float to, float duration = 1.0f)
	{
		AnimationClipAsset clip;
		clip.duration = duration;
		AnimationChannel ch;
		ch.jointIndex = 0;
		ch.path       = AnimPathType::Translation;
		ch.times      = { 0.0f, duration };
		ch.values     = { 0.0f, 0.0f, 0.0f,  to, 0.0f, 0.0f };
		clip.channels.push_back(std::move(ch));
		return clip;
	}

	float rootX(const SkeletalMeshComponent& smc) { return smc.boneMatrices.at(0)[3].x; }
}

// ─── Gate ────────────────────────────────────────────────────────────────────

TEST_CASE("sequence runtime: without a play session nothing advances and nothing is written")
{
	Rig r;
	const entt::entity door = makeActor(r.world, "Door");
	SequenceAsset s;
	s.duration = 2.0f;
	s.bindings = { { 0, "Door", r.world.entityId(door) } };
	s.tracks.push_back(propertyTrack(0, PropTarget::PosX, { 0.0f, 2.0f }, { 5.0f, 9.0f }));
	r.make(std::move(s));

	// The editor's edit world: tickAnimation runs every frame, the context is null.
	for (int i = 0; i < 5; ++i) r.frame(kDt, /*session=*/false);
	CHECK(r.world.registry().get<TransformComponent>(door).position.x == 0.0f);
	CHECK_FALSE(r.player().started);
	CHECK(r.player().time == 0.0f);

	// The same world once play starts: autoplay, and the first frame is written.
	r.frame();
	CHECK(r.player().playing);
	CHECK(r.world.registry().get<TransformComponent>(door).position.x == doctest::Approx(5.2f));
}

// ─── Clock ───────────────────────────────────────────────────────────────────

TEST_CASE("sequence runtime: the clock follows dt and the play rate, and stops on the last frame")
{
	Rig r;
	const entt::entity door = makeActor(r.world, "Door");
	SequenceAsset s;
	s.duration = 1.0f;
	s.bindings = { { 0, "Door", r.world.entityId(door) } };
	s.tracks.push_back(propertyTrack(0, PropTarget::PosY, { 0.0f, 1.0f }, { 0.0f, 10.0f }));
	r.make(std::move(s), true, false, 2.0f);
	auto& reg = r.world.registry();

	r.frame();
	CHECK(r.player().time == doctest::Approx(0.2f));
	CHECK(reg.get<TransformComponent>(door).position.y == doctest::Approx(2.0f));
	for (int i = 0; i < 3; ++i) r.frame();
	CHECK(r.player().time == doctest::Approx(0.8f));
	CHECK(r.player().playing);

	// Over the end: clamped onto it, written there, and stopped.
	r.frame();
	CHECK(r.player().time == doctest::Approx(1.0f));
	CHECK(reg.get<TransformComponent>(door).position.y == doctest::Approx(10.0f));
	CHECK_FALSE(r.player().playing);

	// Stopped means no longer written: the actor is gameplay's again.
	reg.get<TransformComponent>(door).position.y = -3.0f;
	r.frame();
	CHECK(reg.get<TransformComponent>(door).position.y == -3.0f);

	// play() on a player standing at its end starts it over.
	REQUIRE(SequenceSystem::play(r.world, r.cm, r.owner));
	r.frame();
	CHECK(r.player().time == doctest::Approx(0.2f));
	CHECK(reg.get<TransformComponent>(door).position.y == doctest::Approx(2.0f));
}

TEST_CASE("sequence runtime: autoplay off waits for play(), pause holds the clock but keeps the actors")
{
	Rig r;
	const entt::entity lift = makeActor(r.world, "Lift");
	SequenceAsset s;
	s.duration = 4.0f;
	s.bindings = { { 0, "Lift", r.world.entityId(lift) } };
	s.tracks.push_back(propertyTrack(0, PropTarget::PosY, { 0.0f, 4.0f }, { 0.0f, 4.0f }));
	r.make(std::move(s), /*autoplay=*/false);
	auto& reg = r.world.registry();

	// A Property Animator on the same target: the sequence has to win on it.
	PropertyAnimClipAsset clip;
	clip.duration = 1.0f;
	clip.channels.push_back({ PropTarget::PosY, { 0.0f, 1.0f }, { 100.0f, 100.0f } });
	PropertyAnimatorComponent pa;
	pa.clipId = r.cm.registerPropertyAnimClip(std::move(clip));
	reg.emplace<PropertyAnimatorComponent>(lift, pa);

	r.frame();
	r.frame();
	CHECK_FALSE(r.player().playing);
	CHECK(reg.get<TransformComponent>(lift).position.y == doctest::Approx(100.0f));

	REQUIRE(SequenceSystem::play(r.world, r.cm, r.owner));
	r.frame();
	CHECK(reg.get<TransformComponent>(lift).position.y == doctest::Approx(0.1f));

	SequenceSystem::pause(r.world, r.owner);
	for (int i = 0; i < 3; ++i) r.frame();
	CHECK(r.player().time == doctest::Approx(0.1f));
	// Still owned while paused: the property animator does not get it back.
	CHECK(reg.get<TransformComponent>(lift).position.y == doctest::Approx(0.1f));

	// play() on a paused player resumes where it stood.
	SequenceSystem::play(r.world, r.cm, r.owner);
	r.frame();
	CHECK(r.player().time == doctest::Approx(0.2f));

	// stop() rewinds and lets go.
	SequenceSystem::stop(r.world, r.owner);
	r.frame();
	CHECK_FALSE(r.player().playing);
	CHECK(r.player().time == 0.0f);
	CHECK(reg.get<TransformComponent>(lift).position.y == doctest::Approx(100.0f));
}

TEST_CASE("sequence runtime: a looping sequence wraps and fires each event once per pass")
{
	Rig r;
	SequenceAsset s;
	s.duration = 1.0f;
	// On the seam, in the middle, and a notify STATE across the middle.
	s.tracks.push_back(eventTrack(kSequenceNoBinding, { { "Start", 0.0f, 0.0f },
	                                                     { "Mid",   0.55f, 0.0f },
	                                                     { "Win",   0.3f, 0.4f } }));
	r.make(std::move(s), true, /*loop=*/true);

	std::vector<std::string> all;
	// 3.1 s at 0.1: three passes and the first frame of a fourth. Not 3.0: a
	// float sum of thirty 0.1s may stop a hair short of the third seam.
	for (int i = 0; i < 31; ++i)
	{
		const auto fired = r.frame();
		all.insert(all.end(), fired.begin(), fired.end());
	}
	CHECK(r.player().playing);
	CHECK(r.player().time == doctest::Approx(0.1f).epsilon(1e-3));
	// "Start" at 0 fires on the very first frame (the closed origin), and then
	// once per seam crossed — at 1.0, 2.0 and 3.0 — never twice for one seam.
	CHECK(countOf(all, "Start") == 4);
	CHECK(countOf(all, "Mid") == 3);
	CHECK(countOf(all, "Win") == 6);   // Begin + End per pass

	// Every event went to the owner: the track names no actor.
	r.notifies.clear();
	for (int i = 0; i < 10 && r.notifies.empty(); ++i)
		SceneSystems::tickAnimation(r.world, r.cm, kDt, nullptr, nullptr, &r.notifies, &r.ctx);
	REQUIRE_FALSE(r.notifies.empty());
	CHECK(r.notifies.front().entity == static_cast<uint32_t>(r.owner));
}

TEST_CASE("sequence runtime: a non-looping sequence fires its events exactly once, the last one on the end")
{
	Rig r;
	SequenceAsset s;
	s.duration = 1.0f;
	s.tracks.push_back(eventTrack(kSequenceNoBinding, { { "A", 0.0f, 0.0f }, { "B", 0.5f, 0.0f },
	                                                     { "Z", 1.0f, 0.0f } }));
	r.make(std::move(s));

	std::vector<std::string> all;
	for (int i = 0; i < 20; ++i)
	{
		const auto fired = r.frame(0.07f);
		all.insert(all.end(), fired.begin(), fired.end());
	}
	CHECK_FALSE(r.player().playing);
	CHECK(countOf(all, "A") == 1);
	CHECK(countOf(all, "B") == 1);
	CHECK(countOf(all, "Z") == 1);
	REQUIRE(all.size() == 3);
	CHECK(all.back() == "Z");
}

TEST_CASE("sequence runtime: events go to their actor, and a missing actor takes them along")
{
	Rig r;
	const entt::entity door = makeActor(r.world, "Door");
	SequenceAsset s;
	s.duration = 1.0f;
	s.bindings = { { 0, "Door", r.world.entityId(door) },
	               { 1, "Gone", HE::UUID::generate() } };
	s.tracks.push_back(eventTrack(0, { { "Open", 0.1f, 0.0f } }));
	s.tracks.push_back(eventTrack(1, { { "Explode", 0.1f, 0.0f } }));
	// The missing actor's property track does not stop the door's.
	s.tracks.push_back(propertyTrack(1, PropTarget::PosX, { 0.0f }, { 50.0f }));
	s.tracks.push_back(propertyTrack(0, PropTarget::PosZ, { 0.0f, 1.0f }, { 0.0f, 1.0f }));
	r.make(std::move(s));

	r.frame();
	REQUIRE(r.notifies.size() == 1);
	CHECK(r.notifies[0].name == "Open");
	CHECK(r.notifies[0].entity == static_cast<uint32_t>(door));
	CHECK(r.world.registry().get<TransformComponent>(door).position.z == doctest::Approx(0.1f));
	CHECK(r.world.registry().get<TransformComponent>(door).position.x == 0.0f);
}

TEST_CASE("sequence runtime: setTime jumps without firing what it jumps over, and writes a stopped player once")
{
	Rig r;
	const entt::entity door = makeActor(r.world, "Door");
	SequenceAsset s;
	s.duration = 10.0f;
	s.bindings = { { 0, "Door", r.world.entityId(door) } };
	s.tracks.push_back(eventTrack(kSequenceNoBinding, { { "Boom1", 2.0f, 0.0f }, { "Boom2", 3.0f, 0.0f },
	                                                     { "Late",  6.05f, 0.0f } }));
	s.tracks.push_back(propertyTrack(0, PropTarget::PosX, { 0.0f, 10.0f }, { 0.0f, 10.0f }));
	r.make(std::move(s), /*autoplay=*/false);
	auto& reg = r.world.registry();

	// Stopped: the new frame is written once, nothing plays.
	SequenceSystem::setTime(r.world, r.cm, r.owner, 4.0f);
	r.frame();
	CHECK_FALSE(r.player().playing);
	CHECK(reg.get<TransformComponent>(door).position.x == doctest::Approx(4.0f));
	reg.get<TransformComponent>(door).position.x = -1.0f;
	r.frame();
	CHECK(reg.get<TransformComponent>(door).position.x == -1.0f);

	// Playing from 1 s, then a jump to 6: Boom1/Boom2 lie between and stay quiet.
	SequenceSystem::setTime(r.world, r.cm, r.owner, 1.0f);
	SequenceSystem::play(r.world, r.cm, r.owner);
	r.frame();
	SequenceSystem::setTime(r.world, r.cm, r.owner, 6.0f);
	std::vector<std::string> all;
	for (int i = 0; i < 3; ++i) { const auto f = r.frame(); all.insert(all.end(), f.begin(), f.end()); }
	CHECK(countOf(all, "Boom1") == 0);
	CHECK(countOf(all, "Boom2") == 0);
	CHECK(countOf(all, "Late") == 1);
	CHECK(r.player().time == doctest::Approx(6.3f));

	// Out of range is clamped.
	SequenceSystem::setTime(r.world, r.cm, r.owner, 99.0f);
	CHECK(r.player().time == doctest::Approx(10.0f));
}

TEST_CASE("sequence runtime: a slot override binds an actor spawned at runtime")
{
	Rig r;
	SequenceAsset s;
	s.duration = 1.0f;
	s.bindings = { { 3, "Player", HE::UUID{} } };
	s.tracks.push_back(propertyTrack(3, PropTarget::PosY, { 0.0f, 1.0f }, { 0.0f, 1.0f }));
	r.make(std::move(s));
	const entt::entity hero = r.world.createEntity("Hero");   // no entity id at all
	r.world.addComponent(hero, TransformComponent{ .position = {}, .rotation = {}, .scale = glm::vec3(1.0f) });

	SequenceSystem::bindSlot(r.world, r.owner, 3, hero);
	r.frame();
	CHECK(r.world.registry().get<TransformComponent>(hero).position.y == doctest::Approx(0.1f));

	// Cleared mid-play: the slot is empty again from the next frame on.
	SequenceSystem::bindSlot(r.world, r.owner, 3, entt::null);
	r.frame();
	CHECK(r.world.registry().get<TransformComponent>(hero).position.y == doctest::Approx(0.1f));
}

TEST_CASE("sequence runtime: a sequence that is not loaded waits, and a zero-length one ends at once")
{
	Rig r;
	r.make(SequenceAsset{});
	// Swap in an id nothing knows: the player waits instead of failing.
	r.player().sequenceId = HE::UUID::generate();
	r.frame();
	r.frame();
	CHECK(r.player().playing);
	CHECK(r.player().time == 0.0f);

	// Back to the empty (zero-length) sequence: it is written once and stops.
	r.player().sequenceId = r.seqId;
	r.frame();
	CHECK_FALSE(r.player().playing);
}

// ─── Skeleton ────────────────────────────────────────────────────────────────

TEST_CASE("sequence runtime: section rule — latest start wins, seams hand over, clip time wraps or holds")
{
	SequenceTrack t;
	t.kind = SequenceTrackKind::Skeletal;
	SequenceSkeletalSection a; a.start = 2.0f; a.end = 4.0f;
	SequenceSkeletalSection b; b.start = 0.0f; b.end = 2.0f;
	t.sections = { a, b };   // listed out of order on purpose

	CHECK(SequenceSystem::activeSection(t, 1.0f)->start == 0.0f);
	CHECK(SequenceSystem::activeSection(t, 2.0f)->start == 2.0f);   // the seam: the later start
	CHECK(SequenceSystem::activeSection(t, 4.0f)->start == 2.0f);   // the end is inclusive
	CHECK(SequenceSystem::activeSection(t, 4.5f) == nullptr);
	CHECK(SequenceSystem::activeSection(t, -0.5f) == nullptr);

	SequenceSkeletalSection s;
	s.start = 1.0f; s.end = 9.0f; s.clipOffset = 0.25f; s.playRate = 2.0f;
	CHECK(SequenceSystem::sectionClipTime(s, 1.0f, 1.0f) == doctest::Approx(0.25f));
	CHECK(SequenceSystem::sectionClipTime(s, 1.25f, 1.0f) == doctest::Approx(0.75f));
	CHECK(SequenceSystem::sectionClipTime(s, 2.0f, 1.0f) == doctest::Approx(1.0f));    // held
	s.loop = true;
	CHECK(SequenceSystem::sectionClipTime(s, 2.0f, 1.0f) == doctest::Approx(0.25f));   // 2.25 wrapped
}

TEST_CASE("sequence runtime: a skeletal section takes the skeleton from its animator and hands it back")
{
	Rig r;
	const HE::UUID meshId       = r.cm.registerSkeletalMesh(oneBoneMesh());
	const HE::UUID animatorClip = r.cm.registerAnimationClip(translationClip(1.0f, 1.0f));
	const HE::UUID cutsceneClip = r.cm.registerAnimationClip(translationClip(-8.0f, 2.0f));

	const entt::entity hero = makeActor(r.world, "Hero");
	SkeletalMeshComponent smc;
	smc.meshAssetId = meshId;
	r.world.registry().emplace<SkeletalMeshComponent>(hero, smc);
	AnimatorComponent an;
	an.clipAssetId = animatorClip;
	an.looping     = true;
	r.world.registry().emplace<AnimatorComponent>(hero, an);

	SequenceAsset s;
	s.duration = 3.0f;
	s.bindings = { { 0, "Hero", r.world.entityId(hero) } };
	SequenceTrack sk;
	sk.kind    = SequenceTrackKind::Skeletal;
	sk.binding = 0;
	SequenceSkeletalSection sec;
	// Between frames on purpose (the clock steps 0.1), so which frame first
	// lands inside does not hang on float rounding.
	sec.clipId = cutsceneClip; sec.start = 0.45f; sec.end = 1.45f; sec.clipOffset = 0.0f;
	sk.sections.push_back(sec);
	s.tracks.push_back(sk);
	r.make(std::move(s));

	auto& reg = r.world.registry();
	const auto& pose = reg.get<SkeletalMeshComponent>(hero);
	const auto& anim = reg.get<AnimatorComponent>(hero);

	// Before the section: the animator runs as ever.
	for (int i = 0; i < 3; ++i) r.frame();
	CHECK(anim.playbackTime == doctest::Approx(0.3f));
	CHECK(rootX(pose) == doctest::Approx(0.3f));

	// Inside it: the sequence's clip, at section time; the animator's clock stands.
	for (int i = 0; i < 4; ++i) r.frame();   // sequence time 0.7 → clip time 0.25 → -1.0
	CHECK(r.player().time == doctest::Approx(0.7f));
	CHECK(pose.sequencePosed);
	CHECK(rootX(pose) == doctest::Approx(-1.0f));
	CHECK(anim.playbackTime == doctest::Approx(0.4f));   // stopped when the section began

	// After it: handed back, and the animator carries on from where it stood.
	for (int i = 0; i < 9; ++i) r.frame();   // sequence time 1.6
	CHECK_FALSE(pose.sequencePosed);
	CHECK(anim.playbackTime > 0.4f);
	CHECK(rootX(pose) == doctest::Approx(anim.playbackTime));
}

TEST_CASE("sequence runtime: a section whose clip is not loaded leaves the animator in charge")
{
	Rig r;
	const HE::UUID meshId       = r.cm.registerSkeletalMesh(oneBoneMesh());
	const HE::UUID animatorClip = r.cm.registerAnimationClip(translationClip(1.0f, 1.0f));

	const entt::entity hero = makeActor(r.world, "Hero");
	SkeletalMeshComponent smc;
	smc.meshAssetId = meshId;
	r.world.registry().emplace<SkeletalMeshComponent>(hero, smc);
	AnimatorComponent an;
	an.clipAssetId = animatorClip;
	r.world.registry().emplace<AnimatorComponent>(hero, an);

	SequenceAsset s;
	s.duration = 2.0f;
	s.bindings = { { 0, "Hero", r.world.entityId(hero) } };
	SequenceTrack sk;
	sk.kind    = SequenceTrackKind::Skeletal;
	sk.binding = 0;
	SequenceSkeletalSection sec;
	sec.clipId = HE::UUID::generate();   // still streaming, or gone
	sec.start = 0.0f; sec.end = 2.0f;
	sk.sections.push_back(sec);
	s.tracks.push_back(sk);
	r.make(std::move(s));

	for (int i = 0; i < 3; ++i) r.frame();
	const auto& pose = r.world.registry().get<SkeletalMeshComponent>(hero);
	CHECK_FALSE(pose.sequencePosed);
	CHECK(r.world.registry().get<AnimatorComponent>(hero).playbackTime == doctest::Approx(0.3f));
	CHECK(rootX(pose) == doctest::Approx(0.3f));
}

// ─── Sound ───────────────────────────────────────────────────────────────────

TEST_CASE("sequence runtime: sound starts when the playhead crosses its section, and stop() silences it")
{
	AudioEngine engine;
	REQUIRE(engine.init(true));   // no device: nothing pulls frames, a started sound stays started

	Rig r;
	r.ctx.audio = &engine;
	AudioAsset a;
	a.sampleRate = 44100;
	a.channels   = 1;
	a.encoding   = AudioEncoding::PCM16;
	a.audioData.assign(44100 * 2, 0);
	const HE::UUID soundId = r.cm.registerAudio(std::move(a));

	const entt::entity door = makeActor(r.world, "Door");
	SequenceAsset s;
	s.duration = 2.0f;
	s.bindings = { { 0, "Door", r.world.entityId(door) } };
	SequenceTrack music;
	music.kind = SequenceTrackKind::Audio;
	music.audio.push_back({ soundId, 0.0f, 1.0f, 1.0f });    // flat, on the first frame
	s.tracks.push_back(music);
	SequenceTrack creak;
	creak.kind    = SequenceTrackKind::Audio;
	creak.binding = 0;
	creak.audio.push_back({ soundId, 0.45f, 1.0f, 1.0f });   // at the door
	s.tracks.push_back(creak);
	r.make(std::move(s));

	r.frame();
	CHECK(r.player().audioHandles.size() == 1);
	for (int i = 0; i < 3; ++i) r.frame();
	CHECK(r.player().audioHandles.size() == 1);
	r.frame();   // 0.4 → 0.5 crosses the creak
	REQUIRE(r.player().audioHandles.size() == 2);
	const std::vector<uint64_t> handles = r.player().audioHandles;
	for (uint64_t h : handles) CHECK(engine.isPlaying(h));

	// Paused with the clock — and kept, although isPlaying() now says false.
	SequenceSystem::pause(r.world, r.owner);
	r.frame();
	REQUIRE(r.player().audioHandles.size() == 2);
	for (uint64_t h : handles) CHECK(engine.isPaused(h));
	SequenceSystem::play(r.world, r.cm, r.owner);
	r.frame();
	for (uint64_t h : handles) { CHECK(engine.isPlaying(h)); CHECK_FALSE(engine.isPaused(h)); }

	// A jump back over both starts does not restart them.
	SequenceSystem::setTime(r.world, r.cm, r.owner, 0.2f);
	r.frame();
	CHECK(r.player().audioHandles.size() == 2);

	SequenceSystem::stop(r.world, r.owner);
	r.frame();
	CHECK(r.player().audioHandles.empty());
	for (uint64_t h : handles) CHECK_FALSE(engine.isPlaying(h));
	engine.shutdown();
}

TEST_CASE("sequence runtime: backwards, events mirror and no sound starts")
{
	AudioEngine engine;
	REQUIRE(engine.init(true));
	Rig r;
	r.ctx.audio = &engine;
	AudioAsset a;
	a.sampleRate = 44100; a.channels = 1; a.encoding = AudioEncoding::PCM16;
	a.audioData.assign(4410 * 2, 0);
	const HE::UUID soundId = r.cm.registerAudio(std::move(a));

	SequenceAsset s;
	s.duration = 1.0f;
	s.tracks.push_back(eventTrack(kSequenceNoBinding, { { "Mid", 0.5f, 0.0f } }));
	SequenceTrack snd;
	snd.kind = SequenceTrackKind::Audio;
	snd.audio.push_back({ soundId, 0.5f, 1.0f, 1.0f });
	s.tracks.push_back(snd);
	r.make(std::move(s), /*autoplay=*/false, false, /*rate=*/-1.0f);

	SequenceSystem::setTime(r.world, r.cm, r.owner, 1.0f);
	SequenceSystem::play(r.world, r.cm, r.owner);
	std::vector<std::string> all;
	for (int i = 0; i < 12; ++i) { const auto f = r.frame(); all.insert(all.end(), f.begin(), f.end()); }
	CHECK(countOf(all, "Mid") == 1);
	CHECK(r.player().audioHandles.empty());
	CHECK_FALSE(r.player().playing);
	CHECK(r.player().time == 0.0f);
	engine.shutdown();
}

// ─── Scene ───────────────────────────────────────────────────────────────────

TEST_CASE("sequence runtime: the player saves its authored half only, in both formats")
{
	for (SerializeFormat fmt : { SerializeFormat::JSON, SerializeFormat::Binary })
	{
		const fs::path file = fs::temp_directory_path() / "he_test_sequenceplayer.hescene";
		HorizonWorld world;
		const entt::entity e = world.createEntity("Owner");
		SequencePlayerComponent sp;
		sp.sequenceId = HE::UUID::generate();
		sp.autoplay   = false;
		sp.loop       = true;
		sp.playRate   = -0.5f;
		sp.blendOutSeconds = 1.25f;
		sp.blendOutCurve   = HE::BlendCurve::EaseOut;
		sp.lockPlayerInput = true;
		sp.time       = 3.0f;    // session state: must not come back
		sp.playing    = true;
		sp.started    = true;
		sp.cameraOwned = true;
		world.registry().emplace<SequencePlayerComponent>(e, sp);

		SceneSerializer ser;
		REQUIRE(ser.save(world, file, fmt));
		HorizonWorld loaded;
		REQUIRE(ser.load(loaded, file, fmt));

		bool found = false;
		for (auto [le, lsp] : loaded.registry().view<SequencePlayerComponent>().each())
		{
			found = true;
			CHECK(lsp.sequenceId == sp.sequenceId);
			CHECK_FALSE(lsp.autoplay);
			CHECK(lsp.loop);
			CHECK(lsp.playRate == doctest::Approx(-0.5f));
			CHECK(lsp.blendOutSeconds == doctest::Approx(1.25f));
			CHECK(lsp.blendOutCurve == HE::BlendCurve::EaseOut);
			CHECK(lsp.lockPlayerInput);
			CHECK_FALSE(lsp.cameraOwned);
			CHECK(lsp.time == 0.0f);
			CHECK_FALSE(lsp.playing);
			CHECK_FALSE(lsp.started);
		}
		CHECK(found);
		he_test::removeQuiet(file);
	}
}

TEST_CASE("sequence runtime: a scene names the sequence of its player for streaming")
{
	HorizonWorld world;
	SequencePlayerComponent sp;
	sp.sequenceId = HE::UUID::generate();
	world.registry().emplace<SequencePlayerComponent>(world.createEntity("Owner"), sp);
	const auto refs = SceneSystems::collectAssetRefs(world);
	CHECK(std::find(refs.begin(), refs.end(), sp.sequenceId) != refs.end());
}

// ─── Camera and input (plan step 4) ──────────────────────────────────────────
// The sequence holds the camera from its first live cut until it lets go: it
// cuts, blends in and between its own cameras, and hands the view back through
// CameraRigController::blendTo. `appFrame` below is the order both applications
// run: the camera controller (gated on ownsCamera) BEFORE the animation phase.

namespace
{
	entt::entity makeCamera(HorizonWorld& world, const char* name, glm::vec3 pos,
	                        float fov = 60.0f, bool main = false)
	{
		const entt::entity e = makeActor(world, name);
		world.registry().get<TransformComponent>(e).position = pos;
		CameraComponent cam;
		cam.fovDegrees = fov;
		cam.isMain     = main;
		world.registry().emplace<CameraComponent>(e, cam);
		return e;
	}

	SequenceTrack cutTrack(std::vector<SequenceCameraCut> cuts)
	{
		SequenceTrack t;
		t.kind = SequenceTrackKind::CameraCut;
		t.cuts = std::move(cuts);
		return t;
	}

	bool isMain(const entt::registry& reg, entt::entity e) { return reg.get<CameraComponent>(e).isMain; }

	int mainCount(const entt::registry& reg)
	{
		int n = 0;
		for (auto [e, cam] : reg.view<const CameraComponent>().each()) n += cam.isMain ? 1 : 0;
		return n;
	}

	glm::vec3 posOf(const entt::registry& reg, entt::entity e) { return reg.get<TransformComponent>(e).position; }
}

TEST_CASE("sequence camera: the first cut takes the camera, the end gives it back")
{
	Rig r;
	auto& reg = r.world.registry();
	const entt::entity gameplay = makeCamera(r.world, "Gameplay", { 0, 2, 0 }, 60.0f, true);
	const entt::entity shot     = makeCamera(r.world, "Shot",     { 10, 0, 0 }, 40.0f);
	SequenceAsset s;
	s.duration = 1.0f;
	s.bindings = { { 0, "Shot", r.world.entityId(shot) } };
	s.tracks.push_back(cutTrack({ { 0.45f, 0, 0.0f } }));
	r.make(std::move(s));

	// Before the cut the view is gameplay's.
	for (int i = 0; i < 4; ++i) r.frame();
	CHECK_FALSE(SequenceSystem::ownsCamera(reg));
	CHECK(isMain(reg, gameplay));

	// The frame that crosses the cut switches, and only one camera is main.
	r.frame();
	CHECK(SequenceSystem::ownsCamera(reg));
	CHECK(r.player().cameraOwned);
	CHECK(isMain(reg, shot));
	CHECK(mainCount(reg) == 1);
	// A hard cut writes nothing into the shot camera: it shows where it stands.
	CHECK(posOf(reg, shot) == glm::vec3(10, 0, 0));
	CHECK(reg.get<CameraComponent>(shot).fovOffset == 0.0f);

	// The end: the last frame is shown, then the view goes back — a cut, since
	// the gameplay camera has no rig — in the same frame. (One frame spare for
	// float accumulation onto 1.0.)
	for (int i = 0; i < 6; ++i) r.frame();
	CHECK_FALSE(r.player().playing);
	CHECK_FALSE(SequenceSystem::ownsCamera(reg));
	CHECK_FALSE(r.player().cameraOwned);
	CHECK(isMain(reg, gameplay));
	CHECK(mainCount(reg) == 1);
	// A camera without a rig is not moved on the way back: a fly camera would be
	// teleported to where the cutscene ended.
	CHECK(posOf(reg, gameplay) == glm::vec3(0, 2, 0));
}

TEST_CASE("sequence camera: a camera controller gated on ownsCamera cannot move the cutscene camera")
{
	// The applications' gate, as a stand-in for the fly fallback (which reads SDL
	// and cannot run headless): each frame it moves the main camera a metre,
	// unless a sequence holds the camera.
	Rig r;
	auto& reg = r.world.registry();
	const entt::entity gameplay = makeCamera(r.world, "Gameplay", { 0, 0, 0 }, 60.0f, true);
	const entt::entity shot     = makeCamera(r.world, "Shot",     { 10, 0, 0 });
	SequenceAsset s;
	s.duration = 0.5f;
	s.bindings = { { 0, "Shot", r.world.entityId(shot) } };
	s.tracks.push_back(cutTrack({ { 0.0f, 0, 0.0f } }));
	r.make(std::move(s));

	auto flyStandIn = [&](bool gated)
	{
		if (gated && SequenceSystem::ownsCamera(reg)) return;
		for (auto [e, cam, t] : reg.view<CameraComponent, TransformComponent>().each())
			if (cam.isMain) t.position.x += 1.0f;
	};

	r.frame();                       // takes the camera
	REQUIRE(isMain(reg, shot));
	for (int i = 0; i < 3; ++i) { flyStandIn(true); r.frame(); }
	CHECK(posOf(reg, shot).x == 10.0f);
	CHECK(posOf(reg, gameplay).x == 0.0f);

	// Negative control: the same stand-in without the gate does move it — the
	// assertion above is about the gate, not about a controller that did nothing.
	flyStandIn(false);
	CHECK(posOf(reg, shot).x == 11.0f);

	// After the hand-back the controller has its camera again.
	for (int i = 0; i < 3; ++i) r.frame();
	REQUIRE_FALSE(SequenceSystem::ownsCamera(reg));
	flyStandIn(true);
	CHECK(posOf(reg, gameplay).x == 1.0f);
}

TEST_CASE("sequence camera: a blend in starts at the gameplay pose frozen when the camera was taken")
{
	Rig r;
	auto& reg = r.world.registry();
	const entt::entity gameplay = makeCamera(r.world, "Gameplay", { 0, 0, 0 }, 60.0f, true);
	const entt::entity shot     = makeCamera(r.world, "Shot",     { 10, 0, 0 }, 90.0f);
	reg.get<TransformComponent>(shot).rotation = { 0.0f, 90.0f, 0.0f };
	SequenceAsset s;
	s.duration = 2.0f;
	s.bindings = { { 0, "Shot", r.world.entityId(shot) } };
	s.tracks.push_back(cutTrack({ { 0.0f, 0, 1.0f, SequenceBlendCurve::Linear } }));
	r.make(std::move(s));

	r.frame();   // t = 0.1: taken, 10 % of the way
	REQUIRE(isMain(reg, shot));
	CHECK(posOf(reg, shot).x == doctest::Approx(1.0f));
	CHECK(reg.get<CameraComponent>(shot).fovOffset == doctest::Approx(63.0f - 90.0f));
	// The source is FROZEN: gameplay moving on after the take changes nothing.
	reg.get<TransformComponent>(gameplay).position = { 100, 100, 100 };

	for (int i = 0; i < 4; ++i) r.frame();   // t = 0.5
	CHECK(posOf(reg, shot).x == doctest::Approx(5.0f));
	CHECK(posOf(reg, shot).y == doctest::Approx(0.0f).epsilon(1e-4));
	CHECK(reg.get<TransformComponent>(shot).rotation.y == doctest::Approx(45.0f).epsilon(1e-3));
	CHECK(reg.get<CameraComponent>(shot).fovOffset == doctest::Approx(75.0f - 90.0f));
	// Its authored FOV is never written, only the offset.
	CHECK(reg.get<CameraComponent>(shot).fovDegrees == 90.0f);

	// Past the blend the shot camera is exactly where it was placed again — the
	// blended pose was this frame's output, never its state.
	for (int i = 0; i < 6; ++i) r.frame();   // t = 1.1
	CHECK(posOf(reg, shot) == glm::vec3(10, 0, 0));
	CHECK(reg.get<TransformComponent>(shot).rotation == glm::vec3(0, 90, 0));
	CHECK(reg.get<CameraComponent>(shot).fovOffset == 0.0f);
}

TEST_CASE("sequence camera: a blend between two cutscene cameras reads both at the same instant")
{
	Rig r;
	auto& reg = r.world.registry();
	makeCamera(r.world, "Gameplay", { 0, 0, 0 }, 60.0f, true);
	const entt::entity a = makeCamera(r.world, "A", { 0, 0, 0 });
	const entt::entity b = makeCamera(r.world, "B", { 20, 0, 0 });
	SequenceAsset s;
	s.duration = 3.0f;
	s.bindings = { { 0, "A", r.world.entityId(a) }, { 1, "B", r.world.entityId(b) } };
	// B is dollying along Z while the view travels to it.
	s.tracks.push_back(propertyTrack(1, PropTarget::PosZ, { 0.0f, 2.0f }, { 0.0f, 10.0f }));
	// The cut sits off the frame grid on purpose, so no frame lands on it.
	s.tracks.push_back(cutTrack({ { 0.0f, 0, 0.0f }, { 0.95f, 1, 1.0f, SequenceBlendCurve::Linear } }));
	r.make(std::move(s));

	for (int i = 0; i < 10; ++i) r.frame();   // t = 1.0: just past the cut
	CHECK(isMain(reg, b));
	CHECK(mainCount(reg) == 1);
	for (int i = 0; i < 5; ++i) r.frame();    // t = 1.5: alpha 0.55
	// 55 % from A (0,0,0) to B where its track has it NOW (20,0,7.5).
	CHECK(posOf(reg, b).x == doctest::Approx(11.0f));
	CHECK(posOf(reg, b).z == doctest::Approx(4.125f));
	CHECK(posOf(reg, a) == glm::vec3(0, 0, 0));

	// After the blend B carries its track's value and its placed X again.
	for (int i = 0; i < 10; ++i) r.frame();   // t = 2.5
	CHECK(posOf(reg, b).x == doctest::Approx(20.0f));
	CHECK(posOf(reg, b).z == doctest::Approx(10.0f));
}

TEST_CASE("sequence camera: the view goes back to the rig with a blend out, without a stale frame")
{
	Rig r;
	auto& reg = r.world.registry();
	const entt::entity target = makeActor(r.world, "Player");
	const entt::entity rigCam = makeCamera(r.world, "RigCam", { 0, 0, 0 }, 60.0f, true);
	CameraRigComponent rc;
	rc.mode      = CameraRigComponent::Mode::ThirdPerson;
	rc.target    = r.world.entityId(target);
	rc.yaw       = 0.0f;
	rc.pitch     = 0.0f;
	rc.armLength = 4.0f;
	reg.emplace<CameraRigComponent>(rigCam, rc);
	const entt::entity shot = makeCamera(r.world, "Shot", { 50, 5, 0 });

	SequenceAsset s;
	s.duration = 0.25f;   // ends inside the third frame
	s.bindings = { { 0, "Shot", r.world.entityId(shot) } };
	s.tracks.push_back(cutTrack({ { 0.0f, 0, 0.0f } }));
	r.make(std::move(s));
	r.player().blendOutSeconds = 1.0f;
	r.player().blendOutCurve   = HE::BlendCurve::Linear;

	HE::CameraLookInput look;
	look.dt = kDt;
	auto appFrame = [&]
	{
		if (!SequenceSystem::ownsCamera(reg)) HE::CameraRigController::update(r.world, look);
		r.frame();
	};

	appFrame();
	REQUIRE(isMain(reg, shot));
	const glm::vec3 rigPose = posOf(reg, rigCam);   // where the rig put it before the take
	CHECK(rigPose.z == doctest::Approx(4.0f));      // the boom behind the target

	// The rig is not run during the cutscene, so it cannot fight the shot.
	reg.get<TransformComponent>(target).position = { 0, 0, -10 };
	appFrame();
	CHECK(posOf(reg, rigCam) == rigPose);

	appFrame();   // t = 0.25: the end — hand-back in this frame
	REQUIRE_FALSE(r.player().playing);
	CHECK(isMain(reg, rigCam));
	CHECK(mainCount(reg) == 1);
	CHECK(reg.get<CameraRigComponent>(rigCam).isBlending());
	// The frame of the hand-over shows the cutscene's last pose, not the one the
	// rig held before the cutscene.
	CHECK(posOf(reg, rigCam) == glm::vec3(50, 5, 0));

	// Next frame the rig blends from there towards where its target is NOW —
	// solved fresh, not sailing in from the lag pose it had before.
	appFrame();
	const glm::vec3 solved = rigPose + glm::vec3(0.0f, 0.0f, -10.0f);
	const glm::vec3 tenth  = glm::mix(glm::vec3(50, 5, 0), solved, 0.1f);
	CHECK(posOf(reg, rigCam).x == doctest::Approx(tenth.x));
	CHECK(posOf(reg, rigCam).y == doctest::Approx(tenth.y));
	CHECK(posOf(reg, rigCam).z == doctest::Approx(tenth.z));
	for (int i = 0; i < 10; ++i) appFrame();
	CHECK_FALSE(reg.get<CameraRigComponent>(rigCam).isBlending());
	CHECK(posOf(reg, rigCam).x == doctest::Approx(solved.x));
	CHECK(posOf(reg, rigCam).y == doctest::Approx(solved.y));
	CHECK(posOf(reg, rigCam).z == doctest::Approx(solved.z));
}

TEST_CASE("sequence camera: taking the view gives back a first-person rig's hidden body")
{
	Rig r;
	auto& reg = r.world.registry();
	const entt::entity target = makeActor(r.world, "Player");
	reg.emplace<MeshComponent>(target);
	const entt::entity rigCam = makeCamera(r.world, "RigCam", { 0, 0, 0 }, 60.0f, true);
	CameraRigComponent rc;
	rc.mode           = CameraRigComponent::Mode::FirstPerson;
	rc.target         = r.world.entityId(target);
	rc.hideTargetMesh = true;
	reg.emplace<CameraRigComponent>(rigCam, rc);
	const entt::entity shot = makeCamera(r.world, "Shot", { 5, 0, 0 });

	HE::CameraRigController::update(r.world, MouseFrame{});
	REQUIRE_FALSE(reg.get<MeshComponent>(target).visible);

	SequenceAsset s;
	s.duration = 1.0f;
	s.bindings = { { 0, "Shot", r.world.entityId(shot) } };
	s.tracks.push_back(cutTrack({ { 0.0f, 0, 0.0f } }));
	r.make(std::move(s));
	r.frame();
	REQUIRE(isMain(reg, shot));
	// The cutscene shows the player; a hidden body would be a hole in the shot.
	CHECK(reg.get<MeshComponent>(target).visible);
	CHECK((reg.get<CameraRigComponent>(rigCam).meshHiddenEntity == entt::null));
}

TEST_CASE("sequence camera: stop(), a cut to no camera and a missing camera give the view back")
{
	Rig r;
	auto& reg = r.world.registry();
	const entt::entity gameplay = makeCamera(r.world, "Gameplay", { 0, 0, 0 }, 60.0f, true);
	const entt::entity shot     = makeCamera(r.world, "Shot",     { 10, 0, 0 });
	const entt::entity prop     = makeActor(r.world, "NotACamera");
	SequenceAsset s;
	s.duration = 4.0f;
	s.bindings = { { 0, "Shot", r.world.entityId(shot) }, { 1, "Prop", r.world.entityId(prop) } };
	// Shot, back to gameplay, Shot again, then a "camera" that is not one.
	s.tracks.push_back(cutTrack({ { 0.0f, 0, 0.0f },
	                              { 1.0f, kSequenceNoBinding, 0.0f },
	                              { 2.0f, 0, 0.0f },
	                              { 3.0f, 1, 0.0f } }));
	r.make(std::move(s));

	r.frame();
	CHECK(isMain(reg, shot));
	for (int i = 0; i < 10; ++i) r.frame();   // t = 1.1: past the cut to none
	CHECK_FALSE(SequenceSystem::ownsCamera(reg));
	CHECK(isMain(reg, gameplay));
	CHECK(r.player().playing);
	for (int i = 0; i < 10; ++i) r.frame();   // t = 2.1: taken again
	CHECK(SequenceSystem::ownsCamera(reg));
	CHECK(isMain(reg, shot));
	for (int i = 0; i < 10; ++i) r.frame();   // t = 3.1: slot 1 is no camera
	CHECK_FALSE(SequenceSystem::ownsCamera(reg));
	CHECK(isMain(reg, gameplay));

	// stop() mid-shot: the view is back by the end of the next frame.
	SequenceSystem::setTime(r.world, r.cm, r.owner, 0.5f);
	r.frame();
	REQUIRE(isMain(reg, shot));
	SequenceSystem::stop(r.world, r.owner);
	r.frame();
	CHECK_FALSE(SequenceSystem::ownsCamera(reg));
	CHECK(isMain(reg, gameplay));
	CHECK(mainCount(reg) == 1);
}

TEST_CASE("sequence camera: one sequence holds the camera at a time, a destroyed owner still hands back")
{
	Rig r;
	auto& reg = r.world.registry();
	const entt::entity gameplay = makeCamera(r.world, "Gameplay", { 0, 0, 0 }, 60.0f, true);
	const entt::entity shotA    = makeCamera(r.world, "ShotA", { 10, 0, 0 });
	const entt::entity shotB    = makeCamera(r.world, "ShotB", { 20, 0, 0 });

	SequenceAsset sa;
	sa.duration = 0.5f;
	sa.bindings = { { 0, "ShotA", r.world.entityId(shotA) } };
	sa.tracks.push_back(cutTrack({ { 0.0f, 0, 0.0f } }));
	r.make(std::move(sa));

	SequenceAsset sb;
	sb.duration = 5.0f;
	sb.bindings = { { 0, "ShotB", r.world.entityId(shotB) } };
	sb.tracks.push_back(cutTrack({ { 0.0f, 0, 0.0f } }));
	const HE::UUID idB = r.cm.registerSequence(std::move(sb));
	const entt::entity ownerB = r.world.createEntity("OwnerB");
	SequencePlayerComponent spB;
	spB.sequenceId = idB;
	spB.autoplay   = false;
	reg.emplace<SequencePlayerComponent>(ownerB, spB);

	r.frame();
	REQUIRE(isMain(reg, shotA));
	// B starts while A holds the camera: it plays, but waits for the view.
	REQUIRE(SequenceSystem::play(r.world, r.cm, ownerB));
	r.frame();
	CHECK(isMain(reg, shotA));
	CHECK_FALSE(reg.get<SequencePlayerComponent>(ownerB).cameraOwned);

	// A ends and hands back; B takes it no later than the frame after.
	for (int i = 0; i < 5; ++i) r.frame();
	CHECK_FALSE(r.player().playing);
	CHECK(isMain(reg, shotB));
	CHECK(reg.get<SequencePlayerComponent>(ownerB).cameraOwned);
	CHECK(mainCount(reg) == 1);

	// B's owner is destroyed mid-cutscene: the lease outlives it and the view
	// still goes back to the camera that had it — gameplay's, as A left it.
	r.world.destroyEntity(ownerB);
	CHECK(SequenceSystem::ownsCamera(reg));   // until the next frame hands it back
	r.frame();
	CHECK_FALSE(SequenceSystem::ownsCamera(reg));
	CHECK(isMain(reg, gameplay));
}

TEST_CASE("sequence camera: without a session the lease is dropped")
{
	Rig r;
	auto& reg = r.world.registry();
	makeCamera(r.world, "Gameplay", { 0, 0, 0 }, 60.0f, true);
	const entt::entity shot = makeCamera(r.world, "Shot", { 10, 0, 0 });
	SequenceAsset s;
	s.duration = 5.0f;
	s.bindings = { { 0, "Shot", r.world.entityId(shot) } };
	s.tracks.push_back(cutTrack({ { 0.0f, 0, 0.0f } }));
	r.make(std::move(s));
	r.frame();
	REQUIRE(SequenceSystem::ownsCamera(reg));
	// The editor's edit frames between two play sessions: a lease that lived on
	// would lock the camera of the next session, whose scene was reloaded.
	r.frame(kDt, false);
	CHECK_FALSE(SequenceSystem::ownsCamera(reg));
}

TEST_CASE("sequence input: Lock Player Input holds while playing or paused, not after")
{
	Rig r;
	SequenceAsset s;
	s.duration = 0.3f;
	r.make(std::move(s), false);
	auto& reg = r.world.registry();
	CHECK_FALSE(SequenceSystem::locksPlayerInput(reg));

	r.player().lockPlayerInput = true;
	CHECK_FALSE(SequenceSystem::locksPlayerInput(reg));   // not playing yet
	REQUIRE(SequenceSystem::play(r.world, r.cm, r.owner));
	r.frame();
	CHECK(SequenceSystem::locksPlayerInput(reg));
	SequenceSystem::pause(r.world, r.owner);
	r.frame();
	CHECK(SequenceSystem::locksPlayerInput(reg));
	REQUIRE(SequenceSystem::play(r.world, r.cm, r.owner));
	for (int i = 0; i < 5; ++i) r.frame();
	CHECK_FALSE(r.player().playing);
	CHECK_FALSE(SequenceSystem::locksPlayerInput(reg));

	// Off on a playing player: nothing is locked.
	r.player().lockPlayerInput = false;
	REQUIRE(SequenceSystem::play(r.world, r.cm, r.owner));
	r.frame();
	CHECK_FALSE(SequenceSystem::locksPlayerInput(reg));
}
