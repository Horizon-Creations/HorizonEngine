#pragma once
#include <Types/Enums.h>
#include <string>
#include <filesystem>

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

private:
    bool saveJSON  (const HorizonWorld& world, const std::filesystem::path& path);
    bool saveBinary(const HorizonWorld& world, const std::filesystem::path& path);
    bool loadJSON  (HorizonWorld& world, const std::filesystem::path& path);
    bool loadBinary(HorizonWorld& world, const std::filesystem::path& path);
};
