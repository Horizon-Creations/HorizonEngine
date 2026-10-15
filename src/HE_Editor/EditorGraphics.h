#pragma once

#include "EditorConfig.h"
#include <Project/ProjectSettings.h>

// ── The graphics half of the editor's config, and the project's copy of it ───
// EditorConfig carries bloom, SSAO, anti-aliasing, GI and the rest as plain
// members (the renderer reads them each frame, config.json stores them). A
// project can now own the same values (HE::ProjectGraphics, Config/
// ProjectSettings.json). The two lists are one list — HE_PROJECT_GRAPHICS_FIELDS
// names each field's EditorConfig twin — so nothing here is written out twice and
// a field added there is carried across without being touched here.
//
// ImGui-free and AppContext-free, so tests/test_editor_graphics.cpp can ask
// every question without an editor.
namespace EditorGraphics
{
	// The project's values into the config's graphics fields. Touches nothing
	// else in the config (not the window, not the pins, not the mode).
	void apply(const HE::ProjectGraphics& from, EditorConfig& to);

	// The config's graphics fields into the project's. `enabled` is left alone:
	// it is the project's decision, not a value.
	void capture(const EditorConfig& from, HE::ProjectGraphics& to);

	// Only the graphics fields, config to config: what the editor keeps of the
	// user's own values while the project's are on screen, and puts back after.
	void copyFields(const EditorConfig& from, EditorConfig& to);

	// Whether two configs agree on every graphics field.
	bool same(const EditorConfig& a, const EditorConfig& b);

	// The row keys of the settings catalogue (the `row(key, …)` names in
	// EditorSettingsPanel) that edit these fields. A row of these is locked while
	// the project's values are on screen; the pages of the project's graphics
	// settings draw exactly these.
	struct RowSet { const char* const* keys; int count; };
	RowSet qualityRows();           // render path, occlusion culling, anti-aliasing, GPU particles
	RowSet postProcessingRows();    // bloom, SSAO, depth of field, motion blur
	RowSet reflectionsAndGiRows();  // SSR, GI, GI reflections
	bool   isGraphicsRow(const char* key);
}
