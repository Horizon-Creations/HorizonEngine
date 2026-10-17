#pragma once
#include <HorizonScene/CellRuntime.h>
#include <HorizonScene/CellScripts.h>
#include <filesystem>
#include <functional>

class HorizonWorld;
class PhysicsWorld;
class ContentManager;

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
	explicit PlayCellStreaming(ContentManager& content);

	// `physics` is asked at every call: the play session owns it, and it is gone
	// after Stop. `root` overrides the project folder the manifest's paths are
	// relative to (a headless witness that writes its cells elsewhere); empty =
	// the open project's.
	void begin(std::function<PhysicsWorld*()> physics, std::filesystem::path root = {});
	// How the editor builds a script API Ctx for the play world (its apiCtx), so
	// that a cell's Save Game variables go through the play session's script
	// runtime (CellState). Unset: transform and visibility only.
	void setApiCtx(std::function<void(HE::api::Ctx&)> fill) { m_apiCtx = std::move(fill); }
	// The play session's script hosts, asked at every cell that is built or goes
	// (the script context is made after begin() and reset after end()). Unset: a
	// cell's scripts are neither started nor stopped.
	void setScripts(std::function<HE::CellScriptHosts()> hosts) { m_scripts = std::move(hosts); }
	// The editor's multiplayer session (Play as Host): while it hosts, every
	// player's character is an anchor (HE::sessionPlayers). Null: the camera and
	// the pins only.
	void setSession(NetGameSession* session) { m_session = session; }
	void update(HorizonWorld& world, float dt);
	// The cells out of `world` (their bodies out of the physics world, their loads
	// cancelled), and nothing streams until the next begin().
	void end(HorizonWorld& world);

	bool                    playing() const { return m_playing; }
	const HE::CellRuntime&  runtime() const { return m_runtime; }
	HE::CellRuntime&        runtime() { return m_runtime; }

	// ICellHost
	std::function<bool(std::vector<uint8_t>&)> openCellFile(const std::string& path) override;
	PhysicsWorld* cellPhysics() override { return m_physics ? m_physics() : nullptr; }
	void streamCellAssets(HorizonWorld& world, const HE::CancelToken& token,
	                      const std::vector<uint32_t>& entities) override;
	void cellApiCtx(HE::api::Ctx& ctx) override { if (m_apiCtx) m_apiCtx(ctx); }
	void cellLoaded(HorizonWorld& world, entt::entity root, const std::vector<entt::entity>& created) override;
	void cellUnloading(HorizonWorld& world, entt::entity root) override;
	void cellPlayers(HorizonWorld& world, std::vector<HE::CellPlayer>& out) override
	{
		HE::sessionPlayers(m_session, world, out);
	}

private:
	ContentManager&                m_content;
	HE::CellRuntime                m_runtime{ *this };
	std::function<PhysicsWorld*()> m_physics;
	std::function<void(HE::api::Ctx&)> m_apiCtx;
	std::function<HE::CellScriptHosts()> m_scripts;
	NetGameSession*                m_session = nullptr;
	std::filesystem::path          m_root;
	bool                           m_playing = false;
};
