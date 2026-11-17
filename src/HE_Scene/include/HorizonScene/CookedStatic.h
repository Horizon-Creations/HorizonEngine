#pragma once
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <Math/AABB.h>
#include <Types/UUID.h>
#include <cstdint>
#include <cstring>
#include <vector>

// ── The cooked block of a static mesh ────────────────────────────────────────
// Entity cooking (docs/entity-cooking-survey-2026-11-13.md), second half: the
// entities analyzeCookability() cleared are held as flat rows, so the render
// extract emits their objects from this block instead of rebuilding each one
// from the registry every frame.
//
// A READ-ONLY MIRROR. The registry entity stays, with all its components, and
// its handle (entt::entity) keeps working for every reader. Nothing writes the
// block but the extract itself, and the un-cook functions below, which only ever
// send a row back to Pending: a value written to the entity afterwards (a moved
// transform, a component added by a script) takes it out of the block before the
// next extract reads it, so the walk shows the entity as it is now.
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
		// Set when a component that decides cookability (or what an ancestor passes on) was added to,
		// changed on or removed from a row's entity or a parent: the next refresh rebuilds, like an epoch
		// change would. Components come and go without the epoch moving (a script adds one).
		bool     stale = false;
		// The packed verdict of a scene that was exported with cooking on (SceneSerializer::saveToMemory,
		// the project setting): the entities analyzeCookability() cleared at export, as this load made them.
		// The first rebuild at `seedEpoch` takes them instead of analysing the world again, then drops them.
		// Rows born from it are Pending like any other: no matrix, no bounds, nothing a walk has not seen.
		// Any other epoch, or a verdict component changing before that rebuild, and the world is analysed as
		// ever. `enabled` rides along: a world loaded from such a scene asks its extractors to use the block
		// (RenderExtractor reads it next to HE_COOK_STATIC), so an old pack, which has no seed, draws as before.
		std::vector<entt::entity> seed;
		uint64_t seedEpoch = 0;
		bool     enabled   = false;
		// Rebuilds that took the seed / ran the analysis: a test and the log say which one a load got.
		uint32_t seededBuilds = 0;
		uint32_t analysedBuilds = 0;

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

		// ── Un-cooking: every way an entity leaves the block ───────────────────
		// A row goes back to Pending: the next walk reads the entity as it is now (and may settle it
		// again). All of these run on the thread that writes the world, before the extract, so the
		// frame that follows a mutation already shows it.

		// The entity's own row, for a write nothing else can see (the editor's inspector writes
		// fields raw). No row: nothing to do.
		void uncook(entt::entity e)
		{
			const int32_t r = rowOf(e);
			if (r >= 0) state[static_cast<size_t>(r)] = State::Pending;
		}

		// A world matrix was just written for `e` (propagateTransforms, the one writer): a Ready row that
		// holds another matrix is out of date. This is what catches every transform write at once, the
		// engine API, Lua/Python/HorizonCode, the gizmo, collaboration and a parent that moved, because
		// propagateTransforms notices a changed position/rotation/scale by value, whoever wrote it.
		void worldWritten(entt::entity e, const glm::mat4& w)
		{
			const int32_t r = rowOf(e);
			if (r >= 0 && state[static_cast<size_t>(r)] == State::Ready &&
			    std::memcmp(&world[static_cast<size_t>(r)], &w, sizeof(glm::mat4)) != 0)
				state[static_cast<size_t>(r)] = State::Pending;
		}

		// entt listeners (connected once, in cookedStaticOf). A component that changes the verdict
		// (the CookBlocker ones, Mobility) arrived on / left / was replaced on `e` or on a parent of
		// children: the block is rebuilt by the next refresh. Only the way OUT is watched: an entity that
		// left the block (a blocker came) is not looked for again when the blocker goes, it is walked as any
		// entity until the structure changes. Never stale, at worst not cooked; and a removal on every
		// uncooked entity would otherwise cost a rebuild. Raw writes through a pointer fire nothing;
		// those are covered by worldWritten and uncook.
		void onVerdictChanged(entt::registry& reg, entt::entity e);
		// A component the extract copies from (mesh, material) came, went or was replaced on a row.
		void onOutputChanged(entt::registry&, entt::entity e) { uncook(e); }

		// Every row Pending again, content bookkeeping updated. A no-op when the
		// manager and its epoch are the ones the rows were resolved against.
		void syncContent(const void* manager, uint64_t epoch);
	};

	// The block of a registry, kept in its context (one per world, shared by every
	// extractor that reads it). Created empty on first use.
	CookedStatic& cookedStaticOf(entt::registry& reg);

	// Un-cooks `e` if the registry has a block with a row for it. For a writer that changes an
	// entity's fields raw and has no way to tell anyone (the editor's inspector).
	void uncookEntity(entt::registry& reg, entt::entity e);

	// Refills the block from analyzeCookability(reg) when `structureEpoch` is not the
	// one it was built for, or `stale` is set: a row for every cookable entity with a TransformComponent
	// and a MeshComponent, all Pending. Costs nothing when the epoch matches.
	void refreshCookedStatic(entt::registry& reg, uint64_t structureEpoch);

	// Hands the block the cookable set a packed scene carried (see CookedStatic::seed), valid for
	// `structureEpoch`, which is the world's epoch right after the load. Creates the block, so its
	// listeners hear everything that happens to the world from here on.
	void seedCookedStatic(entt::registry& reg, std::vector<entt::entity>&& cookable, uint64_t structureEpoch);
}
