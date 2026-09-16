#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class HorizonWorld;

// Snapshot-based undo/redo for the editor world.
//
// Every undoable operation stores the *pre-mutation* world state (CBOR via
// SceneSerializer). Two usage patterns:
//
//   Structural ops (create/delete/reparent/add component, menu clicks):
//       undo.snapshotNow("Delete Entity");   // capture + push, then mutate
//
//   Continuous ImGui edits (drags, text inputs):
//       undo.capturePre();    // before the widget runs, every frame (cheap)
//       <widget>
//       if (ImGui::IsItemActivated())            undo.stashPre();
//       if (ImGui::IsItemDeactivatedAfterEdit()) undo.commitPending();
//
// Only one ImGui item can be active at a time, so a single pending slot is
// enough. Entity handles are remapped on restore — callers must reset their
// selection after undo()/redo().
//
// ── Labels ───────────────────────────────────────────────────────────────────
// Every entry carries a sentence for the Undo History window ("Move", "Light",
// "Delete Entity"). A caller that knows what it is doing passes one; the many
// that do not inherit the CONTEXT — the string the panel around them set with
// setContext() / a Context scope (the Details panel names the component whose
// rows are being drawn, the outliner names itself). Nothing set at all reads
// as "Edit", which is true and useless, so the scopes are worth keeping.
class EditorUndo
{
public:
	void setWorld(HorizonWorld* world) { m_world = world; }

	void capturePre();      // serialize the current world into the scratch slot
	// scratch → pending (edit session started). The label is decided NOW,
	// while the widget that started the edit is the current one — by the time
	// commitPending() runs, the panel may have moved on to another section.
	void stashPre(const char* label = nullptr);
	void commitPending();   // pending → undo stack (edit session finished)
	void snapshotNow(const char* label = nullptr);   // capture + push in one step

	// The label a push with no label of its own gets. Sticky until changed;
	// clear with "" (or let a Context scope unwind). See the note above.
	void setContext(const char* context) { m_context = context ? context : ""; }
	const std::string& context() const { return m_context; }
	// RAII form: the context for the rest of a block, restored on exit.
	class Context
	{
	public:
		Context(EditorUndo* undo, const char* context)
			: m_undo(undo), m_saved(undo ? undo->m_context : std::string())
		{ if (m_undo) m_undo->setContext(context); }
		~Context() { if (m_undo) m_undo->m_context = m_saved; }
		Context(const Context&) = delete;
		Context& operator=(const Context&) = delete;
	private:
		EditorUndo* m_undo;
		std::string m_saved;
	};

	bool undo();
	bool redo();
	bool canUndo() const { return !m_undoStack.empty(); }
	bool canRedo() const { return !m_redoStack.empty(); }

	// ── History, for the Undo History window ─────────────────────────────────
	// The undo stack oldest-first (index 0 = the first thing that can still be
	// undone, back() = what Ctrl+Z takes back next), the redo stack likewise
	// (back() = what Ctrl+Y brings back next). Each entry's label is the
	// operation it precedes: undoLabelAt(i) names what happened AFTER that
	// snapshot, which is what the user wants to read on the row.
	size_t undoDepth() const { return m_undoStack.size(); }
	size_t redoDepth() const { return m_redoStack.size(); }
	const std::string& undoLabelAt(size_t i) const { return m_undoStack[i].label; }
	const std::string& redoLabelAt(size_t i) const { return m_redoStack[i].label; }
	// What the next Ctrl+Z / Ctrl+Y would take back / bring back ("" = nothing).
	std::string undoLabel() const { return canUndo() ? m_undoStack.back().label : std::string(); }
	std::string redoLabel() const { return canRedo() ? m_redoStack.back().label : std::string(); }

	// Several steps in ONE restore: the intermediate states move between the
	// two stacks without ever being loaded into the world, which is what makes
	// clicking a row far up the history cost the same as one Ctrl+Z. Returns
	// whether anything moved. Clamped to what is there.
	bool undoSteps(size_t n);
	bool redoSteps(size_t n);

	// Monotonically increasing counter, bumped on every world mutation that
	// passes through undo (push/undo/redo). The editor compares it against the
	// value at the last save/load to know whether the scene is dirty.
	uint64_t revision() const { return m_revision; }

	// How many undos have been performed. Separate from revision() because redo
	// and push bump that too — the guided tour asks the user to *undo* something
	// and has to see exactly that, not any mutation.
	uint64_t undoCount() const { return m_undoCount; }

	void clearHistory();

private:
	using Snapshot = std::vector<uint8_t>;
	struct Entry
	{
		Snapshot    data;
		std::string label;
	};

	void restore(const Snapshot& snapshot);
	void pushUndo(Snapshot&& snapshot, std::string label);
	std::string resolveLabel(const char* label) const;

	static constexpr size_t kMaxEntries = 64;

	HorizonWorld* m_world = nullptr;
	Snapshot      m_scratch;
	Snapshot      m_pending;
	std::string   m_pendingLabel;
	bool          m_hasPending = false;
	std::string   m_context;
	uint64_t      m_revision   = 0;
	uint64_t      m_undoCount  = 0;
	std::vector<Entry> m_undoStack;
	std::vector<Entry> m_redoStack;
};
