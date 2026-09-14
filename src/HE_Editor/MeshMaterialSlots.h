#pragma once
#include "EditorWidgets.h"   // SlotAction — what the slot widget reports back
#include <ContentManager/Assets.h>
#include <Types/UUID.h>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

class ContentManager;

// ── The material slots of a mesh asset, as the mesh editors edit them ────────
// A mesh carries one MeshSection per material slot (Assets.h). Until now the
// static-mesh tab printed the mesh's one materialPath read-only and the
// skeletal tab printed nothing; this is the list both tabs show instead — one
// row per section, each with a material picker — and the write-back that
// keeps the asset's two ways of naming a material in step.
//
// Shared between the two panels AND kept free of AppContext on purpose: the
// panels hand in the widget that draws a slot (assetDropSlot, which needs the
// context for its picker and drag-drop), and the headless UI-shot test hands in
// a plain button. Everything that can be wrong about the list — which row
// mirrors into the mesh-level material, what an empty slot means, what undo
// restores — is in here, where a test can reach it without a GPU.
namespace HE::Ed::MeshMaterialSlots
{
	// The references of one mesh, lifted out of the asset. `sections` is never
	// empty: an asset without a table (a primitive) is read as the one section
	// meshSectionsOf() describes, and written back as exactly that.
	//
	// `ownPath`/`ownId` is StaticMeshAsset::materialPath/materialId — the mesh's
	// own material, which the section-unaware draw paths still resolve for the
	// whole mesh. The asset's contract (Assets.h) is that it equals slot 0, and
	// setSlot() is where the editor keeps that promise.
	struct Table
	{
		std::vector<MeshSection> sections;
		std::string              ownPath;
		HE::UUID                 ownId;

		bool operator==(const Table& o) const;
		bool operator!=(const Table& o) const { return !(*this == o); }
	};

	template<typename Mesh>
	Table tableOf(const Mesh& mesh)
	{
		Table t;
		t.sections = HE::meshSectionsOf(mesh);
		t.ownPath  = mesh.materialPath;
		t.ownId    = mesh.materialId;
		return t;
	}

	template<typename Mesh>
	void applyTo(const Table& t, Mesh& mesh)
	{
		mesh.sections     = t.sections;
		mesh.materialPath = t.ownPath;
		mesh.materialId   = t.ownId;
	}

	// Point slot `index` at material `id`. A zero id clears the slot, which for
	// slot 0 means "no material" and for any other slot "the mesh's own
	// material" (see MeshSection). The reference is written as a PATH: the
	// editor edits loose content, and the packer bakes the UUID in its place.
	// Slot 0 is mirrored into ownPath/ownId. Returns false when `id` is not a
	// loaded material (the table is then untouched).
	bool setSlot(ContentManager& cm, Table& t, size_t index, HE::UUID id);

	// Per-tab edit state: the undo stack over the table, plus the slot paths
	// that were tried and found missing on disk — so a slot pointing at a
	// deleted material is looked for once, not sixty times a second.
	//
	// The stack holds whole tables, seeded with the as-loaded one (the undo
	// floor); `pos` indexes the table currently in the asset. The
	// MaterialEditorPanel pattern, transplanted: the world's undo system
	// snapshots entities, not assets, so a slot edit has to remember itself.
	struct Session
	{
		std::vector<Table>              undo;
		int                             undoPos = -1;
		std::unordered_set<std::string> missing;

		void seed(const Table& t);        // the floor — no-op once seeded
		void push(const Table& t);        // after an edit; drops the redo tail
		bool canUndo() const { return undoPos > 0; }
		bool canRedo() const { return undoPos >= 0 && undoPos + 1 < static_cast<int>(undo.size()); }
		bool step(int dir, Table& out);   // -1 undo / +1 redo; false at either end
	};

	// The UUID slot `sec` currently resolves to, for the picker: the baked id
	// when it carries one, else its path looked up (and loaded on first sight —
	// the picker can only show what is resident). Zero for an empty slot and
	// for a path nothing on disk answers to.
	HE::UUID slotId(ContentManager& cm, const MeshSection& sec, Session& session);

	// Draws the picker for one slot: the button, drop target and Clear of
	// assetDropSlot in the panels; anything at all in a test. `emptyText` is
	// what to show for a zero target, which differs per row.
	using SlotWidget = std::function<EditorWidgets::SlotAction(
		size_t index, HE::UUID& target, const char* emptyText)>;

	// The "Materials" block: the heading and one row per slot. `table` is a
	// COPY the caller lifted out of the asset this frame — the widget may load
	// an asset, and a load moves every mesh pointer (ContentManager.h). Returns
	// true when the table changed, in which case the caller writes it back to
	// the (re-fetched) mesh and saves; the session has already recorded it.
	bool draw(ContentManager* cm, Table& table, Session& session, const SlotWidget& slot);

	// Cmd/Ctrl+Z / Shift+Z / Y over the panel: steps the session and writes the
	// restored table into `table`. Call it inside the host window, after the
	// panes — IsWindowHovered(ChildWindows) is what decides it belongs here.
	// Returns true when `table` changed.
	bool handleUndoKeys(Table& table, Session& session);
}
