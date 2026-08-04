#pragma once

// ─── Generated repository configuration ──────────────────────────────────────
// The .gitignore and .gitattributes a HorizonEngine project needs, written once
// at `git init` time. The content encodes two decisions that live here rather
// than in anyone's head:
//
//   • What is COMMITTED: authored data — Content/**, Config/, Shaders/,
//     Source/** and the .heproj. What is IGNORED: everything the engine can
//     regenerate (Saved/, Export/, build output, hot-reload dylib copies, logs).
//
//   • What goes through LFS: the same "these are the big files" judgement that
//     keeps meshes, textures and audio off the collaboration wire routes them
//     into LFS here — plus paks, video and raw source art. Authored graph
//     formats stay OUT of LFS so they remain diffable text/CBOR in reviews.
//
// Init-time only. Once the files exist they belong to the user; regenerating
// them silently would overwrite whatever they added, so later changes must be
// offered as a diff, never applied.

#include "SourceControl/ScCommon.h"

#include <filesystem>
#include <string>

namespace HE::Sc {

class HE_SC_API RepoConfig {
public:
	// The exact text written at init. Exposed so tests can pin the content and
	// the panel can preview it.
	static std::string gitignoreText();
	static std::string gitattributesText();

	// Write both files into `root` — only the ones that do not exist yet, per
	// the contract above. Returns false when a write failed (err says which).
	static bool writeInitialFiles(const std::filesystem::path& root,
	                              std::string* err = nullptr);
};

} // namespace HE::Sc
