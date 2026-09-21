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
// refuse the way the schema says. What the Metal backend actually draws for
// that camera is the HE_DUMP_SCENEIMAGE witness's job (EditorApplication.cpp),
// not this binary's.

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
	McpToolRegistry registry;
	LastRender      last;
	fs::path        dir;
	bool            worldOpen   = true;
	bool            liveActive  = true;
	bool            renderFails = false;

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
		HE::Ed::registerScreenshotTools(registry, std::move(h));
	}

	~Fixture()
	{
		std::error_code ec;
		fs::remove_all(dir, ec);
	}

	ToolResult call(const json& args)
	{
		const auto* t = registry.find("scene_screenshot");
		REQUIRE(t != nullptr);
		return t->handler(args);
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
	for (const char* key : { "position", "look_at", "fov", "width", "height", "output", "name" })
		CHECK(t->inputSchema["properties"].contains(key));
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

	// And the result says what was used.
	CHECK(r.content["camera"]["position"] == json::array({ 10.0, 0.0, 0.0 }));
	CHECK(r.content["camera"]["lookAt"] == json::array({ 0.0, 0.0, 0.0 }));
	CHECK(r.content["camera"]["fov"] == doctest::Approx(35.5));
	CHECK(r.content["camera"]["fromViewport"] == false);
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
