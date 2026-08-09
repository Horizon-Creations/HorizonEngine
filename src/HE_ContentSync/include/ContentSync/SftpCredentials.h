#pragma once

#include "ContentSync/CsCommon.h"

#include <cstdint>
#include <string>

// ─── EngineContent SFTP endpoint — the ONE place these live ───────────────────
// Deliberate, explicit product decision (not an oversight): the EngineContent
// sync account's credentials are baked into the Editor binary rather than
// stored via an OS keychain, a credential-helper, or a settings dialog. The
// account is dedicated to this one purpose (fetching/publishing the shared
// EngineContent asset library), so the blast radius of the value leaking out of
// a built Editor binary (it is trivially recoverable — a plain string in the
// binary) is the EngineContent host, not any of the user's other systems.
//
// They are NOT in the source, though: this repository is public, and a value in
// a tracked file is greppable forever, in every clone and every fork, whether
// or not it is later rotated. They arrive as compile definitions instead —
// src/HE_ContentSync/CMakeLists.txt documents the three ways in (a -D on the
// configure line, which is how CI passes its repository secrets; a gitignored
// cmake/EngineContentCredentials.cmake for a local build; or nothing, which
// leaves the feature switched off).
//
// Practical mitigation that costs no code: keep this SFTP account restricted
// (chrooted/scoped, if the host supports it) to only the EngineContent publish
// path — never a general-purpose account.
//
// Rotation: change the secret at its source (repository secret / local cmake
// file) and rebuild. Nothing else in the engine reads these except SftpClient —
// there is no second copy anywhere to forget.
//
// A build with no credentials supplied is a normal, supported state: host and
// username come out empty and SftpProbe reports "not configured" rather than
// trying to reach a placeholder.
namespace HE::Cs {

struct SftpEndpoint
{
	std::string host;              // e.g. "example.com" — empty means "not configured"
	std::uint16_t port = 22;
	std::string username;
	std::string password;
	// Remote directory that plays the same role as the local EngineContent root:
	// manifest.json and the familiar EngineContent folder structure (Meshes/,
	// Materials/, …) live directly under this path. Empty string = the SFTP
	// account's own home/root directory.
	std::string remoteBasePath;

	bool configured() const { return !host.empty() && !username.empty(); }
};

// The single source of truth for the EngineContent SFTP endpoint. Returned by
// value (this is a handful of short strings, called rarely — at startup and at
// the start of each sync/publish operation, never per-frame).
HE_CS_API const SftpEndpoint& engineContentEndpoint();

} // namespace HE::Cs
