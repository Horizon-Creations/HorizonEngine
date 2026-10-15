#pragma once

// ─── The self-update SFTP account — a SECOND one, and a separate one ──────────
// HorizonContentSync reaches its server with one account, compiled in at build
// time (ContentSync/SftpCredentials.h). The self-update has its own, compiled in
// the same way, and does NOT share that one:
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
//   1. The values come from exactly two places, never from a tracked file (this
//      repository is public):
//        a. the process ENVIRONMENT at run time — when any of HE_SELFUPDATE_HOST,
//           _USER, _PASSWORD, _PORT or _BASEPATH is set, the environment IS the
//           account, and nothing of the compiled-in one is mixed in (a password
//           baked into the binary must never be sent to a host someone named in
//           the environment);
//        b. otherwise the account COMPILED IN at build time
//           (builtInSelfUpdateEndpoint(): cmake/SelfUpdateCredentials.cmake or
//           CI secrets, see src/HE_SelfUpdate/CMakeLists.txt). This is what an
//           installed editor uses, so it has to be a READ-ONLY login.
//      HE_SELFUPDATE_CHANNEL is read from the environment in both cases.
//   2. Nothing here ever reads HE::Cs::engineContentEndpoint() as a SOURCE of
//      values. It is read only to refuse the case where the self-update account
//      (from either place) is the very same login as the EngineContent one.
//   3. The password is scrubbed out of every error text this module returns or
//      logs (HE::Cs::detail::scrub only knows the EngineContent password).
//
// Environment variables (also the CMake names of the compiled-in account):
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
	NotSet,    // no account compiled in and none of HE_SELFUPDATE_* set: the feature is simply off
	Invalid,   // an account is there but unusable — `problem` says why
	Ready,     // `endpoint` is complete and not the EngineContent account
};

// Where the account in a SelfUpdateConfig came from (rule 1).
enum class ConfigSource
{
	None,          // NotSet
	Environment,   // HE_SELFUPDATE_* at run time
	BuiltIn,       // compiled in at build time
};

struct HE_SU_API SelfUpdateConfig   // manifestRemotePath() lives in the .cpp, so the type is exported
{
	ConfigState          state = ConfigState::NotSet;
	ConfigSource         source = ConfigSource::None;
	HE::Cs::SftpEndpoint endpoint;
	std::string          channel = kDefaultChannel;
	std::string          problem;   // empty unless state == Invalid

	bool ready() const { return state == ConfigState::Ready; }

	// "<channel>/manifest.json" — relative to endpoint.remoteBasePath. Artifact
	// paths in it are relative to that file's directory, i.e. to "<channel>/".
	std::string manifestRemotePath() const;
};

// The account compiled into this build; host/user/password empty when none was
// supplied at configure time. Read only through loadSelfUpdateConfig().
HE_SU_API const HE::Cs::SftpEndpoint& builtInSelfUpdateEndpoint();

// Rule 1: HE_SELFUPDATE_* through `env` (std::getenv when empty) when any account
// variable is set there, otherwise builtInSelfUpdateEndpoint().
HE_SU_API SelfUpdateConfig loadSelfUpdateConfig(const EnvLookup& env = {});

// The same with the compiled-in account handed in — a test passes an empty one
// so that its result does not depend on what the build it runs in carries.
HE_SU_API SelfUpdateConfig loadSelfUpdateConfig(const EnvLookup& env, const HE::Cs::SftpEndpoint& builtIn);

// True when both name the same login on the same server (host compared without
// regard to case) — the situation rule 2 above refuses. Two unconfigured
// endpoints are never "the same account".
HE_SU_API bool sameAccount(const HE::Cs::SftpEndpoint& a, const HE::Cs::SftpEndpoint& b);

// Removes every occurrence of `secret` from `text`. A no-op for an empty secret.
HE_SU_API std::string scrubSecret(std::string text, const std::string& secret);

} // namespace HE::Su
