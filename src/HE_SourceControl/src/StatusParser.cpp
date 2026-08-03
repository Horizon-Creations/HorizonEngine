#include "SourceControl/RepoStatus.h"

#include "ScLog.h"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace HE::Sc {
namespace {

std::string toLower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

// One letter of porcelain v2's XY field.
FileState stateFromCode(char c)
{
	switch (c)
	{
	case '.': return FileState::Unmodified;
	case 'M': return FileState::Modified;
	case 'A': return FileState::Added;
	case 'D': return FileState::Deleted;
	case 'R': return FileState::Renamed;
	case 'C': return FileState::Copied;
	case 'T': return FileState::TypeChanged;
	case 'U': return FileState::Conflicted;
	default:  break;
	}
	return FileState::Unmodified;
}

int parseInt(std::string_view sv)
{
	int value = 0;
	if (sv.empty()) return 0;
	const bool negative = sv.front() == '-';
	if (negative || sv.front() == '+') sv.remove_prefix(1);
	std::from_chars(sv.data(), sv.data() + sv.size(), value);
	return negative ? -value : value;
}

// Splits one whitespace-delimited token off the front.
std::string_view takeToken(std::string_view& rest)
{
	while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
	const std::size_t end = rest.find(' ');
	std::string_view token = rest.substr(0, end);
	rest = (end == std::string_view::npos) ? std::string_view{} : rest.substr(end + 1);
	return token;
}

} // namespace

const char* fileStateName(FileState s)
{
	switch (s)
	{
	case FileState::Unmodified:  return "unmodified";
	case FileState::Modified:    return "modified";
	case FileState::Added:       return "added";
	case FileState::Deleted:     return "deleted";
	case FileState::Renamed:     return "renamed";
	case FileState::Copied:      return "copied";
	case FileState::TypeChanged: return "type changed";
	case FileState::Untracked:   return "untracked";
	case FileState::Ignored:     return "ignored";
	case FileState::Conflicted:  return "conflicted";
	}
	return "?";
}

const FileEntry* RepoStatus::find(const std::string& repoRelativePath) const
{
	if (const auto it = files.find(repoRelativePath); it != files.end()) return &it->second;

	// Case-insensitive fallback. Only reachable when the exact lookup missed, so
	// a correctly-cased path never pays for it.
	if (const auto ci = caseIndex.find(toLower(repoRelativePath)); ci != caseIndex.end())
		if (const auto it = files.find(ci->second); it != files.end()) return &it->second;

	return nullptr;
}

void buildDirtyFolders(RepoStatus& status)
{
	status.dirtyFolders.clear();
	for (const auto& [path, entry] : status.files)
	{
		if (!entry.dirty()) continue;
		// Walk up the path, inserting every ancestor. O(depth) per file, done
		// once here rather than per folder tile per frame.
		std::size_t slash = path.rfind('/');
		while (slash != std::string::npos)
		{
			std::string folder = path.substr(0, slash);
			if (folder.empty()) break;
			// Already present means every ancestor above it is too — an earlier
			// file inserted the whole chain.
			if (!status.dirtyFolders.insert(folder).second) break;
			slash = folder.rfind('/');
		}
	}
}

bool parsePorcelainV2(const std::string& raw, RepoStatus& out)
{
	out.files.clear();
	out.caseIndex.clear();
	out.dirtyFolders.clear();

	// Records are NUL-terminated. A rename record is the one case that spans two
	// of them — path first, then the original path — so the reader must be able
	// to pull an extra field, which is why this is an index walk rather than a
	// split-and-loop.
	std::size_t pos = 0;
	auto nextRecord = [&](std::string_view& out_rec) -> bool {
		if (pos >= raw.size()) return false;
		const std::size_t nul = raw.find('\0', pos);
		const std::size_t end = (nul == std::string::npos) ? raw.size() : nul;
		out_rec = std::string_view(raw).substr(pos, end - pos);
		pos = (nul == std::string::npos) ? raw.size() : nul + 1;
		return true;
	};

	auto record = [&](std::string path, FileEntry entry) {
		out.caseIndex[toLower(path)] = path;
		out.files.emplace(std::move(path), std::move(entry));
	};

	std::string_view rec;
	while (nextRecord(rec))
	{
		if (rec.empty()) continue;

		// ── Header lines ────────────────────────────────────────────────────
		if (rec.front() == '#')
		{
			std::string_view rest = rec.substr(1);
			const std::string_view key = takeToken(rest);
			if (key == "branch.oid")
			{
				const std::string_view v = takeToken(rest);
				// A fresh repository with no commits reports the literal
				// "(initial)" here; treating that as an oid would later produce
				// nonsense diffs against a commit that does not exist.
				if (v == "(initial)") out.initialCommit = true;
				else                  out.headOid = std::string(v);
			}
			else if (key == "branch.head")
			{
				const std::string_view v = takeToken(rest);
				if (v == "(detached)") out.detached = true;
				else                   out.branch = std::string(v);
			}
			else if (key == "branch.upstream")
			{
				out.upstream = std::string(takeToken(rest));
			}
			else if (key == "branch.ab")
			{
				// "+2 -1" — ahead of and behind the upstream. Present only when an
				// upstream is configured, which is why ahead/behind default to 0
				// rather than to "unknown".
				out.ahead  = parseInt(takeToken(rest));
				out.behind = -parseInt(takeToken(rest));   // reported as "-N"
			}
			continue;
		}

		const char kind = rec.front();
		std::string_view rest = rec.substr(1);

		if (kind == '?' || kind == '!')
		{
			// "? <path>" / "! <path>" — no other fields.
			while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
			if (rest.empty()) continue;
			FileEntry e;
			e.worktree = (kind == '?') ? FileState::Untracked : FileState::Ignored;
			record(std::string(rest), e);
			continue;
		}

		if (kind == '1' || kind == '2')
		{
			// 1 <XY> <sub> <mH> <mI> <mW> <hH> <hI> <path>
			// 2 <XY> <sub> <mH> <mI> <mW> <hH> <hI> <Xscore> <path> NUL <origPath>
			const std::string_view xy = takeToken(rest);
			if (xy.size() < 2) continue;
			takeToken(rest);   // sub
			takeToken(rest);   // mH
			takeToken(rest);   // mI
			takeToken(rest);   // mW
			takeToken(rest);   // hH
			takeToken(rest);   // hI
			if (kind == '2') takeToken(rest);   // rename/copy score

			while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
			if (rest.empty()) continue;

			FileEntry e;
			e.index    = stateFromCode(xy[0]);
			e.worktree = stateFromCode(xy[1]);

			std::string path(rest);
			if (kind == '2')
			{
				// The original path is a SEPARATE NUL-terminated field. Missing
				// this is what makes a rename swallow the following record.
				std::string_view orig;
				if (nextRecord(orig)) e.origPath = std::string(orig);
			}
			record(std::move(path), e);
			continue;
		}

		if (kind == 'u')
		{
			// u <XY> <sub> <m1> <m2> <m3> <mW> <h1> <h2> <h3> <path>
			const std::string_view xy = takeToken(rest);
			if (xy.size() < 2) continue;
			for (int i = 0; i < 8; ++i) takeToken(rest);   // sub, m1..mW, h1..h3

			while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
			if (rest.empty()) continue;

			FileEntry e;
			// Both halves are marked conflicted regardless of the exact XY pair:
			// every unmerged combination means the same thing to a caller, which
			// is "this cannot be committed until a human resolves it".
			e.index    = FileState::Conflicted;
			e.worktree = FileState::Conflicted;
			record(std::string(rest), e);
			continue;
		}

		// An unknown record type is skipped rather than treated as a failure, so
		// a newer git that adds one degrades to "that file has no badge" instead
		// of "status is broken".
		HE_SC_DEBUG("Unrecognised porcelain record type '%c' — skipped", kind);
	}

	buildDirtyFolders(out);
	return true;
}

} // namespace HE::Sc
