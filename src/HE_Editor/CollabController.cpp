#include "CollabController.h"

#include <Net/NetSession.h>
#include <Net/SecureTransport.h>
#include <Net/Socket.h>
#include <Net/TcpTransport.h>

#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSerializer.h>

#include <Diagnostics/Logger.h>

using namespace HE::Net;

// ─── Scene ↔ session state ───────────────────────────────────────────────────
// The concrete half of ISessionStateProvider. It lives here rather than in
// HorizonNet because HorizonNet sits below the scene layer and must not depend
// on it.
class CollabController::SceneStateProvider final : public ISessionStateProvider
{
public:
	explicit SceneStateProvider(CollabController& owner) : m_owner(owner) {}

	bool captureSnapshot(std::vector<std::uint8_t>& out) override
	{
		if (!m_owner.m_world) return false;
		SceneSerializer serializer;
		return serializer.saveToMemory(*m_owner.m_world, out);
	}

	bool applySnapshot(const std::vector<std::uint8_t>& data) override
	{
		if (!m_owner.m_world) return false;

		// loadFromMemory MERGES — it does not clear first. Without this the
		// host's scene would be added on top of whatever the joiner already had
		// open, silently duplicating everything.
		m_owner.m_world->clear();

		// An empty snapshot is a legitimate state (a brand-new project), and
		// clearing is the whole job in that case.
		bool ok = true;
		if (!data.empty())
		{
			SceneSerializer serializer;
			ok = serializer.loadFromMemory(*m_owner.m_world, data);
		}

		// Selection and undo history point at entity handles from the world that
		// just went away, so the editor has to drop them.
		if (ok && m_owner.m_onWorldReplaced) m_owner.m_onWorldReplaced();
		return ok;
	}

private:
	CollabController& m_owner;
};

CollabController::CollabController()
	: m_provider(std::make_unique<SceneStateProvider>(*this)) {}

CollabController::~CollabController() { teardown(); }

// ─── Lifecycle ───────────────────────────────────────────────────────────────

bool CollabController::startHosting(std::uint16_t port, const std::string& displayName)
{
	teardown();
	m_error.clear();

	auto listener = TcpTransport::listen(port);
	if (!listener)
	{
		m_error  = "Could not open port " + std::to_string(port) +
		           " (already in use, or blocked by the firewall).";
		m_status = Status::Failed;
		return false;
	}
	m_port = listener->boundPort();

	// Machine-generated, never user-chosen: an observer who captures a handshake
	// could brute-force a weak passphrase offline.
	m_joinCode = SecureTransport::generateJoinSecret();

	SecureTransport::Config sec;
	sec.joinSecret       = m_joinCode;
	sec.role             = NetRole::Host;
	sec.requireEncryption = true;

	m_secure = SecureTransport::wrap(std::move(listener), sec);
	if (!m_secure)
	{
		m_error  = "No crypto backend available — a session cannot be secured.";
		m_status = Status::Failed;
		return false;
	}

	m_net = std::make_unique<NetSession>(m_secure.get(), NetRole::Host);

	CollabSession::Config cfg;
	cfg.displayName = displayName;
	m_collab = std::make_unique<CollabSession>(m_net.get(), NetRole::Host, cfg);
	m_collab->setStateProvider(m_provider.get());
	wireCallbacks();

	const std::string lan = socketLocalAddress();
	m_localAddress = lan.empty() ? std::string("127.0.0.1") : lan;

	m_isHost = true;
	m_status = Status::Hosting;
	Logger::Log(Logger::LogLevel::Info,
	            ("Collab: hosting on " + m_localAddress + ":" + std::to_string(m_port)).c_str());
	return true;
}

bool CollabController::joinSession(const std::string& host, std::uint16_t port,
                                   const std::string& joinCode,
                                   const std::string& displayName)
{
	teardown();
	m_error.clear();

	if (joinCode.empty())
	{
		m_error  = "A join code is required.";
		m_status = Status::Failed;
		return false;
	}

	auto link = TcpTransport::connect(host, port);
	if (!link)
	{
		m_error  = "Could not reach " + host + ":" + std::to_string(port) + ".";
		m_status = Status::Failed;
		return false;
	}

	SecureTransport::Config sec;
	sec.joinSecret       = joinCode;
	sec.role             = NetRole::Client;
	sec.requireEncryption = true;

	m_secure = SecureTransport::wrap(std::move(link), sec);
	if (!m_secure)
	{
		m_error  = "No crypto backend available — a session cannot be secured.";
		m_status = Status::Failed;
		return false;
	}

	m_net = std::make_unique<NetSession>(m_secure.get(), NetRole::Client);

	CollabSession::Config cfg;
	cfg.displayName = displayName;
	m_collab = std::make_unique<CollabSession>(m_net.get(), NetRole::Client, cfg);
	m_collab->setStateProvider(m_provider.get());
	wireCallbacks();

	m_joinCode     = joinCode;
	m_localAddress = host;
	m_port         = port;
	m_isHost       = false;
	m_status       = Status::Connecting;
	Logger::Log(Logger::LogLevel::Info,
	            ("Collab: connecting to " + host + ":" + std::to_string(port)).c_str());
	return true;
}

void CollabController::wireCallbacks()
{
	m_collab->onJoined([this](HE::Net::ParticipantId id) {
		m_status = Status::Joined;
		Logger::Log(Logger::LogLevel::Info,
		            ("Collab: joined as participant " + std::to_string(id)).c_str());
	});

	m_collab->onJoinRejected([this](JoinRejectReason reason) {
		switch (reason)
		{
		case JoinRejectReason::VersionMismatch:
			m_error = "The host runs a different HorizonEngine collaboration version.";
			break;
		case JoinRejectReason::SessionFull:
			m_error = "The session is full.";
			break;
		case JoinRejectReason::SnapshotFailed:
			m_error = "The scene could not be transferred.";
			break;
		default:
			m_error = "The host refused the join.";
			break;
		}
		m_status = Status::Failed;
	});

	m_collab->onSnapshotProgress([this](std::uint32_t got, std::uint32_t total) {
		m_snapshotGot   = got;
		m_snapshotTotal = total;
	});
}

void CollabController::leave()
{
	teardown();
	m_status = Status::Idle;
	m_error.clear();
}

void CollabController::teardown()
{
	// Destruction order matters: CollabSession and NetSession hold raw pointers
	// into the transport, so they must go first.
	m_collab.reset();
	m_net.reset();
	m_secure.reset();

	m_isHost        = false;
	m_joinCode.clear();
	m_localAddress.clear();
	m_port          = 0;
	m_snapshotGot   = 0;
	m_snapshotTotal = 0;
}

// ─── Pump ────────────────────────────────────────────────────────────────────

void CollabController::update(std::uint64_t nowMs)
{
	if (!m_secure || !m_net || !m_collab) return;

	m_secure->update();   // drives the transport + handshake
	m_net->pump();        // dispatches framed messages
	m_collab->update(nowMs);

	// A client whose link dropped is no longer in a session; say so rather than
	// leaving a stale "Joined" in the UI.
	if (!m_isHost && m_status == Status::Joined && !m_collab->isJoined())
	{
		m_error  = "The connection to the host was lost.";
		m_status = Status::Failed;
	}
}

// ─── State ───────────────────────────────────────────────────────────────────

float CollabController::snapshotProgress() const
{
	if (m_snapshotTotal == 0) return 1.0f;
	return static_cast<float>(m_snapshotGot) / static_cast<float>(m_snapshotTotal);
}

std::vector<HE::Net::Participant> CollabController::participants() const
{
	if (!m_collab) return {};
	return m_collab->participants();
}

std::string CollabController::sessionFingerprint() const
{
	if (!m_secure) return {};
	// Host mode can hold several peers; the first established one is enough for
	// a visual check.
	for (ConnectionId c = 1; c <= 16; ++c)
	{
		std::string fp = m_secure->sessionFingerprint(c);
		if (!fp.empty()) return fp;
	}
	return {};
}

// ─── Presence ────────────────────────────────────────────────────────────────

void CollabController::setLocalPresence(const float cameraPos[3], const float cameraRot[4],
                                        const std::vector<std::uint64_t>& selection)
{
	if (m_collab) m_collab->setLocalPresence(cameraPos, cameraRot, selection);
}

const HE::Net::PresenceState* CollabController::presenceOf(HE::Net::ParticipantId id) const
{
	return m_collab ? m_collab->presenceOf(id) : nullptr;
}

HE::Net::ParticipantId CollabController::localParticipant() const
{
	return m_collab ? m_collab->localId() : HE::Net::kInvalidParticipant;
}
