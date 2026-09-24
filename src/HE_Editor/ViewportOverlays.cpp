#include "ViewportOverlays.h"

#include "EditorSelection.h"

#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <HorizonScene/Components/ColliderComponent.h>
#include <HorizonScene/Components/LODComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/HorizonWorld.h>

#include <unordered_map>
#include <utility>

namespace HE::Ed::ViewportOverlays
{
	void appendSelectionMarkers(HorizonWorld& world, const EditorSelection& selection,
	                            DebugDrawBuffer& out)
	{
		auto& reg = world.registry();
		for (Entity sel : selection.entities())
		{
			if (!reg.valid(sel)) continue;
			auto* tc = reg.try_get<TransformComponent>(sel);
			if (!tc) continue;
			const glm::vec3 p = tc->position;
			const glm::vec3 color = (sel == selection.primary()) ? kPrimarySelectionColor
			                                                     : kSecondarySelectionColor;
			out.aabb(p - glm::vec3(0.5f), p + glm::vec3(0.5f), color);
		}
	}

	void appendColliderWireframes(HorizonWorld& world, ContentManager& content,
	                              DebugDrawBuffer& out)
	{
		auto& reg = world.registry();
		// Local-space box of a mesh asset, measured once and kept. The
		// mesh-shaped colliders below need it every frame, a loose editor
		// mesh carries no precomputed bounds, and measuring a hundred
		// thousand vertices per frame for a debug line is not a trade.
		static std::unordered_map<HE::UUID, std::pair<glm::vec3, glm::vec3>> s_colliderMeshBox;
		for (auto [entity, col, transform] :
		     reg.view<ColliderComponent, TransformComponent>().each())
		{
			const glm::vec3 color = col.isTrigger ? kTriggerColor : kColliderColor;
			const glm::vec3 pos = transform.position;
			switch (col.shape)
			{
			case ColliderShape::Box:
				out.aabb(pos - col.halfExtents, pos + col.halfExtents, color);
				break;
			case ColliderShape::Sphere:
				out.sphere(pos, col.radius, color);
				break;
			case ColliderShape::Capsule:
				out.capsule(pos, col.radius, col.height, color);
				break;
			case ColliderShape::Mesh:
			case ColliderShape::ConvexHull:
			{
				// These two take their geometry from the entity's MESH, so
				// the authored half extents describe nothing and the honest
				// outline is the mesh's own box. Same source PhysicsWorld
				// builds the shape from — LOD0 where there is a LODComponent,
				// because LODSystem rewrites MeshComponent's id as the camera
				// moves and a collider that changed with the camera would be
				// a different game at every distance.
				HE::UUID meshId{};
				if (const auto* mc = reg.try_get<MeshComponent>(entity))
					meshId = mc->meshAssetId;
				if (const auto* lod = reg.try_get<LODComponent>(entity);
				    lod && !lod->levels.empty())
					meshId = lod->levels.front().meshId;
				if (meshId == HE::UUID{}) break;

				auto it = s_colliderMeshBox.find(meshId);
				if (it == s_colliderMeshBox.end())
				{
					const StaticMeshAsset* mesh = content.getStaticMesh(meshId);
					if (!mesh) break;   // not loaded yet — measured on a later frame
					const bool        cooked = mesh->cooked && !mesh->interleaved.empty();
					const std::size_t count  = cooked ? mesh->vertexCount
					                                  : mesh->vertices.size() / 3;
					const std::size_t stride = cooked ? 8u : 3u;
					const float*      data   = cooked ? mesh->interleaved.data()
					                                  : mesh->vertices.data();
					if (count == 0 || (cooked && mesh->interleaved.size() < count * stride))
						break;
					glm::vec3 lo(data[0], data[1], data[2]);
					glm::vec3 hi = lo;
					for (std::size_t i = 1; i < count; ++i)
					{
						const glm::vec3 v(data[i * stride + 0], data[i * stride + 1],
						                  data[i * stride + 2]);
						lo = glm::min(lo, v);
						hi = glm::max(hi, v);
					}
					it = s_colliderMeshBox.emplace(meshId, std::make_pair(lo, hi)).first;
				}
				// Scaled like the shape itself is (PhysicsWorld bakes
				// transform.scale into the triangles). Axis-aligned, so a
				// rotated mesh reads as its box — the same simplification
				// the Box case above has always made.
				out.aabb(pos + it->second.first  * transform.scale,
				         pos + it->second.second * transform.scale, color);
				break;
			}
			case ColliderShape::HeightField:
				// Deliberately nothing: the height field IS the landscape
				// mesh in the viewport, and a box around a whole terrain
				// would hide the scene inside it.
				break;
			default:
				// An enum value this build does not know. Silent here on
				// purpose — PhysicsWorld logs it once where it matters, and
				// a debug overlay is not the place to repeat that per frame.
				break;
			}
		}
	}
}
