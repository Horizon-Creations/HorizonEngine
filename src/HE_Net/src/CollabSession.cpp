#include "Net/CollabSession.h"

#include "NetLog.h"

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
constexpr MessageId kMsgLockRequest       = kFirstUserMessage + 10;  // client → host
constexpr MessageId kMsgLockRelease       = kFirstUserMessage + 11;  // client → host
constexpr MessageId kMsgLockUpdate        = kFirstUserMessage + 12;  // host → clients
constexpr MessageId kMsgLockDenied        = kFirstUserMessage + 13;  // host → one client
constexpr MessageId kMsgLockTable         = kFirstUserMessage + 14;  // host → joiner
constexpr MessageId kMsgTransformUpdate   = kFirstUserMessage + 15;  // client → host
constexpr MessageId kMsgTransformRelay    = kFirstUserMessage + 16;  // host → clients
constexpr MessageId kMsgAssetBegin        = kFirstUserMessage + 17;
constexpr MessageId kMsgAssetChunk        = kFirstUserMessage + 18;
constexpr MessageId kMsgAssetEnd          = kFirstUserMessage + 19;
constexpr MessageId kMsgStructuralUpdate  = kFirstUserMessage + 20;  // client → host
constexpr MessageId kMsgStructuralRelay   = kFirstUserMessage + 21;  // host → clients
constexpr MessageId kMsgComponentsUpdate  = kFirstUserMessage + 22;  // client → host
constexpr MessageId kMsgComponentsRelay   = kFirstUserMessage + 23;  // host → clients

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
        m_net->on(kMsgLockRequest, [this](ConnectionId conn, BitReader& r) {
            handleLockRequest(conn, r);
        });
        m_net->on(kMsgLockRelease, [this](ConnectionId conn, BitReader& r) {
            handleLockRelease(conn, r);
        });
        m_net->on(kMsgTransformUpdate, [this](ConnectionId conn, BitReader& r) {
            handleTransformUpdate(conn, r);
        });
        m_net->on(kMsgAssetBegin, [this](ConnectionId conn, BitReader& r) {
            handleAssetUpdate(conn, r);
        });
        m_net->on(kMsgStructuralUpdate, [this](ConnectionId conn, BitReader& r) {
            handleStructuralUpdate(conn, r);
        });
        m_net->on(kMsgComponentsUpdate, [this](ConnectionId conn, BitReader& r) {
            handleComponentsUpdate(conn, r);
        });
    } else {
        m_net->on(kMsgJoinAccepted, [this](ConnectionId, BitReader& r) { handleJoinAccepted(r); });
        m_net->on(kMsgJoinRejected, [this](ConnectionId, BitReader& r) { handleJoinRejected(r); });
        m_net->on(kMsgSnapshotBegin, [this](ConnectionId, BitReader& r) { handleSnapshotBegin(r); });
        m_net->on(kMsgSnapshotChunk, [this](ConnectionId, BitReader& r) { handleSnapshotChunk(r); });
        m_net->on(kMsgSnapshotEnd,   [this](ConnectionId, BitReader&)   { handleSnapshotEnd(); });
        m_net->on(kMsgPresenceRelay, [this](ConnectionId, BitReader& r) { handlePresenceRelay(r); });
        m_net->on(kMsgLockUpdate, [this](ConnectionId, BitReader& r) { handleLockUpdate(r); });
        m_net->on(kMsgLockDenied, [this](ConnectionId, BitReader& r) { handleLockDenied(r); });
        m_net->on(kMsgLockTable,  [this](ConnectionId, BitReader& r) { handleLockTable(r); });
        m_net->on(kMsgTransformRelay, [this](ConnectionId, BitReader& r) {
            handleTransformRelay(r);
        });
        m_net->on(kMsgAssetBegin, [this](ConnectionId, BitReader& r) { handleAssetRelay(r); });
        m_net->on(kMsgStructuralRelay, [this](ConnectionId, BitReader& r) {
            handleStructuralRelay(r);
        });
        m_net->on(kMsgComponentsRelay, [this](ConnectionId, BitReader& r) {
            handleComponentsRelay(r);
        });

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

    // Chunk/End are shaped the same for both roles, so they register once.
    installAssetChunkHandlers();
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
    if (!r.readUInt16(version) || !r.readString(name)) {
        HE_LOG_WARN(Net, "Collab: malformed join request on conn %llu",
                    static_cast<unsigned long long>(conn));
        return;
    }
    HE_LOG_INFO(Net, "Collab: join request from \"%s\" (protocol v%u) on conn %llu",
                name.c_str(), static_cast<unsigned>(version),
                static_cast<unsigned long long>(conn));

    const auto reject = [&](JoinRejectReason reason) {
        BitWriter w;
        w.writeByte(static_cast<std::uint8_t>(reason));
        m_net->send(conn, kMsgJoinRejected, w);
    };

    if (version != kCollabProtocolVersion) {
        HE_LOG_WARN(Net, "Collab: rejecting \"%s\" — collab protocol v%u, we speak v%u",
                    name.c_str(), static_cast<unsigned>(version),
                    static_cast<unsigned>(kCollabProtocolVersion));
        reject(JoinRejectReason::VersionMismatch);
        return;
    }
    if (m_participants.size() >= m_cfg.maxParticipants) {
        HE_LOG_WARN(Net, "Collab: rejecting \"%s\" — session is full (%zu of %zu)",
                    name.c_str(), m_participants.size(), m_cfg.maxParticipants);
        reject(JoinRejectReason::SessionFull);
        return;
    }

    // Capture the state *before* admitting the peer: if the host cannot produce
    // a snapshot, the joiner would sit with an empty scene while believing it is
    // in sync. Better to refuse the join outright.
    std::vector<std::uint8_t> snapshot;
    if (!m_state || !m_state->captureSnapshot(snapshot)) {
        HE_LOG_ERROR(Net, "Collab: rejecting \"%s\" — could not capture a scene snapshot%s",
                     name.c_str(), m_state ? "" : " (no state provider installed)");
        reject(JoinRejectReason::SnapshotFailed);
        return;
    }
    if (snapshot.size() > m_cfg.maxSnapshotBytes) {
        HE_LOG_ERROR(Net, "Collab: rejecting \"%s\" — snapshot is %s, over the %s limit",
                     name.c_str(), detail::logBytes(snapshot.size()).c_str(),
                     detail::logBytes(m_cfg.maxSnapshotBytes).c_str());
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
    HE_LOG_INFO(Net, "Collab: sent snapshot to \"%s\" — %s in %u chunk(s) at sequence %llu",
                name.c_str(), detail::logBytes(total).c_str(), chunks,
                static_cast<unsigned long long>(m_sequence));

    // Now register the joiner and tell everyone else.
    m_participants.push_back(joiner);
    m_connToParticipant.emplace_back(conn, id);

    // Hand over the current lock table, or a late arrival would believe every
    // asset is free and immediately collide with whoever is already editing.
    {
        BitWriter table;
        writeLockTable(table);
        m_net->send(conn, kMsgLockTable, table);
    }

    BitWriter announce;
    announce.writeUInt32(id);
    announce.writeString(name);
    for (const ConnectionId other : m_net->connections()) {
        if (other != conn) m_net->send(other, kMsgParticipantJoined, announce);
    }

    HE_LOG_INFO(Net, "Collab: \"%s\" joined as participant %u (%zu in session)",
                name.c_str(), static_cast<unsigned>(id), m_participants.size());
    if (m_onJoin) m_onJoin(joiner);
}

void CollabSession::onPeerDisconnected(ConnectionId conn) {
    if (m_role != NetRole::Host) {
        // Client: the link to the host is gone, so the session is over.
        m_joined   = false;
        m_joinSent = false;
        m_participants.clear();
        m_presence.clear();
        m_locks.clear();
        m_everSentPresence = false;
        HE_LOG_WARN(Net, "Collab: lost the connection to the host — session over");
        return;
    }

    const auto it = std::find_if(m_connToParticipant.begin(), m_connToParticipant.end(),
                                 [conn](const auto& e) { return e.first == conn; });
    if (it == m_connToParticipant.end()) {
        HE_LOG_DEBUG(Net, "Collab: conn %llu dropped before completing a join",
                     static_cast<unsigned long long>(conn));
        return;   // never completed a join
    }

    const ParticipantId id = it->second;
    m_connToParticipant.erase(it);

    // Free their locks BEFORE removing them from the roster, so the broadcast
    // still reaches everyone and nothing they held stays blocked forever.
    releaseLocksOf(id);

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

    HE_LOG_INFO(Net, "Collab: participant %u left (%zu remain)",
                static_cast<unsigned>(id), m_participants.size());
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

    HE_LOG_INFO(Net, "Collab: accepted as participant %u at sequence %llu, %u other(s) "
                     "already present — awaiting the scene snapshot",
                static_cast<unsigned>(m_localId),
                static_cast<unsigned long long>(sequence),
                static_cast<unsigned>(count));
    // Note: not "joined" until the snapshot has been applied — reporting success
    // earlier would let the editor act on a scene it does not have yet.
}

void CollabSession::handleJoinRejected(BitReader& r) {
    std::uint8_t reason = 0;
    r.readByte(reason);
    // Spelled out rather than left as a number: these are the three things a
    // user can actually be told when a join does not go through.
    const char* text = "unknown reason";
    switch (static_cast<JoinRejectReason>(reason)) {
    case JoinRejectReason::VersionMismatch: text = "collab protocol mismatch (different engine build)"; break;
    case JoinRejectReason::SessionFull:     text = "the session is full"; break;
    case JoinRejectReason::SnapshotFailed:  text = "the host could not produce a scene snapshot"; break;
    default: break;
    }
    HE_LOG_WARN(Net, "Collab: join rejected by the host — %s", text);
    m_joined = false;
    if (m_onRejected) m_onRejected(static_cast<JoinRejectReason>(reason));
}

void CollabSession::handleSnapshotBegin(BitReader& r) {
    std::uint32_t total = 0, chunks = 0;
    std::uint64_t sequence = 0;
    if (!r.readUInt32(total) || !r.readUInt32(chunks) || !r.readUInt64(sequence)) return;

    // Never reserve what the peer claims without a bound.
    if (total > m_cfg.maxSnapshotBytes) {
        HE_LOG_ERROR(Net, "Collab: host announced a %s snapshot, over our %s limit — refusing",
                     detail::logBytes(total).c_str(),
                     detail::logBytes(m_cfg.maxSnapshotBytes).c_str());
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

    HE_LOG_INFO(Net, "Collab: receiving snapshot — %s in %u chunk(s)",
                detail::logBytes(total).c_str(), chunks);
    if (m_onProgress) m_onProgress(0, total);
}

void CollabSession::handleSnapshotChunk(BitReader& r) {
    if (!m_snapshotActive) return;

    std::uint32_t index = 0, len = 0;
    if (!r.readUInt32(index) || !r.readUInt32(len)) return;

    // Refuse a chunk that would push the payload past the announced size, rather
    // than growing the buffer to whatever arrives.
    if (len == 0 || m_snapshotReceived + len > m_snapshotExpected) {
        HE_LOG_ERROR(Net, "Collab: snapshot chunk %u would overrun the announced size "
                          "(%u + %u > %u) — aborting the transfer",
                     index, m_snapshotReceived, len, m_snapshotExpected);
        m_snapshotActive = false;
        if (m_onRejected) m_onRejected(JoinRejectReason::SnapshotFailed);
        return;
    }

    const std::size_t offset = m_snapshotBuf.size();
    m_snapshotBuf.resize(offset + len);
    if (!r.readBytes(m_snapshotBuf.data() + offset, len)) {
        HE_LOG_ERROR(Net, "Collab: snapshot chunk %u is truncated — aborting the transfer", index);
        m_snapshotBuf.resize(offset);   // truncated frame
        m_snapshotActive = false;
        if (m_onRejected) m_onRejected(JoinRejectReason::SnapshotFailed);
        return;
    }

    m_snapshotReceived += len;
    HE_LOG_TRACE(Net, "Collab: snapshot chunk %u (%s), %s of %s received",
                 index, detail::logBytes(len).c_str(),
                 detail::logBytes(m_snapshotReceived).c_str(),
                 detail::logBytes(m_snapshotExpected).c_str());
    if (m_onProgress) m_onProgress(m_snapshotReceived, m_snapshotExpected);
}

void CollabSession::handleSnapshotEnd() {
    if (!m_snapshotActive) return;
    m_snapshotActive = false;

    // An incomplete transfer must not be applied — a partially deserialized
    // scene is worse than none.
    if (m_snapshotReceived != m_snapshotExpected) {
        HE_LOG_ERROR(Net, "Collab: snapshot ended early — %s of %s arrived; not applying a "
                          "partial scene",
                     detail::logBytes(m_snapshotReceived).c_str(),
                     detail::logBytes(m_snapshotExpected).c_str());
        if (m_onRejected) m_onRejected(JoinRejectReason::SnapshotFailed);
        return;
    }
    if (!m_state || !m_state->applySnapshot(m_snapshotBuf)) {
        HE_LOG_ERROR(Net, "Collab: the snapshot arrived intact but could not be applied — "
                          "the scene it describes may need assets this project lacks");
        if (m_onRejected) m_onRejected(JoinRejectReason::SnapshotFailed);
        return;
    }

    m_snapshotBuf.clear();
    m_snapshotBuf.shrink_to_fit();

    m_joined = true;
    HE_LOG_INFO(Net, "Collab: snapshot applied — in session as participant %u",
                static_cast<unsigned>(m_localId));
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
    if (id == kInvalidParticipant) {
        // Someone is publishing presence over a connection that never joined —
        // the exact shape a spoofing attempt would take, hence Warning.
        HE_LOG_WARN(Net, "Presence from conn %llu, which is not a joined participant "
                         "— ignored",
                    static_cast<unsigned long long>(conn));
        return;   // not a joined participant
    }

    PresenceState state;
    if (!readPresenceBody(r, state)) {
        HE_LOG_WARN(Net, "Malformed presence from participant %u",
                    static_cast<unsigned>(id));
        return;
    }
    // Trace only: this arrives at up to 10 Hz per participant.
    HE_LOG_TRACE(Net, "Presence from participant %u (%zu selected)",
                 static_cast<unsigned>(id), state.selection.size());

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
    if (!readPresenceBody(r, state)) {
        HE_LOG_WARN(Net, "Malformed presence relayed for participant %u",
                    static_cast<unsigned>(id));
        return;
    }
    HE_LOG_TRACE(Net, "Presence relay for participant %u (%zu selected)",
                 static_cast<unsigned>(id), state.selection.size());

    applyPresence(id, state);
    if (m_onPresence) m_onPresence(id, *presenceOf(id));
}

// ─── Component edits ─────────────────────────────────────────────────────────

namespace {
void writeComponentBody(BitWriter& w, std::uint64_t netId,
                        const std::vector<std::uint8_t>& blob) {
    w.writeUInt64(netId);
    w.writeUInt32(static_cast<std::uint32_t>(blob.size()));
    if (!blob.empty()) w.writeBytes(blob.data(), blob.size());
}

bool readComponentBody(BitReader& r, std::uint64_t& netId,
                       std::vector<std::uint8_t>& blob, std::uint32_t maxBlob) {
    std::uint32_t len = 0;
    if (!r.readUInt64(netId) || !r.readUInt32(len)) return false;
    if (len == 0 || len > maxBlob) return false;   // never allocate on trust
    blob.resize(len);
    return r.readBytes(blob.data(), len);
}
} // namespace

bool CollabSession::sendComponents(const ComponentUpdate& update) {
    if (!m_net || !m_joined) return false;
    if (update.blob.empty() || update.blob.size() > m_cfg.maxSnapshotBytes) return false;
    // The lock is what makes a wholesale overwrite safe: nobody else can be
    // editing this entity, so nothing of theirs can be lost.
    if (!ownsLock(update.netId)) return false;

    if (m_role == NetRole::Host) {
        BitWriter w;
        w.writeUInt32(m_localId);
        writeComponentBody(w, update.netId, update.blob);
        m_net->broadcast(kMsgComponentsRelay, w);
        return true;
    }
    if (m_net->connections().empty()) return false;

    BitWriter w;
    writeComponentBody(w, update.netId, update.blob);   // no id — host stamps it
    m_net->send(m_net->connections().front(), kMsgComponentsUpdate, w);
    return true;
}

void CollabSession::handleComponentsUpdate(ConnectionId conn, BitReader& r) {
    const ParticipantId sender = participantForConnection(conn);
    if (sender == kInvalidParticipant) return;

    ComponentUpdate u;
    if (!readComponentBody(r, u.netId, u.blob, m_cfg.maxSnapshotBytes)) return;

    // Re-check authority on arrival rather than trusting the sender.
    const LockInfo* lock = lockFor(u.netId);
    if (!lock || lock->owner != sender) return;

    if (m_onComponents) m_onComponents(sender, u);

    BitWriter w;
    w.writeUInt32(sender);
    writeComponentBody(w, u.netId, u.blob);
    for (const ConnectionId other : m_net->connections()) {
        if (other != conn) m_net->send(other, kMsgComponentsRelay, w);
    }
}

void CollabSession::handleComponentsRelay(BitReader& r) {
    std::uint32_t sender = 0;
    if (!r.readUInt32(sender)) return;
    if (sender == m_localId) return;   // our own edit echoed back

    ComponentUpdate u;
    if (!readComponentBody(r, u.netId, u.blob, m_cfg.maxSnapshotBytes)) return;
    if (m_onComponents) m_onComponents(sender, u);
}

// ─── Structural changes ──────────────────────────────────────────────────────

void CollabSession::writeStructuralBody(BitWriter& w, const StructuralChange& c) {
    w.writeByte(static_cast<std::uint8_t>(c.kind));
    w.writeUInt64(c.netId);
    w.writeUInt64(c.parentNet);
    w.writeUInt32(static_cast<std::uint32_t>(c.blob.size()));
    if (!c.blob.empty()) w.writeBytes(c.blob.data(), c.blob.size());
}

bool CollabSession::readStructuralBody(BitReader& r, StructuralChange& out,
                                       std::uint32_t maxBlob) {
    std::uint8_t kind = 0;
    if (!r.readByte(kind)) return false;
    if (kind > static_cast<std::uint8_t>(StructuralChange::Kind::Reparented)) return false;
    out.kind = static_cast<StructuralChange::Kind>(kind);

    std::uint32_t len = 0;
    if (!r.readUInt64(out.netId) || !r.readUInt64(out.parentNet) ||
        !r.readUInt32(len)) {
        return false;
    }
    // Never allocate what the peer claims beyond our own bound.
    if (len > maxBlob) return false;

    out.blob.clear();
    if (len > 0) {
        out.blob.resize(len);
        if (!r.readBytes(out.blob.data(), len)) return false;
    }
    return true;
}

bool CollabSession::sendStructural(const StructuralChange& change) {
    if (!m_net || !m_joined) return false;
    if (change.blob.size() > m_cfg.maxSnapshotBytes) return false;

    // Deleting or reparenting something someone else is editing would yank it
    // out from under them, so those need the lock. Creating does not: a brand
    // new entity cannot be held by anyone yet.
    if (change.kind != StructuralChange::Kind::Created) {
        const LockInfo* lock = lockFor(change.netId);
        if (lock && lock->owner != m_localId) return false;
    }

    if (m_role == NetRole::Host) {
        BitWriter w;
        w.writeUInt32(m_localId);
        writeStructuralBody(w, change);
        m_net->broadcast(kMsgStructuralRelay, w);
        return true;
    }
    if (m_net->connections().empty()) return false;

    BitWriter w;
    writeStructuralBody(w, change);   // no id — the host stamps it
    m_net->send(m_net->connections().front(), kMsgStructuralUpdate, w);
    return true;
}

void CollabSession::handleStructuralUpdate(ConnectionId conn, BitReader& r) {
    const ParticipantId sender = participantForConnection(conn);
    if (sender == kInvalidParticipant) return;

    StructuralChange c;
    if (!readStructuralBody(r, c, m_cfg.maxSnapshotBytes)) return;

    // Re-check authority on arrival rather than trusting the sender.
    if (c.kind != StructuralChange::Kind::Created) {
        const LockInfo* lock = lockFor(c.netId);
        if (lock && lock->owner != sender) return;
    }

    // A destroyed entity's lock has to go, or it would block a subject that no
    // longer exists for the rest of the session.
    if (c.kind == StructuralChange::Kind::Destroyed && lockFor(c.netId)) {
        const LockInfo copy = *lockFor(c.netId);
        dropLock(c.netId);
        broadcastLock(c.netId, false, kInvalidConnection);
        if (m_onLockChanged) m_onLockChanged(copy, false);
    }

    if (m_onStructural) m_onStructural(sender, c);

    BitWriter w;
    w.writeUInt32(sender);
    writeStructuralBody(w, c);
    for (const ConnectionId other : m_net->connections()) {
        if (other != conn) m_net->send(other, kMsgStructuralRelay, w);
    }
}

void CollabSession::handleStructuralRelay(BitReader& r) {
    std::uint32_t sender = 0;
    if (!r.readUInt32(sender)) return;
    if (sender == m_localId) return;   // our own change echoed back

    StructuralChange c;
    if (!readStructuralBody(r, c, m_cfg.maxSnapshotBytes)) {
        HE_LOG_WARN(Net, "Collab: malformed structural change relayed from participant %u",
                    static_cast<unsigned>(sender));
        return;
    }
    HE_LOG_DEBUG(Net, "Collab: structural change from participant %u (net id %llu, kind %d)",
                 static_cast<unsigned>(sender),
                 static_cast<unsigned long long>(c.netId), static_cast<int>(c.kind));
    if (m_onStructural) m_onStructural(sender, c);
}

// ─── Authored-asset sync ─────────────────────────────────────────────────────

bool CollabSession::sendAsset(std::uint64_t subject, const std::string& path,
                              const std::vector<std::uint8_t>& bytes) {
    if (!m_net || !m_joined) return false;
    // Same authority rule as transforms: you may only publish what you hold.
    if (!ownsLock(subject)) {
        // The everyday cause of "my change did not show up for the others":
        // the asset was saved without holding its lock.
        HE_LOG_WARN(Net, "Not sending asset \"%s\": we do not hold lock 0x%016llx",
                    path.c_str(), static_cast<unsigned long long>(subject));
        return false;
    }
    if (bytes.size() > m_cfg.maxSnapshotBytes) {
        HE_LOG_ERROR(Net, "Not sending asset \"%s\": %s exceeds the %s limit",
                     path.c_str(), detail::logBytes(bytes.size()).c_str(),
                     detail::logBytes(m_cfg.maxSnapshotBytes).c_str());
        return false;
    }
    HE_LOG_INFO(Net, "Sending asset \"%s\" (%s)", path.c_str(),
                detail::logBytes(bytes.size()).c_str());

    AssetUpdate a;
    a.subject = subject;
    a.path    = path;
    a.bytes   = bytes;

    if (m_role == NetRole::Host) {
        for (const ConnectionId c : m_net->connections()) sendAssetChunks(c, m_localId, a);
        return true;
    }
    if (m_net->connections().empty()) {
        HE_LOG_WARN(Net, "Cannot send asset \"%s\": no connection to the host", path.c_str());
        return false;
    }
    // Client → host: no participant id, the host stamps it.
    sendAssetChunks(m_net->connections().front(), kInvalidParticipant, a);
    return true;
}

void CollabSession::sendAssetChunks(ConnectionId conn, ParticipantId from,
                                    const AssetUpdate& a) {
    const bool stamped = (from != kInvalidParticipant);
    const auto total    = static_cast<std::uint32_t>(a.bytes.size());
    const std::uint32_t chunkSize = std::max<std::uint32_t>(1, m_cfg.chunkBytes);
    const std::uint32_t chunks    = (total + chunkSize - 1) / chunkSize;

    BitWriter begin;
    if (stamped) begin.writeUInt32(from);
    begin.writeUInt64(a.subject);
    begin.writeString(a.path);
    begin.writeUInt32(total);
    begin.writeUInt32(chunks);
    m_net->send(conn, kMsgAssetBegin, begin);

    for (std::uint32_t i = 0; i < chunks; ++i) {
        const std::uint32_t offset = i * chunkSize;
        const std::uint32_t len    = std::min(chunkSize, total - offset);
        BitWriter chunk;
        if (stamped) chunk.writeUInt32(from);
        chunk.writeUInt32(len);
        chunk.writeBytes(a.bytes.data() + offset, len);
        m_net->send(conn, kMsgAssetChunk, chunk);
    }

    BitWriter end;
    if (stamped) end.writeUInt32(from);
    m_net->send(conn, kMsgAssetEnd, end);
}

void CollabSession::handleAssetUpdate(ConnectionId conn, BitReader& r) {
    // Host side. Identity comes from the connection, never the payload.
    const ParticipantId sender = participantForConnection(conn);
    if (sender == kInvalidParticipant) return;

    AssetUpdate header;
    std::uint32_t total = 0, chunks = 0;
    if (!readAssetHeader(r, header, total, chunks)) return;

    // Re-check authority on arrival rather than trusting the sender.
    const LockInfo* lock = lockFor(header.subject);
    if (!lock || lock->owner != sender) {
        HE_LOG_WARN(Net, "Refusing asset \"%s\" from participant %u — it does not hold "
                         "lock 0x%016llx",
                    header.path.c_str(), static_cast<unsigned>(sender),
                    static_cast<unsigned long long>(header.subject));
        return;
    }
    if (total > m_cfg.maxSnapshotBytes) {
        HE_LOG_ERROR(Net, "Refusing asset \"%s\" from participant %u — announced %s, over "
                          "the %s limit",
                     header.path.c_str(), static_cast<unsigned>(sender),
                     detail::logBytes(total).c_str(),
                     detail::logBytes(m_cfg.maxSnapshotBytes).c_str());
        return;
    }
    HE_LOG_INFO(Net, "Receiving asset \"%s\" from participant %u (%s in %u chunk(s))",
                header.path.c_str(), static_cast<unsigned>(sender),
                detail::logBytes(total).c_str(), chunks);

    AssetAssembly asm_;
    asm_.from     = conn;
    asm_.sender   = sender;
    asm_.subject  = header.subject;
    asm_.path     = header.path;
    asm_.expected = total;
    asm_.bytes.reserve(total);

    // Replace any half-finished transfer from the same peer: a second Begin
    // means the first one will never complete.
    m_assetAssembly.erase(
        std::remove_if(m_assetAssembly.begin(), m_assetAssembly.end(),
                       [conn](const AssetAssembly& e) { return e.from == conn; }),
        m_assetAssembly.end());
    m_assetAssembly.push_back(std::move(asm_));
}

bool CollabSession::readAssetHeader(BitReader& r, AssetUpdate& out,
                                    std::uint32_t& outTotal,
                                    std::uint32_t& outChunks) const {
    return r.readUInt64(out.subject) && r.readString(out.path) &&
           r.readUInt32(outTotal) && r.readUInt32(outChunks);
}

void CollabSession::handleAssetRelay(BitReader& r) {
    // Client side: the host has already vouched for the sender.
    std::uint32_t sender = 0;
    if (!r.readUInt32(sender)) return;
    if (sender == m_localId) return;   // our own save echoed back

    AssetUpdate header;
    std::uint32_t total = 0, chunks = 0;
    if (!readAssetHeader(r, header, total, chunks)) return;
    if (total > m_cfg.maxSnapshotBytes) return;

    AssetAssembly asm_;
    asm_.from     = kInvalidConnection;
    asm_.sender   = sender;
    asm_.subject  = header.subject;
    asm_.path     = header.path;
    asm_.expected = total;
    asm_.bytes.reserve(total);

    m_assetAssembly.erase(
        std::remove_if(m_assetAssembly.begin(), m_assetAssembly.end(),
                       [sender](const AssetAssembly& e) { return e.sender == sender; }),
        m_assetAssembly.end());
    m_assetAssembly.push_back(std::move(asm_));
}

namespace {
// Chunk/End bodies differ only in whether a participant id was stamped on the
// front, so both roles share the tail handling.
struct AssetFrameKey { ConnectionId conn; ParticipantId sender; };
} // namespace

void CollabSession::installAssetChunkHandlers() {
    const bool host = (m_role == NetRole::Host);

    m_net->on(kMsgAssetChunk, [this, host](ConnectionId conn, BitReader& r) {
        ParticipantId sender = kInvalidParticipant;
        if (!host) { std::uint32_t s = 0; if (!r.readUInt32(s)) return; sender = s; }
        else       { sender = participantForConnection(conn); }
        if (!host && sender == m_localId) return;   // our own echo

        std::uint32_t len = 0;
        if (!r.readUInt32(len) || len == 0) return;

        for (auto& a : m_assetAssembly) {
            const bool mine = host ? (a.from == conn) : (a.sender == sender);
            if (!mine) continue;

            // Never grow past what the sender announced.
            if (a.bytes.size() + len > a.expected) { a.expected = 0; return; }
            const std::size_t offset = a.bytes.size();
            a.bytes.resize(offset + len);
            if (!r.readBytes(a.bytes.data() + offset, len)) {
                a.bytes.resize(offset);
                a.expected = 0;   // mark broken; End will discard it
            }
            return;
        }
    });

    m_net->on(kMsgAssetEnd, [this, host](ConnectionId conn, BitReader& r) {
        ParticipantId sender = kInvalidParticipant;
        if (!host) { std::uint32_t s = 0; if (!r.readUInt32(s)) return; sender = s; }
        else       { sender = participantForConnection(conn); }
        if (!host && sender == m_localId) return;

        for (std::size_t i = 0; i < m_assetAssembly.size(); ++i) {
            AssetAssembly& a = m_assetAssembly[i];
            const bool mine = host ? (a.from == conn) : (a.sender == sender);
            if (!mine) continue;

            // An incomplete transfer must not be applied — a truncated asset
            // file is worse than no update at all.
            const bool complete = a.expected > 0 && a.bytes.size() == a.expected;
            if (complete) {
                AssetUpdate up;
                up.subject = a.subject;
                up.path    = a.path;
                up.bytes   = a.bytes;

                if (m_onAsset) m_onAsset(a.sender, up);

                // Host also fans it out to everyone else.
                if (host) {
                    for (const ConnectionId other : m_net->connections()) {
                        if (other != conn) sendAssetChunks(other, a.sender, up);
                    }
                }
            }
            m_assetAssembly.erase(m_assetAssembly.begin() +
                                  static_cast<std::ptrdiff_t>(i));
            return;
        }
    });
}

// ─── Live transform deltas ───────────────────────────────────────────────────

void CollabSession::writeTransformBody(BitWriter& w, const TransformDelta& d) {
    w.writeUInt64(d.subject);
    // Full float precision throughout. Unlike a presence gizmo this IS the
    // authoritative value, a scene can span kilometres, and Euler angles are not
    // bounded to a range that would quantize cleanly.
    for (int i = 0; i < 3; ++i) w.writeFloat(d.position[i]);
    for (int i = 0; i < 3; ++i) w.writeFloat(d.rotation[i]);
    for (int i = 0; i < 3; ++i) w.writeFloat(d.scale[i]);
}

bool CollabSession::readTransformBody(BitReader& r, TransformDelta& out) {
    if (!r.readUInt64(out.subject)) return false;
    for (int i = 0; i < 3; ++i) { if (!r.readFloat(out.position[i])) return false; }
    for (int i = 0; i < 3; ++i) { if (!r.readFloat(out.rotation[i])) return false; }
    for (int i = 0; i < 3; ++i) { if (!r.readFloat(out.scale[i])) return false; }
    return true;
}

bool CollabSession::sendTransform(const TransformDelta& delta) {
    if (!m_net || !m_joined) return false;

    // Holding the lock is what makes this safe: nobody else can be moving the
    // same entity, so there is no conflict to resolve — which is exactly why
    // locks come before live deltas rather than after.
    if (!ownsLock(delta.subject)) return false;

    if (m_role == NetRole::Host) {
        BitWriter w;
        w.writeUInt32(m_localId);
        writeTransformBody(w, delta);
        m_net->broadcast(kMsgTransformRelay, w, SendMode::Unreliable);
        return true;
    }

    if (m_net->connections().empty()) return false;
    BitWriter w;
    writeTransformBody(w, delta);   // no id: the host stamps it
    m_net->send(m_net->connections().front(), kMsgTransformUpdate, w,
                SendMode::Unreliable);
    return true;
}

void CollabSession::handleTransformUpdate(ConnectionId conn, BitReader& r) {
    const ParticipantId sender = participantForConnection(conn);
    if (sender == kInvalidParticipant) return;

    TransformDelta d;
    if (!readTransformBody(r, d)) return;

    // Re-check authority here rather than trusting the sender: a client that
    // lost its lock (or never had it) must not be able to move things anyway.
    const LockInfo* lock = lockFor(d.subject);
    if (!lock || lock->owner != sender) return;

    if (m_onTransform) m_onTransform(sender, d);

    BitWriter w;
    w.writeUInt32(sender);
    writeTransformBody(w, d);
    for (const ConnectionId other : m_net->connections()) {
        if (other != conn) m_net->send(other, kMsgTransformRelay, w, SendMode::Unreliable);
    }
}

void CollabSession::handleTransformRelay(BitReader& r) {
    std::uint32_t sender = 0;
    if (!r.readUInt32(sender)) return;
    if (sender == m_localId) return;   // our own change echoed back

    TransformDelta d;
    if (!readTransformBody(r, d)) return;
    if (m_onTransform) m_onTransform(sender, d);
}

// ─── Locks ───────────────────────────────────────────────────────────────────

const LockInfo* CollabSession::lockFor(std::uint64_t subject) const {
    for (const auto& l : m_locks) {
        if (l.subject == subject) return &l;
    }
    return nullptr;
}

bool CollabSession::ownsLock(std::uint64_t subject) const {
    const LockInfo* l = lockFor(subject);
    return l && l->owner == m_localId;
}

void CollabSession::grantLock(std::uint64_t subject, ParticipantId owner,
                              const std::string& name) {
    for (auto& l : m_locks) {
        if (l.subject == subject) { l.owner = owner; l.ownerName = name; return; }
    }
    m_locks.push_back(LockInfo{ subject, owner, name });
}

void CollabSession::dropLock(std::uint64_t subject) {
    m_locks.erase(std::remove_if(m_locks.begin(), m_locks.end(),
                                 [subject](const LockInfo& l) { return l.subject == subject; }),
                  m_locks.end());
}

void CollabSession::broadcastLock(std::uint64_t subject, bool acquired,
                                  ConnectionId /*except*/) {
    const LockInfo* l = lockFor(subject);

    BitWriter w;
    w.writeUInt64(subject);
    w.writeBool(acquired);
    w.writeUInt32(acquired && l ? l->owner : kInvalidParticipant);
    w.writeString(acquired && l ? l->ownerName : std::string{});

    // Sent to everyone, including the requester: the grant IS its confirmation,
    // so there is no separate "you got it" message to keep in sync with this one.
    m_net->broadcast(kMsgLockUpdate, w);
}

bool CollabSession::requestLock(std::uint64_t subject) {
    if (!m_net || !m_joined) return false;

    if (m_role == NetRole::Host) {
        // The host is the authority, so it answers itself — no round trip, and
        // no window in which two peers both believe they hold it.
        const LockInfo* existing = lockFor(subject);
        if (existing && existing->owner != m_localId) {
            HE_LOG_INFO(Net, "Lock 0x%016llx denied to ourselves — held by \"%s\" (%u)",
                        static_cast<unsigned long long>(subject),
                        existing->ownerName.c_str(),
                        static_cast<unsigned>(existing->owner));
            if (m_onLockDenied) m_onLockDenied(subject, LockDenyReason::HeldByOther);
            return false;
        }
        HE_LOG_DEBUG(Net, "Lock 0x%016llx taken (host, self-granted)",
                     static_cast<unsigned long long>(subject));
        grantLock(subject, m_localId, m_cfg.displayName);
        broadcastLock(subject, true, kInvalidConnection);
        if (m_onLockChanged) m_onLockChanged(*lockFor(subject), true);
        return true;
    }

    if (m_net->connections().empty()) {
        HE_LOG_WARN(Net, "Cannot request lock 0x%016llx: no connection to the host",
                    static_cast<unsigned long long>(subject));
        return false;
    }
    HE_LOG_DEBUG(Net, "Requesting lock 0x%016llx from the host",
                 static_cast<unsigned long long>(subject));
    BitWriter w;
    w.writeUInt64(subject);
    m_net->send(m_net->connections().front(), kMsgLockRequest, w);
    return true;
}

void CollabSession::releaseLock(std::uint64_t subject) {
    if (!m_net || !m_joined) return;

    if (m_role == NetRole::Host) {
        const LockInfo* existing = lockFor(subject);
        if (!existing || existing->owner != m_localId) return;   // not ours to give up
        const LockInfo copy = *existing;
        dropLock(subject);
        broadcastLock(subject, false, kInvalidConnection);
        if (m_onLockChanged) m_onLockChanged(copy, false);
        return;
    }

    if (m_net->connections().empty()) return;
    BitWriter w;
    w.writeUInt64(subject);
    m_net->send(m_net->connections().front(), kMsgLockRelease, w);
}

// ── Host side ──

void CollabSession::handleLockRequest(ConnectionId conn, BitReader& r) {
    std::uint64_t subject = 0;
    if (!r.readUInt64(subject)) return;

    const ParticipantId requester = participantForConnection(conn);
    if (requester == kInvalidParticipant) {
        HE_LOG_WARN(Net, "Lock request for 0x%016llx from conn %llu, which is not in the "
                         "session — denied",
                    static_cast<unsigned long long>(subject),
                    static_cast<unsigned long long>(conn));
        BitWriter w;
        w.writeUInt64(subject);
        w.writeByte(static_cast<std::uint8_t>(LockDenyReason::NotInSession));
        m_net->send(conn, kMsgLockDenied, w);
        return;
    }

    const LockInfo* existing = lockFor(subject);
    if (existing && existing->owner != requester) {
        // Someone already has it. Refusing is the entire point — this is the
        // moment a second person would otherwise start editing the same thing.
        HE_LOG_INFO(Net, "Lock 0x%016llx denied to participant %u — held by \"%s\" (%u)",
                    static_cast<unsigned long long>(subject),
                    static_cast<unsigned>(requester), existing->ownerName.c_str(),
                    static_cast<unsigned>(existing->owner));
        BitWriter w;
        w.writeUInt64(subject);
        w.writeByte(static_cast<std::uint8_t>(LockDenyReason::HeldByOther));
        m_net->send(conn, kMsgLockDenied, w);
        return;
    }

    const Participant* p = findParticipant(requester);
    HE_LOG_INFO(Net, "Lock 0x%016llx granted to \"%s\" (%u)",
                static_cast<unsigned long long>(subject),
                p ? p->name.c_str() : "?", static_cast<unsigned>(requester));
    grantLock(subject, requester, p ? p->name : std::string{});
    broadcastLock(subject, true, kInvalidConnection);
    if (m_onLockChanged) m_onLockChanged(*lockFor(subject), true);
}

void CollabSession::handleLockRelease(ConnectionId conn, BitReader& r) {
    std::uint64_t subject = 0;
    if (!r.readUInt64(subject)) return;

    const ParticipantId requester = participantForConnection(conn);
    const LockInfo* existing = lockFor(subject);
    // Only the holder may release: otherwise anyone could free someone else's
    // lock and edit underneath them.
    if (!existing || existing->owner != requester) {
        // Not necessarily malicious — a stale client can ask twice — but it is
        // exactly the shape an attempt to free someone else's lock would take.
        HE_LOG_WARN(Net, "Participant %u tried to release lock 0x%016llx it does not hold",
                    static_cast<unsigned>(requester),
                    static_cast<unsigned long long>(subject));
        return;
    }

    const LockInfo copy = *existing;
    HE_LOG_INFO(Net, "Lock 0x%016llx released by \"%s\" (%u)",
                static_cast<unsigned long long>(subject), copy.ownerName.c_str(),
                static_cast<unsigned>(requester));
    dropLock(subject);
    broadcastLock(subject, false, kInvalidConnection);
    if (m_onLockChanged) m_onLockChanged(copy, false);
}

void CollabSession::releaseLocksOf(ParticipantId owner) {
    // Everything a departing participant held becomes free again, or their locks
    // would block the rest of the session forever.
    std::vector<std::uint64_t> freed;
    for (const auto& l : m_locks) {
        if (l.owner == owner) freed.push_back(l.subject);
    }
    if (!freed.empty()) {
        HE_LOG_INFO(Net, "Freeing %zu lock(s) held by departing participant %u",
                    freed.size(), static_cast<unsigned>(owner));
    }
    for (const std::uint64_t subject : freed) {
        const LockInfo copy = *lockFor(subject);
        dropLock(subject);
        broadcastLock(subject, false, kInvalidConnection);
        if (m_onLockChanged) m_onLockChanged(copy, false);
    }
}

void CollabSession::writeLockTable(BitWriter& w) const {
    w.writeUInt16(static_cast<std::uint16_t>(m_locks.size()));
    for (const auto& l : m_locks) {
        w.writeUInt64(l.subject);
        w.writeUInt32(l.owner);
        w.writeString(l.ownerName);
    }
}

// ── Client side ──

void CollabSession::handleLockUpdate(BitReader& r) {
    std::uint64_t subject = 0;
    bool acquired = false;
    std::uint32_t owner = 0;
    std::string name;
    if (!r.readUInt64(subject) || !r.readBool(acquired) ||
        !r.readUInt32(owner) || !r.readString(name)) {
        return;
    }

    if (acquired) {
        grantLock(subject, owner, name);
        if (m_onLockChanged) m_onLockChanged(*lockFor(subject), true);
    } else {
        const LockInfo* existing = lockFor(subject);
        const LockInfo copy = existing ? *existing : LockInfo{ subject, owner, name };
        dropLock(subject);
        if (m_onLockChanged) m_onLockChanged(copy, false);
    }
}

void CollabSession::handleLockDenied(BitReader& r) {
    std::uint64_t subject = 0;
    std::uint8_t reason = 0;
    if (!r.readUInt64(subject) || !r.readByte(reason)) return;
    if (m_onLockDenied) m_onLockDenied(subject, static_cast<LockDenyReason>(reason));
}

void CollabSession::handleLockTable(BitReader& r) {
    // Sent once on join, so a late arrival does not start out believing
    // everything is free.
    std::uint16_t count = 0;
    if (!r.readUInt16(count)) return;

    m_locks.clear();
    for (std::uint16_t i = 0; i < count; ++i) {
        LockInfo l;
        std::uint32_t owner = 0;
        if (!r.readUInt64(l.subject) || !r.readUInt32(owner) || !r.readString(l.ownerName)) {
            return;
        }
        l.owner = owner;
        m_locks.push_back(l);
    }
}

} // namespace HE::Net
