#include "CppClassEditorPanel.h"
#include "EditorToolbar.h"   // shared toolbar strip
#include "EditorApplication.h"                 // AppContext
#include "EditorPanelState.h"                  // shared per-tab state map
#include "EditorWidgets.h"                     // WrapText
#include "TextEditor.h"                        // ImGuiColorTextEdit (vendored, MIT)
#include <imgui_internal.h>                    // ImGuiContext::PlatformImeData (text-input activation)
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

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
	AssetPanelState<State> s_states;

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
		State& st = s_states[canonical];
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
		const State* st = s_states.find(path);
		return st && (bufDirty(st->header) || bufDirty(st->source));
	}

	void appendDirtyPaths(std::vector<std::string>& out)
	{
		s_states.appendPathsIf(
			[](const State& st) { return bufDirty(st.header) || bufDirty(st.source); }, out);
	}

	bool save(const std::string& path)
	{
		State* st = s_states.find(path);
		// A class this panel never opened has nothing to write — the caller asks
		// every panel about every path, so "not mine" must read as success.
		if (!st) return true;
		bool ok = true;
		if (bufDirty(st->header)) ok = saveBuf(st->header) && ok;
		if (bufDirty(st->source)) ok = saveBuf(st->source) && ok;
		return ok;
	}

	void forget(const std::string& path) { s_states.forget(path); }

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

		FileBuf& cur = (st.active == 1) ? st.source : st.header;
		const bool curExists = !cur.path.empty();

		// ── Toolbar ───────────────────────────────────────────────────────────
		// The .h/.cpp pair is one choice, so it is one well of two cells rather
		// than two buttons tinted by hand. A half that is not on disk is disabled,
		// which the shared cell already draws as such.
		{
			namespace T = EditorToolbar;
			if (ctx.fontBody) ImGui::PushFont(ctx.fontBody);
			T::Bar bar;
			T::assetHeader(bar, st.className.empty() ? "class" : st.className.c_str(),
			               T::iconCode, bufDirty(cur));

			bar.group();
			if (bar.item("##cppHdr", nullptr, ".h", st.active == 0,
			             !st.header.path.empty(), "The class declaration"))
			{
				st.active = 0;
			}
			if (bar.item("##cppSrc", nullptr, ".cpp", st.active == 1,
			             !st.source.path.empty(), "The class implementation"))
			{
				st.active = 1;
			}
			bar.endGroup();

			if (T::saveButton(bar, curExists)) doSave = true;
			if (ctx.fontBody) ImGui::PopFont();
		}

		if ((doSave || saveKey) && curExists) saveBuf(cur);

		// ── The active half fills the rest (monospace for alignment) ──────────
		if (!curExists)
		{
			// The one sentence this panel ever prints, in a window that is only as
			// wide as the tab area happens to be — docked next to a Details panel
			// that is a few hundred pixels. Without a wrap position ImGui draws the
			// line straight past the right edge and clips it, so the reader is told
			// "This class has no" and has to guess which half is missing; that is
			// the same failure as a sideways scrollbar, minus even the hint that
			// something was cut off. The scope is deliberately this branch and not
			// the whole window: the toolbar sizes its cells from the width of their
			// own labels, and the code editor below paints into the draw list at
			// positions it computed itself, so neither wants a wrap column.
			EditorWidgets::WrapText wrap;
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

	bool reloadFromDisk(const std::string& assetPath)
	{
		// A collaboration peer's change just landed in the file. Dropping `loaded`
		// makes the next frame re-read it while the rest of the State survives.
		// Dirty is cleared deliberately: while a peer holds the asset's lock this
		// panel is read-only anyway, so anything "unsaved" here is stale.
		auto* st = s_states.find(assetPath);
		if (!st) return false;
		// The pair of buffers each carry their own loaded flag.
		st->header.loaded = false;
		st->source.loaded = false;
		// dirty derives from undo indices; the reload resets them with the buffers.
		return true;
	}

}

