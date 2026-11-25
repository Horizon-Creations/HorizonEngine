#pragma once

// ─── Two versions of a file, line by line ────────────────────────────────────
// The team server keeps both sides when two saves changed the same thing: the
// later one in the project, the earlier one on its horizon-conflicts branch. This
// is what the editor's conflict window shows and merges them with: the two texts
// as runs of equal lines and changed blocks, and the text that results from
// picking, per changed block, ours, theirs or both.
//
// Line based on purpose: scenes, settings, planner pages and scripts are all
// line-oriented text (JSON is pretty-printed one value per line), and a person
// can read a line diff. A file that is not text (a NUL byte early on) has no
// blocks to pick from; the window offers the whole file either way.

#include "ScCommon.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace HE::Sc
{

struct HE_SC_API DiffBlock
{
	bool                     same = true;   // equal in both; otherwise a changed block
	std::vector<std::string> ours, theirs;  // `same`: ours == theirs
};

enum class Pick : unsigned char { Ours, Theirs, Both };   // Both: ours, then theirs

struct HE_SC_API TextDiff
{
	std::vector<DiffBlock> blocks;
	bool oursEndsWithNewline   = true;
	bool theirsEndsWithNewline = true;

	std::size_t changes() const;   // changed blocks
	// The merged text: one pick per changed block, in order (missing picks: Theirs).
	std::string merge(const std::vector<Pick>& picks) const;
};

// Lines are split on '\n'; a '\r' stays part of its line, so CRLF files come back
// as they were. Common lines at the start and end are matched first; the middle is
// matched by longest common subsequence while it is small enough, and is one
// changed block when it is not (more than kMaxCells line pairs).
HE_SC_API TextDiff diffText(std::string_view ours, std::string_view theirs);
constexpr std::size_t kMaxDiffCells = 4'000'000;

// A NUL byte in the first 8000 bytes, as git decides.
HE_SC_API bool looksBinary(std::string_view bytes);

} // namespace HE::Sc
