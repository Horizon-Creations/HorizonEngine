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
constexpr MessageId kMsgDocDeltasUpdate   = kFirstUserMessage + 24;  // client → host
constexpr MessageId kMsgDocDeltasRelay    = kFirstUserMessage + 25;  // host → clients
constexpr MessageId kMsgLockQuery         = kFirstUserMessage + 26;  // client → host
constexpr MessageId kMsgLockQueryResult   = kFirstUserMessage + 27;  // host → one client
constexpr MessageId kMsgRemoved           = kFirstUserMessage + 28;  // host → one client
// ── Asset creation and the two operations that need a human ──
// A create is small enough to travel whole (an authored stub is bytes, not
// megabytes), so it does NOT use the Begin/Chunk/End machinery — that exists
// for a saved asset which can be any size.
constexpr MessageId kMsgAssetCreate       = kFirstUserMessage + 29;  // client → host
constexpr MessageId kMsgAssetCreateRelay  = kFirstUserMessage + 30;  // host → clients
constexpr MessageId kMsgAssetCreateResult = kFirstUserMessage + 31;  // host → one client
// Delete and rename are REQUESTS. Three ids because the three steps have three
// different audiences: one asks, the host answers that one, and only then does
// everybody hear what happened.
constexpr MessageId kMsgAssetOpRequest    = kFirstUserMessage + 32;  // client → host
constexpr MessageId kMsgAssetOpVerdict    = kFirstUserMessage + 33;  // host → one client
constexpr MessageId kMsgAssetOpApply      = kFirstUserMessage + 34;  // host → clients
// ── "may I have this one?" ──
// Addressed to the HOLDER, not the host: the host knows who that is and does
// the routing, but the decision is not its to make — it is the holder's work
// that would be interrupted. Two ids rather than reusing the pair above,
// because the audiences run the other way: this travels host → holder, and the
// answer comes back holder → host.
constexpr MessageId kMsgAssetEditRequest  = kFirstUserMessage + 35;  // host → the holder
constexpr MessageId kMsgAssetEditAnswer   = kFirstUserMessage + 36;  // holder → host

// Quaternion components are always in [-1, 1], so 16 bits each is ~3e-5 of
// angular resolution — far finer than a camera gizmo can show, at a quarter the
// size of raw floats.
constexpr int kQuatBits = 16;

// Colours travel as three raw bytes wherever a participant does.
void writeColor(BitWriter& w, const ParticipantColor& c) {
    w.writeByte(c.r);
    w.writeByte(c.g);
    w.writeByte(c.b);
}

bool readColor(BitReader& r, ParticipantColor& out) {
    return r.readByte(out.r) && r.readByte(out.g) && r.readByte(out.b);
}

int colorDistanceSq(const ParticipantColor& a, const ParticipantColor& b) {
    const int dr = static_cast<int>(a.r) - static_cast<int>(b.r);
    const int dg = static_cast<int>(a.g) - static_cast<int>(b.g);
    const int db = static_cast<int>(a.b) - static_cast<int>(b.b);
    return dr * dr + dg * dg + db * db;
}

} // namespace

CollabSession::CollabSession(NetSession* net, NetRole role, Config cfg)
    : m_net(net), m_role(role), m_cfg(std::move(cfg)) {
    // An avatar over our own limit is dropped here rather than at every send
    // site: a picture this editor cannot legally put on the wire is one it must
    // not carry around pretending it will.
    if (m_cfg.avatar.size > m_cfg.maxAvatarSize ||
        m_cfg.avatar.rgba.size() !=
            static_cast<std::size_t>(m_cfg.avatar.size) * m_cfg.avatar.size * 4u) {
        if (!m_cfg.avatar.empty()) {
            HE_LOG_WARN(Net, "Collab: discarding a %ux%u profile picture — %s",
                        static_cast<unsigned>(m_cfg.avatar.size),
                        static_cast<unsigned>(m_cfg.avatar.size),
                        m_cfg.avatar.size > m_cfg.maxAvatarSize
                            ? "over this session's size limit"
                            : "its pixel buffer does not match its declared size");
        }
        m_cfg.avatar = Avatar{};
    }

    if (m_role == NetRole::Host) {
        // The host occupies the first slot and is joined from the outset — there
        // is nobody to ask for permission.
        m_localId = m_nextId++;
        Participant self;
        self.id     = m_localId;
        self.name   = m_cfg.displayName;
        self.isHost = true;
        self.avatar = m_cfg.avatar;
        // Nobody to clash with yet, so the host always gets what it asked for —
        // but it still goes through assignColor, which is what gives a host that
        // expressed no preference a colour at all.
        self.color  = assignColor(m_cfg.preferredColor, m_localId);
        m_participants.push_back(std::move(self));
        m_joined = true;
        // The host's wish IS the session's rule, and it is true from the first
        // frame rather than from the first join: the host publishes into its own
        // session before anybody else is there, and a flag that only became true
        // once a guest arrived would have it holding back its own meshes.
        m_sessionLargeAssets = m_cfg.syncLargeAssets;
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
        m_net->on(kMsgDocDeltasUpdate, [this](ConnectionId conn, BitReader& r) {
            handleDocDeltasUpdate(conn, r);
        });
        m_net->on(kMsgLockQuery, [this](ConnectionId conn, BitReader& r) {
            handleLockQuery(conn, r);
        });
        m_net->on(kMsgAssetOpRequest, [this](ConnectionId conn, BitReader& r) {
            handleAssetOpRequest(conn, r);
        });
        m_net->on(kMsgAssetEditAnswer, [this](ConnectionId conn, BitReader& r) {
            handleAssetEditAnswer(conn, r);
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
        // Only the peer that asked hears this — the others learn about the
        // create from the relay above, or never, if it was refused.
        m_net->on(kMsgAssetCreateResult, [this](ConnectionId, BitReader& r) {
            handleAssetCreateResult(r);
        });
        m_net->on(kMsgAssetOpVerdict, [this](ConnectionId, BitReader& r) {
            handleAssetOpVerdict(r);
        });
        m_net->on(kMsgAssetOpApply, [this](ConnectionId, BitReader& r) {
            handleAssetOpApply(r);
        });
        m_net->on(kMsgAssetEditRequest, [this](ConnectionId, BitReader& r) {
            handleAssetEditRequest(r);
        });
        m_net->on(kMsgStructuralRelay, [this](ConnectionId, BitReader& r) {
            handleStructuralRelay(r);
        });
        m_net->on(kMsgComponentsRelay, [this](ConnectionId, BitReader& r) {
            handleComponentsRelay(r);
        });
        m_net->on(kMsgDocDeltasRelay, [this](ConnectionId, BitReader& r) {
            handleDocDeltasRelay(r);
        });
        m_net->on(kMsgLockQueryResult, [this](ConnectionId, BitReader& r) {
            handleLockQueryResult(r);
        });
        m_net->on(kMsgRemoved, [this](ConnectionId, BitReader& r) { handleRemoved(r); });

        // Roster updates for peers other than ourselves.
        m_net->on(kMsgParticipantJoined, [this](ConnectionId, BitReader& r) {
            Participant p;
            std::uint32_t id = 0;
            if (!r.readUInt32(id) || !r.readString(p.name)) return;
            if (!readAvatar(r, p.avatar, m_cfg.maxAvatarSize)) return;
            if (!readColor(r, p.color)) return;
            p.id = id;
            if (findParticipant(p.id)) return;         // already known
            m_participants.push_back(p);
            if (m_onJoin) m_onJoin(m_participants.back());
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
        // Which project we have open. The host refuses a mismatch outright: a
        // session addresses everything by uuids that only mean something inside
        // one project.
        w.writeString(m_cfg.projectKey);
        // Who this editor install is, so a ban outlives the connection it was
        // handed out on, and what its user looks like.
        w.writeString(m_cfg.clientKey);
        writeAvatar(w, m_cfg.avatar);
        // A wish, not a claim — the host answers with the colour we actually get.
        writeColor(w, m_cfg.preferredColor);
        // Whether this user has agreed to receive the big assets. Sent even when
        // false — especially when false: it is the "no" that has to reach the
        // host for it to refuse us, and a refusal is what gets the user asked.
        w.writeBool(m_cfg.syncLargeAssets);
        m_net->send(m_net->connections().front(), kMsgJoinRequest, w);
        m_joinSent = true;
    }

    // Peers we ejected: told, and now out of time. Done before presence so a
    // removed peer never receives one more relay on its way out.
    for (auto it = m_pendingRemovals.begin(); it != m_pendingRemovals.end(); ) {
        // Stamped here rather than at the kick: kickParticipant() is called from
        // UI code that has no business knowing the session's clock, and this runs
        // on the very next pump either way.
        if (!it->armed) {
            it->dueMs = nowMs + m_cfg.removalGraceMs;
            it->armed = true;
            ++it;
            continue;
        }
        if (nowMs < it->dueMs) { ++it; continue; }
        HE_LOG_INFO(Net, "Collab: closing the link to ejected conn %llu",
                    static_cast<unsigned long long>(it->conn));
        m_net->disconnect(it->conn);
        it = m_pendingRemovals.erase(it);
    }

    // Presence: only once the participant actually exists in the session, at
    // most every presenceIntervalMs, and only when something moved.
    if (m_joined && m_localPresenceSet) {
        const bool due = !m_everSentPresence ||
                         (nowMs - m_lastPresenceSendMs) >= m_cfg.presenceIntervalMs;
        // The keep-alive resend covers everything the change filter cannot see:
        // a peer that joined after our last movement, or a relay that never
        // arrived. Cheap — one small message every couple of seconds.
        const bool keepAlive = m_everSentPresence &&
                               (nowMs - m_lastPresenceSendMs) >= m_cfg.presenceKeepAliveMs;
        if ((due && presenceDiffersFromSent()) || keepAlive) {
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
    std::string   projectKey;
    if (!r.readUInt16(version) || !r.readString(name)) {
        HE_LOG_WARN(Net, "Collab: malformed join request on conn %llu",
                    static_cast<unsigned long long>(conn));
        return;
    }
    HE_LOG_INFO(Net, "Collab: join request from \"%s\" (protocol v%u) on conn %llu",
                name.c_str(), static_cast<unsigned>(version),
                static_cast<unsigned long long>(conn));

    const auto reject = [&](JoinRejectReason reason, const std::string& detail = {}) {
        BitWriter w;
        w.writeByte(static_cast<std::uint8_t>(reason));
        w.writeString(detail);
        m_net->send(conn, kMsgJoinRejected, w);
    };

    if (version != kCollabProtocolVersion) {
        HE_LOG_WARN(Net, "Collab: rejecting \"%s\" — collab protocol v%u, we speak v%u",
                    name.c_str(), static_cast<unsigned>(version),
                    static_cast<unsigned>(kCollabProtocolVersion));
        reject(JoinRejectReason::VersionMismatch);
        return;
    }
    // Read AFTER the version check: a peer speaking an older protocol did not
    // send this field, and reading a missing string would make it look like a
    // project mismatch instead of the version mismatch it is.
    std::string      clientKey;
    Avatar           avatar;
    ParticipantColor wish;
    bool             wantsLargeAssets = false;
    if (!r.readString(projectKey)) {
        HE_LOG_WARN(Net, "Collab: malformed join request from \"%s\" (no project key)",
                    name.c_str());
        return;
    }
    if (!r.readString(clientKey) || !readAvatar(r, avatar, m_cfg.maxAvatarSize) ||
        !readColor(r, wish) || !r.readBool(wantsLargeAssets)) {
        HE_LOG_WARN(Net, "Collab: malformed join request from \"%s\" (no identity block)",
                    name.c_str());
        return;
    }
    // Checked before anything else that costs work — a banned peer must not be
    // able to make the host serialize a scene by knocking repeatedly.
    if (isBanned(clientKey, name)) {
        HE_LOG_WARN(Net, "Collab: refusing \"%s\" — banned for this session", name.c_str());
        reject(JoinRejectReason::Banned);
        return;
    }
    if (projectKey != m_cfg.projectKey) {
        // Both keys, spelled out. Without them this verdict is unfalsifiable
        // from the outside: two people whose .heproj files carry the same id
        // still land here when one editor is running on an id it minted in
        // memory, and the message alone gives them nothing to check. An empty
        // key is worth naming separately — that is "no project open on that
        // side", a different mistake with a different fix.
        HE_LOG_WARN(Net, "Collab: rejecting \"%s\" — different project. This session "
                         "edits \"%s\" (id %s); the joiner sent id %s",
                    name.c_str(), m_cfg.projectLabel.c_str(),
                    m_cfg.projectKey.empty() ? "<none>" : m_cfg.projectKey.c_str(),
                    projectKey.empty() ? "<none — nothing open there>" : projectKey.c_str());
        reject(JoinRejectReason::ProjectMismatch, m_cfg.projectLabel);
        return;
    }
    // Only this direction is a refusal. A joiner that WOULD take the big assets
    // arriving at a session that does not carry them is not a conflict at all:
    // the host decides what the session is, and the guest simply follows it and
    // publishes nothing large — there is nobody its preference could bind.
    //
    // The other way round costs the joiner megabytes on a link they may be
    // paying for, so it is refused here rather than downgraded quietly, and it
    // is refused with a reason of its own so the client can ask its user instead
    // of reporting a dead end. Deliberately AFTER the ban and the project check:
    // both of those are unfixable by agreeing to this, and being asked to turn
    // on large-asset sync only to be refused again for the real reason is worse
    // than being told the real reason first.
    if (m_cfg.syncLargeAssets && !wantsLargeAssets) {
        HE_LOG_WARN(Net, "Collab: rejecting \"%s\" — this session also transfers meshes, "
                         "textures and audio, and that editor has not agreed to receive them",
                    name.c_str());
        reject(JoinRejectReason::LargeAssetsRequired, m_cfg.projectLabel);
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
    Participant joiner;
    joiner.id        = id;
    joiner.name      = name;
    joiner.isHost    = false;
    joiner.avatar    = std::move(avatar);
    joiner.clientKey = std::move(clientKey);
    // Settled before the roster goes out, so the joiner's own entry and
    // everyone else's copy of it carry the same colour from the first frame.
    joiner.color     = assignColor(wish, id);

    // Accept: assigned id, the sequence its snapshot corresponds to, and the
    // roster as it stands *before* adding the joiner (its own entry arrives via
    // the assigned id, and everyone else learns about it separately).
    // Deliberately no clientKey in here: see Participant::clientKey — that field
    // is the host's alone and is not a token to hand every peer.
    BitWriter accept;
    accept.writeUInt32(id);
    // The answer to the joiner's colour wish. It has to come back explicitly:
    // the joiner cannot work out on its own that its choice was taken.
    writeColor(accept, joiner.color);
    accept.writeUInt64(m_sequence);
    // What the session carries, so the joiner publishes by the session's rule
    // and not by its own setting. Ahead of the roster on purpose: the roster is
    // variable-length and its reader bails out mid-list on a short frame, and a
    // session property that only arrives when the roster happens to parse would
    // be missing in exactly the case worth diagnosing.
    accept.writeBool(m_sessionLargeAssets);
    accept.writeUInt16(static_cast<std::uint16_t>(m_participants.size()));
    for (const auto& p : m_participants) {
        accept.writeUInt32(p.id);
        accept.writeString(p.name);
        accept.writeBool(p.isHost);
        writeAvatar(accept, p.avatar);
        writeColor(accept, p.color);
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
    HE_LOG_DEBUG(Net, "Collab: \"%s\" identifies as client \"%s\"%s", name.c_str(),
                 joiner.clientKey.empty() ? "(none)" : joiner.clientKey.c_str(),
                 joiner.avatar.empty() ? "" : ", with a profile picture");

    // Hand over the current lock table, or a late arrival would believe every
    // asset is free and immediately collide with whoever is already editing.
    {
        BitWriter table;
        writeLockTable(table);
        m_net->send(conn, kMsgLockTable, table);
    }

    // And everyone's last known presence. Presence is change-driven, so without
    // this a late joiner sees no one until each of them happens to move their
    // camera — the field report was a host and a guest each staring at an empty
    // scene wondering where the other one was.
    for (const auto& [pid, state] : m_presence) {
        if (pid == id || !state.valid) continue;
        BitWriter w;
        w.writeUInt32(pid);
        writePresenceBody(w, state);
        m_net->send(conn, kMsgPresenceRelay, w);
    }

    BitWriter announce;
    announce.writeUInt32(id);
    announce.writeString(name);
    writeAvatar(announce, joiner.avatar);
    writeColor(announce, joiner.color);
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
        // Back to our own answer, which for a client is "nothing agreed yet".
        // The rule belonged to the session that just ended; carrying it into the
        // next join would have us publishing meshes on a host's say-so that we
        // are no longer connected to.
        m_sessionLargeAssets = false;
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
    retireParticipant(id, conn, conn);

    HE_LOG_INFO(Net, "Collab: participant %u left (%zu remain)",
                static_cast<unsigned>(id), m_participants.size());
    if (m_onLeft) m_onLeft(id);
}

// Everything that has to happen when a participant stops being in the session.
// `except` is the connection NOT to announce to — the one that went away, or the
// one we are about to cut loose (it is being told separately, and more usefully).
void CollabSession::retireParticipant(ParticipantId id, ConnectionId conn,
                                      ConnectionId except) {
    m_connToParticipant.erase(
        std::remove_if(m_connToParticipant.begin(), m_connToParticipant.end(),
                       [conn](const auto& e) { return e.first == conn; }),
        m_connToParticipant.end());

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
        if (other != except) m_net->send(other, kMsgParticipantLeft, w);
    }
}

// ─── Moderation ──────────────────────────────────────────────────────────────

bool CollabSession::kickParticipant(ParticipantId id) {
    return removeParticipant(id, RemovalReason::Kicked);
}

bool CollabSession::banParticipant(ParticipantId id) {
    return removeParticipant(id, RemovalReason::Banned);
}

bool CollabSession::removeParticipant(ParticipantId id, RemovalReason reason) {
    // Only the host holds the registry, and it cannot eject itself: there is no
    // session left to be ejected from.
    if (m_role != NetRole::Host || !m_net) return false;
    if (id == kInvalidParticipant || id == m_localId) return false;

    const Participant* found = findParticipant(id);
    if (!found) return false;
    const Participant victim = *found;   // copied: the roster entry is about to go

    ConnectionId conn = kInvalidConnection;
    for (const auto& [c, pid] : m_connToParticipant) {
        if (pid == id) { conn = c; break; }
    }

    if (reason == RemovalReason::Banned) {
        // Recorded before the eject, so a peer that reconnects in the window
        // between the two is already refused.
        if (!isBanned(victim.clientKey, victim.name)) {
            m_bans.push_back(BanEntry{ victim.clientKey, victim.name });
        }
        HE_LOG_INFO(Net, "Collab: banning \"%s\" for this session (client \"%s\")",
                    victim.name.c_str(),
                    victim.clientKey.empty() ? "(none — matching by name)"
                                             : victim.clientKey.c_str());
    } else {
        HE_LOG_INFO(Net, "Collab: kicking \"%s\"", victim.name.c_str());
    }

    if (conn != kInvalidConnection) {
        // Tell them why first. The link is closed a moment later (update()), or
        // the transport would discard this very message along with the socket.
        BitWriter w;
        w.writeByte(static_cast<std::uint8_t>(reason));
        m_net->send(conn, kMsgRemoved, w);
        m_pendingRemovals.push_back(PendingRemoval{ conn, 0, false });
    }

    retireParticipant(id, conn, conn);
    if (m_onRemovedLocal) m_onRemovedLocal(victim, reason);
    return true;
}

namespace {
// One rule, used by both the join check and unban(), so a ban can always be
// lifted by exactly the thing that would have matched it.
//
// The client key is the real subject: two identified peers are compared on that
// alone, so two people who both left the display name at "Horizon User" cannot
// ban one another by accident. The name is the fallback for when either side has
// no durable identity — without it, deleting the identity file would lift every
// ban, which is the one bypass worth closing.
bool banMatches(const CollabSession::BanEntry& b, const std::string& clientKey,
                const std::string& name) {
    if (!clientKey.empty() && !b.clientKey.empty()) return b.clientKey == clientKey;
    return !b.name.empty() && b.name == name;
}
} // namespace

bool CollabSession::isBanned(const std::string& clientKey,
                             const std::string& name) const {
    for (const auto& b : m_bans) {
        if (banMatches(b, clientKey, name)) return true;
    }
    return false;
}

bool CollabSession::unban(const std::string& clientKey, const std::string& name) {
    const auto before = m_bans.size();
    m_bans.erase(std::remove_if(m_bans.begin(), m_bans.end(),
                                [&](const BanEntry& b) {
                                    return banMatches(b, clientKey, name);
                                }),
                 m_bans.end());
    return m_bans.size() != before;
}

void CollabSession::handleRemoved(BitReader& r) {
    std::uint8_t reason = 0;
    if (!r.readByte(reason)) return;
    const auto why = static_cast<RemovalReason>(reason);
    HE_LOG_WARN(Net, "Collab: the host removed us from the session (%s)",
                why == RemovalReason::Banned ? "banned" : "kicked");
    // The link itself goes down a moment later and takes the rest of the state
    // with it (onPeerDisconnected). What matters here is that the editor learns
    // WHY before that happens — afterwards it is indistinguishable from the
    // host's network dropping.
    if (m_onRemoved) m_onRemoved(why);
}

// ─── Colours ─────────────────────────────────────────────────────────────────

ParticipantColor CollabSession::assignColor(ParticipantColor wish,
                                            std::uint32_t seed) const {
    constexpr int kMinSq = kColorCollisionDistance * kColorCollisionDistance;

    const auto freeFor = [this, kMinSq](const ParticipantColor& c) {
        for (const auto& p : m_participants) {
            if (!p.color.unset() && colorDistanceSq(p.color, c) < kMinSq) return false;
        }
        return true;
    };

    // The wish wins whenever it can. Someone who deliberately picked teal should
    // get teal, and only be moved when that would make them indistinguishable
    // from somebody already in the room.
    if (!wish.unset() && freeFor(wish)) return wish;

    constexpr int kPaletteSize =
        static_cast<int>(sizeof(kParticipantPalette) / sizeof(kParticipantPalette[0]));

    // Start somewhere in the palette rather than always at index 0, so the
    // second and third person to be reassigned do not both end up red. A cheap
    // integer hash of the seed spreads consecutive participant ids apart.
    std::uint32_t h = seed * 2654435761u;
    h ^= h >> 15;
    const int start = static_cast<int>(h % static_cast<std::uint32_t>(kPaletteSize));

    for (int i = 0; i < kPaletteSize; ++i) {
        const ParticipantColor& c = kParticipantPalette[(start + i) % kPaletteSize];
        if (freeFor(c)) return c;
    }

    // More people than the palette has colours. Rather than hand out a duplicate
    // silently, take the one that is furthest from everybody present — two
    // similar markers beat two identical ones.
    ParticipantColor best = kParticipantPalette[start];
    int bestScore = -1;
    for (const ParticipantColor& c : kParticipantPalette) {
        int worst = 1 << 30;
        for (const auto& p : m_participants) {
            if (p.color.unset()) continue;
            worst = std::min(worst, colorDistanceSq(p.color, c));
        }
        if (worst > bestScore) { bestScore = worst; best = c; }
    }
    HE_LOG_WARN(Net, "Collab: the colour palette is exhausted — two participants will "
                     "look similar");
    return best;
}

// ─── Avatars ─────────────────────────────────────────────────────────────────

void CollabSession::writeAvatar(BitWriter& w, const Avatar& a) {
    const bool ok = !a.empty() &&
                    a.rgba.size() == static_cast<std::size_t>(a.size) * a.size * 4u;
    w.writeUInt16(ok ? a.size : 0);
    if (ok) w.writeBytes(a.rgba.data(), a.rgba.size());
}

bool CollabSession::readAvatar(BitReader& r, Avatar& out, std::uint16_t maxSize) {
    out = Avatar{};
    std::uint16_t size = 0;
    if (!r.readUInt16(size)) return false;
    if (size == 0) return true;   // no picture — perfectly normal

    // Two separate bounds, because they answer two different questions.
    //
    // kAvatarSizeCeiling is what the WIRE allows: a uint16 side length means a
    // peer can announce 65535², i.e. 17 GB of pixels, and sizing a buffer from
    // that claim is how a one-line message becomes an out-of-memory abort. Past
    // the ceiling is malformed, full stop.
    //
    // maxSize is what THIS session chooses to carry, and the two can legitimately
    // differ between builds — so a picture that is merely over the local
    // preference is read and dropped, never treated as an attack. Losing a
    // portrait must not cost anyone their session.
    constexpr std::uint16_t kAvatarSizeCeiling = 256;   // 256 KiB of pixels
    if (size > kAvatarSizeCeiling) {
        HE_LOG_WARN(Net, "Collab: refusing a %ux%u profile picture — no legitimate peer "
                         "sends one that large",
                    static_cast<unsigned>(size), static_cast<unsigned>(size));
        return false;
    }

    const std::size_t bytes = static_cast<std::size_t>(size) * size * 4u;
    // Nothing is allocated until the frame actually holds that many bytes.
    if (r.bitsRemaining() < bytes * 8) return false;

    std::vector<std::uint8_t> pixels(bytes);
    if (!r.readBytes(pixels.data(), bytes)) return false;
    if (size > maxSize) {
        // Consumed so the rest of the message stays aligned, then discarded.
        HE_LOG_WARN(Net, "Collab: ignoring a %ux%u profile picture — over the %ux%u limit",
                    static_cast<unsigned>(size), static_cast<unsigned>(size),
                    static_cast<unsigned>(maxSize), static_cast<unsigned>(maxSize));
        return true;
    }
    out.size = size;
    out.rgba = std::move(pixels);
    return true;
}

// ─── Client side ─────────────────────────────────────────────────────────────

void CollabSession::handleJoinAccepted(BitReader& r) {
    std::uint32_t    id = 0;
    std::uint64_t    sequence = 0;
    std::uint16_t    count = 0;
    ParticipantColor assigned;
    bool             largeAssets = false;
    if (!r.readUInt32(id) || !readColor(r, assigned) ||
        !r.readUInt64(sequence) || !r.readBool(largeAssets) || !r.readUInt16(count)) {
        return;
    }

    m_localId  = id;
    m_sequence = sequence;
    // The host's answer replaces our wish for the rest of the session. Our own
    // Config::syncLargeAssets only ever decided whether we were ADMITTED; from
    // here on, what travels is the session's rule, and a client that kept
    // consulting its own setting would push meshes into a session that does not
    // carry them (where every other peer would simply have nothing to apply).
    m_sessionLargeAssets = largeAssets;
    m_participants.clear();

    for (std::uint16_t i = 0; i < count; ++i) {
        Participant p;
        std::uint32_t pid = 0;
        bool isHost = false;
        if (!r.readUInt32(pid) || !r.readString(p.name) || !r.readBool(isHost)) return;
        if (!readAvatar(r, p.avatar, m_cfg.maxAvatarSize)) return;
        if (!readColor(r, p.color)) return;
        p.id     = pid;
        p.isHost = isHost;
        m_participants.push_back(std::move(p));
    }
    // Our own entry is not in the host's list yet, so add it here — with our own
    // picture, so the UI can draw every participant, ourselves included, from one
    // roster instead of special-casing the local user.
    Participant self;
    self.id     = m_localId;
    self.name   = m_cfg.displayName;
    self.isHost = false;
    self.avatar = m_cfg.avatar;
    // What the host gave us, NOT what we asked for — the two differ whenever
    // somebody already had that colour, and drawing ourselves in the wish would
    // put us in a colour nobody else uses for us.
    self.color  = assigned;
    m_participants.push_back(std::move(self));

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
    std::string detail;
    r.readString(detail);   // absent on an older host — the empty string is fine
    // Spelled out rather than left as a number: these are the things a user can
    // actually be told when a join does not go through.
    const char* text = "unknown reason";
    switch (static_cast<JoinRejectReason>(reason)) {
    case JoinRejectReason::VersionMismatch: text = "collab protocol mismatch (different engine build)"; break;
    case JoinRejectReason::SessionFull:     text = "the session is full"; break;
    case JoinRejectReason::SnapshotFailed:  text = "the host could not produce a scene snapshot"; break;
    case JoinRejectReason::ProjectMismatch: text = "the host has a different project open"; break;
    case JoinRejectReason::Banned:          text = "the host banned you from this session"; break;
    case JoinRejectReason::LargeAssetsRequired:
        text = "the session also transfers meshes, textures and audio, which this "
               "editor has not agreed to receive";
        break;
    default: break;
    }
    HE_LOG_WARN(Net, "Collab: join rejected by the host — %s%s%s", text,
                detail.empty() ? "" : ": ", detail.c_str());
    m_joined = false;
    if (m_onRejected) m_onRejected(static_cast<JoinRejectReason>(reason), detail);
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
        if (m_onRejected) m_onRejected(JoinRejectReason::SnapshotFailed, {});
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
        if (m_onRejected) m_onRejected(JoinRejectReason::SnapshotFailed, {});
        return;
    }

    const std::size_t offset = m_snapshotBuf.size();
    m_snapshotBuf.resize(offset + len);
    if (!r.readBytes(m_snapshotBuf.data() + offset, len)) {
        HE_LOG_ERROR(Net, "Collab: snapshot chunk %u is truncated — aborting the transfer", index);
        m_snapshotBuf.resize(offset);   // truncated frame
        m_snapshotActive = false;
        if (m_onRejected) m_onRejected(JoinRejectReason::SnapshotFailed, {});
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
        if (m_onRejected) m_onRejected(JoinRejectReason::SnapshotFailed, {});
        return;
    }
    if (!m_state || !m_state->applySnapshot(m_snapshotBuf)) {
        HE_LOG_ERROR(Net, "Collab: the snapshot arrived intact but could not be applied — "
                          "the scene it describes may need assets this project lacks");
        if (m_onRejected) m_onRejected(JoinRejectReason::SnapshotFailed, {});
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
        // A peer we just ejected keeps publishing until its link actually goes
        // down — that is the grace period working as intended, not an intrusion,
        // and at 10 Hz it would bury the log in warnings.
        const bool ejected = std::any_of(
            m_pendingRemovals.begin(), m_pendingRemovals.end(),
            [conn](const PendingRemoval& r) { return r.conn == conn; });
        if (!ejected) {
            // Someone is publishing presence over a connection that never
            // joined — the exact shape a spoofing attempt would take, hence
            // Warning.
            HE_LOG_WARN(Net, "Presence from conn %llu, which is not a joined participant "
                             "— ignored",
                        static_cast<unsigned long long>(conn));
        }
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

// ─── Document deltas ─────────────────────────────────────────────────────────

void CollabSession::writeDocDeltaBody(BitWriter& w, std::uint64_t subject,
                                      const std::string& path,
                                      const std::vector<DocDelta>& batch,
                                      std::uint32_t revision) const {
    w.writeUInt64(subject);
    w.writeString(path);
    // The holder's revision of this document. The same field sits on the asset
    // frame, in the same relation to the path: the two channels carry the same
    // document and had no way of being ordered against one another, so a whole
    // file arriving late silently undid newer deltas — "applied for a moment,
    // then snapped back".
    w.writeUInt32(revision);
    w.writeUInt16(static_cast<std::uint16_t>(batch.size()));
    for (const DocDelta& d : batch) {
        w.writeByte(d.scope);
        w.writeByte(d.kind);
        w.writeByte(d.op);
        w.writeUInt64(static_cast<std::uint64_t>(d.itemId));
        w.writeString(d.json);
    }
}

bool CollabSession::readDocDeltaBody(BitReader& r, std::uint64_t& subject,
                                     std::string& path,
                                     std::vector<DocDelta>& out,
                                     std::uint32_t& revision) const {
    if (!r.readUInt64(subject)) return false;
    if (!r.readString(path)) return false;
    if (!r.readUInt32(revision)) return false;
    std::uint16_t count = 0;
    if (!r.readUInt16(count)) return false;
    if (count > m_cfg.maxDocDeltas) return false;   // bounds what a peer can make us build
    out.clear();
    out.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        DocDelta d;
        if (!r.readByte(d.scope) || !r.readByte(d.kind) || !r.readByte(d.op)) return false;
        std::uint64_t id = 0;
        if (!r.readUInt64(id)) return false;
        d.itemId = static_cast<std::int64_t>(id);
        if (!r.readString(d.json)) return false;
        out.push_back(std::move(d));
    }
    // A partially-read batch must not be applied: half a paste is a broken graph,
    // not a smaller one.
    return !r.overflowed();
}

bool CollabSession::sendDocDeltas(std::uint64_t subject, const std::string& path,
                                  const std::vector<DocDelta>& batch,
                                  std::uint32_t revision) {
    if (!m_net || !m_joined || batch.empty()) return false;
    // Same rule the transform and asset paths use: the lock is what makes an
    // overwrite safe, because it guarantees nobody else is editing this document.
    if (!ownsLock(subject)) return false;
    if (batch.size() > m_cfg.maxDocDeltas) return false;

    // Refuse rather than truncate. writeString cuts silently at 65535 bytes, and
    // half a node's JSON is not a smaller edit — it is an unparseable one that
    // would drop the item on the peer while looking like a success here. The
    // caller falls back to publishing the whole file.
    std::size_t total = path.size() + 16;
    for (const DocDelta& d : batch) {
        if (d.json.size() > m_cfg.maxDocItemBytes) return false;
        total += d.json.size() + 16;
        if (total > m_cfg.maxDocBatchBytes) return false;
    }

    if (m_role == NetRole::Host) {
        BitWriter w;
        w.writeUInt32(m_localId);
        writeDocDeltaBody(w, subject, path, batch, revision);
        m_net->broadcast(kMsgDocDeltasRelay, w);
        return true;
    }
    if (m_net->connections().empty()) return false;

    BitWriter w;
    writeDocDeltaBody(w, subject, path, batch, revision);   // no id — the host stamps it
    m_net->send(m_net->connections().front(), kMsgDocDeltasUpdate, w);
    return true;
}

void CollabSession::handleDocDeltasUpdate(ConnectionId conn, BitReader& r) {
    const ParticipantId sender = participantForConnection(conn);
    if (sender == kInvalidParticipant) return;

    std::uint64_t subject = 0;
    std::string   path;
    std::vector<DocDelta> batch;
    std::uint32_t revision = 0;
    if (!readDocDeltaBody(r, subject, path, batch, revision)) return;

    // Re-check authority on arrival rather than trusting the sender — a client
    // that lost the lock (or never had it) must not be able to rewrite a
    // document just because it addressed the message correctly.
    const LockInfo* lock = lockFor(subject);
    if (!lock || lock->owner != sender) return;

    if (m_onDocDeltas) m_onDocDeltas(sender, subject, path, batch, revision);

    BitWriter w;
    w.writeUInt32(sender);
    // Relayed with the ORIGINATOR's revision, untouched: the host is a courier
    // here, not an author, and renumbering would make every peer's ordering
    // depend on which of them the host told first.
    writeDocDeltaBody(w, subject, path, batch, revision);
    for (const ConnectionId other : m_net->connections()) {
        if (other != conn) m_net->send(other, kMsgDocDeltasRelay, w);
    }
}

void CollabSession::handleDocDeltasRelay(BitReader& r) {
    std::uint32_t sender = 0;
    if (!r.readUInt32(sender)) return;
    if (sender == m_localId) return;   // our own edit echoed back

    std::uint64_t subject = 0;
    std::string   path;
    std::vector<DocDelta> batch;
    std::uint32_t revision = 0;
    if (!readDocDeltaBody(r, subject, path, batch, revision)) return;
    if (m_onDocDeltas) m_onDocDeltas(sender, subject, path, batch, revision);
}

// ─── Lock query ──────────────────────────────────────────────────────────────

bool CollabSession::queryLock(std::uint64_t subject) {
    if (!m_net || !m_joined) return false;

    // The host owns the table, so its answer needs no round trip. Answering
    // inline (rather than making the host a special case at every call site)
    // is what lets the editor drive both roles through one flow.
    if (m_role == NetRole::Host) {
        const LockInfo* lock = lockFor(subject);
        if (m_onLockQuery) {
            m_onLockQuery(subject, lock != nullptr,
                          lock ? lock->owner : kInvalidParticipant,
                          lock ? lock->ownerName : std::string());
        }
        return true;
    }
    if (m_net->connections().empty()) return false;

    BitWriter w;
    w.writeUInt64(subject);
    m_net->send(m_net->connections().front(), kMsgLockQuery, w);
    return true;
}

void CollabSession::handleLockQuery(ConnectionId conn, BitReader& r) {
    std::uint64_t subject = 0;
    if (!r.readUInt64(subject)) return;

    const LockInfo* lock = lockFor(subject);
    BitWriter w;
    w.writeUInt64(subject);
    w.writeBool(lock != nullptr);
    w.writeUInt32(lock ? lock->owner : kInvalidParticipant);
    w.writeString(lock ? lock->ownerName : std::string());
    m_net->send(conn, kMsgLockQueryResult, w);
}

void CollabSession::handleLockQueryResult(BitReader& r) {
    std::uint64_t subject = 0;
    bool          held    = false;
    std::uint32_t owner   = 0;
    std::string   name;
    if (!r.readUInt64(subject) || !r.readBool(held) ||
        !r.readUInt32(owner) || !r.readString(name)) return;
    if (m_onLockQuery) m_onLockQuery(subject, held, owner, name);
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
    AssetUpdate a;
    a.subject = subject;
    a.path    = path;
    a.bytes   = bytes;
    a.intent  = AssetIntent::Update;   // a save, which is what this form is for
    return sendAsset(a);
}

bool CollabSession::sendAsset(const AssetUpdate& a) {
    const std::string& path = a.path;
    if (!m_net || !m_joined) return false;
    // Same authority rule as transforms: you may only publish what you hold.
    // A CREATE is the exception and says so: nobody can hold the lock on an
    // asset that does not exist yet, and the host arbitrates it instead.
    if (a.intent != AssetIntent::Create && !ownsLock(a.subject)) {
        // The everyday cause of "my change did not show up for the others":
        // the asset was saved without holding its lock.
        HE_LOG_WARN(Net, "Not sending asset \"%s\": we do not hold lock 0x%016llx",
                    path.c_str(), static_cast<unsigned long long>(a.subject));
        return false;
    }
    if (a.bytes.size() > m_cfg.maxAssetBytes) {
        HE_LOG_ERROR(Net, "Not sending asset \"%s\": %s exceeds the %s transfer limit",
                     path.c_str(), detail::logBytes(a.bytes.size()).c_str(),
                     detail::logBytes(m_cfg.maxAssetBytes).c_str());
        return false;
    }
    HE_LOG_INFO(Net, "Sending asset \"%s\" (%s%s)", path.c_str(),
                detail::logBytes(a.bytes.size()).c_str(),
                a.intent == AssetIntent::Create ? ", new" : "");

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
    // Ahead of the subject, and the reason v7 exists: a v6 reader would take
    // this byte for the subject's most significant one.
    begin.writeByte(static_cast<std::uint8_t>(a.intent));
    begin.writeUInt64(a.subject);
    begin.writeString(a.path);
    // Directly after the path, exactly where a delta frame carries it — the two
    // have to be comparable, so they sit in the same relation to what they name.
    begin.writeUInt32(a.revision);
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

    // Re-check authority on arrival rather than trusting the sender. A CREATE is
    // exempt for the same reason the sender skips it: the asset does not exist
    // yet, so no lock over it can exist either, and demanding one would refuse
    // every create there has ever been. Authority for a create is the policy
    // below, which is a stronger check than a lock — it asks the host's disk.
    if (header.intent != AssetIntent::Create) {
        const LockInfo* lock = lockFor(header.subject);
        if (!lock || lock->owner != sender) {
            HE_LOG_WARN(Net, "Refusing asset \"%s\" from participant %u — it does not hold "
                             "lock 0x%016llx",
                        header.path.c_str(), static_cast<unsigned>(sender),
                        static_cast<unsigned long long>(header.subject));
            return;
        }
    }
    // Refused on the ANNOUNCED size, before a byte of it is read — the reserve
    // below is what a peer would otherwise be able to make this process allocate
    // just by claiming a number.
    if (total > m_cfg.maxAssetBytes) {
        HE_LOG_ERROR(Net, "Refusing asset \"%s\" from participant %u — announced %s, over "
                          "the %s transfer limit",
                     header.path.c_str(), static_cast<unsigned>(sender),
                     detail::logBytes(total).c_str(),
                     detail::logBytes(m_cfg.maxAssetBytes).c_str());
        // Their side thinks this save went out. Ours is the only one that knows
        // otherwise, so it is the only one that can say so.
        if (m_onAssetRefused) m_onAssetRefused(sender, header.path, total);
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
    asm_.intent   = header.intent;
    asm_.revision = header.revision;
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
    std::uint8_t intent = 0;
    if (!r.readByte(intent)) return false;
    // An unknown intent is refused rather than treated as Update: a future
    // version may add one whose bytes must NOT be written as an ordinary save.
    if (intent > static_cast<std::uint8_t>(AssetIntent::Create)) return false;
    out.intent = static_cast<AssetIntent>(intent);
    return r.readUInt64(out.subject) && r.readString(out.path) &&
           r.readUInt32(out.revision) &&
           r.readUInt32(outTotal) && r.readUInt32(outChunks);
}

std::uint32_t CollabSession::requestAssetOp(AssetOp op, const std::string& path,
                                            const std::string& newPath,
                                            bool folder, std::uint64_t subject,
                                            std::uint32_t batch) {
    if (!m_net || !m_joined || path.empty()) return 0;
    // The host does not ask itself. It has the queue, the disk and the decision;
    // routing its own delete through a request would mean answering a dialog it
    // raised for itself.
    //
    // Edit is the exception, because it is the one op the host does not decide:
    // the holder does, and the holder can be a client. So the host routes its
    // own ask exactly like anyone else's, straight to whoever is holding it.
    if (m_role == NetRole::Host) {
        if (op != AssetOp::Edit) return 0;
        const std::uint32_t id = m_nextOpRequestId++;
        // False means nobody holds it — there is nobody to ask, and the caller
        // takes the lock the ordinary way.
        return routeEditRequest(m_localId, id, path, subject) ? id : 0;
    }
    if (m_net->connections().empty()) return 0;

    const std::uint32_t id = m_nextOpRequestId++;
    BitWriter w;
    w.writeUInt32(id);
    w.writeByte(static_cast<std::uint8_t>(op));
    w.writeString(path);
    w.writeString(newPath);
    w.writeByte(folder ? 1 : 0);
    w.writeUInt64(subject);
    w.writeUInt32(batch);
    m_net->send(m_net->connections().front(), kMsgAssetOpRequest, w);
    return id;
}

void CollabSession::handleAssetOpRequest(ConnectionId conn, BitReader& r) {
    // Host side. Identity from the connection, as everywhere: a client may not
    // ask on somebody else's behalf.
    const ParticipantId who = participantForConnection(conn);
    if (who == kInvalidParticipant) return;

    std::uint32_t id = 0, batch = 0;
    std::uint8_t  op = 0, folder = 0;
    std::uint64_t subject = 0;
    std::string   path, newPath;
    if (!r.readUInt32(id) || !r.readByte(op) ||
        !r.readString(path) || !r.readString(newPath) || !r.readByte(folder) ||
        !r.readUInt64(subject) || !r.readUInt32(batch)) {
        return;
    }
    if (op > static_cast<std::uint8_t>(AssetOp::Reimport)) return;
    if (path.empty()) return;

    // "May I edit this?" is the one op the host does not answer. It goes to
    // whoever holds the lock, because it is their work being interrupted; the
    // host only knows who that is. Nobody holding it means there is nothing to
    // ask about, so it is granted on the spot.
    if (static_cast<AssetOp>(op) == AssetOp::Edit) {
        if (!routeEditRequest(who, id, path, subject))
            sendAssetOpVerdict(who, id, true);
        return;
    }

    if (m_onOpRequested)
        m_onOpRequested(who, id, static_cast<AssetOp>(op), path, newPath,
                        folder != 0, batch);
}

bool CollabSession::routeEditRequest(ParticipantId from, std::uint32_t requestId,
                                     const std::string& path,
                                     std::uint64_t subject) {
    if (m_role != NetRole::Host) return false;
    const LockInfo* lock = lockFor(subject);
    if (!lock || lock->owner == from) return false;   // free, or already theirs

    // The host holding it answers like anybody else: same queue, same buttons.
    // Routing it to itself over the network would be a message to nowhere.
    if (lock->owner == m_localId) {
        if (m_onEditRequested) m_onEditRequested(from, requestId, path);
        return true;
    }

    ConnectionId conn = kInvalidConnection;
    for (const auto& [c, pid] : m_connToParticipant) {
        if (pid == lock->owner) { conn = c; break; }
    }
    if (conn == kInvalidConnection) return false;   // holder vanished — treat as free

    BitWriter w;
    w.writeUInt32(from);
    w.writeUInt32(requestId);
    w.writeString(path);
    m_net->send(conn, kMsgAssetEditRequest, w);
    return true;
}

void CollabSession::handleAssetEditRequest(BitReader& r) {
    std::uint32_t from = 0, requestId = 0;
    std::string   path;
    if (!r.readUInt32(from) || !r.readUInt32(requestId) || !r.readString(path)) return;
    if (m_onEditRequested) m_onEditRequested(from, requestId, path);
}

void CollabSession::handOverLock(std::uint64_t subject, ParticipantId to) {
    // Released and granted in ONE step. Releasing first and letting the asker
    // request it again leaves a gap in which anyone else can take it — and the
    // whole point of asking was to be the one who gets it.
    const Participant* p = findParticipant(to);
    grantLock(subject, to, p ? p->name : std::string{});
    broadcastLock(subject, true, kInvalidConnection);
    if (m_onLockChanged) m_onLockChanged(*lockFor(subject), true);
}

void CollabSession::sendAssetEditAnswer(ParticipantId requester,
                                        std::uint32_t requestId,
                                        const std::string& path, bool allowed,
                                        std::uint64_t subject) {
    if (!m_net || !m_joined) return;
    if (m_role == NetRole::Host) {
        // We ARE the host and we were the holder: no round trip to make, just
        // do what the answer means. handOverLock does the release-and-grant in
        // one step for the same reason it does over the wire.
        if (allowed) handOverLock(subject, requester);
        sendAssetOpVerdict(requester, requestId, allowed);
        return;
    }
    if (m_net->connections().empty()) return;
    BitWriter w;
    w.writeUInt32(requester);
    w.writeUInt32(requestId);
    w.writeUInt64(subject);
    w.writeString(path);
    w.writeByte(allowed ? 1 : 0);
    m_net->send(m_net->connections().front(), kMsgAssetEditAnswer, w);
}

void CollabSession::handleAssetEditAnswer(ConnectionId conn, BitReader& r) {
    // Host side. The answer is only worth anything from the peer that actually
    // holds the lock — otherwise a third party could hand away someone's work.
    const ParticipantId sender = participantForConnection(conn);
    if (sender == kInvalidParticipant) return;

    std::uint32_t requester = 0, requestId = 0;
    std::uint64_t subject = 0;
    std::uint8_t  allowed = 0;
    std::string   path;
    if (!r.readUInt32(requester) || !r.readUInt32(requestId) ||
        !r.readUInt64(subject) || !r.readString(path) || !r.readByte(allowed)) {
        return;
    }
    // Handing the lock over is only worth anything from the peer that actually
    // holds it — otherwise a third party could give away somebody else's work.
    const LockInfo* lock = lockFor(subject);
    if (!lock || lock->owner != sender) {
        // Not (or no longer) the holder: nothing changes hands. But the
        // REQUESTER is still sitting there waiting, and an answer that never
        // arrives leaves them waiting forever — worse than being told no. So
        // they are told what is true right now: free means take it, held by
        // someone else means no.
        sendAssetOpVerdict(requester, requestId, lock == nullptr);
        return;
    }

    if (allowed) handOverLock(subject, requester);
    sendAssetOpVerdict(requester, requestId, allowed != 0);
}

void CollabSession::sendAssetOpVerdict(ParticipantId to, std::uint32_t requestId,
                                       bool approved) {
    if (!m_net || m_role != NetRole::Host) return;

    // The host can be the one waiting. It never asks itself for a delete or a
    // rename — it decides those — but "may I edit this?" is answered by whoever
    // HOLDS the asset, and that can be a client while the host is the asker.
    // There is no connection to send that answer down; the callback is the
    // delivery.
    if (to == m_localId) {
        if (m_onOpVerdict) m_onOpVerdict(requestId, approved);
        return;
    }

    ConnectionId conn = kInvalidConnection;
    for (const auto& [c, pid] : m_connToParticipant) {
        if (pid == to) { conn = c; break; }
    }
    // Gone before the host got round to answering. Nothing to do: they took
    // their pending requests with them.
    if (conn == kInvalidConnection) return;

    BitWriter w;
    w.writeUInt32(requestId);
    w.writeByte(approved ? 1 : 0);
    m_net->send(conn, kMsgAssetOpVerdict, w);
}

void CollabSession::broadcastAssetOpApply(AssetOp op, const std::string& path,
                                          const std::string& newPath,
                                          ParticipantId by, bool folder) {
    if (!m_net || m_role != NetRole::Host) return;
    BitWriter w;
    w.writeUInt32(by);
    w.writeByte(static_cast<std::uint8_t>(op));
    w.writeString(path);
    w.writeString(newPath);
    w.writeByte(folder ? 1 : 0);
    for (const ConnectionId c : m_net->connections())
        m_net->send(c, kMsgAssetOpApply, w);
    // The host applies through the same callback as everyone else, so there is
    // one code path for "this happened" rather than two that must agree.
    if (m_onOpApply) m_onOpApply(by, op, path, newPath, folder);
}

void CollabSession::handleAssetOpVerdict(BitReader& r) {
    std::uint32_t id = 0;
    std::uint8_t  approved = 0;
    if (!r.readUInt32(id) || !r.readByte(approved)) return;
    if (m_onOpVerdict) m_onOpVerdict(id, approved != 0);
}

void CollabSession::handleAssetOpApply(BitReader& r) {
    std::uint32_t by = 0;
    std::uint8_t  op = 0, folder = 0;
    std::string   path, newPath;
    if (!r.readUInt32(by) || !r.readByte(op) ||
        !r.readString(path) || !r.readString(newPath) || !r.readByte(folder)) {
        return;
    }
    if (op > static_cast<std::uint8_t>(AssetOp::Reimport)) return;
    // Edit is the one op that is never broadcast: its answer is a verdict to the
    // one who asked and a lock that changes hands, and there is nothing for the
    // rest of the session to apply. The bound above cannot express that — it
    // sits in the middle of the range — so it is refused by name. Letting it
    // through would hand every peer an "op" whose newPath is empty, which the
    // editor's apply reads as a rename to nowhere.
    if (static_cast<AssetOp>(op) == AssetOp::Edit) return;
    if (m_onOpApply)
        m_onOpApply(by, static_cast<AssetOp>(op), path, newPath, folder != 0);
}

bool CollabSession::arbitrateCreate(ConnectionId conn, const AssetUpdate& a) {
    AssetRejectReason reason = AssetRejectReason::None;
    std::string       suggested;

    // No policy = accept. A host that installed none is not expressing "refuse
    // everything"; it simply has no opinion, and swallowing creates would be the
    // surprising reading.
    const bool ok = !m_createPolicy || m_createPolicy(a, reason, suggested);
    if (ok) return true;

    HE_LOG_INFO(Net, "Collab: refusing create \"%s\" (reason %u%s%s)",
                a.path.c_str(), static_cast<unsigned>(reason),
                suggested.empty() ? "" : ", suggesting ", suggested.c_str());

    BitWriter w;
    w.writeString(a.path);
    w.writeByte(0);   // not accepted
    w.writeByte(static_cast<std::uint8_t>(reason));
    // May be empty. When it is not, the creator can rename locally and try
    // again — which is what turns "two people created the same name" from a
    // lost asset into two assets.
    w.writeString(suggested);
    m_net->send(conn, kMsgAssetCreateResult, w);
    return false;
}

void CollabSession::handleAssetCreateResult(BitReader& r) {
    std::string  path;
    std::uint8_t accepted = 0, reason = 0;
    std::string  suggested;
    if (!r.readString(path) || !r.readByte(accepted) || !r.readByte(reason) ||
        !r.readString(suggested)) {
        return;
    }
    if (reason > static_cast<std::uint8_t>(AssetRejectReason::NotPermitted)) {
        // An unknown reason from a newer host: the verdict still stands, only
        // its explanation is one we cannot name.
        reason = static_cast<std::uint8_t>(AssetRejectReason::None);
    }
    if (m_onCreateResult) {
        m_onCreateResult(path, accepted != 0,
                         static_cast<AssetRejectReason>(reason), suggested);
    }
}

void CollabSession::handleAssetRelay(BitReader& r) {
    // Client side: the host has already vouched for the sender.
    std::uint32_t sender = 0;
    if (!r.readUInt32(sender)) return;
    if (sender == m_localId) return;   // our own save echoed back

    AssetUpdate header;
    std::uint32_t total = 0, chunks = 0;
    if (!readAssetHeader(r, header, total, chunks)) return;
    // The client's own ceiling, enforced exactly as the host enforces its own on
    // the way in. It is deliberately not the host's: the two are set on two
    // machines by two people, and the whole point of checking here is that this
    // process decides what it is willing to hold.
    if (total > m_cfg.maxAssetBytes) {
        HE_LOG_ERROR(Net, "Refusing asset \"%s\" from participant %u — announced %s, over "
                          "the %s transfer limit",
                     header.path.c_str(), static_cast<unsigned>(sender),
                     detail::logBytes(total).c_str(),
                     detail::logBytes(m_cfg.maxAssetBytes).c_str());
        if (m_onAssetRefused) m_onAssetRefused(sender, header.path, total);
        return;
    }

    AssetAssembly asm_;
    asm_.from     = kInvalidConnection;
    asm_.sender   = sender;
    asm_.subject  = header.subject;
    asm_.path     = header.path;
    asm_.intent   = header.intent;
    asm_.revision = header.revision;
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
                // Forwarded as it arrived, so the relay the host sends on says
                // the same thing the original did.
                up.intent  = a.intent;
                up.revision = a.revision;

                // A create is the one frame the host may refuse. Asked BEFORE
                // anything is applied or forwarded: half the session having
                // written a file the other half rejected is not a state worth
                // being able to reach.
                if (host && up.intent == AssetIntent::Create &&
                    !arbitrateCreate(conn, up)) {
                    m_assetAssembly.erase(m_assetAssembly.begin() +
                                          static_cast<std::ptrdiff_t>(i));
                    return;
                }

                if (m_onAsset) m_onAsset(a.sender, up);

                // Host also fans it out to everyone else.
                if (host) {
                    for (const ConnectionId other : m_net->connections()) {
                        if (other != conn) sendAssetChunks(other, a.sender, up);
                    }
                    // …and tells the creator it went through, so their side can
                    // stop treating the asset as merely local.
                    if (up.intent == AssetIntent::Create) {
                        BitWriter ok;
                        ok.writeString(up.path);
                        ok.writeByte(1);   // accepted
                        ok.writeByte(static_cast<std::uint8_t>(AssetRejectReason::None));
                        ok.writeString("");
                        m_net->send(conn, kMsgAssetCreateResult, ok);
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
