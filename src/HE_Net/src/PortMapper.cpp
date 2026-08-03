#include "Net/PortMapper.h"

#include "NetLog.h"

#include "Net/HttpClient.h"
#include "Net/Socket.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace HE::Net {
namespace {

constexpr const char* kSsdpAddr = "239.255.255.250";
constexpr std::uint16_t kSsdpPort = 1900;

// Searched in priority order: an IGD may expose either, and v2 devices still
// answer the v1 service types.
const char* const kWanServices[] = {
    "urn:schemas-upnp-org:service:WANIPConnection:1",
    "urn:schemas-upnp-org:service:WANPPPConnection:1",
    "urn:schemas-upnp-org:service:WANIPConnection:2",
};

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s) {
    const std::size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const std::size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// XML escaping for values we place into SOAP bodies (descriptions are
// user-supplied, so this is not optional).
std::string xmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        switch (c) {
        case '&':  out += "&amp;";  break;
        case '<':  out += "&lt;";   break;
        case '>':  out += "&gt;";   break;
        case '"':  out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default:   out += c;        break;
        }
    }
    return out;
}

} // namespace

// ─── Pure helpers ────────────────────────────────────────────────────────────

std::string PortMapper::buildSsdpSearch() {
    // MX is the maximum random delay a device may wait before answering; ST asks
    // specifically for gateways so unrelated UPnP devices stay quiet.
    return "M-SEARCH * HTTP/1.1\r\n"
           "HOST: 239.255.255.250:1900\r\n"
           "MAN: \"ssdp:discover\"\r\n"
           "MX: 2\r\n"
           "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"
           "\r\n";
}

std::string PortMapper::parseSsdpLocation(const std::string& ssdpResponse) {
    const std::string lower = toLower(ssdpResponse);
    std::size_t pos = 0;
    while (pos < lower.size()) {
        const std::size_t lineEnd = lower.find("\r\n", pos);
        const std::size_t end = (lineEnd == std::string::npos) ? lower.size() : lineEnd;
        if (lower.compare(pos, 9, "location:") == 0) {
            return trim(ssdpResponse.substr(pos + 9, end - pos - 9));
        }
        if (lineEnd == std::string::npos) break;
        pos = lineEnd + 2;
    }
    return {};
}

std::string PortMapper::extractXmlValue(const std::string& xml, const std::string& tag) {
    const std::string open  = "<" + tag + ">";
    const std::string close = "</" + tag + ">";
    const std::size_t b = xml.find(open);
    if (b == std::string::npos) return {};
    const std::size_t valueStart = b + open.size();
    const std::size_t e = xml.find(close, valueStart);
    if (e == std::string::npos) return {};
    return trim(xml.substr(valueStart, e - valueStart));
}

std::string PortMapper::resolveUrl(const std::string& baseUrl, const std::string& relative) {
    if (relative.empty()) return {};
    // Already absolute.
    if (toLower(relative).compare(0, 7, "http://") == 0) return relative;

    HttpUrl base;
    if (!httpParseUrl(baseUrl, base)) return {};

    std::string out = "http://" + base.host + ":" + std::to_string(base.port);
    if (relative.front() != '/') out += "/";
    out += relative;
    return out;
}

bool PortMapper::parseDeviceDescription(const std::string& xml,
                                        const std::string& baseUrl,
                                        IgdDevice& out) {
    // Walk each <service> block and keep the first WAN connection service found,
    // in the priority order above. Scanning per block (rather than searching the
    // whole document for a controlURL) is what keeps the control URL paired with
    // the service type it actually belongs to.
    for (const char* wanted : kWanServices) {
        std::size_t pos = 0;
        while (true) {
            const std::size_t sBegin = xml.find("<service>", pos);
            if (sBegin == std::string::npos) break;
            const std::size_t sEnd = xml.find("</service>", sBegin);
            if (sEnd == std::string::npos) break;

            const std::string block = xml.substr(sBegin, sEnd - sBegin);
            const std::string type  = extractXmlValue(block, "serviceType");
            if (type == wanted) {
                const std::string ctrl = extractXmlValue(block, "controlURL");
                if (!ctrl.empty()) {
                    out.location    = baseUrl;
                    out.serviceType = type;
                    out.controlUrl  = resolveUrl(baseUrl, ctrl);
                    return !out.controlUrl.empty();
                }
            }
            pos = sEnd + 10;
        }
    }
    return false;
}

std::string PortMapper::buildSoapBody(
    const std::string& serviceType, const std::string& action,
    const std::vector<std::pair<std::string, std::string>>& args) {

    std::string body =
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body>"
        "<u:" + action + " xmlns:u=\"" + serviceType + "\">";
    for (const auto& [name, value] : args) {
        body += "<" + name + ">" + xmlEscape(value) + "</" + name + ">";
    }
    body += "</u:" + action + "></s:Body></s:Envelope>";
    return body;
}

bool PortMapper::isPrivateOrCgnat(const std::string& ip) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;

    if (a == 10)                          return true;   // RFC1918
    if (a == 192 && b == 168)             return true;   // RFC1918
    if (a == 172 && b >= 16 && b <= 31)   return true;   // RFC1918
    if (a == 127)                         return true;   // loopback
    if (a == 169 && b == 254)             return true;   // link-local
    // 100.64.0.0/10 — carrier-grade NAT. Seeing this as the "external" IP means
    // the ISP is doing another layer of NAT, so no port mapping can ever be
    // reachable from the internet.
    if (a == 100 && b >= 64 && b <= 127)  return true;
    return false;
}

// ─── Discovery ───────────────────────────────────────────────────────────────

PortMapResult PortMapper::discover(IgdDevice& out, int timeoutMs) {
    HE_LOG_DEBUG(Net, "UPnP: searching for an internet gateway (SSDP, %d ms budget)", timeoutMs);
    SocketHandle udp = socketCreateUdp();
    if (udp == kInvalidSocket) {
        HE_LOG_ERROR(Net, "UPnP: could not create a UDP socket for discovery");
        return PortMapResult::NotSupported;
    }
    if (!socketBindUdp(udp, 0)) {
        HE_LOG_ERROR(Net, "UPnP: could not bind the discovery socket");
        socketClose(udp);
        return PortMapResult::NotSupported;
    }
    socketSetMulticastTtl(udp, 2);

    const std::string search = buildSsdpSearch();
    std::size_t sent = 0;
    const SocketResult sr = socketSendTo(
        udp, reinterpret_cast<const std::uint8_t*>(search.data()), search.size(),
        kSsdpAddr, kSsdpPort, sent);
    if (sr != SocketResult::Ok) {
        // On macOS this is the Local Network permission, not the network: the
        // multicast send fails while internet traffic keeps working, which
        // looks like a routing bug unless you know to look here.
        HE_LOG_WARN(Net, "UPnP: multicast search could not be sent to %s:%u — on macOS this "
                         "usually means the Local Network permission was denied",
                    kSsdpAddr, static_cast<unsigned>(kSsdpPort));
        socketClose(udp);
        return PortMapResult::NoRouterFound;
    }

    // Collect replies until something usable turns up or the budget expires;
    // several devices may answer and only some are gateways.
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeoutMs);
    std::uint8_t buf[4096];
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count());
        if (!socketWaitReadable(udp, std::min(remaining, 250))) continue;

        std::size_t got = 0;
        std::string fromHost;
        std::uint16_t fromPort = 0;
        if (socketRecvFrom(udp, buf, sizeof(buf), got, fromHost, fromPort) != SocketResult::Ok) {
            continue;
        }

        const std::string response(reinterpret_cast<char*>(buf), got);
        const std::string location = parseSsdpLocation(response);
        if (location.empty()) continue;

        HE_LOG_DEBUG(Net, "UPnP: %s answered, fetching device description from %s",
                     fromHost.c_str(), location.c_str());
        const HttpResponse desc = httpGet(location, 4000);
        if (!desc.ok || desc.statusCode != 200) {
            HE_LOG_DEBUG(Net, "UPnP: description fetch failed (HTTP %d) — trying other responders",
                         desc.statusCode);
            continue;
        }

        if (parseDeviceDescription(desc.body, location, out)) {
            HE_LOG_INFO(Net, "UPnP: gateway found at %s (service %s)",
                        fromHost.c_str(), out.serviceType.c_str());
            socketClose(udp);
            return PortMapResult::Ok;
        }
        // Plenty of non-gateway devices answer SSDP (printers, TVs); this is
        // normal, not an error.
        HE_LOG_DEBUG(Net, "UPnP: %s is not an internet gateway, ignoring", fromHost.c_str());
    }

    socketClose(udp);
    HE_LOG_INFO(Net, "UPnP: no internet gateway answered within %d ms "
                     "(UPnP is disabled by default on many routers)", timeoutMs);
    return PortMapResult::NoRouterFound;
}

// ─── SOAP calls ──────────────────────────────────────────────────────────────

namespace {

// Issue a SOAP action and hand back the raw response body.
bool soapCall(const IgdDevice& igd, const std::string& action,
              const std::vector<std::pair<std::string, std::string>>& args,
              std::string& outBody) {
    if (igd.controlUrl.empty() || igd.serviceType.empty()) return false;

    const std::string body = PortMapper::buildSoapBody(igd.serviceType, action, args);
    const std::vector<std::string> headers = {
        "Content-Type: text/xml; charset=\"utf-8\"",
        "SOAPAction: \"" + igd.serviceType + "#" + action + "\"",
    };

    const HttpResponse resp = httpRequest(igd.controlUrl, "POST", headers, body, 5000);
    if (!resp.ok) {
        HE_LOG_WARN(Net, "UPnP: SOAP %s could not reach the router", action.c_str());
        return false;
    }
    outBody = resp.body;
    if (resp.statusCode != 200) {
        // Routers answer a refusal with a SOAP fault carrying a UPnP error
        // code — far more informative than the HTTP status alone.
        const std::string err = PortMapper::extractXmlValue(resp.body, "errorCode");
        HE_LOG_WARN(Net, "UPnP: router refused %s (HTTP %d%s%s)", action.c_str(),
                    resp.statusCode, err.empty() ? "" : ", UPnP error ", err.c_str());
        return false;
    }
    HE_LOG_DEBUG(Net, "UPnP: %s accepted", action.c_str());
    return true;
}

} // namespace

PortMapResult PortMapper::addMapping(const IgdDevice& igd,
                                     std::uint16_t externalPort,
                                     std::uint16_t internalPort,
                                     const std::string& description,
                                     PortMapping& out,
                                     const std::string& internalHost,
                                     std::uint32_t leaseSeconds) {
    std::string host = internalHost.empty() ? socketLocalAddress() : internalHost;
    if (host.empty()) {
        HE_LOG_ERROR(Net, "UPnP: no local address to point the mapping at");
        return PortMapResult::NotSupported;
    }
    if (igd.controlUrl.empty()) {
        HE_LOG_ERROR(Net, "UPnP: gateway exposes no WAN connection service");
        return PortMapResult::NoServiceFound;
    }
    HE_LOG_DEBUG(Net, "UPnP: requesting TCP %u → %s:%u, lease %us",
                 static_cast<unsigned>(externalPort), host.c_str(),
                 static_cast<unsigned>(internalPort), leaseSeconds);

    std::string body;
    const bool ok = soapCall(igd, "AddPortMapping", {
        { "NewRemoteHost",             "" },
        { "NewExternalPort",           std::to_string(externalPort) },
        { "NewProtocol",               "TCP" },
        { "NewInternalPort",           std::to_string(internalPort) },
        { "NewInternalClient",         host },
        { "NewEnabled",                "1" },
        { "NewPortMappingDescription", description },
        { "NewLeaseDuration",          std::to_string(leaseSeconds) },
    }, body);

    if (!ok) return PortMapResult::RequestFailed;

    out.externalPort = externalPort;
    out.internalPort = internalPort;
    out.internalHost = host;
    out.controlUrl   = igd.controlUrl;
    out.serviceType  = igd.serviceType;

    // Best-effort: a router that refuses this still has a working mapping.
    std::string wan;
    if (externalIp(igd, wan) == PortMapResult::Ok) out.externalIp = wan;

    HE_LOG_INFO(Net, "UPnP: mapped TCP %u → %s:%u (router WAN side: %s)",
                static_cast<unsigned>(externalPort), host.c_str(),
                static_cast<unsigned>(internalPort),
                out.externalIp.empty() ? "not reported" : out.externalIp.c_str());
    return PortMapResult::Ok;
}

PortMapResult PortMapper::removeMapping(const IgdDevice& igd, std::uint16_t externalPort) {
    std::string body;
    const bool ok = soapCall(igd, "DeletePortMapping", {
        { "NewRemoteHost",   "" },
        { "NewExternalPort", std::to_string(externalPort) },
        { "NewProtocol",     "TCP" },
    }, body);
    if (ok) HE_LOG_INFO(Net, "UPnP: removed the mapping for TCP %u",
                        static_cast<unsigned>(externalPort));
    else    HE_LOG_WARN(Net, "UPnP: could not remove the mapping for TCP %u — it may stay "
                             "open until the router is restarted",
                        static_cast<unsigned>(externalPort));
    return ok ? PortMapResult::Ok : PortMapResult::RequestFailed;
}

PortMapResult PortMapper::externalIp(const IgdDevice& igd, std::string& out) {
    std::string body;
    if (!soapCall(igd, "GetExternalIPAddress", {}, body)) {
        return PortMapResult::RequestFailed;
    }
    out = extractXmlValue(body, "NewExternalIPAddress");
    return out.empty() ? PortMapResult::RequestFailed : PortMapResult::Ok;
}

// ─── NAT-PMP (RFC 6886) ──────────────────────────────────────────────────────
// Wire format, all big-endian:
//   request  [ver=0][opcode][reserved:2][internalPort:2][externalPort:2][lifetime:4]
//   response [ver=0][opcode+128][result:2][epoch:4][internalPort:2][externalPort:2][lifetime:4]
// Opcode 1 maps UDP, 2 maps TCP; an address request is just [0][0].

namespace {

constexpr std::uint16_t kNatPmpPort   = 5351;
constexpr std::uint8_t  kOpAddress    = 0;
constexpr std::uint8_t  kOpMapTcp     = 2;
constexpr std::uint8_t  kResponseFlag = 128;

void putU16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>(x >> 8));
    v.push_back(static_cast<std::uint8_t>(x & 0xFF));
}
void putU32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int i = 3; i >= 0; --i) v.push_back(static_cast<std::uint8_t>((x >> (i * 8)) & 0xFF));
}
std::uint16_t getU16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}
std::uint32_t getU32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8)  |  static_cast<std::uint32_t>(p[3]);
}

// One request, one reply, bounded wait. NAT-PMP is UDP, so a lost datagram just
// means no answer — treated as "router does not speak it" rather than retried
// forever, since the caller has a fallback ladder anyway.
bool natPmpExchange(const std::string& gateway, const std::vector<std::uint8_t>& request,
                    std::uint8_t* reply, std::size_t replyCapacity,
                    std::size_t& replyLen, int timeoutMs) {
    replyLen = 0;
    SocketHandle udp = socketCreateUdp();
    if (udp == kInvalidSocket) return false;
    if (!socketBindUdp(udp, 0)) { socketClose(udp); return false; }

    std::size_t sent = 0;
    if (socketSendTo(udp, request.data(), request.size(), gateway, kNatPmpPort, sent)
        != SocketResult::Ok) {
        socketClose(udp);
        return false;
    }

    const bool ready = socketWaitReadable(udp, timeoutMs);
    if (!ready) {
        HE_LOG_DEBUG(Net, "NAT-PMP: gateway %s did not answer within %d ms",
                     gateway.c_str(), timeoutMs);
        socketClose(udp);
        return false;
    }

    std::string fromHost;
    std::uint16_t fromPort = 0;
    const SocketResult rc =
        socketRecvFrom(udp, reply, replyCapacity, replyLen, fromHost, fromPort);
    socketClose(udp);

    // Only trust an answer that actually came from the gateway we asked.
    if (rc == SocketResult::Ok && fromHost != gateway) {
        HE_LOG_WARN(Net, "NAT-PMP: ignoring a reply from %s — we asked %s",
                    fromHost.c_str(), gateway.c_str());
    }
    return rc == SocketResult::Ok && fromHost == gateway;
}

} // namespace

std::vector<std::uint8_t> PortMapper::buildNatPmpRequest(std::uint8_t opcode,
                                                         std::uint16_t internalPort,
                                                         std::uint16_t externalPort,
                                                         std::uint32_t lifetimeSeconds) {
    std::vector<std::uint8_t> v;
    v.push_back(0);        // version
    v.push_back(opcode);
    if (opcode == kOpAddress) return v;   // address requests are just two bytes

    putU16(v, 0);          // reserved
    putU16(v, internalPort);
    putU16(v, externalPort);
    putU32(v, lifetimeSeconds);
    return v;
}

bool PortMapper::parseNatPmpMapResponse(const std::uint8_t* data, std::size_t len,
                                        std::uint16_t& outInternalPort,
                                        std::uint16_t& outExternalPort,
                                        std::uint32_t& outLifetime,
                                        std::uint16_t& outResultCode) {
    if (!data || len < 16) return false;
    if (data[0] != 0) return false;                       // unknown version
    if (data[1] < kResponseFlag) return false;            // not a response

    outResultCode   = getU16(data + 2);
    outInternalPort = getU16(data + 8);
    outExternalPort = getU16(data + 10);
    outLifetime     = getU32(data + 12);
    return true;
}

bool PortMapper::parseNatPmpAddressResponse(const std::uint8_t* data, std::size_t len,
                                            std::string& outIp,
                                            std::uint16_t& outResultCode) {
    if (!data || len < 12) return false;
    if (data[0] != 0) return false;
    if (data[1] != (kOpAddress + kResponseFlag)) return false;

    outResultCode = getU16(data + 2);
    outIp = std::to_string(data[8]) + "." + std::to_string(data[9]) + "." +
            std::to_string(data[10]) + "." + std::to_string(data[11]);
    return true;
}

PortMapResult PortMapper::natPmpExternalIp(const std::string& gateway, std::string& out,
                                           int timeoutMs) {
    if (gateway.empty()) return PortMapResult::NotSupported;

    const auto req = buildNatPmpRequest(kOpAddress, 0, 0, 0);
    std::uint8_t reply[64];
    std::size_t  replyLen = 0;
    if (!natPmpExchange(gateway, req, reply, sizeof(reply), replyLen, timeoutMs)) {
        return PortMapResult::NoRouterFound;
    }

    std::uint16_t result = 0;
    if (!parseNatPmpAddressResponse(reply, replyLen, out, result)) {
        HE_LOG_WARN(Net, "NAT-PMP: malformed address reply from %s (%zu bytes)",
                    gateway.c_str(), replyLen);
        return PortMapResult::RequestFailed;
    }
    if (result != 0) {
        HE_LOG_WARN(Net, "NAT-PMP: gateway %s refused the address request (result code %u)",
                    gateway.c_str(), static_cast<unsigned>(result));
        return PortMapResult::RequestFailed;
    }
    HE_LOG_DEBUG(Net, "NAT-PMP: gateway %s reports WAN address %s",
                 gateway.c_str(), out.c_str());
    return PortMapResult::Ok;
}

PortMapResult PortMapper::natPmpAddMapping(const std::string& gateway,
                                           std::uint16_t externalPort,
                                           std::uint16_t internalPort,
                                           std::uint32_t lifetimeSeconds,
                                           PortMapping& out, int timeoutMs) {
    if (gateway.empty()) return PortMapResult::NotSupported;

    HE_LOG_DEBUG(Net, "NAT-PMP: asking %s for TCP %u → %u, lifetime %us",
                 gateway.c_str(), static_cast<unsigned>(externalPort),
                 static_cast<unsigned>(internalPort), lifetimeSeconds);
    const auto req = buildNatPmpRequest(kOpMapTcp, internalPort, externalPort,
                                        lifetimeSeconds);
    std::uint8_t reply[64];
    std::size_t  replyLen = 0;
    if (!natPmpExchange(gateway, req, reply, sizeof(reply), replyLen, timeoutMs)) {
        return PortMapResult::NoRouterFound;
    }

    std::uint16_t result = 0, gotInternal = 0, gotExternal = 0;
    std::uint32_t gotLifetime = 0;
    if (!parseNatPmpMapResponse(reply, replyLen, gotInternal, gotExternal,
                                gotLifetime, result)) {
        HE_LOG_WARN(Net, "NAT-PMP: malformed mapping reply from %s (%zu bytes)",
                    gateway.c_str(), replyLen);
        return PortMapResult::RequestFailed;
    }
    if (result != 0) {
        HE_LOG_WARN(Net, "NAT-PMP: gateway %s refused the mapping (result code %u)",
                    gateway.c_str(), static_cast<unsigned>(result));
        return PortMapResult::RequestFailed;
    }
    // Worth calling out explicitly: publishing the requested port instead of
    // the granted one would advertise an endpoint nobody listens on.
    if (gotExternal != externalPort) {
        HE_LOG_INFO(Net, "NAT-PMP: router granted external port %u instead of the "
                         "requested %u — publishing the granted one",
                    static_cast<unsigned>(gotExternal),
                    static_cast<unsigned>(externalPort));
    }

    out.internalPort = gotInternal;
    // The router may hand back a DIFFERENT external port than requested; using
    // the one we asked for would publish an endpoint nobody is listening on.
    out.externalPort = gotExternal;
    out.internalHost = socketLocalAddress();

    std::string wan;
    if (natPmpExternalIp(gateway, wan, timeoutMs) == PortMapResult::Ok) out.externalIp = wan;
    HE_LOG_INFO(Net, "NAT-PMP: mapped TCP %u → %s:%u for %us (router WAN side: %s)",
                static_cast<unsigned>(out.externalPort), out.internalHost.c_str(),
                static_cast<unsigned>(out.internalPort), gotLifetime,
                out.externalIp.empty() ? "not reported" : out.externalIp.c_str());
    return PortMapResult::Ok;
}

PortMapResult PortMapper::natPmpRemoveMapping(const std::string& gateway,
                                              std::uint16_t internalPort,
                                              int timeoutMs) {
    // RFC 6886: lifetime 0 with external port 0 deletes the mapping.
    const auto req = buildNatPmpRequest(kOpMapTcp, internalPort, 0, 0);
    std::uint8_t reply[64];
    std::size_t  replyLen = 0;
    if (!natPmpExchange(gateway, req, reply, sizeof(reply), replyLen, timeoutMs)) {
        return PortMapResult::NoRouterFound;
    }
    std::uint16_t result = 0, a = 0, b = 0;
    std::uint32_t c = 0;
    if (!parseNatPmpMapResponse(reply, replyLen, a, b, c, result)) {
        return PortMapResult::RequestFailed;
    }
    if (result == 0) {
        HE_LOG_INFO(Net, "NAT-PMP: removed the mapping for internal port %u",
                    static_cast<unsigned>(internalPort));
    } else {
        HE_LOG_WARN(Net, "NAT-PMP: gateway refused to remove the mapping for port %u "
                         "(result code %u) — it will expire on its own",
                    static_cast<unsigned>(internalPort), static_cast<unsigned>(result));
    }
    return result == 0 ? PortMapResult::Ok : PortMapResult::RequestFailed;
}

// ─── The fallback ladder ─────────────────────────────────────────────────────

PortMapResult PortMapper::mapPort(std::uint16_t port, const std::string& description,
                                  MappingHandle& outHandle, PortMapping& outInfo) {
    outHandle = MappingHandle{};
    HE_LOG_INFO(Net, "Port mapping: trying to open TCP %u automatically",
                static_cast<unsigned>(port));

    // UPnP first: broader support across consumer routers.
    IgdDevice igd;
    if (discover(igd, 3000) == PortMapResult::Ok)
    {
        if (addMapping(igd, port, port, description, outInfo) == PortMapResult::Ok)
        {
            outHandle.method = MappingHandle::Method::Upnp;
            outHandle.igd    = igd;
            outHandle.port   = port;
            HE_LOG_INFO(Net, "Port mapping: succeeded via UPnP");
            return PortMapResult::Ok;
        }
    }

    // NAT-PMP second. A router speaks one or the other, so a UPnP failure says
    // nothing about whether this will work — Apple base stations answer here
    // and stay silent to SSDP entirely.
    HE_LOG_DEBUG(Net, "Port mapping: UPnP did not deliver, trying NAT-PMP");
    const std::string gateway = socketDefaultGateway();
    if (gateway.empty()) {
        HE_LOG_WARN(Net, "Port mapping: no default gateway in the routing table — NAT-PMP "
                         "cannot be attempted");
    }
    if (!gateway.empty())
    {
        HE_LOG_DEBUG(Net, "Port mapping: default gateway is %s", gateway.c_str());
        // Two hours, and re-requested whenever the session re-registers; RFC 6886
        // recommends a finite lifetime so a crashed client's mapping expires
        // instead of lingering forever.
        constexpr std::uint32_t kLifetimeSeconds = 7200;
        if (natPmpAddMapping(gateway, port, port, kLifetimeSeconds, outInfo)
            == PortMapResult::Ok)
        {
            outHandle.method  = MappingHandle::Method::NatPmp;
            outHandle.gateway = gateway;
            outHandle.port    = port;
            HE_LOG_INFO(Net, "Port mapping: succeeded via NAT-PMP");
            return PortMapResult::Ok;
        }
    }

    // Distinguish "the router said no" from "we never reached the router". Both
    // arrive here as NoRouterFound, but they call for completely different
    // action, and blaming the router for a permission problem sends people to
    // change settings that were never consulted.
    if (socketLocalNetworkBlocked())
    {
        HE_LOG_WARN(Net, "Port mapping: local network access is blocked for this process, so "
                         "neither UPnP nor NAT-PMP could even reach the router. Nothing was "
                         "wrong with the router or its settings.");
        return PortMapResult::LocalNetworkBlocked;
    }

    HE_LOG_WARN(Net, "Port mapping: neither UPnP nor NAT-PMP opened TCP %u — both are "
                     "commonly disabled, and behind carrier-grade NAT neither can work",
                static_cast<unsigned>(port));
    return PortMapResult::NoRouterFound;
}

void PortMapper::unmapPort(const MappingHandle& handle)
{
    switch (handle.method)
    {
    case MappingHandle::Method::Upnp:   removeMapping(handle.igd, handle.port); break;
    case MappingHandle::Method::NatPmp: natPmpRemoveMapping(handle.gateway, handle.port); break;
    // Not an error — most sessions never got a mapping in the first place.
    case MappingHandle::Method::None:
        HE_LOG_DEBUG(Net, "Port mapping: nothing to take down");
        break;
    }
}

} // namespace HE::Net
