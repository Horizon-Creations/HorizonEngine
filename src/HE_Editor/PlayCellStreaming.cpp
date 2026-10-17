#include "PlayCellStreaming.h"
#include <ContentManager/ContentManager.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSystems.h>
#include <fstream>
#include <iterator>

PlayCellStreaming::PlayCellStreaming(ContentManager& content) : m_content(content) {}

void PlayCellStreaming::begin(std::function<PhysicsWorld*()> physics, std::filesystem::path root)
{
	m_runtime.reset();
	m_physics = std::move(physics);
	m_root    = std::move(root);
	m_playing = true;
}

void PlayCellStreaming::update(HorizonWorld& world, float dt)
{
	if (m_playing) m_runtime.update(world, dt);
}

void PlayCellStreaming::end(HorizonWorld& world)
{
	m_runtime.clear(world);
	m_physics = {};
	m_root.clear();
	m_playing = false;
}

std::function<bool(std::vector<uint8_t>&)> PlayCellStreaming::openCellFile(const std::string& path)
{
	// The project folder, as the Streaming tab's split wrote them (projectRootOf
	// there): the folder above the content root.
	std::filesystem::path root = m_root;
	if (root.empty() && !m_content.contentRoot().empty())
		root = std::filesystem::path(m_content.contentRoot()).parent_path();
	if (root.empty()) return {};
	const std::filesystem::path file = root / path;
	std::error_code ec;
	if (!std::filesystem::exists(file, ec)) return {};
	return [file](std::vector<uint8_t>& out)
	{
		std::ifstream in(file, std::ios::binary);
		out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		return !out.empty();
	};
}

void PlayCellStreaming::streamCellAssets(HorizonWorld& world, const HE::CancelToken& token,
                                         const std::vector<uint32_t>& entities)
{
	// The game's streamSceneAssets for a slice; the editor's per-frame drain
	// (pollAsyncResults in EditorApplication's update) registers what arrives.
	HE::AsyncLoadOptions options;   // Low: never ahead of the frame's own work
	options.cancel = token;
	for (HE::UUID r : SceneSystems::collectAssetRefs(world, entities))
		m_content.loadAssetAsync(r, {}, options);
}
