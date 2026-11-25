#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// ── Project thumbnail ────────────────────────────────────────────────────────
// The picture the Project Hub shows on a recent project's card: the editor
// viewport as it looked when the project was last closed (or the editor quit).
// Taken by EditorApplication::captureProjectThumbnail, read back by the hub.
//
// It lives in <project>/Saved/, which the .gitignore the editor writes already
// excludes (RepoConfig::gitignoreText): it is regenerated on every close and
// differs per machine, so committing it would only make noise in every pull.
//
// ImGui- and renderer-free, so the crop/scale and the file round trip are
// tested on their own (tests/test_project_hub_ui.cpp).
namespace ProjectThumbnail
{
	// The card picture's size. 16:9 like a typical viewport, and twice the size
	// the hub draws a card at on a 1x display, so a Retina card is not blurry.
	inline constexpr int kWidth  = 480;
	inline constexpr int kHeight = 270;

	// <dir of the .heproj>/Saved/ProjectThumbnail.png. Empty for an empty path.
	std::filesystem::path fileFor(const std::string& heprojPath);

	// Cover-crop a tightly packed top-down RGBA8 frame to kWidth:kHeight and
	// box-filter it down to kWidth×kHeight, alpha forced opaque (the viewport's
	// alpha is whatever the last pass left there). False for an empty frame.
	bool fit(const std::vector<std::uint8_t>& rgba, std::uint32_t w, std::uint32_t h,
	         std::vector<std::uint8_t>& out);

	// True when every pixel has the same colour: an application project has no
	// world, so its viewport is a flat clear colour, and a card showing that is
	// worse than the placeholder.
	bool isFlat(const std::vector<std::uint8_t>& rgba);

	// fit() + write fileFor(heprojPath). False (and nothing written) for a flat
	// or empty frame, or when the file cannot be written.
	bool save(const std::string& heprojPath, const std::vector<std::uint8_t>& rgba,
	          std::uint32_t w, std::uint32_t h);
}
