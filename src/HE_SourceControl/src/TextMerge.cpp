#include "SourceControl/TextMerge.h"

#include <algorithm>
#include <cstdint>

namespace HE::Sc
{

namespace
{

std::vector<std::string> splitLines(std::string_view text, bool& endsWithNewline)
{
	std::vector<std::string> lines;
	endsWithNewline = text.empty() || text.back() == '\n';
	std::size_t at = 0;
	while (at < text.size())
	{
		const std::size_t nl = text.find('\n', at);
		if (nl == std::string_view::npos)
		{
			lines.emplace_back(text.substr(at));
			break;
		}
		lines.emplace_back(text.substr(at, nl - at));
		at = nl + 1;
	}
	return lines;
}

void pushSame(std::vector<DiffBlock>& out, const std::string& line)
{
	if (out.empty() || !out.back().same) out.push_back(DiffBlock{});
	out.back().ours.push_back(line);
	out.back().theirs.push_back(line);
}

void pushChange(std::vector<DiffBlock>& out, const std::string* ours, const std::string* theirs)
{
	if (out.empty() || out.back().same)
	{
		DiffBlock b;
		b.same = false;
		out.push_back(std::move(b));
	}
	if (ours)   out.back().ours.push_back(*ours);
	if (theirs) out.back().theirs.push_back(*theirs);
}

} // namespace

std::size_t TextDiff::changes() const
{
	std::size_t n = 0;
	for (const DiffBlock& b : blocks) n += b.same ? 0 : 1;
	return n;
}

std::string TextDiff::merge(const std::vector<Pick>& picks) const
{
	std::vector<const std::string*> lines;
	std::size_t change = 0;
	bool lastFromOurs = false;
	for (const DiffBlock& b : blocks)
	{
		if (b.same)
		{
			for (const std::string& l : b.theirs) lines.push_back(&l);
			lastFromOurs = false;
			continue;
		}
		const Pick p = change < picks.size() ? picks[change] : Pick::Theirs;
		++change;
		if (p == Pick::Ours || p == Pick::Both)
		{
			for (const std::string& l : b.ours) lines.push_back(&l);
			lastFromOurs = !b.ours.empty();
		}
		if (p == Pick::Theirs || p == Pick::Both)
		{
			for (const std::string& l : b.theirs) lines.push_back(&l);
			if (!b.theirs.empty()) lastFromOurs = false;
		}
	}
	std::string out;
	for (std::size_t i = 0; i < lines.size(); ++i)
	{
		out += *lines[i];
		if (i + 1 < lines.size()) out += '\n';
	}
	// The final newline follows whichever side the last line came from.
	const bool newline = lastFromOurs ? oursEndsWithNewline : theirsEndsWithNewline;
	if (!lines.empty() && newline) out += '\n';
	return out;
}

TextDiff diffText(std::string_view oursText, std::string_view theirsText)
{
	TextDiff d;
	const std::vector<std::string> a = splitLines(oursText, d.oursEndsWithNewline);
	const std::vector<std::string> b = splitLines(theirsText, d.theirsEndsWithNewline);

	std::size_t pre = 0;
	while (pre < a.size() && pre < b.size() && a[pre] == b[pre]) ++pre;
	std::size_t suf = 0;
	while (suf < a.size() - pre && suf < b.size() - pre && a[a.size() - 1 - suf] == b[b.size() - 1 - suf]) ++suf;

	for (std::size_t i = 0; i < pre; ++i) pushSame(d.blocks, a[i]);

	const std::size_t n = a.size() - pre - suf, m = b.size() - pre - suf;
	if (n > 0 || m > 0)
	{
		if (n == 0 || m == 0 || n * m > kMaxDiffCells)
		{
			for (std::size_t i = 0; i < n; ++i) pushChange(d.blocks, &a[pre + i], nullptr);
			for (std::size_t j = 0; j < m; ++j) pushChange(d.blocks, nullptr, &b[pre + j]);
		}
		else
		{
			// lcs[i][j]: common lines of a[pre+i..] and b[pre+j..].
			std::vector<std::uint32_t> lcs((n + 1) * (m + 1), 0);
			auto at = [&](std::size_t i, std::size_t j) -> std::uint32_t& { return lcs[i * (m + 1) + j]; };
			for (std::size_t i = n; i-- > 0;)
				for (std::size_t j = m; j-- > 0;)
					at(i, j) = a[pre + i] == b[pre + j] ? at(i + 1, j + 1) + 1 : std::max(at(i + 1, j), at(i, j + 1));
			std::size_t i = 0, j = 0;
			while (i < n || j < m)
			{
				if (i < n && j < m && a[pre + i] == b[pre + j]) { pushSame(d.blocks, a[pre + i]); ++i; ++j; }
				else if (j < m && (i == n || at(i, j + 1) >= at(i + 1, j))) { pushChange(d.blocks, nullptr, &b[pre + j]); ++j; }
				else { pushChange(d.blocks, &a[pre + i], nullptr); ++i; }
			}
		}
	}
	for (std::size_t i = 0; i < suf; ++i) pushSame(d.blocks, a[a.size() - suf + i]);
	return d;
}

bool looksBinary(std::string_view bytes)
{
	return bytes.substr(0, 8000).find('\0') != std::string_view::npos;
}

} // namespace HE::Sc
