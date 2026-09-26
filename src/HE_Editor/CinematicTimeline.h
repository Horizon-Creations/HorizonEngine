#pragma once
#include <ContentManager/Assets.h>   // SequenceAsset, SequenceTrack, PropTarget

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#if __has_include(<imgui.h>)
#include <imgui.h>
#endif

// ── The Cinematic tab's strip: bindings, tracks, cuts, sections, events ──────
// A cinematic Sequence (SequenceAsset) is several actors on one clock, so the
// strip is grouped by actor: the Camera Cuts row on top, then one collapsible
// group per binding with that actor's tracks under it, then the tracks that
// belong to nobody (music, narration, events for the owner). Four kinds of row
// draw four kinds of thing on the shared time axis:
//
//   key row      a property track: diamonds, as in the Sequencer (Visible as a
//                step, lit where the actor is shown)
//   section row  a skeletal track: bars from start to end, dragged to move and
//                at either edge to trim
//   marker row   events, sounds and camera cuts: a flag at a moment, dragged
//                along the axis; an event's duration and a cut's blend-in are
//                drawn behind the flag but edited in the readout
//   group row    an actor's header: its name (red when the scene has no such
//                entity), the arrow that folds its tracks away
//
// Kept free of AppContext for the same reason SequencerTimeline is: the tab
// panel (CinematicPanel) owns loading, saving, undo, the bindings' entities and
// the preview, and hands the asset and the view in here. The headless test in
// tests/test_cinematic_timeline.cpp drives this over a SequenceAsset on the
// stack. The ruler and the seconds ⇄ pixels arithmetic are the Sequencer's
// (drawRuler, UITimelineView), and property keys are edited with the
// Sequencer's insertKey/moveKey/removeKey — one rule for "a key", two strips.
namespace HE::Ed::Cinematic
{
	// ── Editing the sequence ─────────────────────────────────────────────────
	// Plain functions over the asset, outside the ImGui guard, tested without a
	// context. What they keep: every list the strip draws stays sorted by time
	// (cuts, events, sounds, sections by start), so the item under the pointer
	// is found the same way it is drawn, and a binding's slot never changes once
	// handed out — tracks name their actor by slot.

	// A new actor. Its slot is one past the highest in use (0 for the first),
	// never a reused one: a slot freed by removeBinding may still be named by a
	// script's bindSlot or a copy of the asset. Returns the slot.
	uint16_t addBinding(SequenceAsset& seq, const std::string& name, HE::UUID entityId);
	// Index into seq.bindings of `slot`, or -1.
	int findBinding(const SequenceAsset& seq, uint16_t slot);
	// The actor and everything that is only about it: its tracks, and the
	// camera cuts to it (a cut to a camera that is gone would hand the view
	// back to gameplay at a moment nobody chose). Other slots keep their
	// numbers. False when there is no such binding.
	bool removeBinding(SequenceAsset& seq, uint16_t slot);

	// The single Camera Cuts track, or -1. Only the first counts at runtime
	// (SequenceEval::evaluate), so the strip never makes a second.
	int cameraCutTrack(const SequenceAsset& seq);
	// A property track for (slot, target) whose first key, at 0, holds `value`
	// — the actor's current value, so adding a track moves nothing. The
	// existing track's index when there is one: two tracks on one target of one
	// actor would only overwrite each other.
	int addPropertyTrack(SequenceAsset& seq, uint16_t slot, PropTarget target, float value);
	int findPropertyTrack(const SequenceAsset& seq, uint16_t slot, PropTarget target);
	// An empty track of `kind` on `slot` (kSequenceNoBinding for an event or a
	// sound that belongs to nobody). CameraCut returns the existing one if the
	// sequence has it, and ignores `slot`: cuts name their camera one by one.
	// Property goes through addPropertyTrack. -1 for a kind that needs an actor
	// (Skeletal) without one.
	int addTrack(SequenceAsset& seq, SequenceTrackKind kind, uint16_t slot);
	bool removeTrack(SequenceAsset& seq, int track);

	// Cuts, events, sounds: an item at `t`, inserted in time order (after any
	// at the same instant — the later-listed one wins a tie at runtime, and the
	// one just added is the one the author means). Return its index.
	int insertCut(SequenceTrack& tr, float t, uint16_t cameraSlot);
	int insertEvent(SequenceTrack& tr, float t, const std::string& name);
	int insertSound(SequenceTrack& tr, float t, HE::UUID assetId);
	// A skeletal section starting at `start`, `length` long, playing `clipId`
	// from its beginning. Sorted by start.
	int insertSection(SequenceTrack& tr, float start, float length, HE::UUID clipId);

	// The item of whatever this track holds (cut, event, sound, section; a
	// property track's key) — how many, where it starts, and moving it.
	int  itemCount(const SequenceTrack& tr);
	float itemTime(const SequenceTrack& tr, int k);
	// Item `k` to start at `t` (a section keeps its length), re-sorted. Returns
	// the item's index afterwards — it changes when it passes a neighbour. A
	// property key goes through Sequencer::moveKey and so never lands on another.
	int  moveItem(SequenceTrack& tr, int k, float t);
	bool removeItem(SequenceTrack& tr, int k);
	// A section's edges, each kept at least kMinSection from the other; the
	// start edge re-sorts. Returns the section's index afterwards.
	int setSectionStart(SequenceTrack& tr, int k, float start);
	int setSectionEnd(SequenceTrack& tr, int k, float end);
	constexpr float kMinSection = 0.05f;

	// The latest moment anything happens: a key, a cut, an event's end, a
	// sound's start, a section's end. The sequence may not end before it.
	float lastContentTime(const SequenceAsset& seq);
	// A new length, never under lastContentTime — the Sequencer's rule, for the
	// same reason. The length set is returned.
	float setDuration(SequenceAsset& seq, float duration);
	// What a sequence of no length gets when its first track is added.
	constexpr float kDefaultDuration = 5.0f;

	// "Camera Cuts", "Skeletal Animation", "Events", "Sound", and for a property
	// track its target's name ("Position X"). The row's label.
	const char* kindName(SequenceTrackKind k);
	std::string trackLabel(const SequenceTrack& tr);

	// ── The rows ─────────────────────────────────────────────────────────────
	// The strip's layout as data, so the test can ask which row is where
	// without drawing. Order: the Camera Cuts row; then per binding (list
	// order) its group row and, unless folded, its tracks (track order); then,
	// if there are any, an "Unbound" group with the tracks whose slot is none
	// or names no binding.
	struct Row
	{
		enum class Kind : uint8_t { Group, Track };
		Kind     kind  = Kind::Track;
		int      track = -1;                    // Track rows
		uint16_t slot  = kSequenceNoBinding;    // Group rows: the binding (none = Unbound)
		int      depth = 0;                     // 1 under a group
	};

	// ── The view ─────────────────────────────────────────────────────────────
	// Per open tab, never saved into the asset.
	struct View
	{
		float playhead = 0.0f, zoom = 1.0f, scroll = 0.0f;
		int   trackSel = -1;   // index into seq.tracks, or -1
		int   itemSel  = -1;   // key / cut / event / sound / section within it
		// A group row picked (its binding), for the panel's "rebind" and the
		// readout. kSequenceNoBinding with groupPicked = the Unbound group.
		bool     groupPicked = false;
		uint16_t groupSel    = kSequenceNoBinding;
		bool  scrubbing = false;
		// The press on the selected item, and what it grabbed: the whole item
		// or one edge of a section. The offset is pointer time minus the item's
		// start at the press, so a bar does not jump to put its start under the
		// pointer.
		enum class Grab : uint8_t { Move, Start, End };
		bool  armed    = false;
		bool  dragging = false;
		Grab  grab     = Grab::Move;
		float grabOffset = 0.0f;
		// Folded groups, by binding slot (kSequenceNoBinding for Unbound).
		std::vector<uint16_t> folded;
		bool  playing = false;
		bool  loop    = false;

		bool isFolded(uint16_t slot) const;
		void toggleFold(uint16_t slot);
	};

	std::vector<Row> buildRows(const SequenceAsset& seq, const View& view);

	// The playhead after `dt` of playback — the runtime's advance rule, as the
	// Sequencer's transport has it.
	bool advancePlayhead(View& view, float duration, float dt);

	struct Intent { int zoom = 0; bool fit = false; };

	// What the strip needs to know about the world, without AppContext. All
	// optional: an empty context draws every actor as present and every asset
	// by its id.
	struct Labels
	{
		// Per binding (index into seq.bindings): the scene has no entity with
		// this binding's id. Drawn red, "missing".
		std::vector<bool> missing;
		// Per binding: its entity has a CameraComponent. What a new cut aims at
		// when there is no earlier cut to follow.
		std::vector<bool> camera;
		// A clip's or a sound's display name, for the bars and the flags.
		std::function<std::string(HE::UUID)> assetName;
		// A property track's value readout needs nothing more than the channel;
		// the name column's value is sampled there.
	};

	// The camera a new cut at `t` goes to: the one live just before `t` (so a
	// new cut changes nothing until it is pointed elsewhere), else the first
	// binding `camera` marks, else the first binding, else none.
	uint16_t defaultCutSlot(const SequenceAsset& seq, float t, const std::vector<bool>& camera);

	struct Result
	{
		bool playheadMoved = false, selectionChanged = false;
		bool edited = false, committed = false;
		// Where the strip landed, for the test: lane left edge and width, the
		// strip's top, the first row's top. Row i spans
		// [rowsTop + i*rowH, rowsTop + (i+1)*rowH).
		float laneX = 0.0f, laneW = 0.0f, top = 0.0f, rowsTop = 0.0f;
		// A group's context menu asked for its binding to go. The panel does it
		// (it owns undo), so the strip only reports.
		bool     removeBinding = false;
		uint16_t removeSlot    = kSequenceNoBinding;
	};

	struct Metrics
	{
		float nameW  = 230.0f;
		float rulerH = 18.0f;
		float rowH   = 22.0f;
		float gap    = 8.0f;
		float keyR   = 6.0f;
		float edgeW  = 5.0f;    // the grab zone at either end of a section bar
		float indent = 14.0f;   // a track under its group
	};
	const Metrics& metrics();

#if __has_include(<imgui.h>)
	// Draws the strip at the cursor, `size` big (zero = the rest). The ruler
	// scrubs; an item takes a click (select, playhead onto it) and a drag (move;
	// a section's edge trims); a double-click on an empty spot of a key, event
	// or cut row adds one there (a key holds the track's value, an event is
	// called "Event", a cut goes to the camera live at that moment — or, before
	// the first, to the first binding Labels::camera marks, else the first
	// binding; defaultCutSlot says which); Delete removes the
	// selected item; right-click an item, a track name or a group for its menu;
	// the wheel zooms, Shift+wheel pans. Every edit goes through the functions
	// above.
	Result draw(SequenceAsset& seq, View& view, const ImVec2& size,
	            const Labels& labels = {}, const Intent& intent = {});
#endif
}
