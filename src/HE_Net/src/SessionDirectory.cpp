#include "Net/SessionDirectory.h"

#include "Net/HttpsClient.h"

#include "NetLog.h"

#include <Hpak/Aes256Gcm.h>

#include <nlohmann/json.hpp>

#include <random>

namespace HE::Net {
namespace {

using nlohmann::json;

// Fill with CSPRNG bytes, falling back to std::random_device when no crypto
// backend is present. A guessable session id would leak the host's IP.
void fillRandom(std::uint8_t* out, std::size_t n) {
    if (Hpak::randomBytes(out, n)) return;
    std::random_device rd;
    for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<std::uint8_t>(rd() & 0xFFu);
}

// Read a field that a server might send as either a JSON number or a string.
int intField(const json& j, const char* key, int fallback = 0) {
    if (!j.contains(key)) return fallback;
    const auto& v = j.at(key);
    if (v.is_number_integer()) return v.get<int>();
    if (v.is_string()) {
        try { return std::stoi(v.get<std::string>()); } catch (...) { return fallback; }
    }
    return fallback;
}

bool boolField(const json& j, const char* key, bool fallback = false) {
    if (!j.contains(key)) return fallback;
    const auto& v = j.at(key);
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number_integer()) return v.get<int>() != 0;
    return fallback;
}

std::string stringField(const json& j, const char* key) {
    if (!j.contains(key)) return {};
    const auto& v = j.at(key);
    return v.is_string() ? v.get<std::string>() : std::string{};
}

} // namespace

// ─── Pure helpers ────────────────────────────────────────────────────────────

std::string SessionDirectory::newSessionId() {
    // URL-safe alphabet: the id travels as a query parameter and is retyped by
    // hand, so avoid characters that need escaping or look alike.
    static constexpr char kAlphabet[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    std::uint8_t raw[20];
    fillRandom(raw, sizeof(raw));

    std::string out;
    out.reserve(sizeof(raw));
    for (const std::uint8_t b : raw) out.push_back(kAlphabet[b % 36]);
    return out;   // 20 chars, comfortably above the server's 16-char minimum
}

std::string SessionDirectory::buildUrl(const std::string& endpoint,
                                       const std::string& action) {
    const char sep = (endpoint.find('?') == std::string::npos) ? '?' : '&';
    return endpoint + sep + "action=" + action;
}

DirectoryStatus SessionDirectory::statusFromHttp(int httpStatus) {
    if (httpStatus == 200)              return DirectoryStatus::Ok;
    if (httpStatus == 404)              return DirectoryStatus::NotFound;
    if (httpStatus >= 400 && httpStatus < 500) return DirectoryStatus::Rejected;
    return DirectoryStatus::NetworkError;
}

DirectoryStatus SessionDirectory::parseRegistration(const std::string& body,
                                                    SessionRegistration& out) {
    const json j = json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return DirectoryStatus::MalformedResponse;
    if (!boolField(j, "ok"))                return DirectoryStatus::Rejected;

    out.token      = stringField(j, "token");
    out.publicIp   = stringField(j, "publicIp");
    out.port       = static_cast<std::uint16_t>(intField(j, "port"));
    out.reachable  = boolField(j, "reachable");
    out.ttlSeconds = intField(j, "ttl");
    // Only present when the server verified it; an unverified claim never comes
    // back, so this being non-empty already means "reachable on that family".
    out.altAddress = stringField(j, "altAddress");

    // Without a token the caller could never heartbeat or clean up, so an
    // entry it cannot manage is worse than a clear failure.
    if (out.token.empty()) return DirectoryStatus::MalformedResponse;
    return DirectoryStatus::Ok;
}

DirectoryStatus SessionDirectory::parseLookup(const std::string& body,
                                              SessionLookup& out) {
    const json j = json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return DirectoryStatus::MalformedResponse;
    if (!boolField(j, "ok"))                return DirectoryStatus::NotFound;

    // Prefer the list. A server that predates it still sends "host", so the
    // single value is the fallback rather than the other way round.
    out.hosts.clear();
    if (const auto it = j.find("hosts"); it != j.end() && it->is_array())
    {
        for (const auto& h : *it)
            if (h.is_string() && !h.get<std::string>().empty())
                out.hosts.push_back(h.get<std::string>());
    }
    out.host = stringField(j, "host");
    if (out.hosts.empty() && !out.host.empty()) out.hosts.push_back(out.host);
    if (out.host.empty() && !out.hosts.empty()) out.host = out.hosts.front();

    out.port            = static_cast<std::uint16_t>(intField(j, "port"));
    out.name            = stringField(j, "name");
    out.engineVersion   = stringField(j, "engineVersion");
    out.protocolVersion = intField(j, "protocolVersion");
    out.reachable       = boolField(j, "reachable");

    // An entry without a usable address is unusable; refuse it rather than
    // handing the caller a connect target of "":0.
    if (out.host.empty() || out.port == 0) return DirectoryStatus::MalformedResponse;
    return DirectoryStatus::Ok;
}

// ─── Requests ────────────────────────────────────────────────────────────────

DirectoryStatus SessionDirectory::registerSession(
    const std::string& sessionId, std::uint16_t port, const std::string& name,
    const std::string& engineVersion, int protocolVersion,
    SessionRegistration& out, const std::string& altAddress, int timeoutMs) {

    if (!httpsAvailable()) {
        HE_LOG_ERROR(Net, "Directory register skipped: this build has no TLS backend");
        return DirectoryStatus::NoTlsBackend;
    }

    // Session id abbreviated: it is shared deliberately, but it resolves to
    // the host address in the directory, and logs end up in bug reports.
    HE_LOG_INFO(Net, "Directory: registering session %s on port %u as \"%s\"",
                detail::logSessionId(sessionId).c_str(),
                static_cast<unsigned>(port), name.c_str());

    json payload{
        { "sessionId",       sessionId },
        { "port",            port },
        { "name",            name },
        { "engineVersion",   engineVersion },
        { "protocolVersion", protocolVersion },
    };
    // Offered, not asserted. The server refuses anything that is not globally
    // routable or not a different family, and then only publishes it if it can
    // connect back — so this cannot be used to point peers somewhere else.
    if (!altAddress.empty()) payload["altAddress"] = altAddress;

    const HttpsResponse resp = httpsPostJson(buildUrl(m_endpoint, "register"),
                                             payload.dump(), timeoutMs);
    if (!resp.ok) {
        HE_LOG_ERROR(Net, "Directory register failed to reach the endpoint: %s",
                     resp.error.empty() ? "no detail" : resp.error.c_str());
        return DirectoryStatus::NetworkError;
    }

    const DirectoryStatus http = statusFromHttp(resp.statusCode);
    if (http != DirectoryStatus::Ok) {
        // The body is NOT logged: it carries the management token on success
        // and can echo request data on failure.
        HE_LOG_ERROR(Net, "Directory register rejected with HTTP %d", resp.statusCode);
        return http;
    }

    const DirectoryStatus parsed = parseRegistration(resp.body, out);
    if (parsed != DirectoryStatus::Ok) {
        HE_LOG_ERROR(Net, "Directory register returned HTTP 200 but an unusable body");
        return parsed;
    }
    // reachable is the single most consequential field the directory returns:
    // it is the only outside-in verdict anywhere in the stack.
    HE_LOG_INFO(Net, "Directory: registered — seen from %s, reachable=%s, ttl %ds%s%s",
                out.publicIp.empty() ? "<unknown>" : out.publicIp.c_str(),
                out.reachable ? "yes" : "NO", out.ttlSeconds,
                out.altAddress.empty() ? "" : ", also reachable at ",
                out.altAddress.c_str());
    if (!out.reachable) {
        HE_LOG_WARN(Net, "The directory could not connect back to this host — peers on "
                         "other networks will not get through");
    }
    return parsed;
}

DirectoryStatus SessionDirectory::lookup(const std::string& sessionId,
                                         SessionLookup& out, int timeoutMs) {
    if (!httpsAvailable()) {
        HE_LOG_ERROR(Net, "Directory lookup skipped: this build has no TLS backend");
        return DirectoryStatus::NoTlsBackend;
    }

    HE_LOG_INFO(Net, "Directory: looking up session %s",
                detail::logSessionId(sessionId).c_str());

    const std::string url = buildUrl(m_endpoint, "lookup") + "&sessionId=" + sessionId;
    const HttpsResponse resp = httpsGet(url, timeoutMs);
    if (!resp.ok) {
        HE_LOG_ERROR(Net, "Directory lookup failed to reach the endpoint: %s",
                     resp.error.empty() ? "no detail" : resp.error.c_str());
        return DirectoryStatus::NetworkError;
    }

    const DirectoryStatus http = statusFromHttp(resp.statusCode);
    if (http != DirectoryStatus::Ok) {
        // 404 is the everyday case — a typo, or a session that already ended.
        if (http == DirectoryStatus::NotFound)
            HE_LOG_WARN(Net, "Directory: session %s not found (mistyped, or already over)",
                        detail::logSessionId(sessionId).c_str());
        else
            HE_LOG_ERROR(Net, "Directory lookup rejected with HTTP %d", resp.statusCode);
        return http;
    }

    const DirectoryStatus parsed = parseLookup(resp.body, out);
    if (parsed != DirectoryStatus::Ok) {
        HE_LOG_ERROR(Net, "Directory lookup returned an entry without a usable address");
        return parsed;
    }
    HE_LOG_INFO(Net, "Directory: session %s has %zu address(es), first %s:%u "
                     "(\"%s\", engine %s, protocol %d)",
                detail::logSessionId(sessionId).c_str(), out.hosts.size(),
                out.host.c_str(), static_cast<unsigned>(out.port), out.name.c_str(),
                out.engineVersion.c_str(), out.protocolVersion);
    return parsed;
}

DirectoryStatus SessionDirectory::heartbeat(const std::string& sessionId,
                                            const std::string& token, int timeoutMs) {
    if (!httpsAvailable()) return DirectoryStatus::NoTlsBackend;

    // The token authorises heartbeat and unregister for this session, so it
    // is never logged — not even truncated.
    const json payload{ { "sessionId", sessionId }, { "token", token } };
    const HttpsResponse resp = httpsPostJson(buildUrl(m_endpoint, "heartbeat"),
                                             payload.dump(), timeoutMs);
    if (!resp.ok) {
        HE_LOG_WARN(Net, "Directory heartbeat for %s could not reach the endpoint",
                    detail::logSessionId(sessionId).c_str());
        return DirectoryStatus::NetworkError;
    }
    const DirectoryStatus st = statusFromHttp(resp.statusCode);
    if (st != DirectoryStatus::Ok) {
        // Left unfixed this ends with the entry expiring and new guests being
        // unable to find a session that is still running.
        HE_LOG_WARN(Net, "Directory heartbeat for %s rejected with HTTP %d — the entry "
                         "will expire if this keeps failing",
                    detail::logSessionId(sessionId).c_str(), resp.statusCode);
    } else {
        HE_LOG_DEBUG(Net, "Directory heartbeat for %s acknowledged",
                     detail::logSessionId(sessionId).c_str());
    }
    return st;
}

DirectoryStatus SessionDirectory::unregisterSession(const std::string& sessionId,
                                                    const std::string& token,
                                                    int timeoutMs) {
    if (!httpsAvailable()) return DirectoryStatus::NoTlsBackend;

    const json payload{ { "sessionId", sessionId }, { "token", token } };
    const HttpsResponse resp = httpsPostJson(buildUrl(m_endpoint, "unregister"),
                                             payload.dump(), timeoutMs);
    if (!resp.ok) {
        HE_LOG_WARN(Net, "Directory unregister for %s could not reach the endpoint — the "
                         "entry will linger until its TTL expires",
                    detail::logSessionId(sessionId).c_str());
        return DirectoryStatus::NetworkError;
    }
    const DirectoryStatus st = statusFromHttp(resp.statusCode);
    HE_LOG_INFO(Net, "Directory: unregistered session %s (HTTP %d)",
                detail::logSessionId(sessionId).c_str(), resp.statusCode);
    return st;
}

} // namespace HE::Net
