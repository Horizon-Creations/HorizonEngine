#include "EditorFlythrough.h"

#include "EditorCamera.h"
#include <Diagnostics/Logger.h>
#include <Renderer/IRenderer.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

namespace HE::Ed
{
namespace
{
	constexpr double kPi = 3.14159265358979323846;

	// Catmull-Rom through p1..p2 at s in [0, 1], the outer two as tangents.
	template <typename T> T catmull(const T& p0, const T& p1, const T& p2, const T& p3, double s)
	{
		const double s2 = s * s, s3 = s2 * s;
		return 0.5 * ((2.0 * p1) + (-p0 + p2) * s + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * s2 + (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * s3);
	}

	void lookAtAngles(const glm::dvec3& from, const glm::dvec3& to, float& yawDeg, float& pitchDeg)
	{
		const glm::dvec3 d = to - from;
		const double h = std::sqrt(d.x * d.x + d.z * d.z);
		// EditorCamera::forward(): (cos p sin y, sin p, -cos p cos y)
		yawDeg = float(std::atan2(d.x, -d.z) * 180.0 / kPi);
		pitchDeg = float(std::atan2(d.y, h) * 180.0 / kPi);
	}
}

bool parseFlyPath(const std::string& text, FlyPath& out, std::string* error)
{
	const auto fail = [&](const std::string& why) { if (error) *error = why; return false; };
	const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
	if (j.is_discarded() || !j.is_object()) return fail("not a JSON object");
	FlyPath p;
	p.fps = j.value("fps", p.fps);
	p.warmupSeconds = j.value("warmupSeconds", p.warmupSeconds);
	p.realtime = j.value("realtime", p.realtime);
	if (!(p.fps > 0.0f && p.fps <= 240.0f)) return fail("fps must be in (0, 240]");
	if (p.warmupSeconds < 0.0f) return fail("warmupSeconds must not be negative");
	const auto vec = [](const nlohmann::json& a, glm::dvec3& v) {
		if (!a.is_array() || a.size() != 3 || !a[0].is_number() || !a[1].is_number() || !a[2].is_number()) return false;
		v = glm::dvec3(a[0].get<double>(), a[1].get<double>(), a[2].get<double>());
		return true;
	};
	if (!j.contains("keys") || !j["keys"].is_array() || j["keys"].empty()) return fail("no \"keys\"");
	for (const nlohmann::json& k : j["keys"])
	{
		FlyKey key;
		if (!k.is_object() || !k.contains("t") || !k["t"].is_number()) return fail("a key without a time \"t\"");
		key.t = k["t"].get<double>();
		if (!k.contains("pos") || !vec(k["pos"], key.pos)) return fail("a key without \"pos\": [x, y, z]");
		if (k.contains("lookAt"))
		{
			glm::dvec3 at;
			if (!vec(k["lookAt"], at)) return fail("\"lookAt\" is not [x, y, z]");
			lookAtAngles(key.pos, at, key.yawDeg, key.pitchDeg);
		}
		else
		{
			key.yawDeg = k.value("yaw", 0.0f);
			key.pitchDeg = k.value("pitch", 0.0f);
		}
		if (!p.keys.empty() && key.t <= p.keys.back().t) return fail("the keys' times must increase");
		p.keys.push_back(key);
	}
	// Yaw the short way round: each key's yaw unwrapped against the one before.
	for (size_t i = 1; i < p.keys.size(); ++i)
	{
		float& y = p.keys[i].yawDeg;
		const float prev = p.keys[i - 1].yawDeg;
		while (y - prev > 180.0f) y -= 360.0f;
		while (y - prev < -180.0f) y += 360.0f;
	}
	out = std::move(p);
	return true;
}

void sampleFlyPath(const FlyPath& path, double t, glm::dvec3& pos, float& yawDeg, float& pitchDeg)
{
	const std::vector<FlyKey>& k = path.keys;
	if (k.empty()) return;
	if (k.size() == 1 || t <= k.front().t) { pos = k.front().pos; yawDeg = k.front().yawDeg; pitchDeg = k.front().pitchDeg; return; }
	if (t >= k.back().t) { pos = k.back().pos; yawDeg = k.back().yawDeg; pitchDeg = k.back().pitchDeg; return; }
	size_t i = 0;
	while (i + 2 < k.size() && t > k[i + 1].t) ++i;
	const FlyKey& a = k[i == 0 ? 0 : i - 1];
	const FlyKey& b = k[i];
	const FlyKey& c = k[i + 1];
	const FlyKey& d = k[std::min(i + 2, k.size() - 1)];
	const double s = (t - b.t) / (c.t - b.t);
	pos = catmull(a.pos, b.pos, c.pos, d.pos, s);
	const glm::dvec2 ang = catmull(glm::dvec2(a.yawDeg, a.pitchDeg), glm::dvec2(b.yawDeg, b.pitchDeg), glm::dvec2(c.yawDeg, c.pitchDeg),
	                               glm::dvec2(d.yawDeg, d.pitchDeg), s);
	yawDeg = float(ang.x);
	pitchDeg = float(std::clamp(ang.y, -89.5, 89.5));
}

bool Flythrough::startFromEnvironment()
{
	const char* file = std::getenv("HE_FLYTHROUGH");
	if (!file || !*file) return false;
	const char* out = std::getenv("HE_FLYTHROUGH_OUT");
	m_outDir = (out && *out) ? out : "flythrough";
	std::ifstream in(file);
	std::stringstream ss;
	ss << in.rdbuf();
	std::string err;
	if (!in || !parseFlyPath(ss.str(), m_path, &err))
	{
		HE_LOG_ERROR(Editor, "Flythrough: %s cannot be used (%s)", file, in ? err.c_str() : "cannot be read");
		return false;
	}
	std::error_code ec;
	std::filesystem::create_directories(m_outDir, ec);
	m_warmupFrames = int64_t(std::lround(double(m_path.warmupSeconds) * m_path.fps));
	m_recordFrames = int64_t(std::floor(m_path.duration() * m_path.fps)) + 1;
	m_active = true;
	HE_LOG_INFO(Editor, "Flythrough: %s, %zu key(s), %.1f s at %.0f fps (%lld frames) after %.1f s warm-up, to %s", file, m_path.keys.size(),
	            m_path.duration(), double(m_path.fps), static_cast<long long>(m_recordFrames), double(m_path.warmupSeconds), m_outDir.c_str());
	return true;
}

void Flythrough::tick(bool ready, EditorCamera& camera, IRenderer* renderer, const glm::dvec3& origin, const std::function<void()>& quit)
{
	if (!m_active || m_done || !ready || !renderer) return;
	// The frame drawn last tick (the viewport target holds it now): a recorded one is written out.
	const int64_t shown = m_frame - m_warmupFrames;
	if (shown >= 0 && shown < m_recordFrames)
	{
		std::vector<uint8_t> rgba;
		uint32_t w = 0, h = 0;
		if (renderer->CaptureViewport(rgba, w, h) && w > 0 && h > 0)
		{
			char name[32];
			std::snprintf(name, sizeof(name), "frame_%05lld.ppm", static_cast<long long>(shown));
			const std::string path = (std::filesystem::path(m_outDir) / name).string();
			if (std::FILE* f = std::fopen(path.c_str(), "wb"))
			{
				std::fprintf(f, "P6\n%u %u\n255\n", w, h);
				std::vector<uint8_t> rgb(size_t(w) * h * 3);
				for (size_t i = 0, o = 0; i + 2 < rgba.size() && o < rgb.size(); i += 4, o += 3)
				{
					rgb[o] = rgba[i]; rgb[o + 1] = rgba[i + 1]; rgb[o + 2] = rgba[i + 2];
				}
				std::fwrite(rgb.data(), 1, rgb.size(), f);
				std::fclose(f);
			}
			else
				HE_LOG_ERROR(Editor, "Flythrough: cannot write %s", path.c_str());
		}
		else
			HE_LOG_ERROR(Editor, "Flythrough: the renderer gave no frame %lld", static_cast<long long>(shown));
	}
	if (shown + 1 >= m_recordFrames)
	{
		m_done = true;
		HE_LOG_INFO(Editor, "Flythrough: %lld frames written to %s", static_cast<long long>(m_recordFrames), m_outDir.c_str());
		quit();
		return;
	}

	// The camera of the frame drawn next.
	++m_frame;
	const double t = m_path.keys.front().t + double(std::max<int64_t>(0, m_frame - m_warmupFrames)) / double(m_path.fps);
	glm::dvec3 pos;
	float yaw = 0.0f, pitch = 0.0f;
	sampleFlyPath(m_path, t, pos, yaw, pitch);
	camera.restoreView(glm::vec3(pos - origin), float(yaw * kPi / 180.0), float(pitch * kPi / 180.0), camera.pivotDistance());

	// Realtime: a frame lasts at least 1 / fps, like a game running at that rate (the streaming works in wall time).
	if (m_path.realtime)
	{
		const auto slot = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / m_path.fps));
		const auto now = std::chrono::steady_clock::now();
		if (m_last.time_since_epoch().count() != 0 && now < m_last + slot) std::this_thread::sleep_until(m_last + slot);
		m_last = std::chrono::steady_clock::now();
	}
}
}
