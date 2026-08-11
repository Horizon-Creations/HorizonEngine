#pragma once
#include "Net/NetCommon.h"
#include "Net/Socket.h"
#include <cstdint>
#include <string>
#include <vector>

// Finding a session on the local network, when the internet cannot reach it.
//
// A session is normally found through the session directory: the host publishes
// {sessionId, port}, the website records the public address it was contacted
// from, a guest looks it up. That whole chain assumes the host is reachable FROM
// THE OUTSIDE — and behind carrier-grade NAT, a locked-down company router, or
// simply a router that refuses to forward, it is not. Two people sitting in the
// same room with the same Wi-Fi were told to use a relay that does not exist.
//
// On one network none of that is needed: the host says "I am here" into the
// local segment every couple of seconds, and anyone listening builds a list.
//
// TWO THINGS THIS DELIBERATELY DOES NOT DO:
//
//   * The join code is NEVER announced. It is the entire security of a session
//     — with it, anyone joins. A LAN is not a trusted place (shared flats,
//     offices, a café's Wi-Fi), so the code keeps coming from the host by hand.
//     What discovery removes is the ADDRESS and the session id, which are
//     awkward to convey and secret from nobody.
//   * The address is taken from where the packet CAME FROM, never from what it
//     says. Otherwise an announcement could point joins at a third machine —
//     the same reason the session directory records REMOTE_ADDR and ignores any
//     address in the request body.
namespace HE::Net::LanBeacon {

// The well-known port and group. Chosen in the dynamic range, out of the way of
// anything registered; TTL 1 keeps announcements on the local segment where they
// belong (a router will not forward them even if it is willing to route
// multicast).
inline constexpr std::uint16_t kPort           = 47823;
inline constexpr const char*   kMulticastGroup = "239.255.42.99";
inline constexpr const char*   kBroadcast      = "255.255.255.255";

// Sent to both the group and the broadcast address on every tick. One of the two
// is dropped on most networks and which one varies — some access points filter
// multicast, some routers filter broadcast — so both go out and the receiver
// discards the duplicate.
inline constexpr std::uint32_t kMagic          = 0x484E4C31u;  // "HNL1"
inline constexpr std::uint64_t kAnnounceMs     = 2000;   // how often a host speaks
inline constexpr std::uint64_t kExpiryMs       = 7000;   // ~3 missed beacons
inline constexpr std::size_t   kMaxDatagram    = 512;
inline constexpr std::size_t   kMaxSessions    = 32;     // a forged flood must not grow
inline constexpr std::size_t   kMaxStringLen   = 96;

// What a host says about itself. Everything here is world-readable on the
// segment — that is the point — so nothing secret may be added to it.
struct Announcement {
    std::uint16_t protocol   = 0;   // kCollabProtocolVersion of the announcer
    std::uint64_t instance   = 0;   // this editor run; dedupes the two copies
    std::string   sessionId;        // display + matching only
    std::uint16_t port       = 0;
    std::string   hostName;         // the person's display name
    std::string   projectLabel;     // shown, so "which of my projects" is answerable
    std::string   projectKey;       // compared, so a mismatch is visible BEFORE joining
    std::uint8_t  participants = 0;
    bool          closing    = false;  // the session is ending; drop it now
};

// Encode/decode are pure and separately testable, which is the whole reason they
// are not buried in the socket loop: multicast in a test environment is a
// coin toss, and the parsing is where the bugs are.
HE_NET_API std::vector<std::uint8_t> encode(const Announcement& a);
// False when the datagram is not ours, is truncated, or carries an implausible
// length — every string is capped, because this is unauthenticated input from
// anyone who can reach the port.
HE_NET_API bool decode(const std::uint8_t* data, std::size_t len, Announcement& out);

// ─── Host side ───────────────────────────────────────────────────────────────
// Speaks every kAnnounceMs when update() is called. Poll-driven like everything
// else here, so it lives on the frame loop without a thread.
class HE_NET_API Announcer {
public:
    ~Announcer();
    // Opens the socket. False = the local network is unavailable (on macOS,
    // usually a missing Local Network permission), which is not fatal: the
    // session runs, it just cannot be found this way.
    bool start(const Announcement& what);
    void stop();                 // sends the goodbye, then closes
    bool running() const { return m_sock != kInvalidSocket; }

    // Live edits — participant count changes as people come and go.
    void setParticipants(std::uint8_t n) { m_what.participants = n; }

    void update(std::uint64_t nowMs);

private:
    void send(const Announcement& a);

    SocketHandle  m_sock = kInvalidSocket;
    Announcement  m_what;
    std::uint64_t m_lastMs = 0;
};

// ─── Guest side ──────────────────────────────────────────────────────────────
class HE_NET_API Browser {
public:
    struct Session {
        std::string   address;      // from the packet's source, never its payload
        std::uint16_t port = 0;
        std::string   sessionId;
        std::string   hostName;
        std::string   projectLabel;
        std::string   projectKey;
        std::uint16_t protocol     = 0;
        std::uint8_t  participants = 0;
        std::uint64_t instance     = 0;
        std::uint64_t lastSeenMs   = 0;
    };

    ~Browser();
    bool start();
    void stop();
    bool running() const { return m_sock != kInvalidSocket; }

    // Drains the socket and drops anything not heard from for kExpiryMs.
    void update(std::uint64_t nowMs);

    // Public so tests can feed datagrams without a network: the registry rules
    // (dedupe by instance, expiry, the cap, goodbye) are the part worth testing,
    // and none of them need a socket to be wrong.
    void ingest(const std::string& fromHost, const std::uint8_t* data,
                std::size_t len, std::uint64_t nowMs);

    const std::vector<Session>& sessions() const { return m_sessions; }

private:
    SocketHandle         m_sock = kInvalidSocket;
    std::vector<Session> m_sessions;
};

} // namespace HE::Net::LanBeacon
