#pragma once

// ─── Finalizing a temp file on Windows: rename with a short retry ────────────
// Every temp-then-rename in this module (a download's ".partial", the cached
// manifest's ".tmp") ends in std::filesystem::rename, which MSVC implements as
// MoveFileExW(MOVEFILE_REPLACE_EXISTING). That fails whenever anyone holds the
// source or the destination open without FILE_SHARE_DELETE — and Defender and
// the search indexer open freshly closed files for a moment to look at them.
// The source held is ERROR_SHARING_VIOLATION (32, "used by another process"),
// the destination held is ERROR_ACCESS_DENIED (5). Both clear on their own
// within milliseconds, so a few short retries turn a startup failure into a
// delay nobody notices (Git for Windows does the same).
//
// Thema 187 (docs/engine-content-manifest-windows-lock-2026-10-13.md). This is
// the second line of defence only: the race that actually produced the error was
// two refreshes of the editor itself, which EngineContentSync now serializes.
//
// macOS/Linux: rename(2) swaps the directory entry under any open descriptor and
// has no such failure, so there this is exactly one fs::rename, unchanged.
#include "ContentSync/CsLog.h"

#include <filesystem>
#include <system_error>

#ifdef _WIN32
#include <chrono>
#include <thread>
#endif

namespace HE::Cs::detail {

inline void renameReplacing(const std::filesystem::path& from, const std::filesystem::path& to,
                            std::error_code& ec)
{
	std::filesystem::rename(from, to, ec);
#ifdef _WIN32
	// ~20 + 40 + 80 + 160 + 320 ms: long enough for a scanner's look, short
	// enough that a file someone REALLY keeps open still fails within a second.
	constexpr int kRetries = 5;
	for (int attempt = 1; ec && attempt <= kRetries; ++attempt)
	{
		if (ec.value() != 32 && ec.value() != 5) break;   // not a sharing conflict
		// Logged so a log from an affected machine shows the conflict even when
		// a retry then succeeds — the diagnosis doc tells its causes apart this way.
		HE_LOG_WARN(ContentSync, "Renaming '%s' failed (%d: %s) — retry %d of %d",
		            to.filename().string().c_str(), ec.value(), ec.message().c_str(), attempt, kRetries);
		std::this_thread::sleep_for(std::chrono::milliseconds(20 << (attempt - 1)));
		std::filesystem::rename(from, to, ec);
	}
#endif
}

} // namespace HE::Cs::detail
