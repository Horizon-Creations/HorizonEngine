#pragma once
#include <HorizonScene/Components/MaterialComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/HorizonWorld.h>
#include <Types/UUID.h>
#include <algorithm>
#include <vector>

namespace HE
{

// The static meshes some entity in `world` draws with a material of its own: the whole-mesh
// override (MaterialComponent::materialAssetId) or a per-slot one (slotOverrides). The export's
// texture bake rewrites a mesh's UVs onto an atlas, and an override material reads its OWN
// texture through those UVs, so it would show the wrong part of it. Such a mesh is left
// unbaked. Appends to `out` (no duplicates); the world is only read.
inline void collectMeshesWithMaterialOverrides(HorizonWorld& world, std::vector<HE::UUID>& out)
{
	auto view = world.registry().view<MeshComponent, MaterialComponent>();
	for (const auto entity : view)
	{
		const MaterialComponent& mat = view.get<MaterialComponent>(entity);
		if (mat.materialAssetId == HE::UUID{} && !mat.hasSlotOverride()) continue;
		const HE::UUID mesh = view.get<MeshComponent>(entity).meshAssetId;
		if (mesh == HE::UUID{}) continue;
		if (std::find(out.begin(), out.end(), mesh) == out.end()) out.push_back(mesh);
	}
}

} // namespace HE
