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

    // Additive load: merges the entities from a .hescene file into the existing
    // world without clearing it first. The loaded scene's root entity becomes
    // a new child of the current world root, preserving all existing entities.
    // outCreated (optional) receives every entity the merge created — the
    // runtime's zone system uses this to unload an additively-loaded scene again.
    bool loadAdditive(HorizonWorld& world,
                      const std::filesystem::path& path,
                      SerializeFormat format,
                      std::vector<Entity>* outCreated = nullptr);

    // Additive merge from an in-memory CBOR snapshot (a packed scene entry).
    bool loadAdditiveFromMemory(HorizonWorld& world,
                                const std::vector<uint8_t>& data,
                                std::vector<Entity>* outCreated = nullptr);

    // In-memory snapshot (CBOR, same structure as the binary file format).
    // Used by play-in-editor and the undo system. load does not clear the
    // world first — call HorizonWorld::clear() when replacing the content.
    bool saveToMemory(const HorizonWorld& world, std::vector<uint8_t>& out);
    bool loadFromMemory(HorizonWorld& world, const std::vector<uint8_t>& data);

    // Prefab serialization: capture an entity subtree (root + all descendants)
    // as a self-contained CBOR blob. Entities are remapped to contiguous IDs so
    // the prefab is independent of the source world's handle space.
    std::vector<uint8_t> serializeSubtree(const HorizonWorld& world, Entity root);

    // ── Single-entity component state (CBOR) ─────────────────────────────────
    // Capture or restore just the components of ONE existing entity, without
    // creating or destroying anything. This is what live collaboration
    // replicates for non-transform edits: a prefab blob would mint a new entity,
    // which is the wrong operation when the entity already exists on the peer
    // and is merely being edited.
    //
    // applyEntityComponents overwrites the components present in `data` and
    // leaves the rest alone, so it is an update rather than a replacement.
    std::vector<uint8_t> serializeEntityComponents(const HorizonWorld& world, Entity entity);
    bool applyEntityComponents(HorizonWorld& world, Entity entity,
                               const std::vector<uint8_t>& data);

    // Instantiate a prefab blob into the world. Creates fresh entities for
    // every entry in the prefab and re-wires their hierarchy. The new subtree
    // root is reparented to `parent` (world root if entt::null). Returns the
    // new root entity, or entt::null on parse failure.
    //
    // preserveIds keeps the entity UUIDs stored in the blob instead of minting
    // fresh ones. Only for collaboration's structural replication, where both
    // peers must know the new subtree under the SAME identities; a prefab drop
    // must keep the default, or two drops would claim one identity twice.
    Entity instantiatePrefab(HorizonWorld& world,
                             const std::vector<uint8_t>& data,
                             Entity parent = entt::null,
                             bool preserveIds = false);

private:
    bool saveJSON  (const HorizonWorld& world, const std::filesystem::path& path);
    bool saveBinary(const HorizonWorld& world, const std::filesystem::path& path);
    bool loadJSON  (HorizonWorld& world, const std::filesystem::path& path);
    bool loadBinary(HorizonWorld& world, const std::filesystem::path& path);
};
