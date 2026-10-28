#include "Net/ScClient.h"

#include "Net/TcpTransport.h"

#include "NetLog.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace HE::Net {
namespace {

constexpr char         kMagic[4]   = { 'H', 'E', 'R', 'L' };
constexpr std::uint8_t kVersion    = 1;
constexpr std::size_t  kHeaderSize = 6;   // magic + version + type
// An SC_DATA frame is its header, request, offset, last flag and at most 1 MiB;
// a reply frame is smaller. Anything past this is a broken length prefix.
constexpr std::uint32_t kMaxScFrame = static_cast<std::uint32_t>(ScClient::kMaxData) + 64;
// What the server takes in one SC_REQUEST (fields included).
constexpr std::size_t kMaxRequest = 64 * 1024;

std::uint64_t steadyMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

void putHeader(std::vector<std::uint8_t>& out, ScClient::FrameType type) {
    out.insert(out.end(), kMagic, kMagic + 4);
    out.push_back(kVersion);
    out.push_back(static_cast<std::uint8_t>(type));
}

void putU16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

void putU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

void putU64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    putU32(out, static_cast<std::uint32_t>(v >> 32));
    putU32(out, static_cast<std::uint32_t>(v));
}

void putStr8(std::vector<std::uint8_t>& out, const std::string& s) {
    const std::size_t n = std::min<std::size_t>(s.size(), 255);
    out.push_back(static_cast<std::uint8_t>(n));
    out.insert(out.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
}

void putStr16(std::vector<std::uint8_t>& out, const std::string& s) {
    const std::size_t n = std::min<std::size_t>(s.size(), 0xFFFF);
    putU16(out, static_cast<std::uint16_t>(n));
    out.insert(out.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
}

std::uint32_t getU32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}

std::uint64_t getU64(const std::uint8_t* p) {
    return (std::uint64_t(getU32(p)) << 32) | getU32(p + 4);
}

std::string withReason(std::string sentence, const std::string& text) {
    if (text.empty()) return sentence;
    if (!sentence.empty() && sentence.back() == '.') sentence.pop_back();
    return sentence + " (" + text + ").";
}

} // namespace

// ─── Connect ─────────────────────────────────────────────────────────────────

std::vector<std::uint8_t> ScClient::encodeHello(const std::string& team, const std::string& member,
                                                const std::string& project,
                                                const std::string& ticket) {
    std::vector<std::uint8_t> f;
    putHeader(f, FrameType::ScHello);
    putStr8(f, team);
    putStr8(f, member);
    putStr8(f, project);
    f.insert(f.end(), ticket.begin(), ticket.end());
    return f;
}

std::unique_ptr<ScClient> ScClient::connect(const RelayAddress& server, const std::string& team,
                                            const std::string& member, const std::string& project,
                                            const std::string& ticket) {
    if (!RelayTransport::isValidId(team) || !RelayTransport::isValidId(member) ||
        !RelayTransport::isValidId(project) || ticket.size() != kTicketSize ||
        server.host.empty() || server.port == 0)
        return nullptr;

    auto tcp = TcpTransport::connect(server.host, server.port);
    if (!tcp) return nullptr;
    tcp->setMaxFrameSize(kMaxScFrame);

    std::unique_ptr<ScClient> c(new ScClient());
    c->m_tcp        = std::move(tcp);
    c->m_server     = server;
    c->m_project    = project;
    c->m_helloFrame = encodeHello(team, member, project, ticket);
    HE_LOG_INFO(Net, "Sync: connecting to project '%s' of team '%s' as '%s' at %s:%u",
                project.c_str(), team.c_str(), member.c_str(), server.host.c_str(),
                static_cast<unsigned>(server.port));
    return c;
}

ScClient::~ScClient() {
    // The ticket is single use, but nothing else needs it either.
    std::fill(m_helloFrame.begin(), m_helloFrame.end(), std::uint8_t(0));
}

void ScClient::close() {
    if (m_state == State::Closed) return;
    fail(0, "Sync connection closed.");
}

// ─── Pump ────────────────────────────────────────────────────────────────────

void ScClient::update() { update(steadyMs()); }

void ScClient::update(std::uint64_t nowMs) {
    m_nowMs = nowMs;
    if (m_startMs == 0) m_startMs = nowMs ? nowMs : 1;

    if (m_state != State::Closed) {
        m_tcp->update();
        NetEvent ev;
        while (m_state != State::Closed && m_tcp->poll(ev)) {
            switch (ev.type) {
                case NetEventType::Connected:
                    m_conn  = ev.conn;
                    m_state = State::Hello;
                    sendFrame(m_helloFrame);
                    std::fill(m_helloFrame.begin(), m_helloFrame.end(), std::uint8_t(0));
                    m_helloFrame.clear();
                    break;
                case NetEventType::Data:
                    m_lastActivityMs = m_nowMs;
                    onFrame(ev.data);
                    break;
                case NetEventType::Disconnected:
                    fail(0, m_state == State::Connecting
                                ? "Could not reach the team server at " + m_server.host + ":" +
                                      std::to_string(m_server.port) + "."
                                : std::string("The team server closed the sync connection."));
                    break;
            }
        }
    }

    if (m_state == State::Connecting || m_state == State::Hello) {
        if (m_nowMs - m_startMs > kAnswerTimeoutMs)
            fail(0, m_state == State::Connecting
                        ? "Could not reach the team server at " + m_server.host + ":" +
                              std::to_string(m_server.port) + " (no answer)."
                        : std::string("The team server did not answer the sync hello."));
    } else if (m_state == State::Ready && !m_pending.empty()) {
        // The oldest outstanding request is the one whose answer is due. Its
        // clock starts at the last frame that came in (a long stream in
        // progress counts as an answer in progress).
        const Pending& oldest = m_pending.begin()->second;
        if (m_nowMs - m_lastActivityMs > kAnswerTimeoutMs + oldest.allowanceMs)
            fail(0, "The team server stopped answering the sync connection.");
    }

    // Callbacks last, from a local list: one that sends a request or closes
    // this client must not run inside the frame loop above.
    auto ready = std::move(m_ready);
    m_ready.clear();
    for (auto& [fn, reply] : ready)
        if (fn) fn(reply);
}

void ScClient::sendFrame(const std::vector<std::uint8_t>& payload) {
    m_tcp->send(m_conn, payload, SendMode::ReliableOrdered);
}

void ScClient::fail(std::uint8_t code, std::string why) {
    if (m_state == State::Closed) return;
    m_state     = State::Closed;
    m_closeCode = code;
    m_lastError = std::move(why);
    if (m_conn != kInvalidConnection) m_tcp->disconnect(m_conn);
    std::fill(m_helloFrame.begin(), m_helloFrame.end(), std::uint8_t(0));
    m_helloFrame.clear();
    for (auto& [id, p] : m_pending) {
        Reply r;
        r.request = id;
        r.op      = p.op;
        r.result  = Result::ConnectionLost;
        r.text    = m_lastError;
        if (p.done) m_ready.emplace_back(std::move(p.done), std::move(r));
    }
    m_pending.clear();
    if (code != 0)
        HE_LOG_WARN(Net, "Sync: refused by the server — %s", m_lastError.c_str());
    else
        HE_LOG_INFO(Net, "Sync: %s", m_lastError.c_str());
}

// ─── Incoming ────────────────────────────────────────────────────────────────

void ScClient::onFrame(const std::vector<std::uint8_t>& f) {
    if (f.size() < kHeaderSize || std::memcmp(f.data(), kMagic, 4) != 0 || f[4] != kVersion) {
        fail(0, "The server at " + m_server.host + " does not speak the sync protocol.");
        return;
    }
    const std::uint8_t* body = f.data() + kHeaderSize;
    const std::size_t   n    = f.size() - kHeaderSize;
    switch (static_cast<FrameType>(f[5])) {
        case FrameType::Status: {
            if (n < 2 || n != 2u + body[1]) {
                fail(0, "The team server sent a broken status frame.");
                return;
            }
            const std::uint8_t code = body[0];
            const std::string  text(reinterpret_cast<const char*>(body + 2), body[1]);
            if (m_state == State::Hello && code == std::uint8_t(StatusCode::ScReady)) {
                m_state          = State::Ready;
                m_projectMissing = text == "no project yet";
                m_lastActivityMs = m_nowMs;
                HE_LOG_INFO(Net, "Sync: ready for project '%s'%s", m_project.c_str(),
                            m_projectMissing ? " (not created yet)" : "");
                // Whatever was asked for while connecting goes out now, in order.
                for (auto& [id, p] : m_pending) {
                    (void)id;
                    for (const auto& frame : p.held) sendFrame(frame);
                    p.held.clear();
                }
                return;
            }
            fail(code, describeStatus(code, text));
            return;
        }
        case FrameType::ScReply: onReply(body, n); return;
        case FrameType::ScData:  onData(body, n);  return;
        default:
            fail(0, "The team server sent a frame this editor does not know on the sync connection.");
            return;
    }
}

void ScClient::onData(const std::uint8_t* p, std::size_t n) {
    // request u32 | offset u64 | last u8 | bytes
    if (n < 13 || m_pending.empty()) {
        fail(0, "The team server sent sync data nobody asked for.");
        return;
    }
    const std::uint32_t id     = getU32(p);
    const std::uint64_t offset = getU64(p + 4);
    auto it = m_pending.begin();
    if (it->first != id || offset != it->second.stream.size() || n - 13 > kMaxData) {
        fail(0, "The team server sent sync data out of order.");
        return;
    }
    it->second.stream.insert(it->second.stream.end(), p + 13, p + n);
}

void ScClient::onReply(const std::uint8_t* p, std::size_t n) {
    // request u32 | result u8 | text str16 | body
    if (n < 7 || m_pending.empty()) {
        fail(0, "The team server sent a sync reply nobody asked for.");
        return;
    }
    const std::uint32_t id  = getU32(p);
    const std::size_t   len = (std::size_t(p[5]) << 8) | p[6];
    auto it = m_pending.begin();
    if (it->first != id || 7 + len > n) {
        fail(0, "The team server sent a sync reply out of order.");
        return;
    }
    Reply r;
    r.request = id;
    r.op      = it->second.op;
    r.result  = static_cast<Result>(p[4]);
    r.text.assign(reinterpret_cast<const char*>(p + 7), len);
    r.body.assign(p + 7 + len, p + n);
    r.stream  = std::move(it->second.stream);
    ReplyFn done = std::move(it->second.done);
    m_pending.erase(it);
    if (!r.ok())
        HE_LOG_INFO(Net, "Sync: request %u refused: %s%s%s", static_cast<unsigned>(id),
                    resultName(static_cast<std::uint8_t>(r.result)), r.text.empty() ? "" : " — ",
                    r.text.c_str());
    if (done) m_ready.emplace_back(std::move(done), std::move(r));
}

// ─── Requests ────────────────────────────────────────────────────────────────

std::uint32_t ScClient::request(Op op, std::vector<std::uint8_t> fields,
                                const std::vector<std::uint8_t>* stream, ReplyFn done) {
    if (m_state == State::Closed) return 0;
    if (fields.size() + 5 > kMaxRequest) {
        HE_LOG_WARN(Net, "Sync: request too large (%s), not sent", detail::logBytes(fields.size()).c_str());
        return 0;
    }
    const std::uint32_t id = m_nextRequest++;
    std::vector<std::vector<std::uint8_t>> frames;
    {
        std::vector<std::uint8_t> f;
        putHeader(f, FrameType::ScRequest);
        putU32(f, id);
        f.push_back(static_cast<std::uint8_t>(op));
        f.insert(f.end(), fields.begin(), fields.end());
        frames.push_back(std::move(f));
    }
    std::uint64_t allowance = 0;
    if (stream) {
        // An empty stream is one empty last frame.
        std::size_t offset = 0;
        do {
            const std::size_t chunk = std::min(kMaxData, stream->size() - offset);
            std::vector<std::uint8_t> f;
            f.reserve(kHeaderSize + 13 + chunk);
            putHeader(f, FrameType::ScData);
            putU32(f, id);
            putU64(f, offset);
            f.push_back(offset + chunk == stream->size() ? 1 : 0);
            f.insert(f.end(), stream->begin() + static_cast<std::ptrdiff_t>(offset),
                     stream->begin() + static_cast<std::ptrdiff_t>(offset + chunk));
            frames.push_back(std::move(f));
            offset += chunk;
        } while (offset < stream->size());
        allowance = stream->size() / kBytesPerMs;
    }
    Pending& p    = m_pending[id];
    p.op          = op;
    p.done        = std::move(done);
    p.allowanceMs = allowance;
    if (m_state == State::Ready) {
        // Nothing outstanding until now: the answer's clock starts here.
        if (m_pending.size() == 1) m_lastActivityMs = m_nowMs ? m_nowMs : steadyMs();
        for (const auto& f : frames) sendFrame(f);
    } else {
        p.held = std::move(frames);
    }
    return id;
}

std::uint32_t ScClient::head(ReplyFn done) {
    return request(Op::Head, {}, nullptr, std::move(done));
}

std::uint32_t ScClient::manifest(std::uint64_t rev, ReplyFn done) {
    std::vector<std::uint8_t> f;
    putU64(f, rev);
    return request(Op::Manifest, std::move(f), nullptr, std::move(done));
}

std::uint32_t ScClient::have(const std::vector<std::string>& sha256Hex, ReplyFn done) {
    std::vector<std::uint8_t> s;
    for (const std::string& h : sha256Hex) {
        s.insert(s.end(), h.begin(), h.end());
        s.push_back('\n');
    }
    return request(Op::Have, {}, &s, std::move(done));
}

std::uint32_t ScClient::get(const std::string& sha256Hex, ReplyFn done) {
    std::uint8_t raw[32];
    if (!hexToBytes(sha256Hex, raw)) return 0;
    return request(Op::Get, std::vector<std::uint8_t>(raw, raw + 32), nullptr, std::move(done));
}

std::uint32_t ScClient::put(const std::string& sha256Hex, const std::vector<std::uint8_t>& bytes,
                            ReplyFn done) {
    std::uint8_t raw[32];
    if (!hexToBytes(sha256Hex, raw)) return 0;
    std::vector<std::uint8_t> f(raw, raw + 32);
    putU64(f, bytes.size());
    return request(Op::Put, std::move(f), &bytes, std::move(done));
}

std::uint32_t ScClient::commit(std::uint64_t baseRev, const std::string& message,
                               const std::string& entries, ReplyFn done) {
    std::vector<std::uint8_t> f;
    putU64(f, baseRev);
    putStr16(f, message);
    const std::vector<std::uint8_t> s(entries.begin(), entries.end());
    return request(Op::Commit, std::move(f), &s, std::move(done));
}

std::uint32_t ScClient::log(std::uint64_t fromRev, std::uint16_t count, ReplyFn done) {
    std::vector<std::uint8_t> f;
    putU64(f, fromRev);
    putU16(f, count);
    return request(Op::Log, std::move(f), nullptr, std::move(done));
}

std::uint32_t ScClient::createProject(ReplyFn done) {
    return request(Op::CreateProject, {}, nullptr, std::move(done));
}

std::uint32_t ScClient::backupSet(const std::string& url, const std::string& branch, ReplyFn done) {
    std::vector<std::uint8_t> f;
    putStr16(f, url);
    putStr8(f, branch);
    return request(Op::BackupSet, std::move(f), nullptr, std::move(done));
}

std::uint32_t ScClient::backupStatus(ReplyFn done) {
    return request(Op::BackupStatus, {}, nullptr, std::move(done));
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

bool ScClient::hexToBytes(const std::string& hex, std::uint8_t out[32]) {
    if (hex.size() != 64) return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < 32; ++i) {
        const int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return true;
}

bool ScClient::readU8(const std::vector<std::uint8_t>& b, std::size_t& pos, std::uint8_t& v) {
    if (pos + 1 > b.size()) return false;
    v = b[pos++];
    return true;
}

bool ScClient::readU64(const std::vector<std::uint8_t>& b, std::size_t& pos, std::uint64_t& v) {
    if (pos + 8 > b.size()) return false;
    v = getU64(b.data() + pos);
    pos += 8;
    return true;
}

bool ScClient::readStr8(const std::vector<std::uint8_t>& b, std::size_t& pos, std::string& s) {
    if (pos + 1 > b.size() || pos + 1 + b[pos] > b.size()) return false;
    s.assign(reinterpret_cast<const char*>(b.data() + pos + 1), b[pos]);
    pos += 1 + b[pos];
    return true;
}

bool ScClient::readStr16(const std::vector<std::uint8_t>& b, std::size_t& pos, std::string& s) {
    if (pos + 2 > b.size()) return false;
    const std::size_t len = (std::size_t(b[pos]) << 8) | b[pos + 1];
    if (pos + 2 + len > b.size()) return false;
    s.assign(reinterpret_cast<const char*>(b.data() + pos + 2), len);
    pos += 2 + len;
    return true;
}

const char* ScClient::resultName(std::uint8_t result) {
    static constexpr const char* kNames[] = {
        "ok", "bad-request", "not-admin", "missing-blob", "too-large", "case-clash",
        "bad-path", "busy", "backup-not-set", "server-error", "no-project",
        "project-exists", "hash-mismatch",
    };
    if (result < sizeof(kNames) / sizeof(kNames[0])) return kNames[result];
    if (result == std::uint8_t(Result::ConnectionLost)) return "connection-lost";
    return "unknown";
}

std::string ScClient::describeResult(Result result, const std::string& text) {
    switch (result) {
        case Result::Ok:            return {};
        case Result::NotAdmin:      return "Only a team admin can do that.";
        case Result::TooLarge:      return withReason("A file is too large for the team server.", text);
        case Result::CaseClash:     return withReason("A new file differs from an existing one only in upper/lower case.", text);
        case Result::BadPath:       return withReason("The team server refused a file path.", text);
        case Result::Busy:          return "The team server is busy; trying again shortly.";
        case Result::BackupNotSet:  return "No GitHub backup is set up for this project.";
        case Result::ServerError:   return "The team server could not store the change (see its log).";
        case Result::NoProject:     return "The project does not exist on the team server yet.";
        case Result::ProjectExists: return "The project already exists on the team server.";
        case Result::HashMismatch:  return "A file changed while it was being uploaded.";
        case Result::MissingBlob:   return "The team server is missing a file it was told about.";
        case Result::ConnectionLost:
            return text.empty() ? std::string("The sync connection to the team server was lost.") : text;
        default:
            return withReason(std::string("The team server refused: ") +
                                  resultName(static_cast<std::uint8_t>(result)) + ".", text);
    }
}

std::string ScClient::describeStatus(std::uint8_t code, const std::string& text) {
    switch (static_cast<StatusCode>(code)) {
        case StatusCode::BadLogin:   return "The team server did not accept the sync ticket (expired or used).";
        case StatusCode::NoProject:  return "The team has no such project on its server.";
        case StatusCode::Invalid:    return withReason("The team server refused the team, member or project name.", text);
        case StatusCode::BadVersion: return "The team server speaks a different protocol version than this editor.";
        case StatusCode::BadHello:   return withReason("The team server did not understand the sync hello.", text);
        default:
            return withReason("The team server closed the sync connection (status " +
                                  std::to_string(code) + ").", text);
    }
}

} // namespace HE::Net
