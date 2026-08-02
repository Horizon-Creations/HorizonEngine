#include "HorizonScene/GameReplication.h"

#include "HorizonScene/Components/NetworkComponent.h"
#include "HorizonScene/Components/TransformComponent.h"

#include <algorithm>
#include <cmath>

using namespace HE::Net;

namespace
{
	// Gameplay snapshots live in their own message-id range, well clear of the
	// collaboration protocol's — the two systems share a transport but never a
	// message.
	constexpr MessageId kMsgSnapshot = kFirstUserMessage + 200;
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

	if (m_role == NetRole::Server || m_role == NetRole::Host)
	{
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

	advanceInterpolation(dt);
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
	std::uint16_t count = 0;
	if (!r.readUInt16(count)) return;

	++m_stats.snapshotsReceived;

	for (std::uint16_t i = 0; i < count; ++i)
	{
		std::uint32_t netId = 0;
		Sample s;
		if (!r.readUInt32(netId) || !readSample(r, s)) return;   // truncated

		// Shift the buffer: what was current becomes the interpolation origin.
		InterpState& st = m_interp[netId];
		st.previous    = st.current;
		st.current     = s;
		st.hasPrevious = true;
		st.elapsed     = 0.0f;
	}
}

void GameReplication::advanceInterpolation(float dt)
{
	auto& reg = m_world->registry();
	const float span = std::max(0.001f, m_cfg.interpolationDelaySec);

	for (auto& [netId, st] : m_interp)
	{
		const auto it = m_byNetId.find(netId);
		if (it == m_byNetId.end() || !reg.valid(it->second)) continue;

		auto* tc = reg.try_get<TransformComponent>(it->second);
		if (!tc) continue;

		st.elapsed += dt;
		// Without this the entity would step once per snapshot — visibly chunky
		// at 30 Hz on a 60 Hz display.
		const float t = st.hasPrevious ? std::min(1.0f, st.elapsed / span) : 1.0f;

		tc->position = glm::mix(st.previous.position, st.current.position, t);
		// Euler angles are interpolated componentwise, which is wrong across the
		// ±180° seam. Acceptable for v1; a quaternion path is the fix, and is
		// noted with prediction as the next step.
		tc->rotation = glm::mix(st.previous.rotation, st.current.rotation, t);
		tc->dirty    = true;
	}
}
