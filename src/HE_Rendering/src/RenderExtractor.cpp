#include "HorizonRendering/RenderExtractor.h"
#include <cstdint>
#include "HorizonRendering/RenderWorld.h"
#include "HorizonRendering/RenderConstants.h"   // kShadowMapResolution
#include <Diagnostics/Profiler.h>
#include <Renderer/IRenderer.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/SkeletalMeshComponent.h>
#include <HorizonScene/Components/MaterialComponent.h>
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/LightComponent.h>
#include <HorizonScene/Components/DecalComponent.h>
#include <HorizonScene/Components/EnvironmentLightComponent.h>
#include <HorizonScene/Components/ParticleSystemComponent.h>
#include <HorizonScene/Components/WeatherComponent.h>
#include <HorizonScene/Components/FoliageComponent.h>
#include <HorizonScene/Components/TerrainComponent.h>       // landscape layer weightmap
#include <HorizonScene/Components/TerrainChunkComponent.h>  // chunk → parent landscape
#include <HorizonScene/UISystem.h>
#include <ContentManager/DefaultAssets.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <JobSystem/JobSystem.h>
#include <glm/gtc/quaternion.hpp>
#include <glm/common.hpp>
#include <algorithm>
#include <cmath>

// extract() is split into one free function per phase (the banners this file used
// to carry inline). The phases run in the order extract() calls them and each one
// reads what the earlier ones wrote — the camera before anything culling-related,
// every renderable before the shadow fit, the lights before the day-night pass.
namespace
{
	glm::mat4 localMatrix(const TransformComponent& t)
	{
		glm::quat q = glm::quat(glm::radians(t.rotation));
		return glm::translate(glm::mat4(1.0f), t.position)
		     * glm::mat4_cast(q)
		     * glm::scale(glm::mat4(1.0f), t.scale);
	}

	// Recompute world matrices top-down from the world root. This is the only
	// place world matrices are propagated — there is no separate scene-graph
	// pass. Walking from world.rootEntity() is what makes it work: HorizonWorld
	// parents everything to a root *entity*, so anything keyed on
	// parent == entt::null would never fire. Recomputing every frame is cheap at
	// current scene sizes; dirty-flag pruning can come back with profiling.
	void propagateFrom(entt::registry& reg, entt::entity e, const glm::mat4& parentWorld)
	{
		glm::mat4 world = parentWorld;
		if (auto* t = reg.try_get<TransformComponent>(e))
		{
			world          = parentWorld * localMatrix(*t);
			t->worldMatrix = world;
			t->dirty       = false;
		}
		if (auto* h = reg.try_get<HierarchyComponent>(e))
			for (entt::entity child : h->children)
				propagateFrom(reg, child, world);
	}

	const HE::AABB kUnitCube = []{
		HE::AABB b;
		b.expand({ -0.5f, -0.5f, -0.5f });
		b.expand({  0.5f,  0.5f,  0.5f });
		return b;
	}();

	// ── Transforms ──────────────────────────────────────────────────────────
	void extractTransforms(HorizonWorld& world, entt::registry& reg)
	{
		propagateFrom(reg, world.rootEntity(), glm::mat4(1.0f));
		// Entities outside the root hierarchy (no HierarchyComponent)
		for (auto [e, t] : reg.view<TransformComponent>(entt::exclude<HierarchyComponent>).each())
		{
			t.worldMatrix = localMatrix(t);
			t.dirty       = false;
		}
	}

	// ── Camera ──────────────────────────────────────────────────────────────
	// Editor scene view wins when active. Otherwise prefer the camera marked
	// isMain, fall back to the first one found, then to a fixed editor default
	// so an empty scene still renders sanely.
	void extractCamera(entt::registry& reg, RenderWorld& out, float aspectRatio,
	                   const EditorCameraOverride* editorCam)
	{
		if (editorCam && editorCam->active)
		{
			out.camera.position   = editorCam->position;
			out.camera.view       = editorCam->view;
			out.camera.projection = editorCam->orthographic
				? glm::ortho(-aspectRatio * 5.0f, aspectRatio * 5.0f, -5.0f, 5.0f,
				             editorCam->nearPlane, editorCam->farPlane)
				: glm::perspective(glm::radians(editorCam->fovDegrees), aspectRatio,
				                   editorCam->nearPlane, editorCam->farPlane);
		}
		else
		{
			bool cameraFound = false;
			for (auto [e, t, cam] : reg.view<TransformComponent, CameraComponent>().each())
			{
				if (cameraFound && !cam.isMain) continue;

				out.camera.position   = glm::vec3(t.worldMatrix[3]);
				out.camera.view       = glm::inverse(t.worldMatrix);
				out.camera.projection = cam.orthographic
					? glm::ortho(-aspectRatio * 5.0f, aspectRatio * 5.0f, -5.0f, 5.0f,
					             cam.nearPlane, cam.farPlane)
					: glm::perspective(glm::radians(cam.fovDegrees), aspectRatio,
					                   cam.nearPlane, cam.farPlane);
				cameraFound = true;
				if (cam.isMain) break;
			}
			if (!cameraFound)
			{
				const glm::vec3 eye(4.0f, 3.0f, 4.0f);
				out.camera.position   = eye;
				out.camera.view       = glm::lookAt(eye, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
				out.camera.projection = glm::perspective(glm::radians(60.0f), aspectRatio, 0.1f, 1000.0f);
			}
		}
	}

	// ── Renderables ─────────────────────────────────────────────────────────
	// Two-phase build: sequential ECS read (no EnTT concurrency guarantees), then
	// parallel AABB/transform computation (pure math, no registry access).
	// KEEP THE PHASES APART: every registry / ContentManager read belongs in the
	// gather loop, the parallel_for must stay pure maths over `items`.
	void extractMeshes(entt::registry& reg, RenderWorld& out, ContentManager* contentManager)
	{
		struct EntityData {
			glm::mat4 world;
			HE::UUID  meshId;
			HE::UUID  matId;
			uint32_t  entId;
			int       lod;
			bool      castsShadow;
			HE::AABB  localBounds; // real mesh AABB; invalid → world bounds stay invalid (never culled)
			std::vector<float> paramOverride; // merged HeParams block, or empty
			HE::UUID  weightmapId;            // landscape layer weights (chunks only)
		};
		auto meshView = reg.view<TransformComponent, MeshComponent>();
		std::vector<EntityData> items;
		items.reserve(meshView.size_hint());
		for (auto [e, t, mesh] : meshView.each())
		{
			if (!mesh.visible) continue; // hidden (e.g. a preloaded zone)
			EntityData d;
			d.world  = t.worldMatrix;
			d.meshId = mesh.meshAssetId;
			d.matId  = {};
			if (const auto* matComp = reg.try_get<MaterialComponent>(e))
			{
				d.matId = matComp->materialAssetId;
				// Per-entity param overrides → merge into a full HeParams block now (serial,
				// ContentManager-safe). Empty when the entity has no overrides / no material.
				if (!matComp->paramOverrides.empty() && contentManager)
					if (const MaterialAsset* ma = contentManager->getMaterial(d.matId))
					{
						std::vector<float> block(64, 0.0f); // 16 vec4 slots
						const size_t n = std::min(ma->shaderParamData.size(), size_t(64));
						std::copy(ma->shaderParamData.begin(), ma->shaderParamData.begin() + n, block.begin());
						for (const auto& ov : matComp->paramOverrides)
							for (size_t s = 0; s < ma->graphParamNames.size() && s < 16; ++s)
								if (ma->graphParamNames[s] == ov.name)
								{
									for (int k = 0; k < 4; ++k) block[s * 4 + k] = ov.value[k];
									break;
								}
						d.paramOverride = std::move(block);
					}
			}
			// Terrain chunk → its parent landscape's painted layer weightmap, so a
			// Landscape Layer Blend material samples the right terrain's paint. Read
			// from the PARENT: the weights belong to the landscape, the chunks are
			// just the pieces it renders as.
			if (const auto* chunk = reg.try_get<TerrainChunkComponent>(e))
				if (reg.valid(chunk->terrain))
					if (const auto* pt = reg.try_get<TerrainComponent>(chunk->terrain))
						d.weightmapId = pt->weightmapTextureId;
			d.entId  = static_cast<uint32_t>(e);
			d.lod    = mesh.lodBias;
			d.castsShadow = mesh.castsShadow;
			// Look up the real object-space bounds here (sequential — the
			// ContentManager read must not race the parallel_for below).
			if (contentManager)
				if (const StaticMeshAsset* m = contentManager->getStaticMesh(d.meshId))
				{
					HE::AABB b;
					b.min = { m->boundsMin[0], m->boundsMin[1], m->boundsMin[2] };
					b.max = { m->boundsMax[0], m->boundsMax[1], m->boundsMax[2] };
					// Require a real, non-degenerate box. A mesh registered without a computed
					// AABB leaves boundsMin==boundsMax=={0,0,0} — "valid" but a zero-volume point,
					// and culling against it drops the object the moment its pivot exits the view.
					if (b.isValid() && b.max != b.min) d.localBounds = b;
				}
			items.push_back(d);
		}

		out.objects.resize(items.size());
		parallel_for(items.size(), [&](size_t i) {
			const EntityData& d = items[i];
			RenderObject&   obj = out.objects[i];
			obj.meshAssetId     = d.meshId;
			obj.materialAssetId = d.matId;
			obj.transform       = d.world;
			// Cull only against KNOWN bounds. A mesh whose real AABB isn't available yet (not
			// resident — common while streaming or on an LOD swap, worse in packaged builds)
			// must NOT be culled against a tiny unit-cube proxy: that box is far smaller than a
			// large mesh, so the object vanishes while plainly in view. Leaving the bounds
			// invalid makes the conservative culler keep it visible until the backend resolves
			// the mesh and fills in the real bounds (the GPU still clips it if it is genuinely
			// off-screen, so there is no visible cost).
			obj.worldBounds     = d.localBounds.isValid() ? d.localBounds.transformed(d.world) : HE::AABB{};
			obj.entityId        = d.entId;
			obj.lod             = d.lod;
			obj.castsShadow     = d.castsShadow;
			obj.paramOverride   = d.paramOverride; // per-entity HeParams block (empty = none)
			obj.weightmapTextureId = d.weightmapId; // landscape layer weights (chunks only)
		});
	}

	// ── ParticleGraph particles ────────────────────────────────────────────────
	// One ParticleBatch per emitter: raw position/size/t01 per particle, GPU-
	// instanced (see HorizonRendering::ParticleShaderTemplates) — the backend picks
	// a shader baked from `config`'s color/alpha-over-life endpoints (compiled once,
	// cached by content hash, not per-particle CPU work). Size stays CPU-lerped (one
	// scalar; moving it to the GPU buys nothing — see ParticleShaderGen's comment).
	void extractParticleBatches(entt::registry& reg, RenderWorld& out)
	{
		for (auto [e, tc, ps] : reg.view<TransformComponent, ParticleSystemComponent>().each())
		{
			if (!ps.visible) continue; // hidden (e.g. a preloaded zone)
			if (ps.particles.empty()) continue;
			const HE::ParticleEmitterConfig& config = ps.resolvedConfig; // (re)resolved by ParticleSystem::update

			ParticleBatch batch;
			batch.particleAssetId = ps.particleAssetId;
			batch.meshAssetId     = (config.meshAssetId == HE::UUID{}) ? HE::kDefaultQuadMeshId : config.meshAssetId;
			batch.materialAssetId = config.materialAssetId;
			batch.config          = config;
			batch.entityId        = static_cast<uint32_t>(e);
			batch.instances.reserve(ps.particles.size());
			for (const Particle& p : ps.particles)
			{
				if (p.lifetime <= 0.0f) continue;
				const float t01  = 1.0f - p.lifetime / p.maxLifetime;  // 0=born, 1=dead
				const float size = config.startSize + (config.endSize - config.startSize) * t01;
				if (size <= 0.0f) continue;
				batch.instances.push_back({ p.position, size, t01 });
			}
			if (!batch.instances.empty()) out.particleBatches.push_back(std::move(batch));
		}
	}

	// ── Weather precipitation ──────────────────────────────────────────────────
	// Turns each live precip drop into a camera-facing billboard. The per-particle
	// cost is the billboard basis (cross products + normalises), so we first gather
	// lightweight inputs (cheap, serial) and then build the matrices across all
	// cores — the same gather→parallel_for pattern as the mesh path. At high
	// precipitation caps this turns a serial tens-of-thousands stall into a
	// parallel sweep. Same-emitter particles keep one mesh+material so the geometry
	// pass still instances them.
	enum class BillboardKind : uint8_t { Snow, RainStreak };

	// Per-particle payload — deliberately only what actually varies per drop.
	// Everything an emitter shares (mesh, entity id, billboard kind) lives once in
	// PrecipEmitter below: at tens of thousands of drops per frame, copying the
	// emitter constants into every element was the bulk of this vector.
	struct ParticleInput {
		glm::vec3 position;
		glm::vec3 velocity;   // RainStreak only
	};
	// One weather emitter plus the half-open range [begin, end) of `pin` it filled.
	struct PrecipEmitter {
		HE::UUID      meshId;
		uint32_t      entityId;
		BillboardKind kind;
		size_t        end;    // one-past-last index into `pin`
	};

	void extractPrecipitation(entt::registry& reg, RenderWorld& out)
	{
		std::vector<ParticleInput> pin;
		std::vector<PrecipEmitter> emitters;

		for (auto [e, wx] : reg.view<WeatherComponent>().each())
		{
			if (wx.precip.empty()) continue;
			const bool isSnow = (wx.curPrecipType == PrecipType::Snow);
			// Snow uses the star flake mesh; rain uses the quad stretched into a streak.
			const HE::UUID      precipMesh = isSnow ? HE::kDefaultSnowflakeMeshId : HE::kDefaultQuadMeshId;
			const BillboardKind kind       = isSnow ? BillboardKind::Snow : BillboardKind::RainStreak;
			pin.reserve(pin.size() + wx.precip.size());
			for (const Particle& p : wx.precip)
				pin.push_back({ p.position, p.velocity });
			emitters.push_back({ precipMesh, static_cast<uint32_t>(e), kind, pin.size() });
		}

		if (!pin.empty())
		{
			const size_t   base   = out.objects.size();
			const glm::vec3 camPos = out.camera.position;
			out.objects.resize(base + pin.size());
			parallel_for(pin.size(), [&](size_t i) {
				const ParticleInput& in  = pin[i];
				// Emitters are stored with their one-past-last index and are strictly
				// increasing, so this maps a particle back to its emitter without
				// storing the emitter constants per particle. There is normally one.
				const PrecipEmitter& em = *std::upper_bound(
					emitters.begin(), emitters.end(), i,
					[](size_t idx, const PrecipEmitter& e) { return idx < e.end; });
				RenderObject&        obj = out.objects[base + i];
				obj.meshAssetId     = em.meshId;
				obj.entityId        = em.entityId;
				obj.lod             = 0;
				obj.castsShadow     = false;  // billboards: no shadow / AO contribution
				obj.contributesAO   = false;

				glm::vec3   look = camPos - in.position;
				const float d    = glm::length(look);
				glm::mat4   world(1.0f);
				if (d <= 1e-5f)
				{
					world = glm::mat4(0.0f);  // particle sitting on the camera → degenerate/invisible
				}
				else if (em.kind == BillboardKind::RainStreak)
				{
					look /= d;
					glm::vec3 vdir = in.velocity;
					const float vl = glm::length(vdir);
					vdir = (vl > 1e-4f) ? vdir / vl : glm::vec3(0, -1, 0);
					glm::vec3 up = vdir - look * glm::dot(vdir, look);  // project onto camera plane
					if (glm::length(up) < 1e-4f) up = glm::vec3(0, 1, 0);
					up = glm::normalize(up);
					const glm::vec3 right = glm::normalize(glm::cross(up, look));
					const float len = 0.6f, thin = 0.02f;
					world[0] = glm::vec4(right * thin, 0.0f);
					world[1] = glm::vec4(up    * len,  0.0f);
					world[2] = glm::vec4(look,         0.0f);
				}
				else  // Snow flake (the only remaining kind reaching this branch)
				{
					look /= d;
					const float s = 0.16f;
					glm::vec3 right = glm::cross(glm::vec3(0, 1, 0), look);
					if (glm::length(right) < 1e-4f) right = glm::vec3(1, 0, 0);
					right = glm::normalize(right);
					const glm::vec3 up = glm::cross(look, right);
					world[0] = glm::vec4(right * s, 0.0f);
					world[1] = glm::vec4(up    * s, 0.0f);
					world[2] = glm::vec4(look  * s, 0.0f);
				}
				world[3] = glm::vec4(in.position, 1.0f);
				obj.transform   = world;
				obj.worldBounds = kUnitCube.transformed(world);
			});
		}
	}

	// ── Foliage ──────────────────────────────────────────────────────────────
	// Each cached foliage instance is pushed as a RenderObject. Because all
	// instances share the same meshAssetId, the geometry pass batches them into
	// one DrawCall with instanceTransforms automatically.
	void extractFoliage(entt::registry& reg, RenderWorld& out)
	{
		for (auto [e, fol] : reg.view<FoliageComponent>().each())
		{
			if (!fol.visible) continue; // hidden (e.g. a preloaded zone)
			if (fol.meshAssetId == HE::UUID{}) continue;
			const float dd2 = fol.drawDistance * fol.drawDistance;
			const glm::vec3 camPos = out.camera.position;

			for (const glm::mat4& inst : fol.cachedInstances)
			{
				const glm::vec3 wp = glm::vec3(inst[3]);
				const float dx = wp.x - camPos.x;
				const float dz = wp.z - camPos.z;
				if (dx * dx + dz * dz > dd2) continue;

				RenderObject obj;
				obj.meshAssetId     = fol.meshAssetId;
				obj.materialAssetId = fol.materialAssetId;
				obj.transform       = inst;
				// Real bounds are filled in by the backend mesh-resolve refine; leave them invalid
				// here so a not-yet-resident instance stays visible instead of being culled against
				// a unit-cube proxy smaller than the actual foliage mesh.
				obj.worldBounds     = HE::AABB{};
				obj.entityId        = static_cast<uint32_t>(e);
				out.objects.push_back(obj);
			}
		}
	}

	// ── Skinned renderables ─────────────────────────────────────────────────
	void extractSkinnedMeshes(entt::registry& reg, RenderWorld& out)
	{
		out.skinnedObjects.clear();
		for (auto [e, t, smc] : reg.view<TransformComponent, SkeletalMeshComponent>().each())
		{
			if (!smc.visible) continue; // hidden (e.g. a preloaded zone)
			SkinnedRenderObject obj;
			obj.meshAssetId     = smc.meshAssetId;
			obj.transform       = t.worldMatrix;
			obj.worldBounds     = kUnitCube.transformed(t.worldMatrix);
			obj.entityId        = static_cast<uint32_t>(e);
			obj.boneMatrices    = smc.boneMatrices.empty()
			                    ? std::vector<glm::mat4>{ glm::mat4(1.0f) }
			                    : smc.boneMatrices;
			if (const auto* matComp = reg.try_get<MaterialComponent>(e))
				obj.materialAssetId = matComp->materialAssetId;
			out.skinnedObjects.push_back(std::move(obj));
		}
	}

	// ── Decals ──────────────────────────────────────────────────────────────
	// The entity's world matrix IS the projector box (unit cube scaled by the
	// transform); the deferred path blends the colour into the G-buffer.
	void extractDecals(entt::registry& reg, RenderWorld& out)
	{
		for (auto [e, t, d] : reg.view<TransformComponent, DecalComponent>().each())
		{
			DecalData dd;
			dd.transform = t.worldMatrix;
			dd.color     = d.color;
			dd.textureId = d.textureId;
			out.decals.push_back(dd);
		}
	}

	// ── Lights ──────────────────────────────────────────────────────────────
	void extractLights(entt::registry& reg, RenderWorld& out)
	{
		out.lights.reserve(reg.view<LightComponent>().size() + 1); // +1 for the day-night moon
		for (auto [e, t, light] : reg.view<TransformComponent, LightComponent>().each())
		{
			if (!light.visible) continue; // hidden (e.g. a preloaded zone)
			// Per-light distance culling (point/spot only): beyond cullDistance from
			// the camera the light is dropped from the extracted set entirely, so
			// direct shading AND the GI probe bounce ignore it consistently on every
			// backend. 0 = never cull. Directional lights are global by nature.
			if (light.type != HE::LightType::Directional && light.cullDistance > 0.0f
			    && glm::distance(glm::vec3(t.worldMatrix[3]), out.camera.position) > light.cullDistance)
				continue;
			LightData l;
			l.position     = glm::vec3(t.worldMatrix[3]);
			// Lights shine along their local -Z (third column of the world matrix)
			l.direction    = -glm::normalize(glm::vec3(t.worldMatrix[2]));
			l.color        = light.color;
			l.intensity    = light.intensity;
			l.range        = light.range;
			l.spotAngleCos = std::cos(glm::radians(light.spotAngle * 0.5f));
			l.type         = static_cast<uint8_t>(light.type);
			l.castsShadow  = light.castsShadow;
			if (const auto* env = reg.try_get<EnvironmentLightComponent>(e))
				l.envRole = (env->role == EnvironmentLightComponent::Role::Sun) ? 1 : 2;
			out.lights.push_back(l);
		}
	}

	// ── Directional-light shadow view-projection ─────────────────────────────
	// The brightest directional light casts shadows (so the single shadow map
	// follows the sun by day and the moon by night). The ortho frustum is fitted
	// around the union of the (seeded) object bounds — backends refine bounds
	// elsewhere, but this rough fit is enough for a single full-scene shadow map.
	void fitDirectionalShadow(RenderWorld& out)
	{
		out.shadow.enabled = false;
		const LightData* shadowLight = nullptr;
		for (const LightData& l : out.lights)
		{
			if (l.type != 0) continue; // 0 = directional
			if (!shadowLight || l.intensity > shadowLight->intensity)
				shadowLight = &l;
		}
		if (!(shadowLight && shadowLight->intensity > 0.1f)) return;

		HE::AABB sceneBox;
		for (const RenderObject& o : out.objects)
			sceneBox.expand(o.worldBounds);
		glm::vec3 center = sceneBox.isValid() ? sceneBox.center() : glm::vec3(0.0f);
		float radius = sceneBox.isValid() ? glm::length(sceneBox.extents()) : 10.0f;
		radius = std::max(radius, 1.0f);

		const glm::vec3 dir = glm::normalize(shadowLight->direction);
		const glm::vec3 up  = std::abs(dir.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);

		// Texel-snap the frustum centre so the shadow-map samples stay on stable
		// world positions as the day-night light rotates — without this the shadow
		// edges crawl/flicker frame to frame. Snap the centre along the light's
		// right/up axes in whole-texel steps (kShadowMapResolution must match the
		// backends' shadow map resolution).
		constexpr float kShadowMapRes = static_cast<float>(HE::kShadowMapResolution);
		const float worldPerTexel = (2.0f * radius) / kShadowMapRes;
		const glm::vec3 right = glm::normalize(glm::cross(dir, up)); // glm::lookAt side axis
		const glm::vec3 upL   = glm::cross(right, dir);              // glm::lookAt up axis
		const float sx = std::floor(glm::dot(center, right) / worldPerTexel) * worldPerTexel;
		const float sy = std::floor(glm::dot(center, upL)   / worldPerTexel) * worldPerTexel;
		const float sz = glm::dot(center, dir);
		center = right * sx + upL * sy + dir * sz;

		const glm::vec3 eye = center - dir * (radius * 2.0f);
		const glm::mat4 view = glm::lookAt(eye, center, up);
		const glm::mat4 proj = glm::ortho(-radius, radius, -radius, radius, 0.05f, radius * 4.0f);
		out.shadow.viewProj  = proj * view;   // legacy single map (GL / D3D / Vulkan)
		out.shadow.direction = dir;
		out.shadow.enabled   = true;

		// ── Cascaded Shadow Maps (Metal) ───────────────────────────────────
		// Fit `kCascadeCount` tight light frusta to successive slices of the camera
		// frustum, but only out to a BOUNDED shadowDistance (not the 5000-unit far
		// plane) — that bound is what makes the near cascade hug the camera and give
		// sharp shadows. Each cascade is fit to the bounding SPHERE of its sub-frustum
		// (rotation-invariant → stable texel size) and texel-snapped in its own light
		// space (no crawl). The light-direction (Z) range is kept generous so casters
		// between the light and the slice are not clipped.
		constexpr int   kCascadeCount  = 3;
		constexpr float kShadowDistance = 250.0f; // metres of shadow coverage (tunable)
		constexpr float kLambda        = 0.5f;    // uniform↔logarithmic split blend
		constexpr float kCascadeRes    = static_cast<float>(HE::kShadowMapResolution);

		// Camera near/far from the (glm, z∈[-1,1]) projection matrix.
		const glm::mat4& P = out.camera.projection;
		const float camN = P[3][2] / (P[2][2] - 1.0f);
		const float camF = P[3][2] / (P[2][2] + 1.0f);
		const float shadowFar = std::min(std::max(camF, camN + 1.0f), kShadowDistance);

		float splitD[ShadowData::kMaxCascades + 1];
		HE::computeCascadeSplits(camN, shadowFar, kCascadeCount, kLambda, splitD);

		// Stable per-cascade sphere fit (jitter-free → no shadow swim): the bounding
		// sphere of a frustum slice depends ONLY on fov/aspect/splits, not the camera
		// pose, so its radius is constant frame-to-frame → constant texel size. The
		// centre rides the camera forward axis. Texel-snapping is done in shadow-clip
		// space (round the projected world origin to a whole texel) — the robust form
		// that keeps shadows locked to the world as the cascade centre moves.
		// (Both are HE::fitCascadeSphere / HE::cascadeTexelSnapOffset — free functions
		// so the maths has direct test coverage; see tests/test_culling.cpp.)
		const float thfX = (P[0][0] != 0.0f) ? 1.0f / P[0][0] : 1.0f; // tan(halfFovX)
		const float thfY = (P[1][1] != 0.0f) ? 1.0f / P[1][1] : 1.0f; // tan(halfFovY)
		const glm::mat4 camWorld = glm::inverse(out.camera.view);
		const glm::vec3 camPos   = glm::vec3(camWorld[3]);
		const glm::vec3 camFwd   = -glm::normalize(glm::vec3(camWorld[2]));

		out.shadow.cascadeCount = kCascadeCount;
		for (int c = 0; c < kCascadeCount; ++c)
		{
			const float nC = splitD[c];
			const float fC = splitD[c + 1];
			const HE::CascadeSphere sphere = HE::fitCascadeSphere(nC, fC, thfX, thfY);
			const float     crad    = sphere.radius;
			const glm::vec3 ccenter = camPos + camFwd * sphere.centerDistance;

			// Pull the light eye back BEYOND the whole scene toward the light so casters
			// that sit above/behind this cascade slice (off-screen, but casting INTO the
			// visible region) are still rasterized — otherwise their shadows vanish the
			// moment the caster leaves the camera frustum (the reported "shadows
			// disappear at certain angles"). The Z range is generous; D32 depth keeps
			// sub-mm precision over these distances, and the XY snap (crad) is unaffected.
			const float     backZ = crad + radius;   // radius = whole-scene bounding radius
			const glm::vec3 cEye  = ccenter - dir * backZ;
			const glm::mat4 cView = glm::lookAt(cEye, ccenter, up);
			glm::mat4       cProj = glm::ortho(-crad, crad, -crad, crad, 0.05f, backZ + crad);

			// Texel snap in clip space: shift the projection so the (fixed) world origin
			// lands on a whole shadow texel → the texel grid is world-anchored.
			const glm::vec2 off = HE::cascadeTexelSnapOffset(cProj * cView, kCascadeRes);
			cProj[3][0] += off.x;
			cProj[3][1] += off.y;

			out.shadow.cascadeViewProj[c] = cProj * cView;
			out.shadow.cascadeSplit[c]    = fC;   // view-space far distance
		}
	}

	// ── Local-light (point/spot) shadow views ────────────────────────────────
	// Independent of the directional CSM (runs even at night / without a sun).
	// Shadow-casting point/spot lights are packed camera-nearest-first into the
	// 16-layer local shadow atlas: spot = 1 perspective layer, point = 6 cube-face
	// layers. Only the first 8 lights ever reach the shaders (LightGPU cap), so
	// lights beyond that never get layers.
	void assignLocalShadowLayers(RenderWorld& out)
	{
		out.shadow.localLayerCount = 0;
		for (LightData& l : out.lights) l.shadowLayer = -1;

		constexpr int kMaxShaderLights = 8;
		const int lightWindow = std::min<int>(static_cast<int>(out.lights.size()), kMaxShaderLights);
		std::vector<int> shadowed;
		for (int i = 0; i < lightWindow; ++i)
		{
			const LightData& l = out.lights[i];
			if (l.castsShadow && l.type != 0 && l.intensity > 0.0f && l.range > 0.0f)
				shadowed.push_back(i);
		}
		auto dist2 = [&](int i) {
			const glm::vec3 d = out.lights[i].position - out.camera.position;
			return glm::dot(d, d);
		};
		std::sort(shadowed.begin(), shadowed.end(),
		          [&](int a, int b) { return dist2(a) < dist2(b); });

		int layer = 0;
		for (int idx : shadowed)
		{
			LightData& l = out.lights[idx];
			// Range-scaled near plane: a tiny fixed near (0.05) with far = range
			// crushes all useful depth into z≈1 (perspective depth is hyperbolic
			// in the near value) — caster and receiver then differ by less than
			// the PCF bias and the shadow vanishes. 1% of range keeps millimetre
			// separation at typical light ranges.
			const float farP  = std::max(l.range, 0.2f);
			const float nearP = std::max(0.1f, farP * 0.01f);
			if (l.type == 2) // spot: one perspective map down the cone
			{
				if (layer + 1 > ShadowData::kMaxLocalShadowLayers) continue;
				// Full cone angle, slightly widened so the PCF kernel + smoothstep
				// cone falloff never sample outside the rendered frustum.
				const float halfAngle = std::acos(glm::clamp(l.spotAngleCos, -1.0f, 0.999f));
				const float fovy = glm::clamp(2.0f * halfAngle * 1.15f,
				                              glm::radians(5.0f), glm::radians(170.0f));
				const glm::vec3 dir = glm::normalize(l.direction);
				const glm::vec3 up  = std::abs(dir.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
				const glm::mat4 view = glm::lookAt(l.position, l.position + dir, up);
				const glm::mat4 proj = glm::perspective(fovy, 1.0f, nearP, farP);
				out.shadow.localViewProj[layer] = proj * view;
				l.shadowLayer = static_cast<int16_t>(layer);
				layer += 1;
			}
			else if (l.type == 1) // point: 6 cube faces as consecutive array layers
			{
				if (layer + 6 > ShadowData::kMaxLocalShadowLayers) continue;
				static const glm::vec3 kFaceDir[6] = {
					{ 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
				static const glm::vec3 kFaceUp[6] = {
					{ 0, 1, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }, { 0, 1, 0 }, { 0, 1, 0 } };
				// > 90° fov so adjacent faces overlap slightly — hides the face seam
				// that the shader's hard major-axis pick would otherwise show under PCF.
				const glm::mat4 proj = glm::perspective(glm::radians(92.0f), 1.0f, nearP, farP);
				for (int f = 0; f < 6; ++f)
				{
					const glm::mat4 view = glm::lookAt(l.position, l.position + kFaceDir[f], kFaceUp[f]);
					out.shadow.localViewProj[layer + f] = proj * view;
				}
				l.shadowLayer = static_cast<int16_t>(layer);
				layer += 6;
			}
		}
		out.shadow.localLayerCount = layer;
	}
}

namespace HE
{

CascadeSphere fitCascadeSphere(float nearD, float farD, float tanHalfFovX, float tanHalfFovY)
{
	// Sphere through the slice's near + far corner rings, centre on view axis.
	const float xn = nearD * tanHalfFovX, yn = nearD * tanHalfFovY;
	const float xf = farD  * tanHalfFovX, yf = farD  * tanHalfFovY;
	const float aa = xn * xn + yn * yn;
	const float bb = xf * xf + yf * yf;
	float zc = (bb - aa + farD * farD - nearD * nearD) / (2.0f * (farD - nearD));
	zc = glm::clamp(zc, nearD, farD);
	float crad = std::sqrt(std::max(aa + (nearD - zc) * (nearD - zc),
	                                bb + (farD  - zc) * (farD  - zc)));
	crad = std::max(crad, 0.01f);
	crad = std::ceil(crad * 16.0f) / 16.0f;   // quantise → texel quantum stays
	                                          // stable across small fov/aspect drift
	return { zc, crad };
}

void computeCascadeSplits(float camNear, float shadowFar, int cascadeCount,
                          float lambda, float* outSplits)
{
	outSplits[0] = camNear;
	for (int i = 1; i <= cascadeCount; ++i)
	{
		const float pf   = static_cast<float>(i) / static_cast<float>(cascadeCount);
		const float logS = camNear * std::pow(shadowFar / camNear, pf);
		const float uniS = camNear + (shadowFar - camNear) * pf;
		outSplits[i] = lambda * logS + (1.0f - lambda) * uniS;
	}
}

glm::vec2 cascadeTexelSnapOffset(const glm::mat4& lightViewProj, float shadowMapRes)
{
	const glm::vec4 o  = lightViewProj * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f); // ortho → w = 1
	const float halfRes = shadowMapRes * 0.5f;
	const glm::vec2 so  = glm::vec2(o.x, o.y) * halfRes;        // origin in texels
	return (glm::round(so) - so) / halfRes;                     // sub-texel NDC fix
}

} // namespace HE

void RenderExtractor::extract(HorizonWorld& world, RenderWorld& out, float aspectRatio,
                              const EditorCameraOverride* editorCam)
{
	HE_PROFILE_SCOPE_N("RenderExtractor::extract");
	out.clear();
	auto& reg = world.registry();

	extractTransforms(world, reg);
	extractCamera(reg, out, aspectRatio, editorCam);
	// Renderables. Everything that ends up in out.objects must run before the
	// shadow fit below, which fits its frustum around their union.
	extractMeshes(reg, out, m_contentManager);
	extractParticleBatches(reg, out);
	extractPrecipitation(reg, out);
	extractFoliage(reg, out);
	extractSkinnedMeshes(reg, out);
	extractDecals(reg, out);
	// Lights, then the day-night pass that overrides the sun/moon among them.
	extractLights(reg, out);
	applyDayNight(out);
	// Shadows last: both phases read the finished object + light sets.
	fitDirectionalShadow(out);
	assignLocalShadowLayers(out);
}

// ── Environment sun + moon (day-night) ────────────────────────────────────
// The sun and moon are the two built-in directional lights tagged by the
// EnvironmentComponent (envRole 1/2). The environment drives their colour,
// intensity and (day-night) arc direction — render-time only, the authored ECS
// transforms are untouched. A legacy fallback uses the first directional light
// as the sun (and synthesises the moon) for worlds without the built-ins.
void RenderExtractor::applyDayNight(RenderWorld& out) const
{
	LightData* sunLight  = nullptr;
	LightData* moonLight = nullptr;
	for (LightData& l : out.lights)
	{
		if      (l.envRole == 1) sunLight  = &l;
		else if (l.envRole == 2) moonLight = &l;
	}
	if (!sunLight)
		for (LightData& l : out.lights)
			if (l.type == 0) { sunLight = &l; break; } // legacy: first directional

	glm::vec3 sunToward(0.45f, 0.80f, 0.55f); // default high sun
	// Weak ambient floor — always added so the scene is never fully black. Under
	// heavy cloud cover it grows to replace the (switched-off) sun/moon light.
	glm::vec3 ambient(0.08f, 0.09f, 0.13f);

	if (m_dayNight)
	{
		// 0.25 sunrise (+X horizon) → 0.5 noon (up) → 0.75 sunset (-X) → 0/1 night.
		const float a = (m_timeOfDay - 0.25f) * 6.28318530718f;
		sunToward = glm::normalize(glm::vec3(std::cos(a), std::sin(a), 0.45f));
		// The moon rides the opposite arc (same hemisphere z). The sun lights the
		// scene by day, the moon by night; each fades out as its own luminary dips
		// below the horizon, both keeping their own colour (no blend to one hue).
		const glm::vec3 moonToward =
			glm::normalize(glm::vec3(-sunToward.x, -sunToward.y, sunToward.z));
		const float sunUp  = std::clamp((sunToward.y  + 0.10f) / 0.25f, 0.0f, 1.0f);
		const float moonUp = std::clamp((moonToward.y + 0.10f) / 0.25f, 0.0f, 1.0f);

		// Cloud-cover optimisation: above a coverage threshold the direct sun/moon
		// light fades to zero (skipping its contribution + shadow lookup) and its
		// energy feeds a soft scattered ambient fill, tinted by whichever is up.
		const float cov      = std::clamp(m_cloudCoverage, 0.0f, 1.0f);
		const float overcast = glm::smoothstep(0.5f, 1.0f, cov);
		const float direct   = 1.0f - overcast;
		ambient += (m_sunColor  * (m_sunIntensity  * sunUp)
		          + m_moonColor * (m_moonIntensity * moonUp)) * (overcast * 0.22f);

		if (sunLight)
		{
			// Scene geometry gets neutral (luminance-only) direct light so the
			// sun's warm hue scatters only into the sky/clouds, not the terrain.
			const float lum = 0.299f * m_sunColor.r + 0.587f * m_sunColor.g + 0.114f * m_sunColor.b;
			sunLight->color     = glm::vec3(lum);
			sunLight->direction = -sunToward; // light travels away from the sun
			sunLight->intensity = m_sunIntensity * sunUp * direct;
		}
		if (moonLight)
		{
			moonLight->color     = m_moonColor;
			moonLight->direction = -moonToward;
			moonLight->intensity = m_moonIntensity * moonUp * direct;
		}
		else
		{
			// Legacy fallback: no built-in moon → synthesise one. (push_back may
			// reallocate out.lights, so the light pointers above are now stale.)
			LightData moon{};
			moon.type         = 0; // directional
			moon.direction    = -moonToward;
			moon.color        = m_moonColor;
			moon.intensity    = m_moonIntensity * moonUp * direct;
			moon.spotAngleCos = 1.0f;
			out.lights.push_back(moon);
		}
	}
	else
	{
		// Day-night cycle off: the sun shines from a fixed default direction with
		// the environment's sun colour/intensity; the moon is off.
		if (sunLight)
		{
			const float lum = 0.299f * m_sunColor.r + 0.587f * m_sunColor.g + 0.114f * m_sunColor.b;
			sunLight->color     = glm::vec3(lum);
			sunLight->direction = -sunToward;
			sunLight->intensity = m_sunIntensity;
		}
		if (moonLight)
			moonLight->intensity = 0.0f;
	}
	out.sunDirection = glm::normalize(sunToward);
	out.ambient      = ambient;
}

void RenderExtractor::extractUI(HorizonWorld& world, float vpWidth, float vpHeight,
                                RenderWorld& out)
{
	out.uiObjects.clear();
	UISystem::extract(world, vpWidth, vpHeight, out.uiObjects);
	// Live widgets (WidgetManager) append after the sorted entity UI, so they
	// always draw on top of it; internally sorted by (zOrder, layer, depth).
	world.widgets().extract(vpWidth, vpHeight, out.uiObjects);
}
