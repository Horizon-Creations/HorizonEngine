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

glm::vec3 EditorCamera::right() const
{
	return glm::normalize(glm::cross(forward(), glm::vec3(0.0f, 1.0f, 0.0f)));
}

glm::vec3 EditorCamera::up() const
{
	return glm::normalize(glm::cross(right(), forward()));
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
	const float dist = radius / std::tan(glm::radians(m_fov * 0.5f)) * 1.5f;
	m_pivotDistance  = std::max(kMinPivot, dist);
	m_position       = center - forward() * m_pivotDistance;
	m_initialised    = true; // keep current orientation, don't re-derive
}

glm::mat4 EditorCamera::viewMatrix() const
{
	return glm::lookAt(m_position, m_position + forward(), glm::vec3(0.0f, 1.0f, 0.0f));
}

EditorCameraOverride EditorCamera::makeOverride() const
{
	EditorCameraOverride o;
	o.active       = true;
	o.view         = viewMatrix();
	o.position     = m_position;
	o.fovDegrees   = m_fov;
	o.nearPlane    = m_near;
	o.farPlane     = m_far;
	o.orthographic = false;
	return o;
}
