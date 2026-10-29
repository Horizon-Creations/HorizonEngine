// A LandscapeFoliageLayer as JSON: the payload of CHUNK_LFOL. The data itself is
// LandscapeFoliageLayerAsset in Assets.h, with the reasoning in its comment and
// in docs/landscape-foliage-layer-entwurf-2026-10-29.md.
//
// Two layers of checking, kept apart on purpose:
//  * PARSING decides whether the file is readable at all. A document that is not
//    JSON, has a version this build does not know, holds more entries than a
//    landscape can carry, or names an asset in a malformed way is NOT loaded —
//    registering it as an empty set would let the next save overwrite the
//    author's rules with nothing (the Sequence loader's rule).
//  * VALIDATION reports what is readable but wrong (a slope range turned
//    around, a paint channel that does not exist). The asset still loads, so an
//    author can open it and fix it; the apply step is what refuses to use it.
#pragma once

#include <Types/Defines.h> // HE_API
#include <Types/UUID.h>
#include <string>
#include <vector>

struct LandscapeFoliageLayerAsset;

namespace HE
{

// The document format version this build reads and writes.
inline constexpr int kLandscapeFoliageLayerVersion = 1;

// The document, pretty-printed (keys sort, so equal data is equal bytes — the
// publish scan and the pack manifest hash the file). Only the set's data: name,
// id and path are the asset header's.
HE_API std::string landscapeFoliageLayerToJson(const LandscapeFoliageLayerAsset& a);

// Parse `json` into the data fields of `out`, leaving id/name/path alone. False,
// with `out` untouched and `error` (when given) saying why, when the text is not
// a JSON object, "version" is missing or not kLandscapeFoliageLayerVersion,
// "entries" is not an array of at most kLandscapeFoliageMaxEntries objects, an
// entry has more than kLandscapeFoliageMaxLods lods, a field has the wrong JSON
// type, or an asset id is not {"hi":<uint>,"lo":<uint>} (null/absent = none).
// Unknown keys are ignored so a newer writer's additions do not break an older
// reader of the same version.
HE_API bool landscapeFoliageLayerFromJson(const std::string& json, LandscapeFoliageLayerAsset& out,
                                          std::string* error = nullptr);

// What is wrong with a readable set; empty = fine. One line per problem, naming
// the entry ("entry 2 'Rock': ...").
HE_API std::vector<std::string> validateLandscapeFoliageLayer(const LandscapeFoliageLayerAsset& a);

// Every mesh and material the set names (entries and their lods), each once, in
// first-seen order, null ids left out. For ContentManager::expandFrontier.
HE_API void landscapeFoliageLayerAssetRefs(const LandscapeFoliageLayerAsset& a, std::vector<HE::UUID>& out);

} // namespace HE
