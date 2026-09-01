#pragma once
#include <Types/Defines.h>

#include <cstdint>
#include <string>

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

} // namespace HE
