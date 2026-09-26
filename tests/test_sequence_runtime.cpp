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
#include <HorizonScene/Components/AnimatorComponent.h>
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
		sp.time       = 3.0f;    // session state: must not come back
		sp.playing    = true;
		sp.started    = true;
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
