#include "ScriptEditorPanel.h"
#include "EditorApplication.h"                 // AppContext
#include "TextEditor.h"                        // ImGuiColorTextEdit (vendored, MIT)
#include <ContentManager/HAsset.h>
#include <Types/Enums.h>                       // HE::AssetType
#include <filesystem>
#include <map>
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
	std::map<std::string, State> g_states;

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
		State& st = g_states[path];
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
		auto it = g_states.find(path);
		return it != g_states.end() && it->second.loaded && isDirtyState(it->second);
	}

	bool isScriptAsset(const std::string& path)
	{
		HAsset::Reader r;
		if (!r.open(path)) return false;
		return r.assetType() == static_cast<uint16_t>(HE::AssetType::Script);
	}

	void forget(const std::string& path) { g_states.erase(path); }

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

		// ── Header: filename + language badge + dirty marker + Save button ────
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 5.0f));
		if (ctx.fontBody) ImGui::PushFont(ctx.fontBody);
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(st.name.empty() ? "script" : st.name.c_str());
		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Button,
			st.python ? ImVec4(0.24f, 0.42f, 0.72f, 1.0f) : ImVec4(0.26f, 0.52f, 0.34f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_Button));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImGui::GetStyleColorVec4(ImGuiCol_Button));
		ImGui::SmallButton(st.python ? "Python" : "Lua");
		ImGui::PopStyleColor(3);
		if (dirty)
		{
			ImGui::SameLine();
			ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.30f, 1.0f), "* unsaved");
		}
		const float saveW = 72.0f;
		ImGui::SameLine();
		ImGui::SetCursorPosX(ImGui::GetWindowWidth() - saveW - 8.0f);
		if (ImGui::Button("Save", ImVec2(saveW, 0.0f))) doSave = true;
		if (ctx.fontBody) ImGui::PopFont();
		ImGui::PopStyleVar();
		ImGui::Separator();

		if (doSave || saveKey) saveToDisk(st, path);

		// ── Code editor fills the remaining area (monospace font for alignment) ──
		if (ctx.codeFont) ImGui::PushFont(ctx.codeFont);
		// aSize = (0,0) fills the remaining content region below the header.
		st.editor.Render("##code",
			ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows), ImVec2(0, 0), false);
		if (ctx.codeFont) ImGui::PopFont();

		ImGui::End();
	}
}
