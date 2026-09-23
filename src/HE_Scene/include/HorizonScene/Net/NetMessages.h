#pragma once

// ─── Layer 3a — one place for the gameplay message ids ───────────────────────
// Every gameplay message lives in kFirstUserMessage + 200… , clear of the
// collaboration protocol's range: the two systems share a transport and never a
// message. They are gathered HERE rather than in each consumer's anonymous
// namespace because GameReplication, SpawnReplicator and NetGameSession all send
// on the same NetSession, and three private tables would collide the first time
// two of them picked the same free number.
//
// A number, once shipped, is frozen: a build that renumbers one of these does
// not fail to connect, it misreads the other side's payload. New messages go on
// the END.

#include <Net/NetCommon.h>

namespace HE::Net::Game {

// ── Replication (GameReplication, since step 2/3) ──
inline constexpr MessageId kMsgSnapshot        = kFirstUserMessage + 200;   // host → client, Unreliable
inline constexpr MessageId kMsgInput           = kFirstUserMessage + 201;   // client → host, Unreliable
inline constexpr MessageId kMsgIntegrity       = kFirstUserMessage + 202;   // client → host, once
inline constexpr MessageId kMsgAntiCheatNotice = kFirstUserMessage + 203;   // host → client, before a kick

// ── Session handshake (NetGameSession, step 4) ──
// All ReliableOrdered, which is what makes the join sequence below a sequence:
// Welcome → Binds → Spawns → Baseline → JoinComplete cannot arrive shuffled.
inline constexpr MessageId kMsgHello        = kFirstUserMessage + 204;   // client → host: name, version, project
inline constexpr MessageId kMsgWelcome      = kFirstUserMessage + 205;   // host → client: player id, scene, tick rate
inline constexpr MessageId kMsgReject       = kFirstUserMessage + 206;   // host → client: reason + project label
inline constexpr MessageId kMsgJoinComplete = kFirstUserMessage + 211;   // host → client: the baseline is over
inline constexpr MessageId kMsgBye          = kFirstUserMessage + 212;   // client → host: leaving on purpose

// ── Spawn replication (SpawnReplicator, step 4) ──
inline constexpr MessageId kMsgBind     = kFirstUserMessage + 207;   // host → client: netId ↔ scene entity uuid
inline constexpr MessageId kMsgSpawn    = kFirstUserMessage + 208;   // host → client: netId, owner, class, pose
inline constexpr MessageId kMsgDespawn  = kFirstUserMessage + 209;   // host → client: netId

// ── Join baseline (GameReplication::sendBaseline, step 4) ──
// A snapshot is Unreliable and skips entities with replicateTransform = false;
// a joining client would therefore either miss the one state it is never sent
// again, or have it overtaken and dropped as stale by the next regular tick.
// The baseline is the same samples, ReliableOrdered, outside the tick ordering.
inline constexpr MessageId kMsgBaseline = kFirstUserMessage + 210;   // host → client

// ── Possession (NetGameSession::assignControl, step 5) ──
// Host → the OWNER alone, after the spawn that created their character. Carries
// the net id, because a net id is the only name for an entity both sides know:
// the client resolves it through GameReplication::entityOf and then does the
// two things that make it theirs — setLocallyControlled (prediction) and
// player.possess (camera, input routing, player.character()).
inline constexpr MessageId kMsgControl = kFirstUserMessage + 213;   // host → owner: netId

// ── Reserved, so nothing else takes the number before its step lands ──
// 214 kMsgScene / 215 kMsgSceneReady (scene change, plan §5.5)        — step 5
// 216 kMsgPropertyTable / 217 kMsgProperties (plan §6.2)              — step 6
// 218 kMsgRpc (plan §7.3)                                             — step 7

// The gameplay protocol's own version, compared in the Hello. Separate from
// kCollabProtocolVersion because the two protocols change for different reasons
// and on different days; sharing one number would make every collab change
// refuse every game session.
inline constexpr std::uint16_t kGameProtocolVersion = 1;

} // namespace HE::Net::Game
