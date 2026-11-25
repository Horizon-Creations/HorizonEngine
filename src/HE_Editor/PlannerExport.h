#pragma once
#include "Planner.h"
#include "PlannerMarkdown.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// ── Planner content as files other programs open ────────────────────────────
// ImGui-free, so the tab and the tests share it.
//
//   * a page as Word (.docx): headings as Word's Heading 1-3 (they show in its
//     navigation pane), lists and to-dos as marked, indented paragraphs, code in a
//     monospace shaded paragraph, a divider as a rule. The Markdown export is
//     pageToMarkdown (PlannerMarkdown.h).
//   * dated rows as iCalendar (.ics): all-day events, one per row, the row's id as
//     the UID, so importing the file again updates the events instead of adding
//     them twice. Calendar on macOS (and Outlook, Google Calendar) imports it.
namespace HE::Planner
{

// CRC-32 as zip uses it (IEEE, reflected): crc32("123456789") == 0xCBF43926.
std::uint32_t crc32(std::string_view data);

// A zip archive holding `files` (name, bytes) uncompressed. Enough for .docx.
std::string zipStored(const std::vector<std::pair<std::string, std::string>>& files);

std::string pageToDocx(const Page& page, const MarkdownContext& ctx = {});

struct CalendarEvent
{
	UUID         id;            // the row: the event's UID
	std::string  title;
	std::int64_t firstDay = 0;  // days since 1970-01-01 (UTC)
	std::int64_t lastDay  = 0;  // inclusive
	std::string  description;
};
std::string eventsToIcs(const std::vector<CalendarEvent>& events, std::string_view calendarName, std::int64_t nowMs);

// <project>/Export/Planner/<title>.<ext>, the title made safe for a file name
// (`fallback` when nothing is left of it).
std::filesystem::path exportFileFor(const std::filesystem::path& projectRoot, const std::string& title,
                                    const char* ext, const char* fallback);

// Writes `bytes` to `file`, making its folder. False (with `error`) when it could not.
bool writeExport(const std::filesystem::path& file, const std::string& bytes, std::string* error = nullptr);

} // namespace HE::Planner
