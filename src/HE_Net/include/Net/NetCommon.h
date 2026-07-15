#pragma once

// ─── HorizonNet — module export macro ────────────────────────────────────────
// Mirrors HE_Core's HE_API pattern (see Types/Defines.h): explicit dllexport on
// the building side, dllimport on consumers. HorizonNet is API-heavy (transport
// / session / message interfaces are consumed by HE_Scene + HE_Editor), so it
// uses explicit export rather than WINDOWS_EXPORT_ALL_SYMBOLS — this keeps the
// ABI surface deliberate and matches the Windows-CI portability rules.
#ifdef _WIN32
  #ifdef HE_NET_BUILD_DLL
    #define HE_NET_API __declspec(dllexport)
  #else
    #define HE_NET_API __declspec(dllimport)
  #endif
#else
  #define HE_NET_API __attribute__((visibility("default")))
#endif

#include <cstdint>

namespace HE::Net {

// ─── Core identifiers ────────────────────────────────────────────────────────

// A logical peer connection. 0 is reserved for "no connection". On a server this
// identifies a remote client; on a client it identifies the server link.
using ConnectionId = std::uint32_t;
inline constexpr ConnectionId kInvalidConnection = 0;

// Application-level message type id. The high range is reserved for engine
// internal messages; games/editors register their own ids above kFirstUserMessage.
using MessageId = std::uint16_t;
inline constexpr MessageId kInvalidMessage    = 0;
inline constexpr MessageId kFirstUserMessage  = 1024;

// ─── Delivery semantics ──────────────────────────────────────────────────────
// The transport core exposes exactly these guarantees. Gameplay replication
// (Layer 3a) leans on Unreliable for high-frequency snapshots; editor
// collaboration (Layer 3b) — presence, locks — always uses ReliableOrdered.
enum class SendMode : std::uint8_t {
    Unreliable,       // fire-and-forget, may drop / reorder
    Reliable,         // guaranteed delivery, unspecified order
    ReliableOrdered,  // guaranteed delivery, in order per channel
};

// ─── Topology role ───────────────────────────────────────────────────────────
enum class NetRole : std::uint8_t {
    None,     // not networked
    Client,   // connects to a remote authority
    Server,   // dedicated authority, no local player
    Host,     // listen-server: authority + local player
};

// ─── Connection lifecycle, surfaced through the transport event queue ─────────
enum class NetEventType : std::uint8_t {
    Connected,      // a peer link came up (conn is valid)
    Disconnected,   // a peer link went down (conn is valid, was previously up)
    Data,           // a datagram arrived from `conn` (payload in NetEvent::data)
};

} // namespace HE::Net
