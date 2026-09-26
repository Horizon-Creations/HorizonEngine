#pragma once
#include <string>
#include <vector>

struct AppContext;

// ── "Is this texture colour or data?" ────────────────────────────────────────
// One dialog, one row per texture, one "sRGB (Color)" tick each, pre-set from
// the file name (Importer::suggestTextureSrgb). Two jobs share it:
//
//   · Import: the Content Browser's Import on image files and File ▸ Import
//     Asset route their textures through here instead of importing them
//     straight away. Every manual import used to come out linear, so an albedo
//     PNG was sampled as if its encoded bytes were linear light and rendered
//     washed out; the tick is where that is decided now.
//   · Retag: the flag of textures already in the project, rewritten in place
//     (Importer::setTextureSrgb: same file, UUID and pixels). This is the batch
//     fix for everything imported before the flag, which is linear whatever
//     it holds.
//
// Drawn from EditorUI::render at root level, not from the Content Browser:
// File ▸ Import Asset opens it too, and the browser is not drawn while an asset
// tab is in front. All state is file-static in the .cpp and held as strings.
namespace TextureColourSpaceDialog
{
	// Import each of `sources` (texture files) into <contentRoot>/<its relDir>.
	// `relDirs` is parallel to `sources`; each is relative to `contentRoot`.
	void openImport(const std::vector<std::string>& sources,
	                const std::vector<std::string>& relDirs,
	                const std::string&              contentRoot);

	// Set the flag on each of `assets` (texture .hasset files). Paths outside
	// `contentRoot` are dropped; they cannot be rewritten from this project.
	void openRetag(const std::vector<std::string>& assets,
	               const std::string&              contentRoot);

	// The dialog, while one of the above has asked for it. Once per frame.
	void Draw(AppContext& ctx);
}
