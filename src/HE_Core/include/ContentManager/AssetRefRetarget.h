#pragma once
#include "Types/Defines.h"
#include <cstdint>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
//  Asset reference retargeting — "moving an asset must not break what points at it"
//
//  In the editor, assets reference each other by content-relative PATH:
//    mesh → material (MREF), material → shader/textures/parent material (MTRL),
//    material node graph → project textures + material functions (the JSON inside
//    MTRL), HorizonCode graphs → classes/widgets/scenes, widget trees → textures
//    and fonts, input mapping contexts → input actions, …
//  (Scene COMPONENTS reference by UUID instead — those survive a move untouched;
//  paths do not.) Renaming or moving a file in the Content Browser therefore has
//  to carry every stored reference over with it, including the asset's own
//  embedded META path, which is what the packer builds its path→UUID map from.
//
//  The rewrite deliberately does NOT parse assets into their typed structs and
//  re-serialize them: that would mean a second copy of every chunk layout to keep
//  in sync (see the FIELD-SYNCHRONISED warnings around the MTRL writer), and it
//  would rewrite megabytes of geometry/pixels to change one string. Instead it
//  edits the raw .hasset chunk bytes, recognising the two shapes a path can be
//  stored in — a length-prefixed string, and a value inside a JSON blob — and
//  leaves everything else byte-verbatim.
// ─────────────────────────────────────────────────────────────────────────────

namespace HE::AssetRefs
{

// One path substitution. `prefix` marks a FOLDER move: besides the exact path,
// every reference that starts with `from + '/'` is re-rooted onto `to`.
struct Rule
{
	std::string from;
	std::string to;
	bool        prefix = false;
};

// The substitutions a single move implies. `oldRel`/`newRel` are content-relative
// (an "Engine/…" prefix included, exactly as stored in references). Besides the
// content-relative form this also produces the PROJECT-relative one
// ("Content/Level.hescene") that scene references use — pass the content root's
// directory name as `contentDirName` ("" to skip that form).
HE_API std::vector<Rule> moveRules(const std::string& oldRel, const std::string& newRel,
                                   bool folder, const std::string& contentDirName);

// Retarget one stored path VALUE (a reference field, an index key, an asset's own
// path). Only WHOLE values are rewritten — a path is stored complete, so matching
// less than that would corrupt text which merely contains the name. Returns true
// when `value` changed.
HE_API bool retargetValue(std::string& value, const std::vector<Rule>& rules);

// Apply `rules` to one raw .hasset blob. Returns true when anything changed.
HE_API bool retargetBlob(std::vector<uint8_t>& blob, const std::vector<Rule>& rules);

// Apply `rules` to the quoted strings of a JSON document (the .heproj manifest,
// or any raw-JSON asset chunk). Returns true when anything changed.
HE_API bool retargetJsonText(std::string& text, const std::vector<Rule>& rules);

// Rewrite every .hasset under `root` (recursively). Files that do not mention any
// of the rules' paths are not even opened for writing. Returns how many files
// were rewritten.
HE_API size_t retargetTree(const std::string& root, const std::vector<Rule>& rules);

} // namespace HE::AssetRefs
