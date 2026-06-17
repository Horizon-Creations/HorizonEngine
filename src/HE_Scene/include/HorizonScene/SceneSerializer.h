#pragma once
#include <Types/Enums.h>
#include <cstdint>
#include <string>
#include <filesystem>
#include <vector>
#include <entt/entt.hpp>

class HorizonWorld;
using SerializeFormat = HE::SerializeFormat;
using Entity = entt::entity;

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

    // Prefab serialization: capture an entity subtree (root + all descendants)
    // as a self-contained CBOR blob. Entities are remapped to contiguous IDs so
    // the prefab is independent of the source world's handle space.
    std::vector<uint8_t> serializeSubtree(const HorizonWorld& world, Entity root);

    // Instantiate a prefab blob into the world. Creates fresh entities for
    // every entry in the prefab and re-wires their hierarchy. The new subtree
    // root is reparented to `parent` (world root if entt::null). Returns the
    // new root entity, or entt::null on parse failure.
    Entity instantiatePrefab(HorizonWorld& world,
                             const std::vector<uint8_t>& data,
                             Entity parent = entt::null);

private:
    bool saveJSON  (const HorizonWorld& world, const std::filesystem::path& path);
    bool saveBinary(const HorizonWorld& world, const std::filesystem::path& path);
    bool loadJSON  (HorizonWorld& world, const std::filesystem::path& path);
    bool loadBinary(HorizonWorld& world, const std::filesystem::path& path);
};
