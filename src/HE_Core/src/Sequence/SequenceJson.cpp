#include "Sequence/SequenceJson.h"

#include <ContentManager/Assets.h>
#include <GraphCommon/GraphJson.h>
#include <nlohmann/json.hpp>

#include <algorithm>

namespace HE
{
namespace
{

using json = nlohmann::json;

// Kinds are spelled out rather than numbered: the document is also what an MCP
// client reads and writes, and "cameraCut" says what a track is where "2" does not.
const char* kindName(SequenceTrackKind k)
{
    switch (k)
    {
        case SequenceTrackKind::Property:  return "property";
        case SequenceTrackKind::Skeletal:  return "skeletal";
        case SequenceTrackKind::CameraCut: return "cameraCut";
        case SequenceTrackKind::Event:     return "event";
        case SequenceTrackKind::Audio:     return "audio";
    }
    return "";
}

bool kindFromName(const std::string& s, SequenceTrackKind& out)
{
    for (int i = 0; i <= static_cast<int>(SequenceTrackKind::Audio); ++i)
    {
        const auto k = static_cast<SequenceTrackKind>(i);
        if (s == kindName(k)) { out = k; return true; }
    }
    return false;
}

// A binding reference is written only when there is one. Absent, or anything
// that is not a slot number, reads as "no binding" — never as slot 0, which is a
// real actor.
void putBinding(json& j, uint16_t slot)
{
    if (slot != kSequenceNoBinding) j["binding"] = slot;
}

uint16_t getBinding(const json& j)
{
    const auto it = j.find("binding");
    if (it == j.end() || !it->is_number_integer()) return kSequenceNoBinding;
    const int64_t v = it->get<int64_t>();
    if (v < 0 || v >= kSequenceNoBinding) return kSequenceNoBinding;
    return static_cast<uint16_t>(v);
}

// Guarded like blendSpaceKindFromInt: a value from a newer build must not become
// an enum with no enumerator.
SequenceBlendCurve curveFromInt(int v)
{
    switch (v)
    {
        case (int)SequenceBlendCurve::Linear:     return SequenceBlendCurve::Linear;
        case (int)SequenceBlendCurve::SmoothStep: return SequenceBlendCurve::SmoothStep;
        case (int)SequenceBlendCurve::EaseOut:    return SequenceBlendCurve::EaseOut;
        default:                                  return SequenceBlendCurve::SmoothStep;
    }
}

std::vector<float> floats(const json& j, const char* key)
{
    std::vector<float> out;
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array()) return out;
    out.reserve(it->size());
    for (const auto& v : *it)
    {
        // A non-number in the middle would shift every later key onto the wrong
        // time; returning the short list lets the length check below drop the
        // whole channel instead.
        if (!v.is_number()) break;
        out.push_back(v.get<float>());
    }
    return out;
}

} // namespace

std::string sequenceToJson(const SequenceAsset& s)
{
    json j;
    j["version"]   = 1;
    j["duration"]  = s.duration;
    j["frameRate"] = s.frameRate;

    json bindings = json::array();
    for (const SequenceBinding& b : s.bindings)
        bindings.push_back({ { "slot", b.slot }, { "name", b.name },
                             { "entityId", HE::graph::uuidToJson(b.entityId) } });
    j["bindings"] = std::move(bindings);

    json tracks = json::array();
    for (const SequenceTrack& t : s.tracks)
    {
        json tj;
        tj["kind"] = kindName(t.kind);
        putBinding(tj, t.binding);
        switch (t.kind)
        {
            case SequenceTrackKind::Property:
                tj["target"] = static_cast<int>(t.channel.target);
                tj["times"]  = t.channel.times;
                tj["values"] = t.channel.values;
                break;
            case SequenceTrackKind::Skeletal:
            {
                json arr = json::array();
                for (const SequenceSkeletalSection& sec : t.sections)
                    arr.push_back({ { "clipId", HE::graph::uuidToJson(sec.clipId) },
                                    { "start", sec.start }, { "end", sec.end },
                                    { "clipOffset", sec.clipOffset },
                                    { "playRate", sec.playRate }, { "loop", sec.loop } });
                tj["sections"] = std::move(arr);
                break;
            }
            case SequenceTrackKind::CameraCut:
            {
                json arr = json::array();
                for (const SequenceCameraCut& c : t.cuts)
                {
                    json cj = { { "time", c.time }, { "blendIn", c.blendIn },
                                { "curve", static_cast<int>(c.curve) } };
                    putBinding(cj, c.binding);
                    arr.push_back(std::move(cj));
                }
                tj["cuts"] = std::move(arr);
                break;
            }
            case SequenceTrackKind::Event:
            {
                json arr = json::array();
                for (const AnimationNotify& e : t.events)
                    arr.push_back({ { "name", e.name }, { "time", e.time },
                                    { "duration", e.duration } });
                tj["events"] = std::move(arr);
                break;
            }
            case SequenceTrackKind::Audio:
            {
                json arr = json::array();
                for (const SequenceAudioSection& a : t.audio)
                    arr.push_back({ { "assetId", HE::graph::uuidToJson(a.assetId) },
                                    { "start", a.start }, { "volume", a.volume },
                                    { "pitch", a.pitch } });
                tj["sections"] = std::move(arr);
                break;
            }
        }
        tracks.push_back(std::move(tj));
    }
    j["tracks"] = std::move(tracks);

    return j.dump(2);
}

namespace
{

bool parseSequence(const json& j, SequenceAsset& out, int* dropped)
{
    int lost = 0;
    const json kEmpty = json::array();

    float duration  = j.value("duration", 0.0f);
    float frameRate = j.value("frameRate", 30.0f);
    if (!(duration >= 0.0f))  duration  = 0.0f;   // also catches NaN
    if (!(frameRate > 0.0f))  frameRate = 30.0f;

    std::vector<SequenceBinding> bindings;
    const json& bj = j.contains("bindings") && j["bindings"].is_array() ? j["bindings"] : kEmpty;
    for (const auto& b : bj)
    {
        if (!b.is_object()) { ++lost; continue; }
        const json* slot = b.contains("slot") ? &b["slot"] : nullptr;
        if (!slot || !slot->is_number_integer() || slot->get<int64_t>() < 0 ||
            slot->get<int64_t>() >= kSequenceNoBinding) { ++lost; continue; }
        SequenceBinding sb;
        sb.slot = static_cast<uint16_t>(slot->get<int64_t>());
        sb.name = b.value("name", std::string());
        if (auto it = b.find("entityId"); it != b.end()) sb.entityId = HE::graph::uuidFromJson(*it);
        bindings.push_back(std::move(sb));
    }

    std::vector<SequenceTrack> tracks;
    const json& tj = j.contains("tracks") && j["tracks"].is_array() ? j["tracks"] : kEmpty;
    for (const auto& t : tj)
    {
        SequenceTrack st;
        if (!t.is_object() || !kindFromName(t.value("kind", std::string()), st.kind))
        {
            // A kind from a newer build: dropped, not guessed. Keeping it as some
            // other kind would evaluate its payload by the wrong rule.
            ++lost;
            continue;
        }
        st.binding = getBinding(t);
        switch (st.kind)
        {
            case SequenceTrackKind::Property:
            {
                const int target = t.value("target", -1);
                if (target < 0 || target > static_cast<int>(kLastPropTarget)) { ++lost; continue; }
                st.channel.target = static_cast<PropTarget>(target);
                st.channel.times  = floats(t, "times");
                st.channel.values = floats(t, "values");
                if (st.channel.times.size() != st.channel.values.size()) { ++lost; continue; }
                break;
            }
            case SequenceTrackKind::Skeletal:
                for (const auto& s : t.value("sections", kEmpty))
                {
                    if (!s.is_object()) { ++lost; continue; }
                    SequenceSkeletalSection sec;
                    if (auto it = s.find("clipId"); it != s.end()) sec.clipId = HE::graph::uuidFromJson(*it);
                    sec.start      = s.value("start", 0.0f);
                    sec.end        = s.value("end", 0.0f);
                    sec.clipOffset = s.value("clipOffset", 0.0f);
                    sec.playRate   = s.value("playRate", 1.0f);
                    sec.loop       = s.value("loop", false);
                    st.sections.push_back(sec);
                }
                break;
            case SequenceTrackKind::CameraCut:
                for (const auto& c : t.value("cuts", kEmpty))
                {
                    if (!c.is_object()) { ++lost; continue; }
                    SequenceCameraCut cut;
                    cut.time    = c.value("time", 0.0f);
                    cut.binding = getBinding(c);
                    cut.blendIn = c.value("blendIn", 0.0f);
                    if (!(cut.blendIn >= 0.0f)) cut.blendIn = 0.0f;
                    cut.curve   = curveFromInt(c.value("curve", static_cast<int>(SequenceBlendCurve::SmoothStep)));
                    st.cuts.push_back(cut);
                }
                // Kept in time order, so the list a reader sees is the order the
                // cuts happen in. Stable: two cuts at one instant keep the order
                // they were written in, and the later one wins (SequenceEval).
                std::stable_sort(st.cuts.begin(), st.cuts.end(),
                                 [](const SequenceCameraCut& a, const SequenceCameraCut& b)
                                 { return a.time < b.time; });
                break;
            case SequenceTrackKind::Event:
                for (const auto& e : t.value("events", kEmpty))
                {
                    if (!e.is_object()) { ++lost; continue; }
                    AnimationNotify n;
                    n.name     = e.value("name", std::string());
                    n.time     = e.value("time", 0.0f);
                    n.duration = e.value("duration", 0.0f);
                    st.events.push_back(std::move(n));
                }
                break;
            case SequenceTrackKind::Audio:
                for (const auto& a : t.value("sections", kEmpty))
                {
                    if (!a.is_object()) { ++lost; continue; }
                    SequenceAudioSection sec;
                    if (auto it = a.find("assetId"); it != a.end()) sec.assetId = HE::graph::uuidFromJson(*it);
                    sec.start  = a.value("start", 0.0f);
                    sec.volume = a.value("volume", 1.0f);
                    sec.pitch  = a.value("pitch", 1.0f);
                    st.audio.push_back(sec);
                }
                break;
        }
        tracks.push_back(std::move(st));
    }

    out.duration  = duration;
    out.frameRate = frameRate;
    out.bindings  = std::move(bindings);
    out.tracks    = std::move(tracks);
    if (dropped) *dropped = lost;
    return true;
}

} // namespace

bool sequenceFromJson(const std::string& text, SequenceAsset& out, int* dropped)
{
    json j;
    if (!HE::graph::parseGraphObject(text, j)) return false;
    // json::value() throws when a key holds the wrong type ("duration": "2"). A
    // hand-edited file is not a reason to take the loader down with it; the
    // document is refused whole and `out` stays as it was.
    try
    {
        return parseSequence(j, out, dropped);
    }
    catch (const json::exception&)
    {
        return false;
    }
}

void sequenceAssetRefs(const SequenceAsset& s, std::vector<HE::UUID>& out)
{
    auto add = [&out](const HE::UUID& id)
    {
        if (id == HE::UUID{}) return;
        if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
    };
    for (const SequenceTrack& t : s.tracks)
    {
        for (const SequenceSkeletalSection& sec : t.sections) add(sec.clipId);
        for (const SequenceAudioSection& a : t.audio)         add(a.assetId);
    }
}

} // namespace HE
