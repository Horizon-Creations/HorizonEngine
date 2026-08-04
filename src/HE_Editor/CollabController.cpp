#include "CollabController.h"
#include "EditorAssetTypeCache.h"  // .hasset header sniff (the TYPE, not the extension)

#include <Net/HttpsClient.h>
#include <Net/PortMapper.h>
#include <Net/NetSession.h>
#include <Net/SecureTransport.h>
#include <Net/SessionDirectory.h>
#include <Net/Socket.h>
#include <Net/TcpTransport.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <thread>

#include <HorizonScene/Components/EntityIdComponent.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSerializer.h>

#include <Diagnostics/Logger.h>

using namespace HE::Net;

namespace {

// What to actually DO about an unreachable host. Kept in one place because the
// same advice belongs on three different failures (no port mapping, CGNAT,
// directory probe came back negative) and it would drift if written out thrice.
//
// The two remedies are deliberately ordered by how much work they cost the
// user, not by how elegant they are:
//   • Forwarding the port by hand always works when the ISP gives out a real
//     public address — it is only automation that failed, not connectivity.
//   • A mesh VPN is the answer when it does not, notably behind carrier-grade
//     NAT, where no forward at any level can be reached and the free tiers of
//     Tailscale/ZeroTier relay the traffic for you. Naming them beats a bare
//     "not reachable", which leaves the user with nothing to try.
// Note what this does NOT claim: that forwarding is yours to configure. On a
// company or university network the router belongs to someone else, and inbound
// connections are usually blocked on purpose — so the mesh VPN is named as a
// peer option rather than a fallback, and the local network as the one route
// that needs nobody's permission.
constexpr const char* kUnreachableRemedy =
	"What you can do: forward TCP port %u to this machine, if the router is "
	"yours to configure — on a company or campus network it usually is not, and "
	"inbound connections are often blocked deliberately. Otherwise have everyone "
	"join a mesh VPN such as Tailscale or ZeroTier (free tiers work and need no "
	"port forward at all). Guests on this same network can always connect "
	"directly.";

// The CGNAT variant deliberately omits manual forwarding: with a second NAT
// above the router, a hand-made forward is just as unreachable as the
// automatic one, so suggesting it would send the user off to spend an
// afternoon on something that cannot work.
constexpr const char* kCgnatRemedy =
	"What you can do: have everyone join a mesh VPN such as Tailscale or "
	"ZeroTier (free tiers relay the traffic for you), or collaborate on the "
	"same local network. Forwarding the port by hand will not help here.";

std::string remedyFor(std::uint16_t port)
{
	char buf[512];
	std::snprintf(buf, sizeof(buf), kUnreachableRemedy, static_cast<unsigned>(port));
	return buf;
}

} // namespace

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
	m_portMapStatus = "Asking the router to forward the port...";
	m_registerFuture = std::async(std::launch::async, [endpoint, sid, port16, displayName] {
		RegisterResult r;

		// Ask the router to forward the port BEFORE registering, so the
		// directory's reachability probe tests the mapped state rather than the
		// unmapped one. Doing it the other way round would warn the user about a
		// problem that was fixed a moment later.
		//
		// mapPort walks the ladder itself: UPnP first for its broader support,
		// NAT-PMP second because routers speak one or the other — an Apple base
		// station answers NAT-PMP and stays silent to SSDP entirely, so a UPnP
		// failure says nothing about whether the second attempt will work.
		HE::Net::PortMapping mapping;
		const HE::Net::PortMapResult mapResult = HE::Net::PortMapper::mapPort(
			port16, "HorizonEngine collaboration", r.mapping, mapping);
		if (mapResult == HE::Net::PortMapResult::Ok)
		{
			r.portMapped = true;
			using Method = HE::Net::PortMapper::MappingHandle::Method;
			const char* how = r.mapping.method == Method::NatPmp ? "NAT-PMP"
			                : r.mapping.method == Method::Pcp    ? "PCP"
			                                                     : "UPnP";
			r.mapStatus = std::string("Router forwarded the port automatically (") + how + ").";

			// A router can report a private or carrier-grade address as its own
			// WAN side, which means another NAT sits above it and no mapping at
			// this level can ever be reached.
			if (!mapping.externalIp.empty() &&
			    HE::Net::PortMapper::isPrivateOrCgnat(mapping.externalIp))
			{
				r.mapStatus = "Port forwarded, but your ISP uses carrier-grade NAT "
				              "(" + mapping.externalIp + ") — no port forward can be "
				              "reached from outside, not even one set up by hand.";
				r.advice    = kCgnatRemedy;
			}
		}
		else if (mapResult == HE::Net::PortMapResult::LocalNetworkBlocked)
		{
			// Distinct on purpose. The request never left this machine, so the
			// router — and everything the user might change about it — is
			// irrelevant. Reporting the usual "your router refused" here sends
			// people to spend an evening in router settings that were never even
			// consulted.
			r.mapStatus = "This app is not allowed to reach your local network, so the "
			              "router was never contacted. Nothing is wrong with your router.";
#if defined(__APPLE__)
			r.advice = "What you can do: allow HorizonEngine under System Settings > "
			           "Privacy & Security > Local Network. If it is not listed there, "
			           "macOS is holding a stale decision for an earlier build of the "
			           "editor and will neither ask again nor list it — restart your "
			           "Mac, launch the editor and host again; the permission prompt "
			           "then appears and the entry with it. (Running the editor as a "
			           "bare executable rather than the packaged app also causes this — "
			           "macOS only offers the permission to a proper app bundle.)";
#else
			r.advice = "What you can do: allow this application through your firewall, "
			           "then try again.";
#endif
		}
		else if (mapResult == HE::Net::PortMapResult::Refused)
		{
			// The router understood the request and declined it — a different
			// fact from "nothing answered", and the only one of the two the user
			// can actually fix. Saying the protocols are unsupported here would
			// be wrong: they worked, and were used to deliver the refusal.
			r.mapStatus = "Your router was reached and refused to forward the port — "
			              "automatic port forwarding is switched off for this device.";
			r.advice    = "What you can do: your router accepts forwarding requests, but "
			              "not from this machine. On a FRITZ!Box this is a per-device "
			              "permission and separate from the global UPnP setting: Home "
			              "Network > Network > Network Connections, edit this device, and "
			              "tick \"Allow independent port sharing for this device\". Other "
			              "routers word it similarly. If the router is not yours to "
			              "configure, have everyone join a mesh VPN such as Tailscale or "
			              "ZeroTier instead.";
		}
		else
		{
			r.mapStatus = "Neither UPnP nor NAT-PMP got a port forward from your "
			              "router — both are often switched off by default.";
			r.advice    = remedyFor(port16);
		}

		// The pinhole is the other half of reachability: the IPv4 forward above
		// serves v4-only guests, this serves the v6 address the directory also
		// advertises. Done before registration so the reachability probe judges
		// the finished state, not a half-open one.
		const std::string pinholeV6 = HE::Net::socketGlobalIPv6Address();
		if (!pinholeV6.empty())
		{
			// Reuse the gateway UPnP found for the mapping (if it did) — saves a
			// second SSDP search on the common path.
			const HE::Net::PortMapResult pr = HE::Net::PortMapper::openPinhole(
				pinholeV6, port16, r.pinhole, r.mapping.igd);
			r.pinholeOpen = pr == HE::Net::PortMapResult::Ok;
			if (r.pinholeOpen && r.portMapped)
				r.mapStatus += " The IPv6 firewall was opened as well.";
			else if (r.pinholeOpen)
				// The v4 ladder failed but the pinhole did not — reachability is
				// partial, not absent, and the difference decides whether anyone
				// can join at all.
				r.mapStatus += " The IPv6 firewall was opened though, so guests "
				               "whose networks have IPv6 can still reach you "
				               "directly.";
		}

		if (!HE::Net::httpsAvailable())
		{
			r.error = "No HTTPS support in this build — share the address manually.";
			return r;
		}
		SessionDirectory dir(endpoint);
		SessionRegistration reg;
		// Offer this machine's global IPv6 alongside whatever address the
		// registration itself arrives from. The HTTPS call uses exactly one
		// family, so without this a dual-stack host is published under one
		// address only — and a guest on the other family cannot connect at all,
		// with nothing in the UI to explain it. The server verifies the claim by
		// connecting back before publishing it.
		// Every address this host might be reachable under, for the directory
		// to verify. The stable IPv6 first — the pinhole above was opened for
		// it, while the register request itself leaves from the TEMPORARY
		// privacy address, so without this claim the one v6 address that
		// actually works would never be probed. Then the router's WAN IPv4
		// from the mapping, because the request usually travels over IPv6 and
		// the v4 forward is otherwise invisible to the directory too.
		std::vector<std::string> altAddresses;
		if (!pinholeV6.empty()) altAddresses.push_back(pinholeV6);
		if (r.portMapped && !mapping.externalIp.empty() &&
		    !HE::Net::PortMapper::isPrivateOrCgnat(mapping.externalIp))
		{
			altAddresses.push_back(mapping.externalIp);
		}
		const DirectoryStatus st = dir.registerSession(sid, port16, displayName,
		                                               "HorizonEngine", 1, reg,
		                                               altAddresses);
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
			r.host  = look.host;
			r.hosts = look.hosts;
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

	m_collab->onComponents([this](HE::Net::ParticipantId,
	                              const HE::Net::CollabSession::ComponentUpdate& u) {
		const std::uint32_t local = entityForNetId(u.netId);
		if (local != 0 && m_onRemoteComponents) m_onRemoteComponents(local, u.blob);
	});

	m_collab->onStructural([this](HE::Net::ParticipantId,
	                              const HE::Net::CollabSession::StructuralChange& c) {
		using Kind = HE::Net::CollabSession::StructuralChange::Kind;
		switch (c.kind)
		{
		case Kind::Created:
		{
			if (!m_onRemoteCreate) return;
			// Already known? A duplicate Create would spawn a second copy.
			if (entityForNetId(c.netId) != 0) return;
			const std::uint32_t parent = c.parentNet ? entityForNetId(c.parentNet) : 0;
			const std::uint32_t created = m_onRemoteCreate(parent, c.blob);
			// instantiatePrefab mints a FRESH local handle — recording it here is
			// exactly what makes handle-stable instantiation unnecessary.
			if (created != 0) m_netIds.emplace_back(c.netId, created);
			break;
		}
		case Kind::Destroyed:
		{
			const std::uint32_t local = entityForNetId(c.netId);
			if (local != 0 && m_onRemoteDestroy) m_onRemoteDestroy(local);
			forgetNetId(c.netId);
			break;
		}
		case Kind::Reparented:
		{
			const std::uint32_t local  = entityForNetId(c.netId);
			const std::uint32_t parent = c.parentNet ? entityForNetId(c.parentNet) : 0;
			if (local != 0 && m_onRemoteReparent) m_onRemoteReparent(local, parent);
			break;
		}
		}
	});

	m_collab->onAssetUpdated([this](HE::Net::ParticipantId,
	                                const HE::Net::CollabSession::AssetUpdate& a) {
		if (!m_onRemoteAsset) return;
		// Guard the write: applying it hits ContentManager::saveAsset, whose
		// notification would otherwise publish the same bytes straight back.
		m_applyingRemoteAsset = true;
		m_onRemoteAsset(a.path, a.bytes);
		m_applyingRemoteAsset = false;
	});

	m_collab->onDocDeltas([this](HE::Net::ParticipantId, std::uint64_t,
	                             const std::string& path,
	                             const std::vector<HE::Net::CollabSession::DocDelta>& batch) {
		// Straight through to the editor, which routes by path to whichever panel
		// holds that asset. Deliberately NOT written to disk: the point of a delta
		// is that the receiving panel patches the document it is already showing,
		// keeping its canvas, selection and undo history — the whole-file path is
		// what touches files, and it still runs underneath for persistence.
		if (m_onRemoteDocDeltas) m_onRemoteDocDeltas(path, batch);
	});

	m_collab->onLockQueryResult([this](std::uint64_t subject, bool held,
	                                   HE::Net::ParticipantId owner,
	                                   const std::string& ownerName) {
		const auto pit = m_assetSubjectPaths.find(subject);
		if (pit == m_assetSubjectPaths.end()) return;   // not an asset we asked about
		AssetAnswer& a = m_assetAnswers[pit->second];
		a.pending = false;
		// Our own lock is not "held by someone else" — the state accessor reads
		// ownsLock first anyway, but leaving this true would flip a tab we own to
		// read-only for a frame if the table lagged.
		a.held      = held && owner != m_collab->localId();
		a.owner     = owner;
		a.ownerName = ownerName;
	});

	m_collab->onTransform([this](HE::Net::ParticipantId,
	                             const HE::Net::CollabSession::TransformDelta& d) {
		// Subjects are uuid-derived, never raw handles — resolve here so the
		// editor callback keeps receiving something it can hand to the registry.
		const std::uint32_t local = entityForNetId(d.subject);
		if (local != 0 && m_onRemoteTransform)
			m_onRemoteTransform(local, d.position, d.rotation, d.scale);
	});

	m_collab->onLockChanged([this](const LockInfo& l, bool acquired) {
		// If we lost the lock we were holding (the holder left, or we released
		// it), forget it so the next selection re-requests cleanly.
		if (!acquired && l.subject == m_heldSubject && l.owner == m_collab->localId())
			m_heldSubject = 0;
	});

	m_collab->onLockDenied([this](std::uint64_t subject, LockDenyReason reason) {
		// Refused means someone else is already editing it. Say who, so the user
		// knows to ask rather than wonder why nothing responds.
		if (reason == LockDenyReason::HeldByOther)
		{
			const LockInfo* l = m_collab->lockFor(subject);
			m_lockNotice = l && !l->ownerName.empty()
				? (l->ownerName + " is editing this — it is read-only for you.")
				: std::string("Someone else is editing this — it is read-only for you.");
		}
		// We do not hold it, so do not pretend we do.
		if (subject == m_heldSubject) m_heldSubject = 0;

		// An ASSET lock we asked for optimistically: the race-window edits are
		// not ours to keep — hand the path to the editor so it reloads the tab
		// from disk (the last agreed state) instead of sitting on a fork.
		for (auto it = m_heldAssetLocks.begin(); it != m_heldAssetLocks.end(); ++it)
		{
			if (assetSubject(*it) != subject) continue;
			const std::string path = *it;
			m_heldAssetLocks.erase(it);
			if (m_onAssetLockDenied) m_onAssetLockDenied(path);
			break;
		}
	});
}

void CollabController::leave()
{
	// Take the forward back down. Leaving it in place would hold a hole open in
	// the user's firewall long after the session ended — the "clear it again"
	// half that makes automatic forwarding acceptable in the first place.
	if (m_portMapped)
	{
		std::thread([handle = m_mapping] {
			// Torn down the same way it was put up — the handle remembers which
			// protocol won.
			HE::Net::PortMapper::unmapPort(handle);
		}).detach();
	}
	if (m_pinholeOpen)
	{
		std::thread([handle = m_pinhole] {
			HE::Net::PortMapper::closePinhole(handle);
		}).detach();
	}

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
	m_publicAddress.clear();
	m_port          = 0;
	m_snapshotGot   = 0;
	m_snapshotTotal = 0;
	m_heldSubject   = 0;
	m_lockNotice.clear();
	m_netIds.clear();
	m_lastComponentHashes.clear();
	m_heldAssetLocks.clear();
	// Leaving these behind would keep every open tab read-only after the session
	// ends: assetEditState short-circuits to Editable without a session, but a
	// stale "held by X" would resurface the moment a new one started.
	m_assetAnswers.clear();
	m_assetSubjectPaths.clear();
	m_portMapped    = false;
	m_portMapStatus.clear();
	m_advice.clear();
	m_mapping = {};
	m_pinhole = {};
	m_pinholeOpen = false;

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

		// Port-mapping outcome arrives with the registration, whether or not the
		// directory itself succeeded.
		m_mapping       = r.mapping;
		m_portMapped    = r.portMapped;
		m_pinhole       = r.pinhole;
		m_pinholeOpen   = r.pinholeOpen;
		m_portMapStatus = r.mapStatus;
		m_advice        = r.advice;

		if (r.ok)
		{
			m_directoryToken     = r.token;
			m_reachable          = r.reachable;
			m_reachabilityKnown  = true;
			m_lastHeartbeatMs    = nowMs;
			if (!r.publicIp.empty()) m_publicAddress = r.publicIp;
			m_directoryStatus = r.reachable
				? "Session published — peers can join with the ID."
				// Not an error: the entry exists, but nobody outside this network
				// will get through it. Saying so now beats an unexplained timeout
				// on the guest's side later.
				: "Published, but this machine is not reachable from outside.";

			// The probe outranks everything the mapping step concluded, because it
			// is the only check that actually came in from the outside. It can
			// overturn the guess in both directions: a router that reported
			// success can still be unreachable, and one that refused the mapping
			// may already have a forward configured by hand.
			if (r.reachable)      m_advice.clear();
			else if (m_advice.empty()) m_advice = remedyFor(m_port);
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
		// Walk the candidates rather than trusting one. A host may be listed under
		// both families; this machine can only use the ones it has a route for,
		// and which those are is not knowable in advance.
		std::vector<std::string> candidates = r.hosts;
		if (candidates.empty() && !r.host.empty()) candidates.push_back(r.host);

		bool        linked = false;
		std::string chosen;
		for (const std::string& candidate : candidates)
		{
			m_directoryStatus = "Connecting to " + candidate + "...";
			if (beginLink(candidate, r.port, m_pendingJoinCode, m_pendingDisplayName))
			{
				chosen = candidate;
				linked = true;
				break;
			}
		}
		if (!linked)
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

// ─── Live transform deltas ───────────────────────────────────────────────────

void CollabController::publishTransform(std::uint64_t subject,
                                        const float pos[3], const float rotEuler[3],
                                        const float scale[3], std::uint64_t nowMs)
{
	if (!m_collab || !inSession() || subject == 0) return;
	// Only the holder may move something; without the lock this is refused at
	// the host anyway, so do not spend bandwidth on it.
	if (!m_collab->ownsLock(subject)) return;

	const float current[9] = { pos[0], pos[1], pos[2],
	                           rotEuler[0], rotEuler[1], rotEuler[2],
	                           scale[0], scale[1], scale[2] };

	// Same argument as presence: a gizmo drag produces a change every frame, and
	// most of them are indistinguishable. Send only real movement, and at most
	// at a fixed rate.
	bool moved = !m_hasLastTransform || subject != m_lastTransformSubject;
	if (!moved)
	{
		for (int i = 0; i < 9; ++i)
		{
			if (std::fabs(current[i] - m_lastTransform[i]) > 0.0005f) { moved = true; break; }
		}
	}
	if (!moved) return;
	if (m_hasLastTransform && subject == m_lastTransformSubject &&
	    nowMs - m_lastTransformSendMs < 33)   // ~30 Hz while dragging
	{
		return;
	}

	HE::Net::CollabSession::TransformDelta d;
	d.subject = subject;
	for (int i = 0; i < 3; ++i) d.position[i] = pos[i];
	for (int i = 0; i < 3; ++i) d.rotation[i] = rotEuler[i];
	for (int i = 0; i < 3; ++i) d.scale[i]    = scale[i];

	if (m_collab->sendTransform(d))
	{
		m_lastTransformSubject = subject;
		for (int i = 0; i < 9; ++i) m_lastTransform[i] = current[i];
		m_hasLastTransform    = true;
		m_lastTransformSendMs = nowMs;
	}
}

// ─── Component edits ─────────────────────────────────────────────────────────

void CollabController::publishComponents(std::uint32_t entityHandle,
                                         const std::vector<std::uint8_t>& blob)
{
	if (!m_collab || !inSession() || blob.empty()) return;

	// Derived from the entity's uuid, so it needs no prior announcement — a
	// snapshot entity is exactly as addressable as one created mid-session.
	const std::uint64_t netId = netIdFor(entityHandle);
	if (netId == 0) return;
	if (!m_collab->ownsLock(netId)) return;

	// FNV-1a over the blob: the whole point is to send nothing while the user is
	// merely looking at an entity rather than editing it. Keyed per entity —
	// with a single shared slot, alternating between two selected entities read
	// as a change on every switch and re-sent both every time.
	std::uint64_t hash = 1469598103934665603ull;
	for (const std::uint8_t b : blob) { hash ^= b; hash *= 1099511628211ull; }

	const auto it = m_lastComponentHashes.find(entityHandle);
	if (it != m_lastComponentHashes.end() && it->second == hash) return;

	HE::Net::CollabSession::ComponentUpdate u;
	u.netId = netId;
	u.blob  = blob;
	if (m_collab->sendComponents(u))
	{
		m_lastComponentHashes[entityHandle] = hash;
	}
}

// ─── Structural replication ──────────────────────────────────────────────────

std::uint64_t CollabController::netIdFor(std::uint32_t entityHandle)
{
	for (const auto& [net, handle] : m_netIds)
	{
		if (handle == entityHandle) return net;
	}

	// The wire identity is derived from the entity's stable UUID — the one the
	// scene format already carries — so every peer computes the SAME id from its
	// own copy of the world, with no announcement step and no assumption about
	// entt handle numbering. The previous design keyed everything on raw handles
	// ("both sides deserialized the same bytes"), which held only for a client's
	// fresh load: the HOST keeps its live registry, where deletions have left
	// recycled, version-bearing handles — and every delta for such an entity
	// silently missed. That was the "changes only partly sync" bug.
	if (!m_world) return 0;
	auto& reg = m_world->registry();
	const auto e = static_cast<entt::entity>(static_cast<entt::id_type>(entityHandle));
	if (!reg.valid(e)) return 0;

	auto* idc = reg.try_get<EntityIdComponent>(e);
	if (!idc)
	{
		// Every createEntity mints one; an entity without it came through some
		// bypass. Minting here keeps it addressable — both sides will still
		// agree, because the snapshot serializes whatever is minted before send.
		idc = &reg.emplace<EntityIdComponent>(e,
			EntityIdComponent{ HE::UUID::generate() });
	}
	// low64 of the uuid, top bit cleared: a v4 uuid's variant bits force lo's
	// top bit to 1, which would land every entity in the asset-subject space
	// (assetSubject sets the top bit precisely to keep the two apart). Zero
	// would collide with "no subject", so fold hi in as the escape hatch.
	std::uint64_t net = idc->id.lo & ~(1ull << 63);
	if (net == 0) net = (idc->id.hi | 1ull) & ~(1ull << 63);

	m_netIds.emplace_back(net, entityHandle);
	return net;
}

std::uint32_t CollabController::entityForNetId(std::uint64_t netId)
{
	for (const auto& [net, handle] : m_netIds)
	{
		if (net == netId) return handle;
	}

	// Cache miss: resolve against the world. Linear over the id pool, but only
	// on the first sighting of a subject — every later lookup hits the cache.
	if (!m_world) return 0;
	auto& reg = m_world->registry();
	std::uint32_t found = 0;
	reg.view<EntityIdComponent>().each([&](auto e, const EntityIdComponent& idc) {
		std::uint64_t net = idc.id.lo & ~(1ull << 63);
		if (net == 0) net = (idc.id.hi | 1ull) & ~(1ull << 63);
		if (net == netId) found = static_cast<std::uint32_t>(entt::to_integral(e));
	});
	if (found != 0) m_netIds.emplace_back(netId, found);
	return found;
}

void CollabController::forgetNetId(std::uint64_t netId)
{
	m_netIds.erase(std::remove_if(m_netIds.begin(), m_netIds.end(),
	                              [netId](const auto& e) { return e.first == netId; }),
	               m_netIds.end());
}

void CollabController::seedNetIds()
{
	// Warm the uuid↔handle cache for every entity in the world. Not for
	// correctness of lookups — those fall back to a registry scan — but for
	// DELETIONS: by the time a destroyed entity is diffed out of the world, its
	// uuid can no longer be read from the registry, so the cache built here is
	// the only place its wire identity survives. Runs on the client after the
	// snapshot replaced its world, and on the host when it starts hosting.
	m_netIds.clear();
	if (!m_world) return;
	m_world->registry().view<entt::entity>().each([&](auto e) {
		netIdFor(static_cast<std::uint32_t>(entt::to_integral(e)));
	});
}

bool CollabController::publishCreate(std::uint32_t entityHandle,
                                     std::uint32_t parentHandle,
                                     const std::vector<std::uint8_t>& blob)
{
	if (!m_collab || !inSession() || blob.empty()) return false;

	HE::Net::CollabSession::StructuralChange c;
	c.kind      = HE::Net::CollabSession::StructuralChange::Kind::Created;
	c.netId     = netIdFor(entityHandle);
	c.parentNet = parentHandle ? netIdFor(parentHandle) : 0;
	c.blob      = blob;
	return m_collab->sendStructural(c);
}

bool CollabController::publishDestroy(std::uint32_t entityHandle)
{
	if (!m_collab || !inSession()) return false;

	// Look the id up WITHOUT minting one: an entity nobody ever knew about does
	// not need a deletion broadcast.
	std::uint64_t netId = 0;
	for (const auto& [net, handle] : m_netIds)
	{
		if (handle == entityHandle) { netId = net; break; }
	}
	if (netId == 0) return false;

	HE::Net::CollabSession::StructuralChange c;
	c.kind  = HE::Net::CollabSession::StructuralChange::Kind::Destroyed;
	c.netId = netId;
	const bool ok = m_collab->sendStructural(c);
	if (ok) forgetNetId(netId);
	return ok;
}

bool CollabController::publishReparent(std::uint32_t entityHandle,
                                       std::uint32_t newParentHandle)
{
	if (!m_collab || !inSession()) return false;

	HE::Net::CollabSession::StructuralChange c;
	c.kind      = HE::Net::CollabSession::StructuralChange::Kind::Reparented;
	c.netId     = netIdFor(entityHandle);
	c.parentNet = newParentHandle ? netIdFor(newParentHandle) : 0;
	return m_collab->sendStructural(c);
}

// ─── Authored-asset sync ─────────────────────────────────────────────────────

std::uint64_t CollabController::assetSubject(const std::string& relativePath)
{
	// FNV-1a over the normalised path. Entity subjects are small integers
	// (handles), so setting the top bit keeps the two spaces from ever colliding
	// in the shared lock table.
	std::uint64_t hash = 1469598103934665603ull;
	for (char c : relativePath)
	{
		if (c == '\\') c = '/';   // same asset, either separator
		hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
		hash *= 1099511628211ull;
	}
	return hash | (1ull << 63);
}

// ─── Asset-level locking ─────────────────────────────────────────────────────

bool CollabController::assetLockedByOther(const std::string& relativePath)
{
	if (!m_collab || !inSession()) return false;
	const HE::Net::LockInfo* l = m_collab->lockFor(assetSubject(relativePath));
	return l && l->owner != m_collab->localId();
}

bool CollabController::ownsAssetLock(const std::string& relativePath)
{
	if (!m_collab || !inSession()) return false;
	return m_collab->ownsLock(assetSubject(relativePath));
}

void CollabController::requestAssetLock(const std::string& relativePath)
{
	if (!m_collab || !inSession()) return;
	const std::uint64_t subject = assetSubject(relativePath);
	if (m_collab->ownsLock(subject)) return;
	if (const HE::Net::LockInfo* l = m_collab->lockFor(subject);
	    l && l->owner != m_collab->localId())
		return;   // visibly taken — no point asking
	m_collab->requestLock(subject);
	if (std::find(m_heldAssetLocks.begin(), m_heldAssetLocks.end(), relativePath)
	    == m_heldAssetLocks.end())
	{
		m_heldAssetLocks.push_back(relativePath);
	}
}

void CollabController::releaseAssetLock(const std::string& relativePath)
{
	if (m_collab && inSession())
	{
		const std::uint64_t subject = assetSubject(relativePath);
		if (m_collab->ownsLock(subject)) m_collab->releaseLock(subject);
	}
	m_heldAssetLocks.erase(
		std::remove(m_heldAssetLocks.begin(), m_heldAssetLocks.end(), relativePath),
		m_heldAssetLocks.end());
}

const HE::Net::LockInfo* CollabController::assetLockInfo(const std::string& relativePath)
{
	if (!m_collab || !inSession()) return nullptr;
	return m_collab->lockFor(assetSubject(relativePath));
}

// ─── Host-confirmed edit state ───────────────────────────────────────────────

void CollabController::beginAssetEditSession(const std::string& relativePath)
{
	if (!m_collab || !inSession() || relativePath.empty()) return;
	if (m_assetAnswers.count(relativePath)) return;   // already asked

	const std::uint64_t subject = assetSubject(relativePath);
	m_assetSubjectPaths[subject] = relativePath;
	m_assetAnswers[relativePath] = AssetAnswer{};     // pending
	if (!m_collab->queryLock(subject))
	{
		// Nobody to ask (the link went down between the checks) — do not leave the
		// tab read-only forever waiting for an answer that cannot come.
		m_assetAnswers.erase(relativePath);
	}
}

CollabController::AssetEditState
CollabController::assetEditState(const std::string& relativePath)
{
	// Outside a session every asset is simply editable — this whole mechanism is
	// about arbitrating between peers, and there are none.
	if (!m_collab || !inSession()) return AssetEditState::Editable;

	const std::uint64_t subject = assetSubject(relativePath);
	if (m_collab->ownsLock(subject)) return AssetEditState::Owned;

	// The live table wins when it says someone else holds it: it is the most
	// recent thing we know, and it is how a peer taking the lock while our tab
	// sits open flips us to read-only.
	if (const HE::Net::LockInfo* l = m_collab->lockFor(subject);
	    l && l->owner != m_collab->localId())
		return AssetEditState::HeldByOther;

	const auto it = m_assetAnswers.find(relativePath);
	if (it == m_assetAnswers.end()) return AssetEditState::Unknown; // not asked yet
	if (it->second.pending)          return AssetEditState::Unknown;
	if (it->second.held)             return AssetEditState::HeldByOther;
	return AssetEditState::Editable;
}

bool CollabController::beginAssetEdit(const std::string& relativePath)
{
	const AssetEditState st = assetEditState(relativePath);
	if (st == AssetEditState::Owned)    return true;
	if (st == AssetEditState::Editable)
	{
		if (!m_collab || !inSession()) return true;   // solo: nothing to claim
		requestAssetLock(relativePath);
		// The grant is a round trip away. Editing is allowed to continue in the
		// meantime — the host confirmed at open time that the asset was free, so
		// this is a confirmation rather than the race it used to be, and the deny
		// path is now the rare case it was meant to be.
		return true;
	}
	return false;   // Unknown (still asking) or held by someone else
}

void CollabController::forgetAssetEditSession(const std::string& relativePath)
{
	m_assetAnswers.erase(relativePath);
	m_assetSubjectPaths.erase(assetSubject(relativePath));
}

bool CollabController::publishDocDeltas(
	const std::string& relativePath,
	const std::vector<HE::Net::CollabSession::DocDelta>& batch)
{
	if (!m_collab || !inSession() || batch.empty()) return false;
	return m_collab->sendDocDeltas(assetSubject(relativePath), relativePath, batch);
}

std::string CollabController::projectRelativeAssetPath(const std::string& absolutePath,
                                                       const std::string& contentRoot)
{
	if (contentRoot.empty() || absolutePath.size() <= contentRoot.size()) return {};

	// Normalise both sides to forward slashes before comparing: the tab path
	// comes from the OS file dialog / browser and mixes separators on Windows.
	auto norm = [](std::string p) {
		for (char& ch : p) if (ch == '\\') ch = '/';
		return p;
	};
	const std::string abs  = norm(absolutePath);
	const std::string root = norm(contentRoot);
	if (abs.rfind(root, 0) != 0) return {};
	std::size_t cut = root.size();
	if (cut < abs.size() && abs[cut] == '/') ++cut;
	return abs.substr(cut);
}

bool CollabController::isSyncableAsset(const std::string& relativePath)
{
	// The C++ tree travels under its own reserved key prefix (see
	// EditorApplication::collabSyncKey). Those are raw .h/.cpp text files with no
	// HAsset header at all, so they are admitted here and skip the type sniff —
	// which would read them as Unknown and drop them.
	if (relativePath.rfind("::Source::", 0) == 0) return true;

	const std::size_t dot = relativePath.find_last_of('.');
	if (dot == std::string::npos) return false;

	std::string ext = relativePath.substr(dot);
	for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

	// Authored data only. Meshes, textures and audio are excluded for one reason:
	// they are by far the largest files a project holds, and pushing them through
	// a live session would turn it into a poor file sync (see the scope boundary
	// in docs/networking-layer-design.md). They are not less important — they are
	// simply too big for this channel, which is why source control carries them
	// instead.
	//
	// EXTENSION IS NOT THE TYPE. The engine writes EVERY authored asset as a
	// `.hasset` container — materials, UI widgets, HorizonCode graphs, particle
	// and animator graphs, input assets — with the real type in the header. This
	// list used to spell out `.hmat`, `.huiw`, `.hcode`, `.hpart`, `.hasm`,
	// `.hinput`; the engine writes none of those, so it matched NOTHING in a real
	// project and asset collaboration was silently dead for every type except
	// scenes and loose scripts. (The `.hscene`/`.hescene` note below is the same
	// mistake caught once before, in one entry, and it went uncorrected in the
	// other six.)
	//
	// `.hasset` therefore passes here, and the ASSET TYPE inside decides — see
	// isSyncableAssetType, which the caller applies once it has the file.
	static const char* kAuthored[] = {
		".hasset",    // every authored asset the ContentManager writes
		".hescene",   // scenes — note the 'e': ProjectManager writes .hescene
		".lua", ".py",// scripts
	};
	for (const char* a : kAuthored)
	{
		if (ext == a) return true;
	}
	return false;
}

bool CollabController::isSyncableAssetType(HE::AssetType type)
{
	switch (type)
	{
		// Authored in an editor, small, and edited during a session — the whole
		// point of collaborating.
		case HE::AssetType::Material:
		case HE::AssetType::MaterialFunction:
		case HE::AssetType::Widget:
		case HE::AssetType::HorizonCodeClass:
		case HE::AssetType::ParticleSystem:
		case HE::AssetType::AnimatorStateMachine:
		case HE::AssetType::InputAction:
		case HE::AssetType::InputMappingContext:
		case HE::AssetType::Script:
		case HE::AssetType::Scene:
		case HE::AssetType::Prefab:
			return true;

		// Imported binaries. Excluded by size, not by importance — source control
		// carries these. Font and the animation clips are here for the same
		// reason: they are baked data, not something two people edit live.
		case HE::AssetType::StaticMesh:
		case HE::AssetType::SkeletalMesh:
		case HE::AssetType::Texture:
		case HE::AssetType::Audio:
		case HE::AssetType::Font:
		case HE::AssetType::Shader:
		case HE::AssetType::AnimationClip:
		case HE::AssetType::PropertyAnimClip:
		case HE::AssetType::Unknown:
			return false;
	}
	return false;
}

void CollabController::publishAsset(const std::string& relativePath,
                                    const std::string& fullPath)
{
	if (!m_collab || !inSession()) return;
	if (m_applyingRemoteAsset) return;      // this save WAS the remote change
	if (!isSyncableAsset(relativePath)) return;

	std::ifstream f(fullPath, std::ios::binary);
	if (!f) return;
	std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)),
	                                 std::istreambuf_iterator<char>());
	if (bytes.empty()) return;
	// `.hasset` is equally the container around an imported mesh or texture, and
	// those are exactly what must not travel this channel. This hook fires for
	// EVERY ContentManager save, including an import, so the type gate belongs
	// here and not only where the editor walks its open tabs. C++ sources are
	// exempt: they have no HAsset header to sniff.
	if (relativePath.rfind("::Source::", 0) != 0 &&
	    !isSyncableAssetType(EditorAssetTypeCache::assetTypeOf(fullPath))) return;

	const std::uint64_t subject = assetSubject(relativePath);

	// Take the lock if it is free. Without this a save would be silently dropped
	// just because the user edited the asset in a panel rather than selecting it
	// in a way that happened to take a lock.
	if (!m_collab->ownsLock(subject))
	{
		if (m_collab->lockFor(subject)) return;   // someone else owns it — refuse
		m_collab->requestLock(subject);
		if (!m_collab->ownsLock(subject)) return; // a client must await the grant
	}

	m_collab->sendAsset(subject, relativePath, bytes);
}

// ─── Locks ───────────────────────────────────────────────────────────────────

bool CollabController::requestLock(std::uint64_t subject)
{
	return m_collab ? m_collab->requestLock(subject) : false;
}

void CollabController::releaseLock(std::uint64_t subject)
{
	if (m_collab) m_collab->releaseLock(subject);
}

const HE::Net::LockInfo* CollabController::lockFor(std::uint64_t subject) const
{
	return m_collab ? m_collab->lockFor(subject) : nullptr;
}

bool CollabController::ownsLock(std::uint64_t subject) const
{
	return m_collab && m_collab->ownsLock(subject);
}

void CollabController::followSelection(std::uint64_t subject)
{
	if (!m_collab || !inSession()) return;
	if (subject == m_heldSubject) return;   // nothing changed — stay quiet

	// Give up the previous one first: holding two at once would block an entity
	// nobody is even looking at any more.
	if (m_heldSubject != 0) m_collab->releaseLock(m_heldSubject);

	m_heldSubject = subject;
	if (subject != 0) m_collab->requestLock(subject);
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
