#pragma once
#include "Planner.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

// ── The Planner's file rules, for every kind of document ─────────────────────
// Databases, pages and boards share one set of rules for reading and writing their
// files (the TaskBoard's: never save over a file that does not parse, never over one
// that changed since it was read, temp + rename). They are written once, in Planner.cpp;
// this is the door for the document kinds that live in other files (PlannerBoard.cpp).
// Internal to the editor — not part of any exported API.
namespace HE::Planner::detail
{

using ParseFn = std::function<Database::LoadResult(std::string_view)>;

// The ids of the `<32 hex>.json` files in `dir`, ordered by id.
std::vector<UUID> listDir(const std::filesystem::path& dir);

// No file: `reset()` and an ok result. A file that does not read or parse: !ok with the
// reason. `hadFile`/`bytes` are the document's remembered disk state (see saveFile).
Database::LoadResult loadFile(const std::filesystem::path& path, const char* what, const ParseFn& parse,
                              const std::function<void()>& reset, bool& hadFile, std::string& bytes);

// Writes `text` unless the file already holds it. Refuses (false, `error` says why) when
// the file on disk does not parse, or when it is not what the document last saw.
// `nothingToSay`: a document with no content and no file writes nothing.
bool saveFile(const std::filesystem::path& path, const char* what, const std::string& text, bool nothingToSay,
              const ParseFn& parse, bool& hadFile, std::string& bytes, std::string* error);

bool deleteFile(const std::filesystem::path& path, std::string* error);

// Parse a document: a JSON object with an optional positive integer "version", and, when
// `listKey` is given, that key (if present) a list.
bool openDocument(std::string_view text, const char* listKey, nlohmann::ordered_json& out, int& version,
                  Database::LoadResult& r);

} // namespace HE::Planner::detail
