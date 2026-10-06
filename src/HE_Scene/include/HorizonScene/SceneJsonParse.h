#pragma once
#include <nlohmann/json.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace HE
{

// Parse the JSON text of a .hescene. The result is exactly
// nlohmann::json::parse(text, nullptr, false), but a large top-level "entities"
// array is cut at its element boundaries and the pieces are parsed in parallel
// on the job pool: parsing was two thirds of loading a 200k-entity scene, all of
// it on the main thread (Thema 153, docs/world-streaming-baseline-2026-10-06.md).
//
// Anything unexpected in the text's shape (no "entities" array, the key twice,
// an escaped top-level key, an element that is not an object, a piece that does
// not parse) falls back to the one sequential parse, so a broken file is
// reported exactly as before. A discarded json means invalid JSON, as there.
//
// minPieceBytes: the text is only split into pieces of at least this many bytes
// (below twice that it is parsed in one go). Tests lower it to split tiny scenes.
// piecesUsed (optional): how many pieces the entities were parsed in; 1 when the
// text was parsed in one go, fallback included. Lets a test see the split happen.
nlohmann::json parseSceneText(const std::string& text, size_t minPieceBytes = size_t(1) << 20,
                              size_t* piecesUsed = nullptr);

// The same for the CBOR form of a scene (.hescene saved binary, the scene inside
// a .hpak, in-memory snapshots): exactly nlohmann::json::from_cbor(data, true,
// false). The element boundaries come from walking the CBOR item heads, which
// costs a fraction of building the tree. Indefinite lengths, tags, the key
// twice, an element that is not a map or anything that does not walk cleanly to
// the last byte take the one sequential decode.
nlohmann::json parseSceneCbor(const std::vector<uint8_t>& data, size_t minPieceBytes = size_t(1) << 20,
                              size_t* piecesUsed = nullptr);

} // namespace HE
