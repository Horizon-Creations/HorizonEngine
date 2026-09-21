#include "doctest.h"

#include "McpToolRegistry.h"

#include <Application/AppIcon.h>   // heLoadPngRGBA — reading the PNG back

#include <glm/glm.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// ─── A picture of the scene, without a GPU ───────────────────────────────────
// The renderer is a hook, so the questions this file can answer are about the
// tool's own promises: does the fake frame come back as a PNG the decoder reads
// pixel-for-pixel; does it come back as an image block (inline) or as a file
// in the directory the editor named (file), never anywhere else; is the camera
// the one the client asked for, built the way the client asked (position AND
// look-at, forward really pointing at the target); do absent camera arguments
// fall back to the live camera and say so; do the size and the inline limits
// refuse the way the schema says. And, from the second step on: does each
// client keep a camera of its own between calls, can it move it absolutely
// and relatively, can it never reach another client's, and does the camera go
// when the registry is told the client is gone. What the Metal backend
// actually draws for that camera is the HE_DUMP_SCENEIMAGE witness's job
// (EditorApplication.cpp), not this binary's.

using HE::Ed::McpCallContext;
using HE::Ed::McpClientCameras;
using HE::Ed::McpClientId;
using HE::Ed::McpScreenshotHooks;
using HE::Ed::McpToolRegistry;
using HE::Ed::ToolResult;
using nlohmann::json;

namespace fs = std::filesystem;

namespace {

// What the fake renderer was asked for on its last call.
struct LastRender
{
	bool                 called = false;
	EditorCameraOverride camera;
	std::uint32_t        w = 0, h = 0;
};

// Paints a deterministic gradient: R = x, G = y (both scaled to 0..255),
// B = 7, A = 255 — every pixel is a witness for its own position, so a PNG
// that is flipped, cropped or stretched fails the read-back comparison.
void paint(std::vector<std::uint8_t>& rgba, std::uint32_t w, std::uint32_t h)
{
	rgba.assign(static_cast<std::size_t>(w) * h * 4u, 0);
	for (std::uint32_t y = 0; y < h; ++y)
		for (std::uint32_t x = 0; x < w; ++x)
		{
			std::uint8_t* p = &rgba[(static_cast<std::size_t>(y) * w + x) * 4u];
			p[0] = static_cast<std::uint8_t>((x * 255u) / (w > 1 ? w - 1 : 1));
			p[1] = static_cast<std::uint8_t>((y * 255u) / (h > 1 ? h - 1 : 1));
			p[2] = 7;
			p[3] = 255;
		}
}

struct Fixture
{
	McpToolRegistry  registry;
	McpClientCameras cameras;   // handed in, so a test can look at the table
	LastRender       last;
	fs::path         dir;
	bool             worldOpen   = true;
	bool             liveActive  = true;
	bool             renderFails = false;

	Fixture()
	{
		dir = fs::temp_directory_path() /
		      ("he_mcp_shot_" + std::to_string(
		                            std::chrono::steady_clock::now().time_since_epoch().count()));

		McpScreenshotHooks h;
		h.hasWorld    = [this] { return worldOpen; };
		h.backendName = [] { return std::string("Fake"); };
		h.liveCamera  = [this] {
			EditorCameraOverride c;
			c.active     = liveActive;
			c.position   = glm::vec3(1.0f, 2.0f, 3.0f);
			// Looking down -Z from (1,2,3): the identity rotation.
			c.view       = glm::translate(glm::mat4(1.0f), -c.position);
			c.fovDegrees = 45.0f;
			c.nearPlane  = 0.5f;
			c.farPlane   = 900.0f;
			c.editorIcons = true;
			return c;
		};
		h.renderImage = [this](const EditorCameraOverride& cam, std::uint32_t w,
		                       std::uint32_t hh, std::vector<std::uint8_t>& rgba) {
			last.called = true;
			last.camera = cam;
			last.w      = w;
			last.h      = hh;
			if (renderFails) return false;
			paint(rgba, w, hh);
			return true;
		};
		h.screenshotDir = [this] { return dir.string(); };
		h.cameras       = &cameras;
		HE::Ed::registerScreenshotTools(registry, std::move(h));
	}

	~Fixture()
	{
		std::error_code ec;
		fs::remove_all(dir, ec);
	}

	// As the anonymous client, the way every older test and the plain
	// `handler` path call it.
	ToolResult call(const json& args)
	{
		const auto* t = registry.find("scene_screenshot");
		REQUIRE(t != nullptr);
		return t->handler(args);
	}

	// As a named client, the way the bridge calls it.
	ToolResult callAs(McpClientId client, const json& args)
	{
		const auto* t = registry.find("scene_screenshot");
		REQUIRE(t != nullptr);
		last = LastRender{};
		return t->invoke(McpCallContext{ client }, args);
	}
};

// The camera's forward, from its view matrix (third row, negated).
glm::vec3 forwardOf(const EditorCameraOverride& c)
{
	return -glm::vec3(c.view[0][2], c.view[1][2], c.view[2][2]);
}

// Write the bytes out and read them back through the engine's decoder.
bool decodePng(const std::vector<std::uint8_t>& png, const fs::path& scratch,
               std::vector<std::uint8_t>& rgba, int& w, int& h)
{
	std::error_code ec;
	fs::create_directories(scratch.parent_path(), ec);
	{
		std::ofstream f(scratch, std::ios::binary | std::ios::trunc);
		f.write(reinterpret_cast<const char*>(png.data()),
		        static_cast<std::streamsize>(png.size()));
	}
	return HE::heLoadPngRGBA(scratch, rgba, w, h);
}

} // namespace

TEST_CASE("scene_screenshot: registered as a reading tool with the schema the model needs")
{
	Fixture f;
	const auto* t = f.registry.find("scene_screenshot");
	REQUIRE(t != nullptr);
	CHECK_FALSE(t->mutates);
	CHECK(t->inputSchema["type"] == "object");
	CHECK(t->inputSchema["additionalProperties"] == false);
	for (const char* key : { "position", "look_at", "yaw", "pitch", "fov", "turn", "move",
	                         "reset", "render", "width", "height", "output", "name" })
		CHECK(t->inputSchema["properties"].contains(key));
	// Context-aware, and still callable the plain way (as the anonymous client).
	CHECK(t->handlerCtx);
	CHECK(t->handler);
	// Nothing is required: no arguments = the viewport's own view.
	CHECK_FALSE(t->inputSchema.contains("required"));
}

TEST_CASE("scene_screenshot: inline — the fake frame comes back as a PNG image block, pixel for pixel")
{
	Fixture f;
	const ToolResult r = f.call(json{ { "position", json::array({ 0, 5, 10 }) },
	                                  { "look_at",  json::array({ 0, 0, 0 }) },
	                                  { "width", 64 }, { "height", 48 } });
	REQUIRE_FALSE(r.isError);
	CHECK(f.last.called);
	CHECK(f.last.w == 64);
	CHECK(f.last.h == 48);

	// The picture rides beside the JSON, not inside it.
	REQUIRE_FALSE(r.imageBytes.empty());
	CHECK(r.imageMime == "image/png");
	CHECK(r.content["width"] == 64);
	CHECK(r.content["height"] == 48);
	CHECK(r.content["output"] == "inline");
	CHECK(r.content["pngBytes"] == r.imageBytes.size());
	CHECK(r.content["backend"] == "Fake");
	CHECK_FALSE(r.content.contains("path"));

	std::vector<std::uint8_t> back;
	int w = 0, h = 0;
	REQUIRE(decodePng(r.imageBytes, f.dir / "scratch" / "inline.png", back, w, h));
	CHECK(w == 64);
	CHECK(h == 48);
	std::vector<std::uint8_t> expect;
	paint(expect, 64, 48);
	CHECK(back == expect);
}

TEST_CASE("scene_screenshot: the camera is the client's — position, look-at, fov, icons off")
{
	Fixture f;
	const ToolResult r = f.call(json{ { "position", json::array({ 10, 0, 0 }) },
	                                  { "look_at",  json::array({ 0, 0, 0 }) },
	                                  { "fov", 35.5 } });
	REQUIRE_FALSE(r.isError);
	const EditorCameraOverride& c = f.last.camera;
	CHECK(c.active);
	CHECK(c.position == glm::vec3(10.0f, 0.0f, 0.0f));
	CHECK(c.fovDegrees == doctest::Approx(35.5f));
	CHECK_FALSE(c.orthographic);
	CHECK_FALSE(c.editorIcons);
	// Looking from +X at the origin means forward is -X.
	const glm::vec3 fwd = forwardOf(c);
	CHECK(fwd.x == doctest::Approx(-1.0f));
	CHECK(fwd.y == doctest::Approx(0.0f).epsilon(1e-5));
	CHECK(fwd.z == doctest::Approx(0.0f).epsilon(1e-5));
	// Near and far are the viewport's: the client did not say, the live camera did.
	CHECK(c.nearPlane == doctest::Approx(0.5f));
	CHECK(c.farPlane == doctest::Approx(900.0f));

	// And the result says what was used: the look-at is reported one unit
	// ahead (the camera keeps a direction, not the point it was aimed at),
	// with the yaw that direction means: -X is yaw -90.
	CHECK(r.content["camera"]["position"] == json::array({ 10.0, 0.0, 0.0 }));
	CHECK(r.content["camera"]["lookAt"][0] == doctest::Approx(9.0));
	CHECK(r.content["camera"]["lookAt"][1] == doctest::Approx(0.0));
	CHECK(r.content["camera"]["yaw"] == doctest::Approx(-90.0));
	CHECK(r.content["camera"]["pitch"] == doctest::Approx(0.0));
	CHECK(r.content["camera"]["fov"] == doctest::Approx(35.5));
	CHECK(r.content["camera"]["fromViewport"] == false);
	CHECK(r.content["camera"]["stored"] == true);
	CHECK(r.content["rendered"] == true);
}

TEST_CASE("scene_screenshot: straight down does not degenerate the view")
{
	Fixture f;
	const ToolResult r = f.call(json{ { "position", json::array({ 0, 50, 0 }) },
	                                  { "look_at",  json::array({ 0, 0, 0 }) } });
	REQUIRE_FALSE(r.isError);
	const glm::vec3 fwd = forwardOf(f.last.camera);
	CHECK(fwd.y == doctest::Approx(-1.0f));
	// A finite matrix — the degenerate up would have produced NaNs.
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j)
			CHECK(std::isfinite(f.last.camera.view[i][j]));
}

TEST_CASE("scene_screenshot: no camera arguments = the viewport's live camera, as it is")
{
	Fixture f;
	const ToolResult r = f.call(json::object());
	REQUIRE_FALSE(r.isError);
	CHECK(f.last.w == HE::Ed::kScreenshotDefaultWidth);
	CHECK(f.last.h == HE::Ed::kScreenshotDefaultHeight);
	CHECK(f.last.camera.position == glm::vec3(1.0f, 2.0f, 3.0f));
	CHECK(f.last.camera.fovDegrees == doctest::Approx(45.0f));
	// Untouched: the viewport's icons stay in the viewport's picture.
	CHECK(f.last.camera.editorIcons);
	CHECK(r.content["camera"]["fromViewport"] == true);
	// Reported look-at is one unit along the live forward (-Z from (1,2,3)).
	CHECK(r.content["camera"]["lookAt"][2] == doctest::Approx(2.0));
}

TEST_CASE("scene_screenshot: a look-at without a position looks from where the viewport is")
{
	Fixture f;
	const ToolResult r = f.call(json{ { "look_at", json::array({ 1, 2, -7 }) } });
	REQUIRE_FALSE(r.isError);
	CHECK(f.last.camera.position == glm::vec3(1.0f, 2.0f, 3.0f));
	const glm::vec3 fwd = forwardOf(f.last.camera);
	CHECK(fwd.z == doctest::Approx(-1.0f));
	CHECK_FALSE(f.last.camera.editorIcons);
	CHECK(r.content["camera"]["fromViewport"] == false);
}

TEST_CASE("scene_screenshot: without a live camera the client has to say where from")
{
	Fixture f;
	f.liveActive = false;

	ToolResult r = f.call(json::object());
	REQUIRE(r.isError);
	CHECK(r.errorCode == "no_camera");
	CHECK_FALSE(f.last.called);

	r = f.call(json{ { "look_at", json::array({ 0, 0, 0 }) } });
	REQUIRE(r.isError);
	CHECK(r.errorCode == "no_camera");

	// A position alone is enough: it looks at the origin, with the defaults.
	r = f.call(json{ { "position", json::array({ 0, 0, 10 }) } });
	REQUIRE_FALSE(r.isError);
	CHECK(f.last.camera.fovDegrees == doctest::Approx(60.0f));
	CHECK(f.last.camera.nearPlane == doctest::Approx(0.1f));
	CHECK(forwardOf(f.last.camera).z == doctest::Approx(-1.0f));
}

TEST_CASE("scene_screenshot: file — the PNG lands in the editor's directory and nowhere else")
{
	Fixture f;
	const ToolResult r = f.call(json{ { "output", "file" }, { "name", "angle-1" },
	                                  { "width", 32 }, { "height", 20 } });
	REQUIRE_FALSE(r.isError);
	// Nothing inline in file mode.
	CHECK(r.imageBytes.empty());
	REQUIRE(r.content.contains("path"));
	const fs::path path = r.content["path"].get<std::string>();
	CHECK(path.is_absolute());
	CHECK(path.parent_path() == f.dir);
	CHECK(path.filename() == "angle-1.png");
	REQUIRE(fs::exists(path));

	std::vector<std::uint8_t> back;
	int w = 0, h = 0;
	REQUIRE(HE::heLoadPngRGBA(path, back, w, h));
	CHECK(w == 32);
	CHECK(h == 20);
	std::vector<std::uint8_t> expect;
	paint(expect, 32, 20);
	CHECK(back == expect);

	// Unnamed: a generated name, still in the directory, two calls two files.
	const ToolResult a = f.call(json{ { "output", "file" }, { "width", 16 }, { "height", 16 } });
	const ToolResult b = f.call(json{ { "output", "file" }, { "width", 16 }, { "height", 16 } });
	REQUIRE_FALSE(a.isError);
	REQUIRE_FALSE(b.isError);
	CHECK(a.content["path"] != b.content["path"]);
	CHECK(fs::path(a.content["path"].get<std::string>()).parent_path() == f.dir);
	CHECK(fs::exists(fs::path(b.content["path"].get<std::string>())));
}

TEST_CASE("scene_screenshot: a file name cannot leave the directory")
{
	Fixture f;
	for (const char* bad : { "../escape", "a/b", "a.png", ".hidden", "", "with space" })
	{
		const ToolResult r = f.call(json{ { "output", "file" }, { "name", bad } });
		REQUIRE(r.isError);
		CHECK(r.errorCode == "invalid_args");
	}
	// `name` is a file-mode argument; asked inline it is a mistake worth naming.
	const ToolResult r = f.call(json{ { "name", "fine" } });
	REQUIRE(r.isError);
	CHECK(r.errorCode == "invalid_args");
	CHECK_FALSE(f.last.called);
}

TEST_CASE("scene_screenshot: file output without a directory is refused before rendering")
{
	Fixture f;
	// A fresh registry whose editor names no directory.
	McpToolRegistry reg;
	bool rendered = false;
	McpScreenshotHooks h;
	h.hasWorld    = [] { return true; };
	h.renderImage = [&](const EditorCameraOverride&, std::uint32_t w, std::uint32_t hh,
	                    std::vector<std::uint8_t>& rgba) {
		rendered = true;
		paint(rgba, w, hh);
		return true;
	};
	HE::Ed::registerScreenshotTools(reg, std::move(h));
	const ToolResult r = reg.find("scene_screenshot")->handler(
		json{ { "output", "file" }, { "position", json::array({ 0, 0, 1 }) } });
	REQUIRE(r.isError);
	CHECK(r.errorCode == "no_directory");
	CHECK_FALSE(rendered);
}

TEST_CASE("scene_screenshot: size limits refuse the way the schema says")
{
	Fixture f;
	ToolResult r = f.call(json{ { "width", 8 } });
	REQUIRE(r.isError);
	CHECK(r.errorCode == "invalid_args");

	r = f.call(json{ { "width", 5000 }, { "height", 100 } });
	REQUIRE(r.isError);
	CHECK(r.errorCode == "invalid_args");

	// Within the side limit, over the pixel budget.
	r = f.call(json{ { "width", 4096 }, { "height", 4096 } });
	REQUIRE(r.isError);
	CHECK(r.errorCode == "too_large");
	CHECK_FALSE(f.last.called);

	// A float where an integer is wanted reads as the default, never throws.
	r = f.call(json{ { "width", 100.5 } });
	REQUIRE_FALSE(r.isError);
	CHECK(f.last.w == HE::Ed::kScreenshotDefaultWidth);
}

TEST_CASE("scene_screenshot: bad camera arguments are named, not guessed")
{
	Fixture f;
	ToolResult r = f.call(json{ { "position", json::array({ 1, 2 }) } });
	REQUIRE(r.isError);
	CHECK(r.errorCode == "invalid_args");
	CHECK(r.errorMessage.find("position") != std::string::npos);

	r = f.call(json{ { "look_at", "origin" } });
	REQUIRE(r.isError);
	CHECK(r.errorMessage.find("look_at") != std::string::npos);

	r = f.call(json{ { "fov", 0 } });
	REQUIRE(r.isError);
	CHECK(r.errorMessage.find("fov") != std::string::npos);

	r = f.call(json{ { "output", "clipboard" } });
	REQUIRE(r.isError);
	CHECK(r.errorCode == "invalid_args");
	CHECK_FALSE(f.last.called);
}

TEST_CASE("scene_screenshot: no world, and a backend that cannot, are two different refusals")
{
	Fixture f;
	f.worldOpen = false;
	ToolResult r = f.call(json::object());
	REQUIRE(r.isError);
	CHECK(r.errorCode == "no_world");
	CHECK_FALSE(f.last.called);

	f.worldOpen   = true;
	f.renderFails = true;
	r = f.call(json{ { "width", 320 }, { "height", 200 } });
	REQUIRE(r.isError);
	CHECK(r.errorCode == "unsupported");
	CHECK(f.last.called);
	CHECK(r.errorMessage.find("Fake") != std::string::npos);
	CHECK(r.errorMessage.find("320x200") != std::string::npos);
	CHECK(r.imageBytes.empty());
}

TEST_CASE("scene_screenshot: an inline PNG over the shim's budget is refused with the way out")
{
	// Noise does not compress: 1600×1000 of it is well over 2.5 MiB of PNG.
	McpToolRegistry reg;
	McpScreenshotHooks h;
	h.hasWorld    = [] { return true; };
	h.renderImage = [](const EditorCameraOverride&, std::uint32_t w, std::uint32_t hh,
	                   std::vector<std::uint8_t>& rgba) {
		rgba.resize(static_cast<std::size_t>(w) * hh * 4u);
		std::uint32_t s = 0x9E3779B9u;
		for (auto& b : rgba) { s = s * 1664525u + 1013904223u; b = static_cast<std::uint8_t>(s >> 24); }
		return true;
	};
	HE::Ed::registerScreenshotTools(reg, std::move(h));
	const ToolResult r = reg.find("scene_screenshot")->handler(
		json{ { "position", json::array({ 0, 0, 1 }) }, { "width", 1600 }, { "height", 1000 } });
	REQUIRE(r.isError);
	CHECK(r.errorCode == "too_large");
	CHECK(r.errorMessage.find("file") != std::string::npos);
	CHECK(r.imageBytes.empty());
}

// ─── One camera per client ───────────────────────────────────────────────────

TEST_CASE("scene_screenshot: a client's camera is kept between calls and rendered when nothing is said")
{
	Fixture f;
	ToolResult r = f.callAs(7, json{ { "position", json::array({ 0, 5, 10 }) },
	                                 { "look_at",  json::array({ 0, 5, 0 }) },
	                                 { "fov", 50 } });
	REQUIRE_FALSE(r.isError);
	REQUIRE(f.cameras.size() == 1);
	REQUIRE(f.cameras.find(7) != nullptr);
	CHECK(f.cameras.find(7)->position == glm::vec3(0.0f, 5.0f, 10.0f));
	CHECK(f.cameras.find(7)->fovDeg == doctest::Approx(50.0f));

	// Nothing said: the same camera, not the viewport's.
	r = f.callAs(7, json::object());
	REQUIRE_FALSE(r.isError);
	CHECK(f.last.camera.position == glm::vec3(0.0f, 5.0f, 10.0f));
	CHECK(f.last.camera.fovDegrees == doctest::Approx(50.0f));
	CHECK_FALSE(f.last.camera.editorIcons);
	CHECK(r.content["camera"]["fromViewport"] == false);
	CHECK(r.content["camera"]["stored"] == true);
	CHECK(r.content["camera"]["client"] == 7);

	// An absolute argument replaces only its part: the fov stays.
	r = f.callAs(7, json{ { "position", json::array({ 3, 5, 10 }) } });
	REQUIRE_FALSE(r.isError);
	CHECK(f.last.camera.position == glm::vec3(3.0f, 5.0f, 10.0f));
	CHECK(f.last.camera.fovDegrees == doctest::Approx(50.0f));
	CHECK(forwardOf(f.last.camera).z == doctest::Approx(-1.0f));
}

TEST_CASE("scene_screenshot: two clients keep two cameras and cannot reach each other's")
{
	Fixture f;
	REQUIRE_FALSE(f.callAs(1, json{ { "position", json::array({ 10, 0, 0 }) },
	                                { "look_at",  json::array({ 0, 0, 0 }) } }).isError);
	REQUIRE_FALSE(f.callAs(2, json{ { "position", json::array({ 0, 0, 10 }) },
	                                { "look_at",  json::array({ 0, 0, 0 }) },
	                                { "fov", 30 } }).isError);
	CHECK(f.cameras.size() == 2);

	// Each renders its own.
	ToolResult r = f.callAs(1, json::object());
	REQUIRE_FALSE(r.isError);
	CHECK(f.last.camera.position == glm::vec3(10.0f, 0.0f, 0.0f));
	CHECK(forwardOf(f.last.camera).x == doctest::Approx(-1.0f));
	r = f.callAs(2, json::object());
	REQUIRE_FALSE(r.isError);
	CHECK(f.last.camera.position == glm::vec3(0.0f, 0.0f, 10.0f));
	CHECK(f.last.camera.fovDegrees == doctest::Approx(30.0f));
	CHECK(forwardOf(f.last.camera).z == doctest::Approx(-1.0f));

	// Client 1 turns around; client 2 has not moved.
	REQUIRE_FALSE(f.callAs(1, json{ { "turn", json::array({ 180, 0 }) } }).isError);
	CHECK(forwardOf(f.last.camera).x == doctest::Approx(1.0f));
	r = f.callAs(2, json::object());
	CHECK(f.last.camera.position == glm::vec3(0.0f, 0.0f, 10.0f));
	CHECK(forwardOf(f.last.camera).z == doctest::Approx(-1.0f));
	CHECK(f.cameras.find(2)->yawDeg == doctest::Approx(0.0f));

	// A third client that never said anything sees the viewport, and no
	// camera is created for it.
	r = f.callAs(3, json::object());
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["camera"]["fromViewport"] == true);
	CHECK(f.last.camera.editorIcons);
	CHECK(f.cameras.size() == 2);
}

TEST_CASE("scene_screenshot: turn and move are relative, in degrees and along the camera's own axes")
{
	Fixture f;
	// Start at the origin looking down -Z (yaw 0), level.
	REQUIRE_FALSE(f.callAs(4, json{ { "position", json::array({ 0, 0, 0 }) },
	                                { "yaw", 0 }, { "pitch", 0 } }).isError);

	// Turn 90° right: now looking along +X.
	ToolResult r = f.callAs(4, json{ { "turn", json::array({ 90, 0 }) } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["camera"]["yaw"] == doctest::Approx(90.0));
	CHECK(forwardOf(f.last.camera).x == doctest::Approx(1.0f));

	// Step two forward: along +X, not along -Z.
	r = f.callAs(4, json{ { "move", json::array({ 0, 0, 2 }) } });
	REQUIRE_FALSE(r.isError);
	CHECK(f.last.camera.position.x == doctest::Approx(2.0f));
	CHECK(f.last.camera.position.z == doctest::Approx(0.0f).epsilon(1e-5));

	// Turn and move in one call: the move follows the new heading (another
	// 90° right → looking along +Z; "right" is then -X).
	r = f.callAs(4, json{ { "turn", json::array({ 90, 0 }) }, { "move", json::array({ 1, 0, 0 }) } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["camera"]["yaw"] == doctest::Approx(180.0));
	CHECK(f.last.camera.position.x == doctest::Approx(1.0f));
	CHECK(f.last.camera.position.z == doctest::Approx(0.0f).epsilon(1e-5));

	// Pitch stops at the pole rather than flipping over, yaw wraps.
	r = f.callAs(4, json{ { "turn", json::array({ 270, 200 }) } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["camera"]["pitch"] == doctest::Approx(90.0));
	CHECK(r.content["camera"]["yaw"] == doctest::Approx(90.0));
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j)
			CHECK(std::isfinite(f.last.camera.view[i][j]));
}

TEST_CASE("scene_screenshot: the first call of a client starts from the viewport's camera")
{
	Fixture f;
	// A relative move with no camera yet: from where the viewport is, looking
	// the way it looks (-Z from (1,2,3)) — two units forward is (1,2,1).
	ToolResult r = f.callAs(5, json{ { "move", json::array({ 0, 0, 2 }) } });
	REQUIRE_FALSE(r.isError);
	CHECK(f.last.camera.position == glm::vec3(1.0f, 2.0f, 1.0f));
	CHECK(f.last.camera.fovDegrees == doctest::Approx(45.0f));
	CHECK(f.last.camera.nearPlane == doctest::Approx(0.5f));
	CHECK(r.content["camera"]["stored"] == true);

	// Without a viewport there is nothing to start from.
	f.liveActive = false;
	r = f.callAs(6, json{ { "turn", json::array({ 10, 0 }) } });
	REQUIRE(r.isError);
	CHECK(r.errorCode == "no_camera");
	CHECK(f.cameras.find(6) == nullptr);
	// …but the client that already has a camera is unaffected.
	r = f.callAs(5, json::object());
	REQUIRE_FALSE(r.isError);
	CHECK(f.last.camera.position == glm::vec3(1.0f, 2.0f, 1.0f));
}

TEST_CASE("scene_screenshot: render false moves without a picture, and a failed picture keeps the move")
{
	Fixture f;
	ToolResult r = f.callAs(8, json{ { "position", json::array({ 0, 1, 0 }) },
	                                 { "render", false } });
	REQUIRE_FALSE(r.isError);
	CHECK_FALSE(f.last.called);
	CHECK(r.imageBytes.empty());
	CHECK(r.content["rendered"] == false);
	CHECK_FALSE(r.content.contains("pngBytes"));
	REQUIRE(f.cameras.find(8) != nullptr);
	CHECK(f.cameras.find(8)->position == glm::vec3(0.0f, 1.0f, 0.0f));

	f.renderFails = true;
	r = f.callAs(8, json{ { "move", json::array({ 0, 0, 3 }) } });
	REQUIRE(r.isError);
	CHECK(r.errorCode == "unsupported");
	CHECK(f.cameras.find(8)->position.z == doctest::Approx(-3.0f));

	// A refused argument, on the other hand, changes nothing.
	f.renderFails = false;
	r = f.callAs(8, json{ { "move", json::array({ 0, 0, 3 }) }, { "pitch", 200 } });
	REQUIRE(r.isError);
	CHECK(r.errorCode == "invalid_args");
	CHECK(f.cameras.find(8)->position.z == doctest::Approx(-3.0f));
	r = f.callAs(8, json{ { "look_at", json::array({ 0, 0, 0 }) }, { "yaw", 10 } });
	REQUIRE(r.isError);
	CHECK(r.errorCode == "invalid_args");
	r = f.callAs(8, json{ { "reset", true }, { "yaw", 10 } });
	REQUIRE(r.isError);
	CHECK(r.errorCode == "invalid_args");
	CHECK(f.cameras.find(8) != nullptr);
}

TEST_CASE("scene_screenshot: reset drops the camera, and so does the client going away")
{
	Fixture f;
	REQUIRE_FALSE(f.callAs(1, json{ { "position", json::array({ 0, 0, 5 }) } }).isError);
	REQUIRE_FALSE(f.callAs(2, json{ { "position", json::array({ 0, 0, 6 }) } }).isError);
	CHECK(f.cameras.size() == 2);

	// reset: back to the viewport's view, this call included.
	ToolResult r = f.callAs(1, json{ { "reset", true } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["camera"]["fromViewport"] == true);
	CHECK(f.last.camera.position == glm::vec3(1.0f, 2.0f, 3.0f));
	CHECK(f.cameras.find(1) == nullptr);
	CHECK(f.cameras.find(2) != nullptr);

	// The connection goes: the registry is told, the camera is gone, the
	// other client's is not.
	REQUIRE_FALSE(f.callAs(1, json{ { "position", json::array({ 0, 0, 7 }) } }).isError);
	CHECK(f.cameras.size() == 2);
	f.registry.notifyClientGone(1);
	CHECK(f.cameras.find(1) == nullptr);
	REQUIRE(f.cameras.find(2) != nullptr);
	CHECK(f.cameras.find(2)->position == glm::vec3(0.0f, 0.0f, 6.0f));
	// A later client with the same number starts clean.
	r = f.callAs(1, json::object());
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["camera"]["fromViewport"] == true);
	// Unknown ids are fine to report gone.
	f.registry.notifyClientGone(99);
	CHECK(f.cameras.size() == 1);
}

TEST_CASE("scene_screenshot: without a table from the editor the tool keeps its own")
{
	McpToolRegistry reg;
	McpScreenshotHooks h;
	EditorCameraOverride seen;
	h.hasWorld    = [] { return true; };
	h.renderImage = [&seen](const EditorCameraOverride& c, std::uint32_t w, std::uint32_t hh,
	                        std::vector<std::uint8_t>& rgba) {
		seen = c;
		paint(rgba, w, hh);
		return true;
	};
	HE::Ed::registerScreenshotTools(reg, std::move(h));
	const auto* t = reg.find("scene_screenshot");
	REQUIRE(t != nullptr);
	REQUIRE_FALSE(t->invoke(McpCallContext{ 3 }, json{ { "position", json::array({ 0, 0, 4 }) },
	                                                    { "width", 16 }, { "height", 16 } }).isError);
	REQUIRE_FALSE(t->invoke(McpCallContext{ 3 }, json{ { "width", 16 }, { "height", 16 } }).isError);
	CHECK(seen.position == glm::vec3(0.0f, 0.0f, 4.0f));
	reg.notifyClientGone(3);
	// No live camera in this fixture, so with the stored one gone there is
	// nothing to render from.
	const ToolResult r = t->invoke(McpCallContext{ 3 }, json{ { "width", 16 }, { "height", 16 } });
	REQUIRE(r.isError);
	CHECK(r.errorCode == "no_camera");
}

TEST_CASE("McpClientCamera: yaw/pitch follow EditorCamera, and lookAt inverts forward")
{
	HE::Ed::McpClientCamera c;
	// yaw 0 → -Z; +90 → +X; pitch +90 → +Y.
	CHECK(c.forward().z == doctest::Approx(-1.0f));
	c.yawDeg = 90.0f;
	CHECK(c.forward().x == doctest::Approx(1.0f));
	c.yawDeg = 0.0f; c.pitchDeg = 90.0f;
	CHECK(c.forward().y == doctest::Approx(1.0f));

	// Round trip through lookAt for a general direction.
	c = HE::Ed::McpClientCamera{};
	c.position = glm::vec3(1.0f, 2.0f, 3.0f);
	c.lookAt(glm::vec3(4.0f, 4.0f, 1.0f));
	const glm::vec3 want = glm::normalize(glm::vec3(3.0f, 2.0f, -2.0f));
	CHECK(c.forward().x == doctest::Approx(want.x));
	CHECK(c.forward().y == doctest::Approx(want.y));
	CHECK(c.forward().z == doctest::Approx(want.z));

	// Straight down keeps the yaw and gives a finite view with north up.
	c.yawDeg = 30.0f;
	c.lookAt(c.position - glm::vec3(0.0f, 10.0f, 0.0f));
	CHECK(c.pitchDeg == doctest::Approx(-90.0f));
	CHECK(c.yawDeg == doctest::Approx(30.0f));
	const glm::mat4 v = c.view();
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j)
			CHECK(std::isfinite(v[i][j]));

	// A target on the camera itself changes nothing.
	c.lookAt(c.position);
	CHECK(c.pitchDeg == doctest::Approx(-90.0f));
}

TEST_CASE("mcpBase64Encode: RFC 4648 with padding")
{
	auto enc = [](const std::string& s) {
		return HE::Ed::mcpBase64Encode(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
	};
	CHECK(enc("") == "");
	CHECK(enc("f") == "Zg==");
	CHECK(enc("fo") == "Zm8=");
	CHECK(enc("foo") == "Zm9v");
	CHECK(enc("foob") == "Zm9vYg==");
	CHECK(enc("fooba") == "Zm9vYmE=");
	CHECK(enc("foobar") == "Zm9vYmFy");
	const std::uint8_t bin[] = { 0xFF, 0xFE, 0xFD, 0x00 };
	CHECK(HE::Ed::mcpBase64Encode(bin, 4) == "//79AA==");
}
