#include "CollabController.h"

#include <Net/HttpsClient.h>
#include <Net/NetSession.h>
#include <Net/SecureTransport.h>
#include <Net/SessionDirectory.h>
#include <Net/Socket.h>
#include <Net/TcpTransport.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <thread>

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

	// Publish the endpoint so peers only ever need the session id. This is an
	// HTTPS round trip, so it runs off-thread — doing it here would stall the
	// editor for as long as the request takes.
	m_sessionId       = SessionDirectory::newSessionId();
	m_directoryStatus = "Publishing session...";

	const std::string endpoint = directoryEndpoint();
	const std::string sid      = m_sessionId;
	const std::uint16_t port16 = m_port;
	m_registerFuture = std::async(std::launch::async, [endpoint, sid, port16, displayName] {
		RegisterResult r;
		if (!HE::Net::httpsAvailable())
		{
			r.error = "No HTTPS support in this build — share the address manually.";
			return r;
		}
		SessionDirectory dir(endpoint);
		SessionRegistration reg;
		const DirectoryStatus st = dir.registerSession(sid, port16, displayName,
		                                               "HorizonEngine", 1, reg);
		if (st != DirectoryStatus::Ok)
		{
			r.error = "Session directory unreachable — share the address manually.";
			return r;
		}
		r.ok        = true;
		r.publicIp  = reg.publicIp;
		r.reachable = reg.reachable;
		r.token     = reg.token;
		return r;
	});

	return true;
}

void CollabController::participantColor(HE::Net::ParticipantId id, float outRgb[3])
{
	// Golden-ratio hue stepping: consecutive ids land far apart on the colour
	// wheel, so two people who joined one after another never get near-identical
	// gizmos. Full saturation, high value — these are overlay lines, not surfaces.
	const float hue = std::fmod(static_cast<float>(id) * 0.618033988f, 1.0f);
	const float h6  = hue * 6.0f;
	const int   sector = static_cast<int>(h6) % 6;
	const float f   = h6 - std::floor(h6);
	const float q   = 1.0f - f;

	switch (sector)
	{
	case 0: outRgb[0] = 1.0f; outRgb[1] = f;    outRgb[2] = 0.0f; break;
	case 1: outRgb[0] = q;    outRgb[1] = 1.0f; outRgb[2] = 0.0f; break;
	case 2: outRgb[0] = 0.0f; outRgb[1] = 1.0f; outRgb[2] = f;    break;
	case 3: outRgb[0] = 0.0f; outRgb[1] = q;    outRgb[2] = 1.0f; break;
	case 4: outRgb[0] = f;    outRgb[1] = 0.0f; outRgb[2] = 1.0f; break;
	default:outRgb[0] = 1.0f; outRgb[1] = 0.0f; outRgb[2] = q;    break;
	}
}

std::string CollabController::directoryEndpoint()
{
	if (const char* env = std::getenv("HE_COLLAB_DIRECTORY"); env && *env) return env;
	return "https://horizoncreations.dev/HorizonEngine/session-api.php";
}

bool CollabController::joinBySessionId(const std::string& sessionId,
                                       const std::string& joinCode,
                                       const std::string& displayName)
{
	teardown();
	m_error.clear();

	if (sessionId.empty() || joinCode.empty())
	{
		m_error  = "Session ID and join code are both required.";
		m_status = Status::Failed;
		return false;
	}
	if (!HE::Net::httpsAvailable())
	{
		m_error  = "This build has no HTTPS support, so a session ID cannot be resolved.";
		m_status = Status::Failed;
		return false;
	}

	// Remember what the connect will need once the address arrives.
	m_sessionId          = sessionId;
	m_pendingJoinCode    = joinCode;
	m_pendingDisplayName = displayName;
	m_isHost             = false;
	m_status             = Status::Connecting;
	m_directoryStatus    = "Looking up session...";

	const std::string endpoint = directoryEndpoint();
	const std::string sid      = sessionId;
	m_lookupFuture = std::async(std::launch::async, [endpoint, sid] {
		LookupResult r;
		SessionDirectory dir(endpoint);
		SessionLookup look;
		const DirectoryStatus st = dir.lookup(sid, look);
		switch (st)
		{
		case DirectoryStatus::Ok:
			r.ok   = true;
			r.host = look.host;
			r.port = look.port;
			break;
		case DirectoryStatus::NotFound:
			r.error = "No session with that ID — check it, or the host may have closed it.";
			break;
		default:
			r.error = "Could not reach the session directory.";
			break;
		}
		return r;
	});

	return true;
}

bool CollabController::joinSession(const std::string& host, std::uint16_t port,
                                   const std::string& joinCode,
                                   const std::string& displayName)
{
	teardown();
	m_error.clear();
	return beginLink(host, port, joinCode, displayName);
}

// Shared by the direct path and the one that resolved an address from the
// directory, so both establish the link identically.
bool CollabController::beginLink(const std::string& host, std::uint16_t port,
                                 const std::string& joinCode,
                                 const std::string& displayName)
{

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
	// Remove the directory entry so peers stop being handed a dead endpoint.
	// Detached because leaving must feel instant; if it fails the entry simply
	// expires on its TTL instead.
	if (m_isHost && !m_directoryToken.empty() && !m_sessionId.empty())
	{
		std::thread([endpoint = directoryEndpoint(), sid = m_sessionId,
		             token = m_directoryToken] {
			SessionDirectory dir(endpoint);
			dir.unregisterSession(sid, token);
		}).detach();
	}

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
	m_sessionId.clear();
	m_localAddress.clear();
	m_port          = 0;
	m_snapshotGot   = 0;
	m_snapshotTotal = 0;

	// Drop any directory work still in flight. Destroying a std::async future
	// blocks until its thread finishes, which is why the calls carry timeouts.
	if (m_registerFuture.valid()) m_registerFuture = {};
	if (m_lookupFuture.valid())   m_lookupFuture   = {};
	m_directoryToken.clear();
	m_directoryStatus.clear();
	m_reachable         = false;
	m_reachabilityKnown = false;
	m_pendingJoinCode.clear();
	m_pendingDisplayName.clear();
}

// ─── Pump ────────────────────────────────────────────────────────────────────

void CollabController::update(std::uint64_t nowMs)
{
	// Before the early-out: while a session id is being looked up there is no
	// transport yet, and that is exactly when the lookup result has to be
	// collected.
	pumpDirectory(nowMs);

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

// ─── Session directory ───────────────────────────────────────────────────────

bool CollabController::directoryBusy() const
{
	return m_registerFuture.valid() || m_lookupFuture.valid();
}

void CollabController::pumpDirectory(std::uint64_t nowMs)
{
	using namespace std::chrono_literals;

	// ── Host: collect the registration result ──
	if (m_registerFuture.valid() &&
	    m_registerFuture.wait_for(0s) == std::future_status::ready)
	{
		const RegisterResult r = m_registerFuture.get();
		if (r.ok)
		{
			m_directoryToken     = r.token;
			m_reachable          = r.reachable;
			m_reachabilityKnown  = true;
			m_lastHeartbeatMs    = nowMs;
			if (!r.publicIp.empty()) m_localAddress = r.publicIp;
			m_directoryStatus = r.reachable
				? "Session published — peers can join with the ID."
				// Not an error: the entry exists, but nobody outside this network
				// will get through it. Saying so now beats an unexplained timeout
				// on the guest's side later.
				: "Published, but this machine is not reachable from outside. "
				  "Peers on other networks need the port forwarded.";
		}
		else
		{
			m_directoryStatus = r.error;
			// Hosting itself is unaffected — the session still works for anyone
			// given the address directly.
		}
	}

	// ── Host: keep the entry alive ──
	// The directory expires entries after ~150 s, so refresh well inside that.
	if (m_isHost && !m_directoryToken.empty() && !m_registerFuture.valid() &&
	    nowMs - m_lastHeartbeatMs > 60'000)
	{
		m_lastHeartbeatMs = nowMs;
		const std::string endpoint = directoryEndpoint();
		const std::string sid      = m_sessionId;
		const std::string token    = m_directoryToken;
		// Fire and forget: a missed heartbeat only shortens the entry's life.
		std::thread([endpoint, sid, token] {
			SessionDirectory dir(endpoint);
			dir.heartbeat(sid, token);
		}).detach();
	}

	// ── Client: the lookup finished; now connect ──
	if (m_lookupFuture.valid() &&
	    m_lookupFuture.wait_for(0s) == std::future_status::ready)
	{
		const LookupResult r = m_lookupFuture.get();
		if (!r.ok)
		{
			m_error           = r.error;
			m_directoryStatus.clear();
			m_status          = Status::Failed;
			return;
		}
		m_directoryStatus = "Connecting to " + r.host + "...";
		if (!beginLink(r.host, r.port, m_pendingJoinCode, m_pendingDisplayName))
		{
			// beginLink already set the error and status.
			return;
		}
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
