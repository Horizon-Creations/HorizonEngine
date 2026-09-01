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

// The clip of that name, or null. Names are what a graph node and the editor
// both store, so they are the identity — there is no clip id.
HE_API const UIAnimClip* uiAnimFind(const std::vector<UIAnimClip>& clips,
                                    const std::string& name);

} // namespace HE
