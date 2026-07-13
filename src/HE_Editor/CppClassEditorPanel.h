#pragma once
#include <string>
#include <imgui.h>

struct AppContext;

// A syntax-highlighting viewer/editor for a native C++ class, shown as a top-level
// editor tab (opened by double-clicking a class in the Content Browser's Source
// root). A "class" is the .h/.cpp pair sharing a stem; this panel loads BOTH and
// lets the user toggle between the header and the source in one tab. Unlike scripts
// these are RAW files on disk (not .hasset chunks), so load/save is plain text I/O.
// Per-file editor state is cached by the class's canonical path and survives tab
// switches / close+reopen for the session.
namespace CppClassEditorPanel
{
	// Render the viewer for the class whose canonical file is `assetPath` (the
	// header if the pair has one, otherwise the lone .cpp). Derives and loads the
	// sibling file lazily on first use.
	void render(AppContext& ctx, const std::string& assetPath,
	            const ImVec2& pos, const ImVec2& size);

	// True if either the header or the source buffer has unsaved edits.
	bool isDirty(const std::string& assetPath);

	// Whether `path` is a C++ source/header file by extension
	// (.h/.hpp/.hh/.hxx/.cpp/.cc/.cxx/.c). Raw-file check — no HAsset header.
	bool isCppSourceAsset(const std::string& path);

	// Drop cached editor state for `path` (state otherwise persists for the session
	// so unsaved edits survive a tab close+reopen).
	void forget(const std::string& assetPath);
}
