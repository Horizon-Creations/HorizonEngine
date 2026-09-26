#include "CinematicPanel.h"
#include "CinematicPreview.h"      // the write-look-restore bracket
#include "CinematicTimeline.h"     // the strip and the editing rules
#include "SequencerTimeline.h"     // targetName/targetGroup/defaultValue — one list of properties
#include "EditorApplication.h"     // AppContext
#include "EditorAssetTypeCache.h"
#include "EditorCamera.h"
#include "EditorPanelState.h"
#include "EditorToolbar.h"
#include "EditorHelp.h"            // Help::Scope — "Cinematic/<label>"
#include "EditorWidgets.h"

#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/PropertyAnimationSystem.h>
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/MaterialComponent.h>
#include <HorizonScene/Components/NameComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <Renderer/IRenderer.h>
#include <Sequence/SequenceJson.h>  // the undo snapshot is the file's own form
#include <Types/Enums.h>

#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
namespace Cin = HE::Ed::Cinematic;

struct PanelState
{
	bool        loaded = false;
	bool        dirty  = false;
	std::string relPath;
	std::string name;
	HE::UUID    assetId;

	Cin::View view;

	// The tab's own undo, as the Sequencer's: whole snapshots. A sequence's
	// JSON is what the file holds, so "the same as the top of the stack" is a
	// string compare and restoring is the loader's own parse — no second copy
	// of every field to keep in step.
	std::vector<std::string> undo;
	int undoPos = -1;

	// Look through the live cut's camera (else the editor camera).
	bool throughCamera = true;

	// Clips and sounds the sequence names that were asked to load once and did
	// not come — asked again only when the reference changes.
	std::vector<HE::UUID> triedRefs;

	// Scratch for the text fields: the name being typed lives here until the
	// field lets go, so every keystroke is not an undo step. Keyed on what it
	// edits, so a different selection never inherits it.
	std::string scratch;
	std::string scratchKey;

	std::string lastSaveError;
};
AssetPanelState<PanelState> s_states;

// One bracket for all tabs: only the tab in front renders, and the bracket is
// always restored before render() returns.
HE::Ed::CinematicPreview::Bracket s_preview;

SequenceAsset* seqOf(const PanelState& st, AppContext& ctx)
{
	return ctx.contentManager ? ctx.contentManager->getSequenceMutable(st.assetId) : nullptr;
}

bool saveState(PanelState& st, AppContext& ctx)
{
	if (!ctx.contentManager) return false;
	st.lastSaveError.clear();
	SequenceAsset* a = seqOf(st, ctx);
	if (!a) { st.lastSaveError = "Not saved: this sequence is no longer loaded."; return false; }
	if (!ctx.contentManager->saveAsset(*a))
	{
		st.lastSaveError = "Not saved: the file could not be written.";
		return false;
	}
	st.dirty = false;
	return true;
}

// ── Undo ─────────────────────────────────────────────────────────────────────
void pushUndo(PanelState& st, const SequenceAsset& seq)
{
	std::string snap = HE::sequenceToJson(seq);
	if (st.undoPos >= 0 && st.undoPos < static_cast<int>(st.undo.size()) && st.undo[st.undoPos] == snap)
		return;
	st.undo.resize(static_cast<size_t>(st.undoPos + 1));
	st.undo.push_back(std::move(snap));
	if (st.undo.size() > 64) st.undo.erase(st.undo.begin());
	st.undoPos = static_cast<int>(st.undo.size()) - 1;
}

bool restoreSnapshot(PanelState& st, SequenceAsset& seq, int pos)
{
	if (pos < 0 || pos >= static_cast<int>(st.undo.size())) return false;
	SequenceAsset back;
	if (!HE::sequenceFromJson(st.undo[pos], back)) return false;
	seq.duration  = back.duration;
	seq.frameRate = back.frameRate;
	seq.bindings  = std::move(back.bindings);
	seq.tracks    = std::move(back.tracks);
	st.undoPos = pos;
	st.view.armed = st.view.dragging = false;
	st.scratchKey.clear();
	st.dirty = true;
	return true;
}

void commit(PanelState& st, const SequenceAsset& seq)
{
	st.dirty = true;
	pushUndo(st, seq);
}

void fmtTime(char* buf, size_t n, float t)
{
	std::snprintf(buf, n, "%.2f s", t);
}

// ── The world side ───────────────────────────────────────────────────────────
entt::entity entityOf(const AppContext& ctx, const SequenceBinding& b)
{
	if (!ctx.world || b.entityId == HE::UUID{}) return entt::null;
	return ctx.world->findByEntityId(b.entityId);
}

std::string entityName(const AppContext& ctx, entt::entity e)
{
	const auto* n = ctx.world->registry().try_get<NameComponent>(e);
	return (n && !n->name.empty()) ? n->name : std::string("(unnamed)");
}

// The value a new property track starts at: what the actor holds now, so
// adding a track moves nothing (CinematicTimeline.h, addPropertyTrack).
float currentValue(AppContext& ctx, entt::entity e, PropTarget t)
{
	namespace Seq = HE::Ed::Sequencer;
	if (!ctx.world || e == entt::null) return Seq::defaultValue(t);
	auto& reg = ctx.world->registry();
	if (const auto* tc = reg.try_get<TransformComponent>(e))
	{
		switch (t)
		{
		case PropTarget::PosX: return tc->position.x;   case PropTarget::PosY: return tc->position.y;
		case PropTarget::PosZ: return tc->position.z;   case PropTarget::RotX: return tc->rotation.x;
		case PropTarget::RotY: return tc->rotation.y;   case PropTarget::RotZ: return tc->rotation.z;
		case PropTarget::ScaleX: return tc->scale.x;    case PropTarget::ScaleY: return tc->scale.y;
		case PropTarget::ScaleZ: return tc->scale.z;
		default: break;
		}
	}
	if (t == PropTarget::CameraFov)
		if (const auto* cam = reg.try_get<CameraComponent>(e)) return cam->fovDegrees;
	if (const auto* mc = reg.try_get<MaterialComponent>(e); mc && ctx.contentManager)
		if (const MaterialAsset* ma = ctx.contentManager->getMaterial(mc->materialAssetId))
		{
			switch (t)
			{
			case PropTarget::MatColorR: return ma->baseColor[0];
			case PropTarget::MatColorG: return ma->baseColor[1];
			case PropTarget::MatColorB: return ma->baseColor[2];
			case PropTarget::MatMetallic: return ma->metallic;
			case PropTarget::MatRoughness: return ma->roughness;
			case PropTarget::MatOpacity: return ma->opacity;
			default: break;
			}
		}
	return Seq::defaultValue(t);
}

// The binding the selection in the strip is about: the picked group, or the
// selected track's actor.
uint16_t focusSlot(const PanelState& st, const SequenceAsset& seq)
{
	if (st.view.groupPicked) return st.view.groupSel;
	if (st.view.trackSel >= 0 && st.view.trackSel < static_cast<int>(seq.tracks.size()))
	{
		const SequenceTrack& tr = seq.tracks[st.view.trackSel];
		if (tr.kind != SequenceTrackKind::CameraCut) return tr.binding;
	}
	return kSequenceNoBinding;
}

// Clips and sounds the sequence names, loaded if they can be — the strip shows
// their names and the preview poses with the clips. Before the frame's asset
// pointers are taken: a load moves them (ContentManager.h).
void ensureRefsResident(PanelState& st, AppContext& ctx)
{
	if (!ctx.contentManager) return;
	const SequenceAsset* seq = ctx.contentManager->getSequence(st.assetId);
	if (!seq) return;
	std::vector<HE::UUID> refs;
	HE::sequenceAssetRefs(*seq, refs);
	for (const HE::UUID& id : refs)
	{
		// Resident is what counts, not "known": a mounted or indexed asset has
		// a type before it has a payload.
		if (ctx.contentManager->getAnimationClip(id) || ctx.contentManager->getAudio(id)) continue;
		if (std::find(st.triedRefs.begin(), st.triedRefs.end(), id) != st.triedRefs.end()) continue;
		st.triedRefs.push_back(id);
		ctx.contentManager->ensureResident(id);
	}
}

// ── The toolbar row ──────────────────────────────────────────────────────────
void drawControls(PanelState& st, AppContext& ctx, SequenceAsset& seq, Cin::Intent& intent)
{
	namespace Seq = HE::Ed::Sequencer;
	HE::Ed::Help::Scope helpScope("Cinematic");

	if (EditorWidgets::smallButton(st.view.playing ? "Pause##cin_play" : "Play##cin_play"))
		st.view.playing = !st.view.playing;
	EditorWidgets::helpForKey("cinematic.play");
	ImGui::SameLine();
	if (EditorWidgets::smallButton("Stop"))
	{
		st.view.playing  = false;
		st.view.playhead = 0.0f;
	}
	ImGui::SameLine();
	EditorWidgets::checkbox("Loop##cin_loop", &st.view.loop);
	ImGui::SameLine();
	char now[24], len[24];
	fmtTime(now, sizeof(now), st.view.playhead);
	fmtTime(len, sizeof(len), seq.duration);
	ImGui::TextDisabled("%s / %s", now, len);

	ImGui::SameLine(); ImGui::TextDisabled("·"); ImGui::SameLine();
	ImGui::Text("Length");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(80.0f);
	float d = seq.duration;
	const float floor = std::max(Cin::lastContentTime(seq), 0.1f);
	if (ImGui::DragFloat("##cin_length", &d, 0.02f, floor, 3600.0f, "%.2f s"))
	{
		Cin::setDuration(seq, std::max(d, floor));
		st.dirty = true;
	}
	EditorWidgets::helpForKey("cinematic.length");
	if (ImGui::IsItemDeactivatedAfterEdit()) pushUndo(st, seq);

	ImGui::SameLine(); ImGui::TextDisabled("·"); ImGui::SameLine();
	if (EditorWidgets::smallButton("Zoom Out")) intent.zoom = -1;
	ImGui::SameLine();
	if (EditorWidgets::smallButton("Zoom In")) intent.zoom = 1;
	ImGui::SameLine();
	if (EditorWidgets::smallButton("Fit")) intent.fit = true;

	// ── Adding ───────────────────────────────────────────────────────────────
	// The popup offers what fits the focus: a picked actor gets its property
	// tracks and a skeletal track; the unbound kinds are always there.
	ImGui::SameLine(); ImGui::TextDisabled("·"); ImGui::SameLine();
	if (EditorWidgets::smallButton("Add Track")) ImGui::OpenPopup("##cin_addtrack");
	if (ImGui::BeginPopup("##cin_addtrack"))
	{
		const uint16_t slot = focusSlot(st, seq);
		const int b = Cin::findBinding(seq, slot);
		const entt::entity actor = b >= 0 ? entityOf(ctx, seq.bindings[b]) : entt::null;
		if (b >= 0)
		{
			ImGui::TextDisabled("For %s", seq.bindings[b].name.c_str());
			const char* lastGroup = "";
			for (int i = 0; i < Seq::kTargetCount; ++i)
			{
				const auto t = static_cast<PropTarget>(i);
				const char* group = Seq::targetGroup(t);
				if (std::strcmp(group, lastGroup) != 0) { ImGui::Separator(); lastGroup = group; }
				const bool have = Cin::findPropertyTrack(seq, slot, t) >= 0;
				ImGui::BeginDisabled(have);
				if (ImGui::Selectable((std::string(Seq::targetName(t)) + "##cin_addtarget").c_str(), false))
				{
					st.view.trackSel = Cin::addPropertyTrack(seq, slot, t, currentValue(ctx, actor, t));
					st.view.itemSel  = 0;
					st.view.groupPicked = false;
					commit(st, seq);
				}
				ImGui::EndDisabled();
				EditorWidgets::helpForKey("cinematic.add-target");
			}
			ImGui::Separator();
			if (EditorWidgets::selectable("Skeletal Animation"))
			{
				st.view.trackSel = Cin::addTrack(seq, SequenceTrackKind::Skeletal, slot);
				st.view.itemSel  = -1;
				st.view.groupPicked = false;
				commit(st, seq);
			}
			if (EditorWidgets::selectable("Events on this actor"))
			{
				st.view.trackSel = Cin::addTrack(seq, SequenceTrackKind::Event, slot);
				st.view.itemSel  = -1;
				st.view.groupPicked = false;
				commit(st, seq);
			}
			if (EditorWidgets::selectable("Sound at this actor"))
			{
				st.view.trackSel = Cin::addTrack(seq, SequenceTrackKind::Audio, slot);
				st.view.itemSel  = -1;
				st.view.groupPicked = false;
				commit(st, seq);
			}
			ImGui::Separator();
		}
		else
		{
			ImGui::TextDisabled("Pick an actor in the strip for its property tracks.");
			ImGui::Separator();
		}
		ImGui::BeginDisabled(Cin::cameraCutTrack(seq) >= 0);
		if (EditorWidgets::selectable("Camera Cuts"))
		{
			st.view.trackSel = Cin::addTrack(seq, SequenceTrackKind::CameraCut, kSequenceNoBinding);
			st.view.itemSel  = -1;
			st.view.groupPicked = false;
			commit(st, seq);
		}
		ImGui::EndDisabled();
		if (EditorWidgets::selectable("Events"))
		{
			st.view.trackSel = Cin::addTrack(seq, SequenceTrackKind::Event, kSequenceNoBinding);
			st.view.itemSel  = -1;
			st.view.groupPicked = false;
			commit(st, seq);
		}
		if (EditorWidgets::selectable("Sound"))
		{
			st.view.trackSel = Cin::addTrack(seq, SequenceTrackKind::Audio, kSequenceNoBinding);
			st.view.itemSel  = -1;
			st.view.groupPicked = false;
			commit(st, seq);
		}
		ImGui::EndPopup();
	}

	// What the selected track holds, at the playhead. A key holds the
	// track's value there; a section runs two seconds; a cut goes to the
	// camera live there; an event is called "Event"; a sound starts empty,
	// to be picked in the readout.
	const bool haveTrack = st.view.trackSel >= 0 && st.view.trackSel < static_cast<int>(seq.tracks.size());
	ImGui::SameLine();
	ImGui::BeginDisabled(!haveTrack);
	if (EditorWidgets::smallButton("Add at Playhead") && haveTrack)
	{
		SequenceTrack& tr = seq.tracks[st.view.trackSel];
		const float t = st.view.playhead;
		std::vector<bool> cameras;
		for (const SequenceBinding& bd : seq.bindings)
		{
			const entt::entity e = entityOf(ctx, bd);
			cameras.push_back(e != entt::null && ctx.world->registry().all_of<CameraComponent>(e));
		}
		int k = -1;
		switch (tr.kind)
		{
		case SequenceTrackKind::Property:
			k = Seq::insertKey(tr.channel, t, tr.channel.times.empty()
				? Seq::defaultValue(tr.channel.target)
				: PropertyAnimationSystem::sampleChannel(tr.channel, t));
			break;
		case SequenceTrackKind::Skeletal:
		{
			const HE::UUID clip = tr.sections.empty() ? HE::UUID{} : tr.sections.back().clipId;
			k = Cin::insertSection(tr, t, 2.0f, clip);
			break;
		}
		case SequenceTrackKind::CameraCut: k = Cin::insertCut(tr, t, Cin::defaultCutSlot(seq, t, cameras)); break;
		case SequenceTrackKind::Event:     k = Cin::insertEvent(tr, t, "Event"); break;
		case SequenceTrackKind::Audio:     k = Cin::insertSound(tr, t, HE::UUID{}); break;
		}
		if (k >= 0)
		{
			st.view.itemSel = k;
			if (Cin::lastContentTime(seq) > seq.duration) Cin::setDuration(seq, seq.duration);
			commit(st, seq);
		}
	}
	ImGui::EndDisabled();
}

// ── The actors row ───────────────────────────────────────────────────────────
void drawActors(PanelState& st, AppContext& ctx, SequenceAsset& seq)
{
	HE::Ed::Help::Scope helpScope("Cinematic");
	ImGui::TextDisabled("Actors");
	if (seq.bindings.empty())
	{
		ImGui::SameLine();
		ImGui::TextDisabled("none yet: select entities in the scene, then Bind Selected.");
	}
	for (size_t i = 0; i < seq.bindings.size(); ++i)
	{
		const SequenceBinding& b = seq.bindings[i];
		const entt::entity e = entityOf(ctx, b);
		ImGui::SameLine();
		if (i > 0) { ImGui::TextDisabled("·"); ImGui::SameLine(); }
		const std::string shown = b.name.empty() ? std::string("(unnamed)") : b.name;
		if (e == entt::null) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(230, 90, 80, 255));
		const bool picked = st.view.groupPicked && st.view.groupSel == b.slot;
		if (ImGui::Selectable((shown + "##cin_actor" + std::to_string(b.slot)).c_str(), picked,
		                      ImGuiSelectableFlags_None, ImVec2(ImGui::CalcTextSize(shown.c_str()).x + 8.0f, 0.0f)))
		{
			st.view.groupPicked = true;
			st.view.groupSel    = b.slot;
			st.view.trackSel    = -1;
			st.view.itemSel     = -1;
			if (e != entt::null) ctx.selection.set(e);
		}
		if (e == entt::null) ImGui::PopStyleColor();
		EditorWidgets::helpForKey("cinematic.actor");
	}

	if (!ctx.world) return;
	// Every selected entity with an id that is not bound yet.
	std::vector<entt::entity> fresh;
	for (entt::entity e : ctx.selection.entities())
	{
		if (!ctx.world->registry().valid(e)) continue;
		const HE::UUID id = ctx.world->entityId(e);
		if (id == HE::UUID{}) continue;
		const bool bound = std::any_of(seq.bindings.begin(), seq.bindings.end(),
			[&](const SequenceBinding& b) { return b.entityId == id; });
		if (!bound) fresh.push_back(e);
	}
	ImGui::SameLine(); ImGui::TextDisabled("·"); ImGui::SameLine();
	ImGui::BeginDisabled(fresh.empty());
	if (EditorWidgets::smallButton("Bind Selected"))
	{
		uint16_t last = kSequenceNoBinding;
		for (entt::entity e : fresh) last = Cin::addBinding(seq, entityName(ctx, e), ctx.world->entityId(e));
		st.view.groupPicked = true;
		st.view.groupSel    = last;
		st.view.trackSel    = -1;
		st.view.itemSel     = -1;
		commit(st, seq);
	}
	ImGui::EndDisabled();

	// Rebind: the picked actor now means the selected entity — for a binding
	// whose entity was deleted, or a shot re-cast. Tracks keep their slot.
	const int pb = st.view.groupPicked ? Cin::findBinding(seq, st.view.groupSel) : -1;
	const entt::entity primary = ctx.selection.primary();
	const bool canRebind = pb >= 0 && primary != entt::null && ctx.world->registry().valid(primary) &&
	                       ctx.world->entityId(primary) != HE::UUID{} &&
	                       ctx.world->entityId(primary) != seq.bindings[pb].entityId;
	ImGui::SameLine();
	ImGui::BeginDisabled(!canRebind);
	if (EditorWidgets::smallButton("Rebind to Selected") && canRebind)
	{
		seq.bindings[pb].entityId = ctx.world->entityId(primary);
		seq.bindings[pb].name     = entityName(ctx, primary);
		commit(st, seq);
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(pb < 0);
	if (EditorWidgets::dangerSmallButton("Remove Binding") && pb >= 0)
	{
		Cin::removeBinding(seq, st.view.groupSel);
		st.view.groupPicked = false;
		st.view.trackSel = -1;
		st.view.itemSel  = -1;
		commit(st, seq);
	}
	ImGui::EndDisabled();
}

// ── The readout: the selection, field by field ───────────────────────────────
// A field edits in place and marks dirty; the undo point is the field letting go.
void fieldDone(PanelState& st, const SequenceAsset& seq)
{
	if (ImGui::IsItemDeactivatedAfterEdit()) pushUndo(st, seq);
}

// A camera picker over the bindings, plus "(gameplay camera)".
bool cameraCombo(AppContext& ctx, const SequenceAsset& seq, uint16_t& slot)
{
	bool changed = false;
	std::string cur = "(gameplay camera)";
	if (const int b = Cin::findBinding(seq, slot); b >= 0) cur = seq.bindings[b].name;
	ImGui::SetNextItemWidth(160.0f);
	if (ImGui::BeginCombo("##cin_cutcam", cur.c_str()))
	{
		if (ImGui::Selectable("(gameplay camera)##cin_cutnone", slot == kSequenceNoBinding))
		{ slot = kSequenceNoBinding; changed = true; }
		EditorWidgets::helpForKey("cinematic.cut-none");
		for (const SequenceBinding& bd : seq.bindings)
		{
			const entt::entity e = entityOf(ctx, bd);
			const bool isCam = e != entt::null && ctx.world->registry().all_of<CameraComponent>(e);
			std::string lbl = bd.name + (isCam ? "" : "  (no camera)") + "##cin_cutslot" + std::to_string(bd.slot);
			if (ImGui::Selectable(lbl.c_str(), slot == bd.slot)) { slot = bd.slot; changed = true; }
			EditorWidgets::helpForKey("cinematic.cut-camera");
		}
		ImGui::EndCombo();
	}
	EditorWidgets::helpForKey("cinematic.cut-camera");
	return changed;
}

void drawReadout(PanelState& st, AppContext& ctx, SequenceAsset& seq)
{
	namespace Seq = HE::Ed::Sequencer;
	HE::Ed::Help::Scope helpScope("Cinematic");
	ImGui::Separator();
	Cin::View& v = st.view;

	if (v.groupPicked)
	{
		const int b = Cin::findBinding(seq, v.groupSel);
		if (b < 0) { ImGui::TextDisabled("Tracks with no actor: music, narration, events for the owner."); return; }
		SequenceBinding& bd = seq.bindings[b];
		const entt::entity e = entityOf(ctx, bd);
		ImGui::Text("Actor");
		ImGui::SameLine();
		const std::string key = "name" + std::to_string(bd.slot);
		if (st.scratchKey != key) { st.scratchKey = key; st.scratch = bd.name; }
		char buf[128];
		std::snprintf(buf, sizeof(buf), "%s", st.scratch.c_str());
		ImGui::SetNextItemWidth(180.0f);
		if (ImGui::InputText("##cin_bindname", buf, sizeof(buf))) st.scratch = buf;
		EditorWidgets::helpForKey("cinematic.binding-name");
		if (ImGui::IsItemDeactivatedAfterEdit() && st.scratch != bd.name)
		{
			bd.name = st.scratch;
			commit(st, seq);
		}
		ImGui::SameLine();
		if (e == entt::null)
			ImGui::TextColored(ImVec4(0.9f, 0.35f, 0.3f, 1.0f),
			                   "missing: no entity in this scene has its id. Select one and Rebind to Selected.");
		else
			ImGui::TextDisabled("is %s in the scene.", entityName(ctx, e).c_str());
		return;
	}

	const bool haveTrack = v.trackSel >= 0 && v.trackSel < static_cast<int>(seq.tracks.size());
	if (!haveTrack)
	{
		ImGui::TextDisabled("Click a track or an actor to select it; click a key, cut, section, event or sound");
		ImGui::TextDisabled("to edit it here. Double-click a key, event or cut row to add one there.");
		return;
	}
	SequenceTrack& tr = seq.tracks[v.trackSel];
	const int n = Cin::itemCount(tr);
	ImGui::Text("%s", Cin::trackLabel(tr).c_str());
	ImGui::SameLine();
	if (const int b = Cin::findBinding(seq, tr.binding); b >= 0 && tr.kind != SequenceTrackKind::CameraCut)
		ImGui::TextDisabled("on %s  ·  %d item%s", seq.bindings[b].name.c_str(), n, n == 1 ? "" : "s");
	else
		ImGui::TextDisabled("%d item%s", n, n == 1 ? "" : "s");
	if (v.itemSel < 0 || v.itemSel >= n)
	{
		ImGui::TextDisabled("Click an item on this track to edit it, or Add at Playhead.");
		return;
	}

	// The item's time, through moveItem so the list stays in order and the
	// selection follows the item.
	ImGui::Text("At");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(90.0f);
	float t = Cin::itemTime(tr, v.itemSel);
	if (ImGui::DragFloat("##cin_itemtime", &t, 0.005f, 0.0f, seq.duration, "%.3f s"))
	{
		const int nk = Cin::moveItem(tr, v.itemSel, std::clamp(t, 0.0f, seq.duration));
		if (nk >= 0) { v.itemSel = nk; v.playhead = Cin::itemTime(tr, nk); st.dirty = true; }
	}
	EditorWidgets::helpForKey("cinematic.item-time");
	fieldDone(st, seq);
	const int k = v.itemSel;

	switch (tr.kind)
	{
	case SequenceTrackKind::Property:
	{
		ImGui::SameLine(); ImGui::TextDisabled("holds"); ImGui::SameLine();
		ImGui::SetNextItemWidth(110.0f);
		float val = tr.channel.values[k];
		if (tr.channel.target == PropTarget::Visible)
		{
			bool on = val >= 0.5f;
			if (EditorWidgets::checkbox("Shown##cin_keyvisible", &on))
			{ tr.channel.values[k] = on ? 1.0f : 0.0f; commit(st, seq); }
		}
		else
		{
			if (ImGui::DragFloat("##cin_keyvalue", &val, 0.01f, 0.0f, 0.0f, "%.3f"))
			{ tr.channel.values[k] = val; st.dirty = true; }
			EditorWidgets::helpForKey("cinematic.key-value");
			fieldDone(st, seq);
		}
		break;
	}
	case SequenceTrackKind::CameraCut:
	{
		SequenceCameraCut& c = tr.cuts[k];
		ImGui::SameLine(); ImGui::TextDisabled("to"); ImGui::SameLine();
		if (cameraCombo(ctx, seq, c.binding)) commit(st, seq);
		ImGui::SameLine(); ImGui::Text("Blend In"); ImGui::SameLine();
		ImGui::SetNextItemWidth(80.0f);
		if (ImGui::DragFloat("##cin_blendin", &c.blendIn, 0.01f, 0.0f, 30.0f, "%.2f s"))
		{ c.blendIn = std::max(c.blendIn, 0.0f); st.dirty = true; }
		EditorWidgets::helpForKey("cinematic.blend-in");
		fieldDone(st, seq);
		ImGui::SameLine();
		static const char* kCurves[] = { "Linear", "Smooth Step", "Ease Out" };
		int curve = static_cast<int>(c.curve);
		ImGui::SetNextItemWidth(110.0f);
		if (ImGui::Combo("##cin_curve", &curve, kCurves, 3))
		{ c.curve = static_cast<SequenceBlendCurve>(curve); commit(st, seq); }
		EditorWidgets::helpForKey("cinematic.blend-curve");
		break;
	}
	case SequenceTrackKind::Event:
	{
		AnimationNotify& e = tr.events[k];
		ImGui::SameLine(); ImGui::Text("Name"); ImGui::SameLine();
		const std::string key = "event" + std::to_string(v.trackSel) + ":" + std::to_string(k);
		if (st.scratchKey != key) { st.scratchKey = key; st.scratch = e.name; }
		char buf[128];
		std::snprintf(buf, sizeof(buf), "%s", st.scratch.c_str());
		ImGui::SetNextItemWidth(160.0f);
		if (ImGui::InputText("##cin_eventname", buf, sizeof(buf))) st.scratch = buf;
		EditorWidgets::helpForKey("cinematic.event-name");
		if (ImGui::IsItemDeactivatedAfterEdit() && st.scratch != e.name) { e.name = st.scratch; commit(st, seq); }
		ImGui::SameLine(); ImGui::Text("Duration"); ImGui::SameLine();
		ImGui::SetNextItemWidth(80.0f);
		if (ImGui::DragFloat("##cin_eventdur", &e.duration, 0.01f, 0.0f, seq.duration, "%.2f s"))
		{ e.duration = std::max(e.duration, 0.0f); st.dirty = true; }
		EditorWidgets::helpForKey("cinematic.event-duration");
		fieldDone(st, seq);
		break;
	}
	case SequenceTrackKind::Audio:
	{
		SequenceAudioSection& a = tr.audio[k];
		ImGui::SameLine();
		if (EditorWidgets::assetDropSlot(ctx, nullptr, a.assetId, HE::AssetType::Audio, "cin_sound",
		                                 "(pick a sound)", nullptr, false, /*undo=*/false) != EditorWidgets::SlotAction::None)
			commit(st, seq);
		EditorWidgets::helpForKey("cinematic.sound");
		ImGui::SameLine(); ImGui::Text("Volume"); ImGui::SameLine();
		ImGui::SetNextItemWidth(70.0f);
		if (ImGui::DragFloat("##cin_volume", &a.volume, 0.01f, 0.0f, 4.0f, "%.2f")) st.dirty = true;
		EditorWidgets::helpForKey("cinematic.volume");
		fieldDone(st, seq);
		ImGui::SameLine(); ImGui::Text("Pitch"); ImGui::SameLine();
		ImGui::SetNextItemWidth(70.0f);
		if (ImGui::DragFloat("##cin_pitch", &a.pitch, 0.01f, 0.1f, 4.0f, "%.2f")) st.dirty = true;
		EditorWidgets::helpForKey("cinematic.pitch");
		fieldDone(st, seq);
		break;
	}
	case SequenceTrackKind::Skeletal:
	{
		SequenceSkeletalSection& s = tr.sections[k];
		ImGui::SameLine();
		if (EditorWidgets::assetDropSlot(ctx, nullptr, s.clipId, HE::AssetType::AnimationClip, "cin_clip",
		                                 "(pick a clip)", nullptr, false, /*undo=*/false) != EditorWidgets::SlotAction::None)
			commit(st, seq);
		EditorWidgets::helpForKey("cinematic.clip");
		ImGui::SameLine(); ImGui::Text("to"); ImGui::SameLine();
		ImGui::SetNextItemWidth(80.0f);
		float end = s.end;
		if (ImGui::DragFloat("##cin_secend", &end, 0.005f, 0.0f, seq.duration, "%.3f s"))
		{ Cin::setSectionEnd(tr, k, std::min(end, seq.duration)); st.dirty = true; }
		EditorWidgets::helpForKey("cinematic.section-end");
		fieldDone(st, seq);
		ImGui::SameLine(); ImGui::Text("Clip Offset"); ImGui::SameLine();
		ImGui::SetNextItemWidth(80.0f);
		if (ImGui::DragFloat("##cin_secoffset", &s.clipOffset, 0.005f, 0.0f, 3600.0f, "%.3f s")) st.dirty = true;
		EditorWidgets::helpForKey("cinematic.clip-offset");
		fieldDone(st, seq);
		ImGui::SameLine(); ImGui::Text("Rate"); ImGui::SameLine();
		ImGui::SetNextItemWidth(70.0f);
		if (ImGui::DragFloat("##cin_secrate", &s.playRate, 0.01f, -4.0f, 4.0f, "%.2f")) st.dirty = true;
		EditorWidgets::helpForKey("cinematic.play-rate");
		fieldDone(st, seq);
		ImGui::SameLine();
		if (EditorWidgets::checkbox("Loop Clip##cin_secloop", &s.loop)) commit(st, seq);
		break;
	}
	}
}

// ── The picture ──────────────────────────────────────────────────────────────
void drawPreview(PanelState& st, AppContext& ctx, const SequenceAsset& seq, float height)
{
	HE::Ed::Help::Scope helpScope("Cinematic");
	EditorWidgets::checkbox("Through Camera##cin_through", &st.throughCamera);
	ImGui::SameLine();

	if (!ctx.renderer || !ctx.world || !ctx.contentManager || ctx.appLivePreview)
	{
		ImGui::TextDisabled(ctx.appLivePreview ? "(an application has no scene to preview in)"
		                                       : "(no preview here: nothing renders in this editor)");
		ImGui::Dummy(ImVec2(1.0f, height));
		return;
	}
	if (ctx.isPlaying)
	{
		// The game owns the world while it plays; the bracket would write into
		// the play world and the Sequence Players there already drive it.
		ImGui::TextDisabled("(paused while the game plays)");
		ImGui::Dummy(ImVec2(1.0f, height));
		return;
	}

	const float w = std::max(32.0f, std::min(ImGui::GetContentRegionAvail().x, height * 16.0f / 9.0f));
	const float h = std::max(32.0f, height - ImGui::GetFrameHeightWithSpacing());

	// Write, look, put back — nothing between apply and restore may return.
	s_preview.apply(*ctx.world, *ctx.contentManager, seq, st.view.playhead);
	EditorCameraOverride ov;
	const HE::Ed::CinematicPreview::CameraView cv = s_preview.cameraView(*ctx.world);
	const char* through = "editor camera";
	if (st.throughCamera && cv.valid)
	{
		ov.active     = true;
		ov.view       = glm::inverse(glm::translate(glm::mat4(1.0f), cv.position) * glm::mat4_cast(cv.rotation));
		ov.position   = cv.position;
		ov.fovDegrees = cv.fovDegrees;
		ov.nearPlane  = cv.nearPlane;
		ov.farPlane   = cv.farPlane;
		through = "cut camera";
	}
	else if (ctx.editorCamera && ctx.editorCamera->initialised())
		ov = ctx.editorCamera->makeOverride();
	ov.editorIcons = false;

	void* tex = nullptr;
	if (ov.active)
	{
		const IRenderer::EnvironmentSettings& sceneEnv = ctx.renderer->GetEnvironment();
		WorldPreviewEnv env;
		env.sky           = sceneEnv.skyEnabled;
		env.timeOfDay     = sceneEnv.timeOfDay;
		env.cloudCoverage = sceneEnv.cloudCoverage;
		tex = ctx.renderer->RenderWorldPreview(*ctx.contentManager, *ctx.world,
			static_cast<uint32_t>(w), static_cast<uint32_t>(h), ov, glm::vec3(0.0f), env, nullptr, 0);
	}
	s_preview.restore(*ctx.world, *ctx.contentManager);

	if (st.throughCamera && !cv.valid) ImGui::TextDisabled("(no cut is live here: showing the editor camera)");
	else                               ImGui::TextDisabled("(%s)", through);
	if (!tex)
	{
		ImGui::TextDisabled("(no preview on this backend)");
		ImGui::Dummy(ImVec2(w, h - ImGui::GetTextLineHeightWithSpacing()));
		return;
	}
	const ImVec2 org = ImGui::GetCursorScreenPos();
	const bool flipY = (ctx.backend == HE::RendererBackend::OpenGL);
	ImGui::GetWindowDrawList()->AddImage(reinterpret_cast<ImTextureID>(tex), org, ImVec2(org.x + w, org.y + h),
		flipY ? ImVec2(0, 1) : ImVec2(0, 0), flipY ? ImVec2(1, 0) : ImVec2(1, 1));
	ImGui::Dummy(ImVec2(w, h));
}

} // namespace

bool CinematicPanel::isCinematicAsset(const std::string& path)
{ return EditorAssetTypeCache::is(path, HE::AssetType::Sequence); }

bool CinematicPanel::isDirty(const std::string& path) { return s_states.dirty(path); }

bool CinematicPanel::reloadFromDisk(const std::string& assetPath)
{
	auto* st = s_states.find(assetPath);
	if (!st) return false;
	st->loaded = false;
	st->dirty  = false;
	return true;
}

namespace
{
PanelState* stateByContentPath(const std::string& contentPath)
{
	if (contentPath.empty()) return nullptr;
	PanelState* found = nullptr;
	s_states.forEach([&](const std::string&, PanelState& st) {
		if (!found && st.relPath == contentPath) found = &st;
	});
	return found;
}
} // namespace

bool CinematicPanel::isDirtyByContentPath(const std::string& contentPath)
{
	const PanelState* st = stateByContentPath(contentPath);
	return st && st->dirty;
}

bool CinematicPanel::reloadByContentPath(const std::string& contentPath)
{
	PanelState* st = stateByContentPath(contentPath);
	if (!st) return false;
	st->loaded = false;
	st->dirty  = false;
	return true;
}

void CinematicPanel::appendDirtyPaths(std::vector<std::string>& out) { s_states.appendDirtyPaths(out); }

bool CinematicPanel::save(AppContext& ctx, const std::string& path)
{
	PanelState* st = s_states.find(path);
	if (!st || !st->dirty) return true;
	return saveState(*st, ctx);
}

void CinematicPanel::forget(const std::string& path) { s_states.forget(path); }

void CinematicPanel::render(AppContext& ctx, const std::string& assetPath,
                            const ImVec2& pos, const ImVec2& size)
{
	PanelState& st = s_states[assetPath];
	if (!st.loaded && ctx.contentManager)
	{
		st.relPath = ctx.contentManager->toContentRelativePath(assetPath);
		st.assetId = ctx.contentManager->loadAsset(st.relPath);
		st.name    = std::filesystem::path(assetPath).stem().string();
		st.view    = Cin::View{};
		st.undo.clear();
		st.undoPos = -1;
		st.triedRefs.clear();
		st.scratchKey.clear();
		st.loaded  = true;
	}
	// Before any asset pointer of this frame is taken: a load moves them.
	ensureRefsResident(st, ctx);

	HE::Ed::Help::Scope helpScope("Cinematic");
	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(size);
	ImGui::Begin(("##cinematic_" + assetPath).c_str(), nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);

	SequenceAsset* seq = seqOf(st, ctx);
	if (seq && st.undoPos < 0) pushUndo(st, *seq);

	{
		namespace T = EditorToolbar;
		T::Bar bar;
		T::assetHeader(bar, st.name.c_str(), T::iconPlay, st.dirty);
		bar.group();
		bar.readout(nullptr, "Cinematic Sequence", T::kFgDim);
		bar.endGroup();
		if (T::saveButton(bar, st.dirty && seq != nullptr)) saveState(st, ctx);
	}
	if (!st.lastSaveError.empty())
	{
		ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(220, 80, 80, 255));
		ImGui::TextWrapped("%s", st.lastSaveError.c_str());
		ImGui::PopStyleColor();
	}
	if (!seq)
	{
		ImGui::Spacing();
		ImGui::TextDisabled("This sequence could not be loaded, so there is nothing to show.");
		ImGui::TextDisabled("The file is recognised as a Sequence, but no sequence came out of it:");
		ImGui::TextDisabled("it may be damaged, or written by a newer editor.");
		ImGui::End();
		return;
	}

	Cin::advancePlayhead(st.view, seq->duration, ImGui::GetIO().DeltaTime);

	Cin::Intent intent;
	drawControls(st, ctx, *seq, intent);
	drawActors(st, ctx, *seq);

	// ── Picture on top, strip under it, readout at the bottom ────────────────
	const float readoutH = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeightWithSpacing() +
	                       ImGui::GetStyle().ItemSpacing.y * 3.0f + 6.0f;
	const float avail    = ImGui::GetContentRegionAvail().y - readoutH;
	const float previewH = std::clamp(avail * 0.5f, 80.0f, 520.0f);
	drawPreview(st, ctx, *seq, previewH);

	Cin::Labels labels;
	for (const SequenceBinding& b : seq->bindings)
	{
		const entt::entity e = entityOf(ctx, b);
		labels.missing.push_back(e == entt::null);
		labels.camera.push_back(e != entt::null && ctx.world->registry().all_of<CameraComponent>(e));
	}
	ContentManager* cm = ctx.contentManager;
	labels.assetName = [cm](HE::UUID id) -> std::string {
		if (!cm) return {};
		if (const AnimationClipAsset* c = cm->getAnimationClip(id)) return c->name;
		if (const AudioAsset* a = cm->getAudio(id)) return a->name;
		return {};
	};
	const float stripH = std::max(80.0f, ImGui::GetContentRegionAvail().y - readoutH);
	const Cin::Result r = Cin::draw(*seq, st.view, ImVec2(0.0f, stripH), labels, intent);
	if (r.edited)    st.dirty = true;
	if (r.committed) pushUndo(st, *seq);
	if (r.removeBinding && Cin::removeBinding(*seq, r.removeSlot))
	{
		st.view.groupPicked = false;
		st.view.trackSel = st.view.itemSel = -1;
		commit(st, *seq);
	}

	drawReadout(st, ctx, *seq);

	const bool typing = ImGui::IsAnyItemActive() || ImGui::GetIO().WantTextInput;
	if (!typing && ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows))
	{
		const bool ctrl = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper;
		if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) st.view.playing = !st.view.playing;
		if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S)) saveState(st, ctx);
		if (ctrl && !ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z))
			restoreSnapshot(st, *seq, st.undoPos - 1);
		if ((ctrl && ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z)) ||
		    (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y)))
			restoreSnapshot(st, *seq, st.undoPos + 1);
	}

	ImGui::End();
}
