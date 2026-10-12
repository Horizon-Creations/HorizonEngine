#pragma once

// ─── The remote update manifest ───────────────────────────────────────────────
// One small JSON file on the update server names the current build and, per
// platform, the installer file that belongs to it:
//
//   {
//     "schema":   1,
//     "version":  "0.7.1",
//     "codename": "Nimbus",                       (optional)
//     "released": "2026-10-20",                   (optional, free text)
//     "notes":    "what changed, one paragraph",  (optional)
//     "artifacts": [
//       { "platform": "windows-x64",
//         "path":     "releases/0.7.1/HorizonEditor-windows-x64.zip",
//         "sha256":   "<64 hex chars>",
//         "size":     123456789 },
//       { "platform": "macos-arm64", ... },
//       { "platform": "linux-x64",   ... }
//     ]
//   }
//
// `path` is relative to the directory the manifest itself lives in (the channel
// directory, "packages/" or "preview-packages/" on the update account), so a
// channel is self-contained: publishing a build is uploading its files into that
// directory and the manifest LAST. Platform keys are the ones CI already
// publishes its packages under (HorizonEditor-windows-x64, -macos-arm64,
// -linux-x64).
//
// Fields this version does not know are ignored, so the format can grow without
// breaking an editor that is already out in the field — and that is the whole
// reason `schema` exists: only an INCOMPATIBLE change bumps it, and an editor
// that meets a higher number says so instead of guessing. It cannot update
// itself past such a break, so a bump is a decision to be made rarely.
//
// Every string that later becomes a remote path or a local file name is held to
// a strict allow-list here (see isSafeRelativePath): the manifest comes off a
// network server, and "../" or a drive letter in it must never reach a join.

#include "SelfUpdate/SuCommon.h"

#include <cstdint>
#include <string>
#include <vector>

namespace HE::Su {

inline constexpr int kManifestSchema = 1;

struct UpdateArtifact
{
	std::string   platform;   // "windows-x64" | "macos-arm64" | "linux-x64" | …
	std::string   path;       // relative to the manifest's directory, '/'-separated
	std::string   sha256;     // 64 lowercase hex characters
	std::uint64_t size = 0;   // bytes; also the cheap early check before hashing

	// The part of `path` after the last '/', used as the file name in staging.
	std::string fileName() const;
};

struct UpdateManifest
{
	int         schema = kManifestSchema;
	std::string version;      // as written; parse it with parseVersion()
	std::string codename;
	std::string released;
	std::string notes;
	std::vector<UpdateArtifact> artifacts;

	// Null when the manifest has no build for that platform.
	const UpdateArtifact* findArtifact(const std::string& platform) const;
};

// Only [A-Za-z0-9._+-] and '/' between non-empty segments; no leading '/', no
// "." or ".." segment, nothing else — in particular no backslash, no ':' and no
// control character, so neither a Windows drive path nor a traversal survives.
HE_SU_API bool isSafeRelativePath(const std::string& path);

// Validates as it parses: a good schema, a version parseVersion() accepts, at
// least one artifact, every artifact with a known-shape platform, a safe path, a
// 64-hex sha256 (normalised to lowercase) and a non-zero size, no platform
// twice. On failure `error` says which rule broke and `out` is untouched.
HE_SU_API bool parseUpdateManifest(const std::string& json, UpdateManifest& out, std::string& error);

HE_SU_API std::string serializeUpdateManifest(const UpdateManifest& manifest);

} // namespace HE::Su
