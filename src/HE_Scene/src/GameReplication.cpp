#include "HorizonScene/GameReplication.h"

#include "HorizonScene/AntiCheat/AntiCheatService.h"
#include "HorizonScene/Components/MovementComponent.h"
#include "HorizonScene/Components/NetworkComponent.h"
#include "HorizonScene/Components/TransformComponent.h"

#include <Diagnostics/Log.h>

#include <algorithm>
#include <cmath>



using namespace HE::Net;

namespace
{
	// Gameplay snapshots live in their own message-id range, well clear of the
	// collaboration protocol's — the two systems share a transport but never a
	// message.
	constexpr MessageId kMsgSnapshot  = kFirstUserMessage + 200;
	constexpr MessageId kMsgInput     = kFirstUserMessage + 201;   // client → server
	constexpr MessageId kMsgIntegrity = kFirstUserMessage + 202;   // client → server, once

	// A manifest is the exe, a handful of libraries and a few paks. Anything
	// beyond this did not come from our writer.
	constexpr std::uint16_t kMaxManifestEntries = 256;

	// Speed bounds for the anti-cheat displacement check. The engine does not
	// know the game's mover, so it takes the bound from what the entity carries:
	// NetworkComponent::maxSpeed once step 3 of the plan adds it (it takes
	// precedence), MovementComponent::maxSpeed as the fallback. Neither present
	// = 0 = unchecked. Vertical stays unchecked until a game declares it —
	// jumps and falls depend on the game's gravity, and one horizontal bound
	// would either forbid falling or allow flying.
	HE::AntiCheat::MoveLimits resolveMoveLimits(const entt::registry& reg, Entity entity)
	{
		HE::AntiCheat::MoveLimits limits;
		if (const auto* mc = reg.try_get<MovementComponent>(entity))
			limits.maxSpeed = std::max(0.0f, mc->maxSpeed);
		return limits;
	}
} // namespace

GameReplication::GameReplication(NetSession* net, NetRole role, Config cfg)
	: m_net(net), m_role(role), m_cfg(cfg)
{
	if (!m_net) return;

	// Only a client consumes snapshots; a server that received one would be
	// taking orders from a peer, which is the opposite of authoritative.
	if (m_role != NetRole::Server && m_role != NetRole::Host)
	{
		m_net->on(kMsgSnapshot, [this](ConnectionId, BitReader& r) {
			applySnapshot(r);
		});
	}
	else
	{
		m_net->on(kMsgInput, [this](ConnectionId conn, BitReader& r) {
			handleInput(conn, r);
		});
		m_net->on(kMsgIntegrity, [this](ConnectionId conn, BitReader& r) {
			handleIntegrity(conn, r);
		});
	}
}

void GameReplication::assignControl(ConnectionId conn, std::uint32_t netId)
{
	m_controlledByConn[conn] = netId;
}

void GameReplication::setLocallyControlled(Entity entity, std::uint32_t netId)
{
	m_controlled      = entity;
	m_controlledNetId = netId;
	m_pendingInputs.clear();
	m_positionError = glm::vec3(0.0f);
	// The controlled entity is predicted, never interpolated — interpolation
	// would fight the prediction and produce visible rubber-banding.
	m_interp.erase(netId);
}

std::uint32_t GameReplication::pushInput(const glm::vec3& move, float yaw, float dt)
{
	if (!m_world || m_controlled == entt::null || !m_move) return 0;
	auto& reg = m_world->registry();
	if (!reg.valid(m_controlled)) return 0;
	auto* tc = reg.try_get<TransformComponent>(m_controlled);
	if (!tc) return 0;

	InputCommand cmd;
	cmd.sequence  = ++m_inputSequence;
	// Same clamp the server applies. Predicting with the raw dt would diverge on
	// any frame longer than the bound — a hitch, a breakpoint, a loading spike —
	// and the player would feel a correction every time.
	cmd.deltaTime = std::clamp(dt, 0.0f, kMaxInputDeltaTime);
	cmd.move      = move;
	cmd.yaw       = yaw;

	// Apply it NOW. This is the whole point: the character responds on the frame
	// the key was pressed, not a round trip later.
	m_move(*tc, cmd);
	tc->dirty = true;

	// Keep it until the server confirms it, so it can be replayed on top of the
	// authoritative state.
	m_pendingInputs.push_back(cmd);
	if (m_pendingInputs.size() > m_cfg.maxPendingInputs)
	{
		// The link is unusable at this point; dropping the oldest keeps memory
		// bounded rather than pretending we can still reconcile them.
		m_pendingInputs.erase(m_pendingInputs.begin());
	}

	if (m_net && !m_net->connections().empty())
	{
		BitWriter w;
		w.writeUInt32(cmd.sequence);
		w.writeFloat(cmd.deltaTime);
		for (int i = 0; i < 3; ++i) w.writeFloat(cmd.move[i]);
		w.writeFloat(cmd.yaw);
		// Unreliable: a lost input is superseded by the next one, and the server
		// acknowledges by sequence so gaps are self-healing.
		m_net->send(m_net->connections().front(), kMsgInput, w, SendMode::Unreliable);
		++m_stats.inputsSent;
	}
	return cmd.sequence;
}

void GameReplication::handleInput(ConnectionId conn, BitReader& r)
{
	// This is the only place client claims enter the simulation, which makes it
	// the only place validation can live. Every refusal below used to be a
	// silent return; with an anti-cheat service attached, the ones a legitimate
	// client cannot cause become observations. Without one, nothing changes.
	InputCommand cmd;
	const bool parsed = r.readUInt32(cmd.sequence) && r.readFloat(cmd.deltaTime) &&
	                    r.readFloat(cmd.move[0]) && r.readFloat(cmd.move[1]) &&
	                    r.readFloat(cmd.move[2]) && r.readFloat(cmd.yaw);
	if (!parsed)
	{
		// A real client never sends a truncated command: the writer is ours.
		if (m_antiCheat)
			m_antiCheat->observe(conn, HE::AntiCheat::Kind::Malformed, 0.0f,
			                     "truncated input command");
		return;
	}

	// Out-of-order or duplicated input must not be applied twice — UDP-style
	// delivery makes both normal. Silent on purpose, even with anti-cheat: a
	// duplicate is normal on every channel and says nothing about the client.
	auto& last = m_lastProcessedInput[conn];
	if (cmd.sequence <= last) return;

	// A client may only drive the entity it was assigned. Without this check a
	// client could move anyone's character by sending input for their id. No
	// assignment at all is the client's doing (Hard: it controls nothing and
	// sends input anyway); an assignment that no longer resolves — the entity
	// was unregistered while its input was in flight — is server state, and
	// stays silent.
	const auto ctrlIt = m_controlledByConn.find(conn);
	if (ctrlIt == m_controlledByConn.end())
	{
		if (m_antiCheat)
			m_antiCheat->observe(conn, HE::AntiCheat::Kind::ForeignEntity, 0.0f,
			                     "input from a connection that controls no entity");
		return;
	}
	const auto entIt = m_byNetId.find(ctrlIt->second);
	if (entIt == m_byNetId.end()) return;

	auto& reg = m_world->registry();
	if (!reg.valid(entIt->second)) return;
	auto* tc = reg.try_get<TransformComponent>(entIt->second);
	if (!tc || !m_move) return;

	// Pre-apply: format (finite numbers), input rate, dt budget. A refused
	// command simply falls away; the next snapshot corrects the client exactly
	// like any misprediction, so no rollback path is needed here.
	if (m_antiCheat && !m_antiCheat->preApply(conn, cmd)) return;

	// Enforce the bound the client is also supposed to apply: a modified one
	// could otherwise send dt = 10 and cross the level in a single command.
	cmd.deltaTime = std::clamp(cmd.deltaTime, 0.0f, kMaxInputDeltaTime);

	const glm::vec3 before = tc->position;
	m_move(*tc, cmd);

	// Post-apply: the engine does not know the mover, so it measures what the
	// mover did. Further than maxSpeed·dt allows → back to where it was, and
	// the command still counts as processed so the client's replay retires it.
	if (m_antiCheat)
	{
		HE::AntiCheat::MoveCheck check;
		check.netId     = ctrlIt->second;
		check.before    = before;
		check.after     = tc->position;
		check.deltaTime = cmd.deltaTime;
		check.limits    = resolveMoveLimits(reg, entIt->second);
		check.quantStep = quantStep();
		if (!m_antiCheat->postApply(conn, check)) tc->position = before;
	}

	tc->dirty = true;
	last = cmd.sequence;
	++m_stats.inputsProcessed;
}

// ─── Registration ────────────────────────────────────────────────────────────

std::uint32_t GameReplication::registerEntity(Entity entity, std::uint32_t owner)
{
	if (!m_world || !m_world->registry().valid(entity)) return 0;

	auto& reg = m_world->registry();
	auto& nc  = reg.get_or_emplace<NetworkComponent>(entity);
	if (nc.netId == 0) nc.netId = m_nextNetId++;
	nc.owner = owner;

	m_byNetId[nc.netId] = entity;
	return nc.netId;
}

bool GameReplication::adoptEntity(Entity entity, std::uint32_t netId, std::uint32_t owner)
{
	if (!m_world || netId == 0) return false;
	auto& reg = m_world->registry();
	if (!reg.valid(entity)) return false;

	auto& nc = reg.get_or_emplace<NetworkComponent>(entity);
	nc.netId = netId;
	nc.owner = owner;
	m_byNetId[netId] = entity;
	return true;
}

void GameReplication::unregisterEntity(Entity entity)
{
	if (!m_world) return;
	auto& reg = m_world->registry();
	if (!reg.valid(entity)) return;

	if (auto* nc = reg.try_get<NetworkComponent>(entity); nc && nc->netId != 0)
	{
		m_byNetId.erase(nc->netId);
		m_interp.erase(nc->netId);
		nc->netId = 0;
	}
}

void GameReplication::setViewpoint(ConnectionId conn, const glm::vec3& position)
{
	m_viewpoints[conn] = position;
}

// ─── Tick ────────────────────────────────────────────────────────────────────

void GameReplication::update(float dt)
{
	if (!m_net || !m_world) return;

	resolveLocalManifest();

	if (m_role == NetRole::Server || m_role == NetRole::Host)
	{
		// The anti-cheat's wall clock is this frame delta: the dt budget
		// compares the simulated time clients claimed against the host time
		// that really passed, and this is where the host time passes.
		if (m_antiCheat) m_antiCheat->update(dt);

		checkGuestManifests();

		const float interval = (m_cfg.tickHz > 0.0f) ? (1.0f / m_cfg.tickHz) : 0.0f;
		if (interval <= 0.0f) return;

		m_tickAccumulator += dt;
		// A single send after a long stall, not one per missed tick: catching up
		// would burst a backlog of snapshots that are all already stale.
		if (m_tickAccumulator >= interval)
		{
			m_tickAccumulator = std::fmod(m_tickAccumulator, interval);
			sendSnapshots();
		}
		return;
	}

	// The manifest goes out once per connection, as soon as this side knows
	// what it has. NetSession's onConnect slot belongs to the application, so
	// new connections are noticed here rather than through a callback.
	if (m_localManifest != LocalManifest::Unknown)
		for (const ConnectionId conn : m_net->connections())
			if (m_manifestSentTo.insert(conn).second)
				sendManifest(conn);

	advanceInterpolation(dt);
	applySmoothing(dt);
}

// ─── Integrity (plan §3.5) ───────────────────────────────────────────────────

void GameReplication::setLocalManifest(std::optional<HE::Integrity::Manifest> manifest)
{
	if (manifest)
	{
		m_manifest      = std::move(*manifest);
		m_localManifest = LocalManifest::Have;
	}
	else
	{
		m_manifest      = {};
		m_localManifest = LocalManifest::None;
	}
}

void GameReplication::resolveLocalManifest()
{
	if (m_localManifest != LocalManifest::Unknown) return;
	// The probe hashes on a worker; while it runs, nothing is sent and nothing
	// is compared. Idle means it was never started: a dev build, no manifest.
	auto& probe = HE::Integrity::IntegrityProbe::instance();
	switch (probe.state())
	{
	case HE::Integrity::IntegrityProbe::State::Running: return;
	case HE::Integrity::IntegrityProbe::State::Ready:   setLocalManifest(probe.manifest()); return;
	case HE::Integrity::IntegrityProbe::State::Idle:    setLocalManifest(std::nullopt);     return;
	}
}

void GameReplication::sendManifest(ConnectionId conn)
{
	BitWriter w;
	const bool present = m_localManifest == LocalManifest::Have;
	w.writeBool(present);
	if (present)
	{
		const auto count = static_cast<std::uint16_t>(
			std::min<std::size_t>(m_manifest.entries.size(), kMaxManifestEntries));
		w.writeUInt16(count);
		for (std::uint16_t i = 0; i < count; ++i)
		{
			const auto& e = m_manifest.entries[i];
			w.writeByte(static_cast<std::uint8_t>(e.kind));
			w.writeString(e.name);
			w.writeString(e.hash);
		}
	}
	// Reliable: this is the join handshake's one integrity message, and a lost
	// one would read as "the client never said" on the host.
	m_net->send(conn, kMsgIntegrity, w, SendMode::ReliableOrdered);
	++m_stats.manifestsSent;
}

void GameReplication::handleIntegrity(ConnectionId conn, BitReader& r)
{
	// One manifest per connection. A second one is not a client bug our
	// writer could produce, but it is also not worth a level: ignore it, and
	// keep the first — replacing it would let a client "repair" its list.
	if (!m_manifestReceived.insert(conn).second) return;

	GuestManifest guest;
	bool parsed = r.readBool(guest.present);
	if (parsed && guest.present)
	{
		std::uint16_t count = 0;
		parsed = r.readUInt16(count) && count <= kMaxManifestEntries;
		for (std::uint16_t i = 0; parsed && i < count; ++i)
		{
			HE::Integrity::Entry e;
			std::uint8_t kind = 0;
			parsed = r.readByte(kind) && kind <= static_cast<std::uint8_t>(HE::Integrity::FileKind::Pak) &&
			         r.readString(e.name) && r.readString(e.hash);
			if (!parsed) break;
			e.kind = static_cast<HE::Integrity::FileKind>(kind);
			guest.manifest.entries.push_back(std::move(e));
		}
	}
	if (!parsed)
	{
		// Same rule as handleInput: our writer never truncates, so this is Hard.
		if (m_antiCheat)
			m_antiCheat->observe(conn, HE::AntiCheat::Kind::Malformed, 0.0f,
			                     "malformed integrity manifest");
		return;
	}

	// Queue rather than compare now: our own manifest may still be hashing.
	m_pendingGuestManifests[conn] = std::move(guest);
}

void GameReplication::checkGuestManifests()
{
	if (m_pendingGuestManifests.empty()) return;
	if (m_localManifest == LocalManifest::Unknown) return;   // still hashing

	if (m_localManifest == LocalManifest::None)
	{
		// A dev build has nothing to compare against. Say so once, not per
		// join, and drop what arrived.
		if (!m_integrityOffLogged)
		{
			HE_LOG_INFO(AntiCheat, "%s", "integrity check off: this build has no manifest "
			                             "(dev build or editor)");
			m_integrityOffLogged = true;
		}
		m_pendingGuestManifests.clear();
		return;
	}

	for (const auto& [conn, guest] : m_pendingGuestManifests)
		checkGuestManifest(conn, guest);
	m_pendingGuestManifests.clear();
}

void GameReplication::checkGuestManifest(ConnectionId conn, const GuestManifest& guest)
{
	// Anti-cheat OFF is the absence of the service; the switch is the
	// project setting. Either way the comparison is not made, not made and
	// hidden.
	if (!m_antiCheat || !m_cfg.integrityCheck) return;

	++m_stats.manifestsChecked;
	if (!guest.present)
	{
		// The host runs a packaged build, so the client should too. A client
		// without a manifest in a session that has one is a different program.
		++m_stats.integrityMismatches;
		m_antiCheat->integrityMismatch(conn, "no manifest from client");
		return;
	}

	const auto diffs = HE::Integrity::compare(m_manifest, guest.manifest);
	for (const auto& d : diffs)
	{
		++m_stats.integrityMismatches;
		m_antiCheat->integrityMismatch(conn, HE::Integrity::describe(d));
	}
	if (diffs.empty())
		HE_LOG_DEBUG(AntiCheat, "conn %u: integrity manifest matches (%zu entries)", conn,
		             m_manifest.entries.size());
}

float GameReplication::quantStep() const
{
	return (2.0f * m_cfg.worldExtent) /
	       static_cast<float>((1u << std::min(m_cfg.positionBits, 30)) - 1u);
}

void GameReplication::writeSample(BitWriter& w, const Sample& s) const
{
	const float e = m_cfg.worldExtent;
	for (int i = 0; i < 3; ++i)
		w.writeFloatQuantized(s.position[i], -e, e, m_cfg.positionBits);
	for (int i = 0; i < 3; ++i)
		w.writeFloatQuantized(s.rotation[i], -180.0f, 180.0f, m_cfg.rotationBits);
}

bool GameReplication::readSample(BitReader& r, Sample& s) const
{
	const float e = m_cfg.worldExtent;
	for (int i = 0; i < 3; ++i)
	{
		if (!r.readFloatQuantized(s.position[i], -e, e, m_cfg.positionBits)) return false;
	}
	for (int i = 0; i < 3; ++i)
	{
		if (!r.readFloatQuantized(s.rotation[i], -180.0f, 180.0f, m_cfg.rotationBits))
			return false;
	}
	return true;
}

void GameReplication::sendSnapshots()
{
	auto& reg = m_world->registry();

	// Built per client, because interest management makes each one different —
	// that is the whole point of it.
	for (const ConnectionId conn : m_net->connections())
	{
		const auto viewIt = m_viewpoints.find(conn);
		const bool hasView = (viewIt != m_viewpoints.end());

		std::vector<std::pair<std::uint32_t, Sample>> relevant;

		for (const auto& [netId, entity] : m_byNetId)
		{
			if (!reg.valid(entity)) continue;
			auto* nc = reg.try_get<NetworkComponent>(entity);
			auto* tc = reg.try_get<TransformComponent>(entity);
			if (!nc || !tc || !nc->replicateTransform) continue;

			if (hasView)
			{
				const float d2 = glm::dot(tc->position - viewIt->second,
				                          tc->position - viewIt->second);
				if (d2 > nc->relevanceRadius * nc->relevanceRadius)
				{
					++m_stats.entitiesCulled;
					continue;
				}
			}

			Sample s;
			s.position = tc->position;
			s.rotation = tc->rotation;
			relevant.emplace_back(netId, s);
		}

		// An empty snapshot carries no information; sending it would just be a
		// heartbeat, and the transport already has one.
		if (relevant.empty()) continue;

		BitWriter w;
		// The last input we processed FROM THIS CLIENT rides along with the
		// snapshot. Without it the client cannot know which of its predicted
		// moves the authoritative state already includes, and would replay all
		// of them — double-applying its own movement.
		const auto ackIt = m_lastProcessedInput.find(conn);
		w.writeUInt32(ackIt != m_lastProcessedInput.end() ? ackIt->second : 0u);
		w.writeUInt16(static_cast<std::uint16_t>(
			std::min<std::size_t>(relevant.size(), 0xFFFF)));
		for (const auto& [netId, sample] : relevant)
		{
			w.writeUInt32(netId);
			writeSample(w, sample);
		}

		// Unreliable by intent: a lost snapshot is corrected by the next one, and
		// waiting for a retransmit would deliver state that is already wrong.
		m_net->send(conn, kMsgSnapshot, w, SendMode::Unreliable);

		++m_stats.snapshotsSent;
		m_stats.entitiesSent += static_cast<std::uint32_t>(relevant.size());
		m_stats.bytesSent    += static_cast<std::uint32_t>(w.data().size());
	}
}

void GameReplication::applySnapshot(BitReader& r)
{
	std::uint32_t ack = 0;
	std::uint16_t count = 0;
	if (!r.readUInt32(ack) || !r.readUInt16(count)) return;

	++m_stats.snapshotsReceived;

	for (std::uint16_t i = 0; i < count; ++i)
	{
		std::uint32_t netId = 0;
		Sample s;
		if (!r.readUInt32(netId) || !readSample(r, s)) return;   // truncated

		if (netId == m_controlledNetId && m_controlled != entt::null)
		{
			// Our own entity is predicted, not interpolated — hand it to
			// reconciliation instead of the interpolation buffer.
			reconcile(s, ack);
			continue;
		}

		// Shift the buffer: what was current becomes the interpolation origin.
		InterpState& st = m_interp[netId];
		st.previous    = st.current;
		st.current     = s;
		st.hasPrevious = true;
		st.elapsed     = 0.0f;
	}
}

void GameReplication::reconcile(const Sample& authoritative, std::uint32_t ackedSequence)
{
	if (!m_world || !m_move) return;
	auto& reg = m_world->registry();
	if (!reg.valid(m_controlled)) return;
	auto* tc = reg.try_get<TransformComponent>(m_controlled);
	if (!tc) return;

	// Everything the server has already accounted for is history.
	m_pendingInputs.erase(
		std::remove_if(m_pendingInputs.begin(), m_pendingInputs.end(),
		               [ackedSequence](const InputCommand& c) {
		                   return c.sequence <= ackedSequence;
		               }),
		m_pendingInputs.end());

	// Undo any smoothing offset still in flight FIRST. tc->position is what the
	// player sees, which is the logical position plus that offset; comparing
	// against it directly would fold the offset into the next error and make the
	// correction feed on itself, drifting a little further every snapshot.
	tc->position -= m_positionError;
	m_positionError = glm::vec3(0.0f);

	// Where we believe we are, before adopting the server's answer.
	const glm::vec3 predicted = tc->position;

	// Adopt the authoritative state, then REPLAY every input the server has not
	// seen yet. That is the whole trick: the result is the server's truth plus
	// exactly the moves still in flight — not a rewind the player would feel.
	tc->position = authoritative.position;
	tc->rotation = authoritative.rotation;
	for (const InputCommand& cmd : m_pendingInputs) m_move(*tc, cmd);

	const glm::vec3 corrected = tc->position;
	const glm::vec3 error     = predicted - corrected;
	const float     dist      = glm::length(error);

	// The dead zone has to be wider than what the wire can even represent.
	// Positions arrive quantized, so a perfect prediction still differs by up to
	// one step — treating that as an error would count every snapshot as a
	// correction and leave a smoothing offset permanently active, which reads as
	// constant micro-jitter.
	if (dist <= quantStep() * 3.0f)
	{
		// Prediction was right, which is the common case on a healthy link.
		tc->dirty = true;
		return;
	}

	++m_stats.reconciliations;

	if (dist > m_cfg.reconcileSnapDistance)
	{
		// Too far to hide. Easing a large error looks like sliding on ice, and
		// lasts long enough that the player acts on a position that is wrong.
		m_positionError = glm::vec3(0.0f);
		++m_stats.hardSnaps;
	}
	else
	{
		// Small error: stay visually where we were and ease toward the truth, so
		// a minor misprediction is not a visible jolt. The offset is added back
		// onto the corrected position right here — otherwise the entity would
		// jump to the server's answer this frame and the smoothing would have
		// nothing left to hide.
		m_positionError = error;
		tc->position += m_positionError;
	}
	tc->dirty = true;
}

void GameReplication::applySmoothing(float dt)
{
	if (m_controlled == entt::null) return;
	if (glm::dot(m_positionError, m_positionError) < 1e-10f) return;
	if (!m_world) return;

	auto& reg = m_world->registry();
	if (!reg.valid(m_controlled)) return;
	auto* tc = reg.try_get<TransformComponent>(m_controlled);
	if (!tc) return;

	// Exponential decay, framerate-independent. Subtract exactly the amount the
	// offset shrank by, so the entity converges on the authoritative position
	// without the two ever fighting each other.
	const float k = std::exp(-m_cfg.reconcileSmoothing * dt);
	const glm::vec3 before = m_positionError;
	m_positionError *= k;
	tc->position -= (before - m_positionError);
	tc->dirty = true;
}

void GameReplication::advanceInterpolation(float dt)
{
	auto& reg = m_world->registry();
	const float span = std::max(0.001f, m_cfg.interpolationDelaySec);

	for (auto& [netId, st] : m_interp)
	{
		// The controlled entity is predicted; interpolating it too would fight
		// the prediction and rubber-band visibly.
		if (netId == m_controlledNetId) continue;

		const auto it = m_byNetId.find(netId);
		if (it == m_byNetId.end() || !reg.valid(it->second)) continue;

		auto* tc = reg.try_get<TransformComponent>(it->second);
		if (!tc) continue;

		st.elapsed += dt;
		// Without this the entity would step once per snapshot — visibly chunky
		// at 30 Hz on a 60 Hz display.
		const float t = st.hasPrevious ? std::min(1.0f, st.elapsed / span) : 1.0f;

		tc->position = glm::mix(st.previous.position, st.current.position, t);

		// Rotation is interpolated per component along the SHORTER arc: the delta
		// is wrapped into [-180, 180] first. Going from 179° to -179° is a 2°
		// turn, but a plain lerp walks the other 358° — a visible spin every time
		// something crosses the wrap.
		//
		// Deliberately NOT a quaternion slerp, even though that is the more
		// correct rotational path. glm::eulerAngles returns an *equivalent* but
		// different decomposition (a 180° yaw comes back as x=180, y≈0, z=180),
		// so a round trip rewrites the stored triple. The orientation would
		// render identically, but any game code reading rotation.y off a
		// replicated entity would see values it never set. Preserving the
		// representation this engine actually stores is worth more here than a
		// perfect interpolation path, and for single-axis turns — which is what
		// characters do — the two are identical anyway.
		const auto shortestLerp = [](float a, float b, float f) {
			float delta = std::fmod(b - a + 540.0f, 360.0f) - 180.0f;
			return a + delta * f;
		};
		tc->rotation = glm::vec3(
			shortestLerp(st.previous.rotation.x, st.current.rotation.x, t),
			shortestLerp(st.previous.rotation.y, st.current.rotation.y, t),
			shortestLerp(st.previous.rotation.z, st.current.rotation.z, t));
		tc->dirty    = true;
	}
}
