#pragma once

// ─── One camera per connected MCP client ─────────────────────────────────────
// The screenshot tool renders from a camera the client describes. Describing it
// afresh on every call is fine for one picture and useless for "turn a little
// to the left and show me again" — so every client that has ever said anything
// about its camera keeps one HERE, keyed on its connection, and the next call
// starts from it. The store is a plain value table:
//
//   • keyed on the connection, so two clients can never move each other's
//     camera or read each other's picture: a client is handed its own id by
//     the bridge (McpCallContext) and has no way to name another one.
//   • dropped with the connection: the bridge reports every connection it
//     forgets to the registry's client-gone hooks, and the screenshot tool's
//     hook erases the entry. A later connection that gets the same number
//     starts without a camera, like every other new client.
//   • enumerable, in id order, because the next step draws every one of these
//     in the viewport as a frustum with its client number, and the editor
//     owns the table for exactly that reason (EditorApplication hands it to
//     the tool through McpScreenshotHooks::cameras).
//
// The orientation is yaw/pitch in degrees, not a matrix, and follows
// EditorCamera to the letter: yaw 0 / pitch 0 looks down -Z, +yaw turns right,
// +pitch looks up. Same numbers, same picture — a client that reads the
// editor's camera (scene_info) and sets its own to those values sees what the
// human sees, and the gizmo of the next step can reuse EditorCamera's drawing.

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdint>
#include <map>

namespace HE::Ed
{

using McpClientId = std::uint32_t;

struct McpClientCamera
{
	glm::vec3 position   = glm::vec3(0.0f);
	float     yawDeg     = 0.0f;    // (-180, 180]
	float     pitchDeg   = 0.0f;    // [-90, 90]
	float     fovDeg     = 60.0f;   // vertical, 1..170
	float     nearPlane  = 0.1f;
	float     farPlane   = 5000.0f;

	static constexpr float kPi = 3.14159265358979323846f;

	// yaw=0, pitch=0 → -Z; +pitch up; +yaw right. EditorCamera::forward.
	glm::vec3 forward() const
	{
		const float y = yawDeg * kPi / 180.0f, p = pitchDeg * kPi / 180.0f;
		const float cp = std::cos(p), sp = std::sin(p);
		return glm::normalize(glm::vec3(cp * std::sin(y), sp, -cp * std::cos(y)));
	}

	// World up, except on a pole, where forward is parallel to it: there the
	// picture's top points the way yaw 0 would look, like a map with north up.
	// EditorCamera::upReference, so a Top-preset viewport and a client camera
	// at pitch -90 agree about which way is up in the picture.
	glm::vec3 upReference() const
	{
		const float p = pitchDeg * kPi / 180.0f;
		if (std::abs(std::cos(p)) < 1e-4f)
		{
			const float y = yawDeg * kPi / 180.0f;
			return glm::vec3(std::sin(y), 0.0f, -std::cos(y));
		}
		return glm::vec3(0.0f, 1.0f, 0.0f);
	}

	glm::vec3 right() const { return glm::normalize(glm::cross(forward(), upReference())); }
	glm::vec3 up()    const { return glm::normalize(glm::cross(right(), forward())); }

	glm::mat4 view() const
	{
		return glm::lookAt(position, position + forward(), upReference());
	}

	// Point the camera at a world position, from where it stands. A target on
	// the camera itself says nothing about direction and leaves it alone; a
	// target straight above or below keeps the yaw (there is no heading to
	// read off a vertical line), which is also what keeps `turn` meaningful
	// at the pole.
	void lookAt(const glm::vec3& target)
	{
		const glm::vec3 d = target - position;
		if (glm::length(d) < 1e-6f) return;
		const glm::vec3 f = glm::normalize(d);
		pitchDeg = std::asin(glm::clamp(f.y, -1.0f, 1.0f)) * 180.0f / kPi;
		if (std::sqrt(f.x * f.x + f.z * f.z) > 1e-6f)
			yawDeg = std::atan2(f.x, -f.z) * 180.0f / kPi;
		normalise();
	}

	// Keep the numbers in their documented ranges. Yaw wraps, pitch stops at
	// the poles — past a pole the picture would be upside down, which nobody
	// asking to "look up a bit more" wants.
	void normalise()
	{
		pitchDeg = glm::clamp(pitchDeg, -90.0f, 90.0f);
		yawDeg   = std::fmod(yawDeg, 360.0f);
		if (yawDeg > 180.0f)  yawDeg -= 360.0f;
		if (yawDeg <= -180.0f) yawDeg += 360.0f;
	}
};

class McpClientCameras
{
public:
	const McpClientCamera* find(McpClientId client) const
	{
		const auto it = m_cameras.find(client);
		return it == m_cameras.end() ? nullptr : &it->second;
	}
	void set(McpClientId client, const McpClientCamera& cam) { m_cameras[client] = cam; }
	bool erase(McpClientId client) { return m_cameras.erase(client) != 0; }
	void clear() { m_cameras.clear(); }
	std::size_t size() const { return m_cameras.size(); }

	// In client-id order, so a frame that draws them draws them in the same
	// order every time.
	const std::map<McpClientId, McpClientCamera>& all() const { return m_cameras; }

private:
	std::map<McpClientId, McpClientCamera> m_cameras;
};

} // namespace HE::Ed
