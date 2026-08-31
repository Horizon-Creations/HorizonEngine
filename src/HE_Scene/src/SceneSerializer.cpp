#include "HorizonScene/SceneSerializer.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/Components/NameComponent.h"
#include "HorizonScene/Components/HierarchyComponent.h"
#include "HorizonScene/Components/TransformComponent.h"
#include "HorizonScene/Components/Transform2DComponent.h"
#include "HorizonScene/Components/MeshComponent.h"
#include "HorizonScene/Components/MaterialComponent.h"
#include "HorizonScene/Components/CameraComponent.h"
#include "HorizonScene/Components/CameraRigComponent.h"
#include "HorizonScene/Components/MovementComponent.h"
#include "HorizonScene/Components/LightComponent.h"
#include "HorizonScene/Components/DecalComponent.h"
#include "HorizonScene/Components/RigidBodyComponent.h"
#include "HorizonScene/Components/ColliderComponent.h"
#include "HorizonScene/Components/CharacterControllerComponent.h"
#include "HorizonScene/Components/ScriptComponent.h"
#include "HorizonScene/Components/SaveStateComponent.h"
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
#include "HorizonScene/Components/AnimatorComponent.h"
#include "HorizonScene/Components/AnimatorBlendComponent.h"
#include "HorizonScene/Components/SkeletalMeshComponent.h"
#include "HorizonScene/Components/PropertyAnimatorComponent.h"
#include "HorizonScene/Components/NavMeshComponent.h"
#include "HorizonScene/Components/NavAgentComponent.h"
#include "HorizonScene/NavigationSystem.h"
#include "HorizonScene/EngineApi.h"   // HE_ENV_FIELDS_* — the EnvironmentComponent field list
#include <Diagnostics/Log.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <cstring>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

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

	// An entity's stable identity. Every entity gets one in
	// HorizonWorld::createEntity, so the fallback only fires for an entity built
	// by some path that bypassed it — minting one on the spot keeps the file
	// valid rather than writing a zero id that would later collide with every
	// other zero id.
	HE::UUID entityUuid(entt::registry& registry, Entity entity)
	{
		if (auto* c = registry.try_get<EntityIdComponent>(entity)) return c->id;
		const HE::UUID fresh = HE::UUID::generate();
		registry.emplace<EntityIdComponent>(entity, EntityIdComponent{ fresh });
		return fresh;
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

	// ── Field lists shared by both sides of the round-trip ───────────────────
	// A component whose two halves are mechanical (write field → read field back)
	// is described ONCE and both halves are generated from that list, so a new
	// field can no longer be added to the writer and forgotten in the reader.
	//
	// EnvironmentComponent reuses HE_ENV_FIELDS_* from EngineApi.h — the same list
	// that generates its scripting API — with exactly one exception: `flash` is the
	// WeatherSystem's lightning strobe, rewritten every frame during a storm. It
	// has never been in the scene file and must stay out, or a reloaded scene would
	// start frozen mid-strike (test: "Lightning flash is deliberately runtime-only
	// and is not persisted").
	constexpr bool envFieldPersisted(std::string_view field) { return field != "flash"; }

	// NavMeshComponent::config — a flat block of bake parameters, 1:1 on both sides.
#define HE_NAVCFG_FIELDS(X) \
	X(cellSize) X(cellHeight) X(walkableHeight) X(walkableClimb) \
	X(walkableRadius) X(maxSlope) X(maxEdgeLen) X(maxSimplification) \
	X(minRegionArea) X(mergeRegionArea) X(detailSampleDist) X(detailMaxError)

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
		if (auto* mv = registry.try_get<MovementComponent>(entity))
		{
			// Config only. moveInput/lookYaw/lookPitch are this frame's intent
			// and are cleared at the end of it — saving them would restore a
			// character mid-step.
			comps["movement"] = {
				{ "maxSpeed",         mv->maxSpeed },
				{ "turnRate",         mv->turnRate },
				{ "orientToMovement", mv->orientToMovement },
			};
		}
		if (auto* rig = registry.try_get<CameraRigComponent>(entity))
		{
			// yaw/pitch are runtime state, but they are also the camera's
			// starting direction on load — dropping them would snap every
			// reloaded scene back to due north. meshHiddenByRig is NOT saved:
			// it says what the rig did to a mesh this session, and a fresh
			// session has done nothing yet.
			comps["cameraRig"] = {
				{ "mode",           static_cast<uint8_t>(rig->mode) },
				{ "target",         uuidToJson(rig->target) },
				{ "pivotOffset",    vec3ToJson(rig->pivotOffset) },
				{ "armOffset",      vec3ToJson(rig->armOffset) },
				{ "armLength",      rig->armLength },
				{ "yaw",            rig->yaw },
				{ "pitch",          rig->pitch },
				{ "sensitivity",    rig->sensitivity },
				{ "stickSensitivity", rig->stickSensitivity },
				{ "stickInvertY",     rig->stickInvertY },
				{ "pitchMin",       rig->pitchMin },
				{ "pitchMax",       rig->pitchMax },
				{ "targetYaw",       static_cast<uint8_t>(rig->targetYaw) },
				{ "hideTargetMesh",  rig->hideTargetMesh },
				{ "collision",       rig->collision },
				{ "collisionRadius", rig->collisionRadius },
			};
		}
		if (auto* l = registry.try_get<LightComponent>(entity))
		{
			comps["light"] = {
				{ "type",         static_cast<uint8_t>(l->type) },
				{ "color",        vec3ToJson(l->color) },
				{ "intensity",    l->intensity },
				{ "range",        l->range },
				{ "spotAngle",    l->spotAngle },
				{ "cullDistance", l->cullDistance },
				{ "visible",      l->visible },
				{ "castsShadow",  l->castsShadow },
			};
		}
		if (auto* d = registry.try_get<DecalComponent>(entity))
		{
			comps["decal"] = {
				{ "color",     { d->color.r, d->color.g, d->color.b, d->color.a } },
				{ "roughness", d->roughness },
				{ "texture",   uuidToJson(d->textureId) },
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
			// Authored fields only. velocity/isGrounded/airTime are runtime state
			// written by the physics step and are meaningless in a scene file.
			// `jumpSpeed` belongs here with gravity: it is the other half of the
			// jump the author tuned, and without it every packaged game would
			// jump at the 5 m/s default no matter what the Details panel showed.
			comps["characterController"] = {
				{ "slopeLimit", cc->slopeLimit },
				{ "stepHeight", cc->stepHeight },
				{ "skinWidth",  cc->skinWidth  },
				{ "mass",       cc->mass       },
				{ "gravity",    cc->gravity    },
				{ "jumpSpeed",  cc->jumpSpeed  },
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
		if (auto* ss = registry.try_get<SaveStateComponent>(entity))
		{
			comps["saveState"] = {
				{ "enabled",        ss->enabled },
				{ "saveTransform",  ss->saveTransform },
				{ "saveVisibility", ss->saveVisibility },
			};
		}
		if (auto* e = registry.try_get<EnvironmentComponent>(entity))
		{
			// One key per HE_ENV_FIELDS_* row (see envFieldPersisted above). Key
			// names are the member names, exactly as the hand-written block wrote
			// them — the on-disk format is unchanged.
			json env = json::object();
#define HE_ENV_SER_PLAIN(m, Name, disp) if (envFieldPersisted(#m)) env[#m] = e->m;
#define HE_ENV_SER_COLOR(m, Name, disp) if (envFieldPersisted(#m)) env[#m] = vec3ToJson(e->m);
			HE_ENV_FIELDS_FLOAT(HE_ENV_SER_PLAIN)
			HE_ENV_FIELDS_BOOL (HE_ENV_SER_PLAIN)
			HE_ENV_FIELDS_INT  (HE_ENV_SER_PLAIN)
			HE_ENV_FIELDS_COLOR(HE_ENV_SER_COLOR)
#undef HE_ENV_SER_PLAIN
#undef HE_ENV_SER_COLOR
			comps["environment"] = std::move(env);
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
				{ "uvTiling",   t->uvTiling },
				// Authored LOD aggressiveness — was editable but never persisted,
				// so it silently reverted to 1 on every reload.
				{ "lodDistanceScale", t->lodDistanceScale },
			};
			// Painted layer weights (RGBA8). Same base64 treatment as the
			// heights: a JSON array of N bytes dominates the undo snapshot.
			if (!t->layerWeights.empty())
			{
				tc["weightRes"]     = t->weightRes;
				tc["layerWeightsB64"] = base64Encode(t->layerWeights.data(),
				                                     t->layerWeights.size());
			}
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
				{ "particleAsset",       uuidToJson(ps->particleAssetId) },
				{ "visible",             ps->visible },
				{ "playing",             ps->playing },
				{ "destroyWhenFinished", ps->destroyWhenFinished },
			};
		}
		if (auto* sk = registry.try_get<SkeletalMeshComponent>(entity))
		{
			comps["skeletalmesh"] = {
				{ "mesh",           uuidToJson(sk->meshAssetId) },
				{ "visible",        sk->visible },
				{ "castsShadow",    sk->castsShadow },
				{ "receivesShadow", sk->receivesShadow },
			};
		}
		if (auto* an = registry.try_get<AnimatorComponent>(entity))
		{
			comps["animator"] = {
				{ "clip",          uuidToJson(an->clipAssetId) },
				{ "playbackTime",  an->playbackTime },
				{ "playbackSpeed", an->playbackSpeed },
				{ "looping",       an->looping },
				{ "playing",       an->playing },
			};
		}
		if (auto* ab = registry.try_get<AnimatorBlendComponent>(entity))
		{
			comps["animatorblend"] = {
				{ "clipA",         uuidToJson(ab->clipAId) },
				{ "clipB",         uuidToJson(ab->clipBId) },
				{ "blendAlpha",    ab->blendAlpha },
				{ "playbackTime",  ab->playbackTime },
				{ "playbackSpeed", ab->playbackSpeed },
				{ "looping",       ab->looping },
				{ "playing",       ab->playing },
			};
		}
		if (auto* pa = registry.try_get<PropertyAnimatorComponent>(entity))
		{
			comps["propertyanimator"] = {
				{ "clip",          uuidToJson(pa->clipId) },
				{ "playbackTime",  pa->playbackTime },
				{ "playbackSpeed", pa->playbackSpeed },
				{ "looping",       pa->looping },
				{ "playing",       pa->playing },
			};
		}
		if (auto* nm = registry.try_get<NavMeshComponent>(entity))
		{
			json cfg = json::object();
#define HE_NAVCFG_SER(m) cfg[#m] = nm->config.m;
			HE_NAVCFG_FIELDS(HE_NAVCFG_SER)
#undef HE_NAVCFG_SER
			json nmJson;
			nmJson["config"] = std::move(cfg);
			// Baked navMesh/navQuery are runtime-only (re-baked on load, see
			// applyComponents) — only the source geometry needs to survive the
			// round-trip, as base64 blobs like terrain's sculptHeights (a JSON
			// array of thousands of floats/ints would otherwise dominate the file).
			if (!nm->geometry.verts.empty())
				nmJson["geometry"]["vertsB64"] = base64Encode(
					reinterpret_cast<const uint8_t*>(nm->geometry.verts.data()),
					nm->geometry.verts.size() * sizeof(float));
			if (!nm->geometry.tris.empty())
				nmJson["geometry"]["trisB64"] = base64Encode(
					reinterpret_cast<const uint8_t*>(nm->geometry.tris.data()),
					nm->geometry.tris.size() * sizeof(int));
			comps["navmesh"] = std::move(nmJson);
		}
		if (auto* na = registry.try_get<NavAgentComponent>(entity))
		{
			// Authored fields only. path/pathIdx/hasPath/moving are runtime state
			// and are rebuilt every session — writing `moving` out would freeze
			// whatever the agent happened to be doing when the scene was saved
			// into the scene file. `autoStart` is the authored half of it, and
			// the reason an agent moves at all in a packaged game: the "Go"
			// button that used to be the only writer of `moving` is an editor
			// control and does not exist there.
			comps["navagent"] = {
				{ "targetPos",    vec3ToJson(na->targetPos) },
				{ "speed",        na->speed },
				{ "stoppingDist", na->stoppingDist },
				{ "autoStart",    na->autoStart },
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
			// Identity is the entity's UUID, not its entt handle, and the hierarchy
			// links reference UUIDs too. Writing the handle would make the file
			// merge-hostile: handles are dense allocator indices, so two people who
			// each add an entity on their own branch both get the same number, the
			// added JSON blocks do not overlap textually, and git merges them into
			// one file with two entities claiming one identity — silently
			// re-parenting anything that referenced it. See EntityIdComponent.h.
			eJson["uuid"] = uuidToJson(entityUuid(registry, entity));
			eJson["name"] = registry.get<NameComponent>(entity).name;

			if (auto* hier = registry.try_get<HierarchyComponent>(entity))
			{
				// JSON null for "no parent" — that is how the loader finds the root,
				// and it is unambiguous in a way a sentinel number is not.
				eJson["parent"] = (hier->parent == entt::null)
					? json(nullptr)
					: uuidToJson(entityUuid(registry, hier->parent));
				json children = json::array();
				for (auto child : hier->children)
					if (!registry.all_of<EnvironmentLightComponent>(child) &&    // omit built-ins
					    !registry.all_of<TerrainChunkComponent>(child))          // + terrain chunks
						children.push_back(uuidToJson(entityUuid(registry, child)));
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
	// Every component key applyComponents below knows how to restore. A scene
	// carrying anything else is silently losing data — usually a component that
	// was added to the save path but not the load path, or a scene written by a
	// newer build. Reported once per unknown key rather than per entity.
	void warnUnknownComponents(const json& comps)
	{
		if (!comps.is_object()) return;
		static std::mutex                   s_mutex;
		static std::unordered_set<std::string> s_reported;
		for (auto it = comps.begin(); it != comps.end(); ++it)
		{
			if (SceneSerializer::isKnownComponentKey(it.key())) continue;
			std::lock_guard<std::mutex> lk(s_mutex);
			if (!s_reported.insert(it.key()).second) continue;
			HE_LOG_WARN(Serialize, "Scene contains unknown component '%s' — it is being "
			                       "dropped on load (scene written by a newer build, or a "
			                       "save/load mismatch)", it.key().c_str());
		}
	}

	// The same courtesy for the collider shape, which is a raw uint8 in the file.
	// The APPEND-ONLY rule in Types/Enums.h protects the BACKWARDS direction (an
	// old scene keeps meaning what it meant); it says nothing about the forwards
	// one, which is the direction that bites here. An old build reading a scene
	// that uses Mesh/Convex Hull/Height Field (3..5, added with the runtime-shapes
	// work) cast the value straight through, so every one of them landed as
	// Box == 0: the imported house collided as a crate again, with nothing in the
	// log to point at. Box is still the only answer — there is no other shape to
	// fall back to — but it is now said out loud, and it matches what physics does
	// with a shape it does not know (buildColliderShape's default branch logs and
	// builds a box). Reported once per unknown value, not once per entity.
	ColliderShape jsonToColliderShape(uint8_t raw, ColliderShape fallback)
	{
		if (raw <= static_cast<uint8_t>(ColliderShape::HeightField))
			return static_cast<ColliderShape>(raw);

		static std::mutex                s_mutex;
		static std::unordered_set<uint8_t> s_reported;
		{
			std::lock_guard<std::mutex> lk(s_mutex);
			if (!s_reported.insert(raw).second) return fallback;
		}
		HE_LOG_WARN(Serialize, "Scene contains unknown collider shape %u — loading it as a Box "
		                       "(scene written by a newer build; SAVING IT BACK MAKES THAT "
		                       "PERMANENT)", static_cast<unsigned>(raw));
		return fallback;
	}

	void applyComponents(entt::registry& registry, Entity entity, const json& comps)
	{
		warnUnknownComponents(comps);
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
		if (comps.contains("movement"))
		{
			const json& c = comps["movement"];
			MovementComponent mv;
			mv.maxSpeed         = c.value("maxSpeed",         mv.maxSpeed);
			mv.turnRate         = c.value("turnRate",         mv.turnRate);
			mv.orientToMovement = c.value("orientToMovement", mv.orientToMovement);
			registry.emplace_or_replace<MovementComponent>(entity, mv);
		}
		if (comps.contains("cameraRig"))
		{
			const json& c = comps["cameraRig"];
			CameraRigComponent rig;
			rig.mode           = static_cast<CameraRigComponent::Mode>(
			                         c.value("mode", static_cast<uint8_t>(rig.mode)));
			rig.target         = jsonToUuid(c.value("target", json()));
			rig.pivotOffset    = jsonToVec3(c.value("pivotOffset", json()), rig.pivotOffset);
			rig.armOffset      = jsonToVec3(c.value("armOffset",   json()), rig.armOffset);
			rig.armLength      = c.value("armLength",      rig.armLength);
			rig.yaw            = c.value("yaw",            rig.yaw);
			rig.pitch          = c.value("pitch",          rig.pitch);
			rig.sensitivity    = c.value("sensitivity",    rig.sensitivity);
			rig.stickSensitivity = c.value("stickSensitivity", rig.stickSensitivity);
			rig.stickInvertY     = c.value("stickInvertY",     rig.stickInvertY);
			rig.pitchMin       = c.value("pitchMin",       rig.pitchMin);
			rig.pitchMax       = c.value("pitchMax",       rig.pitchMax);
			rig.targetYaw      = static_cast<CameraRigComponent::TargetYaw>(
			                         c.value("targetYaw", static_cast<uint8_t>(rig.targetYaw)));
			rig.hideTargetMesh  = c.value("hideTargetMesh",  rig.hideTargetMesh);
			rig.collision       = c.value("collision",       rig.collision);
			rig.collisionRadius = c.value("collisionRadius", rig.collisionRadius);
			registry.emplace_or_replace<CameraRigComponent>(entity, rig);
		}
		if (comps.contains("light"))
		{
			const json& c = comps["light"];
			LightComponent l;
			l.type         = static_cast<LightType>(c.value("type", static_cast<uint8_t>(l.type)));
			l.color        = jsonToVec3(c.value("color", json()), l.color);
			l.intensity    = c.value("intensity",    l.intensity);
			l.range        = c.value("range",        l.range);
			l.spotAngle    = c.value("spotAngle",    l.spotAngle);
			l.cullDistance = c.value("cullDistance", l.cullDistance);
			l.visible      = c.value("visible",      l.visible);
			l.castsShadow  = c.value("castsShadow",  l.castsShadow);
			registry.emplace_or_replace<LightComponent>(entity, l);
		}
		if (comps.contains("decal"))
		{
			const json& c = comps["decal"];
			DecalComponent d;
			if (auto col = c.find("color"); col != c.end() && col->is_array() && col->size() >= 4)
				d.color = glm::vec4((*col)[0].get<float>(), (*col)[1].get<float>(),
				                    (*col)[2].get<float>(), (*col)[3].get<float>());
			d.roughness = c.value("roughness", d.roughness);
			d.textureId = jsonToUuid(c.value("texture", json()));
			registry.emplace_or_replace<DecalComponent>(entity, d);
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
			col.shape     = jsonToColliderShape(c.value("shape", static_cast<uint8_t>(col.shape)),
			                                    col.shape);
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
			// Defaulted from the fresh component, so a scene written before this
			// field existed loads with the 5 m/s default rather than a zero that
			// would silently refuse every jump.
			cc.jumpSpeed  = c.value("jumpSpeed",  cc.jumpSpeed);
			registry.emplace_or_replace<CharacterControllerComponent>(entity, cc);
		}
		if (comps.contains("saveState"))
		{
			const json& c = comps["saveState"];
			SaveStateComponent ss;
			ss.enabled        = c.value("enabled",        ss.enabled);
			ss.saveTransform  = c.value("saveTransform",  ss.saveTransform);
			ss.saveVisibility = c.value("saveVisibility", ss.saveVisibility);
			registry.emplace_or_replace<SaveStateComponent>(entity, ss);
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
			// Same field list as the writer (see envFieldPersisted): a missing key
			// keeps the component default, exactly as the hand-written block did.
#define HE_ENV_APPLY_PLAIN(m, Name, disp) if (envFieldPersisted(#m)) e.m = c.value(#m, e.m);
#define HE_ENV_APPLY_COLOR(m, Name, disp) if (envFieldPersisted(#m)) e.m = jsonToVec3(c.value(#m, json()), e.m);
			HE_ENV_FIELDS_FLOAT(HE_ENV_APPLY_PLAIN)
			HE_ENV_FIELDS_BOOL (HE_ENV_APPLY_PLAIN)
			HE_ENV_FIELDS_INT  (HE_ENV_APPLY_PLAIN)
			HE_ENV_FIELDS_COLOR(HE_ENV_APPLY_COLOR)
#undef HE_ENV_APPLY_PLAIN
#undef HE_ENV_APPLY_COLOR
			// nebulaQuality (0/1/2) replaced the old nebulaHighFidelity bool — fall back to it
			// for scenes saved before the change (true → High=1, false → Performance=0).
			if (!c.contains("nebulaQuality") && c.contains("nebulaHighFidelity"))
				e.nebulaQuality = c.value("nebulaHighFidelity", true) ? 1 : 0;
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
			t.uvTiling    = c.value("uvTiling",     t.uvTiling);
			t.lodDistanceScale = c.value("lodDistanceScale", t.lodDistanceScale);
			t.weightRes   = c.value("weightRes",    t.weightRes);
			if (c.contains("layerWeightsB64") && c["layerWeightsB64"].is_string())
			{
				t.layerWeights = base64Decode(c["layerWeightsB64"].get<std::string>());
				// A truncated/mismatched blob would index out of bounds when painted
				// or uploaded — drop it rather than carry a half-sized weightmap.
				if (t.layerWeights.size() != static_cast<size_t>(t.weightRes) * t.weightRes * 4)
					t.layerWeights.clear();
				t.weightsDirty = !t.layerWeights.empty();
			}
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
			// Absent in every scene saved before one-shot effects existed, and
			// the default (false) is what those scenes meant: an emitter that has
			// always been scenery must not start deleting itself.
			ps.destroyWhenFinished = c.value("destroyWhenFinished", ps.destroyWhenFinished);
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
		if (comps.contains("skeletalmesh"))
		{
			const json& c = comps["skeletalmesh"];
			SkeletalMeshComponent sk;
			sk.meshAssetId    = jsonToUuid(c.value("mesh", json()));
			sk.visible        = c.value("visible",        sk.visible);
			sk.castsShadow    = c.value("castsShadow",    sk.castsShadow);
			sk.receivesShadow = c.value("receivesShadow", sk.receivesShadow);
			registry.emplace_or_replace<SkeletalMeshComponent>(entity, std::move(sk));
		}
		if (comps.contains("animator"))
		{
			const json& c = comps["animator"];
			AnimatorComponent an;
			an.clipAssetId   = jsonToUuid(c.value("clip", json()));
			an.playbackTime  = c.value("playbackTime",  an.playbackTime);
			an.playbackSpeed = c.value("playbackSpeed", an.playbackSpeed);
			an.looping       = c.value("looping",       an.looping);
			an.playing       = c.value("playing",       an.playing);
			registry.emplace_or_replace<AnimatorComponent>(entity, an);
		}
		if (comps.contains("animatorblend"))
		{
			const json& c = comps["animatorblend"];
			AnimatorBlendComponent ab;
			ab.clipAId       = jsonToUuid(c.value("clipA", json()));
			ab.clipBId       = jsonToUuid(c.value("clipB", json()));
			ab.blendAlpha    = c.value("blendAlpha",    ab.blendAlpha);
			ab.playbackTime  = c.value("playbackTime",  ab.playbackTime);
			ab.playbackSpeed = c.value("playbackSpeed", ab.playbackSpeed);
			ab.looping       = c.value("looping",       ab.looping);
			ab.playing       = c.value("playing",       ab.playing);
			registry.emplace_or_replace<AnimatorBlendComponent>(entity, ab);
		}
		if (comps.contains("propertyanimator"))
		{
			const json& c = comps["propertyanimator"];
			PropertyAnimatorComponent pa;
			pa.clipId        = jsonToUuid(c.value("clip", json()));
			pa.playbackTime  = c.value("playbackTime",  pa.playbackTime);
			pa.playbackSpeed = c.value("playbackSpeed", pa.playbackSpeed);
			pa.looping       = c.value("looping",       pa.looping);
			pa.playing       = c.value("playing",       pa.playing);
			registry.emplace_or_replace<PropertyAnimatorComponent>(entity, pa);
		}
		if (comps.contains("navmesh"))
		{
			const json& c = comps["navmesh"];
			NavMeshComponent nm;
			if (c.contains("config"))
			{
				const json& cc = c["config"];
#define HE_NAVCFG_APPLY(m) nm.config.m = cc.value(#m, nm.config.m);
				HE_NAVCFG_FIELDS(HE_NAVCFG_APPLY)
#undef HE_NAVCFG_APPLY
			}
			if (c.contains("geometry"))
			{
				const json& gj = c["geometry"];
				if (gj.contains("vertsB64") && gj["vertsB64"].is_string())
				{
					const std::vector<uint8_t> bytes = base64Decode(gj["vertsB64"].get<std::string>());
					nm.geometry.verts.resize(bytes.size() / sizeof(float));
					if (!nm.geometry.verts.empty())
						std::memcpy(nm.geometry.verts.data(), bytes.data(),
						            nm.geometry.verts.size() * sizeof(float));
				}
				if (gj.contains("trisB64") && gj["trisB64"].is_string())
				{
					const std::vector<uint8_t> bytes = base64Decode(gj["trisB64"].get<std::string>());
					nm.geometry.tris.resize(bytes.size() / sizeof(int));
					if (!nm.geometry.tris.empty())
						std::memcpy(nm.geometry.tris.data(), bytes.data(),
						            nm.geometry.tris.size() * sizeof(int));
				}
			}
			// navMesh/navQuery weren't persisted (see serializeComponents) — re-bake
			// immediately from the restored geometry so a loaded scene has a working
			// NavMesh without requiring a manual re-bake in the editor.
			if (!nm.geometry.verts.empty() && !nm.geometry.tris.empty())
				NavigationSystem::bake(nm);
			registry.emplace_or_replace<NavMeshComponent>(entity, std::move(nm));
		}
		if (comps.contains("navagent"))
		{
			const json& c = comps["navagent"];
			NavAgentComponent na;
			na.targetPos    = jsonToVec3(c.value("targetPos", json()), na.targetPos);
			na.speed        = c.value("speed",        na.speed);
			na.stoppingDist = c.value("stoppingDist", na.stoppingDist);
			na.autoStart    = c.value("autoStart",    na.autoStart);
			// `moving` deliberately stays false here. Loading a scene is not
			// starting one — the editor loads for editing, and an agent that
			// began walking on load would rewrite its authored position before
			// anyone pressed Play. NavigationSystem turns autoStart into moving
			// once the simulation is running, in both applications.
			registry.emplace_or_replace<NavAgentComponent>(entity, na);
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
						// Same guard the asset reader uses (see the comment on
						// HE::transitionOpFromInt): this is user JSON, so an op
						// this build has no enumerator for must not reach the
						// system's switch().
						t.op        = HE::transitionOpFromInt(tj.value("op", 0));
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
#undef HE_NAVCFG_FIELDS

	// ── Entity addressing, old format and new ────────────────────────────────
	// Scenes written before stable ids address entities by their uint32 entt
	// handle; scenes written since address them by UUID, and prefab blobs still
	// use handles (a prefab is a self-contained template whose ids are already
	// normalised to 0..n-1 by buildSubtreeJson, and it is never git-merged).
	//
	// Rather than duplicating all three load paths once per format, both are
	// folded into one key: a legacy handle becomes {hi = 0, lo = handle}.
	// HE::UUID::generate() always sets the RFC 4122 version bit in `hi`, so a
	// real UUID can never have hi == 0 and the two spaces cannot collide.
	HE::UUID legacyKey(uint32_t handle) { return HE::UUID{ 0, handle }; }

	// The key this record is addressed by, in whichever format the file uses.
	HE::UUID entityKeyOf(const json& eJson)
	{
		if (auto it = eJson.find("uuid"); it != eJson.end())
			return jsonToUuid(*it);
		return legacyKey(eJson.value("id", 0u));
	}

	// A reference to another entity — the "parent" field or one "children" entry.
	// False means "no reference": JSON null in the new format, or the 0xFFFFFFFF
	// sentinel in the old one.
	bool entityRefOf(const json& v, HE::UUID& out)
	{
		constexpr uint32_t kNullHandle = 0xFFFFFFFFu; // static_cast<uint32_t>(entt::null)
		if (v.is_null()) return false;
		if (v.is_array())
		{
			out = jsonToUuid(v);
			return out != HE::UUID{};
		}
		if (v.is_number_unsigned())
		{
			const uint32_t h = v.get<uint32_t>();
			if (h == kNullHandle) return false;
			out = legacyKey(h);
			return true;
		}
		return false;
	}

	// Only the new format carries an identity worth restoring. Legacy records
	// have none, so those entities keep the id minted at creation.
	bool hasStoredUuid(const json& eJson) { return eJson.contains("uuid"); }

	// ── Pass 2 of every load path: rebuild parent/child links ────────────────
	// Shared verbatim by the full-scene, additive and prefab loads — they differ
	// only in how pass 1 created the entities, not in how the links are restored.
	// Entities missing from idMap (or from the registry's hierarchy) are skipped,
	// so a partial/hand-edited scene still loads.
	void rebuildHierarchy(entt::registry& registry, const json& scene,
	                      const std::unordered_map<HE::UUID, Entity>& idMap)
	{
		for (const auto& eJson : scene["entities"])
		{
			if (!eJson.contains("children")) continue;

			auto it = idMap.find(entityKeyOf(eJson));
			if (it == idMap.end()) continue;

			Entity parent = it->second;
			auto*  pHier  = registry.try_get<HierarchyComponent>(parent);
			if (!pHier) continue;

			// Clear the children list rebuilt during createEntity — restore exact order
			pHier->children.clear();

			for (const auto& childRef : eJson["children"])
			{
				HE::UUID cid;
				if (!entityRefOf(childRef, cid)) continue;
				auto cit = idMap.find(cid);
				if (cit == idMap.end()) continue;
				Entity child = cit->second;
				pHier->children.push_back(child);
				if (auto* cHier = registry.try_get<HierarchyComponent>(child))
					cHier->parent = parent;
			}
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

		// Serialised key (UUID, or a folded legacy handle) → newly created entity.
		std::unordered_map<HE::UUID, Entity> idMap;
		auto& registry = world.registry();

		// ── Pass 1: create entities, set names, apply components ─────────────
		// The root is the entity with no parent, NOT necessarily the first entry:
		// entt's view iterates in reverse-creation order, so the root (created
		// first) is usually serialised LAST. Map that one to the existing root
		// instead of creating a duplicate; mapping by position renamed the root to
		// whatever happened to be first and shredded the hierarchy on every
		// save/load and undo. (#root-by-parent)
		bool rootMapped = false;
		for (auto& eJson : scene["entities"])
		{
			const HE::UUID key  = entityKeyOf(eJson);
			std::string    name = eJson.value("name", "Entity");
			HE::UUID       parentKey;
			const bool     hasParent =
				eJson.contains("parent") && entityRefOf(eJson["parent"], parentKey);

			// Two records claiming one identity is what a bad merge produces. It
			// cannot be repaired here — we do not know which one the references
			// meant — but it must not pass silently, because the symptom otherwise
			// shows up much later as an object parented to the wrong thing.
			if (idMap.find(key) != idMap.end())
			{
				HE_LOG_ERROR(Serialize,
				             "Scene load: two entities share one id (near '%s') — the file is "
				             "damaged, most likely by a merge. The later one is skipped; "
				             "hierarchy around it may be wrong.",
				             name.c_str());
				continue;
			}

			Entity e;
			if (!rootMapped && !hasParent)
			{
				e = world.rootEntity();
				world.renameEntity(e, name);
				rootMapped = true;
			}
			else
				e = world.createEntity(name);

			// Restore the stored identity. Legacy records have none, so those
			// entities keep the id minted at creation and gain a stable one from
			// the next save onward.
			if (hasStoredUuid(eJson)) world.setEntityId(e, key);

			idMap[key] = e;

			if (eJson.contains("components"))
				applyComponents(registry, e, eJson["components"]);
		}

		// ── Pass 2: rebuild parent/child links ────────────────────────────────
		rebuildHierarchy(registry, scene, idMap);

		// Legacy scenes stored Environment/Weather on the World root; move them onto
		// dedicated Sky/Weather entities so the whole engine sees one uniform model.
		world.migrateLegacyRootEnvironment();
		// Scenes written before collectSubtree skipped engine-generated entities can
		// carry frozen terrain chunks that nothing owns any more — drop them so the
		// TerrainSystem rebuilds the landscape from scratch.
		world.purgeOrphanedGeneratedEntities();
		// Built-in sun/moon lights aren't serialised — recreate / re-attach them under
		// the Sky entity (or tear down strays when the scene has no sky). Also adopts
		// the ordinary "Sun"/"Moon" copies such a scene file carries.
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

		std::unordered_map<HE::UUID, Entity> idMap;
		auto& registry = world.registry();

		// Pass 1: create all entities fresh (no root remapping).
		//
		// The stored UUIDs are deliberately NOT restored here, for the same reason
		// prefab instantiation does not restore them: an additive load grafts a
		// copy of the scene into a world that may already contain one, and
		// restoring would give both copies the same identities. Each graft keeps
		// the ids minted at creation, so it is a distinct instance.
		for (auto& eJson : scene["entities"])
		{
			const HE::UUID key  = entityKeyOf(eJson);
			std::string    name = eJson.value("name", "Entity");

			// createEntity() parents to world.rootEntity() automatically, which is
			// exactly what the source scene's own root needs too.
			Entity e = world.createEntity(name);

			idMap[key] = e;
			if (outCreated) outCreated->push_back(e);

			if (eJson.contains("components"))
				applyComponents(registry, e, eJson["components"]);
		}

		// Pass 2: rebuild hierarchy (only within the newly loaded entities)
		rebuildHierarchy(registry, scene, idMap);

		world.purgeOrphanedGeneratedEntities();
		world.ensureEnvironmentLights();
		world.markHierarchyDirty();
		return true;
	}

	// ── Prefab helpers (placed after applyComponents so they can call it) ────
	// Engine-generated children are skipped for the same reason buildSceneJson
	// skips them: they are recreated from their owner (ensureEnvironmentLights /
	// TerrainSystem) on every machine and every load. Writing them into a blob is
	// actively destructive — neither marker component is serialised, so the copy
	// comes back as an ORDINARY entity that its owner no longer manages: a full-
	// intensity "Sun" the day-night pass never dims (which then outshines the real
	// one and steals the shadow-casting slot), or a frozen duplicate of the
	// landscape. And since that copy IS serialisable, it lands in the scene file
	// and multiplies on every further round-trip.
	bool isEngineGenerated(entt::registry& registry, Entity e)
	{
		return registry.all_of<EnvironmentLightComponent>(e) ||
		       registry.all_of<TerrainChunkComponent>(e);
	}

	void collectSubtree(entt::registry& registry, Entity root,
	                    std::vector<Entity>& out)
	{
		if (isEngineGenerated(registry, root)) return;
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
			// The sequential index stays, because it is what makes a prefab file
			// readable and it costs nothing. The UUID next to it is what makes the
			// blob usable for anything but a template.
			//
			// It was missing, and the omission was invisible for as long as this
			// blob was only ever a prefab: instantiating a template deliberately
			// mints fresh identities, so nothing ever read the field. Then
			// collaboration started sending the same blob over the wire with
			// preserveIds=true — "a peer instantiating another peer's new subtree
			// must end up with the SAME identities, because every later edit
			// travels addressed by them" — and there was nothing to preserve.
			// entityKeyOf fell back to legacyKey(seqId), so the receiver stamped
			// {hi:0, lo:1}, {hi:0, lo:2} … onto the children. Wire ids are taken
			// from uuid.lo, so those children answered to ids 1 and 2 on one
			// machine and to their real uuids on the other: moving an arm of a
			// replicated prefab moved nothing on the far side, deleting it deleted
			// nothing, and two received prefabs gave one editor two entities both
			// claiming id 1 — which then went into its scene file.
			eJson["uuid"] = uuidToJson(entityUuid(registry, entity));
			eJson["id"]   = seqId;
			eJson["name"] = name;

			if (auto* hier = registry.try_get<HierarchyComponent>(entity))
			{
				// Links by UUID for the same reason, since entityKeyOf now answers
				// with one: a map keyed by UUID cannot be searched with an ordinal,
				// so leaving these as seqIds would have restored no hierarchy at all.
				//
				// The parent is still written ONLY when it is inside this subtree,
				// which is the one place this must not copy the whole-scene path.
				// There the parentless record IS the root and maps onto the world
				// root; here the root's real parent lives outside the blob, and
				// naming it would make applyPrefabJson see every record as having a
				// parent, find no root, and return null — every prefab in the
				// project silently refusing to instantiate.
				if (hier->parent != entt::null &&
				    idMap.find(static_cast<uint32_t>(hier->parent)) != idMap.end())
				{
					eJson["parent"] = uuidToJson(entityUuid(registry, hier->parent));
				}
				json children = json::array();
				for (Entity child : hier->children)
				{
					if (idMap.find(static_cast<uint32_t>(child)) == idMap.end())
						continue;   // engine-generated, or otherwise not in this blob
					children.push_back(uuidToJson(entityUuid(registry, child)));
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

	Entity applyPrefabJson(HorizonWorld& world, const json& scene, Entity prefabParent,
	                       bool preserveIds = false)
	{
		if (!scene.contains("entities")) return entt::null;

		std::unordered_map<HE::UUID, Entity> idMap;
		auto& registry = world.registry();
		Entity prefabRoot = entt::null;

		// A prefab is a template, so the ids minted by createEntity are kept and
		// nothing is restored from the blob. That is what makes the same prefab
		// inserted twice produce two entities with two identities rather than one
		// identity claimed twice.
		//
		// `preserveIds` is the one sanctioned exception, for collaboration: a
		// peer instantiating another peer's newly created subtree must end up
		// with the SAME identities, because every later edit to those entities
		// travels addressed by them. It cannot collide the way a double prefab
		// drop would — the subtree exists on the wire exactly once.
		for (auto& eJson : scene["entities"])
		{
			const HE::UUID key  = entityKeyOf(eJson);
			std::string    name = eJson.value("name", "Entity");
			HE::UUID       parentKey;
			const bool     hasParent =
				eJson.contains("parent") && entityRefOf(eJson["parent"], parentKey);

			Entity e = world.createEntity(name);
			if (preserveIds && (key.hi != 0 || key.lo != 0))
				registry.emplace_or_replace<EntityIdComponent>(e, EntityIdComponent{ key });
			idMap[key] = e;

			if (!hasParent)
				prefabRoot = e;

			if (eJson.contains("components"))
				applyComponents(registry, e, eJson["components"]);
		}

		if (prefabRoot == entt::null) return entt::null;

		rebuildHierarchy(registry, scene, idMap);

		// createEntity() writes BOTH sides of the root link — the new entity's
		// `parent` and the root's `children` entry. rebuildHierarchy then re-points
		// `parent` at the authored parent, but it never touches the root's list,
		// because the World root is not one of the blob's records and so never turns
		// up in idMap. A full scene load has no such gap: there the parentless record
		// maps ONTO world.rootEntity(), whose child list is cleared and restored like
		// any other parent's.
		//
		// Left alone, every non-root prefab entity stays listed under the root as well
		// as under its real parent: it shows up twice in the Outliner, the renderer
		// walks it through a transform chain the author never built, and the double
		// link is written into the .hescene on the next save. So drop the stale
		// entries — the ones this call created that no longer call the root their
		// parent. Anything that kept the root as its parent (the prefab root itself, a
		// child whose authored link a damaged blob failed to restore) stays put, so
		// nothing is ever orphaned into a list-less limbo.
		std::vector<Entity> stale;
		for (const auto& [key, e] : idMap)
		{
			(void)key;
			const auto* h = registry.try_get<HierarchyComponent>(e);
			if (h && h->parent != world.rootEntity())
				stale.push_back(e);
		}
		if (!stale.empty())
			if (auto* rootHier = registry.try_get<HierarchyComponent>(world.rootEntity()))
			{
				auto& rc = rootHier->children;
				rc.erase(std::remove_if(rc.begin(), rc.end(), [&stale](Entity e)
				         {
				             return std::find(stale.begin(), stale.end(), e) != stale.end();
				         }),
				         rc.end());
			}

		Entity targetParent = (prefabParent != entt::null) ? prefabParent : world.rootEntity();
		world.reparentEntity(prefabRoot, targetParent);
		world.markHierarchyDirty();
		return prefabRoot;
	}
} // namespace

// Serialisation failures used to be a bare `return false` — the caller (editor
// save, PIE snapshot, packaged startup) then reported a generic "failed" with no
// idea whether the file was missing, unreadable, or simply malformed. Every exit
// path below says which one it was and for which file.
namespace
{
    size_t sceneEntityCount(const json& scene)
    {
        auto it = scene.find("entities");
        return (it != scene.end() && it->is_array()) ? it->size() : 0u;
    }
}

// Every key applyComponents restores, plus the entity-level extras that ride
// inside the same object on the single-entity (collaboration) path.
//
// This list is written out by hand and there is no compiler check tying it to
// the save or load path — a component wired into BOTH still shows up as
// "unknown" until someone remembers this. The failure is quiet in the worst
// way: the load works fine, but the log claims data was dropped, and a warning
// that cries wolf is a warning people stop reading. tests/test_scene_serializer
// walks a world carrying every component and asserts this covers each key.
bool SceneSerializer::isKnownComponentKey(const std::string& key)
{
	static const std::unordered_set<std::string> kKnown = {
		"animator", "animatorblend", "animstatemachine", "audiolistener",
		"audiosource", "camera", "cameraRig", "characterController", "collider",
		"movement",
		"decal", "environment", "foliage", "light", "lod", "material", "mesh",
		"navagent", "navmesh", "particlesystem", "propertyanimator",
		"rigidbody", "saveState", "script", "skeletalmesh", "terrain",
		"transform", "transform2d", "uibutton", "uicanvas", "uielement",
		"uiimage", "uitext", "weather",
		// Not a component: serializeEntityComponents carries the display name
		// in the same object, because a rename would otherwise be the one edit
		// the component-sync path drops.
		"__name",
	};
	return kKnown.count(key) > 0;
}

bool SceneSerializer::save(const HorizonWorld& world,
                            const std::filesystem::path& path,
                            SerializeFormat format) {
    if (format == SerializeFormat::JSON)   return saveJSON(world, path);
    if (format == SerializeFormat::Binary) return saveBinary(world, path);
    HE_LOG_ERROR(Serialize, "Scene save to '%s': unknown format %d",
                 path.string().c_str(), static_cast<int>(format));
    return false;
}

bool SceneSerializer::load(HorizonWorld& world,
                            const std::filesystem::path& path,
                            SerializeFormat format) {
    if (format == SerializeFormat::JSON)   return loadJSON(world, path);
    if (format == SerializeFormat::Binary) return loadBinary(world, path);
    HE_LOG_ERROR(Serialize, "Scene load from '%s': unknown format %d",
                 path.string().c_str(), static_cast<int>(format));
    return false;
}

bool SceneSerializer::loadAdditive(HorizonWorld& world,
                                    const std::filesystem::path& path,
                                    SerializeFormat format,
                                    std::vector<Entity>* outCreated)
{
    HE_LOG_SLOW_SCOPE(Serialize, 100.0, "SceneSerializer::loadAdditive");
    if (format == SerializeFormat::Binary)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open())
        {
            HE_LOG_ERROR(Serialize, "Additive scene load: cannot open '%s'", path.string().c_str());
            return false;
        }
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                          std::istreambuf_iterator<char>());
        json scene = json::from_cbor(bytes, true, false);
        if (scene.is_discarded())
        {
            HE_LOG_ERROR(Serialize, "Additive scene load: '%s' is not valid CBOR (%zu bytes)",
                         path.string().c_str(), bytes.size());
            return false;
        }
        HE_LOG_INFO(Serialize, "Additive scene load (binary): '%s', %zu entity/-ies",
                    path.string().c_str(), sceneEntityCount(scene));
        return applyAdditiveJson(world, scene, outCreated);
    }
    // Default: JSON
    std::ifstream in(path);
    if (!in.is_open())
    {
        HE_LOG_ERROR(Serialize, "Additive scene load: cannot open '%s'", path.string().c_str());
        return false;
    }
    json scene = json::parse(in, nullptr, false);
    if (scene.is_discarded())
    {
        HE_LOG_ERROR(Serialize, "Additive scene load: '%s' is not valid JSON", path.string().c_str());
        return false;
    }
    HE_LOG_INFO(Serialize, "Additive scene load (JSON): '%s', %zu entity/-ies",
                path.string().c_str(), sceneEntityCount(scene));
    return applyAdditiveJson(world, scene, outCreated);
}

bool SceneSerializer::loadAdditiveFromMemory(HorizonWorld& world,
                                             const std::vector<uint8_t>& data,
                                             std::vector<Entity>* outCreated)
{
    json scene = json::from_cbor(data, true, false);
    if (scene.is_discarded())
    {
        HE_LOG_ERROR(Serialize, "Additive scene load from memory: not valid CBOR (%zu bytes)",
                     data.size());
        return false;
    }
    return applyAdditiveJson(world, scene, outCreated);
}

// ── JSON ──────────────────────────────────────────────────────────────────────
bool SceneSerializer::saveJSON(const HorizonWorld& world, const std::filesystem::path& path)
{
    HE_LOG_SLOW_SCOPE(Serialize, 100.0, "SceneSerializer::saveJSON");
    // Mutable access needed for registry views — safe during serialisation
    json scene = buildSceneJson(const_cast<HorizonWorld&>(world));

    std::ofstream out(path);
    if (!out.is_open())
    {
        HE_LOG_ERROR(Serialize, "Scene save: cannot open '%s' for writing", path.string().c_str());
        return false;
    }
    out << scene.dump(4);
    if (!out.good())
    {
        HE_LOG_ERROR(Serialize, "Scene save: write to '%s' failed (disk full or read-only?)",
                     path.string().c_str());
        return false;
    }
    HE_LOG_INFO(Serialize, "Scene saved (JSON): '%s', %zu entity/-ies",
                path.string().c_str(), sceneEntityCount(scene));
    return true;
}

bool SceneSerializer::loadJSON(HorizonWorld& world, const std::filesystem::path& path)
{
    HE_LOG_SLOW_SCOPE(Serialize, 100.0, "SceneSerializer::loadJSON");
    std::ifstream in(path);
    if (!in.is_open())
    {
        HE_LOG_ERROR(Serialize, "Scene load: cannot open '%s'", path.string().c_str());
        return false;
    }

    json scene = json::parse(in, nullptr, false);
    if (scene.is_discarded())
    {
        HE_LOG_ERROR(Serialize, "Scene load: '%s' is not valid JSON", path.string().c_str());
        return false;
    }

    HE_LOG_INFO(Serialize, "Scene loaded (JSON): '%s', %zu entity/-ies",
                path.string().c_str(), sceneEntityCount(scene));
    return applySceneJson(world, scene);
}

// ── In-memory snapshots (CBOR) ────────────────────────────────────────────────
bool SceneSerializer::saveToMemory(const HorizonWorld& world, std::vector<uint8_t>& out)
{
    json scene = buildSceneJson(const_cast<HorizonWorld&>(world));
    out = json::to_cbor(scene);
    HE_LOG_DEBUG(Serialize, "Scene snapshot to memory: %zu entity/-ies, %zu byte(s)",
                 sceneEntityCount(scene), out.size());
    return true;
}

bool SceneSerializer::loadFromMemory(HorizonWorld& world, const std::vector<uint8_t>& data)
{
    json scene = json::from_cbor(data, true, false);
    if (scene.is_discarded())
    {
        HE_LOG_ERROR(Serialize, "Scene restore from memory: not valid CBOR (%zu bytes)", data.size());
        return false;
    }
    HE_LOG_DEBUG(Serialize, "Scene restored from memory: %zu entity/-ies", sceneEntityCount(scene));
    return applySceneJson(world, scene);
}

// ── Binary (CBOR encoding of the identical JSON structure) ───────────────────
bool SceneSerializer::saveBinary(const HorizonWorld& world, const std::filesystem::path& path)
{
    HE_LOG_SLOW_SCOPE(Serialize, 100.0, "SceneSerializer::saveBinary");
    json scene = buildSceneJson(const_cast<HorizonWorld&>(world));
    const std::vector<uint8_t> cbor = json::to_cbor(scene);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
    {
        HE_LOG_ERROR(Serialize, "Scene save: cannot open '%s' for writing", path.string().c_str());
        return false;
    }
    out.write(reinterpret_cast<const char*>(cbor.data()),
              static_cast<std::streamsize>(cbor.size()));
    if (!out.good())
    {
        HE_LOG_ERROR(Serialize, "Scene save: write to '%s' failed after %zu byte(s)",
                     path.string().c_str(), cbor.size());
        return false;
    }
    HE_LOG_INFO(Serialize, "Scene saved (binary): '%s', %zu entity/-ies, %zu byte(s)",
                path.string().c_str(), sceneEntityCount(scene), cbor.size());
    return true;
}

bool SceneSerializer::loadBinary(HorizonWorld& world, const std::filesystem::path& path)
{
    HE_LOG_SLOW_SCOPE(Serialize, 100.0, "SceneSerializer::loadBinary");
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
    {
        HE_LOG_ERROR(Serialize, "Scene load: cannot open '%s'", path.string().c_str());
        return false;
    }

    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
    json scene = json::from_cbor(bytes, true, false);
    if (scene.is_discarded())
    {
        HE_LOG_ERROR(Serialize, "Scene load: '%s' is not valid CBOR (%zu bytes)",
                     path.string().c_str(), bytes.size());
        return false;
    }

    HE_LOG_INFO(Serialize, "Scene loaded (binary): '%s', %zu entity/-ies",
                path.string().c_str(), sceneEntityCount(scene));
    return applySceneJson(world, scene);
}

// ── Prefab API ────────────────────────────────────────────────────────────────
std::vector<uint8_t> SceneSerializer::serializeSubtree(const HorizonWorld& world, Entity root)
{
    json scene = buildSubtreeJson(const_cast<HorizonWorld&>(world), root);
    std::vector<uint8_t> cbor = json::to_cbor(scene);
    HE_LOG_DEBUG(Serialize, "Serialised prefab subtree: %zu entity/-ies, %zu byte(s)",
                 sceneEntityCount(scene), cbor.size());
    return cbor;
}

std::vector<uint8_t> SceneSerializer::serializeEntityComponents(const HorizonWorld& world,
                                                               Entity entity)
{
    auto& registry = const_cast<HorizonWorld&>(world).registry();
    if (!registry.valid(entity)) return {};
    json comps = serializeComponents(registry, entity);
    // The display name lives OUTSIDE the components block in the scene format
    // (it sits at entity level), so without carrying it here a rename would be
    // the one edit the component-sync path silently drops.
    if (auto* name = registry.try_get<NameComponent>(entity))
        comps["__name"] = name->name;
    return json::to_cbor(comps);
}

bool SceneSerializer::applyEntityComponents(HorizonWorld& world, Entity entity,
                                            const std::vector<uint8_t>& data)
{
    auto& registry = world.registry();
    if (!registry.valid(entity) || data.empty()) return false;

    const json comps = json::from_cbor(data, /*strict=*/true, /*allow_exceptions=*/false);
    if (comps.is_discarded() || !comps.is_object()) return false;

    // Reuses the same restore path as scene loading, so every component type is
    // covered by construction — a new component that loads from a scene file
    // replicates without any extra work here.
    applyComponents(registry, entity, comps);
    if (auto it = comps.find("__name"); it != comps.end() && it->is_string())
        registry.emplace_or_replace<NameComponent>(entity, NameComponent{ *it });
    return true;
}

Entity SceneSerializer::instantiatePrefab(HorizonWorld& world,
                                          const std::vector<uint8_t>& data,
                                          Entity parent,
                                          bool preserveIds)
{
    json scene = json::from_cbor(data, true, false);
    if (scene.is_discarded())
    {
        HE_LOG_ERROR(Serialize, "Prefab instantiation failed: payload is not valid CBOR "
                                "(%zu bytes)", data.size());
        return entt::null;
    }
    return applyPrefabJson(world, scene, parent, preserveIds);
}
