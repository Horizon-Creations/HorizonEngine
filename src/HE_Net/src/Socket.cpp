#include "Net/Socket.h"

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "Ws2_32.lib")
#else
  #include <arpa/inet.h>
  #include <fcntl.h>
  #include <netdb.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <sys/select.h>
  #include <sys/socket.h>
  #include <sys/types.h>
  #include <unistd.h>
  #include <cerrno>
#endif

#include <cstring>
#include <mutex>
#include <string>

namespace HE::Net {
namespace {

// ─── Platform error normalisation ────────────────────────────────────────────

int lastError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool errIsWouldBlock(int e) {
#ifdef _WIN32
    return e == WSAEWOULDBLOCK;
#else
    return e == EAGAIN || e == EWOULDBLOCK;
#endif
}

bool errIsInProgress(int e) {
#ifdef _WIN32
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY;
#else
    return e == EINPROGRESS || e == EALREADY;
#endif
}

bool errIsInterrupted(int e) {
#ifdef _WIN32
    return e == WSAEINTR;
#else
    return e == EINTR;
#endif
}

bool errIsAlreadyConnected(int e) {
#ifdef _WIN32
    return e == WSAEISCONN;
#else
    return e == EISCONN;
#endif
}

} // namespace

// ─── Startup ─────────────────────────────────────────────────────────────────

bool socketSystemInit() {
    // Winsock needs a process-wide WSAStartup. Done once and never torn down:
    // WSACleanup at an arbitrary point would break any socket still open, and
    // the OS reclaims everything at process exit anyway.
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
#ifdef _WIN32
        WSADATA wsa{};
        ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#else
        ok = true;
#endif
    });
    return ok;
}

// ─── Lifetime ────────────────────────────────────────────────────────────────

SocketHandle socketCreateTcp() {
    if (!socketSystemInit()) return kInvalidSocket;

#ifdef _WIN32
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return kInvalidSocket;
    const auto h = static_cast<SocketHandle>(s);
#else
    int s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) return kInvalidSocket;
    const auto h = static_cast<SocketHandle>(s);
#endif

    // Non-blocking from birth — nothing in the engine may stall on a socket.
    if (!socketSetNonBlocking(h, true)) {
        socketClose(h);
        return kInvalidSocket;
    }
    socketSetNoDelay(h, true);
    return h;
}

void socketClose(SocketHandle h) {
    if (h == kInvalidSocket) return;
#ifdef _WIN32
    ::closesocket(static_cast<SOCKET>(h));
#else
    ::close(static_cast<int>(h));
#endif
}

// ─── Options ─────────────────────────────────────────────────────────────────

bool socketSetNonBlocking(SocketHandle h, bool nonBlocking) {
    if (h == kInvalidSocket) return false;
#ifdef _WIN32
    u_long mode = nonBlocking ? 1u : 0u;
    return ::ioctlsocket(static_cast<SOCKET>(h), FIONBIO, &mode) == 0;
#else
    const int fd = static_cast<int>(h);
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    flags = nonBlocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return ::fcntl(fd, F_SETFL, flags) == 0;
#endif
}

bool socketSetNoDelay(SocketHandle h, bool noDelay) {
    if (h == kInvalidSocket) return false;
    const int v = noDelay ? 1 : 0;
#ifdef _WIN32
    return ::setsockopt(static_cast<SOCKET>(h), IPPROTO_TCP, TCP_NODELAY,
                        reinterpret_cast<const char*>(&v), sizeof(v)) == 0;
#else
    return ::setsockopt(static_cast<int>(h), IPPROTO_TCP, TCP_NODELAY,
                        &v, sizeof(v)) == 0;
#endif
}

bool socketSetReuseAddr(SocketHandle h, bool reuse) {
    if (h == kInvalidSocket) return false;
    const int v = reuse ? 1 : 0;
#ifdef _WIN32
    // Deliberately NOT SO_REUSEADDR on Windows: there it allows two sockets to
    // bind the same port outright (hijacking), rather than just reusing a
    // TIME_WAIT port as on BSD/Linux. Windows already permits the rebind we
    // actually want, so this is a no-op there.
    (void)v;
    return true;
#else
    return ::setsockopt(static_cast<int>(h), SOL_SOCKET, SO_REUSEADDR,
                        &v, sizeof(v)) == 0;
#endif
}

// ─── Server ──────────────────────────────────────────────────────────────────

bool socketBindListen(SocketHandle h, std::uint16_t port, int backlog) {
    if (h == kInvalidSocket) return false;
    socketSetReuseAddr(h, true);

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

#ifdef _WIN32
    const SOCKET s = static_cast<SOCKET>(h);
#else
    const int s = static_cast<int>(h);
#endif
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;
    return ::listen(s, backlog) == 0;
}

std::uint16_t socketBoundPort(SocketHandle h) {
    if (h == kInvalidSocket) return 0;

    // sockaddr_storage, not sockaddr_in: a dual-stack listener is AF_INET6, and
    // reading it into the smaller v4 struct would truncate and report a wrong
    // port entirely.
    sockaddr_storage addr{};
#ifdef _WIN32
    int len = static_cast<int>(sizeof(addr));
    if (::getsockname(static_cast<SOCKET>(h),
                      reinterpret_cast<sockaddr*>(&addr), &len) != 0) return 0;
#else
    socklen_t len = sizeof(addr);
    if (::getsockname(static_cast<int>(h),
                      reinterpret_cast<sockaddr*>(&addr), &len) != 0) return 0;
#endif
    if (addr.ss_family == AF_INET6) {
        return ntohs(reinterpret_cast<sockaddr_in6*>(&addr)->sin6_port);
    }
    return ntohs(reinterpret_cast<sockaddr_in*>(&addr)->sin_port);
}

SocketResult socketAccept(SocketHandle listener, SocketHandle& outAccepted) {
    outAccepted = kInvalidSocket;
    if (listener == kInvalidSocket) return SocketResult::Error;

#ifdef _WIN32
    const SOCKET s = ::accept(static_cast<SOCKET>(listener), nullptr, nullptr);
    if (s == INVALID_SOCKET) {
        const int e = lastError();
        if (errIsWouldBlock(e) || errIsInterrupted(e)) return SocketResult::WouldBlock;
        return SocketResult::Error;
    }
    const auto h = static_cast<SocketHandle>(s);
#else
    const int s = ::accept(static_cast<int>(listener), nullptr, nullptr);
    if (s < 0) {
        const int e = lastError();
        if (errIsWouldBlock(e) || errIsInterrupted(e)) return SocketResult::WouldBlock;
        return SocketResult::Error;
    }
    const auto h = static_cast<SocketHandle>(s);
#endif

    socketSetNonBlocking(h, true);
    socketSetNoDelay(h, true);
    outAccepted = h;
    return SocketResult::Ok;
}

// ─── Client ──────────────────────────────────────────────────────────────────

SocketResult socketConnect(SocketHandle h, const std::string& host,
                           std::uint16_t port) {
    if (h == kInvalidSocket) return SocketResult::Error;
    if (!socketSystemInit())  return SocketResult::Error;

    // Resolve the host — accepts both literal IPv4 and DNS names.
    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    const std::string portStr = std::to_string(port);
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        return SocketResult::Error;
    }

#ifdef _WIN32
    const int rc = ::connect(static_cast<SOCKET>(h), res->ai_addr,
                             static_cast<int>(res->ai_addrlen));
#else
    const int rc = ::connect(static_cast<int>(h), res->ai_addr,
                             static_cast<socklen_t>(res->ai_addrlen));
#endif
    ::freeaddrinfo(res);

    if (rc == 0) return SocketResult::Ok;   // completed immediately (loopback)

    const int e = lastError();
    if (errIsInProgress(e) || errIsWouldBlock(e)) return SocketResult::WouldBlock;
    return SocketResult::Error;
}

SocketResult socketCreateTcpConnecting(const std::string& host, std::uint16_t port,
                                       SocketHandle& outSocket) {
    outSocket = kInvalidSocket;
    if (!socketSystemInit()) return SocketResult::Error;

    // AF_UNSPEC: let the resolver decide. A session directory records whichever
    // address it saw the host arrive from, which today is often IPv6 — an
    // AF_INET-only path could not reach those peers at all.
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    const std::string portStr = std::to_string(port);
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        return SocketResult::Error;
    }

    SocketResult result = SocketResult::Error;
    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
#ifdef _WIN32
        SOCKET s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) continue;
#else
        int s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s < 0) continue;
#endif
        const auto h = static_cast<SocketHandle>(s);
        if (!socketSetNonBlocking(h, true)) { socketClose(h); continue; }
        socketSetNoDelay(h, true);

#ifdef _WIN32
        const int rc = ::connect(static_cast<SOCKET>(h), ai->ai_addr,
                                 static_cast<int>(ai->ai_addrlen));
#else
        const int rc = ::connect(static_cast<int>(h), ai->ai_addr,
                                 static_cast<socklen_t>(ai->ai_addrlen));
#endif
        if (rc == 0) { outSocket = h; result = SocketResult::Ok; break; }

        const int e = lastError();
        if (errIsInProgress(e) || errIsWouldBlock(e)) {
            outSocket = h;
            result = SocketResult::WouldBlock;
            break;
        }
        // This candidate failed outright — try the next family/address.
        socketClose(h);
    }

    ::freeaddrinfo(res);
    return result;
}

// ─── Dual-stack listener ─────────────────────────────────────────────────────

SocketHandle socketCreateListenerDualStack(std::uint16_t port, int backlog) {
    if (!socketSystemInit()) return kInvalidSocket;

#ifdef _WIN32
    SOCKET s6 = ::socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    const bool have6 = (s6 != INVALID_SOCKET);
#else
    int s6 = ::socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    const bool have6 = (s6 >= 0);
#endif

    if (have6) {
        const auto h = static_cast<SocketHandle>(s6);

        // Clearing IPV6_V6ONLY makes one socket serve both families: IPv4 peers
        // show up as v4-mapped addresses. Windows and some BSDs default it to
        // ON, so it must be cleared explicitly.
        const int off = 0;
#ifdef _WIN32
        ::setsockopt(static_cast<SOCKET>(h), IPPROTO_IPV6, IPV6_V6ONLY,
                     reinterpret_cast<const char*>(&off), sizeof(off));
#else
        ::setsockopt(static_cast<int>(h), IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
#endif
        socketSetNonBlocking(h, true);
        socketSetNoDelay(h, true);
        socketSetReuseAddr(h, true);

        sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_addr   = in6addr_any;
        addr.sin6_port   = htons(port);

#ifdef _WIN32
        const bool bound = ::bind(static_cast<SOCKET>(h),
                                  reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0
                        && ::listen(static_cast<SOCKET>(h), backlog) == 0;
#else
        const bool bound = ::bind(static_cast<int>(h),
                                  reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0
                        && ::listen(static_cast<int>(h), backlog) == 0;
#endif
        if (bound) return h;
        socketClose(h);
    }

    // No usable IPv6 stack (or the bind failed) — fall back to IPv4 rather than
    // failing outright, so a host on a v4-only network still works.
    SocketHandle h4 = socketCreateTcp();
    if (h4 == kInvalidSocket) return kInvalidSocket;
    if (!socketBindListen(h4, port, backlog)) {
        socketClose(h4);
        return kInvalidSocket;
    }
    return h4;
}

SocketResult socketConnectPoll(SocketHandle h) {
    if (h == kInvalidSocket) return SocketResult::Error;

    // A pending non-blocking connect signals completion by becoming writable;
    // success vs failure is then read from SO_ERROR.
    fd_set writeSet, errSet;
    FD_ZERO(&writeSet);
    FD_ZERO(&errSet);
#ifdef _WIN32
    const SOCKET s = static_cast<SOCKET>(h);
    FD_SET(s, &writeSet);
    FD_SET(s, &errSet);
    const int nfds = 0;   // ignored by Winsock
#else
    const int s = static_cast<int>(h);
    FD_SET(s, &writeSet);
    FD_SET(s, &errSet);
    const int nfds = s + 1;
#endif
    timeval tv{};   // zero timeout — pure poll, never blocks the caller

    const int rc = ::select(nfds, nullptr, &writeSet, &errSet, &tv);
    if (rc < 0) {
        const int e = lastError();
        if (errIsInterrupted(e)) return SocketResult::WouldBlock;
        return SocketResult::Error;
    }
    if (rc == 0) return SocketResult::WouldBlock;   // still connecting

    int soErr = 0;
#ifdef _WIN32
    int len = static_cast<int>(sizeof(soErr));
    if (::getsockopt(s, SOL_SOCKET, SO_ERROR,
                     reinterpret_cast<char*>(&soErr), &len) != 0) {
        return SocketResult::Error;
    }
#else
    socklen_t len = sizeof(soErr);
    if (::getsockopt(s, SOL_SOCKET, SO_ERROR, &soErr, &len) != 0) {
        return SocketResult::Error;
    }
#endif
    if (soErr != 0 && !errIsAlreadyConnected(soErr)) return SocketResult::Error;
    return SocketResult::Ok;
}

// ─── I/O ─────────────────────────────────────────────────────────────────────

SocketResult socketSend(SocketHandle h, const std::uint8_t* data,
                        std::size_t len, std::size_t& outSent) {
    outSent = 0;
    if (h == kInvalidSocket) return SocketResult::Error;
    if (len == 0) return SocketResult::Ok;

#ifdef _WIN32
    const int n = ::send(static_cast<SOCKET>(h),
                         reinterpret_cast<const char*>(data),
                         static_cast<int>(len), 0);
#else
    // MSG_NOSIGNAL: writing to a closed peer must return EPIPE, not raise
    // SIGPIPE and kill the editor. macOS lacks it and uses SO_NOSIGPIPE, but
    // returning EPIPE is what we handle either way.
  #ifdef MSG_NOSIGNAL
    const ssize_t n = ::send(static_cast<int>(h), data, len, MSG_NOSIGNAL);
  #else
    const ssize_t n = ::send(static_cast<int>(h), data, len, 0);
  #endif
#endif

    if (n > 0) {
        outSent = static_cast<std::size_t>(n);
        return SocketResult::Ok;
    }
    const int e = lastError();
    if (errIsWouldBlock(e) || errIsInterrupted(e)) return SocketResult::WouldBlock;
    return SocketResult::Error;
}

bool socketWaitReadable(SocketHandle h, int timeoutMs) {
    if (h == kInvalidSocket) return false;

    fd_set readSet;
    FD_ZERO(&readSet);
#ifdef _WIN32
    const SOCKET s = static_cast<SOCKET>(h);
    FD_SET(s, &readSet);
    const int nfds = 0;   // ignored by Winsock
#else
    const int s = static_cast<int>(h);
    FD_SET(s, &readSet);
    const int nfds = s + 1;
#endif
    timeval tv{};
    tv.tv_sec  = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

    return ::select(nfds, &readSet, nullptr, nullptr, &tv) > 0;
}

SocketResult socketRecv(SocketHandle h, std::uint8_t* buf, std::size_t len,
                        std::size_t& outReceived) {
    outReceived = 0;
    if (h == kInvalidSocket) return SocketResult::Error;
    if (len == 0) return SocketResult::Ok;

#ifdef _WIN32
    const int n = ::recv(static_cast<SOCKET>(h), reinterpret_cast<char*>(buf),
                         static_cast<int>(len), 0);
#else
    const ssize_t n = ::recv(static_cast<int>(h), buf, len, 0);
#endif

    if (n > 0) {
        outReceived = static_cast<std::size_t>(n);
        return SocketResult::Ok;
    }
    if (n == 0) return SocketResult::Closed;   // orderly peer shutdown

    const int e = lastError();
    if (errIsWouldBlock(e) || errIsInterrupted(e)) return SocketResult::WouldBlock;
    return SocketResult::Error;
}

// ─── UDP ─────────────────────────────────────────────────────────────────────

SocketHandle socketCreateUdp() {
    if (!socketSystemInit()) return kInvalidSocket;

#ifdef _WIN32
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return kInvalidSocket;
#else
    int s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return kInvalidSocket;
#endif
    const auto h = static_cast<SocketHandle>(s);
    if (!socketSetNonBlocking(h, true)) {
        socketClose(h);
        return kInvalidSocket;
    }
    return h;
}

bool socketBindUdp(SocketHandle h, std::uint16_t port) {
    if (h == kInvalidSocket) return false;
    socketSetReuseAddr(h, true);

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

#ifdef _WIN32
    return ::bind(static_cast<SOCKET>(h),
                  reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
#else
    return ::bind(static_cast<int>(h),
                  reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
#endif
}

bool socketSetMulticastTtl(SocketHandle h, int ttl) {
    if (h == kInvalidSocket) return false;
#ifdef _WIN32
    const DWORD v = static_cast<DWORD>(ttl);
    return ::setsockopt(static_cast<SOCKET>(h), IPPROTO_IP, IP_MULTICAST_TTL,
                        reinterpret_cast<const char*>(&v), sizeof(v)) == 0;
#else
    // BSD/Linux expect an unsigned char here, not an int — passing sizeof(int)
    // makes the call fail on macOS.
    const unsigned char v = static_cast<unsigned char>(ttl);
    return ::setsockopt(static_cast<int>(h), IPPROTO_IP, IP_MULTICAST_TTL,
                        &v, sizeof(v)) == 0;
#endif
}

SocketResult socketSendTo(SocketHandle h, const std::uint8_t* data, std::size_t len,
                          const std::string& host, std::uint16_t port,
                          std::size_t& outSent) {
    outSent = 0;
    if (h == kInvalidSocket) return SocketResult::Error;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        return SocketResult::Error;
    }

#ifdef _WIN32
    const int n = ::sendto(static_cast<SOCKET>(h),
                           reinterpret_cast<const char*>(data),
                           static_cast<int>(len), 0,
                           reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
#else
    const ssize_t n = ::sendto(static_cast<int>(h), data, len, 0,
                               reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
#endif
    if (n > 0) {
        outSent = static_cast<std::size_t>(n);
        return SocketResult::Ok;
    }
    const int e = lastError();
    if (errIsWouldBlock(e) || errIsInterrupted(e)) return SocketResult::WouldBlock;
    return SocketResult::Error;
}

SocketResult socketRecvFrom(SocketHandle h, std::uint8_t* buf, std::size_t len,
                            std::size_t& outReceived, std::string& outFromHost,
                            std::uint16_t& outFromPort) {
    outReceived = 0;
    outFromHost.clear();
    outFromPort = 0;
    if (h == kInvalidSocket) return SocketResult::Error;

    sockaddr_in from{};
#ifdef _WIN32
    int fromLen = static_cast<int>(sizeof(from));
    const int n = ::recvfrom(static_cast<SOCKET>(h), reinterpret_cast<char*>(buf),
                             static_cast<int>(len), 0,
                             reinterpret_cast<sockaddr*>(&from), &fromLen);
#else
    socklen_t fromLen = sizeof(from);
    const ssize_t n = ::recvfrom(static_cast<int>(h), buf, len, 0,
                                 reinterpret_cast<sockaddr*>(&from), &fromLen);
#endif
    if (n > 0) {
        outReceived = static_cast<std::size_t>(n);
        char ip[INET_ADDRSTRLEN] = {};
        if (::inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip))) outFromHost = ip;
        outFromPort = ntohs(from.sin_port);
        return SocketResult::Ok;
    }
    const int e = lastError();
    if (errIsWouldBlock(e) || errIsInterrupted(e)) return SocketResult::WouldBlock;
    return SocketResult::Error;
}

// ─── Local address ───────────────────────────────────────────────────────────

std::string socketLocalAddress() {
    if (!socketSystemInit()) return {};

    SocketHandle h = socketCreateUdp();
    if (h == kInvalidSocket) return {};

    // Connecting a UDP socket sends nothing — it only asks the kernel to pick a
    // route, which then reveals the outbound interface via getsockname().
    sockaddr_in probe{};
    probe.sin_family = AF_INET;
    probe.sin_port   = htons(53);
    ::inet_pton(AF_INET, "8.8.8.8", &probe.sin_addr);

#ifdef _WIN32
    const int rc = ::connect(static_cast<SOCKET>(h),
                             reinterpret_cast<sockaddr*>(&probe), sizeof(probe));
#else
    const int rc = ::connect(static_cast<int>(h),
                             reinterpret_cast<sockaddr*>(&probe), sizeof(probe));
#endif
    if (rc != 0) { socketClose(h); return {}; }

    sockaddr_in local{};
#ifdef _WIN32
    int len = static_cast<int>(sizeof(local));
    const bool ok = ::getsockname(static_cast<SOCKET>(h),
                                  reinterpret_cast<sockaddr*>(&local), &len) == 0;
#else
    socklen_t len = sizeof(local);
    const bool ok = ::getsockname(static_cast<int>(h),
                                  reinterpret_cast<sockaddr*>(&local), &len) == 0;
#endif
    std::string out;
    if (ok) {
        char ip[INET_ADDRSTRLEN] = {};
        if (::inet_ntop(AF_INET, &local.sin_addr, ip, sizeof(ip))) out = ip;
    }
    socketClose(h);
    return out;
}

} // namespace HE::Net
