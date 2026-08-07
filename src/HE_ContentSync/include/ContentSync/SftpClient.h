#pragma once

// ─── SFTP file transfer for the EngineContent sync ────────────────────────────
// Thin wrapper around libssh2's SFTP subsystem. Every function here opens its
// own TCP connection + SSH session + SFTP channel and tears the whole thing
// down before returning — there is no client object holding a live connection
// across calls. Same principle as HpakReader being reopened per async job: it
// makes every one of these safe to call from any worker thread without a lock,
// at the cost of a fresh handshake per call (fine — these run on background
// threads for one-file-at-a-time downloads, never in a hot loop).
//
// Password auth only, because that is what the configured account uses (see
// SftpCredentials.h) — that is also why this links libssh2 directly instead of
// shelling out to the OpenSSH `sftp` CLI the way HorizonSourceControl shells
// out to `git`: batch-mode `sftp` cannot answer a password prompt
// non-interactively.

#include "ContentSync/CsCommon.h"
#include "ContentSync/SftpCredentials.h"

#include <cstdint>
#include <string>

namespace HE::Cs {

struct SftpResult
{
	bool        ok = false;
	std::string error;   // human-readable, already scrubbed of the password — empty when ok
};

// Connect, authenticate, disconnect. Used for the startup connectivity probe
// and the "Test Connection" affordance — never touches the filesystem.
HE_CS_API SftpResult sftpTestConnection(const SftpEndpoint& endpoint);

// Downloads `remoteRelPath` (resolved against endpoint.remoteBasePath) to
// `localPath`, creating localPath's parent directories if needed. Writes to a
// temporary file next to localPath and renames on success, so a failed/killed
// transfer never leaves a partial file where a caller might mistake it for a
// complete one.
HE_CS_API SftpResult sftpGetFile(const SftpEndpoint& endpoint,
                                  const std::string&  remoteRelPath,
                                  const std::string&  localPath);

// Uploads `localPath` to `remoteRelPath` (resolved against endpoint.remoteBasePath).
// Does NOT create missing remote parent directories — call sftpEnsureRemoteDir
// first (sftp has no mkdir -p equivalent; each level must be created in order).
HE_CS_API SftpResult sftpPutFile(const SftpEndpoint& endpoint,
                                  const std::string&  localPath,
                                  const std::string&  remoteRelPath);

// Creates every missing directory along remoteRelDir's path, one level at a
// time (resolved against endpoint.remoteBasePath). An already-existing
// directory at any level is not an error.
HE_CS_API SftpResult sftpEnsureRemoteDir(const SftpEndpoint& endpoint,
                                          const std::string&  remoteRelDir);

} // namespace HE::Cs
