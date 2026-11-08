#pragma once
#include "AssetIdentity.h"
#include <Types/UUID.h>
#include <map>
#include <string>
#include <vector>

struct AppContext;

// ── Per-tab asset-panel state ────────────────────────────────────────────────
// Every asset editor keeps one State per open tab: created lazily on the tab's
// first render, kept alive for the whole session — closing and reopening a tab
// must not lose the buffer / camera / graph undo history — and dropped by the
// panel's forget() when EditorUI closes a tab that has nothing unsaved.
//
// The state belongs to the ASSET, not to the string the tab happens to hold. It
// is filed under AssetIdentity::keyFor(path): the UUID in the file's header, so
//   * every spelling of the path finds it (a collaboration peer's edit arrives as
//     a content-relative key and is turned back into a path with resolveSavePath,
//     which is not the string the Content Browser opened the tab with), and
//   * a rename or move keeps it — the edits, the undo history and the dirty flag
//     stay with the asset instead of being stranded under the old path while a
//     fresh empty state grows at the new one.
// The entry remembers the path it was last asked under; the first time that is a
// different path (not a different spelling) the State is told, see onMoved below.
//
// Every panel spelled the same map + find + erase out for itself, and the copies
// were where the lifecycle bugs lived. Keeping it in one place also keeps the
// contract visible: find() must NOT create an entry (EditorUI asks isDirty() for
// tabs it is about to close), and forget() must only ever run for a tab whose
// isDirty() said false — dropping a dirty state is exactly how the Particle and
// Animator-State-Machine graphs used to lose unsaved edits.
//
// UI-thread only (all callers run inside the ImGui frame), like the rest of the
// editor's containers.
template <class T>
class AssetPanelState
{
	struct Entry
	{
		std::string path;   // the path this state was last asked under (for the quit prompt, save)
		T           state;
	};
	using Map = std::map<std::string, Entry>;

public:
	// This tab's state, default-constructed on first use.
	T& operator[](const std::string& assetPath) { return locate(assetPath, true)->state; }

	// This tab's state, or nullptr when the tab was never opened.
	T* find(const std::string& assetPath)
	{
		Entry* e = locate(assetPath, false);
		return e ? &e->state : nullptr;
	}
	const T* find(const std::string& assetPath) const
	{
		const auto it = peek(assetPath);
		return it == m_states.end() ? nullptr : &it->second.state;
	}

	// `T::dirty` for the panels whose State carries that flag. A tab that was
	// never opened has nothing unsaved.
	bool dirty(const std::string& assetPath) const
	{
		const T* st = find(assetPath);
		return st && st->dirty;
	}

	// Every unsaved tab this panel is holding, whether or not it is still OPEN.
	// Closing a dirty tab deliberately keeps its state (above) while removing it
	// from EditorUI's tab vector — so anything that answers "is there something to
	// lose?" by walking the tab vector misses exactly those. The quit guard asks
	// here instead. The path is the one the asset has NOW (a move updates it).
	void appendDirtyPaths(std::vector<std::string>& out) const
	{
		appendPathsIf([](const T& st) { return st.dirty; }, out);
	}

	// Same, for the panels whose "unsaved" is not a bool on the State — the script
	// editor compares undo indices, the C++ class editor tests two text buffers.
	// They pass their own isDirty predicate so there is still exactly one place
	// that knows how to walk the map.
	template <class Pred>
	void appendPathsIf(Pred pred, std::vector<std::string>& out) const
	{
		for (const auto& [key, e] : m_states)
			if (pred(e.state)) out.push_back(e.path);
	}

	// Walk every open tab's state — for the questions one tab has to ask about all
	// the others ("is any other tab still using this asset?"), and for the answers
	// that change them ("then YOU own it now").
	template <class Fn>
	void forEach(Fn fn) const
	{
		for (const auto& [key, e] : m_states) fn(e.path, e.state);
	}
	template <class Fn>
	void forEach(Fn fn)
	{
		for (auto& [key, e] : m_states) fn(e.path, e.state);
	}

	// Drop a closed tab's state (see the lifecycle note above).
	void forget(const std::string& assetPath)
	{
		const auto it = peek(assetPath);
		if (it != m_states.end()) m_states.erase(it);
	}

private:
	// The entry for this path: under its identity, or — for a state filed before
	// the file had a UUID — under the path key.
	typename Map::const_iterator peek(const std::string& assetPath) const
	{
		auto it = m_states.find(HE::Ed::AssetIdentity::keyFor(assetPath));
		if (it == m_states.end())
			it = m_states.find(HE::Ed::AssetIdentity::pathKeyFor(assetPath));
		return it;
	}

	Entry* locate(const std::string& assetPath, bool create)
	{
		const std::string key = HE::Ed::AssetIdentity::keyFor(assetPath);
		auto it = m_states.find(key);
		if (it == m_states.end())
		{
			// The file got its UUID after the state was created (a pre-v2 asset's
			// first save): move the state to the identity, keeping the object — the
			// panels hold references into it.
			const std::string legacy = HE::Ed::AssetIdentity::pathKeyFor(assetPath);
			if (legacy != key)
			{
				const auto lit = m_states.find(legacy);
				if (lit != m_states.end())
				{
					auto node = m_states.extract(lit);
					node.key() = key;
					it = m_states.insert(std::move(node)).position;
				}
			}
		}
		if (it == m_states.end())
		{
			if (!create) return nullptr;
			it = m_states.try_emplace(key).first;
			it->second.path = assetPath;
			return &it->second;
		}
		Entry& e = it->second;
		if (!HE::Ed::AssetIdentity::samePathSpelling(e.path, assetPath)) onMoved(e, assetPath);
		return &e;
	}

	// The asset is now asked for under another path: it was renamed or moved. The
	// State caches where it lives (`relPath`, content-relative) for saving and for
	// the crash-recovery copy; that cache is refreshed, nothing else is touched —
	// the edits in it are the point. A State with more to fix up declares
	// `onAssetMoved(newContentRelativePath)` and takes over.
	static void onMoved(Entry& e, const std::string& newPath)
	{
		e.path = newPath;
		const std::string rel = HE::Ed::AssetIdentity::contentRelative(newPath);
		if (rel.empty()) return;
		if constexpr (requires(T& t, const std::string& r) { t.onAssetMoved(r); })
			e.state.onAssetMoved(rel);
		else if constexpr (requires(T& t) { t.relPath = std::string(); })
			if (!e.state.relPath.empty()) e.state.relPath = rel;
	}

	Map m_states;
};

// Open the asset behind a tab: fills `nameOut` with the filename shown in the
// panel header and `relPathOut` with the content-root-relative path (the key the
// ContentManager addresses assets by — or the reserved "Engine/"-prefixed path
// for engine content; an asset under neither root keeps its absolute path), then
// loads it. Returns the asset UUID, null when there is no ContentManager yet.
HE::UUID openPanelAsset(AppContext& ctx, const std::string& absPath,
                        std::string& nameOut, std::string& relPathOut);
