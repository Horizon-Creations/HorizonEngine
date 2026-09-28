#include <HorizonScene/SequenceEval.h>
#include <HorizonScene/CameraPose.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/PropertyAnimationSystem.h>

#include <algorithm>

namespace
{

// SequenceBlendCurve lives in HE_Core, which cannot see HE::BlendCurve. The two
// are the same list in the same order; these asserts are what keeps them so.
static_assert(static_cast<int>(SequenceBlendCurve::Linear)     == static_cast<int>(HE::BlendCurve::Linear));
static_assert(static_cast<int>(SequenceBlendCurve::SmoothStep) == static_cast<int>(HE::BlendCurve::SmoothStep));
static_assert(static_cast<int>(SequenceBlendCurve::EaseOut)    == static_cast<int>(HE::BlendCurve::EaseOut));

HE::BlendCurve toBlendCurve(SequenceBlendCurve c)
{
    return static_cast<HE::BlendCurve>(static_cast<int>(c));
}

// Cuts are ordered by (time, position in the list): the loader keeps them sorted
// by time, stably, but a sequence built in code or mid-edit need not be, and the
// answer must not depend on that. Two cuts at one instant: the later-listed wins.
bool cutBefore(const std::vector<SequenceCameraCut>& cuts, int a, int b)
{
    if (cuts[a].time != cuts[b].time) return cuts[a].time < cuts[b].time;
    return a < b;
}

HE::SequenceEval::CameraState cameraAt(const std::vector<SequenceCameraCut>& cuts, float t)
{
    HE::SequenceEval::CameraState cam;
    const int n = static_cast<int>(cuts.size());

    // The live cut: the last one at or before t. At exactly its time a cut has
    // happened — the frame that lands on the cut shows the new camera.
    int cur = -1;
    for (int i = 0; i < n; ++i)
        if (cuts[i].time <= t && (cur < 0 || cutBefore(cuts, cur, i))) cur = i;
    if (cur < 0) return cam;

    // The one it replaced: the latest cut ordered before it.
    int prev = -1;
    for (int j = 0; j < n; ++j)
        if (j != cur && cutBefore(cuts, j, cur) && (prev < 0 || cutBefore(cuts, prev, j))) prev = j;

    const SequenceCameraCut& cut = cuts[cur];
    // A cut to no camera hands the view back to gameplay.
    cam.active = cut.binding != kSequenceNoBinding;
    cam.slot   = cut.binding;
    if (!cam.active) return cam;

    const float since = t - cut.time;
    if (cut.blendIn > 0.0f && since < cut.blendIn)
    {
        const uint16_t from = prev >= 0 ? cuts[prev].binding : kSequenceNoBinding;
        // Cutting to the camera that is already live is no change of view, and a
        // "blend" from a camera to itself would only restart its own motion.
        if (from != cut.binding)
        {
            cam.blending     = true;
            cam.fromSlot     = from;
            cam.fromGameplay = (from == kSequenceNoBinding);
            cam.alpha        = HE::applyBlendCurve(since / cut.blendIn, toBlendCurve(cut.curve));
        }
    }
    return cam;
}

} // namespace

HE::SequenceEval::Result HE::SequenceEval::evaluate(const SequenceAsset& seq, float t)
{
    Result r;
    const SequenceTrack* cutTrack = nullptr;

    for (const SequenceTrack& tr : seq.tracks)
    {
        switch (tr.kind)
        {
            case SequenceTrackKind::Property:
                // No binding: nobody to write to. No keys: sampleChannel would
                // answer 0, and writing 0 into a scale or an opacity is not "not
                // animated", it is "made invisible".
                if (tr.binding == kSequenceNoBinding || tr.channel.times.empty()) break;
                r.writes.push_back({ tr.binding, tr.channel.target,
                                     PropertyAnimationSystem::sampleChannel(tr.channel, t) });
                break;
            case SequenceTrackKind::CameraCut:
                if (!cutTrack) cutTrack = &tr;
                break;
            case SequenceTrackKind::Skeletal:
            case SequenceTrackKind::Event:
            case SequenceTrackKind::Audio:
                break;
        }
    }

    if (cutTrack) r.camera = cameraAt(cutTrack->cuts, t);
    return r;
}

std::vector<entt::entity> HE::SequenceEval::resolveBindings(const HorizonWorld& world,
                                                            const SequenceAsset& seq,
                                                            const std::vector<SlotOverride>& overrides)
{
    size_t size = 0;
    for (const SequenceBinding& b : seq.bindings) size = (std::max)(size, size_t(b.slot) + 1);
    for (const SlotOverride& o : overrides)
        if (o.slot != kSequenceNoBinding) size = (std::max)(size, size_t(o.slot) + 1);

    std::vector<entt::entity> slots(size, entt::entity{ entt::null });
    for (const SequenceBinding& b : seq.bindings)
    {
        // First binding of a slot wins; a duplicate is an authoring slip, and
        // letting the later one silently replace it would move the wrong actor.
        if (slots[b.slot] != entt::null || b.entityId == HE::UUID{}) continue;
        slots[b.slot] = world.findByEntityId(b.entityId);
    }
    for (const SlotOverride& o : overrides)
        if (o.slot != kSequenceNoBinding) slots[o.slot] = o.entity;
    return slots;
}

void HE::SequenceEval::apply(HorizonWorld& world, ContentManager& cm, const Result& r,
                             const std::vector<entt::entity>& slots)
{
    auto& reg = world.registry();
    for (const PropertyWrite& w : r.writes)
    {
        if (w.slot >= slots.size()) continue;
        const entt::entity e = slots[w.slot];
        if (e == entt::null || !reg.valid(e)) continue;
        PropertyAnimationSystem::applyChannel(world, cm, e, w.target, w.value);
    }
}
