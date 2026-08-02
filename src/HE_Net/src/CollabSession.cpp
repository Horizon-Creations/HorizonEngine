#include "Net/CollabSession.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace HE::Net {
namespace {

// Collaboration message ids. Kept together so the wire protocol is readable in
// one place; the version guard above covers any change to their meaning.
constexpr MessageId kMsgJoinRequest       = kFirstUserMessage + 0;
constexpr MessageId kMsgJoinAccepted      = kFirstUserMessage + 1;
constexpr MessageId kMsgJoinRejected      = kFirstUserMessage + 2;
constexpr MessageId kMsgParticipantJoined = kFirstUserMessage + 3;
constexpr MessageId kMsgParticipantLeft   = kFirstUserMessage + 4;
constexpr MessageId kMsgSnapshotBegin     = kFirstUserMessage + 5;
constexpr MessageId kMsgSnapshotChunk     = kFirstUserMessage + 6;
constexpr MessageId kMsgSnapshotEnd       = kFirstUserMessage + 7;
// Two ids rather than one shared message: the client→host form carries no
// participant id at all, so a client cannot claim to be someone else. The host
// stamps the id from the connection it arrived on.
constexpr MessageId kMsgPresenceUpdate    = kFirstUserMessage + 8;   // client → host
constexpr MessageId kMsgPresenceRelay     = kFirstUserMessage + 9;   // host → clients

// Quaternion components are always in [-1, 1], so 16 bits each is ~3e-5 of
// angular resolution — far finer than a camera gizmo can show, at a quarter the
// size of raw floats.
constexpr int kQuatBits = 16;

} // namespace

CollabSession::CollabSession(NetSession* net, NetRole role, Config cfg)
    : m_net(net), m_role(role), m_cfg(std::move(cfg)) {
    if (m_role == NetRole::Host) {
        // The host occupies the first slot and is joined from the outset — there
        // is nobody to ask for permission.
        m_localId = m_nextId++;
        m_participants.push_back(Participant{ m_localId, m_cfg.displayName, true });
        m_joined = true;
    }
    installHandlers();
}

void CollabSession::installHandlers() {
    if (!m_net) return;

    m_net->onConnect([this](ConnectionId conn) { onPeerConnected(conn); });
    m_net->onDisconnect([this](ConnectionId conn) { onPeerDisconnected(conn); });

    if (m_role == NetRole::Host) {
        m_net->on(kMsgJoinRequest, [this](ConnectionId conn, BitReader& r) {
            handleJoinRequest(conn, r);
        });
        m_net->on(kMsgPresenceUpdate, [this](ConnectionId conn, BitReader& r) {
            handlePresenceUpdate(conn, r);
        });
    } else {
        m_net->on(kMsgJoinAccepted, [this](ConnectionId, BitReader& r) { handleJoinAccepted(r); });
        m_net->on(kMsgJoinRejected, [this](ConnectionId, BitReader& r) { handleJoinRejected(r); });
        m_net->on(kMsgSnapshotBegin, [this](ConnectionId, BitReader& r) { handleSnapshotBegin(r); });
        m_net->on(kMsgSnapshotChunk, [this](ConnectionId, BitReader& r) { handleSnapshotChunk(r); });
        m_net->on(kMsgSnapshotEnd,   [this](ConnectionId, BitReader&)   { handleSnapshotEnd(); });
        m_net->on(kMsgPresenceRelay, [this](ConnectionId, BitReader& r) { handlePresenceRelay(r); });

        // Roster updates for peers other than ourselves.
        m_net->on(kMsgParticipantJoined, [this](ConnectionId, BitReader& r) {
            Participant p;
            std::uint32_t id = 0;
            if (!r.readUInt32(id) || !r.readString(p.name)) return;
            p.id = id;
            if (findParticipant(p.id)) return;         // already known
            m_participants.push_back(p);
            if (m_onJoin) m_onJoin(p);
        });
        m_net->on(kMsgParticipantLeft, [this](ConnectionId, BitReader& r) {
            std::uint32_t id = 0;
            if (!r.readUInt32(id)) return;
            const auto before = m_participants.size();
            m_participants.erase(
                std::remove_if(m_participants.begin(), m_participants.end(),
                               [id](const Participant& p) { return p.id == id; }),
                m_participants.end());
            if (m_participants.size() != before && m_onLeft) m_onLeft(id);
        });
    }
}

void CollabSession::update() {
    update(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()));
}

void CollabSession::update(std::uint64_t nowMs) {
    if (!m_net) return;

    // A client asks to join as soon as the authenticated link is up. Sending
    // once is important: update() runs every frame, and a second request would
    // allocate a second participant slot for the same peer.
    if (m_role == NetRole::Client && !m_joinSent && !m_net->connections().empty()) {
        BitWriter w;
        w.writeUInt16(kCollabProtocolVersion);
        w.writeString(m_cfg.displayName);
        m_net->send(m_net->connections().front(), kMsgJoinRequest, w);
        m_joinSent = true;
    }

    // Presence: only once the participant actually exists in the session, at
    // most every presenceIntervalMs, and only when something moved.
    if (m_joined && m_localPresenceSet) {
        const bool due = !m_everSentPresence ||
                         (nowMs - m_lastPresenceSendMs) >= m_cfg.presenceIntervalMs;
        if (due && presenceDiffersFromSent()) {
            sendLocalPresence();
            m_lastSentPresence   = m_localPresence;
            m_everSentPresence   = true;
            m_lastPresenceSendMs = nowMs;
        }
    }
}

Participant* CollabSession::findParticipant(ParticipantId id) {
    for (auto& p : m_participants) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

ParticipantId CollabSession::participantForConnection(ConnectionId conn) const {
    for (const auto& [c, id] : m_connToParticipant) {
        if (c == conn) return id;
    }
    return kInvalidParticipant;
}

// ─── Host side ───────────────────────────────────────────────────────────────

void CollabSession::onPeerConnected(ConnectionId) {
    // Nothing yet: a peer that has connected has authenticated, but has not
    // asked to join. The host waits for JoinRequest so it learns the display
    // name and can check the protocol version before allocating anything.
}

void CollabSession::handleJoinRequest(ConnectionId conn, BitReader& r) {
    std::uint16_t version = 0;
    std::string   name;
    if (!r.readUInt16(version) || !r.readString(name)) return;

    const auto reject = [&](JoinRejectReason reason) {
        BitWriter w;
        w.writeByte(static_cast<std::uint8_t>(reason));
        m_net->send(conn, kMsgJoinRejected, w);
    };

    if (version != kCollabProtocolVersion) {
        reject(JoinRejectReason::VersionMismatch);
        return;
    }
    if (m_participants.size() >= m_cfg.maxParticipants) {
        reject(JoinRejectReason::SessionFull);
        return;
    }

    // Capture the state *before* admitting the peer: if the host cannot produce
    // a snapshot, the joiner would sit with an empty scene while believing it is
    // in sync. Better to refuse the join outright.
    std::vector<std::uint8_t> snapshot;
    if (!m_state || !m_state->captureSnapshot(snapshot)) {
        reject(JoinRejectReason::SnapshotFailed);
        return;
    }
    if (snapshot.size() > m_cfg.maxSnapshotBytes) {
        reject(JoinRejectReason::SnapshotFailed);
        return;
    }

    const ParticipantId id = m_nextId++;
    Participant joiner{ id, name, false };

    // Accept: assigned id, the sequence its snapshot corresponds to, and the
    // roster as it stands *before* adding the joiner (its own entry arrives via
    // the assigned id, and everyone else learns about it separately).
    BitWriter accept;
    accept.writeUInt32(id);
    accept.writeUInt64(m_sequence);
    accept.writeUInt16(static_cast<std::uint16_t>(m_participants.size()));
    for (const auto& p : m_participants) {
        accept.writeUInt32(p.id);
        accept.writeString(p.name);
        accept.writeBool(p.isHost);
    }
    m_net->send(conn, kMsgJoinAccepted, accept);

    // Then the state itself, chunked.
    const std::uint32_t total = static_cast<std::uint32_t>(snapshot.size());
    const std::uint32_t chunkSize = std::max<std::uint32_t>(1, m_cfg.chunkBytes);
    const std::uint32_t chunks = (total + chunkSize - 1) / chunkSize;

    BitWriter begin;
    begin.writeUInt32(total);
    begin.writeUInt32(chunks);
    begin.writeUInt64(m_sequence);
    m_net->send(conn, kMsgSnapshotBegin, begin);

    for (std::uint32_t i = 0; i < chunks; ++i) {
        const std::uint32_t offset = i * chunkSize;
        const std::uint32_t len = std::min(chunkSize, total - offset);
        BitWriter chunk;
        chunk.writeUInt32(i);
        chunk.writeUInt32(len);
        chunk.writeBytes(snapshot.data() + offset, len);
        m_net->send(conn, kMsgSnapshotChunk, chunk);
    }
    m_net->send(conn, kMsgSnapshotEnd);

    // Now register the joiner and tell everyone else.
    m_participants.push_back(joiner);
    m_connToParticipant.emplace_back(conn, id);

    BitWriter announce;
    announce.writeUInt32(id);
    announce.writeString(name);
    for (const ConnectionId other : m_net->connections()) {
        if (other != conn) m_net->send(other, kMsgParticipantJoined, announce);
    }

    if (m_onJoin) m_onJoin(joiner);
}

void CollabSession::onPeerDisconnected(ConnectionId conn) {
    if (m_role != NetRole::Host) {
        // Client: the link to the host is gone, so the session is over.
        m_joined   = false;
        m_joinSent = false;
        m_participants.clear();
        m_presence.clear();
        m_everSentPresence = false;
        return;
    }

    const auto it = std::find_if(m_connToParticipant.begin(), m_connToParticipant.end(),
                                 [conn](const auto& e) { return e.first == conn; });
    if (it == m_connToParticipant.end()) return;   // never completed a join

    const ParticipantId id = it->second;
    m_connToParticipant.erase(it);
    m_participants.erase(
        std::remove_if(m_participants.begin(), m_participants.end(),
                       [id](const Participant& p) { return p.id == id; }),
        m_participants.end());
    // Drop their presence too, or their camera gizmo would linger in everyone's
    // viewport after they left.
    m_presence.erase(
        std::remove_if(m_presence.begin(), m_presence.end(),
                       [id](const auto& e) { return e.first == id; }),
        m_presence.end());

    BitWriter w;
    w.writeUInt32(id);
    for (const ConnectionId other : m_net->connections()) {
        if (other != conn) m_net->send(other, kMsgParticipantLeft, w);
    }

    if (m_onLeft) m_onLeft(id);
}

// ─── Client side ─────────────────────────────────────────────────────────────

void CollabSession::handleJoinAccepted(BitReader& r) {
    std::uint32_t id = 0;
    std::uint64_t sequence = 0;
    std::uint16_t count = 0;
    if (!r.readUInt32(id) || !r.readUInt64(sequence) || !r.readUInt16(count)) return;

    m_localId  = id;
    m_sequence = sequence;
    m_participants.clear();

    for (std::uint16_t i = 0; i < count; ++i) {
        Participant p;
        std::uint32_t pid = 0;
        bool isHost = false;
        if (!r.readUInt32(pid) || !r.readString(p.name) || !r.readBool(isHost)) return;
        p.id     = pid;
        p.isHost = isHost;
        m_participants.push_back(p);
    }
    // Our own entry is not in the host's list yet, so add it here.
    m_participants.push_back(Participant{ m_localId, m_cfg.displayName, false });

    // Note: not "joined" until the snapshot has been applied — reporting success
    // earlier would let the editor act on a scene it does not have yet.
}

void CollabSession::handleJoinRejected(BitReader& r) {
    std::uint8_t reason = 0;
    r.readByte(reason);
    m_joined = false;
    if (m_onRejected) m_onRejected(static_cast<JoinRejectReason>(reason));
}

void CollabSession::handleSnapshotBegin(BitReader& r) {
    std::uint32_t total = 0, chunks = 0;
    std::uint64_t sequence = 0;
    if (!r.readUInt32(total) || !r.readUInt32(chunks) || !r.readUInt64(sequence)) return;

    // Never reserve what the peer claims without a bound.
    if (total > m_cfg.maxSnapshotBytes) {
        m_snapshotActive = false;
        if (m_onRejected) m_onRejected(JoinRejectReason::SnapshotFailed);
        return;
    }

    m_snapshotBuf.clear();
    m_snapshotBuf.reserve(total);
    m_snapshotExpected = total;
    m_snapshotReceived = 0;
    m_snapshotActive   = true;
    m_sequence         = sequence;

    if (m_onProgress) m_onProgress(0, total);
}

void CollabSession::handleSnapshotChunk(BitReader& r) {
    if (!m_snapshotActive) return;

    std::uint32_t index = 0, len = 0;
    if (!r.readUInt32(index) || !r.readUInt32(len)) return;

    // Refuse a chunk that would push the payload past the announced size, rather
    // than growing the buffer to whatever arrives.
    if (len == 0 || m_snapshotReceived + len > m_snapshotExpected) {
        m_snapshotActive = false;
        if (m_onRejected) m_onRejected(JoinRejectReason::SnapshotFailed);
        return;
    }

    const std::size_t offset = m_snapshotBuf.size();
    m_snapshotBuf.resize(offset + len);
    if (!r.readBytes(m_snapshotBuf.data() + offset, len)) {
        m_snapshotBuf.resize(offset);   // truncated frame
        m_snapshotActive = false;
        if (m_onRejected) m_onRejected(JoinRejectReason::SnapshotFailed);
        return;
    }

    m_snapshotReceived += len;
    if (m_onProgress) m_onProgress(m_snapshotReceived, m_snapshotExpected);
}

void CollabSession::handleSnapshotEnd() {
    if (!m_snapshotActive) return;
    m_snapshotActive = false;

    // An incomplete transfer must not be applied — a partially deserialized
    // scene is worse than none.
    if (m_snapshotReceived != m_snapshotExpected) {
        if (m_onRejected) m_onRejected(JoinRejectReason::SnapshotFailed);
        return;
    }
    if (!m_state || !m_state->applySnapshot(m_snapshotBuf)) {
        if (m_onRejected) m_onRejected(JoinRejectReason::SnapshotFailed);
        return;
    }

    m_snapshotBuf.clear();
    m_snapshotBuf.shrink_to_fit();

    m_joined = true;
    if (m_onJoined) m_onJoined(m_localId);
}

// ─── Presence ────────────────────────────────────────────────────────────────

void CollabSession::setLocalPresence(const float cameraPos[3], const float cameraRot[4],
                                     const std::vector<std::uint64_t>& selection) {
    for (int i = 0; i < 3; ++i) m_localPresence.cameraPos[i] = cameraPos[i];
    for (int i = 0; i < 4; ++i) m_localPresence.cameraRot[i] = cameraRot[i];

    m_localPresence.selection = selection;
    if (m_localPresence.selection.size() > m_cfg.maxSelectionIds) {
        m_localPresence.selection.resize(m_cfg.maxSelectionIds);
    }
    m_localPresence.valid = true;
    m_localPresenceSet    = true;

    // Reflect it locally too, so the UI can treat every participant uniformly.
    applyPresence(m_localId, m_localPresence);
}

const PresenceState* CollabSession::presenceOf(ParticipantId id) const {
    for (const auto& [pid, state] : m_presence) {
        if (pid == id) return &state;
    }
    return nullptr;
}

void CollabSession::applyPresence(ParticipantId id, PresenceState state) {
    for (auto& [pid, existing] : m_presence) {
        if (pid == id) { existing = std::move(state); return; }
    }
    m_presence.emplace_back(id, std::move(state));
}

bool CollabSession::presenceDiffersFromSent() const {
    if (!m_everSentPresence) return true;

    for (int i = 0; i < 3; ++i) {
        if (std::fabs(m_localPresence.cameraPos[i] - m_lastSentPresence.cameraPos[i])
            > m_cfg.presencePositionEpsilon) {
            return true;
        }
    }
    for (int i = 0; i < 4; ++i) {
        if (std::fabs(m_localPresence.cameraRot[i] - m_lastSentPresence.cameraRot[i])
            > m_cfg.presenceRotationEpsilon) {
            return true;
        }
    }
    // Selection changes are discrete — any difference matters.
    return m_localPresence.selection != m_lastSentPresence.selection;
}

void CollabSession::writePresenceBody(BitWriter& w, const PresenceState& p) const {
    // Position stays full precision: a scene can span kilometres, so a fixed
    // quantization range would either clip or lose centimetres.
    for (int i = 0; i < 3; ++i) w.writeFloat(p.cameraPos[i]);
    // Rotation is a unit quaternion, so every component is bounded by [-1, 1]
    // and quantizes cleanly.
    for (int i = 0; i < 4; ++i) w.writeFloatQuantized(p.cameraRot[i], -1.0f, 1.0f, kQuatBits);

    const auto count = static_cast<std::uint16_t>(
        std::min<std::size_t>(p.selection.size(), m_cfg.maxSelectionIds));
    w.writeUInt16(count);
    for (std::uint16_t i = 0; i < count; ++i) w.writeUInt64(p.selection[i]);
}

bool CollabSession::readPresenceBody(BitReader& r, PresenceState& out) const {
    for (int i = 0; i < 3; ++i) {
        if (!r.readFloat(out.cameraPos[i])) return false;
    }
    for (int i = 0; i < 4; ++i) {
        if (!r.readFloatQuantized(out.cameraRot[i], -1.0f, 1.0f, kQuatBits)) return false;
    }

    std::uint16_t count = 0;
    if (!r.readUInt16(count)) return false;
    // Refuse to allocate what the peer claims beyond our own bound.
    if (count > m_cfg.maxSelectionIds) return false;

    out.selection.clear();
    out.selection.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        std::uint64_t id = 0;
        if (!r.readUInt64(id)) return false;
        out.selection.push_back(id);
    }
    out.valid = true;
    return true;
}

void CollabSession::sendLocalPresence() {
    if (!m_net) return;

    if (m_role == NetRole::Host) {
        // The host relays its own presence directly, stamped with its id.
        BitWriter w;
        w.writeUInt32(m_localId);
        writePresenceBody(w, m_localPresence);
        m_net->broadcast(kMsgPresenceRelay, w, SendMode::Unreliable);
    } else {
        // Deliberately no id: the host derives it from the connection, so a
        // client cannot publish presence on someone else's behalf.
        BitWriter w;
        writePresenceBody(w, m_localPresence);
        if (!m_net->connections().empty()) {
            m_net->send(m_net->connections().front(), kMsgPresenceUpdate, w,
                        SendMode::Unreliable);
        }
    }
}

void CollabSession::handlePresenceUpdate(ConnectionId conn, BitReader& r) {
    // Host side. The sender's identity comes from the connection it arrived on,
    // never from the payload.
    const ParticipantId id = participantForConnection(conn);
    if (id == kInvalidParticipant) return;   // not a joined participant

    PresenceState state;
    if (!readPresenceBody(r, state)) return;

    applyPresence(id, state);
    if (m_onPresence) m_onPresence(id, *presenceOf(id));

    // Fan out to everyone else, now stamped with the authoritative id.
    BitWriter w;
    w.writeUInt32(id);
    writePresenceBody(w, state);
    for (const ConnectionId other : m_net->connections()) {
        if (other != conn) m_net->send(other, kMsgPresenceRelay, w, SendMode::Unreliable);
    }
}

void CollabSession::handlePresenceRelay(BitReader& r) {
    // Client side: the host has already vouched for the id.
    std::uint32_t id = 0;
    if (!r.readUInt32(id)) return;
    if (id == m_localId) return;   // our own presence echoed back

    PresenceState state;
    if (!readPresenceBody(r, state)) return;

    applyPresence(id, state);
    if (m_onPresence) m_onPresence(id, *presenceOf(id));
}

} // namespace HE::Net
