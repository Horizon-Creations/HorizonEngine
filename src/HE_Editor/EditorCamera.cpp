#include "EditorCamera.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace
{
	constexpr float kOrbitSpeed = 0.008f;   // radians per pixel
	constexpr float kLookSpeed  = 0.005f;    // radians per pixel
	constexpr float kPitchLimit = 1.55334f;  // ~89° in radians
	constexpr float kMinPivot   = 0.05f;
}

glm::vec3 EditorCamera::forward() const
{
	const float cp = std::cos(m_pitch), sp = std::sin(m_pitch);
	const float cy = std::cos(m_yaw),   sy = std::sin(m_yaw);
	// yaw=0, pitch=0 looks down -Z; +pitch looks up; +yaw turns right.
	return glm::normalize(glm::vec3(cp * sy, sp, -cp * cy));
}

glm::vec3 EditorCamera::upReference() const
{
	// Only the presets put the camera EXACTLY on a pole (update() clamps the
	// interactive pitch at ~89°). The threshold is on cos(pitch), i.e. on how
	// much of forward is left in the horizontal plane; below it the cross with
	// world-up is too short to normalise safely.
	if (std::abs(std::cos(m_pitch)) < 1e-4f)
		return glm::vec3(std::sin(m_yaw), 0.0f, -std::cos(m_yaw));
	return glm::vec3(0.0f, 1.0f, 0.0f);
}

glm::vec3 EditorCamera::right() const
{
	return glm::normalize(glm::cross(forward(), upReference()));
}

glm::vec3 EditorCamera::up() const
{
	return glm::normalize(glm::cross(right(), forward()));
}

const char* EditorCamera::presetName(ViewPreset p)
{
	switch (p)
	{
		case ViewPreset::Top:    return "Top";
		case ViewPreset::Bottom: return "Bottom";
		case ViewPreset::Front:  return "Front";
		case ViewPreset::Back:   return "Back";
		case ViewPreset::Left:   return "Left";
		case ViewPreset::Right:  return "Right";
		default:                 return "Perspective";
	}
}

namespace
{
	constexpr float kHalfPi = 1.57079632679f;

	// Heading of each axis view. Front looks along -Z (yaw 0, the direction the
	// camera faces before anyone touches it), Right sits on +X looking toward
	// -X, so forward = (sin yaw, 0, -cos yaw) gives yaw = -90° there.
	bool presetHeading(EditorCamera::ViewPreset p, float& yaw, float& pitch)
	{
		using VP = EditorCamera::ViewPreset;
		switch (p)
		{
			case VP::Top:    yaw = 0.0f;     pitch = -kHalfPi; return true;
			case VP::Bottom: yaw = 0.0f;     pitch =  kHalfPi; return true;
			case VP::Front:  yaw = 0.0f;     pitch = 0.0f;     return true;
			case VP::Back:   yaw = kHalfPi * 2.0f; pitch = 0.0f; return true;
			case VP::Right:  yaw = -kHalfPi; pitch = 0.0f;     return true;
			case VP::Left:   yaw =  kHalfPi; pitch = 0.0f;     return true;
			default: return false;
		}
	}
}

void EditorCamera::applyPreset(ViewPreset p)
{
	ensureInit();
	if (p == ViewPreset::Perspective)
	{
		m_orthographic = false;
		return;
	}
	float yaw = 0.0f, pitch = 0.0f;
	presetHeading(p, yaw, pitch);
	// Same rule as an orbit: the pivot stays put, the camera swings around it.
	const glm::vec3 pivot = m_position + forward() * m_pivotDistance;
	m_yaw   = yaw;
	m_pitch = pitch;
	m_position     = pivot - forward() * m_pivotDistance;
	m_orthographic = true;
}

EditorCamera::ViewPreset EditorCamera::currentPreset() const
{
	if (!m_orthographic) return ViewPreset::Perspective;
	// A preset lands EXACTLY on its heading; anything an orbit has moved off it
	// is a free ortho view — no preset, so Perspective is returned and the
	// caller tells the two apart through orthographic().
	constexpr float kTol = 1e-4f;
	const glm::vec3 f = forward();
	for (ViewPreset p : { ViewPreset::Top, ViewPreset::Bottom, ViewPreset::Front,
	                      ViewPreset::Back, ViewPreset::Left, ViewPreset::Right })
	{
		float yaw = 0.0f, pitch = 0.0f;
		presetHeading(p, yaw, pitch);
		const float cp = std::cos(pitch), sp = std::sin(pitch);
		const glm::vec3 pf(cp * std::sin(yaw), sp, -cp * std::cos(yaw));
		if (glm::length(pf - f) < kTol) return p;
	}
	return ViewPreset::Perspective;
}

float EditorCamera::orthoHalfHeight() const
{
	return std::max(kMinPivot, m_pivotDistance) * std::tan(glm::radians(m_fov * 0.5f));
}

void EditorCamera::ensureInit()
{
	// Derive yaw/pitch/distance from the spawn position once, so the camera
	// starts looking at the world origin regardless of where it sits.
	if (m_initialised) return;
	const glm::vec3 toTarget = glm::vec3(0.0f) - m_position;
	m_pivotDistance = std::max(kMinPivot, glm::length(toTarget));
	const glm::vec3 f = glm::normalize(toTarget);
	m_pitch = std::asin(glm::clamp(f.y, -1.0f, 1.0f));
	m_yaw   = std::atan2(f.x, -f.z);
	m_initialised = true;
}

void EditorCamera::setOrientation(const glm::vec3& pos, const glm::vec3& forwardDir)
{
	m_position = pos;
	const glm::vec3 f = glm::normalize(forwardDir);
	m_pitch = std::asin(glm::clamp(f.y, -1.0f, 1.0f));
	m_yaw   = std::atan2(f.x, -f.z);
	m_pivotDistance = std::max(kMinPivot, m_pivotDistance);
	m_initialised = true;
}

void EditorCamera::update(const Input& in)
{
	ensureInit();

	// ── Orbit: rotate around the pivot, keeping the pivot fixed ──────────────
	if (in.orbit && (in.mouseDelta.x != 0.0f || in.mouseDelta.y != 0.0f))
	{
		const glm::vec3 pivot = m_position + forward() * m_pivotDistance;
		m_yaw   += in.mouseDelta.x * kOrbitSpeed;
		m_pitch -= in.mouseDelta.y * kOrbitSpeed;
		m_pitch  = glm::clamp(m_pitch, -kPitchLimit, kPitchLimit);
		m_position = pivot - forward() * m_pivotDistance;
	}

	// ── Fly-look: rotate in place, pivot rides along with the camera ─────────
	if (in.look && (in.mouseDelta.x != 0.0f || in.mouseDelta.y != 0.0f))
	{
		m_yaw   += in.mouseDelta.x * kLookSpeed;
		m_pitch -= in.mouseDelta.y * kLookSpeed;
		m_pitch  = glm::clamp(m_pitch, -kPitchLimit, kPitchLimit);
	}

	// ── Pan: move the camera (and pivot) on the view plane ───────────────────
	if (in.pan && (in.mouseDelta.x != 0.0f || in.mouseDelta.y != 0.0f))
	{
		// World units per logical pixel at the pivot distance.
		const float worldPerPixel =
			2.0f * m_pivotDistance * std::tan(glm::radians(m_fov * 0.5f)) /
			std::max(1.0f, in.viewportHeight);
		m_position += (-right() * in.mouseDelta.x + up() * in.mouseDelta.y) * worldPerPixel;
	}

	// ── Dolly: move along the view direction toward/away from the pivot ──────
	if (in.wheel != 0.0f)
	{
		const float step = m_pivotDistance * 0.12f;
		const float move = in.wheel * step;
		m_position      += forward() * move;
		m_pivotDistance  = std::max(kMinPivot, m_pivotDistance - move);
	}

	// ── Fly movement (only while fly-look is held) ───────────────────────────
	if (in.look &&
	    (in.moveAxis.x != 0.0f || in.moveAxis.y != 0.0f || in.moveAxis.z != 0.0f))
	{
		const float speed = m_flySpeed * (in.fast ? 4.0f : 1.0f) * in.dt;
		m_position += (right()   * in.moveAxis.x +
		               up()      * in.moveAxis.y +
		               forward() * in.moveAxis.z) * speed;
	}
}

void EditorCamera::focusOn(const glm::vec3& center, float radius)
{
	ensureInit();
	radius = std::max(radius, 0.1f);
	// Distance so the bounding sphere fits the vertical field of view, with margin.
	// Capped short of the far plane: framing something kilometre-sized (a whole
	// landscape) otherwise parks the camera outside its own frustum, and then the
	// subject it was asked to show is the one thing clipped away.
	const float dist = std::min(radius / std::tan(glm::radians(m_fov * 0.5f)) * 1.5f,
	                            m_far * 0.9f);
	m_pivotDistance  = std::max(kMinPivot, dist);
	m_position       = center - forward() * m_pivotDistance;
	m_initialised    = true; // keep current orientation, don't re-derive
}

glm::mat4 EditorCamera::viewMatrix() const
{
	// up() rather than world +Y: at the Top/Bottom poles lookAt's own cross
	// product would collapse to NaN, and up() already knows the way out.
	return glm::lookAt(m_position, m_position + forward(), up());
}

EditorCameraOverride EditorCamera::makeOverride() const
{
	EditorCameraOverride o;
	o.active          = true;
	o.view            = viewMatrix();
	o.position        = m_position;
	o.fovDegrees      = m_fov;
	o.nearPlane       = m_near;
	o.farPlane        = m_far;
	o.orthographic    = m_orthographic;
	o.orthoHalfHeight = orthoHalfHeight();
	return o;
}
