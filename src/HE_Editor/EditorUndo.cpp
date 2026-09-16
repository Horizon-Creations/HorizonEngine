#include "EditorUndo.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSerializer.h>
#include <algorithm>

void EditorUndo::capturePre()
{
	if (!m_world) return;
	SceneSerializer ser;
	ser.saveToMemory(*m_world, m_scratch);
}

std::string EditorUndo::resolveLabel(const char* label) const
{
	if (label && label[0]) return label;
	if (!m_context.empty()) return m_context;
	return "Edit";
}

void EditorUndo::stashPre(const char* label)
{
	if (m_scratch.empty()) return;
	m_pending      = m_scratch;
	m_pendingLabel = resolveLabel(label);
	m_hasPending   = true;
}

void EditorUndo::commitPending()
{
	if (!m_hasPending) return;
	pushUndo(std::move(m_pending), std::move(m_pendingLabel));
	m_pending.clear();
	m_pendingLabel.clear();
	m_hasPending = false;
}

void EditorUndo::snapshotNow(const char* label)
{
	if (!m_world) return;
	Snapshot snap;
	SceneSerializer ser;
	ser.saveToMemory(*m_world, snap);
	pushUndo(std::move(snap), resolveLabel(label));
}

void EditorUndo::pushUndo(Snapshot&& snapshot, std::string label)
{
	m_undoStack.push_back({ std::move(snapshot), std::move(label) });
	if (m_undoStack.size() > kMaxEntries)
		m_undoStack.erase(m_undoStack.begin());
	m_redoStack.clear();
	++m_revision;
}

void EditorUndo::restore(const Snapshot& snapshot)
{
	m_world->clear();
	SceneSerializer ser;
	ser.loadFromMemory(*m_world, snapshot);
	m_world->markHierarchyDirty();
}

bool EditorUndo::undo() { return undoSteps(1); }
bool EditorUndo::redo() { return redoSteps(1); }

// The two below are one shape mirrored. Taking back N entries means: the
// world as it is now becomes the redo entry for the newest undo entry (its
// label is the operation whose result the world currently shows), every
// intermediate snapshot moves across UNDER THE LABEL OF THE ENTRY ABOVE IT
// (a snapshot is the state BEFORE its own label's operation, which is the
// state AFTER the previous entry's), and only the oldest of the N is loaded.
//
// Labels therefore shift by one as they cross: the world's current state is
// "after L_top", the snapshot of entry i is "after L_{i-1}". Getting this
// wrong shows the wrong word on every redo row, which is the one thing the
// history window exists to get right.
bool EditorUndo::undoSteps(size_t n)
{
	if (!m_world || m_undoStack.empty() || n == 0) return false;
	n = std::min(n, m_undoStack.size());

	Snapshot current;
	SceneSerializer ser;
	ser.saveToMemory(*m_world, current);

	// Newest first: the current world under the top label, then each snapshot
	// under the label of the entry that followed it.
	std::string carried = m_undoStack.back().label;
	m_redoStack.push_back({ std::move(current), carried });
	for (size_t k = 1; k < n; ++k)
	{
		// The snapshot of the entry above is the state AFTER `moving`'s own
		// operation — so on the redo side it is what "redo moving.label"
		// brings back, and travels under that label.
		const Entry& moving = m_undoStack[m_undoStack.size() - k - 1];
		m_redoStack.push_back({ std::move(m_undoStack[m_undoStack.size() - k].data),
		                        moving.label });
	}
	// The oldest of the N is what the world becomes; its label goes nowhere
	// (it is the operation being taken back, and the row for redoing it was
	// pushed first, holding the current world).
	const Entry target = std::move(m_undoStack[m_undoStack.size() - n]);
	m_undoStack.resize(m_undoStack.size() - n);

	restore(target.data);
	++m_revision;
	++m_undoCount;
	return true;
}

bool EditorUndo::redoSteps(size_t n)
{
	if (!m_world || m_redoStack.empty() || n == 0) return false;
	n = std::min(n, m_redoStack.size());

	Snapshot current;
	SceneSerializer ser;
	ser.saveToMemory(*m_world, current);

	// Mirror image: the current world is the pre-state of the top redo entry.
	m_undoStack.push_back({ std::move(current), m_redoStack.back().label });
	for (size_t k = 1; k < n; ++k)
	{
		Entry& moving = m_redoStack[m_redoStack.size() - k - 1];
		m_undoStack.push_back({ std::move(m_redoStack[m_redoStack.size() - k].data),
		                        moving.label });
	}
	const Entry target = std::move(m_redoStack[m_redoStack.size() - n]);
	m_redoStack.resize(m_redoStack.size() - n);

	restore(target.data);
	++m_revision;
	return true;
}

void EditorUndo::clearHistory()
{
	m_undoStack.clear();
	m_redoStack.clear();
	m_pending.clear();
	m_pendingLabel.clear();
	m_hasPending = false;
}
