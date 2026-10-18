#pragma once
// ─── Source-control sync connection to the team server (topic 199) ──────────
// The editor's half of the team server's "Quellcodeverwaltung" (CollabSession
// README; design: docs/team-server-source-control-design-2026-10-13.md, as
// built in its section 12). A sync connection is a TCP connection of its own,
// next to the team connection (TeamClient): the team connection caps a frame
// at 1 KiB and carries presence, while this one carries whole files.
//
//   TeamClient::scTicket(project)  → 16 bytes, valid 60 s, single use
//   ScClient::connect(…, ticket)   → SC_HELLO, STATUS sc-ready
//   request(op, fields, stream)    → SC_REQUEST [+ SC_DATA…] … SC_DATA… SC_REPLY
//
// Replies come strictly in request order, a reply's SC_DATA frames before its
// SC_REPLY, so requests may be pipelined. A refused request keeps the
// connection open; a frame against the rules closes it.
//
// Poll-driven and thread-agnostic: whoever owns it calls update(). The editor's
// TeamSyncService runs it on its worker thread, so a 60 MiB upload never
// stalls a frame. Unencrypted, like the team connection (design section 8).
#include "Net/NetCommon.h"
#include "Net/RelayTransport.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace HE::Net {

class TcpTransport;

class HE_NET_API ScClient {
public:
    // ── Wire constants (public so tests and a fake server speak the same bytes) ──
    enum class FrameType : std::uint8_t {
        Status = 2, ScHello = 10, ScRequest = 11, ScReply = 12, ScData = 13,
    };
    enum class StatusCode : std::uint8_t {
        ScReady = 5, BadHello = 16, BadVersion = 17, BadLogin = 23, Invalid = 25,
        NoProject = 27,
    };
    enum class Op : std::uint8_t {
        Head = 1, Manifest = 2, Have = 3, Get = 4, Put = 5, Commit = 6, Log = 7,
        CreateProject = 8, BackupSet = 9, BackupStatus = 10,
    };
    enum class Result : std::uint8_t {
        Ok = 0, BadRequest = 1, NotAdmin = 2, MissingBlob = 3, TooLarge = 4,
        CaseClash = 5, BadPath = 6, Busy = 7, BackupNotSet = 8, ServerError = 9,
        NoProject = 10, ProjectExists = 11, HashMismatch = 12,
        // Never on the wire: the connection went away before the reply came.
        ConnectionLost = 255,
    };
    enum class State : std::uint8_t {
        Connecting,   // TCP not up yet
        Hello,        // SC_HELLO sent, no STATUS yet
        Ready,        // requests flow
        Closed,       // refused, lost or close()d — see closeCode()/lastError()
    };

    // One SC_DATA frame carries at most this many bytes (both directions).
    static constexpr std::size_t   kMaxData     = 1024 * 1024;
    static constexpr std::size_t   kTicketSize  = 16;
    // Connect + hello, and the oldest outstanding reply. A request that sends
    // a stream gets kAnswerTimeoutMs plus one millisecond per kBytesPerMs of
    // it (about 2 Mbit/s), so a big upload on a slow line is not cut off.
    static constexpr std::uint64_t kAnswerTimeoutMs = 120'000;
    static constexpr std::uint64_t kBytesPerMs      = 256;

    struct Reply {
        std::uint32_t             request = 0;
        Op                        op      = Op::Head;
        Result                    result  = Result::Ok;
        std::string               text;      // the server's reason, may be empty
        std::vector<std::uint8_t> body;      // after the text
        std::vector<std::uint8_t> stream;    // the SC_DATA frames before it, joined
        bool ok() const { return result == Result::Ok; }
    };
    using ReplyFn = std::function<void(Reply&)>;

    // Starts connecting; the hello goes out as soon as TCP is up. Null when the
    // names or the ticket cannot be valid, or the OS refuses on the spot.
    static std::unique_ptr<ScClient> connect(const RelayAddress& server,
                                             const std::string& team,
                                             const std::string& member,
                                             const std::string& project,
                                             const std::string& ticket);
    ~ScClient();

    ScClient(const ScClient&)            = delete;
    ScClient& operator=(const ScClient&) = delete;

    // Pump: read frames, run reply callbacks (from here, never from inside a
    // request call). `nowMs` is a test seam.
    void update();
    void update(std::uint64_t nowMs);
    void close();

    State              state() const { return m_state; }
    bool               ready() const { return m_state == State::Ready; }
    // sc-ready with "no project yet": the project does not exist, and this
    // (admin's) connection may only CREATE_PROJECT.
    bool               projectMissing() const { return m_projectMissing; }
    std::uint8_t       closeCode() const { return m_closeCode; }
    const std::string& lastError() const { return m_lastError; }
    const std::string& project() const { return m_project; }
    // Requests sent and not answered yet.
    std::size_t        outstanding() const { return m_pending.size(); }

    // ── Requests ──
    // Each returns its request id (0 when closed — `done` then never runs).
    // Allowed from the moment connect() returns; requests wait for sc-ready.
    // `stream` (HAVE, PUT, COMMIT) follows the request as SC_DATA frames.
    std::uint32_t request(Op op, std::vector<std::uint8_t> fields,
                          const std::vector<std::uint8_t>* stream, ReplyFn done);

    std::uint32_t head(ReplyFn done);                                   // rev u64 | oid str8
    std::uint32_t manifest(std::uint64_t rev, ReplyFn done);            // 0 = head
    std::uint32_t have(const std::vector<std::string>& sha256Hex, ReplyFn done);
    std::uint32_t get(const std::string& sha256Hex, ReplyFn done);
    std::uint32_t put(const std::string& sha256Hex, const std::vector<std::uint8_t>& bytes,
                      ReplyFn done);
    // `entries`: "A|M|D \t path \t base|- \t new|- \n" lines.
    std::uint32_t commit(std::uint64_t baseRev, const std::string& message,
                         const std::string& entries, ReplyFn done);
    std::uint32_t log(std::uint64_t fromRev, std::uint16_t count, ReplyFn done);
    std::uint32_t createProject(ReplyFn done);
    std::uint32_t backupSet(const std::string& url, const std::string& branch, ReplyFn done);
    std::uint32_t backupStatus(ReplyFn done);

    // ── Helpers ──
    static const char* resultName(std::uint8_t result);
    static std::string describeResult(Result result, const std::string& serverText);
    static std::string describeStatus(std::uint8_t code, const std::string& serverText);
    // 64 lower-case hex digits ↔ 32 bytes. False / empty on anything else.
    static bool        hexToBytes(const std::string& hex, std::uint8_t out[32]);
    // Big-endian readers over a reply body; false (and `pos` unchanged) when short.
    static bool readU8(const std::vector<std::uint8_t>& b, std::size_t& pos, std::uint8_t& v);
    static bool readU64(const std::vector<std::uint8_t>& b, std::size_t& pos, std::uint64_t& v);
    static bool readStr8(const std::vector<std::uint8_t>& b, std::size_t& pos, std::string& s);
    static bool readStr16(const std::vector<std::uint8_t>& b, std::size_t& pos, std::string& s);

    static std::vector<std::uint8_t> encodeHello(const std::string& team, const std::string& member,
                                                 const std::string& project,
                                                 const std::string& ticket);

private:
    ScClient() = default;

    struct Pending {
        Op                        op = Op::Head;
        std::vector<std::vector<std::uint8_t>> held;   // frames waiting for sc-ready
        std::uint64_t             allowanceMs = 0;     // the stream's share of the timeout
        ReplyFn                   done;
        std::vector<std::uint8_t> stream;              // reply data so far
    };

    void sendFrame(const std::vector<std::uint8_t>& payload);
    void onFrame(const std::vector<std::uint8_t>& frame);
    void onReply(const std::uint8_t* p, std::size_t n);
    void onData(const std::uint8_t* p, std::size_t n);
    void fail(std::uint8_t code, std::string why);

    std::unique_ptr<TcpTransport> m_tcp;
    ConnectionId                  m_conn = kInvalidConnection;
    State                         m_state = State::Connecting;
    RelayAddress                  m_server;
    std::string                   m_project;
    std::vector<std::uint8_t>     m_helloFrame;
    bool                          m_projectMissing = false;
    std::uint8_t                  m_closeCode = 0;
    std::string                   m_lastError;
    std::uint32_t                 m_nextRequest = 1;
    // In request order (ids only grow), which is the order replies come in.
    std::map<std::uint32_t, Pending> m_pending;
    std::vector<std::pair<ReplyFn, Reply>> m_ready;
    std::uint64_t                 m_nowMs = 0;
    std::uint64_t                 m_startMs = 0;
    std::uint64_t                 m_lastActivityMs = 0;   // last frame in, or the clock start of a request
};

} // namespace HE::Net
