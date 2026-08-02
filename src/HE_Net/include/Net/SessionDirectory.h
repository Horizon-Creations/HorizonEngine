#pragma once

// ─── HorizonNet — collaboration session directory client ─────────────────────
// Talks to session-api.php so a host can publish where it can be reached and a
// peer can look that up by session id. This is address discovery only — no
// session traffic passes through the directory.
//
// Division of responsibility, deliberately:
//   • the directory learns the host's address, port and a display name,
//   • it NEVER learns the join secret — authentication is engine-to-engine
//     (SecureTransport), so a compromised directory cannot join a session,
//   • the server records the address from REMOTE_ADDR, so a client cannot
//     redirect peers elsewhere by lying in the request body.
//
// register() returns a management token; heartbeat() and unregister() require
// it, so merely knowing a session id is not enough to drop or hijack an entry.
//
// Session ids are public in the sense that anyone holding one learns the host's
// IP — so generate them with newSessionId() (high entropy, short-lived), never
// something guessable like a project name.
//
// All calls block on the network and belong on a worker thread.

#include "Net/NetCommon.h"

#include <cstdint>
#include <string>

namespace HE::Net {

enum class DirectoryStatus : std::uint8_t {
    Ok,
    NoTlsBackend,      // build has no HTTPS support (see httpsAvailable())
    NetworkError,      // could not reach the directory at all
    NotFound,          // no such session (or it expired)
    Rejected,          // server refused: id taken, quota, bad token
    MalformedResponse, // reached the server but could not parse the reply
};

struct SessionRegistration {
    std::string   token;              // required for heartbeat/unregister
    std::string   publicIp;           // as observed by the server
    std::uint16_t port      = 0;
    // False means the server could NOT connect back. Publishing anyway leaves
    // peers in an unexplained timeout, so the UI must act on this: fall back to
    // manual port forwarding, or tell the user a relay is required (CGNAT).
    bool          reachable = false;
    int           ttlSeconds = 0;     // heartbeat well before this elapses
};

struct SessionLookup {
    std::string   host;
    std::uint16_t port = 0;
    std::string   name;
    std::string   engineVersion;
    int           protocolVersion = 0;
    bool          reachable = false;
};

class HE_NET_API SessionDirectory {
public:
    // Base URL of the endpoint, e.g. "https://horizoncreations.dev/HorizonEngine/session-api.php".
    explicit SessionDirectory(std::string endpointUrl)
        : m_endpoint(std::move(endpointUrl)) {}

    // A fresh, unguessable session id (~128 bits, URL-safe).
    static std::string newSessionId();

    DirectoryStatus registerSession(const std::string& sessionId,
                                    std::uint16_t port,
                                    const std::string& name,
                                    const std::string& engineVersion,
                                    int protocolVersion,
                                    SessionRegistration& out,
                                    int timeoutMs = 10000);

    DirectoryStatus lookup(const std::string& sessionId, SessionLookup& out,
                           int timeoutMs = 10000);

    DirectoryStatus heartbeat(const std::string& sessionId, const std::string& token,
                              int timeoutMs = 10000);

    DirectoryStatus unregisterSession(const std::string& sessionId,
                                      const std::string& token,
                                      int timeoutMs = 10000);

    const std::string& endpoint() const { return m_endpoint; }

    // ── Pure helpers, exposed for testing without a live server ──
    static std::string     buildUrl(const std::string& endpoint, const std::string& action);
    static DirectoryStatus parseRegistration(const std::string& json, SessionRegistration& out);
    static DirectoryStatus parseLookup(const std::string& json, SessionLookup& out);
    // Maps an HTTP status onto a DirectoryStatus (404 → NotFound, 4xx → Rejected, …).
    static DirectoryStatus statusFromHttp(int httpStatus);

private:
    std::string m_endpoint;
};

} // namespace HE::Net
