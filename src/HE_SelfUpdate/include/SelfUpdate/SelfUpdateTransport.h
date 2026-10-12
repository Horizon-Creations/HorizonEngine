#pragma once

// ─── Where update files come from ─────────────────────────────────────────────
// The check and the download need exactly one thing from a server: "give me the
// file at this relative path". IUpdateTransport is that one operation, so the
// logic above it (compare versions, pick the platform's artifact, verify the
// hash, stage it) is testable and reusable without a network:
//
//   * makeSftpTransport      — the real one: HorizonContentSync's SftpClient,
//                              with the self-update account's endpoint handed in.
//   * makeLocalDirTransport  — a directory laid out like the server. For tests,
//                              and for the CI matrix to prove the whole path on a
//                              runner without standing up an SSH server first.
//
// Both write a file the way SftpClient does: into "<local>.partial" first and
// rename on success, so an interrupted transfer never leaves a file that could
// be mistaken for a complete one.

#include "SelfUpdate/SuCommon.h"

#include <ContentSync/SftpCredentials.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace HE::Su {

struct SuResult
{
	bool        ok = false;
	std::string error;        // human-readable; never contains the account's password
	// The failure was the connection itself and a retry a moment later may fix
	// it (same meaning as HE::Cs::SftpResult::transient). A missing file, a
	// refused password or an unsafe path is never transient.
	bool        transient = false;
};

// (bytesDone, bytesTotal); total is 0 when the source did not say.
using SuProgressFn = std::function<void(std::uint64_t, std::uint64_t)>;

class IUpdateTransport
{
public:
	virtual ~IUpdateTransport() = default;

	// Fetches `remoteRelPath` (relative to the account's base directory) into
	// `localPath`, creating its parent directories. Blocking: worker threads only.
	virtual SuResult fetchFile(const std::string& remoteRelPath,
	                           const std::filesystem::path& localPath,
	                           const SuProgressFn& progress) = 0;

	// For logs: where this reads from. Never a secret.
	virtual std::string describe() const = 0;
};

// `endpoint` should come from loadSelfUpdateConfig(); an endpoint that is not
// fully configured makes every fetch fail with a clear message (no connection
// is attempted, and nothing falls back to the EngineContent account).
HE_SU_API std::unique_ptr<IUpdateTransport> makeSftpTransport(const HE::Cs::SftpEndpoint& endpoint);

HE_SU_API std::unique_ptr<IUpdateTransport> makeLocalDirTransport(const std::filesystem::path& root);

} // namespace HE::Su
