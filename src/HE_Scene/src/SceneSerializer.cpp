#include "HorizonScene/SceneSerializer.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/Components/NameComponent.h"
#include "HorizonScene/Components/HierarchyComponent.h"
#include "HorizonScene/Components/TransformComponent.h"
#include "HorizonScene/Components/Transform2DComponent.h"
#include "HorizonScene/Components/MeshComponent.h"
#include "HorizonScene/Components/MaterialComponent.h"
#include "HorizonScene/Components/CameraComponent.h"
#include "HorizonScene/Components/LightComponent.h"
#include "HorizonScene/Components/RigidBodyComponent.h"
#include "HorizonScene/Components/ScriptComponent.h"
#include "HorizonScene/Components/EnvironmentComponent.h"
#include "HorizonScene/Components/EnvironmentLightComponent.h"
#include <nlohmann/json.hpp>
#include <fstream>

using json = nlohmann::json;

// ── (De)serialisation helpers ────────────────────────────────────────────────
namespace
{
	json vec3ToJson(const glm::vec3& v)  { return json::array({ v.x, v.y, v.z }); }
	json vec2ToJson(const glm::vec2& v)  { return json::array({ v.x, v.y }); }

	glm::vec3 jsonToVec3(const json& j, const glm::vec3& fallback)
	{
		if (!j.is_array() || j.size() != 3) return fallback;
		return { j[0].get<float>(), j[1].get<float>(), j[2].get<float>() };
	}
	glm::vec2 jsonToVec2(const json& j, const glm::vec2& fallback)
	{
		if (!j.is_array() || j.size() != 2) return fallback;
		return { j[0].get<float>(), j[1].get<float>() };
	}

	json uuidToJson(const HE::UUID& id)  { return json::array({ id.hi, id.lo }); }

	HE::UUID jsonToUuid(const json& j)
	{
		if (!j.is_array() || j.size() != 2) return {};
		HE::UUID id;
		id.hi = j[0].get<uint64_t>();
		id.lo = j[1].get<uint64_t>();
		return id;
	}

	// ── Scene → JSON ─────────────────────────────────────────────────────────
	json buildSceneJson(HorizonWorld& world)
	{
		auto& registry = world.registry();

		json scene;
		scene["version"] = "1.1";

		json entities = json::array();
		auto view = registry.view<NameComponent>();
		for (auto entity : view)
		{
			// The built-in environment sun/moon lights are never serialised — they
			// are recreated on load (ensureEnvironmentLights), so the scene file
			// stays clean and they can never be duplicated or orphaned.
			if (registry.all_of<EnvironmentLightComponent>(entity))
				continue;

			json eJson;
			eJson["id"]   = static_cast<uint32_t>(entity);
			eJson["name"] = registry.get<NameComponent>(entity).name;

			if (auto* hier = registry.try_get<HierarchyComponent>(entity))
			{
				eJson["parent"] = static_cast<uint32_t>(hier->parent);
				json children = json::array();
				for (auto child : hier->children)
					if (!registry.all_of<EnvironmentLightComponent>(child)) // omit built-ins
						children.push_back(static_cast<uint32_t>(child));
				eJson["children"] = children;
			}

			json comps;
			if (auto* t = registry.try_get<TransformComponent>(entity))
			{
				comps["transform"] = {
					{ "position", vec3ToJson(t->position) },
					{ "rotation", vec3ToJson(t->rotation) },
					{ "scale",    vec3ToJson(t->scale) },
				};
			}
			if (auto* t = registry.try_get<Transform2DComponent>(entity))
			{
				comps["transform2d"] = {
					{ "position", vec2ToJson(t->position) },
					{ "rotation", t->rotation },
					{ "scale",    vec2ToJson(t->scale) },
				};
			}
			if (auto* m = registry.try_get<MeshComponent>(entity))
			{
				comps["mesh"] = {
					{ "asset",          uuidToJson(m->meshAssetId) },
					{ "lodBias",        m->lodBias },
					{ "castsShadow",    m->castsShadow },
					{ "receivesShadow", m->receivesShadow },
				};
			}
			if (auto* m = registry.try_get<MaterialComponent>(entity))
			{
				comps["material"] = {
					{ "asset", uuidToJson(m->materialAssetId) },
				};
			}
			if (auto* c = registry.try_get<CameraComponent>(entity))
			{
				comps["camera"] = {
					{ "fovDegrees",   c->fovDegrees },
					{ "nearPlane",    c->nearPlane },
					{ "farPlane",     c->farPlane },
					{ "isMain",       c->isMain },
					{ "orthographic", c->orthographic },
				};
			}
			if (auto* l = registry.try_get<LightComponent>(entity))
			{
				comps["light"] = {
					{ "type",        static_cast<uint8_t>(l->type) },
					{ "color",       vec3ToJson(l->color) },
					{ "intensity",   l->intensity },
					{ "range",       l->range },
					{ "spotAngle",   l->spotAngle },
					{ "castsShadow", l->castsShadow },
				};
			}
			if (auto* r = registry.try_get<RigidBodyComponent>(entity))
			{
				comps["rigidbody"] = {
					{ "type",        static_cast<uint8_t>(r->type) },
					{ "mass",        r->mass },
					{ "friction",    r->friction },
					{ "restitution", r->restitution },
					{ "is2D",        r->is2D },
				};
			}
			if (auto* s = registry.try_get<ScriptComponent>(entity))
			{
				comps["script"] = {
					{ "asset",      uuidToJson(s->scriptAssetId) },
					{ "moduleName", s->moduleName },
					{ "enabled",    s->enabled },
				};
			}
			if (auto* e = registry.try_get<EnvironmentComponent>(entity))
			{
				comps["environment"] = {
					{ "dayNightCycle",     e->dayNightCycle },
					{ "timeOfDay",         e->timeOfDay },
					{ "autoAdvance",       e->autoAdvance },
					{ "cycleSeconds",      e->cycleSeconds },
					{ "sunColor",          vec3ToJson(e->sunColor) },
					{ "sunIntensity",      e->sunIntensity },
					{ "moonColor",         vec3ToJson(e->moonColor) },
					{ "moonIntensity",     e->moonIntensity },
					{ "cloudCoverage",     e->cloudCoverage },
					{ "windDirection",     e->windDirection },
					{ "windSpeed",         e->windSpeed },
					{ "fogDensity",        e->fogDensity },
					{ "fogHeightFalloff",  e->fogHeightFalloff },
					{ "auroraIntensity",   e->auroraIntensity },
					{ "milkyWayIntensity", e->milkyWayIntensity },
					{ "nebulaIntensity",   e->nebulaIntensity },
					{ "nebulaColor",       vec3ToJson(e->nebulaColor) },
					{ "auroraColor",       vec3ToJson(e->auroraColor) },
				};
			}

			if (!comps.is_null())
				eJson["components"] = comps;

			entities.push_back(eJson);
		}
		scene["entities"] = entities;
		return scene;
	}

	// ── JSON → components of one entity ──────────────────────────────────────
	void applyComponents(entt::registry& registry, Entity entity, const json& comps)
	{
		if (comps.contains("transform"))
		{
			const json& c = comps["transform"];
			TransformComponent t;
			t.position = jsonToVec3(c.value("position", json()), t.position);
			t.rotation = jsonToVec3(c.value("rotation", json()), t.rotation);
			t.scale    = jsonToVec3(c.value("scale",    json()), t.scale);
			registry.emplace_or_replace<TransformComponent>(entity, t);
		}
		if (comps.contains("transform2d"))
		{
			const json& c = comps["transform2d"];
			Transform2DComponent t;
			t.position = jsonToVec2(c.value("position", json()), t.position);
			t.rotation = c.value("rotation", t.rotation);
			t.scale    = jsonToVec2(c.value("scale", json()), t.scale);
			registry.emplace_or_replace<Transform2DComponent>(entity, t);
		}
		if (comps.contains("mesh"))
		{
			const json& c = comps["mesh"];
			MeshComponent m;
			m.meshAssetId    = jsonToUuid(c.value("asset", json()));
			m.lodBias        = c.value("lodBias", m.lodBias);
			m.castsShadow    = c.value("castsShadow", m.castsShadow);
			m.receivesShadow = c.value("receivesShadow", m.receivesShadow);
			registry.emplace_or_replace<MeshComponent>(entity, m);
		}
		if (comps.contains("material"))
		{
			MaterialComponent m;
			m.materialAssetId = jsonToUuid(comps["material"].value("asset", json()));
			registry.emplace_or_replace<MaterialComponent>(entity, m);
		}
		if (comps.contains("camera"))
		{
			const json& c = comps["camera"];
			CameraComponent cam;
			cam.fovDegrees   = c.value("fovDegrees",   cam.fovDegrees);
			cam.nearPlane    = c.value("nearPlane",    cam.nearPlane);
			cam.farPlane     = c.value("farPlane",     cam.farPlane);
			cam.isMain       = c.value("isMain",       cam.isMain);
			cam.orthographic = c.value("orthographic", cam.orthographic);
			registry.emplace_or_replace<CameraComponent>(entity, cam);
		}
		if (comps.contains("light"))
		{
			const json& c = comps["light"];
			LightComponent l;
			l.type        = static_cast<LightType>(c.value("type", static_cast<uint8_t>(l.type)));
			l.color       = jsonToVec3(c.value("color", json()), l.color);
			l.intensity   = c.value("intensity",   l.intensity);
			l.range       = c.value("range",       l.range);
			l.spotAngle   = c.value("spotAngle",   l.spotAngle);
			l.castsShadow = c.value("castsShadow", l.castsShadow);
			registry.emplace_or_replace<LightComponent>(entity, l);
		}
		if (comps.contains("rigidbody"))
		{
			const json& c = comps["rigidbody"];
			RigidBodyComponent r;
			r.type        = static_cast<RigidBodyType>(c.value("type", static_cast<uint8_t>(r.type)));
			r.mass        = c.value("mass",        r.mass);
			r.friction    = c.value("friction",    r.friction);
			r.restitution = c.value("restitution", r.restitution);
			r.is2D        = c.value("is2D",        r.is2D);
			registry.emplace_or_replace<RigidBodyComponent>(entity, r);
		}
		if (comps.contains("script"))
		{
			const json& c = comps["script"];
			ScriptComponent s;
			s.scriptAssetId = jsonToUuid(c.value("asset", json()));
			s.moduleName    = c.value("moduleName", s.moduleName);
			s.enabled       = c.value("enabled",    s.enabled);
			registry.emplace_or_replace<ScriptComponent>(entity, s);
		}
		if (comps.contains("environment"))
		{
			const json& c = comps["environment"];
			EnvironmentComponent e;
			e.dayNightCycle     = c.value("dayNightCycle",     e.dayNightCycle);
			e.timeOfDay         = c.value("timeOfDay",         e.timeOfDay);
			e.autoAdvance       = c.value("autoAdvance",       e.autoAdvance);
			e.cycleSeconds      = c.value("cycleSeconds",      e.cycleSeconds);
			e.sunColor          = jsonToVec3(c.value("sunColor",  json()), e.sunColor);
			e.sunIntensity      = c.value("sunIntensity",      e.sunIntensity);
			e.moonColor         = jsonToVec3(c.value("moonColor", json()), e.moonColor);
			e.moonIntensity     = c.value("moonIntensity",     e.moonIntensity);
			e.cloudCoverage     = c.value("cloudCoverage",     e.cloudCoverage);
			e.windDirection     = c.value("windDirection",     e.windDirection);
			e.windSpeed         = c.value("windSpeed",         e.windSpeed);
			e.fogDensity        = c.value("fogDensity",        e.fogDensity);
			e.fogHeightFalloff  = c.value("fogHeightFalloff",  e.fogHeightFalloff);
			e.auroraIntensity   = c.value("auroraIntensity",   e.auroraIntensity);
			e.milkyWayIntensity = c.value("milkyWayIntensity", e.milkyWayIntensity);
			e.nebulaIntensity   = c.value("nebulaIntensity",   e.nebulaIntensity);
			e.nebulaColor       = jsonToVec3(c.value("nebulaColor", json()), e.nebulaColor);
			e.auroraColor       = jsonToVec3(c.value("auroraColor", json()), e.auroraColor);
			registry.emplace_or_replace<EnvironmentComponent>(entity, e);
		}
	}

	// ── JSON → Scene ─────────────────────────────────────────────────────────
	bool applySceneJson(HorizonWorld& world, const json& scene)
	{
		if (!scene.contains("entities")) return true; // empty scene — valid

		// Map serialised uint32 IDs to newly created entt entities.
		std::unordered_map<uint32_t, Entity> idMap;
		auto& registry = world.registry();

		// ── Pass 1: create entities, set names, apply components ─────────────
		// The root is the entity with no parent (parent == entt::null), NOT
		// necessarily the first entry: entt's view iterates in reverse-creation
		// order, so the root (created first) is usually serialised LAST. Map that
		// one to the existing root instead of creating a duplicate; mapping by
		// position renamed the root to whatever happened to be first and shredded
		// the hierarchy on every save/load and undo. (#root-by-parent)
		constexpr uint32_t kNullId = 0xFFFFFFFFu; // static_cast<uint32_t>(entt::null)
		bool rootMapped = false;
		for (auto& eJson : scene["entities"])
		{
			uint32_t    serialId = eJson.value("id",   0u);
			std::string name     = eJson.value("name", "Entity");
			uint32_t    parent   = eJson.value("parent", kNullId);

			Entity e;
			if (!rootMapped && parent == kNullId)
			{
				e = world.rootEntity();
				world.renameEntity(e, name);
				rootMapped = true;
			}
			else
				e = world.createEntity(name);

			idMap[serialId] = e;

			if (eJson.contains("components"))
				applyComponents(registry, e, eJson["components"]);
		}

		// ── Pass 2: rebuild parent/child links ────────────────────────────────
		for (auto& eJson : scene["entities"])
		{
			if (!eJson.contains("children")) continue;

			uint32_t serialId = eJson.value("id", 0u);
			auto it = idMap.find(serialId);
			if (it == idMap.end()) continue;

			Entity parent = it->second;
			auto*  pHier  = registry.try_get<HierarchyComponent>(parent);
			if (!pHier) continue;

			// Clear the children list rebuilt during createEntity — restore exact order
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

		// Built-in sun/moon lights aren't serialised — recreate / re-attach them so
		// every loaded scene has them on the root.
		world.ensureEnvironmentLights();
		world.markHierarchyDirty();
		return true;
	}
} // namespace

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

// ── JSON ──────────────────────────────────────────────────────────────────────
bool SceneSerializer::saveJSON(const HorizonWorld& world, const std::filesystem::path& path)
{
    // Mutable access needed for registry views — safe during serialisation
    json scene = buildSceneJson(const_cast<HorizonWorld&>(world));

    std::ofstream out(path);
    if (!out.is_open()) return false;
    out << scene.dump(4);
    return out.good();
}

bool SceneSerializer::loadJSON(HorizonWorld& world, const std::filesystem::path& path)
{
    std::ifstream in(path);
    if (!in.is_open()) return false;

    json scene = json::parse(in, nullptr, false);
    if (scene.is_discarded()) return false;

    return applySceneJson(world, scene);
}

// ── In-memory snapshots (CBOR) ────────────────────────────────────────────────
bool SceneSerializer::saveToMemory(const HorizonWorld& world, std::vector<uint8_t>& out)
{
    json scene = buildSceneJson(const_cast<HorizonWorld&>(world));
    out = json::to_cbor(scene);
    return true;
}

bool SceneSerializer::loadFromMemory(HorizonWorld& world, const std::vector<uint8_t>& data)
{
    json scene = json::from_cbor(data, true, false);
    if (scene.is_discarded()) return false;
    return applySceneJson(world, scene);
}

// ── Binary (CBOR encoding of the identical JSON structure) ───────────────────
bool SceneSerializer::saveBinary(const HorizonWorld& world, const std::filesystem::path& path)
{
    json scene = buildSceneJson(const_cast<HorizonWorld&>(world));
    const std::vector<uint8_t> cbor = json::to_cbor(scene);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out.write(reinterpret_cast<const char*>(cbor.data()),
              static_cast<std::streamsize>(cbor.size()));
    return out.good();
}

bool SceneSerializer::loadBinary(HorizonWorld& world, const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;

    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
    json scene = json::from_cbor(bytes, true, false);
    if (scene.is_discarded()) return false;

    return applySceneJson(world, scene);
}
