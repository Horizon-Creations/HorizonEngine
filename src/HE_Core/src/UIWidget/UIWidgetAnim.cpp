#include <UIWidget/UIWidgetAnim.h>

#include <algorithm>
#include <cstring>

namespace HE
{
namespace
{
    constexpr int kCount = static_cast<int>(UIEase::COUNT);
    // Index-matched to the enum. One array, so a curve added in the middle
    // cannot end up with another one's name.
    const char* const kNames[kCount] = {
        "Linear",
        "In Quad", "Out Quad", "In Out Quad",
        "In Cubic", "Out Cubic", "In Out Cubic",
        "Out Back",
    };
}

const char* uiEaseName(UIEase e)
{
    const int i = static_cast<int>(e);
    return (i >= 0 && i < kCount) ? kNames[i] : "Linear";
}

UIEase uiEaseFromName(const std::string& s)
{
    for (int i = 0; i < kCount; ++i)
        if (s == kNames[i]) return static_cast<UIEase>(i);
    return UIEase::Linear;
}

float uiEaseApply(UIEase e, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    switch (e)
    {
    case UIEase::InQuad:     return t * t;
    case UIEase::OutQuad:    return 1.0f - (1.0f - t) * (1.0f - t);
    case UIEase::InOutQuad:  return t < 0.5f ? 2.0f * t * t
                                             : 1.0f - 0.5f * (2.0f - 2.0f * t) * (2.0f - 2.0f * t);
    case UIEase::InCubic:    return t * t * t;
    case UIEase::OutCubic:   { const float u = 1.0f - t; return 1.0f - u * u * u; }
    case UIEase::InOutCubic: return t < 0.5f
                                    ? 4.0f * t * t * t
                                    : 1.0f - 0.5f * (2.0f - 2.0f * t) * (2.0f - 2.0f * t)
                                                  * (2.0f - 2.0f * t);
    case UIEase::OutBack:
    {
        // The standard overshoot constants (CSS's ease-out-back, Penner's
        // original): ~10% past the target before it settles. Written out rather
        // than named, because two magic numbers with a comment beat two
        // constants whose names have to be looked up.
        constexpr float c1 = 1.70158f;
        constexpr float c3 = c1 + 1.0f;
        const float u = t - 1.0f;
        return 1.0f + c3 * u * u * u + c1 * u * u;
    }
    case UIEase::Linear:
    case UIEase::COUNT:
    default:                 return t;
    }
}

} // namespace HE
