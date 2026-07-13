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
#include "HorizonScene/Components/ColliderComponent.h"
#include "HorizonScene/Components/CharacterControllerComponent.h"
#include "HorizonScene/Components/ScriptComponent.h"
#include "HorizonScene/Components/EnvironmentComponent.h"
#include "HorizonScene/Components/EnvironmentLightComponent.h"
#include "HorizonScene/Components/TerrainChunkComponent.h"
#include "HorizonScene/Components/WeatherComponent.h"
#include "HorizonScene/Components/TerrainComponent.h"
#include "HorizonScene/Components/AudioSourceComponent.h"
#include "HorizonScene/Components/AudioListenerComponent.h"
#include "HorizonScene/Components/ParticleSystemComponent.h"
#include "HorizonScene/Components/LODComponent.h"
#include "HorizonScene/Components/FoliageComponent.h"
#include "HorizonScene/Components/UICanvasComponent.h"
#include "HorizonScene/Components/UIElementComponent.h"
#include "HorizonScene/Components/UITextComponent.h"
#include "HorizonScene/Components/UIImageComponent.h"
#include "HorizonScene/Components/UIButtonComponent.h"
#include "HorizonScene/Components/AnimatorStateMachineComponent.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <cstring>
#include <cstdint>
#include <string>

using json = nlohmann::json;

// ── (De)serialisation helpers ────────────────────────────────────────────────
namespace
{
	json vec3ToJson(const glm::vec3& v)  { return json::array({ v.x, v.y, v.z }); }
	json vec2ToJson(const glm::vec2& v)  { return json::array({ v.x, v.y }); }
	json vec4ToJson(const glm::vec4& v)  { return json::array({ v.x, v.y, v.z, v.w }); }

	glm::vec4 jsonToVec4(const json& j, const glm::vec4& fallback)
	{
		if (!j.is_array() || j.size() != 4) return fallback;
		return { j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>() };
	}

	// Base64 (RFC 4648) for embedding a large binary array (terrain sculptHeights)
	// as ONE string node instead of a JSON array of N floats — the array form builds
	// N nlohmann json nodes (N≈260k for a 513² terrain), which dominated the editor
	// undo snapshot (saveToMemory → CBOR). The string is host-endian raw float bytes;
	// fine for the in-memory undo and for same-arch scene files (all targets little-endian).
	constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string base64Encode(const uint8_t* data, size_t len)
	{
		std::string out;
		out.reserve(((len + 2) / 3) * 4);
		for (size_t i = 0; i < len; i += 3)
		{
			uint32_t n = static_cast<uint32_t>(data[i]) << 16;
			if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
			if (i + 2 < len) n |= static_cast<uint32_t>(data[i + 2]);
			out.push_back(kB64[(n >> 18) & 63]);
			out.push_back(kB64[(n >> 12) & 63]);
			out.push_back((i + 1 < len) ? kB64[(n >> 6) & 63] : '=');
			out.push_back((i + 2 < len) ? kB64[n & 63]        : '=');
		}
		return out;
	}
	std::vector<uint8_t> base64Decode(const std::string& s)
	{
		auto val = [](char c) -> int {
			if (c >= 'A' && c <= 'Z') return c - 'A';
			if (c >= 'a' && c <= 'z') return c - 'a' + 26;
			if (c >= '0' && c <= '9') return c - '0' + 52;
			if (c == '+') return 62;
			if (c == '/') return 63;
			return -1;
		};
		std::vector<uint8_t> out;
		out.reserve(s.size() / 4 * 3);
		int buf = 0, bits = 0;
		for (char c : s)
		{
			if (c == '=') break;
			const int v = val(c);
			if (v < 0) continue;
			buf = (buf << 6) | v; bits += 6;
			if (bits >= 8) { bits -= 8; out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF)); }
		}
		return out;
	}

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

	json propValueToJson(const ScriptPropValue& v)
	{
		switch (v.type)
		{
		case ScriptPropType::Float:  return { {"type","float"},  {"value", v.f} };
		case ScriptPropType::Int:    return { {"type","int"},    {"value", v.i} };
		case ScriptPropType::Bool:   return { {"type","bool"},   {"value", v.b} };
		case ScriptPropType::String: return { {"type","string"}, {"value", v.s} };
		}
		return {};
	}

	ScriptPropValue jsonToPropValue(const json& j)
	{
		ScriptPropValue v;
		std::string type = j.value("type", "float");
		if (type == "int")    { v.type = ScriptPropType::Int;    v.i = j.value("value", 0);    }
		else if (type == "bool")   { v.type = ScriptPropType::Bool;   v.b = j.value("value", false); }
		else if (type == "string") { v.type = ScriptPropType::String; v.s = j.value("value", std::string{}); }
		else                       { v.type = ScriptPropType::Float;  v.f = j.value("value", 0.0f); }
		return v;
	}

	// ── Per-entity component → JSON ──────────────────────────────────────────
	json serializeComponents(entt::registry& registry, Entity entity)
	{
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
		// Skip MeshComponent for terrain entities — it is regenerated from
		// TerrainComponent parameters at load time, so it must not be
		// serialised (stale meshAssetIds break cross-session UUID stability).
		if (auto* m = registry.try_get<MeshComponent>(entity);
		    m != nullptr && !registry.all_of<TerrainComponent>(entity))
		{
			comps["mesh"] = {
				{ "asset",          uuidToJson(m->meshAssetId) },
				{ "lodBias",        m->lodBias },
				{ "visible",        m->visible },
				{ "castsShadow",    m->castsShadow },
				{ "receivesShadow", m->receivesShadow },
			};
		}
		if (auto* m = registry.try_get<MaterialComponent>(entity))
		{
			comps["material"] = {
				{ "asset", uuidToJson(m->materialAssetId) },
			};
			// Per-entity node-graph param overrides (name → [x,y,z,w]).
			if (!m->paramOverrides.empty())
			{
				json ovs = json::array();
				for (const auto& ov : m->paramOverrides)
					ovs.push_back({ { "name", ov.name },
					                { "value", { ov.value[0], ov.value[1], ov.value[2], ov.value[3] } } });
				comps["material"]["paramOverrides"] = std::move(ovs);
			}
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
				{ "visible",     l->visible },
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
		if (auto* col = registry.try_get<ColliderComponent>(entity))
		{
			comps["collider"] = {
				{ "shape",     static_cast<uint8_t>(col->shape) },
				{ "halfEx",    { col->halfExtents.x, col->halfExtents.y, col->halfExtents.z } },
				{ "radius",    col->radius },
				{ "height",    col->height },
				{ "isTrigger", col->isTrigger },
			};
		}
		if (auto* cc = registry.try_get<CharacterControllerComponent>(entity))
		{
			comps["characterController"] = {
				{ "slopeLimit", cc->slopeLimit },
				{ "stepHeight", cc->stepHeight },
				{ "skinWidth",  cc->skinWidth  },
				{ "mass",       cc->mass       },
				{ "gravity",    cc->gravity    },
			};
		}
		if (auto* s = registry.try_get<ScriptComponent>(entity))
		{
			json props = json::object();
			for (const auto& [k, v] : s->properties)
				props[k] = propValueToJson(v);
			comps["script"] = {
				{ "asset",      uuidToJson(s->scriptAssetId) },
				{ "moduleName", s->moduleName },
				{ "enabled",    s->enabled },
				{ "properties", props },
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
				{ "moonPhase",         e->moonPhase },
				{ "moonPhaseAuto",     e->moonPhaseAuto },
				{ "moonCycleDays",     e->moonCycleDays },
				{ "cloudCoverage",     e->cloudCoverage },
				{ "cloudMode",         e->cloudMode },
				{ "cloudQuality",      e->cloudQuality },
				{ "lowResClouds",      e->lowResClouds },
				{ "cloudHeight",       e->cloudHeight },
				{ "cloudDensity",      e->cloudDensity },
				{ "cloudFluffiness",   e->cloudFluffiness },
				{ "cloudTint",         vec3ToJson(e->cloudTint) },
				{ "contrailAmount",    e->contrailAmount },
				{ "cirrusAmount",      e->cirrusAmount },
				{ "cirrusSeed",        e->cirrusSeed },
				{ "godRays",           e->godRays },
				{ "shootingStars",     e->shootingStars },
				{ "lensFlare",         e->lensFlare },
				{ "windDirection",     e->windDirection },
				{ "windSpeed",         e->windSpeed },
				{ "fogDensity",        e->fogDensity },
				{ "fogHeightFalloff",  e->fogHeightFalloff },
				{ "rainAmount",        e->rainAmount },
				{ "snowAmount",        e->snowAmount },
				{ "wetness",           e->wetness },
				{ "auroraIntensity",   e->auroraIntensity },
				{ "milkyWayIntensity", e->milkyWayIntensity },
				{ "nebulaIntensity",   e->nebulaIntensity },
				{ "nebulaColor",       vec3ToJson(e->nebulaColor) },
				{ "nebulaColor2",      vec3ToJson(e->nebulaColor2) },
				{ "nebulaColor3",      vec3ToJson(e->nebulaColor3) },
				{ "nebulaSeed",        e->nebulaSeed },
				{ "nebulaCoverage",    e->nebulaCoverage },
				{ "nebulaQuality",     e->nebulaQuality },
				{ "auroraColor",       vec3ToJson(e->auroraColor) },
				{ "auroraColorTop",    vec3ToJson(e->auroraColorTop) },
				{ "auroraHeight",        e->auroraHeight },
				{ "auroraFragmentation", e->auroraFragmentation },
				{ "starBrightness",    e->starBrightness },
				{ "starColor",         vec3ToJson(e->starColor) },
				{ "starSize",          e->starSize },
				{ "starSizeVariation", e->starSizeVariation },
				{ "starGlow",          e->starGlow },
				{ "starTwinkle",       e->starTwinkle },
				{ "starDensity",       e->starDensity },
			};
		}
		if (auto* w = registry.try_get<WeatherComponent>(entity))
		{
			comps["weather"] = {
				{ "currentKind",        static_cast<int>(w->currentKind) },
				{ "targetKind",         static_cast<int>(w->targetKind) },
				{ "intensity",          w->intensity },
				{ "transitionDuration", w->transitionDuration },
				{ "autoCycle",          w->autoCycle },
				{ "cycleSeconds",       w->cycleSeconds },
				{ "thunderSound",       uuidToJson(w->thunderSound) },
				{ "maxRainParticles",   w->maxRainParticles },
				{ "maxSnowParticles",   w->maxSnowParticles },
				{ "groundLevel",        w->groundLevel },
			};
		}
		if (auto* t = registry.try_get<TerrainComponent>(entity))
		{
			json tc = {
				{ "sizeX",      t->sizeX },
				{ "sizeZ",      t->sizeZ },
				{ "resolution", t->resolution },
				{ "heightScale",t->heightScale },
				{ "seed",       t->seed },
				{ "octaves",    t->octaves },
				{ "frequency",  t->frequency },
				{ "lacunarity", t->lacunarity },
				{ "gain",       t->gain },
			};
			if (!t->sculptHeights.empty())
			{
				// As a base64 blob, NOT a JSON array of N floats — the array form
				// builds N json nodes and dominated the undo snapshot cost.
				tc["sculptHeightsB64"] = base64Encode(
					reinterpret_cast<const uint8_t*>(t->sculptHeights.data()),
					t->sculptHeights.size() * sizeof(float));
			}
			comps["terrain"] = tc;
		}
		if (auto* a = registry.try_get<AudioSourceComponent>(entity))
		{
			comps["audiosource"] = {
				{ "asset",        uuidToJson(a->assetId) },
				{ "busName",      a->busName },
				{ "volume",       a->volume },
				{ "pitch",        a->pitch },
				{ "range",        a->range },
				{ "innerRange",   a->innerRange },
				{ "rolloffFactor",a->rolloffFactor },
				{ "loop",         a->loop },
				{ "playOnStart",  a->playOnStart },
				{ "spatial",      a->spatial },
			};
		}
		if (auto* l = registry.try_get<AudioListenerComponent>(entity))
		{
			comps["audiolistener"] = {
				{ "masterVolume", l->masterVolume },
			};
		}
		if (auto* ps = registry.try_get<ParticleSystemComponent>(entity))
		{
			comps["particlesystem"] = {
				{ "particleAsset", uuidToJson(ps->particleAssetId) },
				{ "visible",       ps->visible },
				{ "playing",       ps->playing },
			};
		}
		if (auto* sm = registry.try_get<AnimatorStateMachineComponent>(entity))
		{
			// The graph itself (states/transitions/default params) lives in the
			// referenced AnimatorStateMachineAsset — only per-entity runtime state
			// is written here (two entities sharing one asset can be in different
			// states with different live param values), same shape as
			// ParticleSystemComponent's {particleAsset, visible, playing}.
			json params = json::object();
			for (const auto& [k, v] : sm->params) params[k] = v;
			comps["animstatemachine"] = {
				{ "stateMachineAsset", uuidToJson(sm->stateMachineAssetId) },
				{ "currentStateName",  sm->currentStateName },
				{ "params",            params },
			};
		}
		if (auto* lod = registry.try_get<LODComponent>(entity))
		{
			json levels = json::array();
			for (const auto& lvl : lod->levels)
				levels.push_back({ { "meshId", uuidToJson(lvl.meshId) },
				                   { "maxDistance", lvl.maxDistance } });
			comps["lod"] = { { "levels", levels } };
		}
		if (auto* fol = registry.try_get<FoliageComponent>(entity))
		{
			comps["foliage"] = {
				{ "visible",      fol->visible },
				{ "mesh",         uuidToJson(fol->meshAssetId) },
				{ "material",     uuidToJson(fol->materialAssetId) },
				{ "density",      fol->density },
				{ "seed",         fol->seed },
				{ "minScale",     fol->minScale },
				{ "maxScale",     fol->maxScale },
				{ "drawDistance", fol->drawDistance },
			};
		}
		if (auto* c = registry.try_get<UICanvasComponent>(entity))
		{
			comps["uicanvas"] = {
				{ "width",      c->width },
				{ "height",     c->height },
				{ "renderMode", static_cast<int>(c->renderMode) },
				{ "active",     c->active },
			};
		}
		if (auto* e2 = registry.try_get<UIElementComponent>(entity))
		{
			comps["uielement"] = {
				{ "position", vec2ToJson(e2->position) },
				{ "size",     vec2ToJson(e2->size) },
				{ "pivot",    vec2ToJson(e2->pivot) },
				{ "rotation", e2->rotation },
				{ "anchor",   static_cast<int>(e2->anchor) },
				{ "layer",    e2->layer },
				{ "active",   e2->active },
			};
		}
		if (auto* t2 = registry.try_get<UITextComponent>(entity))
		{
			comps["uitext"] = {
				{ "text",     t2->text },
				{ "fontSize", t2->fontSize },
				{ "color",    vec4ToJson(t2->color) },
			};
		}
		if (auto* img = registry.try_get<UIImageComponent>(entity))
		{
			comps["uiimage"] = {
				{ "material", uuidToJson(img->materialAssetId) },
				{ "tint",     vec4ToJson(img->tint) },
			};
		}
		if (auto* btn = registry.try_get<UIButtonComponent>(entity))
		{
			comps["uibutton"] = {
				{ "normalColor",   vec4ToJson(btn->normalColor) },
				{ "hoveredColor",  vec4ToJson(btn->hoveredColor) },
				{ "pressedColor",  vec4ToJson(btn->pressedColor) },
				{ "onClickFunction", btn->onClickFunction },
			};
		}
		return comps;
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
			// The built-in environment sun/moon lights and runtime terrain chunks are
			// never serialised — both are recreated on load (ensureEnvironmentLights /
			// TerrainSystem from the TerrainComponent), so the scene file stays clean.
			if (registry.all_of<EnvironmentLightComponent>(entity) ||
			    registry.all_of<TerrainChunkComponent>(entity))
				continue;

			json eJson;
			eJson["id"]   = static_cast<uint32_t>(entity);
			eJson["name"] = registry.get<NameComponent>(entity).name;

			if (auto* hier = registry.try_get<HierarchyComponent>(entity))
			{
				eJson["parent"] = static_cast<uint32_t>(hier->parent);
				json children = json::array();
				for (auto child : hier->children)
					if (!registry.all_of<EnvironmentLightComponent>(child) &&    // omit built-ins
					    !registry.all_of<TerrainChunkComponent>(child))          // + terrain chunks
						children.push_back(static_cast<uint32_t>(child));
				eJson["children"] = children;
			}

			json comps = serializeComponents(registry, entity);
			if (!comps.is_null())
				eJson["components"] = comps;

			entities.push_back(eJson);
		}
		scene["entities"] = entities;

		// Level script (HorizonCode graph, one per scene). Stored as a nested
		// object so the scene file stays readable; omitted when empty.
		const std::string ls = world.levelScriptJson();
		if (!ls.empty())
			scene["levelScript"] = json::parse(ls, nullptr, /*allow_exceptions=*/false);

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
			m.visible        = c.value("visible", m.visible);
			m.castsShadow    = c.value("castsShadow", m.castsShadow);
			m.receivesShadow = c.value("receivesShadow", m.receivesShadow);
			registry.emplace_or_replace<MeshComponent>(entity, m);
		}
		if (comps.contains("material"))
		{
			MaterialComponent m;
			m.materialAssetId = jsonToUuid(comps["material"].value("asset", json()));
			if (auto it = comps["material"].find("paramOverrides");
			    it != comps["material"].end() && it->is_array())
				for (const auto& jo : *it)
				{
					MaterialParamOverride ov;
					ov.name = jo.value("name", std::string());
					if (auto v = jo.find("value"); v != jo.end() && v->is_array())
						for (size_t k = 0; k < 4 && k < v->size(); ++k) ov.value[k] = (*v)[k].get<float>();
					if (!ov.name.empty()) m.paramOverrides.push_back(std::move(ov));
				}
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
			l.visible     = c.value("visible",     l.visible);
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
		if (comps.contains("collider"))
		{
			const json& c = comps["collider"];
			ColliderComponent col;
			col.shape     = static_cast<ColliderShape>(c.value("shape", static_cast<uint8_t>(col.shape)));
			col.radius    = c.value("radius",    col.radius);
			col.height    = c.value("height",    col.height);
			col.isTrigger = c.value("isTrigger", col.isTrigger);
			if (c.contains("halfEx") && c["halfEx"].is_array() && c["halfEx"].size() == 3)
				col.halfExtents = { c["halfEx"][0], c["halfEx"][1], c["halfEx"][2] };
			registry.emplace_or_replace<ColliderComponent>(entity, col);
		}
		if (comps.contains("characterController"))
		{
			const json& c = comps["characterController"];
			CharacterControllerComponent cc;
			cc.slopeLimit = c.value("slopeLimit", cc.slopeLimit);
			cc.stepHeight = c.value("stepHeight", cc.stepHeight);
			cc.skinWidth  = c.value("skinWidth",  cc.skinWidth);
			cc.mass       = c.value("mass",       cc.mass);
			cc.gravity    = c.value("gravity",    cc.gravity);
			registry.emplace_or_replace<CharacterControllerComponent>(entity, cc);
		}
		if (comps.contains("script"))
		{
			const json& c = comps["script"];
			ScriptComponent s;
			s.scriptAssetId = jsonToUuid(c.value("asset", json()));
			s.moduleName    = c.value("moduleName", s.moduleName);
			s.enabled       = c.value("enabled",    s.enabled);
			if (c.contains("properties") && c["properties"].is_object())
			{
				for (const auto& [k, v] : c["properties"].items())
					s.properties[k] = jsonToPropValue(v);
			}
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
			e.moonPhase         = c.value("moonPhase",         e.moonPhase);
			e.moonPhaseAuto     = c.value("moonPhaseAuto",     e.moonPhaseAuto);
			e.moonCycleDays     = c.value("moonCycleDays",     e.moonCycleDays);
			e.cloudCoverage     = c.value("cloudCoverage",     e.cloudCoverage);
			e.cloudMode         = c.value("cloudMode",         e.cloudMode);
			e.cloudQuality      = c.value("cloudQuality",      e.cloudQuality);
			e.lowResClouds      = c.value("lowResClouds",      e.lowResClouds);
			e.cloudHeight       = c.value("cloudHeight",       e.cloudHeight);
			e.cloudDensity      = c.value("cloudDensity",      e.cloudDensity);
			e.cloudFluffiness   = c.value("cloudFluffiness",   e.cloudFluffiness);
			e.cloudTint         = jsonToVec3(c.value("cloudTint", json()), e.cloudTint);
			e.contrailAmount    = c.value("contrailAmount",    e.contrailAmount);
			e.cirrusAmount      = c.value("cirrusAmount",      e.cirrusAmount);
			e.cirrusSeed        = c.value("cirrusSeed",        e.cirrusSeed);
			e.godRays           = c.value("godRays",           e.godRays);
			e.shootingStars     = c.value("shootingStars",     e.shootingStars);
			e.lensFlare         = c.value("lensFlare",         e.lensFlare);
			e.windDirection     = c.value("windDirection",     e.windDirection);
			e.windSpeed         = c.value("windSpeed",         e.windSpeed);
			e.fogDensity        = c.value("fogDensity",        e.fogDensity);
			e.fogHeightFalloff  = c.value("fogHeightFalloff",  e.fogHeightFalloff);
			e.rainAmount        = c.value("rainAmount",        e.rainAmount);
			e.snowAmount        = c.value("snowAmount",        e.snowAmount);
			e.wetness           = c.value("wetness",           e.wetness);
			e.auroraIntensity   = c.value("auroraIntensity",   e.auroraIntensity);
			e.milkyWayIntensity = c.value("milkyWayIntensity", e.milkyWayIntensity);
			e.nebulaIntensity   = c.value("nebulaIntensity",   e.nebulaIntensity);
			e.nebulaColor       = jsonToVec3(c.value("nebulaColor", json()), e.nebulaColor);
			e.nebulaColor2      = jsonToVec3(c.value("nebulaColor2", json()), e.nebulaColor2);
			e.nebulaColor3      = jsonToVec3(c.value("nebulaColor3", json()), e.nebulaColor3);
			e.nebulaSeed        = c.value("nebulaSeed",        e.nebulaSeed);
			e.nebulaCoverage    = c.value("nebulaCoverage",    e.nebulaCoverage);
			// nebulaQuality (0/1/2) replaced the old nebulaHighFidelity bool — fall back to it
			// for scenes saved before the change (true → High=1, false → Performance=0).
			if (c.contains("nebulaQuality"))
				e.nebulaQuality = c.value("nebulaQuality", e.nebulaQuality);
			else if (c.contains("nebulaHighFidelity"))
				e.nebulaQuality = c.value("nebulaHighFidelity", true) ? 1 : 0;
			e.auroraColor       = jsonToVec3(c.value("auroraColor", json()), e.auroraColor);
			e.auroraColorTop     = jsonToVec3(c.value("auroraColorTop", json()), e.auroraColorTop);
			e.auroraHeight        = c.value("auroraHeight",        e.auroraHeight);
			e.auroraFragmentation = c.value("auroraFragmentation", e.auroraFragmentation);
			e.starBrightness    = c.value("starBrightness",    e.starBrightness);
			e.starColor         = jsonToVec3(c.value("starColor", json()), e.starColor);
			e.starSize          = c.value("starSize",          e.starSize);
			e.starSizeVariation = c.value("starSizeVariation", e.starSizeVariation);
			e.starGlow          = c.value("starGlow",          e.starGlow);
			e.starTwinkle       = c.value("starTwinkle",       e.starTwinkle);
			e.starDensity       = c.value("starDensity",       e.starDensity);
			registry.emplace_or_replace<EnvironmentComponent>(entity, e);
		}
		if (comps.contains("weather"))
		{
			const json& c = comps["weather"];
			WeatherComponent w;
			w.currentKind        = static_cast<WeatherKind>(c.value("currentKind", static_cast<int>(w.currentKind)));
			w.targetKind         = static_cast<WeatherKind>(c.value("targetKind",  static_cast<int>(w.targetKind)));
			w.prevTarget         = w.targetKind; // no spurious reclaim on load → authored env is respected
			w.intensity          = c.value("intensity",          w.intensity);
			w.transitionDuration = c.value("transitionDuration", w.transitionDuration);
			w.autoCycle          = c.value("autoCycle",          w.autoCycle);
			w.cycleSeconds       = c.value("cycleSeconds",       w.cycleSeconds);
			if (c.contains("thunderSound")) w.thunderSound = jsonToUuid(c["thunderSound"]);
			w.maxRainParticles = c.value("maxRainParticles", w.maxRainParticles);
			w.maxSnowParticles = c.value("maxSnowParticles", w.maxSnowParticles);
			w.groundLevel      = c.value("groundLevel",      w.groundLevel);
			registry.emplace_or_replace<WeatherComponent>(entity, w);
		}
		if (comps.contains("terrain"))
		{
			const json& c = comps["terrain"];
			TerrainComponent t;
			t.sizeX       = c.value("sizeX",       t.sizeX);
			t.sizeZ       = c.value("sizeZ",        t.sizeZ);
			t.resolution  = c.value("resolution",   t.resolution);
			t.heightScale = c.value("heightScale",  t.heightScale);
			t.seed        = c.value("seed",         t.seed);
			t.octaves     = c.value("octaves",      t.octaves);
			t.frequency   = c.value("frequency",    t.frequency);
			t.lacunarity  = c.value("lacunarity",   t.lacunarity);
			t.gain        = c.value("gain",         t.gain);
			if (c.contains("sculptHeightsB64") && c["sculptHeightsB64"].is_string())
			{
				const std::vector<uint8_t> bytes =
					base64Decode(c["sculptHeightsB64"].get<std::string>());
				t.sculptHeights.resize(bytes.size() / sizeof(float));
				if (!t.sculptHeights.empty())
					std::memcpy(t.sculptHeights.data(), bytes.data(),
					            t.sculptHeights.size() * sizeof(float));
			}
			else if (c.contains("sculptHeights") && c["sculptHeights"].is_array())
				t.sculptHeights = c["sculptHeights"].get<std::vector<float>>(); // legacy scenes
			t.dirty       = true; // always regenerate after load
			registry.emplace_or_replace<TerrainComponent>(entity, t);
		}
		if (comps.contains("audiosource"))
		{
			const json& c = comps["audiosource"];
			AudioSourceComponent a;
			a.assetId       = jsonToUuid(c.value("asset", json()));
			a.busName       = c.value("busName",       a.busName);
			a.volume        = c.value("volume",        a.volume);
			a.pitch         = c.value("pitch",         a.pitch);
			a.range         = c.value("range",         a.range);
			a.innerRange    = c.value("innerRange",    a.innerRange);
			a.rolloffFactor = c.value("rolloffFactor", a.rolloffFactor);
			a.loop          = c.value("loop",          a.loop);
			a.playOnStart   = c.value("playOnStart",   a.playOnStart);
			a.spatial       = c.value("spatial",       a.spatial);
			registry.emplace_or_replace<AudioSourceComponent>(entity, a);
		}
		if (comps.contains("audiolistener"))
		{
			const json& c = comps["audiolistener"];
			AudioListenerComponent l;
			l.masterVolume = c.value("masterVolume", l.masterVolume);
			registry.emplace_or_replace<AudioListenerComponent>(entity, l);
		}
		if (comps.contains("particlesystem"))
		{
			const json& c = comps["particlesystem"];
			ParticleSystemComponent ps;
			ps.visible = c.value("visible", ps.visible);
			ps.playing = c.value("playing", ps.playing);
			if (c.contains("particleAsset"))
			{
				// Current format: the emitter config lives in a ParticleGraphAsset.
				ps.particleAssetId = jsonToUuid(c.value("particleAsset", json()));
			}
			else
			{
				// Pre-asset format: config was inline on the component. Stage it for
				// ParticleSystem::update to migrate into a real asset on first tick
				// (the serializer has no ContentManager to register one itself).
				auto& lg = ps.legacy;
				lg.hasData         = true;
				lg.meshAssetId     = jsonToUuid(c.value("mesh",     json()));
				lg.materialAssetId = jsonToUuid(c.value("material", json()));
				lg.emitRate        = c.value("emitRate",        lg.emitRate);
				lg.lifetimeMin     = c.value("lifetimeMin",     lg.lifetimeMin);
				lg.lifetimeMax     = c.value("lifetimeMax",     lg.lifetimeMax);
				lg.startSize       = c.value("startSize",       lg.startSize);
				lg.endSize         = c.value("endSize",         lg.endSize);
				lg.startColor      = jsonToVec3(c.value("startColor",      json()), lg.startColor);
				lg.endColor        = jsonToVec3(c.value("endColor",        json()), lg.endColor);
				lg.startAlpha      = c.value("startAlpha",      lg.startAlpha);
				lg.endAlpha        = c.value("endAlpha",        lg.endAlpha);
				lg.initialVelocity = jsonToVec3(c.value("initialVelocity", json()), lg.initialVelocity);
				lg.velocitySpread  = c.value("velocitySpread",  lg.velocitySpread);
				lg.gravity         = jsonToVec3(c.value("gravity",         json()), lg.gravity);
				lg.maxParticles    = c.value("maxParticles",    lg.maxParticles);
				lg.looping         = c.value("looping",         lg.looping);
			}
			registry.emplace_or_replace<ParticleSystemComponent>(entity, std::move(ps));
		}
		if (comps.contains("foliage"))
		{
			const json& c = comps["foliage"];
			FoliageComponent fol;
			fol.visible         = c.value("visible",      fol.visible);
			fol.meshAssetId     = jsonToUuid(c.value("mesh",     json()));
			fol.materialAssetId = jsonToUuid(c.value("material", json()));
			fol.density         = c.value("density",      fol.density);
			fol.seed            = c.value("seed",         fol.seed);
			fol.minScale        = c.value("minScale",     fol.minScale);
			fol.maxScale        = c.value("maxScale",     fol.maxScale);
			fol.drawDistance    = c.value("drawDistance", fol.drawDistance);
			fol.dirty           = true; // regenerate instances after load
			registry.emplace_or_replace<FoliageComponent>(entity, std::move(fol));
		}
		if (comps.contains("animstatemachine"))
		{
			const json& c = comps["animstatemachine"];
			AnimatorStateMachineComponent sm;
			if (c.contains("stateMachineAsset"))
			{
				// Current format: the graph lives in an AnimatorStateMachineAsset.
				sm.stateMachineAssetId = jsonToUuid(c.value("stateMachineAsset", json()));
				if (c.contains("params") && c["params"].is_object())
					for (auto it = c["params"].begin(); it != c["params"].end(); ++it)
						sm.params[it.key()] = it.value().get<float>();
				sm.currentStateName = c.value("currentStateName", std::string());
			}
			else
			{
				// Pre-asset format (Forts. 70, e82137f): states/transitions/params
				// were inline on the component. Stage it for AnimationStateMachine-
				// System to migrate into a real asset on first tick (the serializer
				// has no ContentManager to register one itself). A state entirely
				// missing "id" means the WHOLE array predates id/x/y (the GraphEditor
				// tab didn't exist yet) — auto-assign sequential ids + a simple grid
				// layout so the new editor still gets stable nodes.
				auto& lg = sm.legacy;
				lg.hasData = true;
				bool anyMissingId = false;
				if (c.contains("states") && c["states"].is_array())
				{
					for (const auto& sj : c["states"])
					{
						HE::AnimationState s;
						s.id      = sj.value("id", 0);
						s.name    = sj.value("name", std::string());
						s.clipId  = jsonToUuid(sj.value("clipId", json()));
						s.looping = sj.value("looping", true);
						s.x       = sj.value("x", 0.0f);
						s.y       = sj.value("y", 0.0f);
						if (s.id == 0) anyMissingId = true;
						lg.states.push_back(std::move(s));
					}
				}
				if (anyMissingId)
				{
					int col = 0, row = 0, nextId = 1;
					for (auto& s : lg.states)
					{
						s.id = nextId++;
						s.x  = static_cast<float>(col) * 200.0f;
						s.y  = static_cast<float>(row) * 150.0f;
						if (++col >= 4) { col = 0; ++row; }
					}
				}
				if (c.contains("transitions") && c["transitions"].is_array())
				{
					for (const auto& tj : c["transitions"])
					{
						HE::AnimationTransition t;
						t.fromState = tj.value("fromState", std::string());
						t.toState   = tj.value("toState",   std::string());
						t.paramName = tj.value("paramName", std::string());
						t.op        = static_cast<HE::TransitionOp>(tj.value("op", 0));
						t.threshold = tj.value("threshold", 0.5f);
						t.duration  = tj.value("duration",  0.2f);
						lg.transitions.push_back(std::move(t));
					}
				}
				if (c.contains("params") && c["params"].is_object())
					for (auto it = c["params"].begin(); it != c["params"].end(); ++it)
						lg.params[it.key()] = it.value().get<float>();
				lg.currentStateName = c.value("currentStateName", std::string());
			}
			registry.emplace_or_replace<AnimatorStateMachineComponent>(entity, std::move(sm));
		}
		if (comps.contains("lod"))
		{
			const json& c = comps["lod"];
			LODComponent lod;
			if (c.contains("levels") && c["levels"].is_array())
			{
				for (const auto& lj : c["levels"])
				{
					LODLevel lvl;
					lvl.meshId      = jsonToUuid(lj.value("meshId", json()));
					lvl.maxDistance = lj.value("maxDistance", lvl.maxDistance);
					lod.levels.push_back(lvl);
				}
			}
			registry.emplace_or_replace<LODComponent>(entity, std::move(lod));
		}
		if (comps.contains("uicanvas"))
		{
			const json& c = comps["uicanvas"];
			UICanvasComponent cv;
			cv.width      = c.value("width",      cv.width);
			cv.height     = c.value("height",     cv.height);
			cv.renderMode = static_cast<UIRenderMode>(c.value("renderMode", 0));
			cv.active     = c.value("active",     cv.active);
			registry.emplace_or_replace<UICanvasComponent>(entity, cv);
		}
		if (comps.contains("uielement"))
		{
			const json& c = comps["uielement"];
			UIElementComponent el;
			el.position = jsonToVec2(c.value("position", json()), el.position);
			el.size     = jsonToVec2(c.value("size",     json()), el.size);
			el.pivot    = jsonToVec2(c.value("pivot",    json()), el.pivot);
			el.rotation = c.value("rotation", el.rotation);
			el.anchor   = static_cast<UIAnchor>(c.value("anchor", 0));
			el.layer    = c.value("layer",    el.layer);
			el.active   = c.value("active",   el.active);
			registry.emplace_or_replace<UIElementComponent>(entity, el);
		}
		if (comps.contains("uitext"))
		{
			const json& c = comps["uitext"];
			UITextComponent t2;
			t2.text     = c.value("text",     t2.text);
			t2.fontSize = c.value("fontSize", t2.fontSize);
			t2.color    = jsonToVec4(c.value("color", json()), t2.color);
			registry.emplace_or_replace<UITextComponent>(entity, std::move(t2));
		}
		if (comps.contains("uiimage"))
		{
			const json& c = comps["uiimage"];
			UIImageComponent img;
			img.materialAssetId = jsonToUuid(c.value("material", json()));
			img.tint            = jsonToVec4(c.value("tint", json()), img.tint);
			registry.emplace_or_replace<UIImageComponent>(entity, img);
		}
		if (comps.contains("uibutton"))
		{
			const json& c = comps["uibutton"];
			UIButtonComponent btn;
			btn.normalColor      = jsonToVec4(c.value("normalColor",  json()), btn.normalColor);
			btn.hoveredColor     = jsonToVec4(c.value("hoveredColor", json()), btn.hoveredColor);
			btn.pressedColor     = jsonToVec4(c.value("pressedColor", json()), btn.pressedColor);
			btn.onClickFunction  = c.value("onClickFunction", btn.onClickFunction);
			registry.emplace_or_replace<UIButtonComponent>(entity, std::move(btn));
		}
	}

	// ── JSON → Scene ─────────────────────────────────────────────────────────
	bool applySceneJson(HorizonWorld& world, const json& scene)
	{
		// Level script (scene-wide HorizonCode graph). Load it before the early
		// return so an entity-less scene still restores its script.
		if (scene.contains("levelScript"))
			world.setLevelScriptJson(scene["levelScript"].dump());

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

	// Additive variant: creates ALL entities fresh (including the loaded scene's
	// root, which is parented to world.rootEntity() by createEntity). The loaded
	// scene's children are grafted under the existing world root without clearing it.
	bool applyAdditiveJson(HorizonWorld& world, const json& scene,
	                       std::vector<Entity>* outCreated = nullptr)
	{
		if (!scene.contains("entities")) return true;

		std::unordered_map<uint32_t, Entity> idMap;
		auto& registry = world.registry();
		constexpr uint32_t kNullId = 0xFFFFFFFFu;

		// Pass 1: create all entities fresh (no root remapping)
		for (auto& eJson : scene["entities"])
		{
			uint32_t    serialId = eJson.value("id",   0u);
			std::string name     = eJson.value("name", "Entity");
			uint32_t    parent   = eJson.value("parent", kNullId);

			Entity e;
			if (parent == kNullId)
				// This was the source scene's root; create a fresh sub-root.
				// createEntity() parents it to world.rootEntity() automatically.
				e = world.createEntity(name);
			else
				e = world.createEntity(name);

			idMap[serialId] = e;
			if (outCreated) outCreated->push_back(e);

			if (eJson.contains("components"))
				applyComponents(registry, e, eJson["components"]);
		}

		// Pass 2: rebuild hierarchy (only within the newly loaded entities)
		for (auto& eJson : scene["entities"])
		{
			if (!eJson.contains("children")) continue;

			uint32_t serialId = eJson.value("id", 0u);
			auto it = idMap.find(serialId);
			if (it == idMap.end()) continue;

			Entity parent = it->second;
			auto*  pHier  = registry.try_get<HierarchyComponent>(parent);
			if (!pHier) continue;

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

		world.ensureEnvironmentLights();
		world.markHierarchyDirty();
		return true;
	}

	// ── Prefab helpers (placed after applyComponents so they can call it) ────
	void collectSubtree(entt::registry& registry, Entity root,
	                    std::vector<Entity>& out)
	{
		out.push_back(root);
		if (auto* hier = registry.try_get<HierarchyComponent>(root))
			for (Entity child : hier->children)
				collectSubtree(registry, child, out);
	}

	json buildSubtreeJson(HorizonWorld& world, Entity root)
	{
		auto& registry = world.registry();

		std::vector<Entity> entities;
		collectSubtree(registry, root, entities);

		std::unordered_map<uint32_t, uint32_t> idMap;
		for (size_t i = 0; i < entities.size(); ++i)
			idMap[static_cast<uint32_t>(entities[i])] = static_cast<uint32_t>(i);

		json scene;
		scene["version"] = "1.1";

		json entArray = json::array();
		for (Entity entity : entities)
		{
			uint32_t seqId = idMap.at(static_cast<uint32_t>(entity));
			std::string name = "Entity";
			if (auto* n = registry.try_get<NameComponent>(entity))
				name = n->name;

			json eJson;
			eJson["id"]   = seqId;
			eJson["name"] = name;

			if (auto* hier = registry.try_get<HierarchyComponent>(entity))
			{
				if (hier->parent != entt::null)
				{
					auto pit = idMap.find(static_cast<uint32_t>(hier->parent));
					if (pit != idMap.end())
						eJson["parent"] = pit->second;
				}
				json children = json::array();
				for (Entity child : hier->children)
				{
					auto cit = idMap.find(static_cast<uint32_t>(child));
					if (cit != idMap.end())
						children.push_back(cit->second);
				}
				if (!children.empty())
					eJson["children"] = children;
			}

			json comps = serializeComponents(registry, entity);
			if (!comps.is_null())
				eJson["components"] = comps;

			entArray.push_back(eJson);
		}
		scene["entities"] = entArray;
		return scene;
	}

	Entity applyPrefabJson(HorizonWorld& world, const json& scene, Entity prefabParent)
	{
		if (!scene.contains("entities")) return entt::null;

		constexpr uint32_t kNullId = 0xFFFFFFFFu;
		std::unordered_map<uint32_t, Entity> idMap;
		auto& registry = world.registry();
		Entity prefabRoot = entt::null;

		for (auto& eJson : scene["entities"])
		{
			uint32_t    seqId  = eJson.value("id",   0u);
			std::string name   = eJson.value("name", "Entity");
			uint32_t    parent = eJson.value("parent", kNullId);

			Entity e = world.createEntity(name);
			idMap[seqId] = e;

			if (parent == kNullId)
				prefabRoot = e;

			if (eJson.contains("components"))
				applyComponents(registry, e, eJson["components"]);
		}

		if (prefabRoot == entt::null) return entt::null;

		for (auto& eJson : scene["entities"])
		{
			if (!eJson.contains("children")) continue;

			uint32_t seqId = eJson.value("id", 0u);
			auto it = idMap.find(seqId);
			if (it == idMap.end()) continue;

			Entity parent = it->second;
			auto* pHier = registry.try_get<HierarchyComponent>(parent);
			if (!pHier) continue;

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

		Entity targetParent = (prefabParent != entt::null) ? prefabParent : world.rootEntity();
		world.reparentEntity(prefabRoot, targetParent);
		world.markHierarchyDirty();
		return prefabRoot;
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

bool SceneSerializer::loadAdditive(HorizonWorld& world,
                                    const std::filesystem::path& path,
                                    SerializeFormat format,
                                    std::vector<Entity>* outCreated)
{
    if (format == SerializeFormat::Binary)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) return false;
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                          std::istreambuf_iterator<char>());
        json scene = json::from_cbor(bytes, true, false);
        if (scene.is_discarded()) return false;
        return applyAdditiveJson(world, scene, outCreated);
    }
    // Default: JSON
    std::ifstream in(path);
    if (!in.is_open()) return false;
    json scene = json::parse(in, nullptr, false);
    if (scene.is_discarded()) return false;
    return applyAdditiveJson(world, scene, outCreated);
}

bool SceneSerializer::loadAdditiveFromMemory(HorizonWorld& world,
                                             const std::vector<uint8_t>& data,
                                             std::vector<Entity>* outCreated)
{
    json scene = json::from_cbor(data, true, false);
    if (scene.is_discarded()) return false;
    return applyAdditiveJson(world, scene, outCreated);
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

// ── Prefab API ────────────────────────────────────────────────────────────────
std::vector<uint8_t> SceneSerializer::serializeSubtree(const HorizonWorld& world, Entity root)
{
    json scene = buildSubtreeJson(const_cast<HorizonWorld&>(world), root);
    return json::to_cbor(scene);
}

Entity SceneSerializer::instantiatePrefab(HorizonWorld& world,
                                          const std::vector<uint8_t>& data,
                                          Entity parent)
{
    json scene = json::from_cbor(data, true, false);
    if (scene.is_discarded()) return entt::null;
    return applyPrefabJson(world, scene, parent);
}
