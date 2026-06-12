#pragma once
#include <filesystem>
#include <vector>
#include <cstdint>

// Reads all .hasset files from an AssetRegistry,
// encrypts their data blocks with AES-256-CBC,
// and writes a single .hpak archive.
class HpakWriter {
public:
    struct PackSettings {
        uint8_t  key[32];                    // AES-256 key (32 bytes)
        bool     compressBeforeEncrypt = true; // LZ4 compression before AES
    };


};
