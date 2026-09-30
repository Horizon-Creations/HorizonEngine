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

const UIAnimClip* uiAnimFind(const std::vector<UIAnimClip>& clips, const std::string& name)
{
    for (const UIAnimClip& c : clips) if (c.name == name) return &c;
    return nullptr;
}

namespace
{
    constexpr int kDirCount = static_cast<int>(UIAnimDirection::COUNT);
    // Index-matched to the enum, like kNames above.
    const char* const kDirNames[kDirCount] = { "Forward", "Backward", "Ping Pong" };
}

const char* uiAnimDirectionName(UIAnimDirection d)
{
    const int i = static_cast<int>(d);
    return (i >= 0 && i < kDirCount) ? kDirNames[i] : kDirNames[0];
}

UIAnimDirection uiAnimDirectionFromName(const std::string& s)
{
    for (int i = 0; i < kDirCount; ++i)
        if (s == kDirNames[i]) return static_cast<UIAnimDirection>(i);
    return UIAnimDirection::Forward;
}

float uiAnimPlaySpan(UIAnimDirection dir, float playEnd)
{
    const float end = std::max(playEnd, 0.0f);
    return dir == UIAnimDirection::PingPong ? end * 2.0f : end;
}

float uiAnimDirectedTime(UIAnimDirection dir, float elapsed, float playEnd)
{
    const float end = std::max(playEnd, 0.0f);
    const float t   = std::clamp(elapsed, 0.0f, uiAnimPlaySpan(dir, end));
    switch (dir)
    {
    case UIAnimDirection::Backward: return end - t;
    case UIAnimDirection::PingPong: return t <= end ? t : end * 2.0f - t;
    case UIAnimDirection::Forward:
    case UIAnimDirection::COUNT:
    default:                        return t;
    }
}

float uiAnimPlayEnd(const UIAnimClip& clip)
{
    float last = 0.0f;
    for (const UIAnimTrack& tr : clip.tracks)
        for (const UIAnimKey& k : tr.keys)
            last = std::max(last, k.time);
    // Negative key times are possible in a hand-edited file; they hold from
    // before the start anyway (uiAnimEvaluate) and cannot lengthen anything,
    // which is why the running maximum starts at zero rather than at the first
    // key it sees.
    return std::min(last, std::max(clip.duration, 0.0f));
}

bool uiAnimStretchToLength(UIAnimClip& clip)
{
    const float end = uiAnimPlayEnd(clip);
    // Half a millisecond is the same "already there" the timeline uses to decide
    // whether to grey a tail out; below it there is nothing a person could see.
    if (end <= 0.0f || end >= clip.duration - 0.0005f) return false;
    const float f = clip.duration / end;
    for (UIAnimTrack& tr : clip.tracks)
        for (UIAnimKey& k : tr.keys)
            // The last key(s) set, not multiplied: end * (duration / end) can
            // come out a hair short in float, and a key a hair short of the end
            // is the very thing this is here to repair.
            k.time = k.time >= end ? clip.duration : k.time * f;
    return true;
}

namespace
{
    // The value between two keys. Float, Vec2 and Color are interpolated;
    // everything else (the Visible/Enabled switches, and an Int or String out
    // of a hand-edited file) HOLDS the earlier key until the next one is
    // reached. Returning the later key instead would switch a Visible track off
    // the instant its "on" key is passed — keys 0 = on, 0.5 = off would be off
    // from 0, because `a` is found with "<=".
    UIPropValue between(const UIAnimKey& a, const UIAnimKey& b, float k)
    {
        // Two types on one track (hand-edited file): nothing to blend, so the
        // same rule as any other value that cannot be blended — hold.
        if (a.value.type != b.value.type) return a.value;
        switch (a.value.type)
        {
        case UIPropType::Float:
            return UIPropValue::ofFloat(a.value.f + (b.value.f - a.value.f) * k);
        case UIPropType::Vec2:
            return UIPropValue::ofVec2(a.value.v2 + (b.value.v2 - a.value.v2) * k);
        case UIPropType::Color:
        {
            glm::vec4 c = a.value.col + (b.value.col - a.value.col) * k;
            // Out Back leaves [0,1] on purpose. That is a position overshooting
            // its target, not a red channel overshooting red.
            c = glm::clamp(c, glm::vec4(0.0f), glm::vec4(1.0f));
            return UIPropValue::ofColor(c);
        }
        default:
            return a.value;
        }
    }
}

bool uiAnimTypeInterpolates(UIPropType t)
{
    return t == UIPropType::Float || t == UIPropType::Vec2 || t == UIPropType::Color;
}

UIAnimTrackOffer uiAnimTrackOffer(const UIElement& e, const UIPropDesc& pd)
{
    if (uiAnimTypeInterpolates(pd.type)) return UIAnimTrackOffer::Offer;
    if (pd.type != UIPropType::Bool) return UIAnimTrackOffer::No;
    // The switches a clip may throw. A table so the next one ("Hit Testable")
    // is a line, not a second branch. Every other Bool stays out: Checked,
    // Password, Word Wrap… would all work, and would bury these two in a list
    // nobody animates.
    if (pd.name == "Visible") return UIAnimTrackOffer::Offer;
    if (pd.name == "Enabled")
    {
        // Enabled reaches the whole subtree (uiElementEffectiveEnabled), so a
        // container is as much a place for it as a button: "lock the menu while
        // it slides in". Only an element that neither takes input nor holds
        // anything that could has nothing for it to switch off.
        return (e.interactive() || e.acceptsChildren()) ? UIAnimTrackOffer::Offer
                                                        : UIAnimTrackOffer::NoEffect;
    }
    return UIAnimTrackOffer::No;
}

void uiAnimEvaluate(const UIAnimClip& clip, float time, std::vector<UIAnimSample>& out)
{
    for (const UIAnimTrack& tr : clip.tracks)
    {
        if (tr.keys.empty()) continue;
        UIAnimSample s;
        s.element = tr.element;
        s.prop    = tr.prop;

        // The last key at or before `time`, and the first one after it. Walked
        // rather than searched: a track has keys, not samples, and a linear walk
        // over a handful of them is cheaper than being clever about it.
        const UIAnimKey* prev = nullptr;
        const UIAnimKey* next = nullptr;
        for (const UIAnimKey& k : tr.keys)
        {
            // "<=" so that two keys at the same time resolve to the LATER one in
            // the list — deterministic, which is all a hand-edited file needs.
            if (k.time <= time) prev = &k;
            else if (!next)     next = &k;
        }
        if (!prev)       s.value = tr.keys.front().value;   // before the first
        else if (!next)  s.value = prev->value;             // after the last
        else
        {
            const float span = next->time - prev->time;
            // Two keys at one time: no span to interpolate over, so the later
            // one simply IS the value.
            const float k = span > 0.0f ? (time - prev->time) / span : 1.0f;
            // The easing belongs to the key being moved TO — "slow at the end"
            // is a property of the arrival, not of the departure.
            s.value = between(*prev, *next, uiEaseApply(next->ease, k));
        }
        out.push_back(std::move(s));
    }
}

} // namespace HE
