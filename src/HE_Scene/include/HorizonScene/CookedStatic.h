#pragma once
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <Math/AABB.h>
#include <Types/UUID.h>
#include <cstdint>
#include <vector>

// ── The cooked block of a static mesh ────────────────────────────────────────
// Entity cooking (docs/entity-cooking-survey-2026-11-13.md), second half: the
// entities analyzeCookability() cleared are held as flat rows, so the render
// extract emits their objects from this block instead of rebuilding each one
// from the registry every frame.
//
// A READ-ONLY MIRROR. The registry entity stays, with all its components, and
// its handle (entt::entity) keeps working for every reader. Nothing writes the
// block but the extract itself; a value written to the entity afterwards (a
// moved transform, a swapped material) is NOT seen by the rows until the
// entity is un-cooked (the next step) or the world's structure changes.
// What is read live on purpose, never cached here: MeshComponent::visible and
// the Active switch, because several call sites write the first one raw.
//
// Rows are born Pending and settled by the extract's own walk: the first walk
// after a rebuild builds the object the ordinary way, and a row whose result is
// final (a one-section mesh that is resident, no per-entity params, not water,
// not a terrain chunk) keeps exactly what that walk produced and goes Ready.
// So the fast path can only repeat what the walk wrote, field for field.
//
// One source of truth for "is this block current": the world's structureEpoch.
// A rebuild follows its change; a content change (ContentManager::contentEpoch,
// a mesh edited or imported) sends every Ready row back to Pending.
namespace HE
{
	struct CookedStatic
	{
		enum class State : uint8_t
		{
			Pending,   // not resolved yet (mesh not resident, or no walk since the rebuild): walked as usual
			Ready,     // the row holds the object; the extract copies it
			Walk,      // never fast: multi-section mesh, own params, water, terrain chunk. Walked as usual
		};

		// 0 = never built. HorizonWorld::structureEpoch() starts at 1.
		uint64_t structureEpoch = 0;
		// What the Ready rows were resolved against; a different manager or epoch resets them.
		const void* content      = nullptr;
		uint64_t    contentEpoch = 0;

		// Entity index -> row, -1 = not cooked. Sized once per rebuild.
		std::vector<int32_t> rowOfIndex;

		// The rows (SoA), in the order of the mesh view, so an all-cooked scene
		// extracts in the order an uncooked one does.
		std::vector<entt::entity> entity;
		std::vector<State>        state;
		std::vector<glm::mat4>    world;
		std::vector<HE::AABB>     worldBounds;
		std::vector<HE::UUID>     meshId;
		std::vector<HE::UUID>     materialId;
		std::vector<uint8_t>      lod;
		std::vector<uint8_t>      castsShadow;
		std::vector<uint8_t>      receivesShadow;

		// The row of `e`, or -1. The entity itself is compared as well: an index
		// reused by a younger entity must not read an older one's row.
		int32_t rowOf(entt::entity e) const
		{
			const size_t i = static_cast<size_t>(entt::to_entity(e));
			if (i >= rowOfIndex.size()) return -1;
			const int32_t r = rowOfIndex[i];
			return (r >= 0 && entity[static_cast<size_t>(r)] == e) ? r : -1;
		}
		size_t size() const { return entity.size(); }

		// Every row Pending again, content bookkeeping updated. A no-op when the
		// manager and its epoch are the ones the rows were resolved against.
		void syncContent(const void* manager, uint64_t epoch);
	};

	// The block of a registry, kept in its context (one per world, shared by every
	// extractor that reads it). Created empty on first use.
	CookedStatic& cookedStaticOf(entt::registry& reg);

	// Refills the block from analyzeCookability(reg) when `structureEpoch` is not the
	// one it was built for: a row for every cookable entity with a TransformComponent
	// and a MeshComponent, all Pending. Costs nothing when the epoch matches.
	void refreshCookedStatic(entt::registry& reg, uint64_t structureEpoch);
}
