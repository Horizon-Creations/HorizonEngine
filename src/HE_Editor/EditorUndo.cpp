#include "EditorUndo.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSerializer.h>

void EditorUndo::capturePre()
{
	if (!m_world) return;
	SceneSerializer ser;
	ser.saveToMemory(*m_world, m_scratch);
}

void EditorUndo::stashPre()
{
	if (m_scratch.empty()) return;
	m_pending    = m_scratch;
	m_hasPending = true;
}

void EditorUndo::commitPending()
{
	if (!m_hasPending) return;
	pushUndo(std::move(m_pending));
	m_pending.clear();
	m_hasPending = false;
}

void EditorUndo::snapshotNow()
{
	if (!m_world) return;
	Snapshot snap;
	SceneSerializer ser;
	ser.saveToMemory(*m_world, snap);
	pushUndo(std::move(snap));
}

void EditorUndo::pushUndo(Snapshot&& snapshot)
{
	m_undoStack.push_back(std::move(snapshot));
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

bool EditorUndo::undo()
{
	if (!m_world || m_undoStack.empty()) return false;

	Snapshot current;
	SceneSerializer ser;
	ser.saveToMemory(*m_world, current);
	m_redoStack.push_back(std::move(current));

	restore(m_undoStack.back());
	m_undoStack.pop_back();
	++m_revision;
	return true;
}

bool EditorUndo::redo()
{
	if (!m_world || m_redoStack.empty()) return false;

	Snapshot current;
	SceneSerializer ser;
	ser.saveToMemory(*m_world, current);
	m_undoStack.push_back(std::move(current));

	restore(m_redoStack.back());
	m_redoStack.pop_back();
	++m_revision;
	return true;
}

void EditorUndo::clearHistory()
{
	m_undoStack.clear();
	m_redoStack.clear();
	m_pending.clear();
	m_hasPending = false;
}
