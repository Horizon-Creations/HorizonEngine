#pragma once

// ─── Layer 3a — who is in the session ────────────────────────────────────────
// The concept the replication layer was missing. GameReplication knows
// CONNECTIONS and entity ids; nothing until now knew PLAYERS — that connection
// 7 is "Anna", that she is player 3, that the character with net id 12 is hers.
//
// A PlayerId is not a ConnectionId on purpose. A connection id belongs to the
// transport and may be REUSED after a peer goes away (the anti-cheat plan's
// rule about inheriting a dropped peer's state exists because of exactly that);
// a player id is minted once per join and never handed out twice in one
// session, so a score, a name or a kill count may safely be keyed on it.

#include <Net/NetCommon.h>

#include <cstdint>
#include <string>
#include <vector>

namespace HE::Net::Game {

// 1 is always the host's own player, 2… are handed out in join order.
// 0 is "nobody", which is what an unowned entity carries.
using PlayerId = std::uint32_t;
inline constexpr PlayerId kNoPlayer   = 0;
inline constexpr PlayerId kHostPlayer = 1;

struct PlayerInfo
{
	PlayerId     id   = kNoPlayer;
	ConnectionId conn = kInvalidConnection;   // kInvalidConnection for the host itself
	std::string  name;                        // display name from the Hello
	// The character this player drives, as a network id. 0 until the host
	// possesses one for them (step 5); kept here so step 5 has nothing to add
	// to the wire format, only something to fill in.
	std::uint32_t characterNetId = 0;
	// The HorizonCode instance of their PlayerController, likewise filled in by
	// step 5's PlayerHost rework.
	std::uint32_t controllerInstance = 0;
	bool          local = false;              // this process's own player
};

// A small ordered list rather than a map: a session has a handful of players,
// iteration order is what the UI and the tests want to be stable, and every
// lookup here is over single digits.
class PlayerRoster
{
public:
	const std::vector<PlayerInfo>& players() const { return m_players; }
	std::size_t size() const { return m_players.size(); }
	bool        empty() const { return m_players.empty(); }

	// Mint the next id and record the player. The counter never rewinds, so a
	// connection id the transport reuses cannot be mistaken for the player who
	// held it before.
	PlayerId add(ConnectionId conn, std::string name, bool local = false)
	{
		PlayerInfo info;
		info.id    = m_nextId++;
		info.conn  = conn;
		info.name  = std::move(name);
		info.local = local;
		m_players.push_back(std::move(info));
		return m_players.back().id;
	}

	// Record a player under an id somebody ELSE minted — which on a client is
	// every id, including its own: the host hands it out in the Welcome. Without
	// this a client's roster entry carried 1 while localPlayer() said 2, so
	// net.playerName(net.localPlayer()) came back empty on every client.
	// The counter is pulled past it, so a later add() cannot mint it twice.
	void addWithId(PlayerId id, ConnectionId conn, std::string name, bool local = false)
	{
		if (id == kNoPlayer) return;
		PlayerInfo info;
		info.id    = id;
		info.conn  = conn;
		info.name  = std::move(name);
		info.local = local;
		m_players.push_back(std::move(info));
		if (id >= m_nextId) m_nextId = id + 1;
	}

	// Remove by id or by connection. Returns the id that left, or kNoPlayer.
	PlayerId removeById(PlayerId id)
	{
		for (auto it = m_players.begin(); it != m_players.end(); ++it)
			if (it->id == id) { m_players.erase(it); return id; }
		return kNoPlayer;
	}
	PlayerId removeByConnection(ConnectionId conn)
	{
		for (auto it = m_players.begin(); it != m_players.end(); ++it)
			if (it->conn == conn) { const PlayerId id = it->id; m_players.erase(it); return id; }
		return kNoPlayer;
	}

	PlayerInfo*       find(PlayerId id)
	{
		for (auto& p : m_players) if (p.id == id) return &p;
		return nullptr;
	}
	const PlayerInfo* find(PlayerId id) const
	{
		for (const auto& p : m_players) if (p.id == id) return &p;
		return nullptr;
	}
	PlayerInfo*       findByConnection(ConnectionId conn)
	{
		for (auto& p : m_players) if (p.conn == conn) return &p;
		return nullptr;
	}
	const PlayerInfo* findByConnection(ConnectionId conn) const
	{
		for (const auto& p : m_players) if (p.conn == conn) return &p;
		return nullptr;
	}

	void clear() { m_players.clear(); m_nextId = kHostPlayer; }

private:
	std::vector<PlayerInfo> m_players;
	PlayerId                m_nextId = kHostPlayer;
};

} // namespace HE::Net::Game
