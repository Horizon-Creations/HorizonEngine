#pragma once

// ─── HorizonNet — minimal HTTP/1.1 client ────────────────────────────────────
// One-shot request/response over TCP, sufficient for talking to a UPnP router on
// the LAN (device description GET + SOAP control POST).
//
// ⚠ PLAINTEXT ONLY — there is no TLS here, so this must never be pointed at a
// public endpoint. It exists for router calls, which are LAN-local and
// http:// by protocol definition. The session directory on the website is
// https:// and therefore needs a real TLS-capable client (see
// docs/networking-layer-design.md, N2.5b): the engine currently links only the
// crypto primitives (OpenSSL::Crypto / mbedcrypto), not a TLS stack.
//
// Calls block up to `timeoutMs`, so they belong on a worker thread — never on
// the editor frame loop.

#include "Net/NetCommon.h"

#include <cstdint>
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

} // namespace HE::Net
