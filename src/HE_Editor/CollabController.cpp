#include "CollabController.h"
#include "EditorAssetTypeCache.h"  // .hasset header sniff (the TYPE, not the extension)

// Profile pictures arrive as ordinary image files and leave as raw RGBA — the
// decode happens here so HorizonNet never has to parse an image.
#include "vendor/stb_image.h"

#include <Net/HttpsClient.h>
#include <Net/PortMapper.h>
#include <Net/NetSession.h>
#include <Net/SecureTransport.h>
#include <Net/SessionDirectory.h>
#include <Net/Socket.h>
#include <Net/TcpTransport.h>
#include <Platform/PathSafety.h>   // a create's path must stay in the project

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <thread>
#include <vector>

#include <HorizonScene/Components/EntityIdComponent.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSerializer.h>

#include <Diagnostics/GlobalState.h>
#include <Diagnostics/Logger.h>

#include <random>

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

// The backstop, not the plan: OnShutdown calls shutdown() while the editor is
// still a healthy process. Reaching here with a session still open means that
// did not happen (an unusual exit path, a test), and giving the resources back
// late is better than not at all — shutdown() is a no-op once they are gone.
CollabController::~CollabController() { shutdown(); }

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
	cfg.displayName  = displayName;
	cfg.projectKey   = m_projectId;
	cfg.projectLabel = m_projectLabel;
	applyLocalIdentity(cfg);
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
		r.ok         = true;
		r.publicIp   = reg.publicIp;
		r.altAddress = reg.altAddress;
		r.reachable  = reg.reachable;
		r.token      = reg.token;
		return r;
	});

	return true;
}

void CollabController::colorFor(HE::Net::ParticipantId id, float outRgb[3]) const
{
	for (const HE::Net::Participant& p : participants())
	{
		if (p.id != id || p.color.unset()) continue;
		outRgb[0] = p.color.r / 255.0f;
		outRgb[1] = p.color.g / 255.0f;
		outRgb[2] = p.color.b / 255.0f;
		return;
	}
	participantColor(id, outRgb);
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

// ─── Local identity ──────────────────────────────────────────────────────────
// Name and client key live in the editor's config.json; the picture lives beside
// it as a raw file. Deliberately NOT in the json: 64² RGBA is 16 KiB, which
// base64s to 22 KiB of noise in a file people open and read.

namespace {

constexpr const char* kCfgDisplayName = "CollabDisplayName";
constexpr const char* kCfgClientKey   = "CollabClientKey";
// Packed 0xRRGGBB. 0 is the sentinel for "no preference" (see
// HE::Net::ParticipantColor::unset), so an absent key and an unset colour are
// the same thing and there is no third state to get wrong.
constexpr const char* kCfgColor       = "CollabColor";

std::filesystem::path avatarFilePath()
{
	const std::filesystem::path dir = GlobalState::userDataDir();
	return dir.empty() ? std::filesystem::path() : dir / "collab_avatar.rgba";
}

// A random, non-secret identifier for this installation. Not a credential — it
// says "the same editor as last time", nothing more — so a plain PRNG is the
// right tool and the join code stays the thing that actually guards a session.
std::string mintClientKey()
{
	std::random_device rd;
	std::uniform_int_distribution<unsigned> hex(0, 15);
	static constexpr char kHex[] = "0123456789abcdef";
	std::string out;
	out.reserve(32);
	for (int i = 0; i < 32; ++i) out.push_back(kHex[hex(rd)]);
	return out;
}

} // namespace

// Centre-crop to a square and resample to `dst` pixels per side by averaging
// each destination pixel's source box. Box filtering (rather than picking one
// source pixel) is what keeps a 2000px photo from turning into aliased confetti
// at 64px — and a portrait is the one image in the editor a user will actually
// look at closely.
std::vector<std::uint8_t> CollabController::resampleSquareRgba(const std::uint8_t* src,
                                                               int srcW, int srcH, int dst)
{
	if (!src || srcW <= 0 || srcH <= 0 || dst <= 0) return {};

	const int side = std::min(srcW, srcH);
	const int ox   = (srcW - side) / 2;
	const int oy   = (srcH - side) / 2;

	std::vector<std::uint8_t> out(static_cast<std::size_t>(dst) * dst * 4u);
	for (int y = 0; y < dst; ++y)
	{
		// Source rows this destination row covers. The +1 floor keeps the box
		// non-empty when upscaling, where several destination pixels share one
		// source pixel.
		const int y0 = oy + (y * side) / dst;
		const int y1 = std::max(y0 + 1, oy + ((y + 1) * side) / dst);
		for (int x = 0; x < dst; ++x)
		{
			const int x0 = ox + (x * side) / dst;
			const int x1 = std::max(x0 + 1, ox + ((x + 1) * side) / dst);

			std::uint32_t acc[4] { 0, 0, 0, 0 };
			std::uint32_t n = 0;
			for (int sy = y0; sy < y1; ++sy)
			{
				const std::uint8_t* row = src + (static_cast<std::size_t>(sy) * srcW) * 4u;
				for (int sx = x0; sx < x1; ++sx)
				{
					for (int c = 0; c < 4; ++c) acc[c] += row[sx * 4 + c];
					++n;
				}
			}
			std::uint8_t* d = out.data() + (static_cast<std::size_t>(y) * dst + x) * 4u;
			for (int c = 0; c < 4; ++c)
				d[c] = static_cast<std::uint8_t>(n ? acc[c] / n : 0);
		}
	}
	return out;
}

namespace {

// The one mutable copy. localIdentity() hands it out read-only; the setters here
// are the only writers, which is what keeps "changed on disk" and "changed in
// memory" from ever disagreeing.
CollabController::Identity& identityStorage()
{
	// Loaded once. The client key is minted on first use and written back
	// immediately: a key that only existed in memory would change on every
	// restart, and a session ban keyed on it would mean nothing.
	static CollabController::Identity s_identity = [] {
		CollabController::Identity id;
		GlobalState& gs = GlobalState::getInstance();

		id.name = gs.getCustomConfigString(kCfgDisplayName, "Horizon User");
		if (id.name.empty()) id.name = "Horizon User";

		const int packed = gs.getCustomConfigInt(kCfgColor, 0);
		id.color.r = static_cast<std::uint8_t>((packed >> 16) & 0xFF);
		id.color.g = static_cast<std::uint8_t>((packed >> 8)  & 0xFF);
		id.color.b = static_cast<std::uint8_t>( packed        & 0xFF);

		id.clientKey = gs.getCustomConfigString(kCfgClientKey, "");
		if (id.clientKey.empty())
		{
			id.clientKey = mintClientKey();
			gs.setCustomConfigEntry(kCfgClientKey, id.clientKey);
			gs.writeConfig();
		}

		const std::filesystem::path path = avatarFilePath();
		std::error_code ec;
		if (!path.empty() && std::filesystem::exists(path, ec))
		{
			std::ifstream in(path, std::ios::binary);
			std::uint8_t header[2] {};
			if (in.read(reinterpret_cast<char*>(header), 2))
			{
				const auto size = static_cast<std::uint16_t>(header[0] | (header[1] << 8));
				const std::size_t bytes = static_cast<std::size_t>(size) * size * 4u;
				// Sized from the file's own claim, so it is bounded before the
				// read: a truncated or hand-edited file must not turn into a
				// gigabyte allocation at editor startup.
				if (size > 0 && size <= CollabController::kAvatarSize)
				{
					std::vector<std::uint8_t> px(bytes);
					if (in.read(reinterpret_cast<char*>(px.data()),
					            static_cast<std::streamsize>(bytes)))
					{
						id.avatarSize = size;
						id.avatarRgba = std::move(px);
					}
				}
			}
			if (id.avatarSize == 0)
			{
				Logger::Log(Logger::LogLevel::Warning,
				            "Collab: the stored profile picture could not be read — "
				            "pick one again in the Collaboration window.");
			}
		}
		return id;
	}();
	return s_identity;
}

} // namespace

const CollabController::Identity& CollabController::localIdentity()
{
	return identityStorage();
}

void CollabController::setLocalName(const std::string& name)
{
	Identity& id = identityStorage();
	id.name = name.empty() ? std::string("Horizon User") : name;
	GlobalState& gs = GlobalState::getInstance();
	gs.setCustomConfigEntry(kCfgDisplayName, id.name);
	gs.writeConfig();
}

void CollabController::setLocalColor(HE::Net::ParticipantColor color)
{
	Identity& id = identityStorage();
	id.color     = color;

	GlobalState& gs = GlobalState::getInstance();
	gs.setCustomConfigEntry(kCfgColor, (static_cast<int>(color.r) << 16) |
	                                   (static_cast<int>(color.g) << 8)  |
	                                    static_cast<int>(color.b));
	gs.writeConfig();
}

bool CollabController::setLocalAvatarFromFile(const std::string& imagePath,
                                              std::string& error)
{
	error.clear();

	int w = 0, h = 0, channels = 0;
	unsigned char* pixels = stbi_load(imagePath.c_str(), &w, &h, &channels, 4);
	if (!pixels || w <= 0 || h <= 0)
	{
		const char* why = stbi_failure_reason();
		error = std::string("That file could not be read as an image") +
		        (why ? std::string(" (") + why + ")." : ".");
		if (pixels) stbi_image_free(pixels);
		return false;
	}

	std::vector<std::uint8_t> square = resampleSquareRgba(pixels, w, h, kAvatarSize);
	stbi_image_free(pixels);

	const std::filesystem::path path = avatarFilePath();
	if (path.empty())
	{
		error = "There is no writable settings directory to store the picture in.";
		return false;
	}

	// Written before the in-memory copy is swapped in: a picture the editor
	// cannot persist would come back as the old one on restart, which reads as
	// the change having silently failed a week later.
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		const std::uint8_t header[2] {
			static_cast<std::uint8_t>(kAvatarSize & 0xFF),
			static_cast<std::uint8_t>((kAvatarSize >> 8) & 0xFF)
		};
		out.write(reinterpret_cast<const char*>(header), 2);
		out.write(reinterpret_cast<const char*>(square.data()),
		          static_cast<std::streamsize>(square.size()));
		if (!out)
		{
			error = "The picture could not be saved to " + path.string() + ".";
			return false;
		}
	}

	Identity& id  = identityStorage();
	id.avatarSize = kAvatarSize;
	id.avatarRgba = std::move(square);
	Logger::Log(Logger::LogLevel::Info,
	            ("Collab: profile picture set from " + imagePath).c_str());
	return true;
}

void CollabController::clearLocalAvatar()
{
	Identity& id  = identityStorage();
	id.avatarSize = 0;
	id.avatarRgba.clear();
	std::error_code ec;
	if (const std::filesystem::path path = avatarFilePath(); !path.empty())
		std::filesystem::remove(path, ec);
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
	// Armed here rather than at the connect: the lookup can hang too, and from
	// the user's side "nothing is happening" is the same either way.
	m_connectDeadlineMs  = 0;   // set on the first update(), which knows the clock
	m_connectTarget.clear();

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
	cfg.displayName  = displayName;
	cfg.projectKey   = m_projectId;
	cfg.projectLabel = m_projectLabel;
	applyLocalIdentity(cfg);
	m_collab = std::make_unique<CollabSession>(m_net.get(), NetRole::Client, cfg);
	m_collab->setStateProvider(m_provider.get());
	wireCallbacks();

	m_joinCode      = joinCode;
	m_localAddress  = host;
	m_port          = port;
	m_isHost        = false;
	m_status        = Status::Connecting;
	m_connectTarget = host;
	// The project key goes in the line: it is what the host compares the join
	// against, and a refusal is otherwise only diagnosable from the HOST's log.
	Logger::Log(Logger::LogLevel::Info,
	            ("Collab: connecting to " + host + ":" + std::to_string(port) +
	             " as project \"" + (m_projectLabel.empty() ? std::string("(none)") : m_projectLabel) +
	             "\" (id " + (m_projectId.empty() ? std::string("<none — no project open>") : m_projectId) +
	             ")").c_str());
	return true;
}

// The picture and the install key ride along with every session this editor
// opens or joins. Read HERE rather than stashed at construction, so a picture
// picked a minute ago is the one that travels.
void CollabController::applyLocalIdentity(HE::Net::CollabSession::Config& cfg)
{
	const Identity& me   = localIdentity();
	cfg.clientKey        = me.clientKey;
	cfg.avatar.size      = me.avatarSize;
	cfg.avatar.rgba      = me.avatarRgba;
	cfg.preferredColor   = me.color;
}

void CollabController::wireCallbacks()
{
	m_collab->onRemoved([this](HE::Net::RemovalReason why) {
		// The link drops right after this. Without the notice the user sees a
		// bare "connection lost" and has no way to tell being thrown out from
		// the host's wifi giving up.
		m_removalNotice = why == HE::Net::RemovalReason::Banned
			? std::string("The host removed you from this session and blocked you from "
			              "rejoining it. A new session is not affected.")
			: std::string("The host removed you from this session. You can join again if "
			              "they let you back in.");
		m_error  = m_removalNotice;
		m_status = Status::Failed;
	});

	m_collab->onJoined([this](HE::Net::ParticipantId id) {
		m_status = Status::Joined;
		Logger::Log(Logger::LogLevel::Info,
		            ("Collab: joined as participant " + std::to_string(id)).c_str());
	});

	m_collab->onJoinRejected([this](JoinRejectReason reason, const std::string& detail) {
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
		case JoinRejectReason::ProjectMismatch:
			// Say what to DO, not just what went wrong: the fix is one action,
			// and without naming the project the user cannot take it.
			m_error = detail.empty()
				? std::string("This session is editing a different project. Open the same "
				              "project as the host, then join again.")
				: ("This session is editing the project \"" + detail +
				   "\". You have \"" + (m_projectLabel.empty() ? std::string("(no project)")
				                                              : m_projectLabel) +
				   "\" open — open \"" + detail + "\" and join again.");
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

	m_collab->onAssetUpdated([this](HE::Net::ParticipantId who,
	                                const HE::Net::CollabSession::AssetUpdate& a) {
		if (!m_onRemoteAsset) return;
		// Guard the write: applying it hits ContentManager::saveAsset, whose
		// notification would otherwise publish the same bytes straight back.
		m_applyingRemoteAsset = true;
		m_onRemoteAsset(a.path, a.bytes);
		m_applyingRemoteAsset = false;
		// A create is news; a save is not. Collected rather than announced one
		// by one — Save All is one action and would otherwise be a burst.
		if (a.intent == HE::Net::CollabSession::AssetIntent::Create)
			noteAssetCreated(who, a.path);
	});

	// ── Host: is this create allowed? ──
	// Installed unconditionally: a client never gets asked, and a host that
	// later becomes one would otherwise have no policy at all.
	m_collab->setCreatePolicy(
		[this](const HE::Net::CollabSession::AssetUpdate& a,
		       HE::Net::CollabSession::AssetRejectReason& reason,
		       std::string& suggested) {
			return acceptRemoteCreate(a, reason, suggested);
		});

	m_collab->onAssetCreateResult(
		[this](const std::string& path, bool accepted,
		       HE::Net::CollabSession::AssetRejectReason reason,
		       const std::string& suggested) {
			onOwnCreateAnswered(path, accepted, reason, suggested);
		});

	// ── Host: somebody wants an asset deleted or renamed ──
	m_collab->onAssetOpRequested(
		[this](HE::Net::ParticipantId who, std::uint32_t requestId,
		       HE::Net::CollabSession::AssetOp op,
		       const std::string& path, const std::string& newPath) {
			// Folded into the existing row when the same asset is already
			// waiting: three people wanting one file gone is one decision.
			for (PendingAssetOp& p : m_pendingOps)
			{
				if (p.op == op && p.path == path && p.newPath == newPath)
				{
					for (const auto& rq : p.requesters)
						if (rq.id == who) return;         // asked twice — once is enough
					p.requesters.push_back({ who, requestId });
					return;
				}
			}
			PendingAssetOp p;
			p.op      = op;
			p.path    = path;
			p.newPath = newPath;
			p.requesters.push_back({ who, requestId });
			p.firstAskedMs = m_lastUpdateMs;
			m_pendingOps.push_back(std::move(p));
		});

	m_collab->onAssetOpVerdict([this](std::uint32_t requestId, bool approved) {
		for (std::size_t i = 0; i < m_ourPendingOps.size(); ++i)
		{
			if (m_ourPendingOps[i].requestId != requestId) continue;
			const std::string name =
				std::filesystem::path(m_ourPendingOps[i].path).filename().string();
			// Only the refusal needs saying. An approval is followed immediately
			// by the change itself, which is its own confirmation.
			if (!approved)
				m_assetNotice = "The host did not approve that \"" + name + "\" be " +
					(m_ourPendingOps[i].op == HE::Net::CollabSession::AssetOp::Delete
						? "deleted." : "renamed.");
			m_ourPendingOps.erase(m_ourPendingOps.begin() +
			                      static_cast<std::ptrdiff_t>(i));
			return;
		}
	});

	m_collab->onAssetOpApply(
		[this](HE::Net::ParticipantId, HE::Net::CollabSession::AssetOp op,
		       const std::string& path, const std::string& newPath) {
			if (!m_onRemoteAssetOp) return;
			// Same guard as an arriving save: applying this touches the content
			// tree, and the notification would otherwise come straight back.
			m_applyingRemoteAsset = true;
			m_onRemoteAssetOp(op == HE::Net::CollabSession::AssetOp::Delete, path, newPath);
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

void CollabController::releaseNetworkResources(bool blocking)
{
	// Take the forward back down. Leaving it in place would hold a hole open in
	// the user's firewall long after the session ended — the "clear it again"
	// half that makes automatic forwarding acceptable in the first place.
	//
	// Every call below carries its own timeout, which is what makes waiting for
	// them a bounded thing to do at exit rather than a hang.
	std::vector<std::thread> pending;
	auto run = [&pending, blocking](std::function<void()> work) {
		if (blocking) { work(); return; }
		pending.emplace_back(std::move(work));
	};

	if (m_portMapped)
	{
		// Torn down the same way it was put up — the handle remembers which
		// protocol won.
		run([handle = m_mapping] { HE::Net::PortMapper::unmapPort(handle); });
	}
	if (m_pinholeOpen)
	{
		run([handle = m_pinhole] { HE::Net::PortMapper::closePinhole(handle); });
	}

	// Remove the directory entry so peers stop being handed a dead endpoint. If
	// it fails the entry simply expires on its TTL instead.
	if (m_isHost && !m_directoryToken.empty() && !m_sessionId.empty())
	{
		run([endpoint = directoryEndpoint(), sid = m_sessionId,
		     token = m_directoryToken] {
			SessionDirectory dir(endpoint);
			dir.unregisterSession(sid, token);
		});
	}

	// Detached only in the interactive case: closing a session must feel instant,
	// and the editor stays alive to see the work through.
	for (std::thread& t : pending) t.detach();
}

void CollabController::leave()
{
	releaseNetworkResources(/*blocking=*/false);
	teardown();
	m_status = Status::Idle;
	m_error.clear();
}

void CollabController::shutdown()
{
	// Nothing to give back if no session ever opened — and no reason to make
	// quitting the editor wait on a network call in that case.
	if (!m_portMapped && !m_pinholeOpen && m_directoryToken.empty())
	{
		teardown();
		m_status = Status::Idle;
		return;
	}
	HE_LOG_INFO(Editor, "%s", "Collab: releasing the port forward and directory entry");
	releaseNetworkResources(/*blocking=*/true);
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
	// Starting or leaving a session is the one moment the last one's ejection
	// notice stops being news.
	m_removalNotice.clear();
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
	// Nothing here outlives the session it describes: the author of a create is
	// gone, and a refusal we never surfaced belongs to a conversation that ended.
	m_createdNotices.clear();
	// Open requests die with the session and count as refused. There is nobody
	// to hand them to — clients do not know each other, so no one can take over
	// as host — and nothing was applied, so nothing needs undoing. A requester
	// whose editor is still up learns it from the disconnect.
	if (!m_pendingOps.empty())
	{
		Logger::Log(Logger::LogLevel::Info,
			("Collab: the session ended with " + std::to_string(m_pendingOps.size()) +
			 " unanswered request(s) — all treated as refused").c_str());
		m_pendingOps.clear();
	}
	if (!m_ourPendingOps.empty())
	{
		m_assetNotice = "The session ended before the host answered your request.";
		m_ourPendingOps.clear();
	}
	else m_assetNotice.clear();
	m_pendingCreateFull.clear();
	m_createRetried = false;
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
	m_lastUpdateMs = nowMs;

	// Before the early-out: while a session id is being looked up there is no
	// transport yet, and that is exactly when the lookup result has to be
	// collected.
	pumpDirectory(nowMs);

	// ── Joining has to be able to fail ──
	// The socket is non-blocking, so a SYN that nothing answers — no route to
	// the host's address family, a firewall that drops instead of refusing —
	// never produces an event. Every other exit from Connecting (onJoined,
	// onJoinRejected, onRemoved) needs a link that came up, so without this the
	// UI sits on "Connecting…" for good. Checked before the transport early-out
	// because a lookup that never returns leaves no transport at all.
	if (m_status == Status::Connecting)
	{
		if (m_connectDeadlineMs == 0)
			m_connectDeadlineMs = nowMs + kConnectTimeoutMs;
		else if (nowMs > m_connectDeadlineMs)
		{
			// Name the address. It is the one thing that tells the two causes
			// apart — an IPv6 address here and a guest without IPv6 is a
			// different problem from an address that simply does not answer.
			m_error = m_connectTarget.empty()
				? std::string("The session could not be looked up in time. Check the "
				              "session ID, and that you are online.")
				: ("No answer from " + m_connectTarget + ":" + std::to_string(m_port) +
				   " after 20 seconds. The host is published but nothing there accepted "
				   "the connection — if that address is IPv6, your network may not have "
				   "a route to it; otherwise the host's port is not actually forwarded.");
			m_directoryStatus.clear();
			m_status            = Status::Failed;
			m_connectDeadlineMs = 0;
			Logger::Log(Logger::LogLevel::Warning, ("Collab: " + m_error).c_str());
			return;
		}
	}
	else m_connectDeadlineMs = 0;

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
			// The verified alt address first: that is the one the directory hands
			// to guests, so it is the one that has to be on screen. publicIp is
			// only where the registration came FROM — on a Mac that is the
			// temporary privacy address, which no pinhole was opened for.
			if (!r.altAddress.empty())    m_publicAddress = r.altAddress;
			else if (!r.publicIp.empty()) m_publicAddress = r.publicIp;
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

const std::vector<HE::Net::Participant>& CollabController::participants() const
{
	// A stable empty roster rather than a temporary, so callers can hold the
	// reference for the frame whether or not a session is running.
	static const std::vector<HE::Net::Participant> kNone;
	return m_collab ? m_collab->participants() : kNone;
}

// ─── Moderation ──────────────────────────────────────────────────────────────

bool CollabController::kickParticipant(HE::Net::ParticipantId id)
{
	return m_collab && m_collab->kickParticipant(id);
}

bool CollabController::banParticipant(HE::Net::ParticipantId id)
{
	return m_collab && m_collab->banParticipant(id);
}

const std::vector<HE::Net::CollabSession::BanEntry>& CollabController::bans() const
{
	static const std::vector<HE::Net::CollabSession::BanEntry> kNone;
	return m_collab ? m_collab->bans() : kNone;
}

bool CollabController::unban(const HE::Net::CollabSession::BanEntry& entry)
{
	return m_collab && m_collab->unban(entry.clientKey, entry.name);
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
	// The answer lives beside the enum (Types/Enums.h) so this layer and the
	// wire cannot drift apart. This copy used to BE the answer, and drifted:
	// three type kinds were added to AssetType afterwards, fell through to
	// false, and never replicated.
	return HE::isCollabSyncableAssetType(type);
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

bool CollabController::requestOrPerformAssetOp(
	HE::Net::CollabSession::AssetOp op,
	const std::string& relPath, const std::string& newRelPath)
{
	if (!m_collab || !inSession() || relPath.empty()) return false;

	// The host does it. It already answered the question a request exists to
	// ask — its own confirmation dialog — and asking itself again would be a
	// second dialog for one decision.
	if (isHost())
	{
		m_collab->broadcastAssetOpApply(op, relPath, newRelPath, localParticipant());
		return true;
	}

	const std::uint32_t id = m_collab->requestAssetOp(op, relPath, newRelPath);
	if (id == 0) return false;
	m_ourPendingOps.push_back({ id, op, relPath });
	m_assetNotice = (op == HE::Net::CollabSession::AssetOp::Delete
		? std::string("Asked the host to delete \"")
		: std::string("Asked the host to rename \"")) +
		std::filesystem::path(relPath).filename().string() + "\".";
	return true;
}

void CollabController::approveAssetOp(std::size_t index)
{
	if (!m_collab || !isHost() || index >= m_pendingOps.size()) return;
	const PendingAssetOp op = m_pendingOps[index];
	m_pendingOps.erase(m_pendingOps.begin() + static_cast<std::ptrdiff_t>(index));

	// Everyone who asked hears yes — including the ones who asked second, whose
	// request is answered by this same decision.
	for (const auto& rq : op.requesters)
		m_collab->sendAssetOpVerdict(rq.id, rq.requestId, true);
	// Applied through the broadcast, which also fires locally: one path for
	// "this happened", rather than a local branch that has to stay in step.
	m_collab->broadcastAssetOpApply(op.op, op.path, op.newPath,
	                                op.requesters.empty() ? localParticipant()
	                                                      : op.requesters.front().id);
}

void CollabController::denyAssetOp(std::size_t index)
{
	if (!m_collab || !isHost() || index >= m_pendingOps.size()) return;
	const PendingAssetOp op = m_pendingOps[index];
	m_pendingOps.erase(m_pendingOps.begin() + static_cast<std::ptrdiff_t>(index));
	// Told, not silently dropped: a request that simply vanishes reads as a bug,
	// and the requester would ask again.
	for (const auto& rq : op.requesters)
		m_collab->sendAssetOpVerdict(rq.id, rq.requestId, false);
}

bool CollabController::acceptRemoteCreate(
	const HE::Net::CollabSession::AssetUpdate& a,
	HE::Net::CollabSession::AssetRejectReason& reason,
	std::string& suggestedPath)
{
	using Reason = HE::Net::CollabSession::AssetRejectReason;

	// 1. Does it stay in the project? Checked first because it is the only one
	//    whose failure would already have done damage by the time we noticed.
	if (a.path.empty() || !HE::isRelativePathContained(a.path))
	{
		reason = Reason::BadPath;
		return false;
	}
	// 2. Is this a kind of asset that travels at all? The sender checked too;
	//    the host checks again because the sender is not the authority.
	if (!isSyncableAsset(a.path))
	{
		reason = Reason::NotSyncable;
		return false;
	}
	// 3. Is the name free HERE? The host's disk is the arbiter — that is the
	//    whole reason creates are arbitrated rather than simply relayed.
	if (!m_localPathForKey)
	{
		// No way to resolve the path means no way to check, and accepting on a
		// question we did not ask would be the wrong kind of optimism.
		reason = Reason::NotPermitted;
		return false;
	}
	const std::string full = m_localPathForKey(a.path);
	if (full.empty())
	{
		reason = Reason::BadPath;
		return false;
	}
	std::error_code ec;
	if (std::filesystem::exists(full, ec))
	{
		reason = Reason::NameTaken;
		// Offer the next free name rather than only refusing: two people
		// creating a material at the same moment both get one, instead of the
		// slower of them losing theirs.
		const std::filesystem::path p(a.path);
		const std::string stem = p.stem().string();
		const std::string ext  = p.extension().string();
		const std::filesystem::path dir = p.parent_path();
		for (int n = 2; n < 1000; ++n)
		{
			const std::filesystem::path cand = dir / (stem + std::to_string(n) + ext);
			const std::string candFull = m_localPathForKey(cand.generic_string());
			if (!candFull.empty() && !std::filesystem::exists(candFull, ec))
			{
				suggestedPath = cand.generic_string();
				break;
			}
		}
		return false;
	}
	return true;
}

void CollabController::onOwnCreateAnswered(
	const std::string& path, bool accepted,
	HE::Net::CollabSession::AssetRejectReason reason,
	const std::string& suggestedPath)
{
	using Reason = HE::Net::CollabSession::AssetRejectReason;
	if (accepted)
	{
		m_pendingCreateFull.clear();
		m_createRetried = false;
		return;
	}

	// The asset exists on this machine either way — the file was written before
	// it was ever announced. What differs is whether anyone else will see it.
	if (reason == Reason::NameTaken && !suggestedPath.empty() &&
	    !m_createRetried && !m_pendingCreateFull.empty() && m_localPathForKey)
	{
		const std::string newFull = m_localPathForKey(suggestedPath);
		std::error_code ec;
		if (!newFull.empty() && !std::filesystem::exists(newFull, ec))
		{
			std::filesystem::rename(m_pendingCreateFull, newFull, ec);
			if (!ec)
			{
				m_createRetried = true;
				Logger::Log(Logger::LogLevel::Info,
					("Collab: that name was taken — the asset is now \"" +
					 suggestedPath + "\"").c_str());
				m_assetNotice = "That name was already taken. Your asset is now \"" +
				                suggestedPath + "\".";
				publishAssetCreate(suggestedPath, newFull);
				return;
			}
		}
	}

	const char* why =
		reason == Reason::NameTaken   ? "the name is already taken" :
		reason == Reason::NotSyncable ? "that kind of asset is not shared in a session" :
		reason == Reason::TooLarge    ? "it is too large to send" :
		reason == Reason::BadPath     ? "its path is not inside the project" :
		reason == Reason::RateLimited ? "too many were created at once" :
		reason == Reason::NotPermitted? "the host did not permit it" : "the host refused it";
	m_assetNotice = std::string("Your new asset stays local: ") + why + ".";
	Logger::Log(Logger::LogLevel::Warning, ("Collab: create refused — " + m_assetNotice).c_str());
	m_pendingCreateFull.clear();
	m_createRetried = false;
}

void CollabController::noteAssetCreated(HE::Net::ParticipantId who,
                                        const std::string& path)
{
	CreatedAssetNotice n;
	n.path = path;
	for (const HE::Net::Participant& p : participants())
		if (p.id == who) { n.byName = p.name; break; }
	n.atMs = 0;   // stamped by the UI, which owns the clock this is drawn against
	m_createdNotices.push_back(std::move(n));
	// A burst is a burst — keep the newest and drop the tail rather than let a
	// runaway peer grow this without bound.
	constexpr std::size_t kMax = 64;
	if (m_createdNotices.size() > kMax)
		m_createdNotices.erase(m_createdNotices.begin(),
		                       m_createdNotices.begin() +
		                       static_cast<std::ptrdiff_t>(m_createdNotices.size() - kMax));
}

void CollabController::publishAssetCreate(const std::string& relativePath,
                                          const std::string& fullPath)
{
	if (!m_collab || !inSession()) return;
	// A create that ARRIVED must not be announced back out. Same guard as
	// publishAsset, and it matters more here: a create bounces to everyone, so
	// an echo would multiply instead of merely repeating.
	if (m_applyingRemoteAsset) return;
	if (fullPath.empty()) return;

	const std::string& rel = relativePath;
	if (rel.empty() || !isSyncableAsset(rel)) return;
	// Same type gate as a save, for the same reason: `.hasset` is equally the
	// container around an imported mesh, and those must not travel this channel.
	if (!isSyncableAssetType(EditorAssetTypeCache::assetTypeOf(fullPath))) return;

	std::ifstream f(fullPath, std::ios::binary);
	if (!f) return;
	std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)),
	                                 std::istreambuf_iterator<char>());
	if (bytes.empty()) return;

	// Deliberately NO lock dance, unlike publishAsset: nobody can hold a lock on
	// an asset that does not exist yet, and asking for one would race every
	// other peer creating at the same path. The host settles the name instead,
	// and grants the lock as part of accepting.
	// Remembered so the host's answer can be acted on: a refusal because the
	// name was taken comes back with a free one, and renaming the file we just
	// wrote is only possible if we still know where it is.
	m_pendingCreateFull = fullPath;

	HE::Net::CollabSession::AssetUpdate a;
	a.subject = assetSubject(rel);
	a.path    = rel;
	a.bytes   = std::move(bytes);
	a.intent  = HE::Net::CollabSession::AssetIntent::Create;
	m_collab->sendAsset(a);

	// The host answers its OWN create immediately — there is no round trip to
	// wait for, and leaving the retry armed would misfire on the next one.
	if (isHost())
	{
		m_pendingCreateFull.clear();
		m_createRetried = false;
	}
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
