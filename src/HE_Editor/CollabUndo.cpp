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

void CollabUndo::recordCreate(std::uint64_t subject, std::vector<std::uint8_t> subtree,
                              std::uint64_t parentSubject)
{
	Entry e;
	e.kind        = Kind::Create;
	e.subject     = subject;
	e.subtree     = std::move(subtree);
	e.parentAfter = parentSubject;
	push(std::move(e));
}

void CollabUndo::recordDestroy(std::uint64_t subject, std::vector<std::uint8_t> subtree,
                               std::uint64_t parentSubject)
{
	Entry e;
	e.kind         = Kind::Destroy;
	e.subject      = subject;
	e.subtree      = std::move(subtree);
	e.parentBefore = parentSubject;
	push(std::move(e));
}

void CollabUndo::recordReparent(std::uint64_t subject, std::uint64_t parentBefore,
                                std::uint64_t parentAfter)
{
	Entry e;
	e.kind         = Kind::Reparent;
	e.subject      = subject;
	e.parentBefore = parentBefore;
	e.parentAfter  = parentAfter;
	push(std::move(e));
}

void CollabUndo::recordComponents(std::uint64_t subject, std::vector<std::uint8_t> before,
                                  std::vector<std::uint8_t> after)
{
	Entry e;
	e.kind        = Kind::Components;
	e.subject     = subject;
	e.beforeBytes = std::move(before);
	e.afterBytes  = std::move(after);
	push(std::move(e));
}

void CollabUndo::push(Entry&& e)
{
	m_undo.push_back(std::move(e));
	m_redo.clear();
	if (m_undo.size() > kMaxEntries)
		m_undo.erase(m_undo.begin());
}

std::string CollabUndo::labelFor(const Entry& e, bool isUndo)
{
	const char* verb = isUndo ? "Undo " : "Redo ";
	switch (e.kind)
	{
	case Kind::Asset:      return verb + std::string("change to ") + shortName(e.path);
	case Kind::Create:     return verb + std::string("create");
	case Kind::Destroy:    return verb + std::string("delete");
	case Kind::Reparent:   return verb + std::string("re-parent");
	case Kind::Components: return verb + std::string("change");
	case Kind::Transform:  break;
	}
	return verb + std::string("move");
}

std::string CollabUndo::undoLabel() const
{
	if (m_undo.empty()) return {};
	return labelFor(m_undo.back(), /*isUndo=*/true);
}

std::string CollabUndo::redoLabel() const
{
	if (m_redo.empty()) return {};
	return labelFor(m_redo.back(), /*isUndo=*/false);
}

bool CollabUndo::applyEntry(const Entry& e, bool useBefore)
{
	if (e.kind == Kind::Transform)
	{
		if (!m_applyTransform) return false;
		m_applyTransform(e.subject, useBefore ? e.beforeTransform : e.afterTransform);
		return true;
	}

	// Create and Destroy are each other's inverse, which is why one pair of
	// handlers serves both: undoing a create is a destroy, and redoing a destroy
	// is the same destroy again.
	if (e.kind == Kind::Create || e.kind == Kind::Destroy)
	{
		const bool wantExists = (e.kind == Kind::Create) != useBefore;
		if (wantExists)
		{
			if (!m_applyCreate || e.subtree.empty()) return false;
			m_applyCreate(e.subject, e.subtree,
			              e.kind == Kind::Create ? e.parentAfter : e.parentBefore);
		}
		else
		{
			if (!m_applyDestroy) return false;
			m_applyDestroy(e.subject);
		}
		return true;
	}

	if (e.kind == Kind::Reparent)
	{
		if (!m_applyReparent) return false;
		m_applyReparent(e.subject, useBefore ? e.parentBefore : e.parentAfter);
		return true;
	}

	if (e.kind == Kind::Components)
	{
		if (!m_applyComponents) return false;
		const std::vector<std::uint8_t>& bytes =
			useBefore ? e.beforeBytes : e.afterBytes;
		if (bytes.empty()) return false;
		m_applyComponents(e.subject, bytes);
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
	const auto stale = [this](const Entry& e) {
		// See the header: on one side of a create/destroy entry the subject does
		// not exist, so "we do not hold it" is not evidence that anyone else
		// changed it — it is evidence there is nothing to change.
		if (e.kind == Kind::Create || e.kind == Kind::Destroy) return false;
		return !m_owns(e.subject);
	};
	m_undo.erase(std::remove_if(m_undo.begin(), m_undo.end(), stale), m_undo.end());
	m_redo.erase(std::remove_if(m_redo.begin(), m_redo.end(), stale), m_redo.end());
}
