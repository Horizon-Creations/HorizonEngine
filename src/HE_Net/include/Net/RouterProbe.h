#pragma once

// ─── Read-only collaboration-readiness check ─────────────────────────────────
// Answers "would hosting a session work on this machine?" WITHOUT hosting one:
// it discovers the router and asks it what it is, but never adds a mapping,
// opens a pinhole or registers anything with the session directory.
//
// That read-only rule is the whole design constraint. A startup probe that
// created port mappings would leave state on the user's router every time the
// editor launched, so this uses only the query half of each protocol: SSDP
// discovery + the device description for UPnP, and NAT-PMP's address opcode
// (which asks for the WAN address and changes nothing) for the other.
//
// What it therefore CANNOT tell you: whether an inbound connection actually
// arrives. Only the directory's connect-back probe answers that, and that needs
// a live session. A green result here means "nothing known is in the way".
//
// Same shape as HE::hccg::ToolchainProbe / HE::Sc::GitProbe: a plain aggregate
// that defaults to "nothing found", a `detail` string for a details view, and a
// blocking function meant for a worker thread.

#include "Net/NetCommon.h"

#include <atomic>
#include <string>

namespace HE::Net {

struct RouterProbe {
	// ── The OS layer ─────────────────────────────────────────────────────────
	// True when this process cannot even reach its own gateway — on macOS that
	// is the Local Network privacy permission, and no router setting fixes it.
	// Checked first because every result below is meaningless when it is true.
	bool        localNetworkBlocked = false;

	std::string gatewayV4;              // default route, empty when there is none
	std::string localAddress;           // our LAN address on that route

	// ── IPv4 port forwarding ─────────────────────────────────────────────────
	bool        upnpFound   = false;    // an IGD answered SSDP and exposes a WAN service
	std::string upnpService;            // WANIPConnection:1 / WANPPPConnection:1 / …
	bool        upnpV6Firewall = false; // IGDv2 IPv6 firewall control (pinholes)
	bool        natPmpFound = false;    // the gateway answered NAT-PMP / PCP on 5351

	// The router's WAN address, when either protocol reported one. Empty is not
	// a failure — plenty of routers map ports happily without disclosing it.
	std::string externalIp;
	// externalIp is in a range the internet cannot route back to (CGNAT/RFC1918).
	// One-way: true is conclusive, false proves nothing (see isPrivateOrCgnat).
	bool        carrierNat = false;

	// ── IPv6 ─────────────────────────────────────────────────────────────────
	// A global IPv6 address means no NAT at all — only the router's firewall
	// stands in the way, which is a different (and often easier) problem.
	std::string globalIPv6;

	// ── Build capability ─────────────────────────────────────────────────────
	// Without an HTTPS backend the session directory is unreachable, so joining
	// by session ID cannot work however good the router is.
	bool        httpsAvailable = false;

	// The probe log — what was tried and what answered. Never contains a token
	// or a session id; safe to show verbatim.
	std::string detail;

	// True when the probe was cut short (shutdown). The fields hold whatever had
	// been determined by then, so a cancelled probe must not be shown as a verdict.
	bool        cancelled = false;

	// Is there a working path for someone to reach a session hosted here?
	// Either the router will forward IPv4, or we hold a global IPv6 address.
	bool portForwardingAvailable() const { return upnpFound || natPmpFound; }
	bool ready() const
	{
		return !localNetworkBlocked && httpsAvailable && !carrierNat &&
		       (portForwardingAvailable() || !globalIPv6.empty());
	}
};

// Run the check. Blocks on network I/O for up to a few seconds — call it on a
// worker thread, never on the frame thread.
//
// `cancel` is polled between stages so a probe in flight does not hold up
// shutdown: when it flips, the remaining stages are skipped and the result comes
// back with `cancelled = true`. Pass null when there is nothing to cancel for.
HE_NET_API RouterProbe probeRouter(const std::atomic<bool>* cancel = nullptr);

} // namespace HE::Net
