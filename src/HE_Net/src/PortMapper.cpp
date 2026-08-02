#include "Net/PortMapper.h"

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
    SocketHandle udp = socketCreateUdp();
    if (udp == kInvalidSocket) return PortMapResult::NotSupported;
    if (!socketBindUdp(udp, 0)) { socketClose(udp); return PortMapResult::NotSupported; }
    socketSetMulticastTtl(udp, 2);

    const std::string search = buildSsdpSearch();
    std::size_t sent = 0;
    const SocketResult sr = socketSendTo(
        udp, reinterpret_cast<const std::uint8_t*>(search.data()), search.size(),
        kSsdpAddr, kSsdpPort, sent);
    if (sr != SocketResult::Ok) { socketClose(udp); return PortMapResult::NoRouterFound; }

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

        const HttpResponse desc = httpGet(location, 4000);
        if (!desc.ok || desc.statusCode != 200) continue;

        if (parseDeviceDescription(desc.body, location, out)) {
            socketClose(udp);
            return PortMapResult::Ok;
        }
    }

    socketClose(udp);
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
    if (!resp.ok) return false;
    outBody = resp.body;
    return resp.statusCode == 200;
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
    if (host.empty()) return PortMapResult::NotSupported;
    if (igd.controlUrl.empty()) return PortMapResult::NoServiceFound;

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

    return PortMapResult::Ok;
}

PortMapResult PortMapper::removeMapping(const IgdDevice& igd, std::uint16_t externalPort) {
    std::string body;
    const bool ok = soapCall(igd, "DeletePortMapping", {
        { "NewRemoteHost",   "" },
        { "NewExternalPort", std::to_string(externalPort) },
        { "NewProtocol",     "TCP" },
    }, body);
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

} // namespace HE::Net
