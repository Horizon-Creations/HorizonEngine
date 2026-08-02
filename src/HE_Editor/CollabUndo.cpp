#include "CollabUndo.h"

#include <algorithm>
#include <cstring>

namespace
{
	std::string shortName(const std::string& path)
	{
		const std::size_t slash = path.find_last_of("/\\");
		return slash == std::string::npos ? path : path.substr(slash + 1);
	}
} // namespace

void CollabUndo::recordTransform(std::uint64_t subject,
                                 const float before[9], const float after[9])
{
	Entry e;
	e.kind    = Kind::Transform;
	e.subject = subject;
	std::memcpy(e.beforeTransform, before, sizeof(e.beforeTransform));
	std::memcpy(e.afterTransform,  after,  sizeof(e.afterTransform));

	m_undo.push_back(std::move(e));
	// A new change invalidates the redo branch, exactly as in a single-user
	// stack: those entries describe a future that no longer exists.
	m_redo.clear();

	if (m_undo.size() > kMaxEntries)
		m_undo.erase(m_undo.begin());
}

void CollabUndo::recordAsset(std::uint64_t subject, const std::string& path,
                             std::vector<std::uint8_t> before,
                             std::vector<std::uint8_t> after)
{
	Entry e;
	e.kind        = Kind::Asset;
	e.subject     = subject;
	e.path        = path;
	e.beforeBytes = std::move(before);
	e.afterBytes  = std::move(after);

	m_undo.push_back(std::move(e));
	m_redo.clear();

	if (m_undo.size() > kMaxEntries)
		m_undo.erase(m_undo.begin());
}

std::string CollabUndo::undoLabel() const
{
	if (m_undo.empty()) return {};
	const Entry& e = m_undo.back();
	return e.kind == Kind::Asset ? ("Undo change to " + shortName(e.path))
	                             : std::string("Undo move");
}

std::string CollabUndo::redoLabel() const
{
	if (m_redo.empty()) return {};
	const Entry& e = m_redo.back();
	return e.kind == Kind::Asset ? ("Redo change to " + shortName(e.path))
	                             : std::string("Redo move");
}

bool CollabUndo::applyEntry(const Entry& e, bool useBefore)
{
	if (e.kind == Kind::Transform)
	{
		if (!m_applyTransform) return false;
		m_applyTransform(e.subject, useBefore ? e.beforeTransform : e.afterTransform);
		return true;
	}

	if (!m_applyAsset) return false;
	const std::vector<std::uint8_t>& bytes = useBefore ? e.beforeBytes : e.afterBytes;
	// A newly created asset has no "before" state; undoing it would mean
	// deleting the file, which is a different operation than this stack models.
	if (bytes.empty()) return false;
	m_applyAsset(e.path, bytes);
	return true;
}

bool CollabUndo::undo()
{
	dropUnowned();
	if (m_undo.empty()) return false;

	Entry e = m_undo.back();
	m_undo.pop_back();

	if (!applyEntry(e, /*useBefore=*/true)) return false;

	m_redo.push_back(std::move(e));
	return true;
}

bool CollabUndo::redo()
{
	dropUnowned();
	if (m_redo.empty()) return false;

	Entry e = m_redo.back();
	m_redo.pop_back();

	if (!applyEntry(e, /*useBefore=*/false)) return false;

	m_undo.push_back(std::move(e));
	return true;
}

void CollabUndo::clear()
{
	m_undo.clear();
	m_redo.clear();
}

void CollabUndo::dropUnowned()
{
	if (!m_owns) return;

	// Once the lock is gone someone else may have changed the subject, so the
	// recorded "before" is no longer a truthful inverse — replaying it would
	// overwrite their work rather than undo ours.
	const auto stale = [this](const Entry& e) { return !m_owns(e.subject); };
	m_undo.erase(std::remove_if(m_undo.begin(), m_undo.end(), stale), m_undo.end());
	m_redo.erase(std::remove_if(m_redo.begin(), m_redo.end(), stale), m_redo.end());
}
