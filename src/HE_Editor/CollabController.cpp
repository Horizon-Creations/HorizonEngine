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

constexpr std::size_t kMiB = 1024u * 1024u;

// The megabytes the user chose, as the byte count the session enforces. One
// conversion, in one place, because the number is set from the config, read back
// for the panel and quoted in two notification texts — four spellings of the
// same arithmetic is how a limit ends up being described as one thing and
// applied as another.
std::uint32_t assetCeilingBytes(int mb)
{
	const int clamped = std::clamp(mb, CollabController::kMinAssetMB,
	                                   CollabController::kMaxAssetMB);
	return static_cast<std::uint32_t>(static_cast<std::size_t>(clamped) * kMiB);
}

// Bytes as whole megabytes, rounded UP: a 1.2 MB file described as "1 MB" next
// to a 1 MB limit reads as a contradiction, and the number in a refusal only
// has to be big enough to explain the refusal.
std::size_t asMB(std::size_t bytes)
{
	return (bytes + kMiB - 1) / kMiB;
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
	// On a host this is not a preference but the session's rule, and it is fixed
	// here for as long as the session lives — see setSyncLargeAssets for why it
	// cannot be moved afterwards.
	cfg.syncLargeAssets = m_syncLargeAssets;
	// The ceiling, by contrast, is only ours and stays adjustable — this is the
	// starting value, not a rule anybody else is bound by.
	cfg.maxAssetBytes = assetCeilingBytes(m_maxAssetMB);
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

	// ── Is that session right here? ──
	// Then use the address and port it announced, and never ask the directory.
	// See lanEndpointFor for why this is a correctness fix and not a shortcut.
	if (const auto* near = lanEndpointFor(m_lanBrowser.sessions(), sessionId))
	{
		Logger::Log(Logger::LogLevel::Info,
			("Collab: that session is on this network — connecting directly to " +
			 near->address + ":" + std::to_string(near->port) +
			 " instead of asking the directory").c_str());
		m_sessionId = sessionId;
		return beginLink(near->address, near->port, joinCode, displayName);
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
	// A directory lookup is by definition not a LAN address; beginLink sets this
	// truthfully once an address exists. Cleared here so a previous LAN attempt
	// cannot label this one.
	m_connectIsLan = false;

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
	// On a client this is consent, not a rule: it decides whether a host that
	// carries the big assets will have us at all. What actually travels once we
	// are in is the session's answer, which arrives in the accept.
	cfg.syncLargeAssets = m_syncLargeAssets;
	// Ours alone, and not part of what the host is asked to agree to: a guest
	// with a low ceiling simply does not accept the biggest files, and finds out
	// per file rather than at the door.
	cfg.maxAssetBytes = assetCeilingBytes(m_maxAssetMB);
	applyLocalIdentity(cfg);
	m_collab = std::make_unique<CollabSession>(m_net.get(), NetRole::Client, cfg);
	m_collab->setStateProvider(m_provider.get());
	wireCallbacks();

	// Remembered before anything can fail, so a refusal that the user can answer
	// — "this session sends the big files, agree?" — has a host to dial again.
	// Its own copy: teardown() at the top of every join clears m_joinCode and
	// m_localAddress, so a retry reading those would connect to nothing.
	m_lastJoin.host        = host;
	m_lastJoin.port        = port;
	m_lastJoin.joinCode    = joinCode;
	m_lastJoin.displayName = displayName;
	m_lastJoin.valid       = true;

	m_joinCode      = joinCode;
	m_localAddress  = host;
	m_port          = port;
	m_isHost        = false;
	m_status        = Status::Connecting;
	m_connectTarget = host;
	// Is this an address we HEARD rather than looked up? Derived here rather
	// than passed in, so it is true however the join was started — the list, the
	// session id, or a hand-typed address that happens to be a machine
	// announcing itself. What it changes is which cause a silent failure names.
	m_connectIsLan = false;
	for (const auto& s : m_lanBrowser.sessions())
	{
		if (s.address == host && s.port == port) { m_connectIsLan = true; break; }
	}
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
		case JoinRejectReason::LargeAssetsRequired:
			// The one refusal that is really a question. The error text is still
			// filled in — the panel may be closed, and a status line that said
			// nothing would leave the join looking like a timeout — but the flag
			// is what matters: it is what turns this into a dialog the user can
			// say yes to, rather than a dead end for someone who would happily
			// have agreed.
			m_largeAssetsPrompt = true;
			m_error = "This session also transfers meshes, textures and audio. Turn on "
			          "\"Sync larger assets\" to join it — it can use considerably more "
			          "data.";
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
		// THE fix for "applied for a moment, then snapped back". A whole file is
		// a coarser statement about a document we may already have newer deltas
		// for; applying it then would undo them. A create is exempt: it is the
		// document's first version, so there is nothing it could be older than.
		if (a.intent != HE::Net::CollabSession::AssetIntent::Create &&
		    !acceptRevision(a.path, a.revision))
		{
			return;
		}
		// A create landing on a path WE are still creating. Writing it now would
		// truncate our file before the host's answer has told us to move it
		// aside — see DeferredCreate. Held until our own create is settled.
		if (a.intent == HE::Net::CollabSession::AssetIntent::Create &&
		    m_pendingCreates.count(a.path))
		{
			m_deferredCreates.push_back({ a.path, a.bytes, who });
			return;
		}
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

	m_collab->onAssetRefused([this](HE::Net::ParticipantId who, const std::string& path,
	                                std::uint32_t bytes) {
		// The far end believes this save went out — its own ceiling let it past,
		// so nothing over there failed. This machine is the only one that knows
		// the file is not coming, which makes it the only one that can say so.
		std::string name;
		for (const HE::Net::Participant& p : participants())
		{
			if (p.id != who) continue;
			name = p.name;
			break;
		}
		noteAssetTooLarge(path, bytes, /*sending=*/false, name);
	});

	m_collab->onAssetRefusedByPeer([this](HE::Net::ParticipantId who,
	                                      const std::string& path, std::uint32_t bytes,
	                                      std::uint32_t theirLimit,
	                                      HE::Net::CollabSession::AssetIntent intent) {
		const bool wasCreate = (intent == HE::Net::CollabSession::AssetIntent::Create);

		// A CREATE that never got past their ceiling gets no verdict, and that
		// leaves worse behind than a missing notification. publishAssetCreate
		// parks an entry under this key to match the host's answer to the right
		// file — and for a create refused on ARRIVAL there is no answer: the host
		// drops the frame before the bytes are assembled, so arbitrateCreate,
		// which is what would have sent one, is never reached. The entry then
		// waits for ever, and while it waits the deferred-create branch in
		// onAssetUpdated parks every peer's create for this same path behind it.
		// One user's oversized file would quietly stop that path from ever being
		// created by anyone. The refusal IS the verdict; settle on it.
		if (wasCreate)
		{
			const auto it = m_pendingCreates.find(path);
			if (it != m_pendingCreates.end())
			{
				m_pendingCreates.erase(it);
				// Whatever was held back waiting for our answer is released here
				// for the same reason onOwnCreateAnswered releases it: this is
				// the answer, and it is not going to be followed by another.
				flushDeferredCreate(path);
			}
		}

		noteAssetRefusedByPeer(who, path, bytes, theirLimit, wasCreate);
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
		       const std::string& path, const std::string& newPath, bool folder,
		       std::uint32_t batch) {
			// A folder create needs no answer — it is relayed as it arrives.
			// Queueing it would put a row in front of the host that has no
			// decision in it. A reimport notice is the same shape: it announces
			// something that has already happened on the requester's disk, and
			// there is nothing for the host to weigh or to refuse.
			if (op == HE::Net::CollabSession::AssetOp::Create ||
			    op == HE::Net::CollabSession::AssetOp::Reimport)
			{
				if (m_collab)
					m_collab->broadcastAssetOpApply(op, path, newPath, who, folder);
				return;
			}
			// Folded into the existing row when the same asset is already
			// waiting: three people wanting one file gone is one decision.
			for (PendingAssetOp& p : m_pendingOps)
			{
				if (p.op == op && p.path == path && p.newPath == newPath)
				{
					// Asked twice — one row, but the NEWER request id. A verdict
					// is addressed to a REQUEST, and the first one is no longer
					// what the asker is waiting on: keeping it answered a
					// request they had already replaced and left the live one
					// hanging for good.
					bool known = false;
					for (auto& rq : p.requesters)
					{
						if (rq.id != who) continue;
						rq.requestId = requestId;
						known = true;
						break;
					}
					if (!known) p.requesters.push_back({ who, requestId });
					// A row asked for on its own, now also asked for as part of
					// a selection, JOINS that selection. The alternative is a
					// file that quietly drops out of the bundle it was selected
					// in and is left sitting in the tray alone after the bundle
					// has been answered — which reads as a bug in the delete,
					// not as an artefact of who asked first. A row that already
					// belongs to a batch keeps it: coalescing is per asset, and
					// the first batch to name it owns it.
					if (!p.inBatch() && batch != 0)
					{
						p.batchOwner = who;
						p.batchId    = batch;
					}
					return;
				}
			}
			// A queue nobody could ever work through is not a queue. Repeat
			// asks for one asset already fold into the row above, so getting
			// here this often means a peer is naming hundreds of DIFFERENT
			// files — a runaway client rather than a person, and no host is
			// going to answer that list anyway. Refused at the door, so what
			// the host does see stays readable.
			constexpr std::size_t kMaxPendingOps = 128;
			if (m_pendingOps.size() >= kMaxPendingOps)
			{
				m_collab->sendAssetOpVerdict(who, requestId, false);
				return;
			}
			PendingAssetOp p;
			p.op      = op;
			p.path    = path;
			p.newPath = newPath;
			p.folder  = folder;
			p.requesters.push_back({ who, requestId });
			p.firstAskedMs = m_lastUpdateMs;
			p.batchOwner   = batch != 0 ? who : 0;
			p.batchId      = batch;
			m_pendingOps.push_back(std::move(p));
		});

	// ── Somebody wants what WE are holding ──
	m_collab->onAssetEditRequested(
		[this](HE::Net::ParticipantId from, std::uint32_t requestId,
		       const std::string& path) {
			// Not ours any more — we let go between the host routing this and it
			// arriving. Answering no is what tells the host to look again and
			// give the asker the truthful current answer; a row asking us to
			// hand over something we do not have has no meaningful button.
			if (!ownsAssetLock(path))
			{
				if (m_collab)
					m_collab->sendAssetEditAnswer(from, requestId, path, false,
					                              assetSubject(path));
				return;
			}
			// Asked twice — one row, and the NEWER request id, because that is
			// the one still waiting for an answer. Keeping the first would leave
			// the second ask hanging forever.
			for (EditRequest& e : m_editRequests)
			{
				if (e.id == from && e.path == path) { e.requestId = requestId; return; }
			}
			// Bounded for the same reason the host's tray is: you can only hold
			// so many assets, so a list longer than this is a peer misbehaving,
			// and a refusal it can retry beats a queue that grows for ever.
			constexpr std::size_t kMaxEditRequests = 64;
			if (m_editRequests.size() >= kMaxEditRequests)
			{
				m_collab->sendAssetEditAnswer(from, requestId, path, false,
				                              assetSubject(path));
				return;
			}
			m_editRequests.push_back({ from, requestId, path, m_lastUpdateMs });
		});

	m_collab->onAssetOpVerdict([this](std::uint32_t requestId, bool approved) {
		for (std::size_t i = 0; i < m_ourPendingOps.size(); ++i)
		{
			if (m_ourPendingOps[i].requestId != requestId) continue;
			const auto        op   = m_ourPendingOps[i].op;
			const std::string path = m_ourPendingOps[i].path;
			const std::string name = std::filesystem::path(path).filename().string();
			m_ourPendingOps.erase(m_ourPendingOps.begin() +
			                      static_cast<std::ptrdiff_t>(i));

			if (op == HE::Net::CollabSession::AssetOp::Edit)
			{
				// Yes means it is ours: either the holder handed it over, or
				// nobody held it and there was nobody to ask. Both need saying —
				// unlike a delete, an approved edit request produces no visible
				// change by itself, so silence would read as no answer at all.
				if (approved)
				{
					// The handover already granted it; this covers the "was
					// free" case and, either way, records the lock so closing
					// the tab releases it.
					requestAssetLock(path);
					m_assetNotice = "\"" + name + "\" is yours to edit.";
				}
				else
					m_assetNotice = "\"" + name + "\" stays with whoever is editing it.";
				return;
			}

			// Only the refusal needs saying. An approval is followed immediately
			// by the change itself, which is its own confirmation.
			if (!approved)
			{
				const bool del = op == HE::Net::CollabSession::AssetOp::Delete;
				m_assetNotice = "The host did not approve that \"" + name +
					"\" be " + (del ? "deleted." : "renamed.");
				// Also to the store, because the footer line it just wrote is
				// gone the moment anything else happens — and a refusal is
				// exactly the answer somebody comes back looking for after
				// wondering why the file is still there. Warning, not Problem:
				// nothing is out of step, the host simply said no.
				postNote(HE::Ed::NoteLevel::Warning,
				         std::string("The host did not approve ") +
				             (del ? "deleting \"" : "renaming \"") + name + "\".",
				         {}, path);
			}
			return;
		}
	});

	m_collab->onAssetOpApply(
		[this](HE::Net::ParticipantId by, HE::Net::CollabSession::AssetOp op,
		       const std::string& path, const std::string& newPath, bool folder) {
			// A reimport announces a change we deliberately did NOT send the
			// bytes for, so there is nothing to apply — and it must not reach
			// the editor's apply handler, which reads anything that is not a
			// delete or a folder create as a rename and would try to move the
			// asset to an empty path. Saying so IS the whole message.
			if (op == HE::Net::CollabSession::AssetOp::Reimport)
			{
				if (by == localParticipant()) return;   // we are the one who did it
				noteRemoteReimport(by, path);
				return;
			}
			if (!m_onRemoteAssetOp) return;
			// Same guard as an arriving save: applying this touches the content
			// tree, and the notification would otherwise come straight back.
			m_applyingRemoteAsset = true;
			m_onRemoteAssetOp(op, path, newPath, folder);
			m_applyingRemoteAsset = false;
			// ── Did it actually happen? ──
			// The handler that carries this out takes a std::error_code from
			// every filesystem call it makes and looks at none of them, so a
			// delete that removed nothing and a delete that worked were the same
			// event: silence. That is how peers drift apart without anyone
			// finding out until the next source-control sync contradicts them.
			// The disk is asked instead of the ec, which also catches the two
			// refusals the handler makes on purpose (a rename onto an occupied
			// name, a path that resolves outside the project) — those return
			// early and never set an ec at all.
			verifyAppliedAssetOp(op, path, newPath);
		});

	m_collab->onDocDeltas([this](HE::Net::ParticipantId, std::uint64_t,
	                             const std::string& path,
	                             const std::vector<HE::Net::CollabSession::DocDelta>& batch,
	                             std::uint32_t revision) {
		if (!acceptRevision(path, revision)) return;
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

		// ── An asset lock that changed hands ──
		// Until handovers existed a lock only ever went free → granted, so
		// m_heldAssetLocks was maintained at the two places that asked for one
		// and gave it back. A handover moves it owner → owner without either
		// side calling those, and requestAssetLock() returns early when the
		// lock is already ours — BEFORE it records the path. Left alone, the
		// new holder would hold a lock it has no record of and never release
		// it. This is the one place that sees the transition from both sides.
		if (!acquired) return;
		const auto it = m_assetSubjectPaths.find(l.subject);
		if (it == m_assetSubjectPaths.end()) return;   // an entity, or not ours to track
		const std::string& path = it->second;
		if (l.owner == m_collab->localId())
		{
			if (std::find(m_heldAssetLocks.begin(), m_heldAssetLocks.end(), path)
			    == m_heldAssetLocks.end())
				m_heldAssetLocks.push_back(path);
		}
		else
		{
			m_heldAssetLocks.erase(
				std::remove(m_heldAssetLocks.begin(), m_heldAssetLocks.end(), path),
				m_heldAssetLocks.end());
		}
	});

	m_collab->onLockDenied([this](std::uint64_t subject, LockDenyReason reason) {
		// Refused means someone else is already editing it. Say who, so the user
		// knows to ask rather than wonder why nothing responds.
		if (reason == LockDenyReason::HeldByOther)
		{
			const LockInfo* l = m_collab->lockFor(subject);
			// This used to be written to a string nothing read. It goes where
			// the user can actually find it now — and it names the path when we
			// know one, so the row points at the thing that went read-only
			// rather than at "this".
			const auto it = m_assetSubjectPaths.find(subject);
			postNote(HE::Ed::NoteLevel::Warning,
			         (l && !l->ownerName.empty() ? l->ownerName
			                                     : std::string("Someone else")) +
			             " is editing this — it is read-only for you.",
			         {}, it != m_assetSubjectPaths.end() ? it->second : std::string{});
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
	// Walking away answers the question. Left standing, the dialog would reopen
	// over an idle panel the next time it is drawn, asking about a session the
	// user has already decided against.
	m_largeAssetsPrompt = false;
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
	// Say goodbye on the way out, from the one place EVERY exit from a session
	// passes through — leaving, being kicked, the link dying, the editor
	// quitting. Relying on the next update() to notice would work for all of
	// those except the last, and quitting is precisely when nobody is left to
	// notice: the session would sit in every browser's list until it expired,
	// and the first thing the next person did would be to click a corpse.
	m_lanAnnouncer.stop();
	m_lanInstance = 0;

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
		// The people who asked are gone with the session, but the person who was
		// going to answer is still sitting here, and their queue has just
		// emptied itself. Said once with a count rather than once per row: the
		// interesting fact is that a decision was never made, not which files it
		// was about.
		postNote(HE::Ed::NoteLevel::Warning,
		         m_pendingOps.size() == 1
		             ? "A request was still waiting when the session ended."
		             : std::to_string(m_pendingOps.size()) +
		                   " requests were still waiting when the session ended.",
		         "Nothing was applied — they count as refused.");
		m_pendingOps.clear();
	}
	m_openBatch = 0;
	// Same for asks pointed at us. Nothing was handed over, so the asker keeps
	// nothing and we keep everything — which is exactly what a refusal means.
	m_editRequests.clear();
	if (!m_ourPendingOps.empty())
	{
		// An edit request waits on the HOLDER, everything else on the host —
		// so the sentence has to say which, or it names the wrong person.
		const bool edit = m_ourPendingOps.front().op ==
			HE::Net::CollabSession::AssetOp::Edit;
		m_assetNotice = edit
			? "The session ended before that asset was handed over."
			: "The session ended before the host answered your request.";
		// One row per unanswered ask here, unlike the host's queue above: this
		// side asked for specific files and is still looking at them, so which
		// ones went unanswered is the whole point.
		for (const OurOp& o : m_ourPendingOps)
		{
			postNote(HE::Ed::NoteLevel::Warning,
			         "Nobody answered your request for \"" +
			             std::filesystem::path(o.path).filename().string() +
			             "\" before the session ended.",
			         o.op == HE::Net::CollabSession::AssetOp::Edit
			             ? "It stayed with whoever was editing it."
			             : "Nothing was changed.",
			         o.path);
		}
		m_ourPendingOps.clear();
	}
	else m_assetNotice.clear();
	m_pendingCreates.clear();
	// Bytes held back for an answer that is never coming. Dropped rather than
	// written: nobody is left to agree that this file belongs here, and our own
	// create at that path was never moved aside.
	m_deferredCreates.clear();
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

	// Same reason, more so: the browser's whole job is to have a list ready
	// BEFORE there is a session. Behind the transport early-out it would only
	// ever run once joining had already happened.
	updateLanDiscovery(nowMs);

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
		{
			m_connectDeadlineMs =
				nowMs + (m_connectIsLan ? kLanConnectTimeoutMs : kConnectTimeoutMs);
		}
		else if (nowMs > m_connectDeadlineMs)
		{
			// Name the address, and name the RIGHT cause for it.
			//
			// A machine we can hear announcing itself on this network is not
			// "not forwarded" — forwarding has nothing to do with two computers
			// on one subnet, and saying so sent people into their router while
			// the actual block sat on the host's own machine. The signature is
			// unmistakable once you look for it: a reachable host with nothing
			// listening answers with a reset, and the connect fails INSTANTLY.
			// A wait that runs to the deadline means the packets are being
			// dropped in silence, which is what a host firewall does.
			if (m_connectTarget.empty())
			{
				m_error = "The session could not be looked up in time. Check the "
				          "session ID, and that you are online.";
			}
			else if (m_connectIsLan)
			{
				m_error = "No answer from " + m_connectTarget + ":" +
					std::to_string(m_port) + ", although that machine is announcing "
					"the session on this network. Nothing was refused — the "
					"connection was dropped in silence, which is what a firewall on "
					"the HOST's machine does to a program it has not been allowed to "
					"accept connections for. On Windows, allow the editor through "
					"Windows Defender Firewall for private networks.";
			}
			else
			{
				m_error = "No answer from " + m_connectTarget + ":" +
					std::to_string(m_port) + " after 20 seconds. The host is "
					"published but nothing there accepted the connection — if that "
					"address is IPv6, your network may not have a route to it; "
					"otherwise the host's port is not actually forwarded.";
			}
			m_directoryStatus.clear();
			Logger::Log(Logger::LogLevel::Warning, ("Collab: " + m_error).c_str());

			// Give the attempt up for real. Leaving the socket pending meant the
			// kernel kept dialling long after the editor had stopped waiting —
			// on macOS a further 55 seconds — and then reported the single most
			// diagnostic fact of the whole exchange (ETIMEDOUT: dropped, versus
			// ECONNREFUSED: no listener) into a log nobody had a reason to open
			// any more. Tearing down now ends the attempt where the message
			// says it ended.
			teardown();
			m_status            = Status::Failed;
			m_connectDeadlineMs = 0;
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

// ─── Sessions on this network ────────────────────────────────────────────────

const std::vector<HE::Net::LanBeacon::Browser::Session>&
CollabController::lanSessions() const
{
	return m_lanBrowser.sessions();
}

const HE::Net::LanBeacon::Browser::Session* CollabController::lanEndpointFor(
	const std::vector<HE::Net::LanBeacon::Browser::Session>& sessions,
	const std::string& sessionId)
{
	if (sessionId.empty()) return nullptr;
	for (const auto& s : sessions)
	{
		if (s.sessionId != sessionId) continue;
		// An entry without somewhere to connect to is worse than no entry: it
		// would take precedence over the directory and then fail.
		if (s.address.empty() || s.port == 0) continue;
		return &s;
	}
	return nullptr;
}

void CollabController::setLanDiscoveryEnabled(bool on)
{
	if (m_lanEnabled == on) return;
	m_lanEnabled = on;
	// Takes effect NOW rather than on the next restart: switching it off while
	// hosting has to actually stop the announcing, which is the entire reason
	// somebody would switch it off. stop() sends the goodbye on the way out.
	if (!on)
	{
		m_lanAnnouncer.stop();
		m_lanBrowser.stop();
	}
	// Cleared in BOTH directions, so switching off and on again is the retry
	// after granting the local-network permission — otherwise the latch below
	// would keep refusing for the rest of the run.
	m_lanBlocked = false;
	// Turning it on starts nothing here — updateLanDiscovery does that on the
	// next frame, from the one place that knows whether we host, browse or
	// neither.
}

// ─── Bigger assets ───────────────────────────────────────────────────────────

void CollabController::setSyncLargeAssets(bool on)
{
	if (m_syncLargeAssets == on) return;
	// Refused while a session is up, and without a word: the editor pushes its
	// config down here every frame, so a log line would repeat sixty times a
	// second and a return value nobody reads would say nothing at all. The panel
	// is where the user learns this — it greys the checkbox out and explains it
	// (largeAssetSyncLocked). What this guard actually prevents is the case the
	// UI cannot: a setting changed through some other route mid-session, leaving
	// one peer publishing meshes into a session the others do not carry them in,
	// so half the group silently holds files the other half never sees.
	if (active())
	{
		Logger::Log(Logger::LogLevel::Warning,
		            "Collab: large-asset sync cannot be changed during a session — "
		            "the session keeps the rule it started with.");
		return;
	}
	m_syncLargeAssets = on;
}

void CollabController::setMaxAssetMB(int mb)
{
	// Clamped rather than refused: this comes from a config file that a user can
	// edit by hand, and a 4000 in there must not become a session that will
	// cheerfully try to hold four gigabytes because somebody typed a number. The
	// bounds and the reasoning behind them live in the header, beside the
	// constants, so the panel is reading the same justification it shows.
	const int clamped = std::clamp(mb, kMinAssetMB, kMaxAssetMB);
	if (clamped == m_maxAssetMB) return;   // the editor pushes its config every frame
	m_maxAssetMB = clamped;

	// Straight into a running session, and deliberately so. This binds nothing
	// but our own memory and our own link — there is no agreement between peers
	// for a mid-session change to break, the way there is for the large-asset
	// switch — so somebody told "raise the limit in Preferences" can do it and
	// have the next save go through, rather than being told to leave first.
	if (m_collab) m_collab->setMaxAssetBytes(assetCeilingBytes(m_maxAssetMB));
}

bool CollabController::retryJoinWithLargeAssets()
{
	if (!m_lastJoin.valid) return false;
	// The whole point of this call: the refusal was about our answer, so retrying
	// with the same answer would be refused identically. The caller is expected
	// to have persisted the setting too — this controller does not own
	// config.json — but the value that goes on the wire is set here so a caller
	// that forgot still gets a join that works rather than an infinite loop.
	m_syncLargeAssets   = true;
	m_largeAssetsPrompt = false;

	// COPIES, taken before joinSession() calls teardown(). teardown clears the
	// controller's session state, and m_lastJoin lives on the controller — the
	// arguments would be read out of a struct that had just been reset.
	const std::string   host = m_lastJoin.host;
	const std::uint16_t port = m_lastJoin.port;
	const std::string   code = m_lastJoin.joinCode;
	const std::string   name = m_lastJoin.displayName;
	Logger::Log(Logger::LogLevel::Info,
	            ("Collab: agreeing to large-asset sync and dialling " + host + ":" +
	             std::to_string(port) + " again").c_str());
	return joinSession(host, port, code, name);
}

bool CollabController::sessionSyncsLargeAssets() const
{
	// The session's answer when there is a session, because the host decides and
	// our own setting may well disagree with it. Outside one there is nothing to
	// obey, so the local setting is the truthful answer — and it matters that it
	// is: assetTypeTravels is asked by code that also runs with no session, and
	// a hardcoded false there would be a different answer to the same question
	// depending only on timing.
	if (m_collab && inSession()) return m_collab->sessionSyncsLargeAssets();
	return m_syncLargeAssets;
}

bool CollabController::assetTypeTravels(HE::AssetType type) const
{
	// Two independent reasons for an asset to travel, and the static one is left
	// exactly as it was: isCollabSyncableAssetType lives in HE_Core and has
	// callers outside the editor, so the session's decision is combined HERE
	// rather than smuggled into a predicate that is supposed to describe the
	// asset kind and nothing else.
	return isSyncableAssetType(type) || sessionSyncsLargeAssets();
}

void CollabController::updateLanDiscovery(std::uint64_t nowMs)
{
	if (!m_lanEnabled)
	{
		if (m_lanAnnouncer.running()) m_lanAnnouncer.stop();
		if (m_lanBrowser.running())   m_lanBrowser.stop();
		return;
	}

	// Refused once, refused every frame: creating and failing a socket sixty
	// times a second changes nothing and hides the real event in the log. The
	// latch clears only when the user turns discovery off and on again, which
	// is also the natural thing to do after granting the permission.
	if (m_lanBlocked) return;

	// ── Hosting: say we are here ──
	if (m_isHost && m_status == Status::Hosting && m_port != 0)
	{
		if (!m_lanAnnouncer.running())
		{
			// Derived from the session id, which is CSPRNG-minted per session:
			// the two copies of one beacon (multicast and broadcast) collapse to
			// one row, and a host that reopens a session reads as a new one
			// because it gets a new id. FNV-1a, because all this has to be is
			// stable within a run and different between them.
			std::uint64_t h = 1469598103934665603ull;
			for (const unsigned char c : m_sessionId)
				{ h ^= c; h *= 1099511628211ull; }
			m_lanInstance = h ? h : 1;

			HE::Net::LanBeacon::Announcement a;
			a.protocol     = HE::Net::kCollabProtocolVersion;
			a.instance     = m_lanInstance;
			a.sessionId    = m_sessionId;
			a.port         = m_port;
			a.projectLabel = m_projectLabel;
			a.projectKey   = m_projectId;
			// Read from the SESSION, not from m_syncLargeAssets: this is the same
			// value the handshake answers a joiner with, so the row in someone
			// else's list cannot promise one thing while the join enforces the
			// other. Announced once and never revised, which is safe for exactly
			// as long as setSyncLargeAssets keeps refusing to change it during a
			// session — if that ever softens, this has to be pushed on every tick
			// like the participant count below.
			a.syncsLargeAssets = sessionSyncsLargeAssets();
			for (const HE::Net::Participant& p : participants())
				if (p.isHost) { a.hostName = p.name; break; }
			// No join code. See LanBeacon.h — announcing it would let anyone on
			// the segment walk straight in.
			if (!m_lanAnnouncer.start(a))
			{
				// macOS refuses local-network traffic to an app that was not
				// granted it, and there is nothing about that to retry every
				// frame. The setting stays on; only this attempt failed.
				m_lanBlocked = true;
				return;
			}
			m_lanBlocked = false;
		}
		m_lanAnnouncer.setParticipants(
			static_cast<std::uint8_t>(std::min<std::size_t>(participants().size(), 255)));
		m_lanAnnouncer.update(nowMs);
	}
	else if (m_lanAnnouncer.running())
	{
		m_lanAnnouncer.stop();   // session ended — say goodbye rather than fade out
	}

	// ── Listen, always ──
	// Deliberately NOT only while idle, though the join list is the only thing
	// that consumes it. Two reasons, both learned the hard way: hosting used to
	// switch the ear off, so the obvious way to test — host on both machines and
	// look — could never work by construction, and there was no way to answer
	// "can these two even see each other?" without giving up one of the
	// sessions. A drained socket costs nothing on a frame where nobody spoke.
	//
	// Our own beacon comes straight back off the segment, so the browser is told
	// which instance is us and leaves it out of the list.
	m_lanBrowser.setSelfInstance(m_lanInstance);
	if (!m_lanBrowser.running())
	{
		if (!m_lanBrowser.start()) { m_lanBlocked = true; return; }
		m_lanBlocked = false;
	}
	m_lanBrowser.update(nowMs);
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

bool CollabController::acceptRevision(const std::string& relativePath,
                                      std::uint32_t revision)
{
	if (relativePath.empty()) return false;

	// Revision 0 means "unversioned" — a create, or a frame from a peer that
	// does not number them. Applied, but it never lowers what we have seen: a
	// zero must not be able to reopen the door for older frames behind it.
	if (revision == 0) return true;

	std::uint32_t& seen = m_docRevision[relativePath];
	if (revision <= seen)
	{
		// Not an error, and deliberately not a warning: with two channels for
		// one document, a late whole file arriving behind newer deltas is the
		// ORDINARY case. Refusing it quietly is the entire mechanism.
		return false;
	}
	seen = revision;
	return true;
}

bool CollabController::publishDocDeltas(
	const std::string& relativePath,
	const std::vector<HE::Net::CollabSession::DocDelta>& batch)
{
	if (!m_collab || !inSession() || batch.empty()) return false;
	// One step per publish, whichever channel it goes out on.
	const std::uint32_t rev = ++m_docRevision[relativePath];
	return m_collab->sendDocDeltas(assetSubject(relativePath), relativePath, batch, rev);
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
	// those are what must not travel this channel unless the session was opened
	// to carry them — which is what assetTypeTravels folds in. This hook fires
	// for EVERY ContentManager save, including an import, so the type gate
	// belongs here and not only where the editor walks its open tabs. C++
	// sources are exempt: they have no HAsset header to sniff.
	if (relativePath.rfind("::Source::", 0) != 0 &&
	    !assetTypeTravels(EditorAssetTypeCache::assetTypeOf(fullPath))) return;

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

	// The same counter the deltas use. A whole file is simply a coarser
	// statement about the same document, so it takes the next number rather
	// than a numbering of its own — that is what lets the receiver order the two
	// against each other at all.
	HE::Net::CollabSession::AssetUpdate up;
	up.subject  = subject;
	up.path     = relativePath;
	up.bytes    = bytes;
	up.intent   = HE::Net::CollabSession::AssetIntent::Update;
	up.revision = ++m_docRevision[relativePath];
	const std::size_t size = up.bytes.size();
	if (m_collab->sendAsset(up)) return;

	// It did not go. Two things can cause that here — we hold the lock, which
	// was checked above — and only one of them is worth a notice: the size
	// ceiling. A link that died in the same breath announces itself as a lost
	// session a moment later, and a second message about one file would only
	// bury it.
	//
	// The ceiling, though, is what a user walks straight into the day they turn
	// large-asset sync on: nothing truncates, the frame is simply never emitted
	// (CollabSession::sendAsset, and again on the receiving side), and until now
	// the only trace was a log line. A 200 MB mesh vanishing without a word is
	// precisely what makes people stop believing a setting does anything.
	if (size <= m_collab->maxAssetBytes()) return;
	noteAssetTooLarge(relativePath, size, /*sending=*/true);
}

void CollabController::noteAssetTooLarge(const std::string& relPath, std::size_t bytes,
                                         bool sending, const std::string& who)
{
	const std::string name = std::filesystem::path(relPath).filename().string();
	const std::size_t mb   = asMB(bytes);
	// Read off the live session rather than off m_maxAssetMB, so the number in
	// the sentence is the one that actually refused the file even if the setting
	// has moved since — the reader is being told why THIS did not go through.
	const std::size_t limit = m_collab ? (m_collab->maxAssetBytes() / kMiB)
	                                   : static_cast<std::size_t>(m_maxAssetMB);

	if (sending)
	{
		postNote(HE::Ed::NoteLevel::Problem,
		         "\"" + name + "\" was not sent to the others.",
		         "It is " + std::to_string(mb) + " MB and this machine will not send "
		             "more than " + std::to_string(limit) + " MB in one file. Raise "
		             "\"Largest asset to transfer\" in Preferences ▸ Collaboration, or "
		             "leave this one to source control — the others still have the "
		             "older file.",
		         relPath);
		return;
	}

	// The other end's ceiling was high enough or it would never have started, so
	// theirs is the higher of the two and ours is what stopped it. Saying that
	// plainly is what makes the notice actionable: the fix is on THIS machine,
	// and the person who saved the file cannot make it happen.
	const std::string sender = who.empty() ? std::string("Someone") : who;
	postNote(HE::Ed::NoteLevel::Problem,
	         sender + " sent \"" + name + "\", and this machine refused it.",
	         "It is " + std::to_string(mb) + " MB and this machine accepts at most " +
	             std::to_string(limit) + " MB in one file — their limit is higher than "
	             "yours. Raise \"Largest asset to transfer\" in Preferences ▸ "
	             "Collaboration and ask them to save it again, or pull it from source "
	             "control. Nothing was written here, so the file you have is the old one.",
	         relPath);
}

void CollabController::noteAssetRefusedByPeer(HE::Net::ParticipantId who,
                                              const std::string& relPath,
                                              std::size_t bytes,
                                              std::size_t theirLimitBytes,
                                              bool wasCreate)
{
	const std::string name = std::filesystem::path(relPath).filename().string();
	const std::size_t mb   = asMB(bytes);
	// THEIR number, off the wire. Ours is no use in this sentence — it is the
	// limit that let the file go — and there is no other way to learn it: the
	// ceiling is a local setting on a machine this one cannot ask.
	const std::size_t limit = theirLimitBytes / kMiB;

	// Resolved from the live roster, which may no longer have them: a refusal is
	// forwarded by the host and can land after its author has left. "Someone" is
	// the same fallback the receiving-side notice uses, and it is better than a
	// blank where a name belongs.
	std::string peer = "Someone";
	for (const HE::Net::Participant& p : participants())
	{
		if (p.id != who) continue;
		if (!p.name.empty()) peer = p.name;
		break;
	}

	// The part that makes this actionable rather than merely true. Nothing here
	// failed, so the reader's instinct — save it again — produces exactly the same
	// silence a second time. The way through is the one channel that does carry
	// files this size, and the fix to the session itself is on THEIR machine, not
	// on this one, so the sentence has to say whose limit it was.
	const std::string left = wasCreate
		? std::string("They never received it, so on their machine that asset does "
		              "not exist at all.")
		: std::string("It was not written there, so what they have is still the "
		              "older version.");

	postNote(HE::Ed::NoteLevel::Problem,
	         peer + " could not accept \"" + name + "\".",
	         "It is " + std::to_string(mb) + " MB and " + peer + " accepts at most " +
	             std::to_string(limit) + " MB in one file — their limit is lower than "
	             "yours, which is why nothing on this machine reported a problem. " +
	             left + " Ask them to raise \"Largest asset to transfer\" in "
	             "Preferences ▸ Collaboration and save it again, or put this file "
	             "through source control.",
	         relPath);
}

// ─── Reimport ────────────────────────────────────────────────────────────────

bool CollabController::assetWritableNow(const std::string& relPath)
{
	// Outside a session everything is writable — that is the whole point of the
	// question being asked at all rather than assumed.
	if (!m_collab || !inSession() || relPath.empty()) return true;
	const std::uint64_t      subject = assetSubject(relPath);
	const HE::Net::LockInfo* l       = m_collab->lockFor(subject);
	// The REPLICATED table, deliberately, not a queryLock round trip: this
	// answers a background operation that is about to write a file, and it has
	// to answer now. The table is up to one round trip stale, which costs us the
	// case where somebody took the lock in the last few milliseconds — and that
	// case is caught anyway, because their lock is what the wire refuses next.
	return !l || l->owner == m_collab->localId();
}

bool CollabController::beginBackgroundWrite(const std::string& relPath)
{
	if (assetWritableNow(relPath)) return true;

	// Refused, and the user has to be told why — this is a menu item that simply
	// would not happen otherwise, and "nothing happened" is the one outcome a
	// user cannot act on. Naming the holder is what makes it actionable: the fix
	// is to go and ask that person, not to click again.
	const HE::Net::LockInfo* l    = assetLockInfo(relPath);
	const std::string        name = std::filesystem::path(relPath).filename().string();
	const std::string        who  = l && !l->ownerName.empty() ? l->ownerName
	                                                           : std::string("Someone else");
	// The second sentence depends on the session: with large assets on, the new
	// bytes WOULD have travelled, so claiming they cannot is simply false — and
	// a detail line that is false about the one thing it explains is worse than
	// none. What is true either way is that the holder's copy would have been
	// overwritten, which is the actual reason this was refused.
	postNote(HE::Ed::NoteLevel::Problem,
	         who + " is editing \"" + name + "\" — it was not reimported.",
	         sessionSyncsLargeAssets()
	             ? std::string("Reimporting would have rewritten the file underneath "
	                           "them. Ask them to release it and try again.")
	             : std::string("Reimporting would have rewritten the file underneath "
	                           "them, and the new bytes do not travel over the session."),
	         relPath);
	m_assetNotice = who + " is editing \"" + name + "\" — it was not reimported.";
	return false;
}

void CollabController::publishReimport(const std::string& relPath,
                                       const std::string& fullPath)
{
	if (!m_collab || !inSession() || relPath.empty()) return;

	// An authored asset that travels anyway: its new bytes ARE the message, and
	// they go out through the ordinary save path. Nothing about a reimport makes
	// it a different kind of change once the file is on the wire.
	//
	// The lock condition is not decoration. publishAsset claims the lock when it
	// is free and then RETURNS EMPTY-HANDED on a client, because the grant is a
	// round trip away and it has nothing to wait on — which is fine for a save
	// (the next one carries the file) and useless here, where there is no next
	// one. A reimport happens once. So a client that does not already hold the
	// asset takes the notice branch instead: "go and pull it" is a truthful
	// downgrade, and silence — peers keeping the old bytes for ever with nothing
	// to say so — is the exact failure this function exists to end.
	// "Will publishAsset actually send?" spelled out, rather than trusted: we
	// hold it, or we are the host and it is free (the host's own claim is
	// granted inline, with no round trip to lose).
	const bool canSendBytes = ownsAssetLock(relPath) ||
	                          (isHost() && assetLockInfo(relPath) == nullptr);
	if (canSendBytes && isSyncableAsset(relPath) &&
	    (relPath.rfind("::Source::", 0) == 0 ||
	     assetTypeTravels(EditorAssetTypeCache::assetTypeOf(fullPath))))
	{
		publishAsset(relPath, fullPath);
		return;
	}

	// Everything else — and outside a large-asset session that is MOST of what
	// reimport touches: meshes, textures, audio, fonts. Those sit outside the
	// type-based sync set (see isCollabSyncableAssetType) because they are large
	// and source control is what carries them. That used to be the end of it:
	// widening the set for one menu item would have pushed tens of megabytes
	// down a link built for graphs, silently, on every peer.
	//
	// A HOST can now decide otherwise for its session, and when it has, the
	// branch above already sent the new bytes — assetTypeTravels answered yes.
	// This path is what is left when it did not: the fact travels instead of the
	// file, and each peer is told to go and pull it.
	requestOrPerformAssetOp(HE::Net::CollabSession::AssetOp::Reimport,
	                        relPath, {}, /*folder=*/false);
}

void CollabController::noteRemoteReimport(HE::Net::ParticipantId by,
                                          const std::string& relPath)
{
	std::string who;
	for (const HE::Net::Participant& p : participants())
	{
		if (p.id != by) continue;
		who = p.name;
		break;
	}
	if (who.empty()) who = "Someone";
	const std::string name = std::filesystem::path(relPath).filename().string();
	// Info, not Warning: nothing here is broken. The asset on this machine is
	// simply older than the one they now have, and the sentence says what to do
	// about it — which is the only reason to send this at all.
	// This notice only ever arrives for an asset whose bytes did NOT travel, so
	// the advice is always "pull it". The sentence underneath is the one that
	// goes stale: in a session that carries the big files, "meshes, textures and
	// audio do not travel" contradicts what the user was told when they agreed
	// to it, and the honest thing to say is that this particular one did not.
	postNote(HE::Ed::NoteLevel::Info,
	         who + " reimported \"" + name + "\" — pull it from source control.",
	         sessionSyncsLargeAssets()
	             ? std::string("Its new bytes were not sent — they were too large, or "
	                           "somebody else held the file at the time.")
	             : std::string("Meshes, textures and audio do not travel over the session."),
	         relPath);
}

// ─── Did the peers actually converge? ────────────────────────────────────────

void CollabController::verifyAppliedAssetOp(HE::Net::CollabSession::AssetOp op,
                                            const std::string& relPath,
                                            const std::string& newRelPath)
{
	using Op = HE::Net::CollabSession::AssetOp;
	if (op != Op::Delete && op != Op::Rename) return;
	// No resolver means no way to look, and inventing an answer would be worse
	// than the silence this replaces. (Tests that care install one.)
	if (!m_localPathForKey) return;
	const std::string full = m_localPathForKey(relPath);
	if (full.empty()) return;   // not ours to reason about — outside the project

	std::error_code ec;
	// One test for both ops: the source path must be GONE. A delete that left it
	// there did nothing, and a rename that left it there did not move it — in
	// either case this machine now holds a file the rest of the session believes
	// is not there, and every later reference to it disagrees with everyone.
	if (!std::filesystem::exists(full, ec)) return;

	// Except when "gone" is not what a successful rename looks like. On a
	// case-insensitive filesystem — APFS and Windows, so most of them —
	// renaming Rock.hasset to rock.hasset leaves the OLD path resolving to the
	// file that was just moved, and a plain existence test would report the one
	// rename that certainly worked as drift. Two paths that name one file are
	// not a divergence; `equivalent` is the same guard the editor's own rename
	// handler uses one layer up, for the same reason.
	if (op == Op::Rename && !newRelPath.empty())
	{
		const std::string newFull = m_localPathForKey(newRelPath);
		if (!newFull.empty() && std::filesystem::exists(newFull, ec) &&
		    std::filesystem::equivalent(full, newFull, ec))
			return;
	}

	const std::string name = std::filesystem::path(relPath).filename().string();
	if (op == Op::Delete)
	{
		postNote(HE::Ed::NoteLevel::Problem,
		         "\"" + name + "\" was deleted in the session but is still here.",
		         "Everyone else no longer has it. Remove it by hand, or the next "
		         "source-control sync will disagree.",
		         relPath);
		return;
	}
	postNote(HE::Ed::NoteLevel::Problem,
	         "\"" + name + "\" was renamed in the session but not on this machine.",
	         newRelPath.empty()
	             ? std::string("The file is still under its old name here.")
	             : "Something is already at \"" +
	                   std::filesystem::path(newRelPath).filename().string() +
	                   "\" here, so the move was refused.",
	         relPath);
}

void CollabController::postNote(HE::Ed::NoteLevel level, std::string text,
                                std::string detail, const std::string& relPath)
{
	if (!m_notes) return;   // headless, or a test that never set one
	// Absolute where we can: the flyout offers to reveal what a row is about,
	// and a project-relative key is not something the OS can open. Falls back to
	// the key itself, which at least names the asset.
	std::string path;
	if (!relPath.empty())
	{
		if (m_localPathForKey) path = m_localPathForKey(relPath);
		if (path.empty()) path = relPath;
	}
	m_notes->post(level, std::move(text), std::move(detail), std::move(path));
}

bool CollabController::requestOrPerformAssetOp(
	HE::Net::CollabSession::AssetOp op,
	const std::string& relPath, const std::string& newRelPath, bool folder)
{
	if (!m_collab || !inSession() || relPath.empty()) return false;

	// Creating a folder asks nobody, whoever does it: it destroys nothing, so
	// there is no decision to weigh. A client's create still travels via the
	// host, which is what keeps everyone's order of events the same. A reimport
	// notice is the same: it reports something that has already happened here,
	// and there is no verdict that could undo it.
	const bool needsApproval = op != HE::Net::CollabSession::AssetOp::Create &&
	                           op != HE::Net::CollabSession::AssetOp::Reimport;

	// The host does it. It already answered the question a request exists to
	// ask — its own confirmation dialog — and asking itself again would be a
	// second dialog for one decision.
	if (isHost())
	{
		m_collab->broadcastAssetOpApply(op, relPath, newRelPath, localParticipant(), folder);
		return true;
	}

	// The open batch rides along, so a caller that wrapped its loop in
	// beginAssetOpBatch needs to change nothing else — and one that did not
	// sends 0, which is what a request on its own has always meant.
	const std::uint32_t id = m_collab->requestAssetOp(op, relPath, newRelPath, folder,
	                                                  /*subject=*/0, m_openBatch);
	if (!needsApproval)
	{
		// Sent, but not something we wait on: there is no verdict coming, and
		// putting it in the pending list would leave a count that never falls.
		return id != 0;
	}
	if (id == 0) return false;
	// Asking a second time for the same thing REPLACES our record of the first.
	// The host folds repeat asks into one row and keeps the newest request id,
	// so only one verdict is ever coming — a second entry here would wait for an
	// answer addressed to a request that no longer exists, and "1 request
	// pending" would sit in the footer for the rest of the session.
	bool replaced = false;
	for (OurOp& o : m_ourPendingOps)
	{
		if (o.op != op || o.path != relPath) continue;
		o.requestId = id;
		replaced = true;
		break;
	}
	if (!replaced) m_ourPendingOps.push_back({ id, op, relPath });
	m_assetNotice = (op == HE::Net::CollabSession::AssetOp::Delete
		? std::string("Asked the host to delete \"")
		: std::string("Asked the host to rename \"")) +
		std::filesystem::path(relPath).filename().string() + "\".";
	return true;
}

bool CollabController::hasAskedToEdit(const std::string& relPath) const
{
	for (const OurOp& o : m_ourPendingOps)
	{
		if (o.op == HE::Net::CollabSession::AssetOp::Edit && o.path == relPath)
			return true;
	}
	return false;
}

bool CollabController::requestAssetEdit(const std::string& relPath)
{
	if (!m_collab || !inSession() || relPath.empty()) return false;
	if (ownsAssetLock(relPath))  return false;   // already ours; nothing to ask
	if (hasAskedToEdit(relPath)) return false;   // asked once; asking again is noise

	// Deliberately NOT routed through requestOrPerformAssetOp: that function's
	// rule is "the host does it instead of asking", and here the host is an
	// asker like everyone else. The holder decides.
	const std::uint64_t subject = assetSubject(relPath);
	m_assetSubjectPaths[subject] = relPath;   // so the answer maps back to a path
	const std::uint32_t id = m_collab->requestAssetOp(
		HE::Net::CollabSession::AssetOp::Edit, relPath, {}, false, subject);

	if (id == 0)
	{
		// Nobody is holding it — there is nobody to ask. As host that is known
		// for certain; as client the host says so by granting. Either way the
		// lock is claimed the ordinary way rather than reported as a failure.
		requestAssetLock(relPath);
		return true;
	}

	m_ourPendingOps.push_back({ id, HE::Net::CollabSession::AssetOp::Edit, relPath });
	const HE::Net::LockInfo* l = assetLockInfo(relPath);
	m_assetNotice = "Asked " +
		(l && !l->ownerName.empty() ? l->ownerName : std::string("the person editing it")) +
		" for \"" + std::filesystem::path(relPath).filename().string() + "\".";
	return true;
}

void CollabController::answerEditRequest(std::size_t index, bool allowed)
{
	if (!m_collab || index >= m_editRequests.size()) return;
	const EditRequest e = m_editRequests[index];
	m_editRequests.erase(m_editRequests.begin() + static_cast<std::ptrdiff_t>(index));

	// The lock moves inside sendAssetEditAnswer (host) or on the host's side of
	// it (client) — released and granted together, so no third peer can slip
	// into the gap. Our own m_heldAssetLocks entry is dropped by the lock
	// broadcast that comes back, which is the same signal every other peer gets.
	m_collab->sendAssetEditAnswer(e.id, e.requestId, e.path, allowed,
	                              assetSubject(e.path));
}

void CollabController::approveOne(const PendingAssetOp& op)
{
	if (!m_collab || !isHost()) return;

	// A rename onto a name that is already taken HERE. The host's disk is the
	// arbiter for creates for the same reason it is for this: approving would
	// broadcast a rename that replaces a file — on every machine at once, with
	// no undo behind it. Refused rather than applied, and the requester is told
	// the same way any refusal reaches them.
	//
	// Inside approveOne rather than at the button, so it also catches a rename
	// that was approved as part of a bundle: a whole-selection yes must not be
	// able to do what the same yes on one row refuses to.
	if (op.op == HE::Net::CollabSession::AssetOp::Rename && m_localPathForKey &&
	    !op.newPath.empty())
	{
		const std::string from = m_localPathForKey(op.path);
		const std::string to   = m_localPathForKey(op.newPath);
		std::error_code   ec;
		if (to.empty() ||
		    (std::filesystem::exists(to, ec) &&
		     !(!from.empty() && std::filesystem::equivalent(from, to, ec))))
		{
			denyOne(op);
			m_assetNotice = "\"" +
				std::filesystem::path(op.newPath).filename().string() +
				"\" already exists — the rename was refused.";
			postNote(HE::Ed::NoteLevel::Warning,
			         "\"" + std::filesystem::path(op.newPath).filename().string() +
			             "\" already exists here — the rename was refused.",
			         "Approving it would have replaced that file on every machine.",
			         op.newPath);
			return;
		}
	}

	// Everyone who asked hears yes — including the ones who asked second, whose
	// request is answered by this same decision.
	for (const auto& rq : op.requesters)
		m_collab->sendAssetOpVerdict(rq.id, rq.requestId, true);
	// Applied through the broadcast, which also fires locally: one path for
	// "this happened", rather than a local branch that has to stay in step.
	m_collab->broadcastAssetOpApply(op.op, op.path, op.newPath,
	                                op.requesters.empty() ? localParticipant()
	                                                      : op.requesters.front().id,
	                                op.folder);
}

void CollabController::denyOne(const PendingAssetOp& op)
{
	if (!m_collab || !isHost()) return;
	// Told, not silently dropped: a request that simply vanishes reads as a bug,
	// and the requester would ask again.
	for (const auto& rq : op.requesters)
		m_collab->sendAssetOpVerdict(rq.id, rq.requestId, false);
}

void CollabController::approveAssetOp(std::size_t index)
{
	if (!m_collab || !isHost() || index >= m_pendingOps.size()) return;
	// Copied and erased BEFORE it is answered: approving broadcasts the apply,
	// which fires our own callbacks, and a row still sitting in the vector while
	// that happens is a row that can be drawn or answered twice.
	const PendingAssetOp op = m_pendingOps[index];
	m_pendingOps.erase(m_pendingOps.begin() + static_cast<std::ptrdiff_t>(index));
	approveOne(op);
}

void CollabController::denyAssetOp(std::size_t index)
{
	if (!m_collab || !isHost() || index >= m_pendingOps.size()) return;
	const PendingAssetOp op = m_pendingOps[index];
	m_pendingOps.erase(m_pendingOps.begin() + static_cast<std::ptrdiff_t>(index));
	denyOne(op);
}

std::size_t CollabController::approveAssetOpBatch(HE::Net::ParticipantId owner,
                                                  std::uint32_t batch)
{
	if (!m_collab || !isHost() || batch == 0) return 0;
	// Taken out whole first, then answered. Answering row by row through the
	// index-based calls would work on a vector that each answer shortens, so
	// every index after the first names a different row than the caller meant —
	// and approving twenty deletes would delete the wrong files.
	std::vector<PendingAssetOp> taken;
	for (auto it = m_pendingOps.begin(); it != m_pendingOps.end();)
	{
		if (it->batchId == batch && it->batchOwner == owner)
		{
			taken.push_back(*it);
			it = m_pendingOps.erase(it);
		}
		else ++it;
	}
	for (const PendingAssetOp& op : taken) approveOne(op);
	return taken.size();
}

std::size_t CollabController::denyAssetOpBatch(HE::Net::ParticipantId owner,
                                               std::uint32_t batch)
{
	if (!m_collab || !isHost() || batch == 0) return 0;
	std::vector<PendingAssetOp> taken;
	for (auto it = m_pendingOps.begin(); it != m_pendingOps.end();)
	{
		if (it->batchId == batch && it->batchOwner == owner)
		{
			taken.push_back(*it);
			it = m_pendingOps.erase(it);
		}
		else ++it;
	}
	for (const PendingAssetOp& op : taken) denyOne(op);
	return taken.size();
}

std::uint32_t CollabController::beginAssetOpBatch()
{
	// Never 0 even after four billion batches: 0 is the "on its own" marker, and
	// wrapping onto it would silently take a selection apart into single rows.
	if (m_nextBatchId == 0) m_nextBatchId = 1;
	m_openBatch = m_nextBatchId++;
	return m_openBatch;
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

	// WHICH create is this the answer to? More than one can be in flight — a C++
	// class publishes its .h and its .cpp in the same frame — and the verdicts
	// come back one at a time. Keyed lookup, because a single "the pending one"
	// slot answered the first verdict by renaming the second one's file: the
	// .cpp ended up called Foo2.h, holding implementation text, while Foo.h
	// still declared a class with nothing behind it.
	const auto it = m_pendingCreates.find(path);
	if (it == m_pendingCreates.end()) return;   // not ours, or already settled
	const std::string fullPath = it->second.fullPath;
	const bool        retried  = it->second.retried;
	m_pendingCreates.erase(it);

	// A peer's create for this same path has been held back waiting for exactly
	// this answer. Every exit below IS the answer, including the one in the
	// middle of the retry — so it is released on the way out rather than at
	// three separate returns that a fourth would quietly not join.
	struct FlushOnExit
	{
		CollabController*  self;
		const std::string& path;
		~FlushOnExit() { self->flushDeferredCreate(path); }
	} flushOnExit{ this, path };

	if (accepted) return;

	// The asset exists on this machine either way — the file was written before
	// it was ever announced. What differs is whether anyone else will see it.
	if (reason == Reason::NameTaken && !suggestedPath.empty() &&
	    !retried && !fullPath.empty() && m_localPathForKey)
	{
		const std::string newFull = m_localPathForKey(suggestedPath);
		std::error_code ec;
		if (!newFull.empty() && !std::filesystem::exists(newFull, ec))
		{
			std::filesystem::rename(fullPath, newFull, ec);
			if (!ec)
			{
				Logger::Log(Logger::LogLevel::Info,
					("Collab: that name was taken — the asset is now \"" +
					 suggestedPath + "\"").c_str());
				m_assetNotice = "That name was already taken. Your asset is now \"" +
				                suggestedPath + "\".";
				publishAssetCreate(suggestedPath, newFull);
				// Carried onto the retry, so a name that is taken twice running
				// stops here instead of chasing free names in a loop.
				if (const auto n = m_pendingCreates.find(suggestedPath);
				    n != m_pendingCreates.end())
				{
					n->second.retried = true;
				}
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
}

void CollabController::flushDeferredCreate(const std::string& path)
{
	for (auto it = m_deferredCreates.begin(); it != m_deferredCreates.end(); )
	{
		if (it->path != path) { ++it; continue; }
		const DeferredCreate d = std::move(*it);
		it = m_deferredCreates.erase(it);
		if (!m_onRemoteAsset) continue;
		// By now our own create for this path has been answered: either we lost
		// and moved our file to the suggested name, or it was refused and the
		// path is not ours to keep. Writing the winner's bytes here no longer
		// destroys anything.
		m_applyingRemoteAsset = true;
		m_onRemoteAsset(d.path, d.bytes);
		m_applyingRemoteAsset = false;
		noteAssetCreated(d.who, d.path);
	}
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
	// Same type gate as a save — one helper, so a create and a save can never
	// disagree about whether a kind of asset belongs in this session — and with
	// the same exception: a C++ source is raw text with no HAsset header, so
	// sniffing it reads Unknown and would drop it. Its own key prefix is the
	// admission ticket.
	if (rel.rfind("::Source::", 0) != 0 &&
	    !assetTypeTravels(EditorAssetTypeCache::assetTypeOf(fullPath))) return;

	std::ifstream f(fullPath, std::ios::binary);
	if (!f) return;
	std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)),
	                                 std::istreambuf_iterator<char>());
	if (bytes.empty()) return;

	// Deliberately NO lock dance, unlike publishAsset: nobody can hold a lock on
	// an asset that does not exist yet, and asking for one would race every
	// other peer creating at the same path. The host settles the name instead,
	// and grants the lock as part of accepting.
	// Remembered UNDER ITS KEY so the host's answer can be matched to the right
	// file: a refusal because the name was taken comes back with a free one, and
	// renaming the file we just wrote is only possible if we know which one the
	// verdict is about.
	m_pendingCreates[rel].fullPath = fullPath;

	HE::Net::CollabSession::AssetUpdate a;
	a.subject = assetSubject(rel);
	a.path    = rel;
	const std::size_t size = bytes.size();
	a.bytes   = std::move(bytes);
	a.intent  = HE::Net::CollabSession::AssetIntent::Create;
	// The same ceiling refuses a create, and a create is the worse one to lose
	// quietly: a save that does not travel leaves the others with an older
	// version of something they have, while this leaves them with no asset at all
	// and nothing on screen to suggest one was ever made.
	if (!m_collab->sendAsset(a))
	{
		if (size > m_collab->maxAssetBytes())
			noteAssetTooLarge(rel, size, /*sending=*/true);
		// Nothing went out, so no verdict is coming. Left behind, the entry waits
		// for an answer for ever AND holds back any create the others make on the
		// same path — see the deferred-create branch in wireCallbacks, which
		// parks a remote create precisely while one of ours is still open.
		m_pendingCreates.erase(rel);
		return;
	}

	// The host answers its OWN create immediately — no round trip, so nothing is
	// pending and an entry left behind would only wait for a verdict that is
	// never sent.
	if (isHost()) m_pendingCreates.erase(rel);
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
