#include "CinematicPreview.h"

#include <ContentManager/ContentManager.h>
#include <HorizonScene/AnimationPreview.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/PropertyAnimationSystem.h>
#include <HorizonScene/SequenceSystem.h>      // activeSection, sectionClipTime — the runtime's section rule
#include <HorizonScene/TransformHierarchy.h>  // worldMatrixOf, propagateTransforms
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/LightComponent.h>
#include <HorizonScene/Components/MaterialComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/ParticleSystemComponent.h>
#include <HorizonScene/Components/RopeComponent.h>
#include <HorizonScene/Components/SkeletalMeshComponent.h>
#include <HorizonScene/Components/TrailComponent.h>
#include <HorizonScene/Components/TransformComponent.h>

#include <algorithm>

namespace HE::Ed::CinematicPreview
{

// -1: the entity has no such component; else the flag as it was.
struct Bracket::Saved
{
	entt::entity e = entt::null;
	bool hasTransform = false;
	TransformComponent transform;
	bool hasCamera = false;
	CameraComponent camera;
	int8_t mesh = -1, skinned = -1, light = -1, particles = -1, rope = -1, trail = -1;
	bool hasBones = false;
	std::vector<glm::mat4> bones;
};

struct Bracket::SavedMaterial
{
	HE::UUID id;
	float baseColor[3] = { 1.0f, 1.0f, 1.0f };
	float metallic = 0.0f, roughness = 0.5f, opacity = 1.0f;
};

Bracket::Bracket()  = default;
Bracket::~Bracket() = default;

namespace
{
	template <class C>
	int8_t visibleOf(const entt::registry& reg, entt::entity e)
	{
		const C* c = reg.try_get<C>(e);
		return c ? int8_t(c->visible ? 1 : 0) : int8_t(-1);
	}
	template <class C>
	void putVisible(entt::registry& reg, entt::entity e, int8_t v)
	{
		if (v < 0) return;
		if (C* c = reg.try_get<C>(e)) c->visible = v != 0;
	}

	bool isMaterialTarget(PropTarget t)
	{
		switch (t)
		{
		case PropTarget::MatColorR: case PropTarget::MatColorG: case PropTarget::MatColorB:
		case PropTarget::MatMetallic: case PropTarget::MatRoughness: case PropTarget::MatOpacity:
			return true;
		default:
			return false;
		}
	}

	// A camera's world pose now, composed from its parent chain — the preview
	// has just moved it and nothing has propagated.
	CameraView poseOf(HorizonWorld& world, entt::entity e)
	{
		CameraView v;
		auto& reg = world.registry();
		if (e == entt::null || !reg.valid(e) || !reg.all_of<CameraComponent, TransformComponent>(e)) return v;
		const glm::mat4 m = HE::worldMatrixOf(world, e);
		v.position = glm::vec3(m[3]);
		glm::mat3 basis(m);
		for (int i = 0; i < 3; ++i)
		{
			const float len = glm::length(basis[i]);
			if (len > 1e-6f) basis[i] /= len;
		}
		v.rotation = glm::normalize(glm::quat_cast(basis));
		const CameraComponent& cam = reg.get<CameraComponent>(e);
		v.fovDegrees = cam.fovDegrees + cam.fovOffset;
		v.nearPlane  = cam.nearPlane;
		v.farPlane   = cam.farPlane;
		v.camera     = e;
		v.valid      = true;
		return v;
	}

	// The camera on screen in the scene: isMain, else the first — the
	// renderer's rule, and the runtime's "gameplay camera".
	entt::entity shownCamera(entt::registry& reg)
	{
		entt::entity found = entt::null;
		for (auto [e, cam] : reg.view<CameraComponent>().each())
		{
			if (found == entt::null) found = e;
			if (cam.isMain) return e;
		}
		return found;
	}
}

void Bracket::apply(HorizonWorld& world, ContentManager& cm, const SequenceAsset& seq, float t)
{
	if (m_applied) restore(world, cm);
	auto& reg = world.registry();

	m_slots  = HE::SequenceEval::resolveBindings(world, seq);
	const HE::SequenceEval::Result frame = HE::SequenceEval::evaluate(seq, t);
	m_camera = frame.camera;
	m_gameplay = poseOf(world, shownCamera(reg));

	// ── Save ─────────────────────────────────────────────────────────────────
	for (entt::entity e : m_slots)
	{
		if (e == entt::null || !reg.valid(e)) continue;
		if (std::any_of(m_saved.begin(), m_saved.end(), [&](const Saved& s) { return s.e == e; })) continue;
		Saved s;
		s.e = e;
		if (const auto* tc = reg.try_get<TransformComponent>(e)) { s.hasTransform = true; s.transform = *tc; }
		if (const auto* cc = reg.try_get<CameraComponent>(e))    { s.hasCamera = true;    s.camera = *cc; }
		s.mesh      = visibleOf<MeshComponent>(reg, e);
		s.skinned   = visibleOf<SkeletalMeshComponent>(reg, e);
		s.light     = visibleOf<LightComponent>(reg, e);
		s.particles = visibleOf<ParticleSystemComponent>(reg, e);
		s.rope      = visibleOf<RopeComponent>(reg, e);
		s.trail     = visibleOf<TrailComponent>(reg, e);
		if (const auto* smc = reg.try_get<SkeletalMeshComponent>(e)) { s.hasBones = true; s.bones = smc->boneMatrices; }
		m_saved.push_back(std::move(s));
	}
	for (const HE::SequenceEval::PropertyWrite& w : frame.writes)
	{
		if (!isMaterialTarget(w.target) || w.slot >= m_slots.size()) continue;
		const entt::entity e = m_slots[w.slot];
		if (e == entt::null || !reg.valid(e)) continue;
		const auto* mc = reg.try_get<MaterialComponent>(e);
		if (!mc) continue;
		const HE::UUID id = mc->materialAssetId;
		if (std::any_of(m_materials.begin(), m_materials.end(), [&](const SavedMaterial& m) { return m.id == id; }))
			continue;
		const MaterialAsset* ma = cm.getMaterial(id);
		if (!ma) continue;
		SavedMaterial sm;
		sm.id = id;
		std::copy(ma->baseColor, ma->baseColor + 3, sm.baseColor);
		sm.metallic  = ma->metallic;
		sm.roughness = ma->roughness;
		sm.opacity   = ma->opacity;
		m_materials.push_back(sm);
	}
	m_applied = true;

	// ── Write: the runtime's own path for the properties ─────────────────────
	HE::SequenceEval::apply(world, cm, frame, m_slots);

	// ── Skeletons: the clip's pose alone ─────────────────────────────────────
	for (const SequenceTrack& tr : seq.tracks)
	{
		if (tr.kind != SequenceTrackKind::Skeletal || tr.binding >= m_slots.size()) continue;
		const entt::entity e = m_slots[tr.binding];
		if (e == entt::null || !reg.valid(e)) continue;
		auto* smc = reg.try_get<SkeletalMeshComponent>(e);
		const SequenceSkeletalSection* s = SequenceSystem::activeSection(tr, t);
		if (!smc || !s) continue;
		const AnimationClipAsset* clip = cm.getAnimationClip(s->clipId);
		const SkeletalMeshAsset*  mesh = cm.getSkeletalMesh(smc->meshAssetId);
		if (!clip || clip->duration <= 0.0f || !mesh || mesh->skeleton.empty()) continue;
		AnimationPreview::evaluateClipPose(*mesh, *clip,
			SequenceSystem::sectionClipTime(*s, t, clip->duration), smc->boneMatrices);
		smc->dirty = true;
	}
}

void Bracket::restore(HorizonWorld& world, ContentManager& cm)
{
	if (!m_applied) return;
	auto& reg = world.registry();
	for (const Saved& s : m_saved)
	{
		if (!reg.valid(s.e)) continue;
		if (s.hasTransform)
			if (auto* tc = reg.try_get<TransformComponent>(s.e)) { *tc = s.transform; tc->dirty = true; }
		if (s.hasCamera)
			if (auto* cc = reg.try_get<CameraComponent>(s.e)) *cc = s.camera;
		putVisible<MeshComponent>(reg, s.e, s.mesh);
		putVisible<SkeletalMeshComponent>(reg, s.e, s.skinned);
		putVisible<LightComponent>(reg, s.e, s.light);
		putVisible<ParticleSystemComponent>(reg, s.e, s.particles);
		putVisible<RopeComponent>(reg, s.e, s.rope);
		putVisible<TrailComponent>(reg, s.e, s.trail);
		if (s.hasBones)
			if (auto* smc = reg.try_get<SkeletalMeshComponent>(s.e)) { smc->boneMatrices = s.bones; smc->dirty = true; }
	}
	for (const SavedMaterial& sm : m_materials)
	{
		if (MaterialAsset* ma = cm.getMaterialMutable(sm.id))
		{
			std::copy(sm.baseColor, sm.baseColor + 3, ma->baseColor);
			ma->metallic  = sm.metallic;
			ma->roughness = sm.roughness;
			ma->opacity   = sm.opacity;
		}
		// Every user of the asset, not only the actor: they all drew with the
		// preview's values, and all of them hold a copy that has to refresh.
		for (auto [e, mc] : reg.view<MaterialComponent>().each())
			if (mc.materialAssetId == sm.id) mc.dirty = true;
	}
	// The extraction inside the render propagated the preview's pose into every
	// worldMatrix under the actors; this puts the author's back.
	HE::propagateTransforms(world);

	m_saved.clear();
	m_materials.clear();
	m_applied = false;
}

CameraView Bracket::cameraView(HorizonWorld& world) const
{
	if (!m_applied || !m_camera.active || m_camera.slot >= m_slots.size()) return {};
	CameraView live = poseOf(world, m_slots[m_camera.slot]);
	if (!live.valid || !m_camera.blending) return live;

	CameraView from;
	if (m_camera.fromGameplay) from = m_gameplay;
	else if (m_camera.fromSlot < m_slots.size()) from = poseOf(world, m_slots[m_camera.fromSlot]);
	if (!from.valid) return live;

	// The runtime's blend (SequenceSystem.cpp, cameraFrame): position mixed,
	// rotation slerped, FOV mixed, by the SHAPED alpha evaluate() gives.
	const float a = m_camera.alpha;
	CameraView v = live;
	v.position   = glm::mix(from.position, live.position, a);
	v.rotation   = glm::slerp(from.rotation, live.rotation, a);
	v.fovDegrees = glm::mix(from.fovDegrees, live.fovDegrees, a);
	return v;
}

} // namespace HE::Ed::CinematicPreview
