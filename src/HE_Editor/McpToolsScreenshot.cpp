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
//   • THE CAMERA IS THE CLIENT'S, NOT THE EDITOR'S. The first call that says
//     anything about the camera creates one for THIS client (keyed on the
//     connection the bridge hands over in McpCallContext) and every later call
//     starts from it: absolute arguments replace parts, `move`/`turn` nudge
//     it, and saying nothing renders it where it stands. The editor's camera
//     is read only as the starting point of a client that has none yet, and it
//     is never written. Two clients cannot reach each other's camera — the id
//     comes from the bridge, not from the arguments. A client that has never
//     described a camera and sends nothing "sees what the user sees".
//
//   • THE CAMERA STATE OUTLIVES A FAILED PICTURE. It is stored BEFORE the
//     render, so a backend that answers `unsupported` does not also eat the
//     move the client just made; `render: false` stores without rendering at
//     all. It does not outlive the connection: the registry's client-gone
//     hook erases it.
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

// The forward direction an override looks along: the third row of the view
// matrix, negated (view space looks down -Z).
glm::vec3 forwardOf(const EditorCameraOverride& c)
{
	return -glm::vec3(c.view[0][2], c.view[1][2], c.view[2][2]);
}

// The live viewport camera as a client camera: the starting point of a client
// that has none of its own yet. Yaw/pitch are read off the view's forward.
McpClientCamera fromLive(const EditorCameraOverride& live)
{
	McpClientCamera c;
	c.position  = live.position;
	c.fovDeg    = live.fovDegrees;
	c.nearPlane = live.nearPlane;
	c.farPlane  = live.farPlane;
	c.lookAt(live.position + forwardOf(live));
	return c;
}

EditorCameraOverride toOverride(const McpClientCamera& c)
{
	EditorCameraOverride o;
	o.active       = true;
	o.view         = c.view();
	o.position     = c.position;
	o.fovDegrees   = c.fovDeg;
	o.nearPlane    = c.nearPlane;
	o.farPlane     = c.farPlane;
	o.orthographic = false;
	// The editor's light / camera / audio billboards are for the human at the
	// viewport; a client asking for its own angle gets the scene.
	o.editorIcons  = false;
	return o;
}

// The camera arguments, read and checked before anything is changed: a call
// that is refused for one bad argument leaves the client's camera untouched.
struct CameraArgs
{
	bool      hasPos = false, hasTarget = false, hasYaw = false, hasPitch = false,
	          hasFov = false, hasMove = false, hasTurn = false, reset = false;
	glm::vec3 position = glm::vec3(0.0f), target = glm::vec3(0.0f), move = glm::vec3(0.0f);
	float     yaw = 0.0f, pitch = 0.0f, fov = 60.0f;
	float     turnYaw = 0.0f, turnPitch = 0.0f;

	bool any() const
	{
		return hasPos || hasTarget || hasYaw || hasPitch || hasFov || hasMove || hasTurn;
	}
};

bool readCameraArgs(const json& args, CameraArgs& a, ToolResult& failure)
{
	auto bad = [&](const char* what) {
		failure = ToolResult::fail("invalid_args", what);
		return false;
	};
	auto number = [&](const char* key, bool& has, float& out, double lo, double hi,
	                  const char* what) {
		if (!hasArg(args, key)) return true;
		if (!args[key].is_number()) return bad(what);
		const double v = numArg(args, key, 0.0);
		if (!(v >= lo && v <= hi)) return bad(what);
		has = true;
		out = static_cast<float>(v);
		return true;
	};

	a.hasPos = vec3Arg(args, "position", a.position);
	if (hasArg(args, "position") && !a.hasPos)
		return bad("'position' must be an array of three numbers [x, y, z].");
	a.hasTarget = vec3Arg(args, "look_at", a.target);
	if (hasArg(args, "look_at") && !a.hasTarget)
		return bad("'look_at' must be an array of three numbers [x, y, z].");
	if (!number("yaw", a.hasYaw, a.yaw, -1e6, 1e6, "'yaw' must be a number (degrees)."))
		return false;
	if (!number("pitch", a.hasPitch, a.pitch, -90.0, 90.0,
	            "'pitch' must be a number within -90..90 (degrees, + looks up)."))
		return false;
	if (!number("fov", a.hasFov, a.fov, 1.0, 170.0,
	            "'fov' is the vertical field of view in degrees and must be within 1..170."))
		return false;
	a.hasMove = vec3Arg(args, "move", a.move);
	if (hasArg(args, "move") && !a.hasMove)
		return bad("'move' must be an array of three numbers [right, up, forward] in "
		           "world units, relative to the camera's own axes.");
	if (hasArg(args, "turn"))
	{
		const json& t = args["turn"];
		if (!t.is_array() || t.size() != 2 || !t[0].is_number() || !t[1].is_number())
			return bad("'turn' must be an array of two numbers [yaw, pitch] in degrees "
			           "(+yaw turns right, +pitch looks up).");
		a.hasTurn   = true;
		a.turnYaw   = t[0].get<float>();
		a.turnPitch = t[1].get<float>();
	}
	if (hasArg(args, "reset"))
	{
		if (!args["reset"].is_boolean()) return bad("'reset' must be a boolean.");
		a.reset = args["reset"].get<bool>();
	}

	if (a.hasTarget && (a.hasYaw || a.hasPitch))
		return bad("'look_at' and 'yaw'/'pitch' both set the direction; send one or "
		           "the other.");
	if (a.reset && a.any())
		return bad("'reset' drops your camera; send it alone, and describe the new "
		           "camera on the next call.");
	return true;
}

struct CameraChoice
{
	EditorCameraOverride cam;
	McpClientCamera      own;              // meaningful when !fromLive
	bool                 fromLive = false;
	bool                 ok = false;
	ToolResult           failure = ToolResult::ok(json::object());
};

// Read, update and store the calling client's camera, and say which camera the
// picture is taken with. The one place the table is written.
CameraChoice chooseCamera(const McpScreenshotHooks& h, McpClientCameras& table,
                          McpClientId client, const json& args)
{
	CameraChoice c;
	CameraArgs   a;
	if (!readCameraArgs(args, a, c.failure)) return c;

	if (a.reset) table.erase(client);

	EditorCameraOverride live;
	if (h.liveCamera) live = h.liveCamera();
	const McpClientCamera* stored = table.find(client);

	if (!a.any())
	{
		if (stored)
		{
			// The client's camera, as it stands.
			c.own      = *stored;
			c.cam      = toOverride(c.own);
			c.ok       = true;
			return c;
		}
		// Nothing about the camera was ever said: the picture is the
		// viewport's own view, icons and all — and no camera is created for
		// that; "what the user sees" is not a camera the client owns. Without
		// a live camera there is nothing to fall back on.
		if (!live.active)
		{
			c.failure = ToolResult::fail("no_camera",
				"No camera was given and the editor has no live viewport camera to fall "
				"back on. Send 'position' (and usually 'look_at').");
			return c;
		}
		c.cam      = live;
		c.fromLive = true;
		c.ok       = true;
		return c;
	}

	// Where the changes start from: the client's camera, else the viewport's,
	// else — with a position — a default camera at that position looking at
	// the world origin, the one point every scene has.
	McpClientCamera cam;
	if (stored)           cam = *stored;
	else if (live.active) cam = fromLive(live);
	else if (a.hasPos)
	{
		cam.position = a.position;
		cam.lookAt(glm::vec3(0.0f));
	}
	else
	{
		c.failure = ToolResult::fail("no_camera",
			"'position' is required on the first call when the editor has no live "
			"viewport camera to start from.");
		return c;
	}

	// Absolute first, relative after: "put me at P looking at T, then step
	// forward two" reads in that order.
	if (a.hasPos)    cam.position = a.position;
	if (a.hasYaw)    cam.yawDeg   = a.yaw;
	if (a.hasPitch)  cam.pitchDeg = a.pitch;
	if (a.hasTarget) cam.lookAt(a.target);
	if (a.hasFov)    cam.fovDeg   = a.fov;
	if (a.hasTurn)
	{
		cam.yawDeg   += a.turnYaw;
		cam.pitchDeg += a.turnPitch;
	}
	cam.normalise();
	if (a.hasMove)
		// Along the camera's own axes, after the turn: "turn left, walk
		// forward" walks the new way.
		cam.position += cam.right() * a.move.x + cam.up() * a.move.y +
		                cam.forward() * a.move.z;

	table.set(client, cam);
	c.own = cam;
	c.cam = toOverride(cam);
	c.ok  = true;
	return c;
}

json cameraJson(const CameraChoice& c, McpClientId client)
{
	if (c.fromLive)
	{
		const McpClientCamera live = fromLive(c.cam);
		return json{
			{ "position",     vec3Json(c.cam.position) },
			{ "lookAt",       vec3Json(c.cam.position + forwardOf(c.cam)) },
			{ "yaw",          live.yawDeg },
			{ "pitch",        live.pitchDeg },
			{ "fov",          c.cam.fovDegrees },
			{ "fromViewport", true },
			{ "stored",       false },
			{ "client",       client },
		};
	}
	return json{
		{ "position",     vec3Json(c.own.position) },
		{ "lookAt",       vec3Json(c.own.position + c.own.forward()) },
		{ "yaw",          c.own.yawDeg },
		{ "pitch",        c.own.pitchDeg },
		{ "fov",          c.own.fovDeg },
		{ "fromViewport", false },
		{ "stored",       true },
		{ "client",       client },
	};
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

// Everything the handler shares across calls: the hooks, and the camera table
// when the editor did not hand one in.
struct ScreenshotState
{
	McpScreenshotHooks hooks;
	McpClientCameras   ownTable;
	McpClientCameras&  table;

	explicit ScreenshotState(McpScreenshotHooks h)
		: hooks(std::move(h)), table(hooks.cameras ? *hooks.cameras : ownTable) {}
};

void registerScreenshotTools(McpToolRegistry& registry, McpScreenshotHooks hooks)
{
	auto s = std::make_shared<ScreenshotState>(std::move(hooks));

	// The camera dies with the connection. The bridge reports every connection
	// it forgets; a test fires this through registry.notifyClientGone.
	registry.addClientGoneHook([s](McpClientId client) { s->table.erase(client); });

	McpTool shot;
	shot.name    = "scene_screenshot";
	shot.mutates = false;
	shot.description =
		"Render one still image of the open scene from YOUR OWN camera and return it "
		"as a PNG. Reading only: the world, the editor's own camera and the live "
		"viewport are left exactly as they were. You have one camera of your own, "
		"kept between calls for as long as you are connected and invisible to other "
		"clients. The first call that gives any camera argument creates it (starting "
		"from the viewport's camera); later calls start from where you left it. "
		"Absolute: 'position' [x,y,z] (world units, +Y up), 'look_at' [x,y,z] or "
		"'yaw'/'pitch' (degrees; yaw 0 looks along -Z, +yaw turns right, +pitch looks "
		"up), 'fov'. Relative, applied after the absolute ones: 'turn' [yaw, pitch] "
		"degrees, then 'move' [right, up, forward] world units along the camera's "
		"own axes. Give no camera argument to render your camera as it stands, or, "
		"if you never set one, the viewport's current view. 'reset': true drops your "
		"camera (send it alone). 'render': false moves the camera without taking a "
		"picture and returns only the camera. 'output' is 'inline' (the PNG comes "
		"back as an image content block you can look at; limited to ~2.5 MB of PNG — "
		"reduce the size or use 'file' above that) or 'file' (the PNG is written to "
		"the editor's screenshot directory and its absolute path is returned; use "
		"this from `batch`, which carries no image blocks). The JSON result always "
		"reports the camera (position, lookAt, yaw, pitch, fov) and size actually "
		"used. Refuses with 'no_world' when no scene is open and 'unsupported' when "
		"this renderer backend has no still-on-request path (currently Metal only).";
	shot.inputSchema = objectSchema(json{
		{ "position", vec3Prop("Camera position [x, y, z] in world units (absolute). Omit "
		                       "to keep your camera's position (or the viewport's, on the "
		                       "first call).") },
		{ "look_at",  vec3Prop("World point the camera looks at [x, y, z] (absolute; sets "
		                       "yaw and pitch). Omit to keep the current direction. Not "
		                       "together with 'yaw'/'pitch'.") },
		{ "yaw",      json{ { "type", "number" },
		                    { "description", "Heading in degrees (absolute): 0 looks along "
		                                     "-Z, +90 along +X (right)." } } },
		{ "pitch",    json{ { "type", "number" }, { "minimum", -90 }, { "maximum", 90 },
		                    { "description", "Elevation in degrees (absolute): 0 level, "
		                                     "+90 straight up, -90 straight down." } } },
		{ "fov",      json{ { "type", "number" }, { "minimum", 1 }, { "maximum", 170 },
		                    { "description", "Vertical field of view in degrees. Default: "
		                                     "the viewport's (60 without one)." } } },
		{ "turn",     json{ { "type",        "array" },
		                    { "items",       json{ { "type", "number" } } },
		                    { "minItems",    2 },
		                    { "maxItems",    2 },
		                    { "description", "Relative rotation [yaw, pitch] in degrees, "
		                                     "applied after the absolute arguments: "
		                                     "[+10, 0] turns 10° to the right." } } },
		{ "move",     vec3Prop("Relative move [right, up, forward] in world units along the "
		                       "camera's own axes, applied after 'turn': [0, 0, 2] steps two "
		                       "units the way the camera looks.") },
		{ "reset",    json{ { "type", "boolean" },
		                    { "description", "Drop your camera. Send alone; the picture (if "
		                                     "any) is then the viewport's view." } } },
		{ "render",   json{ { "type", "boolean" },
		                    { "description", "Default true. False: update the camera and "
		                                     "return it without rendering a picture." } } },
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
	shot.handlerCtx = [s](const McpCallContext& ctx, const json& args) -> ToolResult {
		const McpScreenshotHooks& h = s->hooks;
		if (!h.hasWorld || !h.hasWorld())
			return ToolResult::fail("no_world",
				"No scene is open, so there is nothing to render. Open a project and a "
				"scene first (scene_open).");

		const std::string output = hasArg(args, "output") ? strArg(args, "output") : "inline";
		if (output != "inline" && output != "file")
			return ToolResult::fail("invalid_args", "'output' must be 'inline' or 'file'.");
		bool render = true;
		if (hasArg(args, "render"))
		{
			if (!args["render"].is_boolean())
				return ToolResult::fail("invalid_args", "'render' must be a boolean.");
			render = args["render"].get<bool>();
		}

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
		// The directory is checked BEFORE the camera is touched and the render
		// runs, so a client that cannot be given a file neither pays for a frame
		// it will never see nor finds its camera moved by a refused call.
		std::filesystem::path dir;
		if (output == "file")
		{
			const std::string d = h.screenshotDir ? h.screenshotDir() : std::string();
			if (d.empty())
				return ToolResult::fail("no_directory",
					"The editor has no screenshot directory to write into; use output 'inline'.");
			dir = d;
		}
		// From here on the client's camera IS updated, whatever the renderer
		// says next: a move is a move even when the picture fails.
		const CameraChoice cam = chooseCamera(h, s->table, ctx.client, args);
		if (!cam.ok) return cam.failure;

		json result{
			{ "width",     size.w },
			{ "height",    size.h },
			{ "output",    output },
			{ "rendered",  render },
			{ "camera",    cameraJson(cam, ctx.client) },
		};
		if (h.backendName) result["backend"] = h.backendName();
		if (!render) return ToolResult::ok(std::move(result));

		if (!h.renderImage)
			return ToolResult::fail("unsupported",
				"This editor build has no still-on-request render path.");
		std::vector<std::uint8_t> rgba;
		if (!h.renderImage(cam.cam, size.w, size.h, rgba) ||
		    rgba.size() != static_cast<std::size_t>(size.w) * size.h * 4u)
		{
			const std::string backend = h.backendName ? h.backendName() : std::string();
			return ToolResult::fail("unsupported",
				"The renderer could not produce a " + std::to_string(size.w) + "x" +
				std::to_string(size.h) + " still" +
				(backend.empty() ? std::string() : " on the " + backend + " backend") +
				". Either this backend has no still-on-request path (currently Metal only) "
				"or the editor window is minimised and has nothing to draw into. Your "
				"camera has been updated regardless.");
		}

		const std::vector<std::uint8_t> png =
			hePngEncode(rgba.data(), static_cast<int>(size.w), static_cast<int>(size.h));
		if (png.empty())
			return ToolResult::fail("encode_failed", "The PNG encoder produced no bytes.");
		result["pngBytes"] = png.size();

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
