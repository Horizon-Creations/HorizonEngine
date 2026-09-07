#pragma once
// Internal header: the per-playhead half of notifies, shared by the clip, blend
// and state-machine systems. The rule it holds — clamp, prime, dominance — is
// three lines, and three lines copied into five call sites is three lines that
// drift apart. AnimationNotify.h holds the arithmetic; this holds the etiquette.
#include <HorizonScene/AnimationNotify.h>
#include <ContentManager/Assets.h>
#include <entt/entt.hpp>
#include <algorithm>

namespace HE {

// One playhead's notifies over the span (tPrev, tEnd].
//
// out == nullptr: nothing is collected AND `primed` is left alone. That is the
// editor's case — the queue costs nothing there, and when a play session starts
// the playhead primes on its first real frame instead of having spent its one
// priming on a frame nobody was listening to.
//
// dominant == false: the playhead is marked primed and nothing is emitted. The
// light half of a crossfade still has to be walked past, or it would keep its
// priming and dump a frame-0 notify the moment the weight tipped over to it.
//
// The non-looping clamp lives here rather than in five call sites: a playhead
// that has just stopped at the end would otherwise hand the walk a span running
// past the clip, and the lap decomposition would read that as a wrap.
inline void notifyCollectClip(entt::entity e, const AnimationClipAsset& clip,
                              float tPrev, float tEnd, bool looping,
                              bool dominant, bool& primed, NotifyQueue* out)
{
    if (!out) return;

    // A playhead's FIRST evaluated frame starts exactly where it stands, and the
    // span is open at its origin — so a notify sitting on that very spot (the
    // footstep on frame 0, which is the ordinary authoring case) would fall just
    // outside it forever. Closing the origin edge once fixes that, for the first
    // frame of a fresh component and for the incoming clip of a transition alike.
    const bool includeStart = !primed;
    primed = true;
    if (!dominant) return;

    if (!looping) tEnd = std::clamp(tEnd, 0.0f, clip.duration);
    collectNotifies(clip, static_cast<uint32_t>(e), tPrev, tEnd, includeStart, *out);
}

} // namespace HE
