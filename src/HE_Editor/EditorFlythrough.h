#pragma once
#include <glm/glm.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class EditorCamera;
class IRenderer;

// ── Camera flights from the command line (scripts/he_flythrough.py) ─────────────────
// A path of camera keys, flown through the open scene at a fixed frame rate while every frame is written out: a video of
// what the engine draws while the camera MOVES (streaming, LOD and far-image transitions, popping), without a person at
// the mouse. The editor starts one when HE_FLYTHROUGH names a path file; the frames go to HE_FLYTHROUGH_OUT as
// frame_00000.ppm, ... and the editor leaves after the last one. The script turns them into an MP4.
//
// The path file (JSON; positions in absolute world metres, angles in degrees, `t` in seconds, keys in time order):
//   { "fps": 30, "warmupSeconds": 5, "realtime": true,
//     "keys": [ { "t": 0,  "pos": [38000, 1800, 38000], "yaw": -45, "pitch": -5 },
//               { "t": 10, "pos": [20000,  900, 20000], "lookAt": [30000, 600, 10000] } ] }
// yaw 0 looks down -Z, +yaw turns right, +pitch looks up (EditorCamera). "lookAt" stands in for yaw and pitch.
// Between keys the position follows a Catmull-Rom curve through them, the angles too (yaw the short way round).
// warmupSeconds: the first key is held that long before the recording starts (the ground streams in, shaders build).
// realtime: each frame takes at least 1 / fps of wall time, so streaming and the camera keep the pace a game at that
// frame rate would have; false renders as fast as it can (the camera then outruns the streaming).
namespace HE::Ed
{
	struct FlyKey
	{
		double     t = 0.0;
		glm::dvec3 pos{ 0.0 };
		float      yawDeg = 0.0f, pitchDeg = 0.0f;
	};

	struct FlyPath
	{
		std::vector<FlyKey> keys;
		float fps = 30.0f;
		float warmupSeconds = 5.0f;
		bool  realtime = true;
		double duration() const { return keys.empty() ? 0.0 : keys.back().t - keys.front().t; }
	};

	bool parseFlyPath(const std::string& json, FlyPath& out, std::string* error = nullptr);
	// The camera at time t (clamped to the path): absolute position, yaw and pitch in degrees.
	void sampleFlyPath(const FlyPath& path, double t, glm::dvec3& pos, float& yawDeg, float& pitchDeg);

	// The run the editor drives once a frame, at the top of OnRender (before the viewport pushes the camera).
	class Flythrough
	{
	public:
		// Reads HE_FLYTHROUGH / HE_FLYTHROUGH_OUT; false (and nothing happens) when unset or the file does not parse.
		bool startFromEnvironment();
		bool active() const { return m_active; }
		// `ready`: the project is open and the scene loaded (the warm-up counts from there). `origin`: the world's floating
		// origin (the editor camera is relative to it). `quit` is called once, after the last frame is written.
		void tick(bool ready, EditorCamera& camera, IRenderer* renderer, const glm::dvec3& origin, const std::function<void()>& quit);

	private:
		FlyPath     m_path;
		std::string m_outDir;
		bool        m_active = false, m_done = false;
		int64_t     m_frame = -1;            // frames since ready; the recording starts at m_warmupFrames
		int64_t     m_warmupFrames = 0, m_recordFrames = 0;
		std::chrono::steady_clock::time_point m_last{};
	};
}
