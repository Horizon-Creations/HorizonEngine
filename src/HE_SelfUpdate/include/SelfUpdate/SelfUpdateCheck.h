#pragma once

// ─── Check for a newer editor, fetch it, verify it ────────────────────────────
// The whole of what this module does, in two blocking calls for a worker thread:
//
//   checkForUpdate     reads the remote manifest and compares its version with
//                      the running build's. Network only; changes nothing on disk
//                      beyond a scratch file it removes again.
//   downloadUpdate     fetches the artifact for THIS platform into the staging
//                      directory, checks size and SHA-256 against the manifest,
//                      and only then records it in pending.json.
//
// Neither installs anything and neither touches an open project: what happens
// to the staged file is the takeover step's business. pending.json is the single
// thing that step may trust — a file in staging without an entry there was never
// verified (an interrupted download, a leftover from an older run).
//
// Policy: an update is offered when the remote version is NEWER. A remote that
// is older is reported as such (a rolled-back release is worth knowing about)
// but never downloaded on its own; the same version is "up to date".
//
// What the hash does and does not give: it proves the file is the one the
// manifest named, byte for byte (corruption, a truncated transfer). Both come
// from the same server over the same SSH session, so it is INTEGRITY, not
// AUTHENTICITY — someone who controls the server, or sits between it and an
// editor that does not check the host key, can change manifest and file
// together. Closing that needs a signature checked against a key shipped in the
// editor, and pinning the host key; see docs/editor-selfupdate-sftp-design.md.

#include "SelfUpdate/SelfUpdateEndpoint.h"
#include "SelfUpdate/SelfUpdateManifest.h"
#include "SelfUpdate/SelfUpdateTransport.h"
#include "SelfUpdate/SuCommon.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace HE::Su {

// "windows-x64", "macos-arm64", "linux-x64" — the platform key this build asks
// the manifest for, spelled the way CI names its packages. Other combinations
// come out as "<os>-<arch>" ("linux-arm64"), "unknown" for what is not known, and
// simply find no artifact in the manifest.
HE_SU_API std::string currentPlatformKey();

struct LocalBuild
{
	std::string version;    // "0.7.0"
	std::string platform;   // currentPlatformKey()
};

// The running build: HE_VERSION_STRING from the generated HorizonVersion.h.
HE_SU_API LocalBuild runningBuild();

struct CheckOptions
{
	// Where the manifest is parked while it is read. Empty = the system temp
	// directory. Each check uses a file name of its own, so two editors on one
	// machine cannot trample each other.
	std::filesystem::path scratchDir;
	// A transient failure (SuResult::transient) is tried up to `attempts` times
	// in all, sleeping backoffMs, then 2 x backoffMs, ... between tries. Tests set
	// backoffMs to 0.
	int attempts  = 3;
	int backoffMs = 1000;
};

enum class CheckStatus
{
	NotConfigured,        // no usable self-update account (feature off, or `error` says why)
	UpToDate,             // remote version == running version
	UpdateAvailable,      // remote is newer and has a build for this platform
	RemoteOlder,          // remote is older than the running build; nothing to do
	NoBuildForPlatform,   // remote is newer but has no artifact for this platform
	Failed,               // could not read or understand the manifest — see `error`
};

struct CheckResult
{
	CheckStatus status = CheckStatus::Failed;
	std::string error;            // for Failed / NotConfigured / NoBuildForPlatform
	std::string localVersion;     // as given
	std::string remoteVersion;    // normalised, when the manifest was read
	std::string platform;         // the key that was looked up
	UpdateManifest manifest;      // valid whenever the manifest was read and parsed
	UpdateArtifact artifact;      // this platform's build; valid for UpdateAvailable
	// Where `artifact` lives on the server, relative to the account's base
	// directory: the manifest's directory + artifact.path. What downloadUpdate asks for.
	std::string artifactRemotePath;

	bool updateAvailable() const { return status == CheckStatus::UpdateAvailable; }
};

// The transport-level core: `manifestRemotePath` is where the manifest lives on
// the server ("packages/manifest.json").
HE_SU_API CheckResult checkForUpdate(IUpdateTransport& transport,
                                     const std::string& manifestRemotePath,
                                     const LocalBuild& local,
                                     const CheckOptions& options = {});

// The product entry point: builds the SFTP transport from the config (a
// NotSet/Invalid config returns NotConfigured without touching the network).
HE_SU_API CheckResult checkForUpdate(const SelfUpdateConfig& config,
                                     const LocalBuild& local = runningBuild(),
                                     const CheckOptions& options = {});

// ── Staging ───────────────────────────────────────────────────────────────────

// <per-user data dir>/SelfUpdate — next to the logs, never next to the
// executable (a signed macOS bundle or Program Files is not writable). Empty
// when the platform gives no home directory at all.
HE_SU_API std::filesystem::path defaultStagingDir();

struct StagedUpdate
{
	std::string   version;
	std::string   platform;
	std::filesystem::path filePath;   // the verified installer/archive
	std::string   sha256;
	std::uint64_t size = 0;
};

struct DownloadResult
{
	bool ok = false;
	std::string error;
	StagedUpdate staged;           // valid when ok
	bool alreadyStaged = false;    // the same build was staged and still verifies: nothing fetched
};

// Downloads and verifies `check.artifact` (check.updateAvailable() must hold)
// into `stagingRoot`, then records it in <stagingRoot>/pending.json and removes
// older staged builds. A failure — transport error, wrong size, wrong hash,
// not enough disk — leaves no partial file behind and does not touch an earlier
// pending.json. `progress` is called on the calling thread.
HE_SU_API DownloadResult downloadUpdate(IUpdateTransport& transport,
                                        const CheckResult& check,
                                        const std::filesystem::path& stagingRoot,
                                        const SuProgressFn& progress = {},
                                        const CheckOptions& options = {});

// Reads <stagingRoot>/pending.json. False with `error` set when there is none
// ("nothing staged" is reported as a plain false with an empty error), it does
// not parse, or its file path leaves the staging directory.
HE_SU_API bool readPendingUpdate(const std::filesystem::path& stagingRoot,
                                 StagedUpdate& out, std::string& error);

// Hashes the staged file again and compares it with the recorded SHA-256 and
// size. The takeover step calls this right before it acts, so a file that was
// swapped or damaged between download and install is caught.
HE_SU_API bool verifyStagedUpdate(const StagedUpdate& staged, std::string& error);

// Deletes everything under `stagingRoot` that this module put there.
HE_SU_API void discardStagedUpdates(const std::filesystem::path& stagingRoot);

} // namespace HE::Su
