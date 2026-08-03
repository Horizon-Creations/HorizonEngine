#include "Net/PortMapper.h"

#include "NetLog.h"

#include "Net/HttpClient.h"
#include "Net/Socket.h"

// PCP carries raw addresses on the wire, so this file needs the address
// conversion functions directly rather than going through the socket wrapper.
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
#endif

#include <Hpak/Aes256Gcm.h>   // Hpak::randomBytes for the PCP nonce

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
    // The IPv6 firewall service rides along in the same walk: it lives in the
    // same description document, and finding it now saves a re-fetch when a
    // pinhole is wanted later.
    static const char* const kV6Fw = "urn:schemas-upnp-org:service:WANIPv6FirewallControl:1";
    out.v6fwControlUrl.clear();
    out.v6fwServiceType.clear();

    bool found = false;
    for (const char* wanted : kWanServices) {
        std::size_t pos = 0;
        while (true) {
            const std::size_t sBegin = xml.find("<service>", pos);
            if (sBegin == std::string::npos) break;
            const std::size_t sEnd = xml.find("</service>", sBegin);
            if (sEnd == std::string::npos) break;

            const std::string block = xml.substr(sBegin, sEnd - sBegin);
            const std::string type  = extractXmlValue(block, "serviceType");
            const std::string ctrl  = extractXmlValue(block, "controlURL");
            if (!found && type == wanted && !ctrl.empty()) {
                out.location    = baseUrl;
                out.serviceType = type;
                out.controlUrl  = resolveUrl(baseUrl, ctrl);
                found = !out.controlUrl.empty();
            }
            if (type == kV6Fw && !ctrl.empty() && out.v6fwControlUrl.empty()) {
                out.v6fwServiceType = type;
                out.v6fwControlUrl  = resolveUrl(baseUrl, ctrl);
            }
            pos = sEnd + 10;
        }
        if (found) break;
    }
    return found;
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

namespace {

// Interfaces worth searching on: a router can only be behind a routable IPv4
// address. Loopback has no router, and 169.254.x is what an adapter falls back
// to when it never got a lease — both are pure noise here, and on Windows there
// are usually several of them (Hyper-V, WSL, VPN, VirtualBox all add adapters).
bool isSearchableIPv4(const std::string& a) {
    if (a.find(':') != std::string::npos) return false;   // IPv6
    if (a.rfind("127.", 0) == 0)          return false;   // loopback
    if (a.rfind("169.254.", 0) == 0)      return false;   // link-local
    return !a.empty();
}

} // namespace

PortMapResult PortMapper::discover(IgdDevice& out, int timeoutMs) {
    const std::string search  = buildSsdpSearch();
    const std::string gateway = socketDefaultGateway();

    // Search from EVERY interface rather than from whichever one the routing
    // table happens to pick for the multicast group.
    //
    // This is the difference between working and silently not working on
    // Windows. A machine there almost always has virtual adapters, and one of
    // them regularly wins the route to 239.255.255.250 — at which point the
    // search leaves into a virtual switch, the router never sees it, and the
    // failure is indistinguishable from "this router has no UPnP": the send
    // reports success, nothing answers, the search times out.
    std::vector<std::string> ifaces;
    for (const auto& a : socketLocalAddresses())
        if (isSearchableIPv4(a)) ifaces.push_back(a);
    if (ifaces.empty()) ifaces.emplace_back();   // fall back to the default route

    HE_LOG_DEBUG(Net, "UPnP: searching for an internet gateway on %zu interface(s), %d ms budget",
                 ifaces.size(), timeoutMs);

    struct Probe { SocketHandle sock; std::string iface; };
    std::vector<Probe> probes;

    for (const auto& iface : ifaces) {
        const char* ifname = iface.empty() ? "(default route)" : iface.c_str();
        SocketHandle udp = socketCreateUdp();
        if (udp == kInvalidSocket) {
            HE_LOG_WARN(Net, "UPnP: no socket for %s", ifname);
            continue;
        }
        // Binding to the interface pins BOTH the multicast source and the port
        // the router's unicast reply comes back to.
        if (!(iface.empty() ? socketBindUdp(udp, 0) : socketBindUdpTo(udp, iface, 0))) {
            HE_LOG_DEBUG(Net, "UPnP: could not bind the search socket to %s", ifname);
            socketClose(udp);
            continue;
        }
        socketSetMulticastTtl(udp, 2);
        if (!iface.empty()) socketSetMulticastInterface(udp, iface);

        const auto send = [&](const char* host, const char* what) {
            std::size_t sent = 0;
            const SocketResult r = socketSendTo(
                udp, reinterpret_cast<const std::uint8_t*>(search.data()), search.size(),
                host, kSsdpPort, sent);
            if (r != SocketResult::Ok)
                HE_LOG_TRACE(Net, "UPnP: %s search from %s failed", what, ifname);
            return r == SocketResult::Ok;
        };

        // Twice, because SSDP rides on UDP multicast and a dropped search looks
        // exactly like a router that does not speak UPnP.
        bool anySent = false;
        for (int attempt = 0; attempt < 2; ++attempt)
            anySent |= send(kSsdpAddr, "multicast");

        // And once straight at the router. A unicast M-SEARCH is legal SSDP and
        // involves no multicast routing whatsoever, so it still arrives on the
        // machines where the multicast above left through the wrong adapter.
        if (!gateway.empty()) anySent |= send(gateway.c_str(), "unicast");

        if (!anySent) { socketClose(udp); continue; }
        HE_LOG_DEBUG(Net, "UPnP: search sent from %s", ifname);
        probes.push_back({udp, iface});
    }

    const auto closeAll = [&] { for (auto& p : probes) socketClose(p.sock); };

    if (probes.empty()) {
        // On macOS this is the Local Network permission, not the network: the
        // multicast send fails while internet traffic keeps working, which
        // looks like a routing bug unless you know to look here.
        HE_LOG_WARN(Net, "UPnP: the search could not be sent on any interface — on macOS "
                         "this usually means the Local Network permission was denied");
        return PortMapResult::NoRouterFound;
    }

    // Collect replies until something usable turns up or the budget expires;
    // several devices may answer and only some are gateways.
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeoutMs);
    std::uint8_t buf[4096];
    int responders = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        for (auto& probe : probes) {
            if (std::chrono::steady_clock::now() >= deadline) break;
            if (!socketWaitReadable(probe.sock, 50)) continue;

            std::size_t got = 0;
            std::string fromHost;
            std::uint16_t fromPort = 0;
            if (socketRecvFrom(probe.sock, buf, sizeof(buf), got, fromHost, fromPort)
                != SocketResult::Ok) {
                continue;
            }
            ++responders;

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
                HE_LOG_INFO(Net, "UPnP: gateway found at %s via %s (service %s)",
                            fromHost.c_str(),
                            probe.iface.empty() ? "(default route)" : probe.iface.c_str(),
                            out.serviceType.c_str());
                closeAll();
                return PortMapResult::Ok;
            }
            // Plenty of non-gateway devices answer SSDP (printers, TVs); this is
            // normal, not an error.
            HE_LOG_DEBUG(Net, "UPnP: %s is not an internet gateway, ignoring", fromHost.c_str());
        }
    }

    closeAll();
    // Which of the two happened decides what the user should do, so they are
    // reported apart: nothing at all points at the search never arriving,
    // whereas answers without a gateway among them means UPnP is genuinely off.
    if (responders == 0) {
        HE_LOG_INFO(Net, "UPnP: nothing answered on any of %zu interface(s) within %d ms "
                         "(UPnP is disabled by default on many routers)",
                    probes.size(), timeoutMs);
    } else {
        HE_LOG_INFO(Net, "UPnP: %d device(s) answered but none is an internet gateway",
                    responders);
    }
    return PortMapResult::NoRouterFound;
}

// ─── SOAP calls ──────────────────────────────────────────────────────────────

namespace {

// Issue a SOAP action and hand back the raw response body.
bool soapCallTo(const std::string& controlUrl, const std::string& serviceType,
                const std::string& action,
                const std::vector<std::pair<std::string, std::string>>& args,
                std::string& outBody) {
    if (controlUrl.empty() || serviceType.empty()) return false;

    const std::string body = PortMapper::buildSoapBody(serviceType, action, args);
    const std::vector<std::string> headers = {
        "Content-Type: text/xml; charset=\"utf-8\"",
        "SOAPAction: \"" + serviceType + "#" + action + "\"",
    };

    const HttpResponse resp = httpRequest(controlUrl, "POST", headers, body, 5000);
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

bool soapCall(const IgdDevice& igd, const std::string& action,
              const std::vector<std::pair<std::string, std::string>>& args,
              std::string& outBody) {
    return soapCallTo(igd.controlUrl, igd.serviceType, action, args, outBody);
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

    if (!ok) {
        // 606 is "Action not authorized": the router understood the request and
        // declined it. Reported apart from a generic failure so the user is sent
        // to the setting instead of being told their router lacks the feature.
        if (extractXmlValue(body, "errorCode") == "606") {
            HE_LOG_WARN(Net, "UPnP: the router refuses port forwarding for this device "
                             "(error 606, not authorized)");
            return PortMapResult::Refused;
        }
        return PortMapResult::RequestFailed;
    }

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
                    std::size_t& replyLen, int timeoutMs,
                    const std::string& bindAddress = {}) {
    replyLen = 0;
    const bool v6 = gateway.find(':') != std::string::npos;
    SocketHandle udp = v6 ? socketCreateUdp6() : socketCreateUdp();
    if (udp == kInvalidSocket) return false;

    // PCP requires the datagram's source address to equal the client address
    // named inside it — the router rejects anything else as ADDRESS_MISMATCH.
    // An unbound socket leaves that choice to the OS, which on macOS prefers
    // the temporary privacy address and so never matches the stable one the
    // request names. Binding is what makes the two agree.
    const bool bound = bindAddress.empty()
        ? (v6 ? socketBindUdp6To(udp, "::", 0) : socketBindUdp(udp, 0))
        : (v6 ? socketBindUdp6To(udp, bindAddress, 0)
              : socketBindUdpTo(udp, bindAddress, 0));
    if (!bound) {
        HE_LOG_DEBUG(Net, "PCP: could not bind to %s",
                     bindAddress.empty() ? "(any)" : bindAddress.c_str());
        socketClose(udp);
        return false;
    }

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

    // Only trust an answer that actually came from the gateway we asked. The
    // scope suffix is ours, not part of the wire address, so it is stripped
    // before comparing — recvfrom reports the bare address.
    const std::string bare = gateway.substr(0, gateway.find('%'));
    if (rc == SocketResult::Ok && fromHost != bare) {
        HE_LOG_WARN(Net, "NAT-PMP: ignoring a reply from %s — we asked %s",
                    fromHost.c_str(), bare.c_str());
    }
    return rc == SocketResult::Ok && fromHost == bare;
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
        if (replyLen >= 1 && reply[0] >= 2) {
            // Port 5351 carries both protocols. A PCP-only router answers a
            // NAT-PMP request in PCP's own format, which is not corruption —
            // the PCP attempt is the one that counts, so this stays quiet.
            HE_LOG_DEBUG(Net, "NAT-PMP: %s speaks PCP (version %u), not NAT-PMP", 
                         gateway.c_str(), static_cast<unsigned>(reply[0]));
            return PortMapResult::NotSupported;
        }
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
        if (replyLen >= 1 && reply[0] >= 2) {
            // Port 5351 carries both protocols. A PCP-only router answers a
            // NAT-PMP request in PCP's own format, which is not corruption —
            // the PCP attempt is the one that counts, so this stays quiet.
            HE_LOG_DEBUG(Net, "NAT-PMP: %s speaks PCP (version %u), not NAT-PMP", 
                         gateway.c_str(), static_cast<unsigned>(reply[0]));
            return PortMapResult::NotSupported;
        }
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

// ─── PCP (RFC 6887) ──────────────────────────────────────────────────────────

namespace {

constexpr std::uint8_t kPcpVersion   = 2;
constexpr std::uint8_t kPcpOpMap     = 1;
constexpr std::uint8_t kPcpResponse  = 0x80;   // top bit of the opcode byte
constexpr std::uint8_t kPcpProtoTcp  = 6;      // IANA protocol number
constexpr std::size_t  kPcpHeaderLen = 24;
constexpr std::size_t  kPcpMapLen    = 36;

// PCP carries every address as 16 bytes. An IPv4 address goes in as the
// IPv4-mapped form ::ffff:a.b.c.d, which is what lets one message shape serve
// both families.
bool addressToPcpBytes(const std::string& text, std::uint8_t out[16])
{
    std::memset(out, 0, 16);
    in6_addr v6{};
    if (::inet_pton(AF_INET6, text.c_str(), &v6) == 1)
    {
        std::memcpy(out, &v6, 16);
        return true;
    }
    in_addr v4{};
    if (::inet_pton(AF_INET, text.c_str(), &v4) == 1)
    {
        out[10] = 0xFF;
        out[11] = 0xFF;
        std::memcpy(out + 12, &v4, 4);
        return true;
    }
    return false;
}

std::string pcpBytesToAddress(const std::uint8_t in[16])
{
    // Recognise the IPv4-mapped prefix and render it as a v4 address, or callers
    // would see "::ffff:1.2.3.4" where they expect "1.2.3.4".
    static const std::uint8_t kV4Prefix[12] = { 0,0,0,0, 0,0,0,0, 0,0,0xFF,0xFF };
    char buf[INET6_ADDRSTRLEN] = {};
    if (std::memcmp(in, kV4Prefix, 12) == 0)
    {
        in_addr v4{};
        std::memcpy(&v4, in + 12, 4);
        if (::inet_ntop(AF_INET, &v4, buf, sizeof(buf))) return buf;
        return {};
    }
    in6_addr v6{};
    std::memcpy(&v6, in, 16);
    if (::inet_ntop(AF_INET6, &v6, buf, sizeof(buf))) return buf;
    return {};
}

} // namespace

std::vector<std::uint8_t> PortMapper::buildPcpMapRequest(const std::string& clientAddress,
                                                         const std::uint8_t nonce[12],
                                                         std::uint16_t internalPort,
                                                         std::uint16_t suggestedExternalPort,
                                                         std::uint32_t lifetimeSeconds)
{
    std::vector<std::uint8_t> v;
    v.reserve(kPcpHeaderLen + kPcpMapLen);

    // Header: version, opcode (request → top bit clear), 2 reserved, lifetime,
    // then the client's own address.
    v.push_back(kPcpVersion);
    v.push_back(kPcpOpMap);
    v.push_back(0);
    v.push_back(0);
    putU32(v, lifetimeSeconds);
    std::uint8_t client[16];
    addressToPcpBytes(clientAddress, client);
    v.insert(v.end(), client, client + 16);

    // MAP payload: nonce, protocol, 3 reserved, internal port, suggested
    // external port, suggested external address.
    v.insert(v.end(), nonce, nonce + 12);
    v.push_back(kPcpProtoTcp);
    v.push_back(0);
    v.push_back(0);
    v.push_back(0);
    putU16(v, internalPort);
    putU16(v, suggestedExternalPort);
    // All-zero means "you choose", which is the honest request: the router is
    // free to hand back a different port, and using the one we asked for instead
    // of the one granted would publish an endpoint nobody listens on.
    v.insert(v.end(), 16, 0);
    return v;
}

bool PortMapper::parsePcpMapResponse(const std::uint8_t* data, std::size_t len,
                                     PcpMapping& out, std::uint8_t& outResultCode)
{
    if (!data || len < kPcpHeaderLen + kPcpMapLen) return false;
    if (data[0] != kPcpVersion) return false;                     // not PCP
    if ((data[1] & kPcpResponse) == 0) return false;              // a request echoed back
    if ((data[1] & 0x7F) != kPcpOpMap) return false;              // answer to something else

    outResultCode      = data[3];
    out.lifetimeSeconds = getU32(data + 4);

    const std::uint8_t* map = data + kPcpHeaderLen;
    std::memcpy(out.nonce, map, 12);
    out.externalPort    = getU16(map + 18);
    out.externalAddress = pcpBytesToAddress(map + 20);
    return true;
}

PortMapResult PortMapper::pcpMap(const std::string& gateway,
                                 const std::string& clientAddress,
                                 std::uint16_t internalPort,
                                 std::uint16_t suggestedExternalPort,
                                 std::uint32_t lifetimeSeconds,
                                 PcpMapping& out, int timeoutMs)
{
    if (gateway.empty() || clientAddress.empty()) return PortMapResult::NotSupported;

    // The nonce identifies this mapping for the rest of its life: the router
    // matches a later delete against it, so a caller that loses it can no longer
    // take its own mapping down.
    std::uint8_t nonce[12];
    if (!Hpak::randomBytes(nonce, sizeof(nonce)))
    {
        for (std::size_t i = 0; i < sizeof(nonce); ++i)
            nonce[i] = static_cast<std::uint8_t>((internalPort * 31u + i * 7u) & 0xFF);
    }

    const auto req = buildPcpMapRequest(clientAddress, nonce, internalPort,
                                        suggestedExternalPort, lifetimeSeconds);
    std::uint8_t reply[256];
    std::size_t  replyLen = 0;
    if (!natPmpExchange(gateway, req, reply, sizeof(reply), replyLen, timeoutMs,
                        clientAddress))
        return PortMapResult::NoRouterFound;

    std::uint8_t result = 0;
    if (!parsePcpMapResponse(reply, replyLen, out, result))
    {
        HE_LOG_DEBUG(Net, "PCP: %s answered something that is not a MAP response", gateway.c_str());
        return PortMapResult::RequestFailed;
    }
    if (result != 0)
    {
        // 1 = UNSUPP_VERSION, which is the expected answer from a NAT-PMP-only
        // router and not worth alarming about; anything else is a real refusal.
        if (result == 1)
            HE_LOG_DEBUG(Net, "PCP: %s does not speak PCP (UNSUPP_VERSION)", gateway.c_str());
        // 2 = NOT_AUTHORIZED: the router understood us and declined. Same
        // meaning as UPnP 606, and routers that refuse one refuse both.
        if (result == 2) {
            HE_LOG_WARN(Net, "PCP: %s refuses port forwarding for this device "
                             "(NOT_AUTHORIZED)", gateway.c_str());
            return PortMapResult::Refused;
        }
        if (result != 1)
            HE_LOG_WARN(Net, "PCP: %s refused the request (result code %u)",
                        gateway.c_str(), static_cast<unsigned>(result));
        return PortMapResult::RequestFailed;
    }

    std::memcpy(out.nonce, nonce, sizeof(nonce));
    return PortMapResult::Ok;
}

PortMapResult PortMapper::pcpUnmap(const std::string& gateway,
                                   const std::string& clientAddress,
                                   const std::uint8_t nonce[12],
                                   std::uint16_t internalPort,
                                   int timeoutMs)
{
    // Lifetime 0 is the delete. The nonce is what tells the router which of its
    // mappings this refers to.
    const auto req = buildPcpMapRequest(clientAddress, nonce, internalPort, 0, 0);
    std::uint8_t reply[256];
    std::size_t  replyLen = 0;
    if (!natPmpExchange(gateway, req, reply, sizeof(reply), replyLen, timeoutMs,
                        clientAddress))
        return PortMapResult::NoRouterFound;

    PcpMapping ignored;
    std::uint8_t result = 0;
    if (!parsePcpMapResponse(reply, replyLen, ignored, result)) return PortMapResult::RequestFailed;
    if (result == 0)
        HE_LOG_INFO(Net, "PCP: removed the mapping for port %u",
                    static_cast<unsigned>(internalPort));
    return result == 0 ? PortMapResult::Ok : PortMapResult::RequestFailed;
}

// ─── The fallback ladder ─────────────────────────────────────────────────────

// ─── IPv6 firewall pinholes ──────────────────────────────────────────────────

PortMapResult PortMapper::addPinhole(const IgdDevice& igd, const std::string& internalClient,
                                     std::uint16_t port, std::uint32_t leaseSeconds,
                                     std::string& outUniqueId) {
    outUniqueId.clear();
    if (igd.v6fwControlUrl.empty()) return PortMapResult::NoServiceFound;

    std::string body;
    const bool ok = soapCallTo(igd.v6fwControlUrl, igd.v6fwServiceType, "AddPinhole", {
        // RemoteHost/RemotePort empty+0 = any peer, which is what a session
        // host wants: the guests' addresses are unknown until they connect.
        { "RemoteHost",     "" },
        { "RemotePort",     "0" },
        { "InternalClient", internalClient },
        { "InternalPort",   std::to_string(port) },
        { "Protocol",       "6" },                            // TCP, by IANA number
        { "LeaseTime",      std::to_string(leaseSeconds) },
    }, body);
    if (!ok) {
        // 606 again means "understood, declined" — same verdict as everywhere
        // else, and the same per-device permission behind it.
        if (extractXmlValue(body, "errorCode") == "606") return PortMapResult::Refused;
        return PortMapResult::RequestFailed;
    }

    outUniqueId = extractXmlValue(body, "UniqueID");
    if (outUniqueId.empty()) {
        HE_LOG_WARN(Net, "UPnP: AddPinhole succeeded but returned no UniqueID — the "
                         "pinhole exists and cannot be deleted by us");
        return PortMapResult::RequestFailed;
    }
    return PortMapResult::Ok;
}

PortMapResult PortMapper::deletePinhole(const IgdDevice& igd, const std::string& uniqueId) {
    if (igd.v6fwControlUrl.empty() || uniqueId.empty()) return PortMapResult::NoServiceFound;
    std::string body;
    const bool ok = soapCallTo(igd.v6fwControlUrl, igd.v6fwServiceType, "DeletePinhole",
                               { { "UniqueID", uniqueId } }, body);
    return ok ? PortMapResult::Ok : PortMapResult::RequestFailed;
}

PortMapResult PortMapper::openPinhole(const std::string& globalV6, std::uint16_t port,
                                      PinholeHandle& out, const IgdDevice& igd) {
    out = PinholeHandle{};
    if (globalV6.empty()) return PortMapResult::NotSupported;

    bool refused = false;

    // PCP first: the standards path, a single datagram, and the only one of the
    // two that works on routers with no UPnP at all.
    const std::string gateway6 = socketDefaultGatewayIPv6();
    if (!gateway6.empty()) {
        PcpMapping pcp;
        const PortMapResult r = pcpMap(gateway6, globalV6, port, port, 7200, pcp);
        if (r == PortMapResult::Ok) {
            out.method        = PinholeHandle::Method::Pcp;
            out.gateway       = gateway6;
            out.clientAddress = globalV6;
            out.port          = port;
            std::memcpy(out.pcpNonce, pcp.nonce, sizeof(out.pcpNonce));
            HE_LOG_INFO(Net, "Pinhole: opened TCP [%s]:%u via PCP",
                        globalV6.c_str(), static_cast<unsigned>(port));
            return PortMapResult::Ok;
        }
        refused = refused || r == PortMapResult::Refused;
    }

    // UPnP WANIPv6FirewallControl second. Not an exotic fallback: a FRITZ!Box —
    // the most common home router in this codebase's user base — refuses PCP
    // pinholes even with its per-device permission set, and grants exactly the
    // same request through this service.
    IgdDevice dev = igd;
    if (dev.v6fwControlUrl.empty()) {
        if (discover(dev, 3000) != PortMapResult::Ok || dev.v6fwControlUrl.empty()) {
            HE_LOG_INFO(Net, "Pinhole: no PCP pinhole and no IPv6 firewall service — the "
                             "IPv6 address stays firewalled");
            return refused ? PortMapResult::Refused : PortMapResult::NoServiceFound;
        }
    }

    std::string uniqueId;
    // 86400 s is the ceiling the spec allows for a pinhole lease; a session that
    // outlives a day re-registers long before then anyway.
    const PortMapResult r = addPinhole(dev, globalV6, port, 86400, uniqueId);
    if (r == PortMapResult::Ok) {
        out.method   = PinholeHandle::Method::Upnp6fc;
        out.igd      = dev;
        out.uniqueId = uniqueId;
        out.port     = port;
        HE_LOG_INFO(Net, "Pinhole: opened TCP [%s]:%u via UPnP IPv6 firewall control "
                         "(id %s)", globalV6.c_str(), static_cast<unsigned>(port),
                    uniqueId.c_str());
        return PortMapResult::Ok;
    }
    if (refused || r == PortMapResult::Refused) {
        HE_LOG_WARN(Net, "Pinhole: the router refuses to open its IPv6 firewall for this "
                         "device");
        return PortMapResult::Refused;
    }
    return r;
}

void PortMapper::closePinhole(const PinholeHandle& handle) {
    switch (handle.method) {
    case PinholeHandle::Method::Pcp:
        pcpUnmap(handle.gateway, handle.clientAddress, handle.pcpNonce, handle.port);
        break;
    case PinholeHandle::Method::Upnp6fc:
        deletePinhole(handle.igd, handle.uniqueId);
        break;
    case PinholeHandle::Method::None:
        break;
    }
}

PortMapResult PortMapper::mapPort(std::uint16_t port, const std::string& description,
                                  MappingHandle& outHandle, PortMapping& outInfo) {
    outHandle = MappingHandle{};
    HE_LOG_INFO(Net, "Port mapping: trying to open TCP %u automatically",
                static_cast<unsigned>(port));

    // Sticky across every rung below: any one of them hearing an explicit "no"
    // changes the verdict, even if a later rung merely stays silent.
    bool refused = false;


    // UPnP first among the IPv4 mechanisms: broader support across consumer routers.
    IgdDevice igd;
    if (discover(igd, 3000) == PortMapResult::Ok)
    {
        const PortMapResult r = addMapping(igd, port, port, description, outInfo);
        refused = refused || r == PortMapResult::Refused;
        if (r == PortMapResult::Ok)
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

        // PCP for IPv4 too. A router can speak PCP without speaking NAT-PMP —
        // the version byte is what distinguishes them — so this is a genuinely
        // separate attempt rather than a retry of the same thing.
        const std::string lan = socketLocalAddress();
        if (!lan.empty())
        {
            PcpMapping pcp;
            const PortMapResult r = pcpMap(gateway, lan, port, port, 7200, pcp);
            refused = refused || r == PortMapResult::Refused;
            if (r == PortMapResult::Ok)
            {
                outHandle.method        = MappingHandle::Method::Pcp;
                outHandle.gateway       = gateway;
                outHandle.port          = port;
                outHandle.clientAddress = lan;
                outHandle.isIPv6        = false;
                std::memcpy(outHandle.pcpNonce, pcp.nonce, sizeof(outHandle.pcpNonce));

                outInfo.externalPort = pcp.externalPort ? pcp.externalPort : port;
                outInfo.internalPort = port;
                outInfo.internalHost = lan;
                outInfo.externalIp   = pcp.externalAddress;
                HE_LOG_INFO(Net, "Port mapping: succeeded via PCP (IPv4)");
                return PortMapResult::Ok;
            }
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

    // A refusal outranks everything else here. The router was reached, it
    // understood the request and it declined — saying "nothing answered" would
    // send the user looking for a problem that does not exist.
    if (refused)
    {
        HE_LOG_WARN(Net, "Port mapping: the router was reached but refuses to forward TCP %u "
                         "for this device. It has automatic port forwarding switched off — "
                         "on a FRITZ!Box that is a per-device permission, separate from the "
                         "global one.", static_cast<unsigned>(port));
        return PortMapResult::Refused;
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
    case MappingHandle::Method::Pcp:
        pcpUnmap(handle.gateway, handle.clientAddress, handle.pcpNonce, handle.port);
        break;
    // Not an error — most sessions never got a mapping in the first place.
    case MappingHandle::Method::None:
        HE_LOG_DEBUG(Net, "Port mapping: nothing to take down");
        break;
    }
}

} // namespace HE::Net
