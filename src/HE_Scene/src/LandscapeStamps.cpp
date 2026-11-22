#include "HorizonScene/LandscapeStamps.h"
#include "HorizonScene/Components/LandscapeStampComponent.h"
#include "HorizonScene/Components/TransformComponent.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/LandscapeRuntime.h"
#include "HorizonScene/TransformHierarchy.h"

#include <ContentManager/ContentManager.h>
#include <Diagnostics/Log.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace HE
{

namespace
{
// Where a stamp entity puts its stamp: everything but the picture.
struct Placed
{
	double x = 0.0, z = 0.0;
	float  sizeX = 0.0f, sizeZ = 0.0f, yaw = 0.0f, heightM = 0.0f, falloff = 0.0f;
};

Placed place(HorizonWorld& world, entt::entity e, const LandscapeStampComponent& sc)
{
	const glm::mat4 m = HE::worldMatrixOf(world, e);
	const glm::vec3 ax(m[0]), ay(m[1]), az(m[2]);
	Placed p;
	p.x = static_cast<double>(m[3].x) + world.origin().x;
	p.z = static_cast<double>(m[3].z) + world.origin().z;
	p.sizeX = sc.sizeM * glm::length(ax);
	p.sizeZ = sc.sizeM * glm::length(az);
	p.heightM = sc.heightM * glm::length(ay);
	p.yaw = std::atan2(-ax.z, ax.x);   // a turn around Y puts local +X at (cos, 0, -sin)
	p.falloff = std::clamp(sc.falloff, 0.0f, 1.0f);
	return p;
}
} // namespace

std::vector<LandscapeStamp> landscapeStampsOfWorld(HorizonWorld& world, ContentManager& content, LandscapeStampImages& images)
{
	std::vector<LandscapeStamp> out;
	for (auto [e, tf, sc] : world.registry().view<TransformComponent, LandscapeStampComponent>().each())
	{
		if (!sc.enabled || sc.textureId == HE::UUID{}) continue;
		auto it = images.find(sc.textureId);
		if (it == images.end())
		{
			LandscapeStampImage made;
			content.ensureResident(sc.textureId);
			const TextureAsset* t = content.getTexture(sc.textureId);
			// shortcut: 8 bits a channel (256 height steps, smoothed by the bilinear read), take 16-bit sources when texture assets hold them
			if (t && t->format == ::TextureFormat::RGBA8 && t->channels >= 1 && t->width >= 1 && t->height >= 1
			    && t->data.size() >= static_cast<size_t>(t->width) * t->height * t->channels)
			{
				auto img = std::make_shared<std::vector<float>>(static_cast<size_t>(t->width) * t->height);
				for (size_t i = 0; i < img->size(); ++i) (*img)[i] = static_cast<float>(t->data[i * t->channels]) / 255.0f;
				made = { std::move(img), static_cast<int>(t->width), static_cast<int>(t->height) };
			}
			else
				HE_LOG_WARN(Scene, "Landscape stamp: its texture is missing or not stored as plain 8-bit pixels; the stamp does nothing");
			it = images.emplace(sc.textureId, std::move(made)).first;   // a null stays: not tried again every change
		}
		if (!it->second.values) continue;
		const Placed p = place(world, e, sc);
		LandscapeStamp s;
		s.image = it->second.values;
		s.w = it->second.w;
		s.h = it->second.h;
		s.x = p.x; s.z = p.z;
		s.sizeX = p.sizeX; s.sizeZ = p.sizeZ;
		s.yaw = p.yaw;
		s.heightM = p.heightM;
		s.falloff = p.falloff;
		out.push_back(std::move(s));
	}
	return out;
}

size_t LandscapeStampSync::sync(HorizonWorld& world, LandscapeRuntime& runtime, ContentManager& content)
{
	if (!runtime.active()) return 0;
	// The idle path: fold what places every stamp into one number and compare.
	uint64_t key = 1469598103934665603ull;
	const auto mix = [&](const void* p, size_t n) {
		for (size_t i = 0; i < n; ++i) key = (key ^ static_cast<const uint8_t*>(p)[i]) * 1099511628211ull;
	};
	size_t count = 0;
	for (auto [e, tf, sc] : world.registry().view<TransformComponent, LandscapeStampComponent>().each())
	{
		if (!sc.enabled || sc.textureId == HE::UUID{}) continue;
		const Placed p = place(world, e, sc);
		mix(&p.x, sizeof(p.x)); mix(&p.z, sizeof(p.z));
		const float f[5] = { p.sizeX, p.sizeZ, p.yaw, p.heightM, p.falloff };
		mix(f, sizeof(f));
		mix(&sc.textureId, sizeof(sc.textureId));
		++count;
	}
	LandscapeEditSource* edits = runtime.edits();
	// A runtime that began again (another scene, a rebuilt pack) has a new overlay without the stamps.
	const bool sameOverlay = edits == m_overlay && (!edits || edits->stampCount() == m_applied);
	if (key == m_key && sameOverlay)
	{
		// The stamps have come to rest: the far levels take the tiles they changed (too slow for every step of a drag).
		if (!m_pendingCoarse.empty() && edits && std::chrono::steady_clock::now() - m_last >= std::chrono::milliseconds(300))
		{
			std::sort(m_pendingCoarse.begin(), m_pendingCoarse.end(), [](const LandscapeTileKey& a, const LandscapeTileKey& b) { return a.x != b.x ? a.x < b.x : a.z < b.z; });
			m_pendingCoarse.erase(std::unique(m_pendingCoarse.begin(), m_pendingCoarse.end()), m_pendingCoarse.end());
			edits->patchCoarseFor(m_pendingCoarse);
			m_pendingCoarse.clear();
			runtime.coarseChanged();
		}
		return 0;
	}
	if (!sameOverlay) m_pendingCoarse.clear();

	if (!edits)
	{
		if (count == 0) { m_key = key; return 0; }
		// A runtime without an overlay (a game on a pack with no edits file): it begins again with one, and the next call lays the stamps on.
		runtime.setEditable(true);
		runtime.reset();
		return 0;
	}
	// A stamp that is being dragged changes every frame: its tiles are read again at most ten times a second.
	const auto now = std::chrono::steady_clock::now();
	if (sameOverlay && m_key != 0 && now - m_last < std::chrono::milliseconds(100)) return 0;

	const std::vector<LandscapeTileKey> changed = edits->setStamps(landscapeStampsOfWorld(world, content, m_images), runtime.config().heightScaleM, /*patchCoarse=*/false);
	if (!changed.empty())
	{
		runtime.tileChanged(changed);   // read again on the job pool, with the stamps over them
		m_pendingCoarse.insert(m_pendingCoarse.end(), changed.begin(), changed.end());
	}
	m_key = key;
	m_last = now;
	m_overlay = edits;
	m_applied = edits->stampCount();
	return changed.size();
}

} // namespace HE
