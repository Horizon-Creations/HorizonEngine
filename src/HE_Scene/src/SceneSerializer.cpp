#include "HorizonScene/SceneSerializer.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/Components/NameComponent.h"
#include "HorizonScene/Components/HierarchyComponent.h"
#include <nlohmann/json.hpp>
#include <fstream>

using json = nlohmann::json;

bool SceneSerializer::save(const HorizonWorld& world,
                            const std::filesystem::path& path,
                            SerializeFormat format) {
    if (format == SerializeFormat::JSON)   return saveJSON(world, path);
    if (format == SerializeFormat::Binary) return saveBinary(world, path);
    return false;
}

bool SceneSerializer::load(HorizonWorld& world,
                            const std::filesystem::path& path,
                            SerializeFormat format) {
    if (format == SerializeFormat::JSON)   return loadJSON(world, path);
    if (format == SerializeFormat::Binary) return loadBinary(world, path);
    return false;
}

// ── JSON Save ─────────────────────────────────────────────────────────────────
bool SceneSerializer::saveJSON(const HorizonWorld& world, const std::filesystem::path& path)
{
    // We need mutable access to the registry for component views — cast is safe for serialisation
    HorizonWorld& mutableWorld = const_cast<HorizonWorld&>(world);
    auto& registry = mutableWorld.registry();

    json scene;
    scene["version"] = "1.0";

    // Serialise every entity that has a NameComponent (the root entity is included)
    json entities = json::array();
    auto view = registry.view<NameComponent>();
    for (auto entity : view)
    {
        json eJson;
        eJson["id"]   = static_cast<uint32_t>(entity);
        eJson["name"] = registry.get<NameComponent>(entity).name;

        if (auto* hier = registry.try_get<HierarchyComponent>(entity))
        {
            eJson["parent"] = static_cast<uint32_t>(hier->parent);
            json children = json::array();
            for (auto child : hier->children)
                children.push_back(static_cast<uint32_t>(child));
            eJson["children"] = children;
        }

        entities.push_back(eJson);
    }
    scene["entities"] = entities;

    std::ofstream out(path);
    if (!out.is_open()) return false;
    out << scene.dump(4);
    return true;
}

// ── JSON Load ─────────────────────────────────────────────────────────────────
bool SceneSerializer::loadJSON(HorizonWorld& world, const std::filesystem::path& path)
{
    std::ifstream in(path);
    if (!in.is_open()) return false;

    json scene = json::parse(in, nullptr, false);
    if (scene.is_discarded()) return false;

    if (!scene.contains("entities")) return true; // empty scene — valid

    // ── Pass 1: create all entities and set names ─────────────────────────────
    // We need to map the serialised uint32 IDs to newly created entt entities.
    std::unordered_map<uint32_t, Entity> idMap;

    auto& registry = world.registry();

    // The first entity is always the root — don't create a duplicate root.
    // Instead map the serialised root id to the existing rootEntity().
    bool rootMapped = false;
    for (auto& eJson : scene["entities"])
    {
        uint32_t    serialId = eJson.value("id",   0u);
        std::string name     = eJson.value("name", "Entity");

        if (!rootMapped)
        {
            // First entry — map to existing root
            idMap[serialId] = world.rootEntity();
            world.renameEntity(world.rootEntity(), name);
            rootMapped = true;
        }
        else
        {
            Entity e = world.createEntity(name);
            idMap[serialId] = e;
        }
    }

    // ── Pass 2: rebuild parent/child links ────────────────────────────────────
    for (auto& eJson : scene["entities"])
    {
        if (!eJson.contains("children")) continue;

        uint32_t serialId = eJson.value("id", 0u);
        auto it = idMap.find(serialId);
        if (it == idMap.end()) continue;

        Entity parent = it->second;
        auto*  pHier  = registry.try_get<HierarchyComponent>(parent);
        if (!pHier) continue;

        // Clear the children list rebuilt during createEntity — we'll restore exact order
        pHier->children.clear();

        for (auto& childId : eJson["children"])
        {
            uint32_t cid = childId.get<uint32_t>();
            auto cit = idMap.find(cid);
            if (cit == idMap.end()) continue;
            Entity child = cit->second;
            pHier->children.push_back(child);
            if (auto* cHier = registry.try_get<HierarchyComponent>(child))
                cHier->parent = parent;
        }
    }

    world.markHierarchyDirty();
    return true;
}

bool SceneSerializer::saveBinary(const HorizonWorld&, const std::filesystem::path&) { return false; }
bool SceneSerializer::loadBinary(HorizonWorld&,       const std::filesystem::path&) { return false; }
