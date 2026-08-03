#pragma once

// ─── HorizonNet — router port mapping (UPnP IGD) ─────────────────────────────
// Asks the LAN router to forward an external port to this machine, so a
// collaboration host is reachable from the internet without the user editing
// router settings by hand.
//
// Flow:
//   1. SSDP M-SEARCH (multicast UDP to 239.255.255.250:1900) to find an
//      InternetGatewayDevice — this self-discovers the router, so no default
//      gateway lookup is needed.
//   2. HTTP GET the device description XML from the LOCATION header.
//   3. Locate the WANIPConnection / WANPPPConnection service and its control URL.
//   4. SOAP POST AddPortMapping (and GetExternalIPAddress / DeletePortMapping).
//
// ⚠ Honest limits — mapping is best-effort and *will* fail for real users:
//   • Many routers ship with UPnP disabled (it is a known attack surface).
//   • Behind **CGNAT** there is no forwardable port at all — not via UPnP, not
//     manually. Only a relay helps. This is common on mobile and some ISPs.
//   • Corporate networks and double-NAT setups generally block it.
// So a failed mapping is a normal outcome, not a bug: the caller must fall back
// (IPv6 direct → UPnP → manual forwarding → relay) and, above all, must verify
// external reachability before publishing an endpoint to the session directory.
//
// ⚠ macOS ≥ Sequoia — Local Network privacy. Discovery here fails with
// EHOSTUNREACH ("No route to host") on *every* LAN and multicast destination
// unless the app holds the Local Network permission, while internet traffic
// continues to work normally. That asymmetry makes it look like a routing or
// socket bug rather than a missing entitlement. Requirements:
//   • the bundle must declare NSLocalNetworkUsageDescription (the editor's is
//     written by scripts/package_macos.sh), and
//   • the user must approve the prompt (System Settings ▸ Privacy & Security ▸
//     Local Network).
// A bare CLI binary launched from a terminal generally does not hold it, so
// discover() returning NoRouterFound there says nothing about the router.
//
// Every call blocks on network I/O and belongs on a worker thread.

#include "Net/NetCommon.h"

#include <cstdint>
#include <string>
#include <vector>

namespace HE::Net {

enum class PortMapResult : std::uint8_t {
    Ok,
    NoRouterFound,      // no IGD answered the SSDP search
    NoServiceFound,     // router replied but exposes no WAN connection service
    RequestFailed,      // SOAP call failed or was refused (UPnP often disabled)
    NotSupported,       // could not determine a local address to map to
};

struct PortMapping {
    std::uint16_t externalPort = 0;
    std::uint16_t internalPort = 0;
    std::string   internalHost;     // LAN address the mapping points at
    std::string   externalIp;       // router's WAN address, when it reports one
    std::string   controlUrl;       // needed to remove the mapping later
    std::string   serviceType;
};

// A discovered InternetGatewayDevice.
struct IgdDevice {
    std::string location;      // device description URL (from SSDP LOCATION)
    std::string controlUrl;    // absolute control endpoint
    std::string serviceType;   // WANIPConnection:1 / WANPPPConnection:1 / …
};

class HE_NET_API PortMapper {
public:
    // Find an IGD on the LAN. `timeoutMs` bounds the SSDP wait.
    static PortMapResult discover(IgdDevice& out, int timeoutMs = 3000);

    // Map externalPort → internalPort on this machine. Pass internalHost empty to
    // use socketLocalAddress(). leaseSeconds 0 means "permanent" — many routers
    // reject non-zero leases, so 0 is the compatible default.
    static PortMapResult addMapping(const IgdDevice& igd,
                                    std::uint16_t externalPort,
                                    std::uint16_t internalPort,
                                    const std::string& description,
                                    PortMapping& out,
                                    const std::string& internalHost = {},
                                    std::uint32_t leaseSeconds = 0);

    static PortMapResult removeMapping(const IgdDevice& igd, std::uint16_t externalPort);

    // The router's WAN-side address. Note this is *not* proof of reachability:
    // behind CGNAT it returns a private address (100.64.0.0/10 or RFC1918),
    // which is a strong hint that no port mapping can ever work.
    static PortMapResult externalIp(const IgdDevice& igd, std::string& out);

    // True when `ip` is in a range that cannot be reached from the internet.
    //
    // ⚠ One-way test: true is conclusive, false proves nothing. It compares
    // address ranges, so it catches ISP-level CGNAT and RFC1918 WAN sides — but
    // a corporate or campus network whose gateway holds a genuine public address
    // and drops inbound connections looks exactly like a directly reachable
    // home router here. Never read a false as "this host is reachable"; only the
    // directory's connect-back probe answers that.
    static bool isPrivateOrCgnat(const std::string& ip);

    // ── NAT-PMP (RFC 6886) ───────────────────────────────────────────────────
    // The other protocol routers speak for this. Where UPnP is SSDP discovery
    // plus SOAP over HTTP, NAT-PMP is a 12-byte datagram to the default gateway
    // on port 5351 — no discovery step, no XML.
    //
    // Worth having as a fallback because routers speak one or the other, not
    // both: Apple's AirPort and Time Capsule historically offered only NAT-PMP,
    // so a UPnP-only client reports "no router found" at a router that would
    // have obliged immediately.
    static PortMapResult natPmpExternalIp(const std::string& gateway, std::string& out,
                                          int timeoutMs = 1500);
    static PortMapResult natPmpAddMapping(const std::string& gateway,
                                          std::uint16_t externalPort,
                                          std::uint16_t internalPort,
                                          std::uint32_t lifetimeSeconds,
                                          PortMapping& out,
                                          int timeoutMs = 1500);
    static PortMapResult natPmpRemoveMapping(const std::string& gateway,
                                             std::uint16_t internalPort,
                                             int timeoutMs = 1500);

    // ── One call that tries everything ───────────────────────────────────────
    // UPnP first (broadest support), NAT-PMP second. Records which method won so
    // the mapping can be taken down the same way it was put up.
    struct MappingHandle
    {
        enum class Method : std::uint8_t { None, Upnp, NatPmp };
        Method        method = Method::None;
        IgdDevice     igd;        // Upnp
        std::string   gateway;    // NatPmp
        std::uint16_t port = 0;
    };

    static PortMapResult mapPort(std::uint16_t port, const std::string& description,
                                 MappingHandle& outHandle, PortMapping& outInfo);
    static void          unmapPort(const MappingHandle& handle);

    // ── Pure helpers, exposed for testing without a live router ──
    static std::vector<std::uint8_t> buildNatPmpRequest(std::uint8_t opcode,
                                                        std::uint16_t internalPort,
                                                        std::uint16_t externalPort,
                                                        std::uint32_t lifetimeSeconds);
    // Return false on a malformed frame; `outResultCode` carries the router's own
    // verdict when the frame parsed but the request was refused.
    static bool parseNatPmpMapResponse(const std::uint8_t* data, std::size_t len,
                                       std::uint16_t& outInternalPort,
                                       std::uint16_t& outExternalPort,
                                       std::uint32_t& outLifetime,
                                       std::uint16_t& outResultCode);
    static bool parseNatPmpAddressResponse(const std::uint8_t* data, std::size_t len,
                                           std::string& outIp,
                                           std::uint16_t& outResultCode);
    static std::string  buildSsdpSearch();
    static std::string  parseSsdpLocation(const std::string& ssdpResponse);
    static bool         parseDeviceDescription(const std::string& xml,
                                               const std::string& baseUrl,
                                               IgdDevice& out);
    static std::string  buildSoapBody(const std::string& serviceType,
                                      const std::string& action,
                                      const std::vector<std::pair<std::string, std::string>>& args);
    static std::string  extractXmlValue(const std::string& xml, const std::string& tag);
    static std::string  resolveUrl(const std::string& baseUrl, const std::string& relative);
};

} // namespace HE::Net
