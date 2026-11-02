#pragma once
#include "TaskBoard.h"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

// ── The task board's JSON helpers, shared with the Planner ───────────────────
// Config/Planner/* follows the same bargain with git as Config/TaskBoard.json
// (lossless, ordered keys, unknown keys kept, ids as 32 hex digits), so it reads
// and writes with the very same helpers instead of a second copy that could
// drift. They live in TaskBoard.cpp's anonymous namespace; this header is the
// door. Internal to the editor — not part of any exported API.
namespace HE::TaskJson
{

// ORDERED: a load-then-save keeps the file's key order.
using ojson = nlohmann::ordered_json;

void readString(const ojson& j, const char* key, std::string& out);
void readStrings(const ojson& j, const char* key, std::vector<std::string>& out);
bool readUuid(const ojson& j, const char* key, UUID& out);
void readTime(const ojson& j, const char* key, std::int64_t& out);

// Every key of `obj` not in `known`, as a JSON object string ("" when none).
std::string extrasOf(const ojson& obj, std::initializer_list<std::string_view> known);
// Adds the kept keys after the known ones; a key the caller already wrote wins.
void appendExtras(ojson& out, const std::string& extraJson);

// Pretty-printed, LF only, UTF-8 unescaped, one trailing newline.
std::string dumpDocument(const ojson& j);

TaskLink readLink(const ojson& j);
ojson    writeLink(const TaskLink& l);

} // namespace HE::TaskJson
