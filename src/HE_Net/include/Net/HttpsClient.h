#pragma once

// ─── HorizonNet — TLS-capable HTTP client (platform-backed) ──────────────────
// Used to talk to the session directory on the website (https://). Unlike
// HttpClient — which is a hand-rolled plaintext client for LAN router SOAP —
// this one deliberately delegates to the operating system:
//
//   macOS/iOS   NSURLSession   (HttpsClient_Apple.mm)
//   Windows     WinHTTP        (HttpsClient_Win.cpp)
//   Linux       libcurl        (HttpsClient_Curl.cpp)
//
// The reason is certificate validation. Hostname matching, chain building,
// expiry, revocation, the system trust store, and corporate proxies are exactly
// where hand-written TLS goes wrong — and a mistake there is invisible until
// someone is actively attacked. Each platform's stack already solves it, follows
// OS policy updates, and honours the user's proxy configuration.
//
// Both http:// and https:// URLs are accepted (the platform stacks handle
// either), but callers talking to the directory must use https://.
//
// Every call blocks until the response arrives or the timeout expires, so these
// belong on a worker thread — never on the editor frame loop.

#include "Net/NetCommon.h"

#include <string>
#include <vector>

namespace HE::Net {

struct HttpsResponse {
    bool        ok         = false;   // transport-level success, not HTTP status
    int         statusCode = 0;
    std::string body;
    std::string error;                // populated when ok == false
};

// True when this build has a TLS backend compiled in. On Linux this is false if
// libcurl was not found at configure time; callers must surface that rather than
// silently failing to reach the directory.
HE_NET_API bool httpsAvailable();

// Name of the active backend ("NSURLSession", "WinHTTP", "libcurl", "none") —
// for diagnostics and the About/troubleshooting UI.
HE_NET_API const char* httpsBackendName();

// Perform a request. `extraHeaders` entries are "Name: value" without CRLF.
HE_NET_API HttpsResponse httpsRequest(const std::string& url,
                                      const std::string& method,
                                      const std::vector<std::string>& extraHeaders,
                                      const std::string& body,
                                      int timeoutMs = 10000);

inline HttpsResponse httpsGet(const std::string& url, int timeoutMs = 10000) {
    return httpsRequest(url, "GET", {}, {}, timeoutMs);
}

inline HttpsResponse httpsPostJson(const std::string& url, const std::string& json,
                                   int timeoutMs = 10000) {
    return httpsRequest(url, "POST", { "Content-Type: application/json" }, json, timeoutMs);
}

} // namespace HE::Net
