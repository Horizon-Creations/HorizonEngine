#include "doctest.h"

#include "CinematicPreview.h"
#include "CinematicTimeline.h"
#include "SequencerTimeline.h"   // insertKey — the fixtures key their tracks the strip's way
#include "UITimelineMath.h"

#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/TransformHierarchy.h>
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/MaterialComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/SkeletalMeshComponent.h>
#include <HorizonScene/Components/TransformComponent.h>

#include <imgui.h>

#include <algorithm>
#include <string>
#include <vector>

// ── The Cinematic tab, without a window ──────────────────────────────────────
// Hive topic 84, plan step 6 (docs/sequencer-cinematics-plan.md §3.6). Three
// things, each without a GPU:
//   * the editing functions keep the lists the strip draws sorted and a
//     binding's slot fixed for good;
//   * the strip itself, driven with mouse events over a SequenceAsset on the
//     stack (the Sequencer's strip test is the model);
//   * the preview bracket leaves the scene EXACTLY as it found it — local
//     transforms, the world matrices under the actors, the shared material,
//     visibility, the camera, the bones — however far it moved them.

using namespace HE::Ed::Cinematic;

namespace
{
	struct ImGuiCtx
	{
		ImGuiCtx()
		{
			ImGui::CreateContext();
			ImGuiIO& io = ImGui::GetIO();
			io.DisplaySize = ImVec2(1280.0f, 720.0f);
			io.DeltaTime   = 1.0f / 60.0f;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
			ImGui::GetStyle().WindowPadding = ImVec2(0.0f, 0.0f);
		}
		~ImGuiCtx() { ImGui::DestroyContext(); }
	};

	constexpr float kWinW = 1000.0f, kWinH = 400.0f;

	Result frame(SequenceAsset& seq, View& view, const Labels& labels = {})
	{
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(kWinW, kWinH));
		ImGui::Begin("##cintest", nullptr,
		             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);
		const Result r = draw(seq, view, ImVec2(kWinW, kWinH), labels);
		ImGui::End();
		ImGui::Render();
		return r;
	}

	HE::Ed::UITimelineView viewOf(const Result& r, const SequenceAsset& seq, const View& v)
	{
		return HE::Ed::UITimelineView{ r.laneX, r.laneW, seq.duration, v.zoom, v.scroll };
	}

	void mouseAt(float x, float y) { ImGui::GetIO().AddMousePosEvent(x, y); }
	void mouseButton(bool down)    { ImGui::GetIO().AddMouseButtonEvent(0, down); }
	void keyEvent(ImGuiKey key, bool down) { ImGui::GetIO().AddKeyEvent(key, down); }

	float rowCentre(const Result& r, int row)
	{
		return r.rowsTop + metrics().rowH * (static_cast<float>(row) + 0.5f);
	}

	// Hero (slot 0) and Cam (slot 1). Rows:
	//   0 Camera Cuts   1 [Hero]   2 Position X   3 Skeletal Animation
	//   4 [Cam]         5 [Unbound]   6 Events
	SequenceAsset cutscene()
	{
		SequenceAsset seq;
		seq.duration = 4.0f;
		addBinding(seq, "Hero", HE::UUID::generate());
		addBinding(seq, "Cam", HE::UUID::generate());
		SequenceTrack& cuts = seq.tracks[addTrack(seq, SequenceTrackKind::CameraCut, kSequenceNoBinding)];
		insertCut(cuts, 0.0f, 1);
		insertCut(cuts, 2.0f, 1);
		const int pos = addPropertyTrack(seq, 0, PropTarget::PosX, 0.0f);
		HE::Ed::Sequencer::insertKey(seq.tracks[pos].channel, 2.0f, 4.0f);
		const int skel = addTrack(seq, SequenceTrackKind::Skeletal, 0);
		insertSection(seq.tracks[skel], 1.0f, 2.0f, HE::UUID::generate());
		const int ev = addTrack(seq, SequenceTrackKind::Event, kSequenceNoBinding);
		insertEvent(seq.tracks[ev], 1.0f, "Boom");
		return seq;
	}

	template <class T, class F>
	bool sortedBy(const std::vector<T>& v, F f)
	{
		for (size_t i = 1; i < v.size(); ++i) if (f(v[i]) < f(v[i - 1])) return false;
		return true;
	}
}

// ── Editing ──────────────────────────────────────────────────────────────────

TEST_CASE("cinematic: a binding's slot is handed out once and never renumbered")
{
	SequenceAsset seq;
	CHECK(addBinding(seq, "A", HE::UUID::generate()) == 0);
	CHECK(addBinding(seq, "B", HE::UUID::generate()) == 1);
	CHECK(addBinding(seq, "C", HE::UUID::generate()) == 2);

	// B's tracks and the cuts to B go with it; A's and C's keep their slots.
	const int cutTrack = addTrack(seq, SequenceTrackKind::CameraCut, kSequenceNoBinding);
	insertCut(seq.tracks[cutTrack], 0.0f, 1);
	insertCut(seq.tracks[cutTrack], 1.0f, 2);
	addPropertyTrack(seq, 0, PropTarget::PosX, 0.0f);
	addPropertyTrack(seq, 1, PropTarget::PosY, 0.0f);
	addTrack(seq, SequenceTrackKind::Event, 1);
	addPropertyTrack(seq, 2, PropTarget::PosZ, 0.0f);

	REQUIRE(removeBinding(seq, 1));
	CHECK_FALSE(removeBinding(seq, 1));
	REQUIRE(seq.bindings.size() == 2);
	CHECK(seq.bindings[0].slot == 0);
	CHECK(seq.bindings[1].slot == 2);
	const int ct = cameraCutTrack(seq);
	REQUIRE(ct >= 0);
	REQUIRE(seq.tracks[ct].cuts.size() == 1);
	CHECK(seq.tracks[ct].cuts[0].binding == 2);
	CHECK(findPropertyTrack(seq, 0, PropTarget::PosX) >= 0);
	CHECK(findPropertyTrack(seq, 2, PropTarget::PosZ) >= 0);
	CHECK(findPropertyTrack(seq, 1, PropTarget::PosY) < 0);
	for (const SequenceTrack& tr : seq.tracks)
		if (tr.kind != SequenceTrackKind::CameraCut) CHECK(tr.binding != 1);

	// The freed slot is not handed out again: a script's bindSlot or an old
	// copy of the asset may still name it.
	CHECK(addBinding(seq, "D", HE::UUID::generate()) == 3);
}

TEST_CASE("cinematic: one camera-cut track, one property track per target, skeletal needs an actor")
{
	SequenceAsset seq;
	CHECK(seq.duration == 0.0f);
	addBinding(seq, "Hero", HE::UUID::generate());
	const int p = addPropertyTrack(seq, 0, PropTarget::RotY, 90.0f);
	CHECK(seq.duration == doctest::Approx(kDefaultDuration));
	CHECK(addPropertyTrack(seq, 0, PropTarget::RotY, 0.0f) == p);
	// The first key holds the value handed in — the actor's own — so a new
	// track moves nothing.
	CHECK(seq.tracks[p].channel.values.at(0) == doctest::Approx(90.0f));
	CHECK(addPropertyTrack(seq, kSequenceNoBinding, PropTarget::PosX, 0.0f) == -1);

	// The cut track goes in FRONT, and there is only ever one.
	const int c = addTrack(seq, SequenceTrackKind::CameraCut, 0);
	CHECK(c == 0);
	CHECK(seq.tracks[0].kind == SequenceTrackKind::CameraCut);
	CHECK(seq.tracks[0].binding == kSequenceNoBinding);
	CHECK(addTrack(seq, SequenceTrackKind::CameraCut, kSequenceNoBinding) == 0);
	CHECK(cameraCutTrack(seq) == 0);

	CHECK(addTrack(seq, SequenceTrackKind::Skeletal, kSequenceNoBinding) == -1);
	CHECK(addTrack(seq, SequenceTrackKind::Skeletal, 0) >= 0);
	CHECK(addTrack(seq, SequenceTrackKind::Property, 0) == -1);
	CHECK(addTrack(seq, SequenceTrackKind::Audio, kSequenceNoBinding) >= 0);
}

TEST_CASE("cinematic: items stay in time order, and a moved one is found under its new index")
{
	SequenceTrack cuts;
	cuts.kind = SequenceTrackKind::CameraCut;
	CHECK(insertCut(cuts, 2.0f, 0) == 0);
	CHECK(insertCut(cuts, 1.0f, 1) == 0);
	// Same instant: after the one there — the later-listed wins a tie at
	// runtime, and the one just added is the one meant.
	CHECK(insertCut(cuts, 2.0f, 2) == 2);
	CHECK(cuts.cuts[2].binding == 2);
	CHECK(insertCut(cuts, -1.0f, 3) == 0);
	CHECK(cuts.cuts[0].time == 0.0f);

	// Moving the 1 s cut past the 2 s ones.
	const int k = moveItem(cuts, 1, 3.0f);
	CHECK(k == 3);
	CHECK(cuts.cuts[3].binding == 1);
	CHECK(sortedBy(cuts.cuts, [](const SequenceCameraCut& c) { return c.time; }));

	SequenceTrack ev;
	ev.kind = SequenceTrackKind::Event;
	insertEvent(ev, 1.0f, "A");
	insertEvent(ev, 0.5f, "B");
	CHECK(ev.events[0].name == "B");
	CHECK(moveItem(ev, 0, 2.0f) == 1);
	CHECK(ev.events[1].name == "B");
	CHECK(removeItem(ev, 0));
	CHECK_FALSE(removeItem(ev, 5));
	CHECK(ev.events.size() == 1);

	// Sections: a move keeps the length; the edges trim and never cross.
	SequenceTrack sk;
	sk.kind = SequenceTrackKind::Skeletal;
	insertSection(sk, 2.0f, 1.0f, HE::UUID::generate());
	insertSection(sk, 0.0f, 1.0f, HE::UUID::generate());
	CHECK(sk.sections[0].start == 0.0f);
	int s = moveItem(sk, 0, 4.0f);
	CHECK(s == 1);
	CHECK(sk.sections[1].start == doctest::Approx(4.0f));
	CHECK(sk.sections[1].end   == doctest::Approx(5.0f));
	s = setSectionEnd(sk, 1, 3.0f);   // before its own start
	CHECK(sk.sections[s].end == doctest::Approx(4.0f + kMinSection));
	s = setSectionStart(sk, s, 1.0f);   // re-sorts it in front
	CHECK(s == 0);
	CHECK(sk.sections[0].start == doctest::Approx(1.0f));
	s = setSectionStart(sk, 0, 10.0f);
	CHECK(sk.sections[s].start == doctest::Approx(sk.sections[s].end - kMinSection));

	// A property key goes through the Sequencer's rule: never onto another key.
	SequenceTrack pr;
	pr.kind = SequenceTrackKind::Property;
	HE::Ed::Sequencer::insertKey(pr.channel, 0.0f, 1.0f);
	HE::Ed::Sequencer::insertKey(pr.channel, 1.0f, 2.0f);
	moveItem(pr, 0, 1.0f);
	CHECK(pr.channel.times.size() == 2);
	CHECK(pr.channel.times[0] != pr.channel.times[1]);
}

TEST_CASE("cinematic: the length never cuts off the last thing that happens")
{
	SequenceAsset seq = cutscene();
	// The section ends at 3, the key sits at 2, the event at 1.
	CHECK(lastContentTime(seq) == doctest::Approx(3.0f));
	CHECK(setDuration(seq, 1.0f) == doctest::Approx(3.0f));
	CHECK(setDuration(seq, 10.0f) == doctest::Approx(10.0f));
	// An event state reaches as far as its end.
	const int ev = addTrack(seq, SequenceTrackKind::Event, kSequenceNoBinding);
	const int k = insertEvent(seq.tracks[ev], 4.0f, "Long");
	seq.tracks[ev].events[k].duration = 2.5f;
	CHECK(lastContentTime(seq) == doctest::Approx(6.5f));
}

TEST_CASE("cinematic: rows are the cut row, then each actor's group, then Unbound")
{
	SequenceAsset seq = cutscene();
	View view;
	std::vector<Row> rows = buildRows(seq, view);
	REQUIRE(rows.size() == 7);
	CHECK(rows[0].kind == Row::Kind::Track);
	CHECK(seq.tracks[rows[0].track].kind == SequenceTrackKind::CameraCut);
	CHECK((rows[1].kind == Row::Kind::Group && rows[1].slot == 0));
	CHECK(seq.tracks[rows[2].track].kind == SequenceTrackKind::Property);
	CHECK(seq.tracks[rows[3].track].kind == SequenceTrackKind::Skeletal);
	CHECK((rows[4].kind == Row::Kind::Group && rows[4].slot == 1));
	CHECK((rows[5].kind == Row::Kind::Group && rows[5].slot == kSequenceNoBinding));
	CHECK(seq.tracks[rows[6].track].kind == SequenceTrackKind::Event);
	CHECK(rows[2].depth == 1);

	// Folding Hero hides its two tracks, not its header.
	view.toggleFold(0);
	rows = buildRows(seq, view);
	CHECK(rows.size() == 5);
	view.toggleFold(0);
	CHECK(buildRows(seq, view).size() == 7);

	// A track whose slot names no binding, and a second cut track (dead at
	// runtime), are listed under Unbound so they can still be removed.
	SequenceTrack stray;
	stray.kind = SequenceTrackKind::Property;
	stray.binding = 42;
	stray.channel.times = { 0.0f }; stray.channel.values = { 0.0f };
	seq.tracks.push_back(stray);
	SequenceTrack cut2;
	cut2.kind = SequenceTrackKind::CameraCut;
	seq.tracks.push_back(cut2);
	rows = buildRows(seq, view);
	REQUIRE(rows.size() == 9);
	CHECK(rows[7].track == static_cast<int>(seq.tracks.size()) - 2);
	CHECK(rows[8].track == static_cast<int>(seq.tracks.size()) - 1);
}

TEST_CASE("cinematic: a new cut follows the live camera, else the first camera binding")
{
	SequenceAsset seq;
	addBinding(seq, "Door", HE::UUID::generate());
	addBinding(seq, "Cam", HE::UUID::generate());
	CHECK(defaultCutSlot(seq, 1.0f, {}) == 0);
	CHECK(defaultCutSlot(seq, 1.0f, { false, true }) == 1);
	SequenceTrack& tr = seq.tracks[addTrack(seq, SequenceTrackKind::CameraCut, kSequenceNoBinding)];
	insertCut(tr, 0.5f, 0);
	CHECK(defaultCutSlot(seq, 1.0f, { false, true }) == 0);   // Door is live at 1 s
	CHECK(defaultCutSlot(seq, 0.2f, { false, true }) == 1);   // nothing live yet
	SequenceAsset empty;
	CHECK(defaultCutSlot(empty, 0.0f, {}) == kSequenceNoBinding);
}

// ── The strip ────────────────────────────────────────────────────────────────

TEST_CASE("cinematic strip: the ruler scrubs and a flag drags along the axis as one edit")
{
	ImGuiCtx ctx;
	SequenceAsset seq = cutscene();
	View view;
	mouseAt(-100.0f, -100.0f);
	Result r = frame(seq, view);
	const HE::Ed::UITimelineView tv = viewOf(r, seq, view);

	// Scrub.
	mouseAt(tv.xOf(1.5f), r.top + metrics().rulerH * 0.5f);
	mouseButton(true);
	r = frame(seq, view);
	CHECK(view.playhead == doctest::Approx(1.5f).epsilon(0.01));
	CHECK(r.playheadMoved);
	mouseButton(false);
	frame(seq, view);

	// Press the 2 s cut: selected, the playhead on it, nothing edited yet.
	const float y = rowCentre(r, 0);
	mouseAt(tv.xOf(2.0f) + 2.0f, y);
	mouseButton(true);
	r = frame(seq, view);
	const int ct = cameraCutTrack(seq);
	CHECK(view.trackSel == ct);
	CHECK(view.itemSel == 1);
	CHECK(view.armed);
	CHECK_FALSE(r.edited);
	CHECK(view.playhead == doctest::Approx(2.0f));

	// Drag it to 3 s: it moves by the pointer's travel, not to under it.
	mouseAt(tv.xOf(3.0f) + 2.0f, y);
	r = frame(seq, view);
	CHECK(r.edited);
	CHECK_FALSE(r.committed);
	CHECK(seq.tracks[ct].cuts[1].time == doctest::Approx(3.0f).epsilon(0.01));
	mouseButton(false);
	r = frame(seq, view);
	CHECK(r.committed);
	CHECK_FALSE(view.armed);
}

TEST_CASE("cinematic strip: a section's end edge trims, its body moves, and it stops at the end")
{
	ImGuiCtx ctx;
	SequenceAsset seq = cutscene();
	View view;
	mouseAt(-100.0f, -100.0f);
	Result r = frame(seq, view);
	const HE::Ed::UITimelineView tv = viewOf(r, seq, view);
	const int skel = buildRows(seq, view)[3].track;
	const float y = rowCentre(r, 3);

	// The end edge at 3 s, dragged to 3.5 s: the end moves, the start stays.
	mouseAt(tv.xOf(3.0f) - 2.0f, y);
	mouseButton(true);
	frame(seq, view);
	CHECK(view.grab == View::Grab::End);
	mouseAt(tv.xOf(3.5f) - 2.0f, y);
	frame(seq, view);
	mouseButton(false);
	r = frame(seq, view);
	CHECK(r.committed);
	CHECK(seq.tracks[skel].sections[0].start == doctest::Approx(1.0f));
	CHECK(seq.tracks[skel].sections[0].end   == doctest::Approx(3.5f).epsilon(0.02));

	// The body, grabbed in the middle and dragged far right: the bar keeps its
	// length and its end stops at the sequence's end.
	mouseAt(tv.xOf(2.0f), y);
	mouseButton(true);
	frame(seq, view);
	CHECK(view.grab == View::Grab::Move);
	mouseAt(tv.xOf(4.0f), y);
	frame(seq, view);
	mouseButton(false);
	frame(seq, view);
	const SequenceSkeletalSection& s = seq.tracks[skel].sections[0];
	CHECK(s.end - s.start == doctest::Approx(2.5f).epsilon(0.02));
	CHECK(s.end == doctest::Approx(4.0f).epsilon(0.01));
}

TEST_CASE("cinematic strip: double-click adds an event, Delete removes it, the arrow folds a group")
{
	ImGuiCtx ctx;
	SequenceAsset seq = cutscene();
	View view;
	mouseAt(-100.0f, -100.0f);
	Result r = frame(seq, view);
	const HE::Ed::UITimelineView tv = viewOf(r, seq, view);
	const int ev = buildRows(seq, view)[6].track;

	mouseAt(tv.xOf(2.5f), rowCentre(r, 6));
	mouseButton(true);  frame(seq, view);
	mouseButton(false); frame(seq, view);
	mouseButton(true);  r = frame(seq, view);
	mouseButton(false); frame(seq, view);
	REQUIRE(seq.tracks[ev].events.size() == 2);
	CHECK(r.committed);
	CHECK(view.trackSel == ev);
	CHECK(view.itemSel == 1);
	CHECK(seq.tracks[ev].events[1].name == "Event");
	CHECK(seq.tracks[ev].events[1].time == doctest::Approx(2.5f).epsilon(0.01));

	keyEvent(ImGuiKey_Delete, true);
	r = frame(seq, view);
	keyEvent(ImGuiKey_Delete, false);
	frame(seq, view);
	CHECK(r.committed);
	CHECK(seq.tracks[ev].events.size() == 1);
	CHECK(seq.tracks[ev].events[0].name == "Boom");

	// A double-click on the cut row adds a cut to the camera live there.
	const int ct = cameraCutTrack(seq);
	mouseAt(tv.xOf(1.0f), rowCentre(r, 0));
	mouseButton(true);  frame(seq, view);
	mouseButton(false); frame(seq, view);
	mouseButton(true);  frame(seq, view);
	mouseButton(false); frame(seq, view);
	REQUIRE(seq.tracks[ct].cuts.size() == 3);
	CHECK(seq.tracks[ct].cuts[1].binding == 1);

	// The arrow of Hero's header (row 1) folds its two tracks away.
	const float frameH = ImGui::GetFrameHeight();
	mouseAt(r.laneX - metrics().nameW - metrics().gap + 2.0f + frameH * 0.5f, rowCentre(r, 1));
	mouseButton(true);  frame(seq, view);
	mouseButton(false); frame(seq, view);
	CHECK(view.isFolded(0));
	CHECK(buildRows(seq, view).size() == 5);
}

TEST_CASE("cinematic strip: clicking an actor's header picks the group, not a track")
{
	ImGuiCtx ctx;
	SequenceAsset seq = cutscene();
	View view;
	Labels labels;
	labels.missing = { true, false };
	mouseAt(-100.0f, -100.0f);
	Result r = frame(seq, view, labels);
	// Clicking the header picks the group, not a track.
	mouseAt(r.laneX - metrics().gap - 40.0f, rowCentre(r, 4));
	mouseButton(true);  frame(seq, view, labels);
	mouseButton(false); r = frame(seq, view, labels);
	CHECK(view.groupPicked);
	CHECK(view.groupSel == 1);
	CHECK(view.trackSel == -1);
	CHECK_FALSE(r.removeBinding);
	CHECK(seq.bindings.size() == 2);
}

// ── The preview bracket ──────────────────────────────────────────────────────

namespace
{
	SkeletalMeshAsset oneBoneMesh()
	{
		SkeletalMeshAsset sma;
		sma.name = "cinSkel";
		SkeletonJoint root;
		root.name   = "Root";
		root.parent = -1;
		root.inverseBindMatrix = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
		sma.skeleton.push_back(root);
		return sma;
	}

	AnimationClipAsset slideClip(float to, float duration)
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

	entt::entity actor(HorizonWorld& world, const char* name, glm::vec3 pos)
	{
		const entt::entity e = world.createEntity(name);
		auto& t = world.registry().get_or_emplace<TransformComponent>(e);
		t.position = pos;
		world.setEntityId(e, HE::UUID::generate());
		return e;
	}

	bool sameMatrix(const glm::mat4& a, const glm::mat4& b)
	{
		for (int c = 0; c < 4; ++c)
			for (int r = 0; r < 4; ++r)
				if (std::abs(a[c][r] - b[c][r]) > 1e-5f) return false;
		return true;
	}

	struct Scene
	{
		ContentManager cm;
		HorizonWorld   world;
		entt::entity hero = entt::null, hat = entt::null, bystander = entt::null;
		entt::entity cutCam = entt::null, playerCam = entt::null;
		HE::UUID material, clip;
		SequenceAsset seq;

		Scene()
		{
			auto& reg = world.registry();
			MaterialAsset mat;
			mat.roughness = 0.5f;
			material = cm.registerMaterial(mat);
			const HE::UUID mesh = cm.registerSkeletalMesh(oneBoneMesh());
			clip = cm.registerAnimationClip(slideClip(3.0f, 2.0f));

			hero = actor(world, "Hero", glm::vec3(0.0f));
			reg.emplace_or_replace<MeshComponent>(hero);
			reg.emplace_or_replace<MaterialComponent>(hero).materialAssetId = material;
			auto& smc = reg.emplace_or_replace<SkeletalMeshComponent>(hero);
			smc.meshAssetId = mesh;
			hat = actor(world, "Hat", glm::vec3(0.0f, 1.0f, 0.0f));
			world.reparentEntity(hat, hero);
			bystander = actor(world, "Bystander", glm::vec3(5.0f, 0.0f, 0.0f));
			reg.emplace_or_replace<MaterialComponent>(bystander).materialAssetId = material;

			cutCam = actor(world, "CutCam", glm::vec3(0.0f, 0.0f, 10.0f));
			reg.emplace_or_replace<CameraComponent>(cutCam).fovDegrees = 60.0f;
			playerCam = actor(world, "PlayerCam", glm::vec3(0.0f));
			auto& pc = reg.emplace_or_replace<CameraComponent>(playerCam);
			pc.isMain = true;
			pc.fovDegrees = 60.0f;

			seq.duration = 4.0f;
			addBinding(seq, "Hero", world.entityId(hero));
			addBinding(seq, "CutCam", world.entityId(cutCam));
			int t = addPropertyTrack(seq, 0, PropTarget::PosX, 0.0f);
			HE::Ed::Sequencer::insertKey(seq.tracks[t].channel, 2.0f, 4.0f);
			t = addPropertyTrack(seq, 0, PropTarget::MatRoughness, 0.5f);
			HE::Ed::Sequencer::insertKey(seq.tracks[t].channel, 2.0f, 1.0f);
			t = addPropertyTrack(seq, 0, PropTarget::Visible, 1.0f);
			HE::Ed::Sequencer::insertKey(seq.tracks[t].channel, 1.0f, 0.0f);
			t = addPropertyTrack(seq, 1, PropTarget::CameraFov, 60.0f);
			HE::Ed::Sequencer::insertKey(seq.tracks[t].channel, 2.0f, 30.0f);
			t = addTrack(seq, SequenceTrackKind::Skeletal, 0);
			insertSection(seq.tracks[t], 0.0f, 2.0f, clip);
			t = addTrack(seq, SequenceTrackKind::CameraCut, kSequenceNoBinding);
			const int k = insertCut(seq.tracks[t], 0.0f, 1);
			seq.tracks[t].cuts[k].blendIn = 2.0f;
			seq.tracks[t].cuts[k].curve   = SequenceBlendCurve::Linear;

			HE::propagateTransforms(world);
			// Settled, the way the editor leaves it after a frame.
			for (auto [e, mc] : reg.view<MaterialComponent>().each()) mc.dirty = false;
		}
	};
}

TEST_CASE("cinematic preview: writes the sequence at t, and restore leaves the scene as it was")
{
	Scene s;
	auto& reg = s.world.registry();
	const TransformComponent heroBefore = reg.get<TransformComponent>(s.hero);
	const glm::mat4 hatWorldBefore = reg.get<TransformComponent>(s.hat).worldMatrix;
	const std::vector<glm::mat4> bonesBefore = reg.get<SkeletalMeshComponent>(s.hero).boneMatrices;

	HE::Ed::CinematicPreview::Bracket b;
	b.apply(s.world, s.cm, s.seq, 1.0f);
	REQUIRE(b.applied());

	// The sequence at 1 s, written the runtime's way.
	CHECK(reg.get<TransformComponent>(s.hero).position.x == doctest::Approx(2.0f));
	CHECK(s.cm.getMaterial(s.material)->roughness == doctest::Approx(0.75f));
	CHECK_FALSE(reg.get<MeshComponent>(s.hero).visible);
	CHECK(reg.get<CameraComponent>(s.cutCam).fovDegrees == doctest::Approx(45.0f));
	// The skeletal section at 1 s: half way along the slide.
	REQUIRE_FALSE(reg.get<SkeletalMeshComponent>(s.hero).boneMatrices.empty());
	CHECK(reg.get<SkeletalMeshComponent>(s.hero).boneMatrices[0][3].x == doctest::Approx(1.5f));

	// The view through the cut: half way from the player camera (0,0,0) to the
	// cut camera (0,0,10), FOV half way from 60 to the keyed 45.
	const auto cv = b.cameraView(s.world);
	REQUIRE(cv.valid);
	CHECK(cv.camera == s.cutCam);
	CHECK(cv.position.z == doctest::Approx(5.0f));
	CHECK(cv.fovDegrees == doctest::Approx(52.5f));

	// What the render does inside the bracket: propagate the preview's pose
	// into every world matrix, the hat's included.
	HE::propagateTransforms(s.world);
	CHECK(reg.get<TransformComponent>(s.hat).worldMatrix[3].x == doctest::Approx(2.0f));

	b.restore(s.world, s.cm);
	CHECK_FALSE(b.applied());
	const TransformComponent& heroAfter = reg.get<TransformComponent>(s.hero);
	CHECK(heroAfter.position == heroBefore.position);
	CHECK(heroAfter.rotation == heroBefore.rotation);
	CHECK(heroAfter.scale    == heroBefore.scale);
	CHECK(sameMatrix(heroAfter.worldMatrix, heroBefore.worldMatrix));
	CHECK(sameMatrix(reg.get<TransformComponent>(s.hat).worldMatrix, hatWorldBefore));
	CHECK(s.cm.getMaterial(s.material)->roughness == doctest::Approx(0.5f));
	CHECK(reg.get<MeshComponent>(s.hero).visible);
	CHECK(reg.get<CameraComponent>(s.cutCam).fovDegrees == doctest::Approx(60.0f));
	CHECK(reg.get<CameraComponent>(s.playerCam).isMain);
	CHECK_FALSE(reg.get<CameraComponent>(s.cutCam).isMain);
	const auto& bonesAfter = reg.get<SkeletalMeshComponent>(s.hero).boneMatrices;
	REQUIRE(bonesAfter.size() == bonesBefore.size());
	for (size_t i = 0; i < bonesAfter.size(); ++i) CHECK(sameMatrix(bonesAfter[i], bonesBefore[i]));
	// Everybody drawing with the shared material refreshes its copy — the
	// bystander drew with the preview's roughness too.
	CHECK(reg.get<MaterialComponent>(s.bystander).dirty);
	CHECK(reg.get<MaterialComponent>(s.hero).dirty);
}

TEST_CASE("cinematic preview: a second apply saves the author's world, not the last preview")
{
	Scene s;
	auto& reg = s.world.registry();
	HE::Ed::CinematicPreview::Bracket b;
	b.apply(s.world, s.cm, s.seq, 1.0f);
	b.apply(s.world, s.cm, s.seq, 2.0f);
	CHECK(reg.get<TransformComponent>(s.hero).position.x == doctest::Approx(4.0f));
	b.restore(s.world, s.cm);
	CHECK(reg.get<TransformComponent>(s.hero).position.x == doctest::Approx(0.0f));
	CHECK(s.cm.getMaterial(s.material)->roughness == doctest::Approx(0.5f));
	// Restoring twice does nothing more.
	b.restore(s.world, s.cm);
	CHECK(reg.get<TransformComponent>(s.hero).position.x == doctest::Approx(0.0f));

	// A destroyed actor is skipped: the live cut's camera is gone, so there
	// is no view through it, and restore steps over the dead entities.
	b.apply(s.world, s.cm, s.seq, 0.5f);
	s.world.destroyEntity(s.hat);
	s.world.destroyEntity(s.cutCam);
	CHECK_FALSE(b.cameraView(s.world).valid);
	b.restore(s.world, s.cm);
	CHECK(reg.get<TransformComponent>(s.hero).position.x == doctest::Approx(0.0f));
}
