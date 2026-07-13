#include "HorizonRendering/RenderExtractor.h"
#include "HorizonRendering/RenderWorld.h"
#include <Diagnostics/Profiler.h>
#include <Renderer/IRenderer.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/SkeletalMeshComponent.h>
#include <HorizonScene/Components/MaterialComponent.h>
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/LightComponent.h>
#include <HorizonScene/Components/EnvironmentLightComponent.h>
#include <HorizonScene/Components/ParticleSystemComponent.h>
#include <HorizonScene/Components/WeatherComponent.h>
#include <HorizonScene/Components/FoliageComponent.h>
#include <HorizonScene/UISystem.h>
#include <ContentManager/DefaultAssets.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <JobSystem/JobSystem.h>
#include <glm/gtc/quaternion.hpp>
#include <glm/common.hpp>
#include <algorithm>
#include <cmath>

namespace
{
	glm::mat4 localMatrix(const TransformComponent& t)
	{
		glm::quat q = glm::quat(glm::radians(t.rotation));
		return glm::translate(glm::mat4(1.0f), t.position)
		     * glm::mat4_cast(q)
		     * glm::scale(glm::mat4(1.0f), t.scale);
	}

	// Recompute world matrices top-down from the world root. Note this does
	// NOT use SceneGraph::propagateTransforms — that only visits entities
	// with parent == entt::null, but HorizonWorld parents everything to a
	// root *entity*, so it never fires. Recomputing every frame is cheap at
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
}

void RenderExtractor::extract(HorizonWorld& world, RenderWorld& out, float aspectRatio,
                              const EditorCameraOverride* editorCam)
{
	HE_PROFILE_SCOPE_N("RenderExtractor::extract");
	out.clear();
	auto& reg = world.registry();

	// ── Transforms ──────────────────────────────────────────────────────────
	propagateFrom(reg, world.rootEntity(), glm::mat4(1.0f));
	// Entities outside the root hierarchy (no HierarchyComponent)
	for (auto [e, t] : reg.view<TransformComponent>(entt::exclude<HierarchyComponent>).each())
	{
		t.worldMatrix = localMatrix(t);
		t.dirty       = false;
	}

	// ── Camera ──────────────────────────────────────────────────────────────
	// Editor scene view wins when active. Otherwise prefer the camera marked
	// isMain, fall back to the first one found, then to a fixed editor default
	// so an empty scene still renders sanely.
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

	// ── Renderables ─────────────────────────────────────────────────────────
	static const HE::AABB kUnitCube = []{
		HE::AABB b;
		b.expand({ -0.5f, -0.5f, -0.5f });
		b.expand({  0.5f,  0.5f,  0.5f });
		return b;
	}();

	// Two-phase build: sequential ECS read (no EnTT concurrency guarantees), then
	// parallel AABB/transform computation (pure math, no registry access).
	struct EntityData {
		glm::mat4 world;
		HE::UUID  meshId;
		HE::UUID  matId;
		uint32_t  entId;
		int       lod;
		bool      castsShadow;
		HE::AABB  localBounds; // real mesh AABB, or invalid → unit-cube fallback
		std::vector<float> paramOverride; // merged HeParams block, or empty
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
			if (!matComp->paramOverrides.empty() && m_contentManager)
				if (const MaterialAsset* ma = m_contentManager->getMaterial(d.matId))
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
		d.entId  = static_cast<uint32_t>(e);
		d.lod    = mesh.lodBias;
		d.castsShadow = mesh.castsShadow;
		// Look up the real object-space bounds here (sequential — the
		// ContentManager read must not race the parallel_for below).
		if (m_contentManager)
			if (const StaticMeshAsset* m = m_contentManager->getStaticMesh(d.meshId))
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
	});

	// ── ParticleGraph particles ────────────────────────────────────────────────
	// One ParticleBatch per emitter: raw position/size/t01 per particle, GPU-
	// instanced (see HorizonRendering::ParticleShaderTemplates) — the backend picks
	// a shader baked from `config`'s color/alpha-over-life endpoints (compiled once,
	// cached by content hash, not per-particle CPU work). Size stays CPU-lerped (one
	// scalar; moving it to the GPU buys nothing — see ParticleShaderGen's comment).
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

	// ── Weather precipitation ──────────────────────────────────────────────────
	// Turns each live precip drop into a camera-facing billboard. The per-particle
	// cost is the billboard basis (cross products + normalises), so we first gather
	// lightweight inputs (cheap, serial) and then build the matrices across all
	// cores — the same gather→parallel_for pattern as the mesh path. At high
	// precipitation caps this turns a serial tens-of-thousands stall into a
	// parallel sweep. Same-emitter particles keep one mesh+material so the geometry
	// pass still instances them.
	enum class BillboardKind : uint8_t { Snow, RainStreak };
	struct ParticleInput {
		glm::vec3     position;
		glm::vec3     velocity;   // RainStreak only
		HE::UUID      meshId;
		HE::UUID      matId;
		uint32_t      entityId;
		BillboardKind kind;
	};
	std::vector<ParticleInput> pin;

	for (auto [e, wx] : reg.view<WeatherComponent>().each())
	{
		if (wx.precip.empty()) continue;
		const bool isSnow = (wx.curPrecipType == PrecipType::Snow);
		// Snow uses the star flake mesh; rain uses the quad stretched into a streak.
		const HE::UUID      precipMesh = isSnow ? HE::kDefaultSnowflakeMeshId : HE::kDefaultQuadMeshId;
		const BillboardKind kind       = isSnow ? BillboardKind::Snow : BillboardKind::RainStreak;
		pin.reserve(pin.size() + wx.precip.size());
		for (const Particle& p : wx.precip)
			pin.push_back({ p.position, p.velocity, precipMesh, HE::UUID{},
			                static_cast<uint32_t>(e), kind });
	}

	if (!pin.empty())
	{
		const size_t   base   = out.objects.size();
		const glm::vec3 camPos = out.camera.position;
		out.objects.resize(base + pin.size());
		parallel_for(pin.size(), [&](size_t i) {
			const ParticleInput& in  = pin[i];
			RenderObject&        obj = out.objects[base + i];
			obj.meshAssetId     = in.meshId;
			obj.materialAssetId = in.matId;
			obj.entityId        = in.entityId;
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
			else if (in.kind == BillboardKind::RainStreak)
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

	// ── Foliage ──────────────────────────────────────────────────────────────
	// Each cached foliage instance is pushed as a RenderObject. Because all
	// instances share the same meshAssetId, the geometry pass batches them into
	// one DrawCall with instanceTransforms automatically.
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

	// ── Skinned renderables ─────────────────────────────────────────────────
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

	// ── Lights ──────────────────────────────────────────────────────────────
	out.lights.reserve(reg.view<LightComponent>().size() + 1); // +1 for the day-night moon
	for (auto [e, t, light] : reg.view<TransformComponent, LightComponent>().each())
	{
		if (!light.visible) continue; // hidden (e.g. a preloaded zone)
		LightData l;
		l.position     = glm::vec3(t.worldMatrix[3]);
		// Lights shine along their local -Z (third column of the world matrix)
		l.direction    = -glm::normalize(glm::vec3(t.worldMatrix[2]));
		l.color        = light.color;
		l.intensity    = light.intensity;
		l.range        = light.range;
		l.spotAngleCos = std::cos(glm::radians(light.spotAngle * 0.5f));
		l.type         = static_cast<uint8_t>(light.type);
		if (const auto* env = reg.try_get<EnvironmentLightComponent>(e))
			l.envRole = (env->role == EnvironmentLightComponent::Role::Sun) ? 1 : 2;
		out.lights.push_back(l);
	}

	// ── Environment sun + moon (day-night) ────────────────────────────────────
	// The sun and moon are the two built-in directional lights tagged by the
	// EnvironmentComponent (envRole 1/2). The environment drives their colour,
	// intensity and (day-night) arc direction — render-time only, the authored ECS
	// transforms are untouched. A legacy fallback uses the first directional light
	// as the sun (and synthesises the moon) for worlds without the built-ins.
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

	// ── Directional-light shadow view-projection ─────────────────────────────
	// The brightest directional light casts shadows (so the single shadow map
	// follows the sun by day and the moon by night). The ortho frustum is fitted
	// around the union of the (seeded) object bounds — backends refine bounds
	// elsewhere, but this rough fit is enough for a single full-scene shadow map.
	out.shadow.enabled = false;
	const LightData* shadowLight = nullptr;
	for (const LightData& l : out.lights)
	{
		if (l.type != 0) continue; // 0 = directional
		if (!shadowLight || l.intensity > shadowLight->intensity)
			shadowLight = &l;
	}
	if (shadowLight && shadowLight->intensity > 0.1f)
	{
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
		// right/up axes in whole-texel steps (kShadowMapRes must match the
		// backends' shadow map resolution).
		constexpr float kShadowMapRes = 2048.0f;
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
		constexpr float kCascadeRes    = 2048.0f;

		// Camera near/far from the (glm, z∈[-1,1]) projection matrix.
		const glm::mat4& P = out.camera.projection;
		const float camN = P[3][2] / (P[2][2] - 1.0f);
		const float camF = P[3][2] / (P[2][2] + 1.0f);
		const float shadowFar = std::min(std::max(camF, camN + 1.0f), kShadowDistance);

		float splitD[ShadowData::kMaxCascades + 1];
		splitD[0] = camN;
		for (int i = 1; i <= kCascadeCount; ++i)
		{
			const float pf   = static_cast<float>(i) / static_cast<float>(kCascadeCount);
			const float logS = camN * std::pow(shadowFar / camN, pf);
			const float uniS = camN + (shadowFar - camN) * pf;
			splitD[i] = kLambda * logS + (1.0f - kLambda) * uniS;
		}

		// Stable per-cascade sphere fit (jitter-free → no shadow swim): the bounding
		// sphere of a frustum slice depends ONLY on fov/aspect/splits, not the camera
		// pose, so its radius is constant frame-to-frame → constant texel size. The
		// centre rides the camera forward axis. Texel-snapping is done in shadow-clip
		// space (round the projected world origin to a whole texel) — the robust form
		// that keeps shadows locked to the world as the cascade centre moves.
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
			// Sphere through the slice's near + far corner rings, centre on view axis.
			const float xn = nC * thfX, yn = nC * thfY;
			const float xf = fC * thfX, yf = fC * thfY;
			const float aa = xn * xn + yn * yn;
			const float bb = xf * xf + yf * yf;
			float zc = (bb - aa + fC * fC - nC * nC) / (2.0f * (fC - nC));
			zc = glm::clamp(zc, nC, fC);
			float crad = std::sqrt(std::max(aa + (nC - zc) * (nC - zc),
			                                bb + (fC - zc) * (fC - zc)));
			crad = std::max(crad, 0.01f);
			crad = std::ceil(crad * 16.0f) / 16.0f;   // quantise → texel quantum stays
			                                          // stable across small fov/aspect drift
			const glm::vec3 ccenter = camPos + camFwd * zc;

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
			const glm::mat4 vp = cProj * cView;
			const glm::vec4 o  = vp * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f); // ortho → w = 1
			const float halfRes = kCascadeRes * 0.5f;
			const glm::vec2 so  = glm::vec2(o.x, o.y) * halfRes;        // origin in texels
			const glm::vec2 off = (glm::round(so) - so) / halfRes;      // sub-texel NDC fix
			cProj[3][0] += off.x;
			cProj[3][1] += off.y;

			out.shadow.cascadeViewProj[c] = cProj * cView;
			out.shadow.cascadeSplit[c]    = fC;   // view-space far distance
		}
	}
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
