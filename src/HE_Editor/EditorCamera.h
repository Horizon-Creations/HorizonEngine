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
	// The canned views. Perspective is "wherever you are, with a lens"; the six
	// others aim the camera along a world axis AND switch to orthographic, which
	// is what a Top/Front/Side view is for — lining things up without the lens
	// making the far wall look shorter than the near one. Front looks along -Z
	// (the same heading yaw=0 has), Right sits on +X looking toward -X.
	enum class ViewPreset { Perspective = 0, Top, Bottom, Front, Back, Left, Right };
	static const char* presetName(ViewPreset p);

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

	// Aim along a world axis (and go orthographic), keeping the pivot where it
	// is: the thing in the middle of the view stays in the middle, seen from
	// above / the front / the side instead. Perspective only drops the ortho
	// flag and leaves the heading alone.
	void applyPreset(ViewPreset p);
	// Which preset the camera is currently sitting in, read back from its
	// state (heading + projection) rather than remembered: an orbit out of Top
	// is no longer Top, and the toolbar label must say so. Perspective for any
	// free heading, and for an axis view that was switched back to a lens.
	ViewPreset currentPreset() const;

	// Orthographic projection: parallel rays, no lens. The visible height of
	// the view volume is NOT extra state — it is the height the perspective
	// view shows at the pivot distance, so toggling keeps whatever is at the
	// pivot framed at the same size, and the wheel (which moves the pivot
	// distance) zooms the ortho view exactly as it dollies the perspective one.
	bool      orthographic()     const { return m_orthographic; }
	void      setOrthographic(bool on) { m_orthographic = on; }
	float     orthoHalfHeight()  const;   // world units, half the visible height

	glm::mat4 viewMatrix()  const;
	glm::vec3 position()    const { return m_position; }
	float     fovDegrees()  const { return m_fov; }
	float     nearPlane()   const { return m_near; }
	float     farPlane()    const { return m_far; }

	float     flySpeed()    const         { return m_flySpeed; }
	void      setFlySpeed(float s)        { m_flySpeed = s > 0.0f ? s : m_flySpeed; }

	// Field of view (VERTICAL, degrees). The scene view keeps the wide default;
	// an asset preview wants a narrower lens, because it looks at ONE object
	// from close up and a wide angle bends everything near the border — which,
	// as the camera moves, reads as the picture warping rather than as the lens
	// it actually is. Also feeds focusOn() and the pan scale, so a preview
	// camera frames and pans consistently with what it draws.
	void      setFovDegrees(float deg)    { m_fov = glm::clamp(deg, 10.0f, 120.0f); }

	// ── View persistence (save/restore the last editor camera between sessions) ──
	float     yaw()           const { return m_yaw; }
	float     pitch()         const { return m_pitch; }
	float     pivotDistance() const { return m_pivotDistance; }
	bool      initialised()   const { return m_initialised; }   // false until first update/use
	// Restore a previously-saved view EXACTLY (position + orientation + orbit distance).
	// Marks the camera initialised so ensureInit() won't re-derive a look-at-origin pose.
	void      restoreView(const glm::vec3& pos, float yawRad, float pitchRad, float pivotDist)
	{
		m_position      = pos;
		m_yaw           = yawRad;
		m_pitch         = pitchRad;
		m_pivotDistance = pivotDist > 0.0f ? pivotDist : m_pivotDistance;
		m_initialised   = true;
	}

	EditorCameraOverride makeOverride() const;

private:
	void      ensureInit();
	glm::vec3 forward() const;
	glm::vec3 right()   const;
	glm::vec3 up()      const;
	// The reference "up" the basis is built against: world +Y, except at the
	// poles (Top/Bottom look straight along Y, where +Y is parallel to forward
	// and the cross product vanishes). There the heading's own horizontal
	// direction takes over, so a Top view has -Z at the top of the screen at
	// yaw 0, like a map with north up, and turns with the yaw.
	glm::vec3 upReference() const;

	// Looking at the origin from a pleasant 3/4 angle.
	glm::vec3 m_position{ 6.0f, 4.5f, 6.0f };
	float     m_yaw           = 0.0f;   // radians, initialised in update() lazily
	float     m_pitch         = 0.0f;   // radians
	float     m_pivotDistance = 8.7f;   // |m_position - pivot|
	bool      m_initialised   = false;
	bool      m_orthographic  = false;

	float     m_fov       = 60.0f;
	float     m_near      = 0.1f;
	float     m_far       = 5000.0f;
	float     m_flySpeed  = 6.0f;       // world units / second
};
