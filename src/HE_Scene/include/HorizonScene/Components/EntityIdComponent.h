#pragma once
#include <Types/UUID.h>

// ─── Stable identity for an entity ───────────────────────────────────────────
// An entt handle identifies an entity only within one registry instance and only
// for as long as it lives: the allocator hands out dense indices, so two worlds
// that created one entity each both call it 0, and a destroyed handle is reused.
// That is fine in memory and wrong the moment identity has to survive a file.
//
// Why it matters concretely — scenes are pretty-printed JSON, so git merges them
// line by line. Two people who each add an entity on their own branch both get
// the same next handle; the added blocks do not overlap textually, so the merge
// succeeds with no conflict and produces one file where two entities claim the
// same id. Everything that referenced that number — including hierarchy links
// from entities neither person touched — then resolves to whichever entity
// happened to be created last. The file parses and loads without a single error.
//
// A UUID minted at creation removes that failure by construction: two entities
// created independently can never collide, so the merged file is simply correct.
//
// Minted in HorizonWorld::createEntity (and for the world root in the
// constructor), so every entity has one without any caller doing anything. Scene
// LOADING restores the serialised value — identity must survive a save/load
// round trip. Prefab INSTANTIATION deliberately does not: a prefab is a template,
// and keeping the freshly minted id is what stops the same prefab inserted twice
// from producing two entities with one identity.
struct EntityIdComponent {
	HE::UUID id;
};
