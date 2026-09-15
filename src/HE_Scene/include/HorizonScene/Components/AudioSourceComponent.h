#pragma once
#include <Types/UUID.h>
#include <HorizonScene/AudioAttenuation.h>
#include <cstdint>
#include <string>

struct AudioSourceComponent {
    HE::UUID    assetId;
    std::string busName;            // target bus name; "" = master
    float       volume        = 1.0f;
    float       pitch         = 1.0f;
    float       range         = 20.0f; // max audible distance (m)
    float       rolloffFactor = 1.0f;  // attenuation speed — see AudioAttenuation.h
    float       innerRange    = 1.0f;  // min distance — full volume within this radius
    // Which curve the volume follows between innerRange and range. See
    // attenuationGain() for what each one does; the Details panel draws it.
    AudioAttenuation attenuation = AudioAttenuation::Linear;
    bool        loop          = false;
    bool        playOnStart   = false;
    bool        spatial       = false; // enable 3D position-based attenuation

    // Runtime only — not serialized
    uint64_t    handle        = 0;
};
