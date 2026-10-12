#pragma once

// ─── The self-update SFTP account — a SECOND one, and a separate one ──────────
// HorizonContentSync reaches its server with one account, compiled in at build
// time (ContentSync/SftpCredentials.h). The self-update does NOT share it:
//
//   * What the account can reach differs. The EngineContent account is scoped to
//     the asset library; this one is scoped to the directory holding editor
//     builds. Reusing one account would force either of them to be wider than it
//     needs to be, and a leak of one would reach the other's files.
//   * What a leak costs differs. Whoever can write to the update directory can
//     hand every editor out there code to run. So this account should be
//     READ-ONLY for the editor; publishing a build uses yet another credential
//     that never leaves the release machine/CI.
//
// Hence the rules this header enforces:
//
//   1. The values come from the process ENVIRONMENT at run time, never from a
//      compile definition and never from a tracked file (this repository is
//      public). A packaged editor that should update by itself on a user's
//      machine needs a delivery decision for that — see the design doc.
//   2. Nothing here ever reads HE::Cs::engineContentEndpoint() as a SOURCE of
//      values. It is read only to refuse the case where the self-update variables
//      were pointed at the very same account as the EngineContent one.
//   3. The password is scrubbed out of every error text this module returns or
//      logs (HE::Cs::detail::scrub only knows the EngineContent password).
//
// Environment variables:
//   HE_SELFUPDATE_HOST       required. SFTP host name.
//   HE_SELFUPDATE_USER       required. SFTP user name.
//   HE_SELFUPDATE_PASSWORD   required. Password auth only (SftpClient's limit).
//   HE_SELFUPDATE_PORT       optional, 1-65535, default 22.
//   HE_SELFUPDATE_BASEPATH   optional. Directory the update tree lives in;
//                            empty = the account's own root (the chrooted case).
//   HE_SELFUPDATE_CHANNEL    optional. The directory under BASEPATH that holds one
//                            line of releases, [A-Za-z0-9._+-]: "packages" (the
//                            default, released builds) or "preview-packages". The
//                            manifest is <BASEPATH>/<CHANNEL>/manifest.json.

#include "SelfUpdate/SuCommon.h"

#include <ContentSync/SftpCredentials.h>

#include <functional>
#include <string>

namespace HE::Su {

inline constexpr const char* kEnvHost     = "HE_SELFUPDATE_HOST";
inline constexpr const char* kEnvUser     = "HE_SELFUPDATE_USER";
inline constexpr const char* kEnvPassword = "HE_SELFUPDATE_PASSWORD";
inline constexpr const char* kEnvPort     = "HE_SELFUPDATE_PORT";
inline constexpr const char* kEnvBasePath = "HE_SELFUPDATE_BASEPATH";
inline constexpr const char* kEnvChannel  = "HE_SELFUPDATE_CHANNEL";

// The two directories the update account has at its root.
inline constexpr const char* kDefaultChannel = "packages";
inline constexpr const char* kPreviewChannel = "preview-packages";

// Looks a variable up; null when it is unset. Defaults to std::getenv so the
// product code needs no argument, and a test can hand in a map instead of
// mutating the process environment.
using EnvLookup = std::function<const char*(const char*)>;

enum class ConfigState
{
	NotSet,    // none of the HE_SELFUPDATE_* variables is set: the feature is simply off
	Invalid,   // some are set but the result is unusable — `problem` says why
	Ready,     // `endpoint` is complete and not the EngineContent account
};

struct SelfUpdateConfig
{
	ConfigState          state = ConfigState::NotSet;
	HE::Cs::SftpEndpoint endpoint;
	std::string          channel = kDefaultChannel;
	std::string          problem;   // empty unless state == Invalid

	bool ready() const { return state == ConfigState::Ready; }

	// "<channel>/manifest.json" — relative to endpoint.remoteBasePath. Artifact
	// paths in it are relative to that file's directory, i.e. to "<channel>/".
	std::string manifestRemotePath() const;
};

// Reads HE_SELFUPDATE_* through `env` (std::getenv when empty).
HE_SU_API SelfUpdateConfig loadSelfUpdateConfig(const EnvLookup& env = {});

// True when both name the same login on the same server (host compared without
// regard to case) — the situation rule 2 above refuses. Two unconfigured
// endpoints are never "the same account".
HE_SU_API bool sameAccount(const HE::Cs::SftpEndpoint& a, const HE::Cs::SftpEndpoint& b);

// Removes every occurrence of `secret` from `text`. A no-op for an empty secret.
HE_SU_API std::string scrubSecret(std::string text, const std::string& secret);

} // namespace HE::Su
