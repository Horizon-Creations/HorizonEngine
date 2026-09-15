#pragma once
#include <Types/Defines.h>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

namespace HE {

// ─── Audio buses ──────────────────────────────────────────────────────────────
// A bus is a NAMED MIXER GROUP, project wide: "Music", "SFX", "Voice". An audio
// source names the bus it plays through (AudioSourceComponent::busName) and a
// script turns a whole group down at once (audio.setBusVolume). Until this
// existed a bus came into being only when a script first mentioned it, so a
// source whose Bus field said "Music" played on the master bus with a warning
// in the log — the field was a promise nothing kept.
//
// WHY THIS LIVES IN HE_Core AND NOT NEXT TO AudioEngine: it is a PROJECT
// setting, so it travels ProjectData (.heproj, HE_Tools) → ExportSettings →
// ProjectConfig (project.hcfg) → runtime, exactly like CollisionLayerConfig.
// HorizonScene does not link HE_Tools, so AudioEngine may never see
// ProjectData; HE_Core is the one place both ends of that road can look at.
//
// What is deliberately NOT here: mute and solo. Those are what the mixer
// window does while you listen, and a shipped game with a bus somebody muted
// during a test session is a bug nobody would find. They live in the editor.
struct HE_API AudioBusDef
{
    std::string name;
    float       volume = 1.0f;   // linear gain, 1 = unity; the engine clamps at 0
};

struct HE_API AudioBusConfig
{
    // In authoring order — the order the mixer shows them in, and the order the
    // engine creates them in. Names are unique (add() refuses a duplicate).
    std::vector<AudioBusDef> buses;

    // The gain of everything at once, in front of every bus. 1 is what every
    // project written before this existed was getting.
    float masterVolume = 1.0f;

    // A default-constructed config is EMPTY on purpose: a project written
    // before buses existed must load as "no buses", and a source that names one
    // keeps falling back to master exactly as it did. The mixer window is where
    // the usual three (Music, SFX, Voice) are one click away.

    const AudioBusDef* find(const std::string& name) const;
    AudioBusDef*       find(const std::string& name);

    // False for an empty name or one that already exists — the mixer reports
    // that rather than creating a second "Music" that half the sources hit.
    bool add(const std::string& name, float volume = 1.0f);
    bool remove(const std::string& name);

    // True when nothing was ever authored — no buses, master at 1. The writers
    // use it to leave the block out of a .heproj/.hcfg entirely, so a project
    // that never opened the mixer does not grow a key that says nothing.
    bool isDefault() const;

    // The same JSON shape on both sides of the road (.heproj and .hcfg):
    // { "master": 1.0, "buses": [ { "name": "Music", "volume": 0.8 }, … ] }.
    void toJson(nlohmann::json& out) const;
    void fromJson(const nlohmann::json& in);
};

} // namespace HE
