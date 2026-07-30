#pragma once
#include <Types/UUID.h>
#include <map>
#include <string>

struct AppContext;

// ── Per-tab asset-panel state ────────────────────────────────────────────────
// Every asset editor keeps one State per open tab, keyed by the asset's absolute
// path (the same key EditorUI's tab bar uses): created lazily on the tab's first
// render, kept alive for the whole session — closing and reopening a tab must not
// lose the buffer / camera / graph undo history — and dropped by the panel's
// forget() when EditorUI closes a tab that has nothing unsaved.
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
public:
	// This tab's state, default-constructed on first use.
	T& operator[](const std::string& assetPath) { return m_states[assetPath]; }

	// This tab's state, or nullptr when the tab was never opened.
	T* find(const std::string& assetPath)
	{
		const auto it = m_states.find(assetPath);
		return it == m_states.end() ? nullptr : &it->second;
	}
	const T* find(const std::string& assetPath) const
	{
		const auto it = m_states.find(assetPath);
		return it == m_states.end() ? nullptr : &it->second;
	}

	// `T::dirty` for the panels whose State carries that flag. A tab that was
	// never opened has nothing unsaved.
	bool dirty(const std::string& assetPath) const
	{
		const T* st = find(assetPath);
		return st && st->dirty;
	}

	// Drop a closed tab's state (see the lifecycle note above).
	void forget(const std::string& assetPath) { m_states.erase(assetPath); }

private:
	std::map<std::string, T> m_states;
};

// Open the asset behind a tab: fills `nameOut` with the filename shown in the
// panel header and `relPathOut` with the content-root-relative path (the key the
// ContentManager addresses assets by — or the reserved "Engine/"-prefixed path
// for engine content; an asset under neither root keeps its absolute path), then
// loads it. Returns the asset UUID, null when there is no ContentManager yet.
HE::UUID openPanelAsset(AppContext& ctx, const std::string& absPath,
                        std::string& nameOut, std::string& relPathOut);
