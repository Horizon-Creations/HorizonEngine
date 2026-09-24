#include "ViewportOverlays.h"

#include "EditorSelection.h"

#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <HorizonScene/Components/ColliderComponent.h>
#include <HorizonScene/Components/LODComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/TransformHierarchy.h>

#include <limits>
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
			if (!reg.all_of<TransformComponent>(sel)) continue;
			// WORLD position, where the entity's icon and mesh are drawn.
			// TransformComponent::position is local, and under a moved parent
			// the box stood beside the thing it marks. worldPositionOf rather
			// than worldMatrix: that is only as fresh as the last propagate.
			const glm::vec3 p = HE::worldPositionOf(world, sel);
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
		auto view = reg.view<ColliderComponent, TransformComponent>();
		for (Entity entity : view)
		{
			const auto&     col   = view.get<ColliderComponent>(entity);
			const glm::vec3 color = col.isTrigger ? kTriggerColor : kColliderColor;
			// The WORLD pose, the one PhysicsWorld builds the body at
			// (worldPoseOf), composed on the spot like the joints' lines are.
			// TransformComponent::position is local: under a moved parent the
			// outline stood where the parent's offset had taken the body away
			// from.
			const glm::mat4 worldM = HE::worldMatrixOf(world, entity);
			const glm::vec3 pos    = glm::vec3(worldM[3]);
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
				// Scaled like the shape itself is: PhysicsWorld bakes the
				// COMPOSED scale into the triangles, so a mesh under a scaled
				// parent collides at the parent's size too. The mesh box's
				// eight corners go through the world matrix and the outline
				// is their axis-aligned box — which also keeps a mirrored
				// axis from turning lo and hi around.
				const glm::vec3 lo = it->second.first, hi = it->second.second;
				glm::vec3 wlo(std::numeric_limits<float>::max());
				glm::vec3 whi(std::numeric_limits<float>::lowest());
				for (int c = 0; c < 8; ++c)
				{
					const glm::vec3 corner((c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y,
					                       (c & 4) ? hi.z : lo.z);
					const glm::vec3 w = glm::vec3(worldM * glm::vec4(corner, 1.0f));
					wlo = glm::min(wlo, w);
					whi = glm::max(whi, w);
				}
				out.aabb(wlo, whi, color);
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
