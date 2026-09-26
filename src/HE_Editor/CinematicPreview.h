#pragma once
#include <ContentManager/Assets.h>
#include <HorizonScene/SequenceEval.h>

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <vector>

class HorizonWorld;
class ContentManager;

// ── The Cinematic tab's preview: write, look, put back ───────────────────────
// A preview that writes into the edit world and cleans up "later" has to find
// every later there is — save, play-in-editor, an undo snapshot, the autosave,
// a collaboration peer reading the world — and one it misses saves a cutscene
// frame into the level. So this preview never leaves the world changed between
// two calls: apply() saves what the sequence at `t` is about to write and
// writes it, the caller renders (IRenderer::RenderWorldPreview, which is a
// synchronous round trip and extracts the world inside the call), and
// restore() puts every value back before anything else runs. The asset tab
// replaces the scene viewport while it is in front anyway, so the picture has
// to come from the tab's own render; nothing else would see the written state.
//
// What is saved, because it is what the sequence writes (SequenceEval::apply,
// PropertyAnimationSystem::applyChannel, and the skeletal sections):
//   * each bound actor's Transform (whole; restored dirty, then the world is
//     re-propagated so no worldMatrix — the actor's or a child's — keeps the
//     preview's pose), its Camera, the `visible` flag of each renderable, and a
//     skinned actor's bone matrices;
//   * each MATERIAL a material track touches, once per material asset and not
//     per entity — the asset is shared — with every MaterialComponent that
//     uses it marked dirty afterwards, so a cached GPU copy is refreshed.
//
// Events and sound never run here: the preview is a scrub, and a scrub fires
// nothing (plan §3.1). Skeletal sections are posed from their clip alone
// (AnimationPreview::evaluateClipPose) — no layers, no IK, no notifies, and the
// runtime's sequencePosed claim is never set.
namespace HE::Ed::CinematicPreview
{
	// Where the view through the sequence's live camera is at `t`: the cut's
	// camera, or the blend into it from the previous one (or from the scene's
	// main camera for the first cut) — the same mix/slerp the runtime blends
	// with. `valid` false when no cut is live.
	struct CameraView
	{
		bool      valid = false;
		glm::vec3 position{ 0.0f };
		glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
		float     fovDegrees = 60.0f;
		float     nearPlane  = 0.1f;
		float     farPlane   = 1000.0f;
		entt::entity camera  = entt::null;   // the live cut's camera
	};

	class Bracket
	{
	public:
		// Save, then write the sequence at `t`. A second apply() without a
		// restore() in between restores first: the save is always of the
		// world as the author left it.
		void apply(HorizonWorld& world, ContentManager& cm, const SequenceAsset& seq, float t);
		// Every saved value back, the transforms re-propagated. Safe to call
		// when nothing is applied. Entities destroyed meanwhile are skipped.
		void restore(HorizonWorld& world, ContentManager& cm);
		bool applied() const { return m_applied; }

		// Valid between apply() and restore(): the view through the live cut.
		CameraView cameraView(HorizonWorld& world) const;
		// The resolved actors of the last apply(), index = slot.
		const std::vector<entt::entity>& slots() const { return m_slots; }
		const HE::SequenceEval::CameraState& camera() const { return m_camera; }

	private:
		struct Saved;
		struct SavedMaterial;
		std::vector<Saved>         m_saved;
		std::vector<SavedMaterial> m_materials;
		std::vector<entt::entity>  m_slots;
		HE::SequenceEval::CameraState m_camera;
		// The scene's main camera's world pose at apply(), before anything was
		// written: where a blend into the first cut starts, like the frozen
		// gameplay pose at runtime.
		CameraView m_gameplay;
		bool m_applied = false;

	public:
		Bracket();
		~Bracket();
		Bracket(const Bracket&) = delete;
		Bracket& operator=(const Bracket&) = delete;
	};
}
