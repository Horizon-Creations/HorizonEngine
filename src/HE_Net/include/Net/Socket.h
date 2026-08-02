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

// Create a non-blocking TCP socket. Returns kInvalidSocket on failure.
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
// Actual bound port (useful after binding to 0). Returns 0 on failure.
HE_NET_API std::uint16_t socketBoundPort(SocketHandle h);
// Accept one pending connection. WouldBlock when none is queued. The accepted
// socket is returned non-blocking with Nagle disabled.
HE_NET_API SocketResult socketAccept(SocketHandle listener, SocketHandle& outAccepted);

// ─── Client ──────────────────────────────────────────────────────────────────

// Start a non-blocking connect. Ok means it completed immediately (typical on
// loopback); WouldBlock means it is in progress — poll socketConnectPoll().
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

} // namespace HE::Net
