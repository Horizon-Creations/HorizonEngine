#include "Net/SessionDirectory.h"

#include "Net/HttpsClient.h"

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

    out.host            = stringField(j, "host");
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
    SessionRegistration& out, int timeoutMs) {

    if (!httpsAvailable()) return DirectoryStatus::NoTlsBackend;

    json payload{
        { "sessionId",       sessionId },
        { "port",            port },
        { "name",            name },
        { "engineVersion",   engineVersion },
        { "protocolVersion", protocolVersion },
    };

    const HttpsResponse resp = httpsPostJson(buildUrl(m_endpoint, "register"),
                                             payload.dump(), timeoutMs);
    if (!resp.ok) return DirectoryStatus::NetworkError;

    const DirectoryStatus http = statusFromHttp(resp.statusCode);
    if (http != DirectoryStatus::Ok) return http;

    return parseRegistration(resp.body, out);
}

DirectoryStatus SessionDirectory::lookup(const std::string& sessionId,
                                         SessionLookup& out, int timeoutMs) {
    if (!httpsAvailable()) return DirectoryStatus::NoTlsBackend;

    const std::string url = buildUrl(m_endpoint, "lookup") + "&sessionId=" + sessionId;
    const HttpsResponse resp = httpsGet(url, timeoutMs);
    if (!resp.ok) return DirectoryStatus::NetworkError;

    const DirectoryStatus http = statusFromHttp(resp.statusCode);
    if (http != DirectoryStatus::Ok) return http;

    return parseLookup(resp.body, out);
}

DirectoryStatus SessionDirectory::heartbeat(const std::string& sessionId,
                                            const std::string& token, int timeoutMs) {
    if (!httpsAvailable()) return DirectoryStatus::NoTlsBackend;

    const json payload{ { "sessionId", sessionId }, { "token", token } };
    const HttpsResponse resp = httpsPostJson(buildUrl(m_endpoint, "heartbeat"),
                                             payload.dump(), timeoutMs);
    if (!resp.ok) return DirectoryStatus::NetworkError;
    return statusFromHttp(resp.statusCode);
}

DirectoryStatus SessionDirectory::unregisterSession(const std::string& sessionId,
                                                    const std::string& token,
                                                    int timeoutMs) {
    if (!httpsAvailable()) return DirectoryStatus::NoTlsBackend;

    const json payload{ { "sessionId", sessionId }, { "token", token } };
    const HttpsResponse resp = httpsPostJson(buildUrl(m_endpoint, "unregister"),
                                             payload.dump(), timeoutMs);
    if (!resp.ok) return DirectoryStatus::NetworkError;
    return statusFromHttp(resp.statusCode);
}

} // namespace HE::Net
