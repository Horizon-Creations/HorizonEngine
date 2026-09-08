#pragma once
#include <ContentManager/Assets.h>
#include <cstdint>
#include <string>
#include <vector>

// ── Animation notifies ───────────────────────────────────────────────────────
// The events an artist put ON the timeline, turned into something gameplay can
// hear: the footstep at 0.31 s, the frame the sword starts hurting, the moment
// the smoke puff belongs to.
//
// Public for the same reason RootMotion.h is: three pose drivers, the editor's
// preview and the tests all need the identical firing rule, and none of them
// share an ECS. The ECS half is AnimationNotifySystem, which does nothing but
// hand a collected queue to whatever code sits on each entity.
namespace HE {

// One fired event, waiting to be delivered. Nothing but the entity, the name and
// which of the three things happened — a notify carries no other payload, and
// inventing one (a time, a clip id) would be a second thing to keep true.
struct AnimationNotifyEvent
{
    enum class Kind : uint8_t { Fire, Begin, End };

    uint32_t    entity = 0;
    std::string name;
    Kind        kind = Kind::Fire;
};

// Collected during the animation phase, drained once afterwards. A plain vector
// because the whole life of the thing is "filled this frame, emptied this frame".
using NotifyQueue = std::vector<AnimationNotifyEvent>;

// ── The firing rule ──────────────────────────────────────────────────────────
// Over the UNWRAPPED span (tPrev, tEnd], exactly the span the root-motion delta
// is taken over. Unwrapped is what makes it decidable: out of two wrapped
// playheads neither the direction nor the number of laps can be recovered, so a
// clip running one and a half laps in a frame would look like half a frame, and
// a rewind would look like a wrap.
//
// Which edges are closed, in one sentence: THE ONLY OPEN EDGE IS THE ORIGIN.
// The destination is closed, and so is every seam the walk crosses — the clip's
// end on the way out of a lap, and 0 on the way into the next one. That is what
// makes a notify at the seam fire exactly once per lap: going forward it belongs
// to the lap it ENDS (at `duration`) and to the lap it BEGINS (at 0), and those
// are two different laps, never the same one twice.
//
// The origin being open is why `includeStart` exists. A playhead's very first
// evaluated frame starts AT its own position, and a notify sitting exactly there
// — the footstep on frame 0, which is the ordinary authoring case and not the
// exception — would fall just outside the span forever. Passing true on that one
// frame closes the origin edge too. (An epsilon nudge would do the same thing and
// then be silently eaten by the clamp the caller applies for a non-looping clip.)
//
// A NOTIFY STATE fires Begin at `time` and End at `time + duration`, the latter
// clamped to the clip's end: a state authored past the end of its own clip ends
// with the clip rather than never. Within one frame both edges can fall in the
// same span, and they come out Begin before End.
//
// Rewind (tEnd < tPrev) mirrors all of it: the same timestamps, the same kinds,
// walked backwards. A reversed attack therefore reports the window's End first.
// Anything else would be a second semantics for the same authored data.
//
// Events are appended to `out` in the order the playhead met them.
void collectNotifies(const AnimationClipAsset& clip, uint32_t entity,
                     float tPrev, float tEnd, bool includeStart, NotifyQueue& out);

// Which half of a crossfade is allowed to fire: the outgoing playhead below this
// weight, the incoming one at or above it. Only one of them, because letting both
// fire means two footsteps in every single transition — the complaint this whole
// feature would otherwise earn on its first day.
//
// A named constant rather than 0.5 in two comparisons: the blend system and the
// state machine have to agree, and a number written twice is a number that can
// drift once.
inline constexpr float kNotifyDominanceAlpha = 0.5f;

} // namespace HE
