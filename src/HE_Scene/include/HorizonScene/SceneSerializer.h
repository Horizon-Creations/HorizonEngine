#pragma once
#include <Types/Enums.h>
#include <cstdint>
#include <string>
#include <filesystem>
#include <vector>

class HorizonWorld;
using SerializeFormat = HE::SerializeFormat;

class SceneSerializer {
public:
    bool save(const HorizonWorld& world,
              const std::filesystem::path& path,
              SerializeFormat format);

    bool load(HorizonWorld& world,
              const std::filesystem::path& path,
              SerializeFormat format);

    // In-memory snapshot (CBOR, same structure as the binary file format).
    // Used by play-in-editor and the undo system. load does not clear the
    // world first — call HorizonWorld::clear() when replacing the content.
    bool saveToMemory(const HorizonWorld& world, std::vector<uint8_t>& out);
    bool loadFromMemory(HorizonWorld& world, const std::vector<uint8_t>& data);

private:
    bool saveJSON  (const HorizonWorld& world, const std::filesystem::path& path);
    bool saveBinary(const HorizonWorld& world, const std::filesystem::path& path);
    bool loadJSON  (HorizonWorld& world, const std::filesystem::path& path);
    bool loadBinary(HorizonWorld& world, const std::filesystem::path& path);
};
