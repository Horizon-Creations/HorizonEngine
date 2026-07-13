#include "CppClassEditorPanel.h"
#include "EditorApplication.h"                 // AppContext
#include "TextEditor.h"                        // ImGuiColorTextEdit (vendored, MIT)
#include <imgui_internal.h>                    // ImGuiContext::PlatformImeData (text-input activation)
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>

namespace fs = std::filesystem;

namespace
{
	std::string lowerExt(const std::string& e)
	{
		std::string s = e;
		for (auto& c : s) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
		return s;
	}
	bool isHeaderExt(const std::string& e)
	{
		std::string s = lowerExt(e);
		return s == ".h" || s == ".hpp" || s == ".hh" || s == ".hxx";
	}
	bool isSourceExt(const std::string& e)
	{
		std::string s = lowerExt(e);
		return s == ".cpp" || s == ".cc" || s == ".cxx" || s == ".c";
	}

	std::string readTextFile(const std::string& path)
	{
		std::ifstream in(path, std::ios::binary);
		return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	}
	bool writeTextFile(const std::string& path, const std::string& text)
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (!out.is_open()) return false;
		out << text;
		return out.good();
	}

	// The first existing sibling <dir>/<stem><ext> across a set of extensions,
	// or empty if none exists.
	std::string findSibling(const fs::path& dir, const std::string& stem,
	                        std::initializer_list<const char*> exts)
	{
		for (const char* e : exts)
		{
			fs::path p = dir / (stem + e);
			if (fs::exists(p)) return p.string();
		}
		return {};
	}

	// One editor buffer (header or source) plus its on-disk path and dirty baseline.
	struct FileBuf
	{
		std::string path;                // empty = this half of the pair doesn't exist
		TextEditor  editor;
		int         savedUndoIndex = 0;
		bool        loaded         = false;
	};

	struct State
	{
		FileBuf     header;
		FileBuf     source;
		int         active = 0;          // 0 = header, 1 = source
		bool        resolved = false;    // sibling paths worked out yet?
		std::string className;           // stem, shown in the header bar
	};
	std::map<std::string, State> g_states;

	void loadBuf(FileBuf& fb)
	{
		fb.editor.SetLanguageDefinition(TextEditor::LanguageDefinitionId::Cpp);
		fb.editor.SetPalette(TextEditor::PaletteId::Dark);
		fb.editor.SetTabSize(4);
		fb.editor.SetText(fb.path.empty() ? std::string{} : readTextFile(fb.path));
		fb.savedUndoIndex = fb.editor.GetUndoIndex();
		fb.loaded = true;
	}

	// Resolve the .h and .cpp of the class whose canonical file is `canonical`, and
	// load whichever exist. `canonical` is one of the two files (the grid item).
	State& stateFor(const std::string& canonical)
	{
		State& st = g_states[canonical];
		if (st.resolved) return st;

		fs::path p(canonical);
		const fs::path dir = p.parent_path();
		st.className = p.stem().string();

		if (isHeaderExt(p.extension().string()))
		{
			st.header.path = canonical;
			st.source.path = findSibling(dir, st.className, { ".cpp", ".cc", ".cxx", ".c" });
		}
		else // source (or anything else) is the canonical file
		{
			st.source.path = canonical;
			st.header.path = findSibling(dir, st.className, { ".h", ".hpp", ".hh", ".hxx" });
		}

		loadBuf(st.header);
		loadBuf(st.source);
		// Open on whichever half actually exists — prefer the header.
		st.active = st.header.path.empty() ? 1 : 0;
		st.resolved = true;
		return st;
	}

	bool bufDirty(const FileBuf& fb)
	{
		return fb.loaded && !fb.path.empty() && fb.editor.GetUndoIndex() != fb.savedUndoIndex;
	}

	bool saveBuf(FileBuf& fb)
	{
		if (fb.path.empty()) return true;                 // nothing to save
		if (!writeTextFile(fb.path, fb.editor.GetText())) return false;
		fb.savedUndoIndex = fb.editor.GetUndoIndex();
		return true;
	}
}

namespace CppClassEditorPanel
{
	bool isCppSourceAsset(const std::string& path)
	{
		const std::string ext = fs::path(path).extension().string();
		return isHeaderExt(ext) || isSourceExt(ext);
	}

	bool isDirty(const std::string& path)
	{
		auto it = g_states.find(path);
		if (it == g_states.end()) return false;
		return bufDirty(it->second.header) || bufDirty(it->second.source);
	}

	void forget(const std::string& path) { g_states.erase(path); }

	void render(AppContext& ctx, const std::string& path, const ImVec2& pos, const ImVec2& size)
	{
		State& st = stateFor(path);

		ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
		ImGui::SetNextWindowSize(size, ImGuiCond_Always);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,  ImVec2(0.0f, 0.0f));
		ImGui::Begin("##CppClassEditor", nullptr,
			ImGuiWindowFlags_NoTitleBar         | ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoMove             | ImGuiWindowFlags_NoCollapse |
			ImGuiWindowFlags_NoScrollbar        | ImGuiWindowFlags_NoScrollWithMouse |
			ImGuiWindowFlags_NoSavedSettings    | ImGuiWindowFlags_NoBringToFrontOnFocus |
			ImGuiWindowFlags_NoDocking);
		ImGui::PopStyleVar(2);

		ImGuiIO& io = ImGui::GetIO();
		const bool saveKey = (io.KeyCtrl || io.KeySuper) && ImGui::IsKeyPressed(ImGuiKey_S, false);
		bool doSave = false;

		// ── Header bar: class name + Header/Source toggle + dirty + Save ──────
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 5.0f));
		if (ctx.fontBody) ImGui::PushFont(ctx.fontBody);
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(st.className.empty() ? "class" : st.className.c_str());
		ImGui::SameLine();

		// Toggle: a half that doesn't exist on disk is shown disabled.
		auto tabButton = [&](const char* label, int which, bool exists)
		{
			const bool selected = (st.active == which);
			ImGui::BeginDisabled(!exists);
			ImGui::PushStyleColor(ImGuiCol_Button, selected
				? ImVec4(0.26f, 0.46f, 0.78f, 1.0f) : ImVec4(0.22f, 0.22f, 0.24f, 1.0f));
			if (ImGui::SmallButton(label)) st.active = which;
			ImGui::PopStyleColor();
			ImGui::EndDisabled();
		};
		tabButton(".h##cppHdr",  0, !st.header.path.empty());
		ImGui::SameLine(0, 4);
		tabButton(".cpp##cppSrc", 1, !st.source.path.empty());

		FileBuf& cur = (st.active == 1) ? st.source : st.header;
		const bool curExists = !cur.path.empty();

		if (bufDirty(cur))
		{
			ImGui::SameLine();
			ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.30f, 1.0f), "* unsaved");
		}
		const float saveW = 72.0f;
		ImGui::SameLine();
		ImGui::SetCursorPosX(ImGui::GetWindowWidth() - saveW - 8.0f);
		ImGui::BeginDisabled(!curExists);
		if (ImGui::Button("Save", ImVec2(saveW, 0.0f))) doSave = true;
		ImGui::EndDisabled();
		if (ctx.fontBody) ImGui::PopFont();
		ImGui::PopStyleVar();
		ImGui::Separator();

		if ((doSave || saveKey) && curExists) saveBuf(cur);

		// ── The active half fills the rest (monospace for alignment) ──────────
		if (!curExists)
		{
			ImGui::TextDisabled("This class has no %s file.", st.active == 1 ? ".cpp" : ".h");
		}
		else
		{
			if (ctx.codeFont) ImGui::PushFont(ctx.codeFont);
			cur.editor.Render("##cppcode",
				ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows), ImVec2(0, 0), false);
			if (ctx.codeFont) ImGui::PopFont();

			// Same ImGui 1.92 text-input activation workaround as ScriptEditorPanel:
			// a custom widget must drive PlatformImeData.WantTextInput itself or SDL
			// never starts delivering character events.
			if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
			{
				ImGuiContext& g = *ImGui::GetCurrentContext();
				g.PlatformImeData.WantTextInput = true;
				g.PlatformImeData.InputPos      = ImGui::GetWindowPos();
			}
		}

		ImGui::End();
	}
}
