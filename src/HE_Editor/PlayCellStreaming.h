#pragma once
#include <HorizonScene/CellRuntime.h>
#include <filesystem>
#include <functional>

class HorizonWorld;
class PhysicsWorld;
namespace HE { class ContentManager; }

// ── Streaming cells while the editor plays (Thema 164, step 2c) ─────────────
// Play runs in the editor's own world, which after a split holds only the base
// scene. This is the editor's ICellHost: the same HE::CellRuntime the packaged
// game runs, fed from the cell files in the project folder (the editor mounts no
// pak), with the play session's physics world and the editor's asset drain. One
// per editor; begin() at Play, update() once a play frame after the camera,
// end() at Stop BEFORE the edited scene is put back.
class PlayCellStreaming final : public HE::ICellHost
{
public:
	explicit PlayCellStreaming(HE::ContentManager& content);

	// `physics` is asked at every call: the play session owns it, and it is gone
	// after Stop. `root` overrides the project folder the manifest's paths are
	// relative to (a headless witness that writes its cells elsewhere); empty =
	// the open project's.
	void begin(std::function<PhysicsWorld*()> physics, std::filesystem::path root = {});
	void update(HorizonWorld& world, float dt);
	// The cells out of `world` (their bodies out of the physics world, their loads
	// cancelled), and nothing streams until the next begin().
	void end(HorizonWorld& world);

	bool                    playing() const { return m_playing; }
	const HE::CellRuntime&  runtime() const { return m_runtime; }

	// ICellHost
	std::function<bool(std::vector<uint8_t>&)> openCellFile(const std::string& path) override;
	PhysicsWorld* cellPhysics() override { return m_physics ? m_physics() : nullptr; }
	void streamCellAssets(HorizonWorld& world, const HE::CancelToken& token,
	                      const std::vector<uint32_t>& entities) override;

private:
	HE::ContentManager&            m_content;
	HE::CellRuntime                m_runtime{ *this };
	std::function<PhysicsWorld*()> m_physics;
	std::filesystem::path          m_root;
	bool                           m_playing = false;
};
