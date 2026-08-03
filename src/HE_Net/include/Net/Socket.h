#pragma once

// ─── HorizonNet Layer 0 — platform sockets ───────────────────────────────────
// Thin cross-platform TCP wrapper (Winsock on Windows, BSD sockets elsewhere).
// Deliberately minimal: no buffering, no framing, no policy — just the syscalls
// TcpTransport needs, with the platform differences (SOCKET vs int, closesocket
// vs close, WSAGetLastError vs errno, WSAEWOULDBLOCK vs EAGAIN) normalised away.
//
// Everything is non-blocking by design: the editor pumps this from its frame
// loop and must never stall on a slow peer. Callers distinguish "no data right
// now" (WouldBlock) from "peer went away" (Closed) from real failures (Error).
//
// This layer is also what the source-control HTTPS stack will build on later —
// hence it lives in HorizonNet rather than inside TcpTransport.

#include "Net/NetCommon.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace HE::Net {

#ifdef _WIN32
// Windows SOCKET is UINT_PTR, not int — narrowing it loses handles on x64.
using SocketHandle = std::uintptr_t;
inline constexpr SocketHandle kInvalidSocket = ~static_cast<SocketHandle>(0);
#else
using SocketHandle = int;
inline constexpr SocketHandle kInvalidSocket = static_cast<SocketHandle>(-1);
#endif

enum class SocketResult : std::uint8_t {
    Ok,
    WouldBlock,   // non-blocking op has nothing to do right now — not an error
    Closed,       // peer performed an orderly shutdown
    Error,        // genuine failure; the socket should be discarded
};

// Process-wide socket startup (WSAStartup on Windows, no-op elsewhere).
// Idempotent and thread-safe; safe to call from every socket entry point.
HE_NET_API bool socketSystemInit();

// ─── Lifetime ────────────────────────────────────────────────────────────────

// Create a non-blocking IPv4 TCP socket. Returns kInvalidSocket on failure.
//
// Prefer socketCreateTcpConnecting() for outbound links: the address family has
// to match the destination, and a session directory reports whatever address the
// host actually reached it from — which is frequently IPv6.
HE_NET_API SocketHandle socketCreateTcp();
HE_NET_API void         socketClose(SocketHandle h);

// ─── Options ─────────────────────────────────────────────────────────────────

HE_NET_API bool socketSetNonBlocking(SocketHandle h, bool nonBlocking);
// Disable Nagle. Editor traffic is many small messages where added latency is
// far worse than the extra packets.
HE_NET_API bool socketSetNoDelay(SocketHandle h, bool noDelay);
// Allow rebinding a port still in TIME_WAIT (server restarts, and back-to-back
// tests that would otherwise fail to bind).
HE_NET_API bool socketSetReuseAddr(SocketHandle h, bool reuse);

// ─── Server ──────────────────────────────────────────────────────────────────

// Bind + listen. Pass port 0 to let the OS pick a free one, then query it with
// socketBoundPort().
HE_NET_API bool socketBindListen(SocketHandle h, std::uint16_t port, int backlog = 16);

// Create a listening socket that accepts BOTH IPv4 and IPv6 peers: an AF_INET6
// socket with IPV6_V6ONLY cleared, so IPv4 clients arrive as v4-mapped
// addresses. Falls back to IPv4-only where IPv6 is unavailable.
//
// This matters because a host published in the session directory is reached at
// whichever address the directory observed — and an IPv4-only listener would
// simply be unreachable for half of them.
HE_NET_API SocketHandle socketCreateListenerDualStack(std::uint16_t port, int backlog = 16);
// Actual bound port (useful after binding to 0). Returns 0 on failure.
HE_NET_API std::uint16_t socketBoundPort(SocketHandle h);
// Accept one pending connection. WouldBlock when none is queued. The accepted
// socket is returned non-blocking with Nagle disabled.
HE_NET_API SocketResult socketAccept(SocketHandle listener, SocketHandle& outAccepted);

// ─── Client ──────────────────────────────────────────────────────────────────

// Resolve `host` (IPv4 literal, IPv6 literal, or DNS name), create a socket of
// the matching family, and start a non-blocking connect. This is the correct
// entry point for outbound links — socketCreateTcp() + socketConnect() can only
// ever reach IPv4 peers.
//
// Returns Ok (connected immediately, typical on loopback), WouldBlock (in
// progress — poll socketConnectPoll()), or Error. `outSocket` is kInvalidSocket
// unless the call returns Ok or WouldBlock.
HE_NET_API SocketResult socketCreateTcpConnecting(const std::string& host,
                                                  std::uint16_t port,
                                                  SocketHandle& outSocket);

// Start a non-blocking connect on an existing IPv4 socket.
HE_NET_API SocketResult socketConnect(SocketHandle h, const std::string& host,
                                      std::uint16_t port);
// Resolve a pending connect: Ok when established, WouldBlock while still
// pending, Error when it failed.
HE_NET_API SocketResult socketConnectPoll(SocketHandle h);

// ─── I/O ─────────────────────────────────────────────────────────────────────

// Send as much as the kernel accepts. `outSent` may be < len (partial write) —
// callers must keep the remainder queued.
HE_NET_API SocketResult socketSend(SocketHandle h, const std::uint8_t* data,
                                   std::size_t len, std::size_t& outSent);
// Read available bytes. Closed means the peer shut down cleanly.
HE_NET_API SocketResult socketRecv(SocketHandle h, std::uint8_t* buf,
                                   std::size_t len, std::size_t& outReceived);

// Block until the socket is readable, or the timeout elapses. Only for one-shot
// request/response exchanges on a worker thread (SSDP discovery, router SOAP
// calls) — never on the transport path the editor frame loop pumps.
HE_NET_API bool socketWaitReadable(SocketHandle h, int timeoutMs);

// ─── UDP ─────────────────────────────────────────────────────────────────────
// Needed for router port mapping: SSDP discovery is multicast UDP, and NAT-PMP
// is a small binary UDP protocol.

HE_NET_API SocketHandle socketCreateUdp();
// Bind to a local port (0 = any). Required before receiving.
HE_NET_API bool         socketBindUdp(SocketHandle h, std::uint16_t port);
// Allow sending to multicast groups (SSDP's 239.255.255.250).
HE_NET_API bool         socketSetMulticastTtl(SocketHandle h, int ttl);

HE_NET_API SocketResult socketSendTo(SocketHandle h, const std::uint8_t* data,
                                     std::size_t len, const std::string& host,
                                     std::uint16_t port, std::size_t& outSent);
// `outFromHost`/`outFromPort` identify the responder — SSDP replies arrive from
// the router's own address, which is how it is located.
HE_NET_API SocketResult socketRecvFrom(SocketHandle h, std::uint8_t* buf,
                                       std::size_t len, std::size_t& outReceived,
                                       std::string& outFromHost,
                                       std::uint16_t& outFromPort);

// ─── Local address ───────────────────────────────────────────────────────────

// The LAN address of the interface that would be used to reach the internet.
// Determined by "connecting" a UDP socket to a public address — this sends no
// packets, it only makes the kernel pick a route — then reading the socket's
// local end. Far more reliable than enumerating interfaces and guessing which
// one matters on a machine with VPNs, VMs or several NICs.
// Returns an empty string on failure.
HE_NET_API std::string socketLocalAddress();

// The default gateway — i.e. the router. Needed by NAT-PMP, which talks to it
// directly instead of discovering it the way UPnP's SSDP does.
//
// Read from the OS routing table rather than guessed from the subnet: assuming
// x.y.z.1 is right often enough to look correct and wrong often enough to be a
// support nightmare. Empty when there is no default route.
HE_NET_API std::string socketDefaultGateway();

} // namespace HE::Net
