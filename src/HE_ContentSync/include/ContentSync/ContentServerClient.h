#pragma once

// ─── The new HTTP EngineContent server (Thema 194) ────────────────────────────
// A purpose-built C++ server (github.com/Horizon-Creations/horizon-content-server) that answers
// GET /manifest.json and GET /content/<path> over plain HTTP, replacing the content-DELIVERY
// half of what EngineContentSync used to get exclusively from SFTP. SFTP (SftpClient.h) stays
// the fallback: whenever this is not configured, EngineContentSync behaves exactly as before
// this topic, and publishing content still goes through SFTP either way — this server is
// read-only, it was never meant to accept uploads.
//
// Configuration is a RUNTIME setting (config.json's "EngineContentServerUrl", e.g.
// "http://127.0.0.1:18080"), not a compile-time credential like SftpCredentials.h: the server
// has no login, so there is nothing here worth keeping out of a tracked file, and pointing an
// install at a different host (a local test server, later a production one) does not need a
// rebuild to do it.

#include "ContentSync/CsCommon.h"
#include "ContentSync/SftpClient.h"   // SftpResult, SftpProgressFn — reused, not reinvented

#include <cstdint>
#include <string>

namespace HE::Cs {

struct ContentServerEndpoint
{
	std::string   host;        // empty = not configured, EngineContentSync falls back to SFTP
	std::uint16_t port = 0;

	bool configured() const { return !host.empty(); }
};

namespace detail {
// Parses config.json's "EngineContentServerUrl" value into host/port (http:// only, same as
// HE::Net::httpParseUrl — this delegates to it). Pure, no I/O: factored out of
// contentServerEndpoint() below purely so a test can exercise the parsing without racing that
// function's once-per-process cache (see there).
HE_CS_API bool parseContentServerUrl(const std::string& url, ContentServerEndpoint& out);
} // namespace detail

// The single source of truth for the content-server endpoint. Read from config.json once per
// process — same caching as engineContentEndpoint() for the SFTP side — so a config.json edit
// needs a restart to take effect, like every other setting read through GlobalState's custom
// config.
HE_CS_API const ContentServerEndpoint& contentServerEndpoint();

// Test-only: replaces the cached value contentServerEndpoint() returns for the rest of this
// process, bypassing config.json entirely — lets a test point at a loopback test server without
// writing a config file. Not for production code.
HE_CS_API void setContentServerEndpointForTesting(const ContentServerEndpoint& endpoint);

// GET http://host:port/manifest.json into `localPath`. Matches
// EngineContentSync::ManifestFetchFn's contract exactly, so it can be handed straight to
// refreshManifestWith (see EngineContentSync.cpp) the same way the SFTP fetch is.
HE_CS_API SftpResult contentServerGetManifest(const ContentServerEndpoint& endpoint,
                                               const std::string& localPath);

// GET http://host:port/content/<remoteRelPath> (percent-encoded) to `localPath`, via a
// temp-then-rename so a failed/killed transfer never leaves a partial file where a caller might
// mistake it for a complete one — the same contract as sftpGetFile. `onProgress` is optional.
HE_CS_API SftpResult contentServerGetFile(const ContentServerEndpoint& endpoint,
                                           const std::string&          remoteRelPath,
                                           const std::string&          localPath,
                                           const SftpProgressFn&       onProgress = {});

} // namespace HE::Cs
