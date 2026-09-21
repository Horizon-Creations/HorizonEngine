#include "McpToolRegistry.h"

#include "McpToolCommon.h"   // the argument readers and the schema helpers

#include <Application/AppIcon.h>   // hePngEncode — the engine's one PNG writer

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

// ─── A picture of the scene, from outside the editor ─────────────────────────
// The rationale for the shape is in McpToolRegistry.h beside McpScreenshotHooks
// (two outputs, why never a client-chosen path, why the live camera is a hook).
// What is worth stating HERE is what the handler actually promises:
//
//   • THE CAMERA IS THE CLIENT'S, NOT THE EDITOR'S. `position` and `look_at`
//     build a view matrix of their own; the editor's camera is read only when
//     the client gave no position at all, and it is never written. A client
//     that wants to "see what the user sees" sends nothing; one that wants its
//     own angle sends a position and gets exactly that.
//
//   • WHAT COMES BACK IS WHAT WAS ASKED FOR, OR A REFUSAL. The renderer is asked
//     for exactly `width`×`height`; a backend that cannot (no still-on-request
//     path, or a minimised window with nothing to draw into) answers false and
//     the client reads `unsupported` with the backend's name — never a picture
//     of some other size, and never a stale one from the viewport.
//
//   • ICONS OFF FOR A CLIENT CAMERA. The editor's light / camera / audio
//     billboards are for the human at the viewport. A client asking for a
//     picture of the scene from its own angle gets the scene; the live-camera
//     fallback keeps whatever the viewport shows, because that is the point of
//     that fallback.
//
//   • THE INLINE LIMIT IS THE SHIM'S, NOT A TASTE. scripts/he_mcp.py refuses a
//     frame over 4 MiB. base64 grows a PNG by a third, and the JSON around it
//     is not free, so kInlineMaxPngBytes is what fits. Over it the tool says
//     `too_large` and names the two ways out (smaller, or `output: "file"`)
//     rather than sending a reply the client will never see.

namespace HE::Ed
{

using nlohmann::json;

namespace
{

// ── Argument readers this file needs beyond McpToolCommon's ──────────────────

bool vec3Arg(const json& args, const char* key, glm::vec3& out)
{
	if (!args.is_object()) return false;
	const auto it = args.find(key);
	if (it == args.end() || !it->is_array() || it->size() != 3) return false;
	for (int i = 0; i < 3; ++i)
	{
		if (!(*it)[i].is_number()) return false;
		out[i] = (*it)[i].get<float>();
	}
	return true;
}

json vec3Prop(const char* what)
{
	return json{
		{ "type",        "array" },
		{ "items",       json{ { "type", "number" } } },
		{ "minItems",    3 },
		{ "maxItems",    3 },
		{ "description", what },
	};
}

json vec3Json(const glm::vec3& v)
{
	return json::array({ v.x, v.y, v.z });
}

json intProp(const char* what, int minimum, int maximum)
{
	return json{
		{ "type",        "integer" },
		{ "minimum",     minimum },
		{ "maximum",     maximum },
		{ "description", what },
	};
}

// A file base name a client may pick: letters, digits, '_' and '-', 1..64.
// The same shape as a tool name and for a related reason — nothing in it can
// leave the screenshot directory, name a dotfile, or need quoting anywhere.
bool safeBaseName(const std::string& s)
{
	if (s.empty() || s.size() > 64) return false;
	return std::all_of(s.begin(), s.end(), [](unsigned char c) {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		       (c >= '0' && c <= '9') || c == '_' || c == '-';
	});
}

// ── The camera ───────────────────────────────────────────────────────────────
// Right-handed, +Y up, the convention EditorCamera::viewMatrix follows; a view
// straight up or down would make (0,1,0) degenerate, so that one case takes +Z
// as the up reference instead of failing.
glm::mat4 lookAtView(const glm::vec3& eye, const glm::vec3& target)
{
	const glm::vec3 fwd = target - eye;
	const float     len = glm::length(fwd);
	if (len < 1e-6f) return glm::lookAt(eye, eye + glm::vec3(0.0f, 0.0f, -1.0f),
	                                    glm::vec3(0.0f, 1.0f, 0.0f));
	const glm::vec3 f  = fwd / len;
	const glm::vec3 up = (std::fabs(f.y) > 0.999f) ? glm::vec3(0.0f, 0.0f, 1.0f)
	                                                : glm::vec3(0.0f, 1.0f, 0.0f);
	return glm::lookAt(eye, target, up);
}

// The forward direction an override looks along: the third row of the view
// matrix, negated (view space looks down -Z).
glm::vec3 forwardOf(const EditorCameraOverride& c)
{
	return -glm::vec3(c.view[0][2], c.view[1][2], c.view[2][2]);
}

struct CameraChoice
{
	EditorCameraOverride cam;
	glm::vec3            lookAt = glm::vec3(0.0f);
	bool                 fromLive = false;
	bool                 ok = false;
	ToolResult           failure = ToolResult::ok(json::object());
};

CameraChoice chooseCamera(const McpScreenshotHooks& h, const json& args)
{
	CameraChoice c;

	// The live camera is the starting point for everything the client did not
	// say: near/far, the fov, and — when no position came — the whole view.
	EditorCameraOverride live;
	if (h.liveCamera) live = h.liveCamera();

	glm::vec3  position(0.0f);
	const bool hasPos = vec3Arg(args, "position", position);
	if (hasArg(args, "position") && !hasPos)
	{
		c.failure = ToolResult::fail("invalid_args",
			"'position' must be an array of three numbers [x, y, z].");
		return c;
	}
	glm::vec3  target(0.0f);
	const bool hasTarget = vec3Arg(args, "look_at", target);
	if (hasArg(args, "look_at") && !hasTarget)
	{
		c.failure = ToolResult::fail("invalid_args",
			"'look_at' must be an array of three numbers [x, y, z].");
		return c;
	}

	float fov = live.active ? live.fovDegrees : 60.0f;
	if (hasArg(args, "fov"))
	{
		const double f = numArg(args, "fov", -1.0);
		if (!(f >= 1.0 && f <= 170.0))
		{
			c.failure = ToolResult::fail("invalid_args",
				"'fov' is the vertical field of view in degrees and must be within 1..170.");
			return c;
		}
		fov = static_cast<float>(f);
	}

	if (!hasPos && !hasTarget && !hasArg(args, "fov"))
	{
		// Nothing about the camera was said: the picture is the viewport's own
		// view, icons and all. Without a live camera there is nothing to fall
		// back on, and the client has to say where to look from.
		if (!live.active)
		{
			c.failure = ToolResult::fail("no_camera",
				"No camera was given and the editor has no live viewport camera to fall "
				"back on. Send 'position' (and usually 'look_at').");
			return c;
		}
		c.cam      = live;
		c.lookAt   = live.position + forwardOf(live);
		c.fromLive = true;
		c.ok       = true;
		return c;
	}

	if (!hasPos)
	{
		// A look-at or a fov without a position: from where the viewport is.
		if (!live.active)
		{
			c.failure = ToolResult::fail("no_camera",
				"'position' is required when the editor has no live viewport camera to "
				"take it from.");
			return c;
		}
		position = live.position;
	}
	if (!hasTarget)
		// Keep looking the way the viewport looks, from the new place; with no
		// viewport, look at the world origin — the one point every scene has.
		target = live.active ? position + forwardOf(live) : glm::vec3(0.0f);

	c.cam.active          = true;
	c.cam.view            = lookAtView(position, target);
	c.cam.position        = position;
	c.cam.fovDegrees      = fov;
	c.cam.nearPlane       = live.active ? live.nearPlane : 0.1f;
	c.cam.farPlane        = live.active ? live.farPlane : 5000.0f;
	c.cam.orthographic    = false;
	c.cam.editorIcons     = false;
	c.lookAt              = target;
	c.ok                  = true;
	return c;
}

// ── The size ─────────────────────────────────────────────────────────────────

struct SizeChoice
{
	std::uint32_t w = kScreenshotDefaultWidth;
	std::uint32_t h = kScreenshotDefaultHeight;
	bool          ok = false;
	ToolResult    failure = ToolResult::ok(json::object());
};

SizeChoice chooseSize(const json& args)
{
	SizeChoice s;
	const int w = intArg(args, "width",  static_cast<int>(kScreenshotDefaultWidth));
	const int h = intArg(args, "height", static_cast<int>(kScreenshotDefaultHeight));
	if (w < 16 || h < 16 || w > static_cast<int>(kScreenshotMaxSide) ||
	    h > static_cast<int>(kScreenshotMaxSide))
	{
		s.failure = ToolResult::fail("invalid_args",
			"'width' and 'height' must be integers within 16.." +
			std::to_string(kScreenshotMaxSide) + ".");
		return s;
	}
	if (static_cast<std::uint64_t>(w) * static_cast<std::uint64_t>(h) > kScreenshotMaxPixels)
	{
		s.failure = ToolResult::fail("too_large",
			"width × height may not exceed " + std::to_string(kScreenshotMaxPixels) +
			" pixels (3840×2160).");
		return s;
	}
	s.w  = static_cast<std::uint32_t>(w);
	s.h  = static_cast<std::uint32_t>(h);
	s.ok = true;
	return s;
}

// A name for an unnamed file: the wall clock to the millisecond, which sorts
// and does not collide across two clients in the same frame only because the
// counter after it does.
std::string generatedBaseName()
{
	using namespace std::chrono;
	static unsigned s_seq = 0;
	const auto ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
	return "scene_" + std::to_string(ms) + "_" + std::to_string(++s_seq);
}

} // namespace

void registerScreenshotTools(McpToolRegistry& registry, McpScreenshotHooks hooks)
{
	auto h = std::make_shared<McpScreenshotHooks>(std::move(hooks));

	McpTool shot;
	shot.name    = "scene_screenshot";
	shot.mutates = false;
	shot.description =
		"Render one still image of the open scene from a camera you choose and "
		"return it as a PNG. Reading only: the world, the editor's own camera and "
		"the live viewport are left exactly as they were. Give 'position' and "
		"'look_at' (world units, +Y up) for your own angle; give nothing to get the "
		"viewport's current view. 'output' is 'inline' (the PNG comes back as an "
		"image content block you can look at; limited to ~2.5 MB of PNG — reduce the "
		"size or use 'file' above that) or 'file' (the PNG is written to the editor's "
		"screenshot directory and its absolute path is returned; use this from "
		"`batch`, which carries no image blocks). The JSON result always reports the "
		"camera and size actually used. Refuses with 'no_world' when no scene is "
		"open and 'unsupported' when this renderer backend has no still-on-request "
		"path (currently Metal only).";
	shot.inputSchema = objectSchema(json{
		{ "position", vec3Prop("Camera position [x, y, z] in world units. Omit to use the "
		                       "editor's live viewport camera position.") },
		{ "look_at",  vec3Prop("World point the camera looks at [x, y, z]. Omit to keep "
		                       "looking the way the viewport looks (or at the origin when "
		                       "there is no viewport).") },
		{ "fov",      json{ { "type", "number" }, { "minimum", 1 }, { "maximum", 170 },
		                    { "description", "Vertical field of view in degrees. Default: "
		                                     "the viewport's (60 without one)." } } },
		{ "width",    intProp("Image width in pixels. Default 1280.",  16,
		                      static_cast<int>(kScreenshotMaxSide)) },
		{ "height",   intProp("Image height in pixels. Default 720.",  16,
		                      static_cast<int>(kScreenshotMaxSide)) },
		{ "output",   json{ { "type", "string" }, { "enum", json::array({ "inline", "file" }) },
		                    { "description", "'inline' (default): PNG as an image content "
		                                     "block. 'file': PNG on disk, path returned." } } },
		{ "name",     stringProp("File base name for output 'file' (letters, digits, '_', "
		                         "'-'; '.png' is appended; an existing file of that name "
		                         "is overwritten). Omit for a generated, timestamped name.") },
	}, {});
	shot.handler = [h](const json& args) -> ToolResult {
		if (!h->hasWorld || !h->hasWorld())
			return ToolResult::fail("no_world",
				"No scene is open, so there is nothing to render. Open a project and a "
				"scene first (scene_open).");

		const std::string output = hasArg(args, "output") ? strArg(args, "output") : "inline";
		if (output != "inline" && output != "file")
			return ToolResult::fail("invalid_args", "'output' must be 'inline' or 'file'.");

		std::string baseName;
		if (hasArg(args, "name"))
		{
			baseName = strArg(args, "name");
			if (!safeBaseName(baseName))
				return ToolResult::fail("invalid_args",
					"'name' may contain only letters, digits, '_' and '-' (1..64 characters), "
					"without an extension.");
			if (output != "file")
				return ToolResult::fail("invalid_args",
					"'name' only applies to output 'file'.");
		}

		const SizeChoice size = chooseSize(args);
		if (!size.ok) return size.failure;
		const CameraChoice cam = chooseCamera(*h, args);
		if (!cam.ok) return cam.failure;

		// The directory is checked BEFORE the render, so a client that cannot
		// be given a file does not pay for a frame it will never see.
		std::filesystem::path dir;
		if (output == "file")
		{
			const std::string d = h->screenshotDir ? h->screenshotDir() : std::string();
			if (d.empty())
				return ToolResult::fail("no_directory",
					"The editor has no screenshot directory to write into; use output 'inline'.");
			dir = d;
		}

		if (!h->renderImage)
			return ToolResult::fail("unsupported",
				"This editor build has no still-on-request render path.");
		std::vector<std::uint8_t> rgba;
		if (!h->renderImage(cam.cam, size.w, size.h, rgba) ||
		    rgba.size() != static_cast<std::size_t>(size.w) * size.h * 4u)
		{
			const std::string backend = h->backendName ? h->backendName() : std::string();
			return ToolResult::fail("unsupported",
				"The renderer could not produce a " + std::to_string(size.w) + "x" +
				std::to_string(size.h) + " still" +
				(backend.empty() ? std::string() : " on the " + backend + " backend") +
				". Either this backend has no still-on-request path (currently Metal only) "
				"or the editor window is minimised and has nothing to draw into.");
		}

		const std::vector<std::uint8_t> png =
			hePngEncode(rgba.data(), static_cast<int>(size.w), static_cast<int>(size.h));
		if (png.empty())
			return ToolResult::fail("encode_failed", "The PNG encoder produced no bytes.");

		json result{
			{ "width",     size.w },
			{ "height",    size.h },
			{ "output",    output },
			{ "pngBytes",  png.size() },
			{ "camera",    json{
				{ "position",   vec3Json(cam.cam.position) },
				{ "lookAt",     vec3Json(cam.lookAt) },
				{ "fov",        cam.cam.fovDegrees },
				{ "fromViewport", cam.fromLive },
			} },
		};
		if (h->backendName) result["backend"] = h->backendName();

		if (output == "file")
		{
			std::error_code ec;
			std::filesystem::create_directories(dir, ec);
			const std::filesystem::path path =
				dir / ((baseName.empty() ? generatedBaseName() : baseName) + ".png");
			std::ofstream f(path, std::ios::binary | std::ios::trunc);
			if (f) f.write(reinterpret_cast<const char*>(png.data()),
			               static_cast<std::streamsize>(png.size()));
			if (!f)
				return ToolResult::fail("write_failed",
					"Could not write " + path.string() + ".");
			result["path"] = path.string();
			return ToolResult::ok(std::move(result));
		}

		if (png.size() > kInlineMaxPngBytes)
			return ToolResult::fail("too_large",
				"The PNG is " + std::to_string(png.size()) + " bytes, over the " +
				std::to_string(kInlineMaxPngBytes) + "-byte limit for an inline image "
				"(the client's frame limit). Ask for a smaller width/height, or use "
				"output 'file' and read the path.");
		ToolResult r  = ToolResult::ok(std::move(result));
		r.imageBytes  = png;
		r.imageMime   = "image/png";
		return r;
	};
	registry.add(std::move(shot));
}

} // namespace HE::Ed
