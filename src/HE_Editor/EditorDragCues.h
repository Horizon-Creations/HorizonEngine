#pragma once

// Drag and drop cues for the HorizonCode graph canvas (topic 140, step 4).
//
// The canvas (GraphEditor::draw) already has its drag and drop; nothing here
// changes what it does. It reports five events through the optional
// Model::onDragCue, and only HcGraphHost::buildModel sets that — the material,
// particle and animator canvases stay silent and pay nothing.
//
//   Pickup       a wire leaves a pin (or a connected input is grabbed and its
//                wire comes loose), or a variable / element from the side list
//                starts to be dragged over the canvas
//   OverValid    the wire in hand enters a pin it would connect to (canConnect),
//                or a payload the canvas accepts enters the canvas
//   OverInvalid  …a pin it would not (wrong side, same node, refused type), or
//                a payload the canvas does not take
//   Drop         it landed: a wire connected, a node made from the drag-off
//                menu or a quick-spawn key, a payload delivered
//   Cancel       it did not: released on a pin that refused it, a detached
//                wire let go in empty space, the drag-off menu closed with
//                nothing picked, a payload let go somewhere else
//
// Edges only, never per frame: a hover cue sounds when the pin under the
// cursor CHANGES, leaving a pin for empty canvas is silent, and moving a node
// is silent (it has no target). Pickup → hover → Drop stays a handful of
// clicks per gesture.
//
// Not reward moments: no footer line, nothing counted, and not through the
// Feed — its 2 s gap would swallow everything after the pickup. Instead
// Rewards::postDragCue queues the cue, the next pollBuild plays it if
// dragCueWanted (master, Success Sound, Drag and Drop Sound, volume > 0, not
// muted, not during Play — no focus gate, the user is dragging right now) and
// the Gate below lets it through. A reward tone that played in the same frame
// wins: the cue is dropped rather than heard on top of it.
//
// The sounds (Rewards::dragCuePcm16), all shorter (≤ 60 ms) and quieter
// (peak ≤ 0.6 × the save tick's) than the quietest reward tone, same rules
// otherwise (≥ 4 ms onset, end on exactly 0, nothing under 600 Hz, A major
// pentatonic):
//   Pickup       E6 → A6, a short blip up
//   OverValid    C#7, a very light tick
//   OverInvalid  B5, a muted tick, quicker damping — no buzz
//   Drop         E6 with a click on the attack, a snap
//   Cancel       A6 → E6, the pickup turned down, softer onset

namespace HE::Ed
{
enum class DragCue { Pickup, OverValid, OverInvalid, Drop, Cancel };
inline constexpr int kDragCueCount = 5;

namespace DragCues
{
inline constexpr double kHoverGapSec  = 0.05;  // between two hover cues (zig-zag over a pin row)
inline constexpr double kRepeatGapSec = 0.05;  // the same cue twice (two canvases, one event)

inline bool isHover(DragCue c) { return c == DragCue::OverValid || c == DragCue::OverInvalid; }

// The cues' own pacing, ImGui-free (the tests drive it with their own clock).
// take() books the cue when it may sound at `now`.
class Gate
{
public:
	bool take(DragCue c, double now)
	{
		if (m_has && c == m_last && now - m_lastAt < kRepeatGapSec && now >= m_lastAt)
			return false;
		if (isHover(c) && m_hovered && now - m_hoverAt < kHoverGapSec && now >= m_hoverAt)
			return false;
		m_has = true; m_last = c; m_lastAt = now;
		if (isHover(c)) { m_hovered = true; m_hoverAt = now; }
		return true;
	}

private:
	bool    m_has     = false;
	DragCue m_last    = DragCue::Pickup;
	double  m_lastAt  = 0.0;
	bool    m_hovered = false;
	double  m_hoverAt = 0.0;
};
} // namespace DragCues
} // namespace HE::Ed
