#pragma once

// ─── Which MCP client holds which session lock ───────────────────────────────
// The bookkeeping behind EditorApplication::updateMcpLocks, kept apart from it so
// the one question that went wrong can be answered in a test without a window:
// WHOSE is an external lock?
//
// The session only knows participants. To the host, every lock this editor
// takes is "this editor's", whichever client asked for it. So the answer has to
// live here, on this side of the wire: a subject is held by the set of MCP
// clients that took it, and it goes back to the session when the LAST of them is
// gone — not when the editor has no clients at all. The first version did the
// latter (docs/mcp-editor-integration-plan.md §20.3): a client that hung up
// while a second one stayed connected left its locks behind, the second client
// inherited them without ever having asked, and every peer kept getting
// `locked_by_other` for an entity nobody was editing.
//
// A set of holders rather than one, because two clients of the same editor can
// both edit the same entity. The session holds the lock once for both; handing
// it back when the first leaves would pull it from under the second.
//
// Client 0 is the anonymous caller — a tool invoked without a bridge behind it
// (a test, a call outside any `tools/call`). It never disconnects, so its locks
// only go when the editor has no clients left (updateMcpLocks keeps that sweep
// for exactly this) or the session ends.
//
// Header-only and free of the session, the registry and the Net headers: the
// caller decides what "release" means and does it with what this hands back.

#include "McpClientCameras.h"   // McpClientId

#include <algorithm>
#include <cstdint>
#include <vector>

namespace HE::Ed
{

class McpLockBook
{
public:
	struct Entry
	{
		std::uint64_t            subject     = 0;
		// When the session was last asked for this subject. The caller re-asks
		// at most every so often (see updateMcpLocks); stamped on take so the
		// first re-ask does not land in the same frame as the ask itself.
		std::uint64_t            lastAskedMs = 0;
		std::vector<McpClientId> holders;
	};

	// `client` has taken `subject`. Idempotent per (subject, client); a second
	// client on the same subject joins the holders. Subject 0 is "no entity"
	// and is ignored.
	void take(std::uint64_t subject, McpClientId client, std::uint64_t nowMs)
	{
		if (subject == 0) return;
		for (Entry& e : m_entries)
		{
			if (e.subject != subject) continue;
			if (std::find(e.holders.begin(), e.holders.end(), client) == e.holders.end())
				e.holders.push_back(client);
			return;
		}
		m_entries.push_back(Entry{ subject, nowMs, { client } });
	}

	// `client` is gone. Drops it from every holder list and returns the subjects
	// nobody holds any more — those, and only those, go back to the session.
	// Subjects another client still holds stay, unchanged.
	std::vector<std::uint64_t> clientGone(McpClientId client)
	{
		std::vector<std::uint64_t> released;
		for (auto it = m_entries.begin(); it != m_entries.end();)
		{
			auto& h = it->holders;
			const auto before = h.size();
			h.erase(std::remove(h.begin(), h.end(), client), h.end());
			if (h.empty() && before != 0)
			{
				released.push_back(it->subject);
				it = m_entries.erase(it);
			}
			else
				++it;
		}
		return released;
	}

	// Every subject, and the book emptied: the editor has no client left, or
	// the session ended.
	std::vector<std::uint64_t> takeAll()
	{
		std::vector<std::uint64_t> all;
		all.reserve(m_entries.size());
		for (const Entry& e : m_entries) all.push_back(e.subject);
		m_entries.clear();
		return all;
	}

	void clear() { m_entries.clear(); }
	bool empty() const { return m_entries.empty(); }
	std::size_t size() const { return m_entries.size(); }

	bool holds(std::uint64_t subject, McpClientId client) const
	{
		for (const Entry& e : m_entries)
			if (e.subject == subject)
				return std::find(e.holders.begin(), e.holders.end(), client) != e.holders.end();
		return false;
	}

	// For the re-ask loop, which also forgets a subject a peer has taken.
	std::vector<Entry>&       entries()       { return m_entries; }
	const std::vector<Entry>& entries() const { return m_entries; }

private:
	// Insertion order; at most as long as the number of entities the clients of
	// one editor touched in a session, so a linear search is the right shape.
	std::vector<Entry> m_entries;
};

} // namespace HE::Ed
