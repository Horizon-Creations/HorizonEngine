#pragma once
#include <Types/Defines.h>
#include <UIWidget/UIElement.h>   // UIPropValue — a key holds one

#include <cstdint>
#include <string>
#include <vector>

// ── How a value gets from here to there ──────────────────────────────────────
// docs/he-apps-plan.md B8. An interface without transitions reads as cheap, and
// the reason is not decoration: a value that JUMPS makes the eye ask what
// happened, a value that moves answers it on the way.
//
// A closed vocabulary, for the same reasons the theme's roles are closed: it can
// be offered in a dropdown, checked by a test, and understood without
// documentation. Every curve here is one line of arithmetic on t ∈ [0,1]; a
// spline editor would be a second way to say the same thing and a second thing
// to keep working.
namespace HE
{

// ╔══════════════════════════════════════════════════════════════════════════╗
// ║  THESE NAMES ARE STORED. A graph node keeps the easing by NAME, so       ║
// ║  renaming one silently turns every animation that used it into Linear.   ║
// ║  test_ui_widgets.cpp pins the full list.                                 ║
// ╚══════════════════════════════════════════════════════════════════════════╝
enum class UIEase : uint8_t
{
    Linear = 0,
    // The three that carry almost everything: slow start, slow end, both.
    InQuad, OutQuad, InOutQuad,
    // The same shape, steeper — for movement over a longer distance, where a
    // quadratic still looks like it is being dragged.
    InCubic, OutCubic, InOutCubic,
    // Overshoots the target and settles back. What makes a dialog land instead
    // of arriving. Deliberately only on the way OUT: an entrance that starts by
    // going backwards looks like a mistake.
    OutBack,
    COUNT
};

HE_API const char* uiEaseName(UIEase e);
// Unknown name → Linear, never a failure: an animation with a misspelled curve
// should still play. Every other "name no longer resolves" case in the widget
// system leaves the value alone, but there is no value to leave alone here —
// the alternative to a curve is no animation at all.
HE_API UIEase uiEaseFromName(const std::string& s);

// The eased position for a linear one. `t` is clamped to [0,1] on the way in;
// what comes OUT may leave it (OutBack overshoots past 1 on purpose), so a
// caller that interpolates a bounded value has to clamp the result, not this.
HE_API float uiEaseApply(UIEase e, float t);

// ╔══════════════════════════════════════════════════════════════════════════╗
// ║  THESE NAMES ARE STORED too — a graph node keeps the direction by NAME.  ║
// ╚══════════════════════════════════════════════════════════════════════════╝
// Which way a clip runs. Closed vocabulary for the same reason UIEase is one:
// it can be offered as a dropdown and checked by a test.
//
// Backward is not "the same animation, mirrored" in the easing sense — the
// curves still sit on the keys where the author put them, and the clip is read
// from the far end towards zero. That is what makes "the way it came in, in
// reverse" one setting rather than a second clip somebody has to maintain.
enum class UIAnimDirection : uint8_t
{
    Forward = 0,
    Backward,
    // Out and back in one play: the panel that grows and settles, the button
    // that nudges. Twice as long as the clip, and with Loop it is the shape
    // every "breathing" animation has.
    PingPong,
    COUNT
};

HE_API const char* uiAnimDirectionName(UIAnimDirection d);
// Unknown name → Forward, never a failure. Same rule as uiEaseFromName: a
// misspelled setting should still play the animation.
HE_API UIAnimDirection uiAnimDirectionFromName(const std::string& s);

// ── Clips: an animation you author instead of one you write ──────────────────
// The other half of B8, and the half a designer needs: a NAMED animation that
// belongs to the widget, made of tracks (one property of one element) made of
// keys. "Play FadeIn" instead of six calls with six durations.
//
// The model is deliberately the small one every timeline has — time, value,
// curve — and not a curve editor: a key carries the easing INTO it, which is
// enough to say "slow at the end" without a second window full of tangents.

struct UIAnimKey
{
    float       time = 0.0f;   // seconds from the clip's start
    UIPropValue value;
    // The curve FROM the previous key INTO this one. On the first key it means
    // nothing and is ignored — there is nothing to come from.
    UIEase      ease = UIEase::Linear;
};

struct UIAnimTrack
{
    // The element this track drives, by id, LOCAL to the widget the clip
    // belongs to. An embedded component's clips keep their own numbering and
    // are offset when they play, exactly like its graph's element references
    // (see WidgetManager::resolveScriptOwner).
    int         element = 0;
    std::string prop;          // the property's name, as everywhere else
    // Sorted by time. Nothing enforces it on load, and evaluation does not
    // depend on it beyond "the last key at a time wins", so a hand-edited file
    // with two keys at 0.5 still evaluates the same way twice.
    std::vector<UIAnimKey> keys;
};

struct UIAnimClip
{
    std::string name;
    float       duration = 1.0f;   // seconds; what the timeline shows and loops on
    bool        loop = false;
    std::vector<UIAnimTrack> tracks;
};

// One property's value at one moment, as a clip reports it.
struct UIAnimSample
{
    int         element = 0;
    std::string prop;
    UIPropValue value;
};

// What the clip says at `time` — appended to `out`, one entry per track that
// has any keys at all.
//
// A READING, not a write. The runtime writes what it gets back; the designer
// overlays it while scrubbing without touching the document, which is the whole
// reason this is a free function and not a method on something that owns a tree.
//
// The boring cases, decided once because they are an on-disk format:
//   · before the first key → the first key's value (NOT the element's authored
//     one, or scrubbing to 0 and back would quietly lose it)
//   · after the last key   → the last key's value
//   · one key              → that value, always
//   · two keys at the same time → the later one in the list wins
HE_API void uiAnimEvaluate(const UIAnimClip& clip, float time,
                           std::vector<UIAnimSample>& out);

// How long the clip actually PLAYS: the last key on any of its tracks, capped by
// the authored length.
//
// Not the same thing as `duration`, and the difference is what somebody watching
// notices. Past the last key nothing moves any more — every track is holding a
// value that will not change again — so playing on to the authored end is a wait
// with nothing in it, and a clip that reports finished a second after it visibly
// finished makes every graph waiting on it late. The length stays what it is,
// the room you author in; this is the part of it that has anything to say.
//
// Capped by `duration` because that is what shortening a clip MEANS: keys past
// the end are hidden, not thrown away, and a hidden key must not be able to
// stretch playback past the length the author set.
//
// A clip with no keys at all ends at zero. Playing one is a no-op that reports
// finished at once — including a looping one, because there is nothing to loop.
HE_API float uiAnimPlayEnd(const UIAnimClip& clip);

// How long ONE pass takes in this direction: the clip's own length, or twice it
// for a ping-pong (out and back). What playback wraps and finishes on.
HE_API float uiAnimPlaySpan(UIAnimDirection dir, float playEnd);

// The moment of the CLIP to sample, `elapsed` seconds into a pass. Forward is
// the identity; backward reads from the far end towards zero; ping-pong goes out
// and comes back. Clamped into [0, playEnd], so a caller that has just wrapped
// its elapsed time cannot ask for a moment outside the clip.
HE_API float uiAnimDirectedTime(UIAnimDirection dir, float elapsed, float playEnd);

// The clip of that name, or null. Names are what a graph node and the editor
// both store, so they are the identity — there is no clip id.
HE_API const UIAnimClip* uiAnimFind(const std::vector<UIAnimClip>& clips,
                                    const std::string& name);

} // namespace HE
