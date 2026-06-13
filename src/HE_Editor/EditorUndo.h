#pragma once
#include <cstdint>
#include <vector>

class HorizonWorld;

// Snapshot-based undo/redo for the editor world.
//
// Every undoable operation stores the *pre-mutation* world state (CBOR via
// SceneSerializer). Two usage patterns:
//
//   Structural ops (create/delete/reparent/add component, menu clicks):
//       undo.snapshotNow();   // capture + push, then mutate
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
class EditorUndo
{
public:
	void setWorld(HorizonWorld* world) { m_world = world; }

	void capturePre();      // serialize the current world into the scratch slot
	void stashPre();        // scratch → pending (edit session started)
	void commitPending();   // pending → undo stack (edit session finished)
	void snapshotNow();     // capture + push in one step

	bool undo();
	bool redo();
	bool canUndo() const { return !m_undoStack.empty(); }
	bool canRedo() const { return !m_redoStack.empty(); }

	// Monotonically increasing counter, bumped on every world mutation that
	// passes through undo (push/undo/redo). The editor compares it against the
	// value at the last save/load to know whether the scene is dirty.
	uint64_t revision() const { return m_revision; }

	void clearHistory();

private:
	using Snapshot = std::vector<uint8_t>;

	void restore(const Snapshot& snapshot);
	void pushUndo(Snapshot&& snapshot);

	static constexpr size_t kMaxEntries = 64;

	HorizonWorld* m_world = nullptr;
	Snapshot      m_scratch;
	Snapshot      m_pending;
	bool          m_hasPending = false;
	uint64_t      m_revision   = 0;
	std::vector<Snapshot> m_undoStack;
	std::vector<Snapshot> m_redoStack;
};
