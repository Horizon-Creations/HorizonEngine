#pragma once

// ─── HorizonNet — minimal HTTP/1.1 client ────────────────────────────────────
// One-shot request/response over TCP, sufficient for talking to a UPnP router on
// the LAN (device description GET + SOAP control POST).
//
// ⚠ PLAINTEXT ONLY — there is no TLS here. It exists for router calls, which
// are LAN-local and http:// by protocol definition. The session directory on
// the website is https:// and therefore needs a real TLS-capable client (see
// docs/networking-layer-design.md, N2.5b): the engine currently links only the
// crypto primitives (OpenSSL::Crypto / mbedcrypto), not a TLS stack.
//
// Second user (Thema 194): HE_ContentSync's new EngineContent content-server
// client (ContentSync/ContentServerClient.h) — that server is also plaintext
// HTTP today (see the server repo, horizon-content-server), so this is not a
// downgrade on the client's part. Unlike the router, this one IS meant to be
// pointed at a non-LAN host eventually (a rented server) once this module's own
// manifest/content payloads carry anything worth protecting in transit; tracked
// there, not here.
//
// Calls block up to `timeoutMs`, so they belong on a worker thread — never on
// the editor frame loop.

#include "Net/NetCommon.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace HE::Net {

struct HttpResponse {
    bool        ok         = false;   // transport-level success (not HTTP status)
    int         statusCode = 0;
    std::string body;
    std::string error;                // populated when ok == false
};

// A URL split into its parts. Only http:// is accepted.
struct HttpUrl {
    std::string   host;
    std::uint16_t port = 80;
    std::string   path = "/";
};

// Parse "http://host[:port][/path]". Returns false for anything else — notably
// https://, which this client cannot serve and must not silently downgrade.
HE_NET_API bool httpParseUrl(const std::string& url, HttpUrl& out);

// Split a raw HTTP response into status code and body. Handles the status line,
// header block, and both Content-Length and chunked transfer encoding.
HE_NET_API bool httpParseResponse(const std::string& raw, HttpResponse& out);

// Perform a request. `extraHeaders` entries must not include trailing CRLF.
HE_NET_API HttpResponse httpRequest(const std::string& url,
                                    const std::string& method,
                                    const std::vector<std::string>& extraHeaders,
                                    const std::string& body,
                                    int timeoutMs = 5000);

inline HttpResponse httpGet(const std::string& url, int timeoutMs = 5000) {
    return httpRequest(url, "GET", {}, {}, timeoutMs);
}

// Progress for httpDownloadToFile: (bytesDone, bytesTotal). Called from the read loop on the
// calling thread, so it must be cheap and must not block.
using HttpProgressFn = std::function<void(std::uint64_t bytesDone, std::uint64_t bytesTotal)>;

// GET `url`, streaming the response body straight to `destPath` instead of buffering it in a
// std::string first — httpRequest/httpGet above hold the whole response in HttpResponse::body,
// which is fine for the small UPnP replies they exist for but would copy an entire file through
// RAM for a large download. Requires the response to carry Content-Length (every route this is
// used against does) and verifies the byte count on completion: a connection that closes early
// deletes whatever it had written to `destPath` and fails, rather than leaving a silently short
// file behind.
//
// `idleTimeoutMs` resets on every chunk received rather than counting down from the start of the
// call — a large file over a slow connection legitimately takes longer than any single fixed
// deadline; a connection that goes quiet for that long is what this treats as stalled.
// HttpResponse::body is left empty; use statusCode/ok/error the same way httpRequest's callers do.
HE_NET_API HttpResponse httpDownloadToFile(const std::string& url,
                                           const std::string& destPath,
                                           const HttpProgressFn& onProgress = {},
                                           int idleTimeoutMs = 10000);

} // namespace HE::Net
