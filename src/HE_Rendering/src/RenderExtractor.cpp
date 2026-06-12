#include "HorizonRendering/RenderExtractor.h"
#include "HorizonRendering/RenderWorld.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/LightComponent.h>
#include <glm/gtc/quaternion.hpp>

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

void RenderExtractor::extract(HorizonWorld& world, RenderWorld& out, float aspectRatio)
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
	// Prefer the camera marked isMain, fall back to the first one found,
	// then to a fixed editor default so an empty scene still renders sanely.
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

	// ── Renderables ─────────────────────────────────────────────────────────
	for (auto [e, t, mesh] : reg.view<TransformComponent, MeshComponent>().each())
	{
		RenderObject obj;
		obj.meshAssetId = mesh.meshAssetId;
		obj.transform   = t.worldMatrix;
		obj.entityId    = static_cast<uint32_t>(e);
		obj.lod         = mesh.lodBias;
		out.objects.push_back(obj);
	}

	// ── Lights ──────────────────────────────────────────────────────────────
	for (auto [e, t, light] : reg.view<TransformComponent, LightComponent>().each())
	{
		LightData l;
		l.position  = glm::vec3(t.worldMatrix[3]);
		l.color     = light.color;
		l.intensity = light.intensity;
		l.type      = static_cast<uint8_t>(light.type);
		out.lights.push_back(l);
	}
}
