#include "Net/HttpClient.h"

#include "NetLog.h"
#include "Net/Socket.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <thread>

namespace HE::Net {
namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Case-insensitive header lookup within an already-isolated header block.
std::string findHeader(const std::string& headers, const std::string& name) {
    const std::string lowerHeaders = toLower(headers);
    const std::string lowerName    = toLower(name) + ":";

    std::size_t pos = 0;
    while (pos < lowerHeaders.size()) {
        const std::size_t lineEnd = lowerHeaders.find("\r\n", pos);
        const std::size_t end = (lineEnd == std::string::npos) ? lowerHeaders.size() : lineEnd;
        if (lowerHeaders.compare(pos, lowerName.size(), lowerName) == 0) {
            std::string value = headers.substr(pos + lowerName.size(),
                                               end - pos - lowerName.size());
            // Trim surrounding whitespace.
            const std::size_t b = value.find_first_not_of(" \t");
            const std::size_t e = value.find_last_not_of(" \t\r");
            if (b == std::string::npos) return {};
            return value.substr(b, e - b + 1);
        }
        if (lineEnd == std::string::npos) break;
        pos = lineEnd + 2;
    }
    return {};
}

// Chunked bodies: [hex length]\r\n[data]\r\n… terminated by a zero-length chunk.
std::string decodeChunked(const std::string& body) {
    std::string out;
    std::size_t pos = 0;
    while (pos < body.size()) {
        const std::size_t lineEnd = body.find("\r\n", pos);
        if (lineEnd == std::string::npos) break;

        const std::string lenStr = body.substr(pos, lineEnd - pos);
        const auto len = static_cast<std::size_t>(std::strtoul(lenStr.c_str(), nullptr, 16));
        if (len == 0) break;                       // terminating chunk

        const std::size_t dataStart = lineEnd + 2;
        if (dataStart + len > body.size()) break;  // truncated
        out.append(body, dataStart, len);
        pos = dataStart + len + 2;                 // skip the chunk's trailing CRLF
    }
    return out;
}

} // namespace

// ─── URL ─────────────────────────────────────────────────────────────────────

bool httpParseUrl(const std::string& url, HttpUrl& out) {
    constexpr const char* kScheme = "http://";
    constexpr std::size_t kSchemeLen = 7;

    // Reject https:// explicitly rather than stripping the scheme and sending
    // plaintext to a TLS port — a silent downgrade would be worse than failing.
    if (toLower(url).compare(0, kSchemeLen, kScheme) != 0) return false;

    const std::string rest = url.substr(kSchemeLen);
    if (rest.empty()) return false;

    const std::size_t slash = rest.find('/');
    std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    out.path = (slash == std::string::npos) ? "/" : rest.substr(slash);
    if (authority.empty()) return false;

    const std::size_t colon = authority.find(':');
    if (colon == std::string::npos) {
        out.host = authority;
        out.port = 80;
    } else {
        out.host = authority.substr(0, colon);
        const long p = std::strtol(authority.c_str() + colon + 1, nullptr, 10);
        if (p <= 0 || p > 65535) return false;
        out.port = static_cast<std::uint16_t>(p);
    }
    return !out.host.empty();
}

// ─── Response ────────────────────────────────────────────────────────────────

bool httpParseResponse(const std::string& raw, HttpResponse& out) {
    const std::size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos) return false;

    const std::string head = raw.substr(0, headerEnd);
    std::string       body = raw.substr(headerEnd + 4);

    // Status line: "HTTP/1.1 200 OK"
    const std::size_t sp1 = head.find(' ');
    if (sp1 == std::string::npos) return false;
    out.statusCode = static_cast<int>(std::strtol(head.c_str() + sp1 + 1, nullptr, 10));
    if (out.statusCode == 0) return false;

    const std::size_t firstLineEnd = head.find("\r\n");
    const std::string headers =
        (firstLineEnd == std::string::npos) ? std::string{} : head.substr(firstLineEnd + 2);

    if (toLower(findHeader(headers, "Transfer-Encoding")).find("chunked") != std::string::npos) {
        body = decodeChunked(body);
    } else {
        const std::string cl = findHeader(headers, "Content-Length");
        if (!cl.empty()) {
            const auto want = static_cast<std::size_t>(std::strtoul(cl.c_str(), nullptr, 10));
            if (body.size() > want) body.resize(want);
        }
    }

    out.body = std::move(body);
    out.ok   = true;
    return true;
}

// ─── Request ─────────────────────────────────────────────────────────────────

HttpResponse httpRequest(const std::string& url, const std::string& method,
                         const std::vector<std::string>& extraHeaders,
                         const std::string& body, int timeoutMs) {
    HttpResponse resp;

    HttpUrl parsed;
    if (!httpParseUrl(url, parsed)) {
        // Includes the deliberate https:// refusal — this client has no TLS and
        // must never quietly send plaintext to a TLS port.
        HE_LOG_ERROR(Net, "HTTP: refusing url \"%s\" (not a plain http:// address)",
                     url.c_str());
        resp.error = "invalid or non-http url";
        return resp;
    }
    HE_LOG_TRACE(Net, "HTTP: %s %s%s", method.c_str(), parsed.host.c_str(),
                 parsed.path.c_str());

    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeoutMs);

    // Resolve first, then create a socket of the matching family: routers on a
    // v6-capable LAN may well answer on an IPv6 address.
    SocketHandle sock = kInvalidSocket;
    SocketResult rc = socketCreateTcpConnecting(parsed.host, parsed.port, sock);
    if (rc == SocketResult::Error || sock == kInvalidSocket) {
        HE_LOG_WARN(Net, "HTTP: cannot connect to %s:%u", parsed.host.c_str(),
                    static_cast<unsigned>(parsed.port));
        if (sock != kInvalidSocket) socketClose(sock);
        resp.error = "connect failed";
        return resp;
    }
    while (rc == SocketResult::WouldBlock) {
        if (std::chrono::steady_clock::now() >= deadline) {
            HE_LOG_WARN(Net, "HTTP: connect to %s:%u timed out after %d ms",
                        parsed.host.c_str(), static_cast<unsigned>(parsed.port), timeoutMs);
            socketClose(sock);
            resp.error = "connect timeout";
            return resp;
        }
        rc = socketConnectPoll(sock);
        if (rc == SocketResult::Error) {
            socketClose(sock);
            resp.error = "connect failed";
            return resp;
        }
        if (rc == SocketResult::WouldBlock) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    // Build the request. Connection: close lets the server signal the end of the
    // body by closing, which keeps the read loop simple.
    std::string req = method + " " + parsed.path + " HTTP/1.1\r\n";
    req += "Host: " + parsed.host + ":" + std::to_string(parsed.port) + "\r\n";
    req += "Connection: close\r\n";
    for (const auto& h : extraHeaders) req += h + "\r\n";
    if (!body.empty()) {
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "\r\n";
    req += body;

    std::size_t sentTotal = 0;
    while (sentTotal < req.size()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            socketClose(sock);
            resp.error = "send timeout";
            return resp;
        }
        std::size_t sent = 0;
        const SocketResult sr = socketSend(
            sock, reinterpret_cast<const std::uint8_t*>(req.data()) + sentTotal,
            req.size() - sentTotal, sent);
        if (sr == SocketResult::Ok)          { sentTotal += sent; continue; }
        if (sr == SocketResult::WouldBlock)  { std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; }
        socketClose(sock);
        resp.error = "send failed";
        return resp;
    }

    // Read until the peer closes or the deadline passes.
    std::string raw;
    std::uint8_t buf[8192];
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;

        const auto remainingMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        if (!socketWaitReadable(sock, std::min(remainingMs, 200))) continue;

        std::size_t got = 0;
        const SocketResult rr = socketRecv(sock, buf, sizeof(buf), got);
        if (rr == SocketResult::Ok)         { raw.append(reinterpret_cast<char*>(buf), got); continue; }
        if (rr == SocketResult::WouldBlock) continue;
        break;   // Closed (normal end of body) or Error
    }
    socketClose(sock);

    if (raw.empty()) {
        HE_LOG_WARN(Net, "HTTP: %s:%u closed without sending anything",
                    parsed.host.c_str(), static_cast<unsigned>(parsed.port));
        resp.error = "empty response";
        return resp;
    }
    if (!httpParseResponse(raw, resp)) {
        resp.error = "malformed response";
        resp.ok = false;
        return resp;
    }
    return resp;
}

} // namespace HE::Net
