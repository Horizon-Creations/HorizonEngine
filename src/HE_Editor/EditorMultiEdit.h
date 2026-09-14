#pragma once
#include <HorizonScene/HorizonWorld.h> // Entity
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

// ── Editing several entities at once: the rule, without the widgets ─────────
// With more than one entity selected the Details panel draws the ACTIVE
// entity's rows for every component the whole selection shares, and an edit
// there is meant to land on all of them. The component editor is 2700 lines
// of hand-written widgets, one block per component; teaching each block about
// a list of entities would be a second copy of every row. Instead the panel
// asks the scene serializer for the active entity's component state before
// and after its widgets ran, works out WHICH leaves changed, and writes those
// same leaves into every other member that has the component. A component
// that gains a field is covered the moment it serializes.
//
// Absolute values, Unity-style: dragging Position X to 4 puts every member at
// X = 4. Only the leaves that changed travel — Y and Z stay whatever they were
// on each member, because a vec3 is diffed element by element, not as one
// value. Deliberately ImGui-free so the rules are answerable in a test.
namespace EditorMultiEdit
{
	using json = nlohmann::json;

	// One leaf that changed on the active entity: which component, where in
	// its serialized object (a JSON pointer relative to the component, "" for
	// the whole component), and the new value.
	struct Change
	{
		std::string         component;
		json::json_pointer  path;
		json                value;
	};

	// The component state the serializer writes for `e`, as a JSON object keyed
	// by component ("transform", "light", …). The display name is left out: it
	// belongs to the entity, is not offered in the multi-selection panel and
	// must never be copied from one entity onto another. Empty for a handle
	// the registry does not know.
	json state(HorizonWorld& world, Entity e);

	// The leaves that differ between two states. Objects are walked by key,
	// arrays of the same length element by element (so a vec3 drag on one axis
	// yields one change, not three); anything else that differs is a leaf.
	// Component keys present in `before` but absent in `after` are NOT
	// reported: a removal has no serialized shape to write and is handled by
	// the panel through the component's own remove path.
	std::vector<Change> diff(const json& before, const json& after);

	// Write `changes` into every entity of `members` except `primary`. A
	// member takes a change only when it has the component AND the path exists
	// in its own state (a shorter array, a missing sub-object: skipped, not
	// grown), and only the touched components go back through the serializer,
	// so a light edit never re-emplaces a member's mesh or script. Members the
	// registry no longer knows are skipped. `skip`, if given, vetoes a member
	// (the collab panel uses it for entities another participant holds).
	// Returns how many members were written to.
	int propagate(HorizonWorld& world, const std::vector<Change>& changes,
	              const std::vector<Entity>& members, Entity primary,
	              const std::vector<Entity>& skip = {});
}
