#pragma once

#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <string>
#include <vector>

// ── When the session's state is meant to be KEPT ───────────────────────────
// A live session changes documents continuously: a transform every frame of a
// drag, a delta per graph edit, the holder's whole-file autosave every second.
// None of that is a moment anybody would want written down as "the state".
// Two moments are:
//
//   • Saved — somebody in the session explicitly saved the document (Ctrl+S,
//     Save All, an MCP save). Collected for kSaveBatchMs so a Save All is one
//     batch, not twenty.
//   • Idle  — nobody has changed the document for kIdleMs: nobody is working on
//     it anymore.
//
// Only the HOST tracks this: its disk is the session's state (catch-up reads
// it, topic 199's design says the host syncs for all and clients pause). Each
// settled key is first handed to a persist step (the host writes what is not on
// its disk yet, i.e. the scene) and then reported to whoever listens — the
// attach point for the team server's upload (topic 199) and for anything else
// that wants "the session's state is now on disk".
//
// Locks deliberately do NOT count as "working": an asset lock is held for as
// long as its tab is open and an entity lock for as long as it is selected, so
// a lock can sit for hours with nobody touching anything. Quiet time is the
// signal. A change that arrives after a settle simply makes the key pending
// again.
//
// Pure and clock-driven (nowMs is passed in), so tests drive time instead of
// sleeping.
namespace HE::Ed
{
class CollabSettle
{
public:
	enum class Reason : std::uint8_t { Saved, Idle };

	struct Settled
	{
		std::string key;
		Reason      reason = Reason::Idle;
	};

	// The open scene. Its parts that travel under keys of their own (the Level
	// Script, LevelScriptPanel::kTabPath) settle as the scene: they are stored
	// in it.
	static constexpr const char* kSceneKey       = "::Scene::";
	static constexpr const char* kLevelScriptKey = "::LevelScript::";

	// The holder's whole-file autosave runs every second while a tab has unsaved
	// edits (EditorApplication kAutosaveIntervalMs), a drag streams a transform
	// per frame and a sculpt stroke a patch every 33 ms. Five seconds of nothing
	// from anybody is several autosave periods: no edit is in flight any more,
	// and the last whole file has reached the host.
	static constexpr std::uint64_t kIdleMs = 5000;
	// Save All writes its files one after another in a frame or two, and a
	// client's notices trail the files they follow by a round trip.
	static constexpr std::uint64_t kSaveBatchMs = 1000;

	static std::string normalize(const std::string& key)
	{
		return key == kLevelScriptKey ? std::string(kSceneKey) : key;
	}

	// The session changed `key` (applied on the host, ours or a peer's).
	void noteChanged(const std::string& key, std::uint64_t nowMs)
	{
		if (key.empty()) return;
		Entry& e = m_entries[normalize(key)];
		e.dirty        = true;
		e.lastChangeMs = nowMs;
	}

	// Somebody in the session saved `key`.
	void noteSaved(const std::string& key, std::uint64_t nowMs)
	{
		if (key.empty()) return;
		Entry& e = m_entries[normalize(key)];
		e.saved     = true;
		e.savedAtMs = nowMs;
		// Every save pushes the batch out, so a Save All lands as one.
		m_saveDueMs = nowMs + kSaveBatchMs;
	}

	// Persist step: asked once per key as it settles. False means "not now"
	// (Play, an unnamed scene, a disk conflict) and leaves the key pending; it
	// is asked again at the next update.
	using Persist = std::function<bool(const std::string& key, Reason)>;

	// What settled since the last call, in key order; saves before idles.
	std::vector<Settled> update(std::uint64_t nowMs, const Persist& persist)
	{
		std::vector<Settled> out;
		const bool saveDue = m_saveDueMs != 0 && nowMs >= m_saveDueMs;
		bool savesLeft = false;
		if (saveDue)
		{
			for (auto& [key, e] : m_entries)
			{
				if (!e.saved) continue;
				if (persist && !persist(key, Reason::Saved)) { savesLeft = true; continue; }
				e.saved = false;
				// A change after the save is not in what the save meant; it
				// settles on its own once it goes quiet.
				if (e.lastChangeMs <= e.savedAtMs) e.dirty = false;
				out.push_back({ key, Reason::Saved });
			}
			m_saveDueMs = savesLeft ? nowMs + kSaveBatchMs : 0;
		}
		for (auto& [key, e] : m_entries)
		{
			if (!e.dirty || e.saved) continue;
			if (nowMs < e.lastChangeMs + kIdleMs) continue;
			if (persist && !persist(key, Reason::Idle)) continue;
			e.dirty = false;
			out.push_back({ key, Reason::Idle });
		}
		// Settled entries carry nothing; dropping them keeps a long session's
		// map at the size of what is being worked on.
		for (auto it = m_entries.begin(); it != m_entries.end(); )
			it = (!it->second.dirty && !it->second.saved) ? m_entries.erase(it) : std::next(it);
		return out;
	}

	bool isPending(const std::string& key) const
	{
		auto it = m_entries.find(normalize(key));
		return it != m_entries.end() && (it->second.dirty || it->second.saved);
	}
	std::size_t pendingCount() const { return m_entries.size(); }

	void reset() { m_entries.clear(); m_saveDueMs = 0; }

private:
	struct Entry
	{
		bool          dirty        = false;
		bool          saved        = false;
		std::uint64_t lastChangeMs = 0;
		std::uint64_t savedAtMs    = 0;
	};
	std::map<std::string, Entry> m_entries;
	std::uint64_t                m_saveDueMs = 0;
};
} // namespace HE::Ed
