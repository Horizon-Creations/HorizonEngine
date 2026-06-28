#pragma once
#include <glm/glm.hpp>
#include <Renderer/IRenderer.h>   // EditorCameraOverride

// ─── EditorCamera ───────────────────────────────────────────────────────────
// Scene-view camera for the editor. Unity-style navigation:
//   • Alt + Left-Mouse drag : orbit around the pivot
//   • Middle-Mouse drag     : pan (truck/pedestal)
//   • Right-Mouse drag      : fly-look (rotate in place)
//   • Right-Mouse + WASDQE  : fly movement (Shift = faster)
//   • Mouse wheel           : dolly toward/away from the pivot
//   • focusOn()             : frame a bounding sphere (F key in the UI)
//
// The class is input-agnostic: the UI layer collects raw deltas from ImGui and
// feeds them in via update(). State is yaw/pitch + position + pivot distance,
// so orbit and fly share a single representation.
class EditorCamera
{
public:
	struct Input
	{
		bool      orbit   = false;     // Alt + LMB drag
		bool      pan     = false;     // MMB drag
		bool      look    = false;     // RMB drag (fly look)
		glm::vec2 mouseDelta{ 0.0f };  // logical pixels since last frame
		float     wheel   = 0.0f;      // scroll ticks (+ = zoom in)
		glm::vec3 moveAxis{ 0.0f };    // fly move: x=right y=up z=forward, each [-1,1]
		bool      fast    = false;     // Shift held → move faster
		float     dt      = 0.0f;      // seconds
		float     viewportHeight = 1.0f; // logical pixels, for pan scaling
	};

	void update(const Input& in);

	// Frame a bounding sphere keeping the current orientation.
	void focusOn(const glm::vec3& center, float radius);

	// Place the camera at a world position looking along a forward direction
	// (used by headless captures / scripted views to aim the camera deterministically).
	void setOrientation(const glm::vec3& pos, const glm::vec3& forwardDir);

	glm::mat4 viewMatrix()  const;
	glm::vec3 position()    const { return m_position; }
	float     fovDegrees()  const { return m_fov; }
	float     nearPlane()   const { return m_near; }
	float     farPlane()    const { return m_far; }

	float     flySpeed()    const         { return m_flySpeed; }
	void      setFlySpeed(float s)        { m_flySpeed = s > 0.0f ? s : m_flySpeed; }

	EditorCameraOverride makeOverride() const;

private:
	void      ensureInit();
	glm::vec3 forward() const;
	glm::vec3 right()   const;
	glm::vec3 up()      const;

	// Looking at the origin from a pleasant 3/4 angle.
	glm::vec3 m_position{ 6.0f, 4.5f, 6.0f };
	float     m_yaw           = 0.0f;   // radians, initialised in update() lazily
	float     m_pitch         = 0.0f;   // radians
	float     m_pivotDistance = 8.7f;   // |m_position - pivot|
	bool      m_initialised   = false;

	float     m_fov       = 60.0f;
	float     m_near      = 0.1f;
	float     m_far       = 5000.0f;
	float     m_flySpeed  = 6.0f;       // world units / second
};
