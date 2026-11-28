#include "HorizonScene/LandscapeStamps.h"
#include "HorizonScene/Components/LandscapeStampComponent.h"
#include "HorizonScene/Components/TransformComponent.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/LandscapeRuntime.h"
#include "HorizonScene/TerrainHeightmap.h"

#include <filesystem>
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
bool stampHasSource(const LandscapeStampComponent& sc) { return sc.enabled && (!sc.heightmapFile.empty() || sc.textureId != HE::UUID{}); }

// What the stamp's picture is read from: the file (relative paths from the project's folder, the content root's parent), else the texture.
std::string stampSourceKey(const LandscapeStampComponent& sc, const ContentManager& content)
{
	if (sc.heightmapFile.empty()) return "tex:" + std::to_string(sc.textureId.hi) + ":" + std::to_string(sc.textureId.lo);
	std::filesystem::path p(sc.heightmapFile);
	if (p.is_relative() && !content.contentRoot().empty()) p = std::filesystem::path(content.contentRoot()).parent_path() / p;
	return p.string();
}
} // namespace

std::vector<LandscapeStamp> landscapeStampsOfWorld(HorizonWorld& world, ContentManager& content, LandscapeStampImages& images)
{
	std::vector<LandscapeStamp> out;
	for (auto [e, tf, sc] : world.registry().view<TransformComponent, LandscapeStampComponent>().each())
	{
		if (!stampHasSource(sc)) continue;
		const std::string source = stampSourceKey(sc, content);
		const int channel = std::clamp(sc.channel, 0, 4);
		const std::string key = source + "|" + std::to_string(channel);   // one picture per source AND channel
		auto it = images.find(key);
		if (it == images.end())
		{
			LandscapeStampImage made;
			if (!sc.heightmapFile.empty())
			{
				// A file: 16 bits a pixel (65 536 height steps), which a texture asset cannot hold.
				std::vector<uint16_t> grey;
				uint32_t w = 0, h = 0;
				std::string error;
				if (TerrainHeightmap::readGrey16(source, grey, w, h, error, channel == 4 ? -1 : channel) && w >= 1 && h >= 1)
				{
					auto img = std::make_shared<std::vector<float>>(grey.size());
					for (size_t i = 0; i < grey.size(); ++i) (*img)[i] = static_cast<float>(grey[i]) / 65535.0f;
					made = { std::move(img), static_cast<int>(w), static_cast<int>(h) };
				}
				else
					HE_LOG_WARN(Scene, "Landscape stamp: heightmap file '%s' cannot be read (%s); the stamp does nothing", source.c_str(), error.c_str());
			}
			else
			{
				content.ensureResident(sc.textureId);
				const TextureAsset* t = content.getTexture(sc.textureId);
				// 8 bits a channel: 256 height steps, smoothed by the bilinear read. The file above is the way to more.
				if (t && t->format == ::TextureFormat::RGBA8 && t->channels >= 1 && t->width >= 1 && t->height >= 1
				    && t->data.size() >= static_cast<size_t>(t->width) * t->height * t->channels)
				{
					auto img = std::make_shared<std::vector<float>>(static_cast<size_t>(t->width) * t->height);
					const size_t C = t->channels;
					for (size_t i = 0; i < img->size(); ++i)
					{
						const uint8_t* p = &t->data[i * C];
						float v;
						if (channel == 4) v = C >= 3 ? 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2] : static_cast<float>(p[0]);   // the grey value
						else if (channel == 3) v = (C == 2 || C == 4) ? static_cast<float>(p[C - 1]) : 255.0f;                        // no alpha: white
						else v = static_cast<float>(p[C >= 3 ? static_cast<size_t>(channel) : 0]);                                    // a grey texture: its grey
						(*img)[i] = v / 255.0f;
					}
					made = { std::move(img), static_cast<int>(t->width), static_cast<int>(t->height) };
				}
				else
					HE_LOG_WARN(Scene, "Landscape stamp: its texture is missing or not stored as plain 8-bit pixels; the stamp does nothing");
			}
			it = images.emplace(key, std::move(made)).first;   // a null stays: not tried again every change
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
		if (!stampHasSource(sc)) continue;
		const Placed p = place(world, e, sc);
		mix(&p.x, sizeof(p.x)); mix(&p.z, sizeof(p.z));
		mix(sc.heightmapFile.data(), sc.heightmapFile.size());
		mix(&sc.channel, sizeof(sc.channel));
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
			runtime.coarseChanged(&m_pendingCoarse);   // also: what was painted from the far data of these tiles is stale
			m_pendingCoarse.clear();
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
