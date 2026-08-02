#pragma once

// ─── Editor ↔ HorizonNet bridge ──────────────────────────────────────────────
// Owns the whole networking stack for a collaboration session so neither
// EditorApplication nor the UI has to know how it fits together:
//
//     TcpTransport → SecureTransport → NetSession → CollabSession
//
// This is also the layer where the scene finally meets the network. HorizonNet
// deliberately knows nothing about HorizonScene (see CollabSession's
// ISessionStateProvider), so the implementation that serializes the world lives
// here — in the editor, the one place that already depends on both.
//
// Everything is poll-driven: update() must be called once per frame or nothing
// happens at all.

#include <Net/CollabSession.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class HorizonWorld;

namespace HE::Net {
class TcpTransport;
class SecureTransport;
class NetSession;
} // namespace HE::Net

class CollabController
{
public:
	enum class Status : std::uint8_t
	{
		Idle,        // nothing running
		Hosting,     // listening; peers may join
		Connecting,  // client: link up, join not yet complete
		Joined,      // client: snapshot applied, fully in the session
		Failed,      // see lastError()
	};

	CollabController();
	~CollabController();

	CollabController(const CollabController&)            = delete;
	CollabController& operator=(const CollabController&) = delete;

	// The world that is captured for joiners and replaced on join. Must outlive
	// any active session.
	void setWorld(HorizonWorld* world) { m_world = world; }

	// Fires after a received snapshot replaced the world. The editor must drop
	// its selection and undo history here — both refer to entity handles that no
	// longer exist, and acting on them would touch freed storage.
	void onWorldReplaced(std::function<void()> fn) { m_onWorldReplaced = std::move(fn); }

	// ── Lifecycle ──
	// Opens a session on `port` (0 = OS-assigned) and generates a join code.
	bool startHosting(std::uint16_t port, const std::string& displayName);
	bool joinSession(const std::string& host, std::uint16_t port,
	                 const std::string& joinCode, const std::string& displayName);
	void leave();

	// Pump transport → session → collaboration. Call once per frame.
	void update(std::uint64_t nowMs);

	// ── State for the UI ──
	Status             status() const { return m_status; }
	bool               active() const { return m_status == Status::Hosting ||
	                                            m_status == Status::Connecting ||
	                                            m_status == Status::Joined; }
	bool               isHost() const { return m_isHost; }
	const std::string& joinCode() const { return m_joinCode; }
	const std::string& localAddress() const { return m_localAddress; }
	std::uint16_t      port() const { return m_port; }
	const std::string& lastError() const { return m_error; }

	// 0..1 while a snapshot is arriving; 1 when complete.
	float snapshotProgress() const;
	bool  snapshotInProgress() const { return m_snapshotTotal > 0 &&
	                                          m_snapshotGot < m_snapshotTotal; }

	std::vector<HE::Net::Participant> participants() const;

	// Short digest of the negotiated key, identical on both peers — lets two
	// users confirm out of band that they are in the same session.
	std::string sessionFingerprint() const;

	// ── Presence ──
	void setLocalPresence(const float cameraPos[3], const float cameraRot[4],
	                      const std::vector<std::uint64_t>& selection);
	const HE::Net::PresenceState* presenceOf(HE::Net::ParticipantId id) const;
	HE::Net::ParticipantId        localParticipant() const;

private:
	void teardown();
	void wireCallbacks();

	class SceneStateProvider;

	std::unique_ptr<SceneStateProvider>        m_provider;
	std::unique_ptr<HE::Net::SecureTransport>  m_secure;   // owns the TcpTransport
	std::unique_ptr<HE::Net::NetSession>       m_net;
	std::unique_ptr<HE::Net::CollabSession>    m_collab;

	HorizonWorld* m_world  = nullptr;
	Status        m_status = Status::Idle;
	bool          m_isHost = false;

	std::string   m_joinCode;
	std::string   m_localAddress;
	std::uint16_t m_port = 0;
	std::string   m_error;

	std::uint32_t m_snapshotGot   = 0;
	std::uint32_t m_snapshotTotal = 0;

	std::function<void()> m_onWorldReplaced;
};
