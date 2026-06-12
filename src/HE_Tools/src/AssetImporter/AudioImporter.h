#pragma once
#include <filesystem>
#include <memory>

// Imports raw PCM / WAV / OGG into an AudioAsset.
class AudioImporter {
public:
    struct ImportSettings {
        uint32_t targetSampleRate = 48000;
        uint16_t targetChannels   = 2;
        bool     mono             = false;
    };

};
