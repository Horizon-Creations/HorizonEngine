#include "HorizonRendering/RenderExtractor.h"
#include "HorizonRendering/RenderWorld.h"
#include <Renderer/IRenderer.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/MaterialComponent.h>
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/LightComponent.h>
#include <HorizonScene/Components/EnvironmentLightComponent.h>
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
	};
	auto meshView = reg.view<TransformComponent, MeshComponent>();
	std::vector<EntityData> items;
	items.reserve(meshView.size_hint());
	for (auto [e, t, mesh] : meshView.each())
	{
		EntityData d;
		d.world  = t.worldMatrix;
		d.meshId = mesh.meshAssetId;
		d.matId  = {};
		if (const auto* matComp = reg.try_get<MaterialComponent>(e))
			d.matId = matComp->materialAssetId;
		d.entId  = static_cast<uint32_t>(e);
		d.lod    = mesh.lodBias;
		items.push_back(d);
	}

	out.objects.resize(items.size());
	parallel_for(items.size(), [&](size_t i) {
		const EntityData& d = items[i];
		RenderObject&   obj = out.objects[i];
		obj.meshAssetId     = d.meshId;
		obj.materialAssetId = d.matId;
		obj.transform       = d.world;
		obj.worldBounds     = kUnitCube.transformed(d.world);
		obj.entityId        = d.entId;
		obj.lod             = d.lod;
	});

	// ── Lights ──────────────────────────────────────────────────────────────
	out.lights.reserve(reg.view<LightComponent>().size() + 1); // +1 for the day-night moon
	for (auto [e, t, light] : reg.view<TransformComponent, LightComponent>().each())
	{
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
	glm::vec3 ambient(0.03f, 0.035f, 0.05f);

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
			sunLight->color     = m_sunColor;
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
			sunLight->color     = m_sunColor;
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
	if (shadowLight && shadowLight->intensity > 1e-4f)
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
		out.shadow.viewProj  = proj * view;
		out.shadow.direction = dir;
		out.shadow.enabled   = true;
	}
}
