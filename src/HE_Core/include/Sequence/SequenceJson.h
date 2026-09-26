// A cinematic Sequence as JSON: the payload of CHUNK_SEQU, and the form the MCP
// tools read and write. The data itself is SequenceAsset in Assets.h, next to the
// PropertyAnimClipAsset whose channel type its property tracks reuse.
//
// ── Why JSON and not a binary chunk like PANM ────────────────────────────────
// A sequence references other assets (skeletal clips, sounds) and other ENTITIES
// (its bindings), all by UUID. The reference scan (AssetRefScan.cpp) and the
// rename retarget (AssetRefRetarget.cpp) can only see a UUID that is spelled as
// {"hi":…,"lo":…} inside a JSON payload chunk; sixteen raw bytes in a binary
// chunk are invisible to both, and the delete dialog would answer "nothing
// references it" about a clip a cutscene plays. Every authored asset added since
// PANM (BLSP, BMSK, THEM, ASMG, SGTP) is JSON for the same reason.
//
// Unlike BlendSpaceAsset, the asset holds the PARSED form, not the text: the
// runtime evaluates a sequence every frame, and parsing it there would be the
// wrong place to find out it is broken. The loader parses once.
#pragma once

#include <Types/Defines.h> // HE_API
#include <Types/UUID.h>
#include <string>
#include <vector>

struct SequenceAsset;

namespace HE
{

// The document, pretty-printed. Only the sequence's data — id, name and path are
// the asset header's, not the payload's.
HE_API std::string sequenceToJson(const SequenceAsset& s);

// Parse `json` into the data fields of `out` (duration, frameRate, bindings,
// tracks), leaving id/name/path alone. False, with `out` untouched, when the text
// is not a JSON object at all. Individual bad entries are dropped rather than
// failing the whole document — a property channel whose times and values differ
// in length, a target or kind this build does not know — for the same reason the
// PANM loader stops at a short channel: the runtime would index past the end.
// `dropped`, when given, counts them so a caller can say so.
HE_API bool sequenceFromJson(const std::string& json, SequenceAsset& out, int* dropped = nullptr);

// Every asset the sequence itself references (skeletal clips, sounds), each once,
// in first-seen order. Entity bindings are NOT assets and are not listed. For the
// scene's preload list (SceneSystems::collectAssetRefs, once the player component
// exists) — a clip only the sequence names would otherwise not be resident on
// the first frame, and its track would silently do nothing.
HE_API void sequenceAssetRefs(const SequenceAsset& s, std::vector<HE::UUID>& out);

} // namespace HE
