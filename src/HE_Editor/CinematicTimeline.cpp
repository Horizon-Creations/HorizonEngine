#include "CinematicTimeline.h"
#include "SequencerTimeline.h"   // insertKey/moveKey/removeKey, targetName, formatValue, drawRuler

#include <HorizonScene/PropertyAnimationSystem.h>   // sampleChannel + advance — the runtime's rules

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace HE::Ed::Cinematic
{

// ── Bindings ─────────────────────────────────────────────────────────────────

uint16_t addBinding(SequenceAsset& seq, const std::string& name, HE::UUID entityId)
{
	int next = 0;
	for (const SequenceBinding& b : seq.bindings) next = std::max(next, int(b.slot) + 1);
	// 0xFFFF is "no actor"; a sequence with sixty-five thousand actors has
	// other problems, but the slot must still never read as none.
	if (next >= int(kSequenceNoBinding)) next = int(kSequenceNoBinding) - 1;
	SequenceBinding b;
	b.slot     = static_cast<uint16_t>(next);
	b.name     = name;
	b.entityId = entityId;
	seq.bindings.push_back(std::move(b));
	return static_cast<uint16_t>(next);
}

int findBinding(const SequenceAsset& seq, uint16_t slot)
{
	for (int i = 0; i < static_cast<int>(seq.bindings.size()); ++i)
		if (seq.bindings[i].slot == slot) return i;
	return -1;
}

bool removeBinding(SequenceAsset& seq, uint16_t slot)
{
	const int b = findBinding(seq, slot);
	if (b < 0) return false;
	seq.bindings.erase(seq.bindings.begin() + b);
	// A duplicate binding of the same slot (an authoring slip the resolver
	// tolerates) keeps the tracks alive: they still have an actor.
	if (findBinding(seq, slot) >= 0) return true;
	seq.tracks.erase(std::remove_if(seq.tracks.begin(), seq.tracks.end(),
		[&](const SequenceTrack& tr) {
			return tr.kind != SequenceTrackKind::CameraCut && tr.binding == slot;
		}), seq.tracks.end());
	for (SequenceTrack& tr : seq.tracks)
		if (tr.kind == SequenceTrackKind::CameraCut)
			tr.cuts.erase(std::remove_if(tr.cuts.begin(), tr.cuts.end(),
				[&](const SequenceCameraCut& c) { return c.binding == slot; }), tr.cuts.end());
	return true;
}

// ── Tracks ───────────────────────────────────────────────────────────────────

int cameraCutTrack(const SequenceAsset& seq)
{
	for (int i = 0; i < static_cast<int>(seq.tracks.size()); ++i)
		if (seq.tracks[i].kind == SequenceTrackKind::CameraCut) return i;
	return -1;
}

int findPropertyTrack(const SequenceAsset& seq, uint16_t slot, PropTarget target)
{
	for (int i = 0; i < static_cast<int>(seq.tracks.size()); ++i)
	{
		const SequenceTrack& tr = seq.tracks[i];
		if (tr.kind == SequenceTrackKind::Property && tr.binding == slot && tr.channel.target == target)
			return i;
	}
	return -1;
}

int addPropertyTrack(SequenceAsset& seq, uint16_t slot, PropTarget target, float value)
{
	if (slot == kSequenceNoBinding) return -1;
	if (const int i = findPropertyTrack(seq, slot, target); i >= 0) return i;
	SequenceTrack tr;
	tr.kind           = SequenceTrackKind::Property;
	tr.binding        = slot;
	tr.channel.target = target;
	tr.channel.times  = { 0.0f };
	tr.channel.values = { value };
	seq.tracks.push_back(std::move(tr));
	if (seq.duration <= 0.0f) seq.duration = kDefaultDuration;
	return static_cast<int>(seq.tracks.size()) - 1;
}

int addTrack(SequenceAsset& seq, SequenceTrackKind kind, uint16_t slot)
{
	switch (kind)
	{
	case SequenceTrackKind::Property:
		return -1;   // addPropertyTrack: it needs a target and a value
	case SequenceTrackKind::CameraCut:
		if (const int i = cameraCutTrack(seq); i >= 0) return i;
		slot = kSequenceNoBinding;
		break;
	case SequenceTrackKind::Skeletal:
		if (slot == kSequenceNoBinding) return -1;
		break;
	case SequenceTrackKind::Event:
	case SequenceTrackKind::Audio:
		break;
	}
	SequenceTrack tr;
	tr.kind    = kind;
	tr.binding = slot;
	// The cut row stands on top of the strip, and the track list reads the
	// same way when it is first in the file too.
	if (kind == SequenceTrackKind::CameraCut)
	{
		seq.tracks.insert(seq.tracks.begin(), std::move(tr));
		if (seq.duration <= 0.0f) seq.duration = kDefaultDuration;
		return 0;
	}
	seq.tracks.push_back(std::move(tr));
	if (seq.duration <= 0.0f) seq.duration = kDefaultDuration;
	return static_cast<int>(seq.tracks.size()) - 1;
}

bool removeTrack(SequenceAsset& seq, int track)
{
	if (track < 0 || track >= static_cast<int>(seq.tracks.size())) return false;
	seq.tracks.erase(seq.tracks.begin() + track);
	return true;
}

// ── Items ────────────────────────────────────────────────────────────────────

namespace
{
	// Insert `item` into `v` sorted by `timeOf`, AFTER anything at the same
	// time. Returns its index.
	template <class T, class F>
	int insertSorted(std::vector<T>& v, T item, F timeOf)
	{
		const float t = timeOf(item);
		const auto it = std::upper_bound(v.begin(), v.end(), t,
			[&](float x, const T& e) { return x < timeOf(e); });
		const int k = static_cast<int>(it - v.begin());
		v.insert(it, std::move(item));
		return k;
	}

	template <class T, class F>
	int reinsert(std::vector<T>& v, int k, F timeOf)
	{
		T item = std::move(v[k]);
		v.erase(v.begin() + k);
		return insertSorted(v, std::move(item), timeOf);
	}

	const auto cutTime     = [](const SequenceCameraCut& c)       { return c.time; };
	const auto eventTime   = [](const AnimationNotify& n)         { return n.time; };
	const auto soundTime   = [](const SequenceAudioSection& s)    { return s.start; };
	const auto sectionTime = [](const SequenceSkeletalSection& s) { return s.start; };
}

int insertCut(SequenceTrack& tr, float t, uint16_t cameraSlot)
{
	SequenceCameraCut c;
	c.time    = std::max(t, 0.0f);
	c.binding = cameraSlot;
	return insertSorted(tr.cuts, c, cutTime);
}

int insertEvent(SequenceTrack& tr, float t, const std::string& name)
{
	AnimationNotify n;
	n.name = name;
	n.time = std::max(t, 0.0f);
	return insertSorted(tr.events, n, eventTime);
}

int insertSound(SequenceTrack& tr, float t, HE::UUID assetId)
{
	SequenceAudioSection s;
	s.assetId = assetId;
	s.start   = std::max(t, 0.0f);
	return insertSorted(tr.audio, s, soundTime);
}

int insertSection(SequenceTrack& tr, float start, float length, HE::UUID clipId)
{
	SequenceSkeletalSection s;
	s.clipId = clipId;
	s.start  = std::max(start, 0.0f);
	s.end    = s.start + std::max(length, kMinSection);
	return insertSorted(tr.sections, s, sectionTime);
}

int itemCount(const SequenceTrack& tr)
{
	switch (tr.kind)
	{
	case SequenceTrackKind::Property:
		return static_cast<int>(std::min(tr.channel.times.size(), tr.channel.values.size()));
	case SequenceTrackKind::Skeletal:  return static_cast<int>(tr.sections.size());
	case SequenceTrackKind::CameraCut: return static_cast<int>(tr.cuts.size());
	case SequenceTrackKind::Event:     return static_cast<int>(tr.events.size());
	case SequenceTrackKind::Audio:     return static_cast<int>(tr.audio.size());
	}
	return 0;
}

float itemTime(const SequenceTrack& tr, int k)
{
	if (k < 0 || k >= itemCount(tr)) return 0.0f;
	switch (tr.kind)
	{
	case SequenceTrackKind::Property:  return tr.channel.times[k];
	case SequenceTrackKind::Skeletal:  return tr.sections[k].start;
	case SequenceTrackKind::CameraCut: return tr.cuts[k].time;
	case SequenceTrackKind::Event:     return tr.events[k].time;
	case SequenceTrackKind::Audio:     return tr.audio[k].start;
	}
	return 0.0f;
}

int moveItem(SequenceTrack& tr, int k, float t)
{
	if (k < 0 || k >= itemCount(tr)) return -1;
	t = std::max(t, 0.0f);
	switch (tr.kind)
	{
	case SequenceTrackKind::Property:
		return Sequencer::moveKey(tr.channel, k, t);
	case SequenceTrackKind::Skeletal:
	{
		SequenceSkeletalSection& s = tr.sections[k];
		const float len = s.end - s.start;
		s.start = t;
		s.end   = t + len;
		return reinsert(tr.sections, k, sectionTime);
	}
	case SequenceTrackKind::CameraCut:
		tr.cuts[k].time = t;
		return reinsert(tr.cuts, k, cutTime);
	case SequenceTrackKind::Event:
		tr.events[k].time = t;
		return reinsert(tr.events, k, eventTime);
	case SequenceTrackKind::Audio:
		tr.audio[k].start = t;
		return reinsert(tr.audio, k, soundTime);
	}
	return -1;
}

bool removeItem(SequenceTrack& tr, int k)
{
	if (k < 0 || k >= itemCount(tr)) return false;
	switch (tr.kind)
	{
	case SequenceTrackKind::Property:  return Sequencer::removeKey(tr.channel, k);
	case SequenceTrackKind::Skeletal:  tr.sections.erase(tr.sections.begin() + k); return true;
	case SequenceTrackKind::CameraCut: tr.cuts.erase(tr.cuts.begin() + k);         return true;
	case SequenceTrackKind::Event:     tr.events.erase(tr.events.begin() + k);     return true;
	case SequenceTrackKind::Audio:     tr.audio.erase(tr.audio.begin() + k);       return true;
	}
	return false;
}

int setSectionStart(SequenceTrack& tr, int k, float start)
{
	if (tr.kind != SequenceTrackKind::Skeletal || k < 0 || k >= static_cast<int>(tr.sections.size()))
		return -1;
	SequenceSkeletalSection& s = tr.sections[k];
	s.start = std::clamp(start, 0.0f, std::max(0.0f, s.end - kMinSection));
	return reinsert(tr.sections, k, sectionTime);
}

int setSectionEnd(SequenceTrack& tr, int k, float end)
{
	if (tr.kind != SequenceTrackKind::Skeletal || k < 0 || k >= static_cast<int>(tr.sections.size()))
		return -1;
	SequenceSkeletalSection& s = tr.sections[k];
	s.end = std::max(end, s.start + kMinSection);
	return k;
}

float lastContentTime(const SequenceAsset& seq)
{
	float last = 0.0f;
	for (const SequenceTrack& tr : seq.tracks)
	{
		for (size_t i = 0; i < std::min(tr.channel.times.size(), tr.channel.values.size()); ++i)
			last = std::max(last, tr.channel.times[i]);
		for (const SequenceSkeletalSection& s : tr.sections) last = std::max(last, s.end);
		for (const SequenceCameraCut& c : tr.cuts)           last = std::max(last, c.time);
		for (const AnimationNotify& n : tr.events)           last = std::max(last, n.time + std::max(n.duration, 0.0f));
		for (const SequenceAudioSection& a : tr.audio)       last = std::max(last, a.start);
	}
	return last;
}

float setDuration(SequenceAsset& seq, float duration)
{
	seq.duration = std::max(duration, lastContentTime(seq));
	return seq.duration;
}

uint16_t defaultCutSlot(const SequenceAsset& seq, float t, const std::vector<bool>& camera)
{
	const int ct = cameraCutTrack(seq);
	if (ct >= 0)
	{
		// The cuts are kept sorted, so the last one at or before t is live.
		const SequenceTrack& tr = seq.tracks[ct];
		for (int i = static_cast<int>(tr.cuts.size()) - 1; i >= 0; --i)
			if (tr.cuts[i].time <= t && tr.cuts[i].binding != kSequenceNoBinding)
				return tr.cuts[i].binding;
	}
	for (size_t i = 0; i < seq.bindings.size() && i < camera.size(); ++i)
		if (camera[i]) return seq.bindings[i].slot;
	return seq.bindings.empty() ? kSequenceNoBinding : seq.bindings.front().slot;
}

const char* kindName(SequenceTrackKind k)
{
	switch (k)
	{
	case SequenceTrackKind::Property:  return "Property";
	case SequenceTrackKind::Skeletal:  return "Skeletal Animation";
	case SequenceTrackKind::CameraCut: return "Camera Cuts";
	case SequenceTrackKind::Event:     return "Events";
	case SequenceTrackKind::Audio:     return "Sound";
	}
	return "(unknown)";
}

std::string trackLabel(const SequenceTrack& tr)
{
	if (tr.kind == SequenceTrackKind::Property) return Sequencer::targetName(tr.channel.target);
	return kindName(tr.kind);
}

// ── Rows and view ────────────────────────────────────────────────────────────

bool View::isFolded(uint16_t slot) const
{
	return std::find(folded.begin(), folded.end(), slot) != folded.end();
}

void View::toggleFold(uint16_t slot)
{
	const auto it = std::find(folded.begin(), folded.end(), slot);
	if (it == folded.end()) folded.push_back(slot);
	else                    folded.erase(it);
}

std::vector<Row> buildRows(const SequenceAsset& seq, const View& view)
{
	std::vector<Row> rows;
	const int cut = cameraCutTrack(seq);
	if (cut >= 0) rows.push_back({ Row::Kind::Track, cut, kSequenceNoBinding, 0 });

	for (const SequenceBinding& b : seq.bindings)
	{
		// A duplicate slot is drawn once, under its first binding — the one the
		// runtime resolves (SequenceEval::resolveBindings).
		if (findBinding(seq, b.slot) != static_cast<int>(&b - seq.bindings.data())) continue;
		rows.push_back({ Row::Kind::Group, -1, b.slot, 0 });
		if (view.isFolded(b.slot)) continue;
		for (int i = 0; i < static_cast<int>(seq.tracks.size()); ++i)
		{
			const SequenceTrack& tr = seq.tracks[i];
			if (tr.kind != SequenceTrackKind::CameraCut && tr.binding == b.slot)
				rows.push_back({ Row::Kind::Track, i, b.slot, 1 });
		}
	}

	// Unbound: no actor, an actor the list does not have, or a second cut
	// track (dead at runtime, but listed so it can be removed).
	std::vector<int> loose;
	for (int i = 0; i < static_cast<int>(seq.tracks.size()); ++i)
	{
		const SequenceTrack& tr = seq.tracks[i];
		if (tr.kind == SequenceTrackKind::CameraCut) { if (i != cut) loose.push_back(i); continue; }
		if (tr.binding == kSequenceNoBinding || findBinding(seq, tr.binding) < 0) loose.push_back(i);
	}
	if (!loose.empty())
	{
		rows.push_back({ Row::Kind::Group, -1, kSequenceNoBinding, 0 });
		if (!view.isFolded(kSequenceNoBinding))
			for (int i : loose) rows.push_back({ Row::Kind::Track, i, kSequenceNoBinding, 1 });
	}
	return rows;
}

bool advancePlayhead(View& view, float duration, float dt)
{
	if (!view.playing) return false;
	if (duration <= 0.0f) { view.playing = false; return false; }
	const float before = view.playhead;
	PropertyAnimationSystem::advance(view.playhead, view.playing, 1.0f, view.loop, duration, dt);
	return view.playhead != before;
}

const Metrics& metrics()
{
	static const Metrics m;
	return m;
}

} // namespace HE::Ed::Cinematic

#if __has_include(<imgui.h>)

#include "EditorHelp.h"       // Help::Scope — "Cinematic/<label>"
#include "EditorWidgets.h"    // helpForKey
#include "UITimelineMath.h"   // seconds ⇄ pixels

#include <imgui.h>

namespace HE::Ed::Cinematic
{

namespace
{
	// The Sequencer's palette, plus one tint per kind of row.
	constexpr ImU32 kRowBg      = IM_COL32( 34,  32,  30, 255);
	constexpr ImU32 kRowBgSel   = IM_COL32( 46,  42,  38, 255);
	constexpr ImU32 kGroupBg    = IM_COL32( 42,  38,  34, 255);
	constexpr ImU32 kRowLine    = IM_COL32( 24,  22,  20, 255);
	constexpr ImU32 kKey        = IM_COL32(230, 170,  60, 255);
	constexpr ImU32 kKeyHot     = IM_COL32(255, 214, 140, 255);
	constexpr ImU32 kKeyRing    = IM_COL32(255, 250, 240, 255);
	constexpr ImU32 kPlayhead   = IM_COL32(255, 236, 200, 230);
	constexpr ImU32 kValueText  = IM_COL32(150, 145, 138, 255);
	constexpr ImU32 kItemText   = IM_COL32(235, 228, 218, 255);
	constexpr ImU32 kMissing    = IM_COL32(230,  90,  80, 255);
	constexpr ImU32 kCut        = IM_COL32(120, 200, 170, 255);
	constexpr ImU32 kCutBlend   = IM_COL32(120, 200, 170,  70);
	constexpr ImU32 kSection    = IM_COL32( 90, 130, 200, 255);
	constexpr ImU32 kSectionHot = IM_COL32(130, 170, 235, 255);
	constexpr ImU32 kEvent      = IM_COL32(230, 120, 160, 255);
	constexpr ImU32 kSound      = IM_COL32(150, 200,  90, 255);
	constexpr ImU32 kVisibleOn  = IM_COL32(230, 170,  60,  60);

	ImU32 kindColour(const SequenceTrack& tr)
	{
		switch (tr.kind)
		{
		case SequenceTrackKind::Property:
		{
			const char* g = Sequencer::targetGroup(tr.channel.target);
			if (g[0] == 'T') return IM_COL32(120, 180, 255, 255);
			if (g[0] == 'M') return IM_COL32(190, 130, 255, 255);
			return IM_COL32(200, 200, 140, 255);   // camera FOV, visibility
		}
		case SequenceTrackKind::Skeletal:  return kSection;
		case SequenceTrackKind::CameraCut: return kCut;
		case SequenceTrackKind::Event:     return kEvent;
		case SequenceTrackKind::Audio:     return kSound;
		}
		return kValueText;
	}

	bool pointerIn(const ImVec2& mp, float x0, float y0, float x1, float y1)
	{
		return ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
		       mp.x >= x0 && mp.x <= x1 && mp.y >= y0 && mp.y <= y1;
	}

	std::string bindingName(const SequenceAsset& seq, uint16_t slot)
	{
		if (slot == kSequenceNoBinding) return "(gameplay camera)";
		const int b = findBinding(seq, slot);
		if (b < 0) return "(missing binding)";
		return seq.bindings[b].name.empty() ? std::string("(unnamed)") : seq.bindings[b].name;
	}

	std::string assetLabel(const Labels& labels, HE::UUID id)
	{
		if (id == HE::UUID{}) return "(none)";
		if (labels.assetName)
		{
			std::string n = labels.assetName(id);
			if (!n.empty()) return n;
		}
		return "(not loaded)";
	}

	// An item's hit box. Press selects and arms; right-click opens its menu.
	struct Hit { bool pressed = false; bool hovered = false; bool remove = false; };
	Hit itemButton(const char* id, float x0, float y0, float x1, float y1, bool held)
	{
		Hit h;
		ImGui::SetCursorScreenPos(ImVec2(x0, y0));
		ImGui::InvisibleButton(id, ImVec2(std::max(x1 - x0, 1.0f), std::max(y1 - y0, 1.0f)));
		EditorWidgets::helpForKey("cinematic.item");
		h.hovered = ImGui::IsItemHovered() || held;
		h.pressed = ImGui::IsItemActivated();
		if (ImGui::BeginPopupContextItem("##cin_itemmenu"))
		{
			// Pushed here too: the help audit reads top to bottom, and this
			// helper stands above draw().
			HE::Ed::Help::Scope helpScope("Cinematic");
			if (EditorWidgets::dangerMenuItem("Delete")) h.remove = true;
			ImGui::EndPopup();
		}
		return h;
	}

	void drawDiamond(ImDrawList* dl, float cx, float cy, float r, ImU32 col, bool picked)
	{
		const ImVec2 d[4] = { { cx, cy - r }, { cx + r, cy }, { cx, cy + r }, { cx - r, cy } };
		dl->AddConvexPolyFilled(d, 4, col);
		if (picked)
		{
			const float o = r + 3.0f;
			const ImVec2 ring[4] = { { cx, cy - o }, { cx + o, cy }, { cx, cy + o }, { cx - o, cy } };
			dl->AddPolyline(ring, 4, kKeyRing, ImDrawFlags_Closed, 1.5f);
		}
	}

	// A flag: a post at the moment, a pennant, and the label beside it.
	void drawFlag(ImDrawList* dl, float x, float y0, float y1, ImU32 col, bool hot, bool picked,
	              const char* label)
	{
		dl->AddLine(ImVec2(x, y0 + 2.0f), ImVec2(x, y1 - 2.0f), hot ? kKeyHot : col, picked ? 2.5f : 1.5f);
		const ImVec2 p[3] = { { x, y0 + 3.0f }, { x + 8.0f, y0 + 7.0f }, { x, y0 + 11.0f } };
		dl->AddConvexPolyFilled(p, 3, hot ? kKeyHot : col);
		if (picked) dl->AddRect(ImVec2(x - 3.0f, y0 + 1.0f), ImVec2(x + 10.0f, y1 - 1.0f), kKeyRing, 2.0f);
		dl->AddText(ImVec2(x + 11.0f, y0 + 3.0f), kItemText, label);
	}
}

Result draw(SequenceAsset& seq, View& view, const ImVec2& size,
            const Labels& labels, const Intent& intent)
{
	Result out;
	const Metrics& M = metrics();
	HE::Ed::Help::Scope helpScope("Cinematic");

	ImGui::BeginChild("##cin_strip", size, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollWithMouse);

	const int trackCount = static_cast<int>(seq.tracks.size());
	if (view.trackSel >= trackCount) { view.trackSel = -1; view.itemSel = -1; }
	if (view.trackSel >= 0 && view.itemSel >= itemCount(seq.tracks[view.trackSel])) view.itemSel = -1;
	if (view.itemSel < 0) { view.armed = false; view.dragging = false; }

	const float duration = std::max(seq.duration, 1e-4f);
	view.playhead = std::clamp(view.playhead, 0.0f, duration);

	const std::vector<Row> rows = buildRows(seq, view);

	const ImVec2 area  = ImGui::GetContentRegionAvail();
	const float  laneW = std::max(60.0f, area.x - M.nameW - M.gap);
	const ImVec2 top   = ImGui::GetCursorScreenPos();
	ImDrawList*  dl    = ImGui::GetWindowDrawList();
	const float  laneL = top.x + M.nameW + M.gap, laneR = laneL + laneW;
	const float  stripBottom = top.y + area.y;
	out.laneX = laneL; out.laneW = laneW; out.top = top.y;

	HE::Ed::UITimelineView tv{ laneL, laneW, duration, view.zoom, view.scroll };
	tv.clampScroll();

	const ImGuiIO& io = ImGui::GetIO();
	const ImVec2 mp = io.MousePos;
	if (pointerIn(mp, laneL, top.y, laneR, stripBottom) && io.MouseWheel != 0.0f)
	{
		if (io.KeyShift) { tv.scroll -= io.MouseWheel * tv.visibleSpan() * 0.15f; tv.clampScroll(); }
		else tv.zoomAt(tv.zoom * std::pow(1.25f, io.MouseWheel), mp.x);
	}
	if (intent.zoom != 0)
	{
		const bool onScreen = view.playhead >= tv.scroll && view.playhead <= tv.scroll + tv.visibleSpan();
		tv.zoomAt(tv.zoom * (intent.zoom > 0 ? 1.5f : 1.0f / 1.5f),
		          onScreen ? tv.xOf(view.playhead) : laneL + laneW * 0.5f);
	}
	if (intent.fit) { tv.zoom = 1.0f; tv.scroll = 0.0f; }
	view.zoom = tv.zoom; view.scroll = tv.scroll;

	// ── The ruler: the Sequencer's, and the same scrub ───────────────────────
	Sequencer::drawRuler(dl, tv, top.y, M.rulerH, stripBottom);
	ImGui::SetCursorScreenPos(ImVec2(laneL, top.y));
	ImGui::InvisibleButton("##cin_ruler", ImVec2(laneW, M.rulerH));
	EditorWidgets::helpForKey("cinematic.ruler");
	if (ImGui::IsItemActivated()) view.scrubbing = true;
	if (view.scrubbing)
	{
		if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			const float t = tv.tOf(mp.x);
			if (t != view.playhead) { view.playhead = t; out.playheadMoved = true; }
		}
		else view.scrubbing = false;
	}

	const float rowsTop = top.y + M.rulerH + 2.0f;
	out.rowsTop = rowsTop;

	bool deleteSelected = false;
	if (view.trackSel >= 0 && view.itemSel >= 0 && !io.WantTextInput &&
	    ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
	    ImGui::IsKeyPressed(ImGuiKey_Delete))
		deleteSelected = true;

	int removeTrackIdx = -1, removeItemTrack = -1, removeItemIdx = -1;
	uint16_t foldToggle = 0; bool doFold = false;

	for (int r = 0; r < static_cast<int>(rows.size()); ++r)
	{
		const Row& row = rows[r];
		const float rowY = rowsTop + M.rowH * static_cast<float>(r);
		const float rowB = rowY + M.rowH;
		ImGui::PushID(r);

		if (row.kind == Row::Kind::Group)
		{
			// ── An actor's header ────────────────────────────────────────────
			const int b = row.slot == kSequenceNoBinding ? -1 : findBinding(seq, row.slot);
			const bool missing = b >= 0 && b < static_cast<int>(labels.missing.size()) && labels.missing[b];
			const bool picked  = view.groupPicked && view.groupSel == row.slot;
			dl->AddRectFilled(ImVec2(top.x, rowY), ImVec2(laneR, rowB), picked ? kRowBgSel : kGroupBg);
			dl->AddLine(ImVec2(top.x, rowB), ImVec2(laneR, rowB), kRowLine);

			ImGui::SetCursorScreenPos(ImVec2(top.x + 2.0f, rowY + (M.rowH - ImGui::GetFrameHeight()) * 0.5f));
			if (ImGui::ArrowButton("##cin_fold", view.isFolded(row.slot) ? ImGuiDir_Right : ImGuiDir_Down))
			{ doFold = true; foldToggle = row.slot; }
			EditorWidgets::helpForKey("cinematic.fold");

			std::string name = b >= 0 ? (seq.bindings[b].name.empty() ? std::string("(unnamed)") : seq.bindings[b].name)
			                          : std::string("Unbound");
			if (missing) name += "  (missing)";
			ImGui::SetCursorScreenPos(ImVec2(top.x + 24.0f, rowY + 2.0f));
			if (missing) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kMissing));
			if (ImGui::Selectable((name + "##cin_group").c_str(), picked, 0,
			                      ImVec2(M.nameW - 24.0f, M.rowH - 4.0f)))
			{
				if (!picked) out.selectionChanged = true;
				view.groupPicked = true;
				view.groupSel    = row.slot;
				view.trackSel    = -1;
				view.itemSel     = -1;
			}
			if (missing) ImGui::PopStyleColor();
			EditorWidgets::helpForKey(b >= 0 ? "cinematic.binding" : "cinematic.unbound");
			if (b >= 0 && ImGui::BeginPopupContextItem("##cin_groupmenu"))
			{
				if (EditorWidgets::dangerMenuItem("Remove Binding"))
				{ out.removeBinding = true; out.removeSlot = row.slot; }
				ImGui::EndPopup();
			}
			ImGui::PopID();
			continue;
		}

		// ── A track row ──────────────────────────────────────────────────────
		const int ti = row.track;
		SequenceTrack& tr = seq.tracks[ti];
		const bool selected = view.trackSel == ti;
		const float indent = M.indent * static_cast<float>(row.depth);

		dl->AddRectFilled(ImVec2(top.x, rowY), ImVec2(laneL - M.gap, rowB), selected ? kRowBgSel : kRowBg);
		dl->AddCircleFilled(ImVec2(top.x + indent + 10.0f, rowY + M.rowH * 0.5f), 3.5f, kindColour(tr));
		ImGui::SetCursorScreenPos(ImVec2(top.x + indent + 20.0f, rowY + 2.0f));
		if (ImGui::Selectable((trackLabel(tr) + "##cin_track").c_str(), selected, 0,
		                      ImVec2(M.nameW - indent - 20.0f, M.rowH - 4.0f)))
		{
			if (!selected) out.selectionChanged = true;
			view.trackSel = ti;
			view.itemSel  = -1;
			view.groupPicked = false;
		}
		EditorWidgets::helpForKey(tr.kind == SequenceTrackKind::CameraCut ? "cinematic.cut-track"
		                                                                   : "cinematic.track");
		if (ImGui::BeginPopupContextItem("##cin_trackmenu"))
		{
			if (EditorWidgets::dangerMenuItem("Remove Track")) removeTrackIdx = ti;
			ImGui::EndPopup();
		}

		// The value at the playhead, for a property track — scrubbing shows
		// numbers move before anything else does.
		if (tr.kind == SequenceTrackKind::Property && !tr.channel.times.empty())
		{
			char val[32];
			Sequencer::formatValue(tr.channel.target,
			                       PropertyAnimationSystem::sampleChannel(tr.channel, view.playhead),
			                       val, sizeof(val));
			const ImVec2 vs = ImGui::CalcTextSize(val);
			dl->AddText(ImVec2(laneL - M.gap - 6.0f - vs.x, rowY + (M.rowH - vs.y) * 0.5f), kValueText, val);
		}

		dl->PushClipRect(ImVec2(laneL, top.y), ImVec2(laneR, stripBottom), true);
		dl->AddRectFilled(ImVec2(laneL, rowY), ImVec2(laneR, rowB), selected ? kRowBgSel : kRowBg);
		dl->AddLine(ImVec2(laneL, rowB), ImVec2(laneR, rowB), kRowLine);
		dl->PopClipRect();

		const int n = itemCount(tr);
		const float cy = rowY + M.rowH * 0.5f;
		bool anyHovered = false;

		// A Visible track is a switch: lit spans where the actor is shown, the
		// runtime's step rule (PropertyAnimationSystem::isStepTarget).
		if (tr.kind == SequenceTrackKind::Property && tr.channel.target == PropTarget::Visible && n > 0)
		{
			dl->PushClipRect(ImVec2(laneL, rowY), ImVec2(laneR, rowB), true);
			for (int k = 0; k < n; ++k)
			{
				if (tr.channel.values[k] < 0.5f) continue;
				const float x0 = k == 0 ? laneL : tv.xOf(tr.channel.times[k]);
				const float x1 = k + 1 < n ? tv.xOf(tr.channel.times[k + 1]) : laneR;
				dl->AddRectFilled(ImVec2(x0, rowY + 4.0f), ImVec2(x1, rowB - 4.0f), kVisibleOn);
			}
			// Before the first key the first key's value holds, as it does at runtime.
			if (tr.channel.values[0] >= 0.5f)
				dl->AddRectFilled(ImVec2(laneL, rowY + 4.0f), ImVec2(tv.xOf(tr.channel.times[0]), rowB - 4.0f), kVisibleOn);
			dl->PopClipRect();
		}

		for (int k = 0; k < n; ++k)
		{
			const bool picked = selected && view.itemSel == k;
			const bool held   = picked && view.dragging;
			ImGui::PushID(k);
			Hit hit;
			View::Grab grab = View::Grab::Move;

			if (tr.kind == SequenceTrackKind::Skeletal)
			{
				const SequenceSkeletalSection& s = tr.sections[k];
				const float x0 = tv.xOf(s.start), x1 = tv.xOf(s.end);
				if (x1 < laneL || x0 > laneR) { ImGui::PopID(); continue; }
				const float bx0 = std::max(x0, laneL), bx1 = std::min(x1, laneR);
				// The edges BEFORE the body: where two items overlap, ImGui
				// gives the hover to the one submitted first, and the edge is
				// the smaller target that has to win.
				Hit hs, he;
				if (x0 >= laneL - M.edgeW)
					hs = itemButton("##cin_start", x0 - M.edgeW * 0.5f, rowY + 2.0f, x0 + M.edgeW, rowB - 2.0f,
					                held && view.grab == View::Grab::Start);
				if (x1 <= laneR + M.edgeW)
					he = itemButton("##cin_end", x1 - M.edgeW, rowY + 2.0f, x1 + M.edgeW * 0.5f, rowB - 2.0f,
					                held && view.grab == View::Grab::End);
				hit = itemButton("##cin_body", bx0, rowY + 2.0f, bx1, rowB - 2.0f, held && view.grab == View::Grab::Move);
				if (hs.pressed) { hit.pressed = true; grab = View::Grab::Start; }
				if (he.pressed) { hit.pressed = true; grab = View::Grab::End; }
				hit.hovered = hit.hovered || hs.hovered || he.hovered;
				hit.remove  = hit.remove || hs.remove || he.remove;

				dl->PushClipRect(ImVec2(laneL, rowY), ImVec2(laneR, rowB), true);
				dl->AddRectFilled(ImVec2(x0, rowY + 3.0f), ImVec2(x1, rowB - 3.0f), hit.hovered ? kSectionHot : kSection, 3.0f);
				if (picked) dl->AddRect(ImVec2(x0, rowY + 2.0f), ImVec2(x1, rowB - 2.0f), kKeyRing, 3.0f, 0, 1.5f);
				const std::string lbl = assetLabel(labels, s.clipId);
				dl->PushClipRect(ImVec2(bx0, rowY), ImVec2(bx1, rowB), true);
				dl->AddText(ImVec2(std::max(x0, laneL) + 5.0f, rowY + 4.0f), kItemText, lbl.c_str());
				dl->PopClipRect();
				dl->PopClipRect();
			}
			else if (tr.kind == SequenceTrackKind::Property)
			{
				const float cx = tv.xOf(tr.channel.times[k]);
				if (cx < laneL - M.keyR || cx > laneR + M.keyR) { ImGui::PopID(); continue; }
				hit = itemButton("##cin_key", cx - M.keyR, cy - M.keyR, cx + M.keyR, cy + M.keyR, held);
				dl->PushClipRect(ImVec2(laneL, top.y), ImVec2(laneR, stripBottom), true);
				drawDiamond(dl, cx, cy, M.keyR, hit.hovered ? kKeyHot : kKey, picked);
				dl->PopClipRect();
			}
			else
			{
				// Cut, event, sound: a flag at the moment.
				const float t  = itemTime(tr, k);
				const float cx = tv.xOf(t);
				if (cx < laneL - M.keyR || cx > laneR + M.keyR) { ImGui::PopID(); continue; }
				hit = itemButton("##cin_flag", cx - M.keyR, rowY + 1.0f, cx + M.keyR + 4.0f, rowB - 1.0f, held);
				std::string lbl;
				ImU32 col = kindColour(tr);
				dl->PushClipRect(ImVec2(laneL, rowY), ImVec2(laneR, rowB), true);
				if (tr.kind == SequenceTrackKind::CameraCut)
				{
					const SequenceCameraCut& c = tr.cuts[k];
					lbl = bindingName(seq, c.binding);
					if (c.blendIn > 0.0f)
					{
						// The blend-in as a ramp: the view is still arriving.
						const float x1 = tv.xOf(t + c.blendIn);
						dl->AddTriangleFilled(ImVec2(cx, rowB - 2.0f), ImVec2(x1, rowY + 2.0f), ImVec2(x1, rowB - 2.0f), kCutBlend);
					}
				}
				else if (tr.kind == SequenceTrackKind::Event)
				{
					const AnimationNotify& e = tr.events[k];
					lbl = e.name.empty() ? std::string("(unnamed)") : e.name;
					if (e.duration > 0.0f)
						dl->AddRectFilled(ImVec2(cx, rowB - 5.0f), ImVec2(tv.xOf(t + e.duration), rowB - 2.0f), col);
				}
				else
				{
					lbl = assetLabel(labels, tr.audio[k].assetId);
				}
				drawFlag(dl, cx, rowY, rowB, col, hit.hovered, picked, lbl.c_str());
				dl->PopClipRect();
			}

			anyHovered = anyHovered || hit.hovered;
			if (hit.pressed)
			{
				if (!picked) out.selectionChanged = true;
				view.trackSel    = ti;
				view.itemSel     = k;
				view.groupPicked = false;
				const float at = grab == View::Grab::End ? tr.sections[k].end : itemTime(tr, k);
				if (view.playhead != at) out.playheadMoved = true;
				view.playhead   = std::clamp(at, 0.0f, duration);
				view.scrubbing  = false;
				view.armed      = true;
				view.dragging   = false;
				view.grab       = grab;
				view.grabOffset = tv.tOf(mp.x) - itemTime(tr, k);
			}
			if (hit.remove) { removeItemTrack = ti; removeItemIdx = k; }
			ImGui::PopID();
		}

		// A double-click on an empty spot adds what this row holds, where the
		// pointer is. Not on skeletal or sound rows: those need an asset, and
		// the readout's Add button is where one is picked.
		if (!anyHovered && !view.armed && pointerIn(mp, laneL, rowY, laneR, rowB) &&
		    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
		{
			const float t = tv.tOf(mp.x);
			int k = -1;
			switch (tr.kind)
			{
			case SequenceTrackKind::Property:
				k = Sequencer::insertKey(tr.channel, t, tr.channel.times.empty()
					? Sequencer::defaultValue(tr.channel.target)
					: PropertyAnimationSystem::sampleChannel(tr.channel, t));
				break;
			case SequenceTrackKind::Event:     k = insertEvent(tr, t, "Event"); break;
			case SequenceTrackKind::CameraCut: k = insertCut(tr, t, defaultCutSlot(seq, t, labels.camera)); break;
			default: break;
			}
			if (k >= 0)
			{
				view.trackSel = ti;
				view.itemSel  = k;
				view.groupPicked = false;
				view.playhead = std::clamp(itemTime(tr, k), 0.0f, duration);
				out.edited = out.committed = out.selectionChanged = out.playheadMoved = true;
			}
		}
		ImGui::PopID();
	}

	const float rowsBottom = rowsTop + M.rowH * static_cast<float>(rows.size());

	// ── The drag of the armed item ───────────────────────────────────────────
	if (view.armed && view.trackSel >= 0 && view.itemSel >= 0)
	{
		SequenceTrack& tr = seq.tracks[view.trackSel];
		if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			if (view.dragging || ImGui::IsMouseDragging(ImGuiMouseButton_Left))
			{
				view.dragging = true;
				const float pt = tv.tOf(mp.x);
				int nk = -1;
				float at = 0.0f;
				switch (view.grab)
				{
				case View::Grab::Move:
				{
					float to = std::max(0.0f, pt - view.grabOffset);
					// A bar stops with its END at the sequence's end, the way a
					// key stops there: past it, nobody could reach it again.
					if (tr.kind == SequenceTrackKind::Skeletal)
					{
						const SequenceSkeletalSection& s = tr.sections[view.itemSel];
						to = std::min(to, std::max(0.0f, duration - (s.end - s.start)));
					}
					nk = moveItem(tr, view.itemSel, to);
					if (nk >= 0) at = itemTime(tr, nk);
					break;
				}
				case View::Grab::Start:
					nk = setSectionStart(tr, view.itemSel, pt);
					if (nk >= 0) at = tr.sections[nk].start;
					break;
				case View::Grab::End:
					nk = setSectionEnd(tr, view.itemSel, std::min(pt, duration));
					if (nk >= 0) at = tr.sections[nk].end;
					break;
				}
				if (nk >= 0)
				{
					view.itemSel = nk;
					if (view.playhead != at) out.playheadMoved = true;
					view.playhead = std::clamp(at, 0.0f, duration);
					out.edited = true;
				}
			}
		}
		else
		{
			if (view.dragging) out.committed = true;
			view.armed = false;
			view.dragging = false;
		}
	}

	// ── Removals and folds, after the loops ──────────────────────────────────
	if (deleteSelected && removeItemIdx < 0) { removeItemTrack = view.trackSel; removeItemIdx = view.itemSel; }
	if (removeItemTrack >= 0 && removeItem(seq.tracks[removeItemTrack], removeItemIdx))
	{
		if (view.trackSel == removeItemTrack) view.itemSel = -1;
		view.armed = view.dragging = false;
		out.edited = out.committed = out.selectionChanged = true;
	}
	if (removeTrackIdx >= 0 && removeTrack(seq, removeTrackIdx))
	{
		if (view.trackSel == removeTrackIdx) { view.trackSel = -1; view.itemSel = -1; }
		else if (view.trackSel > removeTrackIdx) --view.trackSel;
		view.armed = view.dragging = false;
		out.edited = out.committed = out.selectionChanged = true;
	}
	if (doFold) view.toggleFold(foldToggle);

	if (rows.empty())
	{
		ImGui::SetCursorScreenPos(ImVec2(top.x + 8.0f, rowsTop + 6.0f));
		ImGui::TextDisabled("Nothing here yet. Bind an actor above (select it in the scene, then");
		ImGui::SetCursorScreenPos(ImVec2(top.x + 8.0f, rowsTop + 6.0f + ImGui::GetTextLineHeight()));
		ImGui::TextDisabled("Bind Selected) and give it tracks, or add a Camera Cuts track.");
	}

	// ── The playhead ─────────────────────────────────────────────────────────
	{
		const float x = tv.xOf(view.playhead);
		const float bottom = std::max(rowsBottom, top.y + M.rulerH + 24.0f);
		dl->PushClipRect(ImVec2(laneL, top.y), ImVec2(laneR, stripBottom), true);
		dl->AddLine(ImVec2(x, top.y), ImVec2(x, bottom), kPlayhead, 1.5f);
		const ImVec2 tri[3] = { { x - 5.0f, top.y }, { x + 5.0f, top.y }, { x, top.y + 6.0f } };
		dl->AddConvexPolyFilled(tri, 3, kPlayhead);
		dl->PopClipRect();
	}

	ImGui::SetCursorScreenPos(ImVec2(top.x, std::max(rowsBottom, rowsTop + 2.0f * M.rowH)));
	ImGui::Dummy(ImVec2(1.0f, 1.0f));
	ImGui::EndChild();
	return out;
}

} // namespace HE::Ed::Cinematic

#endif // __has_include(<imgui.h>)
