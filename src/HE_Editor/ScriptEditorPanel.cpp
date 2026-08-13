#include "ScriptEditorPanel.h"
#include "EditorToolbar.h"   // shared toolbar strip
#include "EditorApplication.h"                 // AppContext
#include "EditorAssetTypeCache.h"              // shared, invalidatable path → AssetType sniff
#include "EditorPanelState.h"                  // shared per-tab state map
#include "TextEditor.h"                        // ImGuiColorTextEdit (vendored, MIT)
#include <imgui_internal.h>                    // ImGuiContext::PlatformImeData (text-input activation)
#include <ContentManager/HAsset.h>
#include <Types/Enums.h>                       // HE::AssetType
#include <filesystem>
#include <cstdint>

namespace
{
	// Per-file editor state, kept alive for the session so switching tabs (or
	// closing + reopening a tab) never loses the buffer / cursor / undo history.
	struct State
	{
		TextEditor  editor;
		int         savedUndoIndex = 0;   // GetUndoIndex() at the last save → dirty test
		bool        loaded         = false;
		bool        python         = false;
		std::string name;                 // filename, shown in the header
	};
	AssetPanelState<State> s_states;

	bool isDirtyState(const State& st) { return st.editor.GetUndoIndex() != st.savedUndoIndex; }

	void loadFromDisk(State& st, const std::string& path)
	{
		std::string src;
		bool python = false;
		HAsset::Reader r;
		if (r.open(path))
		{
			if (const auto* c = r.findChunk(HAsset::CHUNK_SRC); c && !c->data.empty())
				src.assign(reinterpret_cast<const char*>(c->data.data()), c->data.size());
			// CHUNK_SLNG: 1 byte, 0 = Lua, 1 = Python (absent → Lua).
			if (const auto* c = r.findChunk(HAsset::CHUNK_SLNG); c && !c->data.empty())
				python = (c->data[0] == 1);
		}
		st.python = python;
		st.editor.SetLanguageDefinition(python ? TextEditor::LanguageDefinitionId::Python
		                                       : TextEditor::LanguageDefinitionId::Lua);
		st.editor.SetPalette(TextEditor::PaletteId::Dark);
		st.editor.SetTabSize(4);
		st.editor.SetText(src);
		st.savedUndoIndex = st.editor.GetUndoIndex();
		st.name   = std::filesystem::path(path).filename().string();
		st.loaded = true;
	}

	State& stateFor(const std::string& path)
	{
		State& st = s_states[path];
		if (!st.loaded) loadFromDisk(st, path);
		return st;
	}

	// Rewrite the .hasset preserving every chunk (META keeps the UUID, SLNG the
	// language) and replacing only CHUNK_SRC with the edited text. Save to disk only:
	// a running session keeps its already-parsed copy until the project reloads —
	// ContentManager::loadAsset() is a no-op for an already-loaded path, and a
	// re-parse-and-swap reload isn't exposed safely yet, so live-reload is a follow-up.
	bool saveToDisk(State& st, const std::string& path)
	{
		HAsset::Reader r;
		if (!r.open(path)) return false;
		const uint16_t type = r.assetType();
		HAsset::Writer w;
		for (const auto& c : r.chunks())
			if (c.id != HAsset::CHUNK_SRC)
				w.addChunk(c.id, c.data.data(), c.data.size());
		const std::string text = st.editor.GetText();
		w.addChunk(HAsset::CHUNK_SRC, text.data(), text.size());
		if (!w.write(path, type)) return false;
		st.savedUndoIndex = st.editor.GetUndoIndex();
		return true;
	}
}

namespace ScriptEditorPanel
{
	bool isDirty(const std::string& path)
	{
		const State* st = s_states.find(path);
		return st && st->loaded && isDirtyState(*st);
	}

	void appendDirtyPaths(std::vector<std::string>& out)
	{
		s_states.appendPathsIf([](const State& st) { return st.loaded && isDirtyState(st); }, out);
	}

	bool save(const std::string& path)
	{
		State* st = s_states.find(path);
		// A tab this panel never opened has nothing to write — the caller asks
		// every panel about every path, so "not mine" must read as success.
		if (!st || !st->loaded || !isDirtyState(*st)) return true;
		return saveToDisk(*st, path);
	}

	bool isScriptAsset(const std::string& path)
	{
		return EditorAssetTypeCache::is(path, HE::AssetType::Script);
	}

	void forget(const std::string& path) { s_states.forget(path); }

	void render(AppContext& ctx, const std::string& path, const ImVec2& pos, const ImVec2& size)
	{
		State& st = stateFor(path);
		const bool dirty = isDirtyState(st);

		ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
		ImGui::SetNextWindowSize(size, ImGuiCond_Always);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,  ImVec2(0.0f, 0.0f));
		ImGui::Begin("##ScriptEditor", nullptr,
			ImGuiWindowFlags_NoTitleBar         | ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoMove             | ImGuiWindowFlags_NoCollapse |
			ImGuiWindowFlags_NoScrollbar        | ImGuiWindowFlags_NoScrollWithMouse |
			ImGuiWindowFlags_NoSavedSettings    | ImGuiWindowFlags_NoBringToFrontOnFocus |
			ImGuiWindowFlags_NoDocking);
		ImGui::PopStyleVar(2);

		// Ctrl+S (or Cmd+S on macOS) saves.
		ImGuiIO& io = ImGui::GetIO();
		const bool saveKey = (io.KeyCtrl || io.KeySuper) && ImGui::IsKeyPressed(ImGuiKey_S, false);
		bool doSave = false;

		// ── Toolbar ───────────────────────────────────────────────────────────
		// The language badge was a SmallButton tinted to look like a label, which
		// invites a click that does nothing. It is a readout now, and says so.
		{
			namespace T = EditorToolbar;
			if (ctx.fontBody) ImGui::PushFont(ctx.fontBody);
			T::Bar bar;
			T::assetHeader(bar, st.name.empty() ? "script" : st.name.c_str(),
			               T::iconCode, dirty);
			bar.group();
			bar.readout(nullptr, st.python ? "Python" : "Lua", T::kFgDim);
			bar.endGroup();
			if (T::saveButton(bar, true)) doSave = true;
			if (ctx.fontBody) ImGui::PopFont();
		}

		if (doSave || saveKey) saveToDisk(st, path);

		// ── Code editor fills the remaining area (monospace font for alignment) ──
		// This window gets no text-wrap scope, and that is a decision rather than an
		// oversight: the two things it draws are the toolbar strip, whose cells are
		// measured from the width of their own labels — wrap them and the strip
		// reflows around a measurement taken before the wrap — and the code editor,
		// which paints every glyph into the draw list at a position it computed from
		// a line and a column. A wrap position would be ignored by the second and
		// would corrupt the first. Source lines are the editor's own business; it
		// scrolls them, and the column arithmetic that puts the caret where the user
		// clicked assumes one screen row per source line.
		if (ctx.codeFont) ImGui::PushFont(ctx.codeFont);
		// aSize = (0,0) fills the remaining content region below the header.
		st.editor.Render("##code",
			ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows), ImVec2(0, 0), false);
		if (ctx.codeFont) ImGui::PopFont();

		// ImGuiColorTextEdit signals typing via io.WantTextInput, but ImGui 1.92 drives
		// platform text-input activation (SDL_StartTextInput on macOS) from
		// g.PlatformImeData.WantTextInput instead — which a custom widget never sets. So
		// character events never reach io.InputQueueCharacters and typing does nothing
		// (arrows/backspace still work — those are key events). Drive it directly while
		// the editor is focused so SDL starts delivering SDL_EVENT_TEXT_INPUT.
		if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
		{
			ImGuiContext& g = *ImGui::GetCurrentContext();
			g.PlatformImeData.WantTextInput = true;
			g.PlatformImeData.InputPos      = ImGui::GetWindowPos();
		}

		ImGui::End();
	}

	bool reloadFromDisk(const std::string& assetPath)
	{
		// A collaboration peer's change just landed in the file. Dropping `loaded`
		// makes the next frame re-read it while the rest of the State survives.
		// Dirty is cleared deliberately: while a peer holds the asset's lock this
		// panel is read-only anyway, so anything "unsaved" here is stale.
		auto* st = s_states.find(assetPath);
		if (!st) return false;
		st->loaded = false;
		// dirty derives from undo indices; the reload resets both.
		return true;
	}

}

