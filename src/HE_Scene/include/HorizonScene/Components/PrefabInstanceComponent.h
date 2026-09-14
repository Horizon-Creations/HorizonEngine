#pragma once
#include <Types/UUID.h>
#include <algorithm>
#include <string>
#include <vector>

// ─── A placed prefab, and what still ties it to its asset ────────────────────
// Placing a prefab is a COPY: `SceneSerializer::instantiatePrefab` mints fresh
// identities for every entity in the blob, which is what makes the same prefab
// placed twice two things rather than one thing claiming two places. For a
// while the copy carried nothing but the uuid of the asset it came from
// (`PrefabLinkComponent`, "which lamp post came out of Prefabs/Lamp.hasset");
// this is that link grown into an instance: it still names the asset, and it
// now also records which entity of the placement corresponds to which record
// of the template, and which properties a human has edited since placing it.
//
// Written on the ROOT of the placement only. The children are part of the
// instance, not instances of their own — `prefab_instances` counts instances
// by walking this component, and a child that is itself a placement of ANOTHER
// prefab carries its own (nested instances are allowed and were before).
//
// ── Bindings: which entity is which template record ──────────────────────────
// The template blob gives every record a `uuid` (buildSubtreeJson writes the
// captured entity's id). Instantiation mints new ids, so without a table the
// instance could not say "this Light IS the template's Light" — and neither
// propagation (re-applying the asset after it changed) nor an override ("the
// Light's intensity here is mine") has anything to hang on. A binding is that
// pair: the record's uuid on the template side, the placed entity's uuid
// (EntityIdComponent) on the instance side. The root is bound too.
//
// A binding whose instance entity no longer exists means the child was deleted
// from the placement — that is an override in its own right, not a broken
// table, and a reader must not repair it by dropping the entry. A binding to
// the NULL uuid says the same thing outright: "no counterpart here" — written
// when an instance older than this table is adopted and a record cannot be
// matched to a child unambiguously, so that propagation neither creates it nor
// keeps looking. A binding whose template record no longer exists in the asset
// means the asset changed (or the instance is older than this table):
// "unbound", never "delete it".
//
// ── Overrides: what a human authored on the placement ────────────────────────
// An override is a MARKER, not a value: `{ template entity, component key,
// property key }` says "this property on this entity was edited here, do not
// propagate the asset's value over it". The instance holds the live value in
// its component, the asset holds the template's; storing a third copy would
// only give the three a chance to disagree. Keys are the scene format's own:
// `component` is the key the serializer writes ("transform", "light"),
// `property` the top-level key inside that block ("intensity"), because that
// is the granularity the serializer can diff and re-apply. An empty `property`
// covers the whole component (added here, or removed here).
//
// Keyed by the TEMPLATE entity, not the instance entity, so a duplicate, an
// undo/redo or a peer's copy — all of which re-mint instance ids — keep every
// override without translation. The root's transform is the placement itself
// and is by convention never propagated, so it needs no override to be safe.
//
// ── What the serializer promises, and what it does not ───────────────────────
// Save/load round-trips all three fields (key "prefab" in the components
// block; an old scene without bindings/overrides loads with both empty).
// applyPrefabJson re-points bindings at the freshly minted entities whenever a
// blob carrying this component is instantiated, so a duplicate binds to ITS
// children, not the original's. Propagation is `SceneSerializer::
// syncPrefabInstance`: it re-applies the asset's records to the bound entities,
// keeps whatever the override list names (the display name is addressed as
// component "__name"), creates records that have no binding yet, and adopts
// an instance without bindings by binding what it can and recording every
// difference as an override — the editor runs it over the whole scene on open
// and before save. The Inspector's revert and push-to-prefab are the remaining
// readers, and push-to-prefab in particular must translate instance ids back
// to template ids through the bindings rather than re-serialise the instance
// verbatim — or every binding in the project breaks on the first push.
//
// ── Why a uuid and not a path ────────────────────────────────────────────────
// The same reason every other asset reference in a scene is one: a path in a
// scene file breaks the moment the asset is moved, and `AssetRefScan` finds
// asset references in a `.hescene` by walking for [hi, lo] pairs INSIDE a
// "components" block — so an instance is found by the delete/move dialogs for
// free, and only because the id sits where every other asset id sits. The
// entity uuids in the bindings sit in the same block; the scan matches against
// the asset it was asked about, so they can never answer for one.
struct PrefabInstanceComponent {
	struct Binding {
		HE::UUID templateEntity;   // the record's uuid in the prefab blob
		HE::UUID instanceEntity;   // EntityIdComponent of the placed entity
		bool operator==(const Binding&) const = default;
	};
	struct Override {
		HE::UUID    templateEntity; // which record of the template (see bindings)
		std::string component;      // scene-format component key ("transform")
		std::string property;       // top-level key inside it; empty = whole component
		bool operator==(const Override&) const = default;
	};

	HE::UUID              asset;     // the Prefab asset this entity was instantiated from
	std::vector<Binding>  bindings;  // template record ↔ placed entity, root included
	std::vector<Override> overrides; // locally authored properties, keyed by template entity

	// ── Lookups ──────────────────────────────────────────────────────────────
	// Null uuid when unbound. Linear: a prefab is a handful of entities.
	HE::UUID instanceOf(const HE::UUID& templateEntity) const
	{
		for (const Binding& b : bindings)
			if (b.templateEntity == templateEntity) return b.instanceEntity;
		return {};
	}
	HE::UUID templateOf(const HE::UUID& instanceEntity) const
	{
		for (const Binding& b : bindings)
			if (b.instanceEntity == instanceEntity) return b.templateEntity;
		return {};
	}

	// ── Override set ─────────────────────────────────────────────────────────
	// A whole-component override (empty property) covers every property of that
	// component, so hasOverride answers true for "light"/"intensity" when
	// "light"/"" is recorded. setOverride is idempotent; clearOverride removes
	// exactly the entry named, and with an empty property every entry of that
	// component. Order of insertion is kept — it is what the Inspector lists.
	bool hasOverride(const HE::UUID& templateEntity, const std::string& component,
	                 const std::string& property = {}) const
	{
		for (const Override& o : overrides)
			if (o.templateEntity == templateEntity && o.component == component &&
			    (o.property.empty() || o.property == property))
				return true;
		return false;
	}
	bool setOverride(const HE::UUID& templateEntity, std::string component,
	                 std::string property = {})
	{
		for (const Override& o : overrides)
			if (o.templateEntity == templateEntity && o.component == component &&
			    o.property == property)
				return false;
		overrides.push_back({ templateEntity, std::move(component), std::move(property) });
		return true;
	}
	bool clearOverride(const HE::UUID& templateEntity, const std::string& component,
	                   const std::string& property = {})
	{
		const size_t before = overrides.size();
		overrides.erase(std::remove_if(overrides.begin(), overrides.end(),
			[&](const Override& o)
			{
				return o.templateEntity == templateEntity && o.component == component &&
				       (property.empty() || o.property == property);
			}), overrides.end());
		return overrides.size() != before;
	}
};
