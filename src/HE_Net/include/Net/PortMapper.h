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
    // The request never left this machine: the OS refused local-network traffic.
    // Kept distinct from NoRouterFound because the remedy is entirely different —
    // a permission has to be granted, and changing router settings does nothing.
    // On macOS this is the Local Network privacy control, which reports itself as
    // EHOSTUNREACH for the very gateway all internet traffic flows through.
    LocalNetworkBlocked,
    // The router received the request and said no. Distinct from every failure
    // above, all of which mean the request did not get a considered answer.
    //
    // Worth its own value because the remedy is the opposite one: nothing is
    // broken and nothing needs installing — a setting has to be turned on. A
    // FritzBox is the common case and refuses with UPnP 606 / PCP NOT_AUTHORIZED
    // until "selbstandige Portfreigaben" is enabled FOR THAT DEVICE, which is a
    // separate per-device checkbox from the global one people usually find.
    Refused,
};

// Which transport protocol a mapping or pinhole is for. Collaboration hosts
// map TCP; a game host (UdpTransport) maps UDP. A router treats the two as
// unrelated entries, so the protocol is part of the mapping's identity and
// travels in the handles below — taking a UDP mapping down "as TCP" removes
// nothing.
enum class Protocol : std::uint8_t { Tcp, Udp };

inline const char* protocolName(Protocol p) { return p == Protocol::Udp ? "UDP" : "TCP"; }
// IANA protocol number, as PCP and the UPnP IPv6 firewall service spell it.
inline std::uint8_t protocolIanaNumber(Protocol p) { return p == Protocol::Udp ? 17 : 6; }

struct PortMapping {
    Protocol      protocol     = Protocol::Tcp;
    std::uint16_t externalPort = 0;
    std::uint16_t internalPort = 0;
    std::string   internalHost;     // LAN address the mapping points at
    std::string   externalIp;       // router's WAN address, when it reports one
    std::string   controlUrl;       // needed to remove the mapping later
    std::string   serviceType;
    // The lease the router actually granted, which may be shorter than the one
    // asked for. 0 = permanent (UPnP routers that only take permanent leases).
    std::uint32_t leaseSeconds = 0;
};

// A discovered InternetGatewayDevice.
struct IgdDevice {
    std::string location;      // device description URL (from SSDP LOCATION)
    std::string controlUrl;    // absolute control endpoint
    std::string serviceType;   // WANIPConnection:1 / WANPPPConnection:1 / …

    // IPv6 firewall control (IGDv2), when the gateway offers it. This is how a
    // FRITZ!Box opens IPv6 — its PCP responder refuses pinholes even with the
    // device permission set, but AddPinhole on this service works. Empty when
    // the device description lists no such service.
    std::string v6fwControlUrl;
    std::string v6fwServiceType;
};

class HE_NET_API PortMapper {
public:
    // The lease every mechanism asks for. Long enough that renewing it is rare,
    // short enough that an entry left behind by a crash is gone by the next day
    // rather than sitting in the router's table forever. A session that outlives
    // it has to renew — see renewMapping.
    static constexpr std::uint32_t kLeaseSeconds = 7200;

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
                                    std::uint32_t leaseSeconds = 0,
                                    Protocol protocol = Protocol::Tcp);

    static PortMapResult removeMapping(const IgdDevice& igd, std::uint16_t externalPort,
                                       Protocol protocol = Protocol::Tcp);

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
                                          int timeoutMs = 1500,
                                          Protocol protocol = Protocol::Tcp);
    static PortMapResult natPmpRemoveMapping(const std::string& gateway,
                                             std::uint16_t internalPort,
                                             int timeoutMs = 1500,
                                             Protocol protocol = Protocol::Tcp);
    // What a NAT-PMP result code means for the caller. Code 2 is "Not
    // Authorized/Refused" — the router speaks NAT-PMP and the user switched
    // mapping off — which is the same verdict as UPnP 606 and PCP 2 and must not
    // be reported as the router being absent.
    static PortMapResult natPmpResultCode(std::uint16_t code);

    // ── PCP (RFC 6887) ───────────────────────────────────────────────────────
    // The successor to NAT-PMP, on the same port 5351 and deliberately
    // distinguishable by its version byte (2, where NAT-PMP is 0) so a router
    // that only speaks the older one answers UNSUPP_VERSION rather than
    // misreading the request.
    //
    // ⚠ This is the ONLY mechanism here that can help an IPv6 host, which makes
    // it the important rung rather than a third fallback. With IPv6 there is no
    // NAT: a machine holding a global address is already addressable, and the
    // only obstacle is the router's firewall. UPnP AddPortMapping and NAT-PMP
    // are both defined purely in terms of IPv4 address translation and have
    // nothing to say about a firewall pinhole — so on a native-IPv6 connection
    // they can fail forever without that meaning anything about reachability.
    //
    // PCP also carries the client's own address in the request, which is what
    // lets one message ask for either an IPv4 mapping or an IPv6 pinhole.
    struct PcpMapping
    {
        std::uint8_t  nonce[12] = {};   // must be replayed to delete the mapping
        std::uint16_t externalPort = 0;
        std::string   externalAddress;
        std::uint32_t lifetimeSeconds = 0;
    };

    // `clientAddress` is this machine's own address on the interface facing the
    // router — the LAN IPv4 for a mapping, the global IPv6 for a pinhole.
    // `renewNonce` renews an existing mapping: RFC 6887 identifies it by the
    // nonce, so a fresh one would ask for a second mapping instead.
    static PortMapResult pcpMap(const std::string& gateway,
                                const std::string& clientAddress,
                                std::uint16_t internalPort,
                                std::uint16_t suggestedExternalPort,
                                std::uint32_t lifetimeSeconds,
                                PcpMapping& out,
                                int timeoutMs = 1500,
                                Protocol protocol = Protocol::Tcp,
                                const std::uint8_t* renewNonce = nullptr);
    // Lifetime 0 removes it. The nonce from the original grant identifies which.
    static PortMapResult pcpUnmap(const std::string& gateway,
                                  const std::string& clientAddress,
                                  const std::uint8_t nonce[12],
                                  std::uint16_t internalPort,
                                  int timeoutMs = 1500,
                                  Protocol protocol = Protocol::Tcp);

    // ── One call that tries everything ───────────────────────────────────────
    // UPnP first (broadest support), NAT-PMP second. Records which method won so
    // the mapping can be taken down the same way it was put up.
    struct MappingHandle
    {
        enum class Method : std::uint8_t { None, Upnp, NatPmp, Pcp };
        Method        method = Method::None;
        Protocol      protocol = Protocol::Tcp;
        IgdDevice     igd;        // Upnp
        std::string   gateway;    // NatPmp / Pcp
        std::uint16_t port = 0;
        // What the router granted, so a renewal asks for the same entry again.
        std::uint16_t externalPort = 0;
        std::uint32_t leaseSeconds = 0;   // 0 = permanent, nothing to renew
        std::string   description;        // Upnp — re-sent on renewal
        // Pcp — needed to take the mapping down again, and the address family
        // that was opened.
        std::uint8_t  pcpNonce[12] = {};
        std::string   clientAddress;
        bool          isIPv6 = false;
    };

    static PortMapResult mapPort(std::uint16_t port, const std::string& description,
                                 MappingHandle& outHandle, PortMapping& outInfo,
                                 Protocol protocol = Protocol::Tcp);
    static void          unmapPort(const MappingHandle& handle);
    // Re-requests the mapping the handle describes, the same way it was put up.
    // Every lease here is finite, so a host that outlives it loses its forward
    // without a word from the router; RFC 6886 has the client renew at half the
    // granted lifetime. Blocks on network I/O like everything else here.
    static PortMapResult renewMapping(const MappingHandle& handle);

    // ── IPv6 firewall pinhole ────────────────────────────────────────────────
    // Deliberately separate from mapPort: a pinhole and an IPv4 mapping are not
    // alternatives but complements. The pinhole makes the advertised IPv6
    // address actually accept connections; the IPv4 mapping is what guests
    // without IPv6 use. A host wants BOTH, so conflating them into one ladder
    // would leave one class of guest stranded whichever way it resolved.
    struct PinholeHandle
    {
        enum class Method : std::uint8_t { None, Pcp, Upnp6fc };
        Method        method = Method::None;
        Protocol      protocol = Protocol::Tcp;
        // Pcp
        std::string   gateway;          // IPv6, scope included
        std::string   clientAddress;
        std::uint8_t  pcpNonce[12] = {};
        std::uint16_t port = 0;
        // Upnp6fc
        IgdDevice     igd;
        std::string   uniqueId;         // the router's name for the pinhole
    };

    // Open `port` (TCP or UDP) on the router's IPv6 firewall for `globalV6`
    // (this machine's address). Tries PCP first (the standards path), then UPnP
    // WANIPv6FirewallControl (the FRITZ!Box path). `igd` may carry a device
    // found by an earlier discover() to skip a second SSDP round; pass an empty
    // one to let this discover on its own.
    static PortMapResult openPinhole(const std::string& globalV6, std::uint16_t port,
                                     PinholeHandle& out, const IgdDevice& igd = {},
                                     Protocol protocol = Protocol::Tcp);
    static void          closePinhole(const PinholeHandle& handle);

    // UPnP WANIPv6FirewallControl primitives (IGDv2).
    static PortMapResult addPinhole(const IgdDevice& igd, const std::string& internalClient,
                                    std::uint16_t port, std::uint32_t leaseSeconds,
                                    std::string& outUniqueId,
                                    Protocol protocol = Protocol::Tcp);
    static PortMapResult deletePinhole(const IgdDevice& igd, const std::string& uniqueId);

    // ── Pure helpers, exposed for testing without a live router ──
    // PCP wire format, exposed for testing without a router.
    static std::vector<std::uint8_t> buildPcpMapRequest(const std::string& clientAddress,
                                                        const std::uint8_t nonce[12],
                                                        std::uint16_t internalPort,
                                                        std::uint16_t suggestedExternalPort,
                                                        std::uint32_t lifetimeSeconds,
                                                        Protocol protocol = Protocol::Tcp);
    static bool parsePcpMapResponse(const std::uint8_t* data, std::size_t len,
                                    PcpMapping& out, std::uint8_t& outResultCode);

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
