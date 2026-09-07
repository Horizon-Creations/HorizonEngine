#include <HorizonScene/AnimationNotify.h>
#include <Diagnostics/Log.h>

#include <algorithm>
#include <cmath>

namespace
{

// Same guard, same number and the same reason as the root-motion walk: a span
// covering more laps than this is a stall or a runaway playback speed, and
// firing every notify sixty thousand times would be a worse answer than saying
// so once.
constexpr int kMaxRounds = 64;

// Sort key inside ONE instant: a window opens, point events happen inside it,
// the window closes. A state short enough to fit entirely in one frame therefore
// comes out Begin then End and never the other way round.
int kindRank(HE::AnimationNotifyEvent::Kind k)
{
    switch (k)
    {
        case HE::AnimationNotifyEvent::Kind::Begin: return 0;
        case HE::AnimationNotifyEvent::Kind::Fire:  return 1;
        case HE::AnimationNotifyEvent::Kind::End:   return 2;
    }
    return 1;
}

// Everything the playhead meets on ONE segment, which by construction does not
// cross the clip's edge. `from` → `to` is the direction of travel; the
// destination edge is always inside, the origin edge only when `closedFrom`.
void emitSegment(const AnimationClipAsset& clip, uint32_t entity,
                 float from, float to, bool closedFrom, HE::NotifyQueue& out)
{
    using Kind = HE::AnimationNotifyEvent::Kind;

    const bool  fwd = (to >= from);
    const float lo  = fwd ? from : to;
    const float hi  = fwd ? to   : from;

    const auto inSpan = [&](float t)
    {
        if (t < lo || t > hi) return false;
        return closedFrom || t != from;
    };

    struct Pending { float t; Kind kind; const std::string* name; };
    std::vector<Pending> pending;

    for (const AnimationNotify& n : clip.notifies)
    {
        if (n.duration > 0.0f)
        {
            if (inSpan(n.time)) pending.push_back({ n.time, Kind::Begin, &n.name });
            // Clamped to the clip: a state authored to run past the end of its own
            // clip ends WITH the clip. Wrapping it into the next lap instead would
            // give a non-looping clip an End before its Begin.
            const float end = std::min(n.time + n.duration, clip.duration);
            if (inSpan(end)) pending.push_back({ end, Kind::End, &n.name });
        }
        else if (inSpan(n.time))
        {
            pending.push_back({ n.time, Kind::Fire, &n.name });
        }
    }
    if (pending.empty()) return;

    // The order the playhead met them — mirrored whole for a rewind, so a
    // reversed clip reports the same events in the opposite order and never a
    // different set of them.
    std::stable_sort(pending.begin(), pending.end(),
                     [fwd](const Pending& a, const Pending& b)
                     {
                         if (a.t != b.t) return fwd ? a.t < b.t : a.t > b.t;
                         return fwd ? kindRank(a.kind) < kindRank(b.kind)
                                    : kindRank(a.kind) > kindRank(b.kind);
                     });

    for (const Pending& p : pending)
        out.push_back(HE::AnimationNotifyEvent{ entity, *p.name, p.kind });
}

} // namespace

void HE::collectNotifies(const AnimationClipAsset& clip, uint32_t entity,
                         float tPrev, float tEnd, bool includeStart, NotifyQueue& out)
{
    if (clip.duration <= 0.0f || clip.notifies.empty()) return;
    if (tPrev == tEnd && !includeStart) return;

    const float D = clip.duration;

    // The playhead is always inside the clip; the far end is what may leave it.
    float a         = std::clamp(tPrev, 0.0f, D);
    float e         = a + (tEnd - tPrev);
    bool  closed    = includeStart;
    int   rounds    = 0;
    bool  exhausted = false;

    if (e >= a)
    {
        while (e > D)
        {
            if (rounds++ >= kMaxRounds) { exhausted = true; break; }
            emitSegment(clip, entity, a, D, closed, out);
            e -= D;
            a  = 0.0f;
            // The seam the playhead just crossed. `duration` belonged to the lap
            // that ended, `0` belongs to the one that starts here, and closing
            // this edge is what makes a notify on frame 0 fire once per lap
            // instead of never.
            closed = true;
        }
        emitSegment(clip, entity, a, std::min(e, D), closed, out);
    }
    else
    {
        while (e < 0.0f)
        {
            if (rounds++ >= kMaxRounds) { exhausted = true; break; }
            emitSegment(clip, entity, a, 0.0f, closed, out);
            e += D;
            a  = D;
            closed = true;
        }
        emitSegment(clip, entity, a, std::max(e, 0.0f), closed, out);
    }

    if (exhausted)
        HE_LOG_THROTTLE(Animation, Warning, 5.0,
                        "Notify span (%.3f s .. %.3f s) covers more than %d laps of a "
                        "%.3f s clip — the rest is dropped. A frame that long is a stall "
                        "or a runaway playback speed, not animation anyone authored.",
                        static_cast<double>(tPrev), static_cast<double>(tEnd),
                        kMaxRounds, static_cast<double>(D));
}
