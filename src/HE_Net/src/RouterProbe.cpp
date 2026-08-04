#include "Net/RouterProbe.h"

#include "Net/HttpsClient.h"
#include "Net/PortMapper.h"
#include "Net/Socket.h"

namespace HE::Net {

namespace {

// The SSDP wait. Deliberately shorter than PortMapper's own 3000 ms default:
// this runs at every editor start and its thread is joined on shutdown, so a
// quit two seconds after launch must not sit waiting for a router that is never
// going to answer. A router that does answer replies in tens of milliseconds —
// the long tail is entirely "nothing is there", which is what we are timing out.
constexpr int kDiscoverTimeoutMs = 1500;
constexpr int kNatPmpTimeoutMs   = 1000;

bool cancelled(const std::atomic<bool>* cancel)
{
	return cancel && cancel->load(std::memory_order_acquire);
}

} // namespace

RouterProbe probeRouter(const std::atomic<bool>* cancel)
{
	RouterProbe p;
	auto note = [&p](const std::string& line) { p.detail += line + "\n"; };

	// Free, and it decides whether anything below can mean anything: on macOS a
	// missing Local Network permission makes every discovery fail as though no
	// router existed, so reporting "no router" there would send the user to
	// their router's web interface for a problem that lives in System Settings.
	p.localNetworkBlocked = socketLocalNetworkBlocked();
	p.gatewayV4           = socketDefaultGateway();
	p.localAddress        = socketLocalAddress();
	p.globalIPv6          = socketGlobalIPv6Address();
	p.httpsAvailable      = HE::Net::httpsAvailable();

	note(p.gatewayV4.empty() ? "no IPv4 default gateway"
	                         : ("gateway: " + p.gatewayV4));
	note(p.localAddress.empty() ? "no local address" : ("local address: " + p.localAddress));
	note(p.globalIPv6.empty() ? "no global IPv6 address"
	                          : ("global IPv6: " + p.globalIPv6));
	note(p.httpsAvailable ? "HTTPS backend available"
	                      : "no HTTPS backend — the session directory is unreachable");

	if (p.localNetworkBlocked)
	{
		// Stop here rather than spend seconds on discovery that cannot succeed.
		note("local network traffic is blocked for this application — nothing left "
		     "the machine, so the router was never asked");
		return p;
	}
	if (cancelled(cancel)) { p.cancelled = true; return p; }

	// ── UPnP: SSDP discovery + the device description (both read-only) ───────
	IgdDevice igd;
	const PortMapResult disc = PortMapper::discover(igd, kDiscoverTimeoutMs);
	if (disc == PortMapResult::Ok)
	{
		p.upnpFound      = true;
		p.upnpService    = igd.serviceType;
		p.upnpV6Firewall = !igd.v6fwControlUrl.empty();
		note("UPnP: gateway found, service " + igd.serviceType);
		if (p.upnpV6Firewall) note("UPnP: IPv6 firewall control available (pinholes)");
	}
	else if (disc == PortMapResult::NoServiceFound)
	{
		// The distinction matters: a device answered, so the LAN and the
		// permission are fine — UPnP is simply switched off on the router.
		note("UPnP: a device answered but exposes no WAN connection service "
		     "(UPnP port forwarding is probably disabled on the router)");
	}
	else
	{
		note("UPnP: no gateway answered the search");
	}

	if (cancelled(cancel)) { p.cancelled = true; return p; }

	// ── NAT-PMP: the address opcode only ─────────────────────────────────────
	// This is a query — it asks the router for its WAN address and creates
	// nothing. Routers speak one protocol or the other (Apple's historically
	// only this one), so a UPnP miss above is not the end of the story.
	if (!p.gatewayV4.empty())
	{
		std::string wan;
		if (PortMapper::natPmpExternalIp(p.gatewayV4, wan, kNatPmpTimeoutMs) == PortMapResult::Ok)
		{
			p.natPmpFound = true;
			p.externalIp  = wan;
			note("NAT-PMP: gateway answered, WAN address " + wan);
		}
		else
		{
			note("NAT-PMP: gateway did not answer on port 5351");
		}
	}

	if (cancelled(cancel)) { p.cancelled = true; return p; }

	// The UPnP WAN address, when NAT-PMP did not already supply one. Asked last
	// because it is another SOAP round trip and only refines the verdict.
	if (p.externalIp.empty() && p.upnpFound)
	{
		std::string wan;
		if (PortMapper::externalIp(igd, wan) == PortMapResult::Ok && !wan.empty())
		{
			p.externalIp = wan;
			note("UPnP: WAN address " + wan);
		}
	}

	if (!p.externalIp.empty() && PortMapper::isPrivateOrCgnat(p.externalIp))
	{
		// A private WAN address means a second NAT above this router (carrier-
		// grade NAT). Forwarding a port on the local router then succeeds and
		// still nothing arrives, which is the most confusing failure of the lot.
		p.carrierNat = true;
		note("the router's WAN address is itself private — carrier-grade NAT is in "
		     "use, so a port forward on this router cannot be reached from outside");
	}

	return p;
}

} // namespace HE::Net
