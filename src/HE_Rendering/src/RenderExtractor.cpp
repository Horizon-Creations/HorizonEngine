#include "HorizonRendering/RenderExtractor.h"
#include "HorizonRendering/RenderWorld.h"
#include <Renderer/IRenderer.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/MaterialComponent.h>
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/LightComponent.h>
#include <glm/gtc/quaternion.hpp>
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

	for (auto [e, t, mesh] : reg.view<TransformComponent, MeshComponent>().each())
	{
		RenderObject obj;
		obj.meshAssetId = mesh.meshAssetId;
		// Optional explicit material override (MaterialComponent). Backends use
		// this in place of the mesh's embedded material when it is set.
		if (const auto* matComp = reg.try_get<MaterialComponent>(e))
			obj.materialAssetId = matComp->materialAssetId;
		obj.transform   = t.worldMatrix;
		// Seed with the fallback cube's box; backends replace it with the
		// real mesh AABB once the asset is resolved.
		obj.worldBounds = kUnitCube.transformed(t.worldMatrix);
		obj.entityId    = static_cast<uint32_t>(e);
		obj.lod         = mesh.lodBias;
		out.objects.push_back(obj);
	}

	// ── Lights ──────────────────────────────────────────────────────────────
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
		out.lights.push_back(l);
	}

	// ── Sun direction (toward the sun) + optional day-night override ──────────
	// The sun is the first directional light. When the day-night cycle is on, the
	// time of day drives the sun's arc (and dims it at night) instead of the
	// authored light direction — render-time only, the ECS is untouched.
	LightData* sunLight = nullptr;
	for (LightData& l : out.lights)
		if (l.type == 0) { sunLight = &l; break; } // 0 = directional

	glm::vec3 sunToward(0.45f, 0.80f, 0.55f); // default high sun
	if (m_dayNight)
	{
		// 0.25 sunrise (+X horizon) → 0.5 noon (up) → 0.75 sunset (-X) → 0/1 night.
		const float a = (m_timeOfDay - 0.25f) * 6.28318530718f;
		sunToward = glm::normalize(glm::vec3(std::cos(a), std::sin(a), 0.45f));
		if (sunLight)
		{
			// Two independent directional lights — the warm sun and a cool moon
			// on the opposite arc. Each one is faded out as its own luminary dips
			// below the horizon, so the sun lights the scene by day and the moon
			// by night and both keep their own colour (no blend to a single hue).
			const glm::vec3 moonToward =
				glm::normalize(glm::vec3(-sunToward.x, -sunToward.y, sunToward.z));
			const float sunUp  = std::clamp((sunToward.y  + 0.10f) / 0.25f, 0.0f, 1.0f);
			const float moonUp = std::clamp((moonToward.y + 0.10f) / 0.25f, 0.0f, 1.0f);

			const float authoredInt = sunLight->intensity; // authored daylight intensity

			// Sun: authored colour/intensity, switched off once it has set.
			sunLight->direction = -sunToward; // light travels away from the sun
			sunLight->intensity = authoredInt * sunUp;

			// Moon: a second, cooler and dimmer directional light, off while down.
			// (push_back may reallocate out.lights, so sunLight must not be used
			// after this point.)
			LightData moon{};
			moon.type         = 0; // directional
			moon.direction    = -moonToward; // light travels away from the moon
			moon.color        = glm::vec3(0.55f, 0.65f, 0.95f);
			moon.intensity    = authoredInt * 0.30f * moonUp;
			moon.range        = 0.0f;
			moon.spotAngleCos = 1.0f;
			out.lights.push_back(moon);
			sunLight = nullptr;
		}
	}
	else if (sunLight)
	{
		sunToward = -glm::normalize(sunLight->direction);
	}
	out.sunDirection = glm::normalize(sunToward);

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
		const glm::vec3 center = sceneBox.isValid() ? sceneBox.center() : glm::vec3(0.0f);
		float radius = sceneBox.isValid() ? glm::length(sceneBox.extents()) : 10.0f;
		radius = std::max(radius, 1.0f);

		const glm::vec3 dir = glm::normalize(shadowLight->direction);
		const glm::vec3 up  = std::abs(dir.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
		const glm::vec3 eye = center - dir * (radius * 2.0f);
		const glm::mat4 view = glm::lookAt(eye, center, up);
		const glm::mat4 proj = glm::ortho(-radius, radius, -radius, radius, 0.05f, radius * 4.0f);
		out.shadow.viewProj  = proj * view;
		out.shadow.direction = dir;
		out.shadow.enabled   = true;
	}
}
