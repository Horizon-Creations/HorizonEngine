#pragma once
#include <string_view>

// ── Is a setting pinned to Quick Settings? ───────────────────────────────────
// EditorConfig::QuickSettingsFavorites is a comma-separated list of stable keys
// ("backend,vsync,grid"). The Preferences pages ask this once per row, every
// frame, to draw the pin — and the first spelling of it glued ",<list>," and
// ",<key>," together to search one inside the other: two strings per row, 27
// allocations a frame in the performance audit
// (docs/perf-audit/step3-cpu-memory-deep-dive-2026-09-27.md, 4.2).
//
// This walks the list in place instead. Same answer, including for the odd
// lists a hand-edited config.json can hold: empty tokens between doubled or
// trailing commas are skipped, and an empty key is never "pinned".
namespace HE::Ed
{
inline bool favoritesContain(std::string_view list, std::string_view key)
{
	if (key.empty()) return false;
	while (!list.empty())
	{
		const std::size_t comma = list.find(',');
		const std::string_view token = list.substr(0, comma);
		if (token == key) return true;
		if (comma == std::string_view::npos) break;
		list.remove_prefix(comma + 1);
	}
	return false;
}
} // namespace HE::Ed
