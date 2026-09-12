#include "EditorSettingsCatalog.h"

#include <algorithm>
#include <cctype>

namespace HE::Ed
{
namespace
{
using json = nlohmann::json;

SettingDesc boolRow(const char* key, const char* label, const char* category,
                    const char* row, bool EditorConfig::* member, const char* help)
{
	SettingDesc d;
	d.key = key; d.label = label; d.category = category; d.row = row;
	d.type = SettingType::Bool; d.help = help; d.pb = member;
	return d;
}

SettingDesc floatRow(const char* key, const char* label, const char* category,
                     const char* row, float EditorConfig::* member,
                     double lo, double hi, const char* help)
{
	SettingDesc d;
	d.key = key; d.label = label; d.category = category; d.row = row;
	d.type = SettingType::Float; d.help = help; d.pf = member;
	d.hasRange = true; d.minValue = lo; d.maxValue = hi;
	return d;
}

SettingDesc intRow(const char* key, const char* label, const char* category,
                   const char* row, int EditorConfig::* member,
                   double lo, double hi, const char* help)
{
	SettingDesc d;
	d.key = key; d.label = label; d.category = category; d.row = row;
	d.type = SettingType::Int; d.help = help; d.pi = member;
	d.hasRange = true; d.minValue = lo; d.maxValue = hi;
	return d;
}

SettingDesc enumRow(const char* key, const char* label, const char* category,
                    const char* row, int EditorConfig::* member,
                    std::vector<std::string> options, const char* help)
{
	SettingDesc d;
	d.key = key; d.label = label; d.category = category; d.row = row;
	d.type = SettingType::Enum; d.help = help; d.pi = member;
	d.options = std::move(options);
	d.hasRange = true; d.minValue = 0; d.maxValue = static_cast<double>(d.options.size()) - 1;
	return d;
}

std::string lowered(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

// ─── The table ───────────────────────────────────────────────────────────────
// Ordered the way Preferences orders its pages: General, Editor, Rendering.
// Every range here is the one the widget already enforces — a number outside it
// is refused rather than written and clamped somewhere else later.
std::vector<SettingDesc> buildCatalog()
{
	std::vector<SettingDesc> t;

	// ── General ▸ Appearance ────────────────────────────────────────────────
	t.push_back(floatRow("appearance.fontScale", "Editor Font Scale", "Appearance",
	                     "fontscale", &EditorConfig::UiFontScale, 0.5, 3.0,
	                     "Global scale of the editor's own text."));
	{
		SettingDesc d;
		d.key = "appearance.quickSettingsPins"; d.label = "Quick Settings Pins";
		d.category = "Appearance"; d.row = "fontscale";
		d.type = SettingType::String; d.ps = &EditorConfig::QuickSettingsFavorites;
		d.help = "Comma-separated list of the setting GROUP keys pinned to the "
		         "Quick Settings panel — the `row` value of the entries in this "
		         "table, not their `key`.";
		t.push_back(std::move(d));
	}

	// ── General ▸ Viewport ──────────────────────────────────────────────────
	t.push_back(floatRow("viewport.cameraSpeed", "Camera Speed", "Viewport",
	                     "camspeed", &EditorConfig::EditorCameraSpeed, 0.1, 100.0,
	                     "Editor fly-camera speed, world units per second."));
	t.push_back(enumRow("viewport.pointerInput", "Pointer Device", "Viewport",
	                    "pointerinput", &EditorConfig::PointerInput,
	                    { "Auto", "Mouse", "Trackpad" },
	                    "Which pointer grammar the preview panes use. Auto detects "
	                    "a trackpad."));

	// ── General ▸ Input ─────────────────────────────────────────────────────
	t.push_back(floatRow("input.gamepadStickDeadzone", "Stick Deadzone", "Input",
	                     "gamepad", &EditorConfig::GamepadStickDeadzone, 0.0, 0.9,
	                     "Radial deadzone of both gamepad sticks."));
	t.push_back(floatRow("input.gamepadTriggerDeadzone", "Trigger Deadzone", "Input",
	                     "gamepad", &EditorConfig::GamepadTriggerDeadzone, 0.0, 0.9,
	                     "Scalar deadzone of both gamepad triggers."));

	// ── General ▸ Content Browser ───────────────────────────────────────────
	t.push_back(boolRow("contentBrowser.keepCpuAssets", "Keep CPU Assets",
	                    "Content Browser", "cpucache", &EditorConfig::KeepCPUAssets,
	                    "Keep mesh/texture data in RAM after upload. Costs memory, "
	                    "saves a re-read."));
	t.push_back(intRow("contentBrowser.refreshRate", "Refresh Rate",
	                   "Content Browser", "cbrefresh",
	                   &EditorConfig::ContentBrowserRefreshRate, 1, 600,
	                   "Seconds between content directory rescans."));

	// ── Editor ▸ Collaboration ──────────────────────────────────────────────
	t.push_back(boolRow("collab.lanDiscovery", "LAN Discovery", "Collaboration",
	                    "landiscovery", &EditorConfig::CollabLanDiscovery,
	                    "Announce a hosted session on the local network, and listen "
	                    "for other people's."));
	t.push_back(boolRow("collab.syncLargeAssets", "Sync Large Assets",
	                    "Collaboration", "collabsynclarge",
	                    &EditorConfig::CollabSyncLargeAssets,
	                    "Also send meshes, textures, audio and fonts over the "
	                    "session instead of leaving them to source control. The "
	                    "HOST's decision for the session; it cannot be changed "
	                    "while one runs."));
	t.push_back(intRow("collab.maxAssetMB", "Max Asset Size (MB)", "Collaboration",
	                   "collabmaxasset", &EditorConfig::CollabMaxAssetMB, 1, 4096,
	                   "The biggest single asset this editor will send or accept "
	                   "over a session. A transfer is held whole in memory on both "
	                   "ends."));

	// ── Editor ▸ Remote Control ─────────────────────────────────────────────
	// Readable, never writable: this is the door these tools came through.
	{
		SettingDesc d = boolRow("remoteControl.enabled", "MCP Server Enabled",
		                        kRemoteControlCategory, "mcpserver",
		                        &EditorConfig::McpServerEnabled,
		                        "Whether this editor accepts external MCP clients "
		                        "at all.");
		d.writable = false;
		d.readOnlyReason = "the switch this connection itself came through — "
		                   "turn it on or off in Preferences > Editor > Remote Control";
		t.push_back(std::move(d));
	}
	{
		SettingDesc d = intRow("remoteControl.port", "MCP Port", kRemoteControlCategory,
		                       "mcpport", &EditorConfig::McpPort, 0, 65535,
		                       "Listening port (0 = the OS picks one).");
		d.writable = false;
		d.readOnlyReason = "changing it would drop this connection — "
		                   "set it in Preferences > Editor > Remote Control";
		t.push_back(std::move(d));
	}

	// ── Rendering ▸ Display ─────────────────────────────────────────────────
	{
		SettingDesc d;
		d.key = "display.backend"; d.label = "Graphics Backend"; d.category = "Display";
		d.row = "backend"; d.type = SettingType::String;
		d.storage = SettingStorage::External;
		d.writable = false;
		d.readOnlyReason = "chosen at startup — the editor has to be restarted for "
		                   "a different one";
		d.help = "The renderer this editor is running on.";
		t.push_back(std::move(d));
	}
	{
		SettingDesc d;
		d.key = "display.vsync"; d.label = "VSync"; d.category = "Display";
		d.row = "vsync"; d.type = SettingType::Bool;
		d.storage = SettingStorage::External;
		// No `apply`: an External row is read AND written through the caller's
		// hooks, so the hook's write is already the place the Application hears
		// about it. `apply` exists for the other case — a field that IS in
		// EditorConfig and additionally has to travel (display.maxFps).
		d.help = "Present synchronised to the display. Lives on the Application, "
		         "not in the editor config, because the profiler's capture saves "
		         "and restores it.";
		t.push_back(std::move(d));
	}
	{
		SettingDesc d = floatRow("display.maxFps", "Max FPS", "Display", "maxfps",
		                         &EditorConfig::MaxFps, 0.0, 1000.0,
		                         "VSync-off frame cap (0 = unlimited). Also paces "
		                         "mouse-look.");
		d.apply = "maxfps";
		t.push_back(std::move(d));
	}
	t.push_back(enumRow("display.renderPath", "Render Path", "Display", "renderpath",
	                    &EditorConfig::RenderPath, { "Forward", "Deferred" },
	                    "Deferred needs a backend that supports it (Metal, OpenGL); "
	                    "an unsupported choice falls back at push time."));

	// ── Rendering ▸ Post-Processing ─────────────────────────────────────────
	t.push_back(enumRow("postProcess.antiAliasing", "Anti-Aliasing",
	                    "Post-Processing", "aa", &EditorConfig::AntiAliasing,
	                    { "Off", "FXAA", "SMAA", "TAA", "MetalFX" },
	                    "Methods the backend cannot do fall back at push time."));
	t.push_back(floatRow("postProcess.aaSharpness", "AA Sharpness", "Post-Processing",
	                     "aa", &EditorConfig::AASharpness, 0.0, 1.0,
	                     "Temporal modes only (TAA, MetalFX)."));
	t.push_back(floatRow("postProcess.renderScale", "Render Scale", "Post-Processing",
	                     "aa", &EditorConfig::RenderScale, 0.25, 2.0,
	                     "< 1 upscales, > 1 supersamples."));
	t.push_back(boolRow("postProcess.specularAA", "Specular AA", "Post-Processing",
	                    "aa", &EditorConfig::SpecularAA,
	                    "Roughness regularization against shading aliasing."));
	t.push_back(floatRow("postProcess.specularAAStrength", "Specular AA Strength",
	                     "Post-Processing", "aa", &EditorConfig::SpecularAAStrength,
	                     0.0, 2.0, ""));
	t.push_back(boolRow("postProcess.bloomEnabled", "Bloom", "Post-Processing",
	                    "bloom", &EditorConfig::BloomEnabled, ""));
	t.push_back(floatRow("postProcess.bloomThreshold", "Bloom Threshold",
	                     "Post-Processing", "bloom", &EditorConfig::BloomThreshold,
	                     0.0, 10.0, "Luminance above which a pixel blooms."));
	t.push_back(floatRow("postProcess.bloomIntensity", "Bloom Intensity",
	                     "Post-Processing", "bloom", &EditorConfig::BloomIntensity,
	                     0.0, 5.0, ""));
	t.push_back(boolRow("postProcess.ssaoEnabled", "SSAO", "Post-Processing",
	                    "ssao", &EditorConfig::SSAOEnabled, ""));
	t.push_back(floatRow("postProcess.ssaoRadius", "SSAO Radius", "Post-Processing",
	                     "ssao", &EditorConfig::SSAORadius, 0.01, 10.0,
	                     "Hemisphere sampling radius in view-space units."));
	t.push_back(floatRow("postProcess.ssaoIntensity", "SSAO Intensity",
	                     "Post-Processing", "ssao", &EditorConfig::SSAOIntensity,
	                     0.0, 1.0, "0 = off … 1 = full ambient occlusion."));
	t.push_back(enumRow("postProcess.ssaoMethod", "AO Method", "Post-Processing",
	                    "ssao", &EditorConfig::SSAOMethod,
	                    { "SSAO", "HBAO", "GTAO" }, ""));
	t.push_back(boolRow("postProcess.ssrEnabled", "Screen-Space Reflections",
	                    "Post-Processing", "ssr", &EditorConfig::SSREnabled,
	                    "Metal + the deferred render path; the backend gates it."));
	t.push_back(floatRow("postProcess.ssrIntensity", "SSR Intensity",
	                     "Post-Processing", "ssr", &EditorConfig::SSRIntensity,
	                     0.0, 2.0, ""));
	t.push_back(floatRow("postProcess.ssrMaxRoughness", "SSR Max Roughness",
	                     "Post-Processing", "ssr", &EditorConfig::SSRMaxRoughness,
	                     0.0, 1.0, "Surfaces rougher than this get no reflection."));
	t.push_back(enumRow("postProcess.ssrQuality", "SSR Quality", "Post-Processing",
	                    "ssr", &EditorConfig::SSRQuality,
	                    { "Low", "Medium", "High" },
	                    "16 raw steps / 32 + blur / 64 + glossy."));

	// ── Rendering ▸ Global Illumination ─────────────────────────────────────
	t.push_back(boolRow("gi.enabled", "Global Illumination", "Global Illumination",
	                    "gi", &EditorConfig::GlobalIlluminationEnabled,
	                    "Ray-traced DDGI. Metal-only and strictly opt-in; when on "
	                    "and supported it REPLACES CSM shadows and AO/ambient."));
	t.push_back(floatRow("gi.indirectIntensity", "Indirect Intensity",
	                     "Global Illumination", "gi",
	                     &EditorConfig::GIIndirectIntensity, 0.0, 5.0, ""));
	t.push_back(floatRow("gi.lightRadius", "Sun Angular Radius",
	                     "Global Illumination", "gi", &EditorConfig::GILightRadius,
	                     0.0, 10.0, "Degrees — drives shadow penumbra softness."));
	t.push_back(boolRow("gi.reflectionsEnabled", "GI Reflections",
	                    "Global Illumination", "girefl",
	                    &EditorConfig::GIReflectionsEnabled,
	                    "Real scene rays instead of the sky cubemap."));
	t.push_back(floatRow("gi.reflIntensity", "Reflection Intensity",
	                     "Global Illumination", "girefl",
	                     &EditorConfig::GIReflIntensity, 0.0, 2.0, ""));
	t.push_back(floatRow("gi.reflMaxRoughness", "Reflection Max Roughness",
	                     "Global Illumination", "girefl",
	                     &EditorConfig::GIReflMaxRoughness, 0.0, 1.0, ""));
	t.push_back(enumRow("gi.reflQuality", "Reflection Quality",
	                    "Global Illumination", "girefl", &EditorConfig::GIReflQuality,
	                    { "Raw", "Blur", "Glossy" }, ""));
	t.push_back(intRow("gi.reflBounces", "Reflection Bounces", "Global Illumination",
	                   "girefl", &EditorConfig::GIReflBounces, 1, 4,
	                   "Metal only — the OpenGL kernel traces one segment."));
	t.push_back(boolRow("gi.reflBlur", "Reflection Blur", "Global Illumination",
	                    "girefl", &EditorConfig::GIReflBlur, ""));

	// ── Rendering ▸ Effects ─────────────────────────────────────────────────
	t.push_back(boolRow("effects.gpuParticles", "GPU Weather Particles", "Effects",
	                    "gpuparticles", &EditorConfig::GpuParticles,
	                    "Simulate rain/snow on the GPU instead of the CPU pool. The "
	                    "backend gates it."));

	return t;
}

} // namespace

const std::vector<SettingDesc>& editorSettingCatalog()
{
	static const std::vector<SettingDesc> table = buildCatalog();
	return table;
}

const SettingDesc* findEditorSetting(const std::string& key)
{
	for (const SettingDesc& d : editorSettingCatalog())
		if (d.key == key) return &d;
	return nullptr;
}

bool readEditorSetting(const EditorConfig& cfg, const SettingDesc& d, json& out)
{
	if (d.storage != SettingStorage::Config) return false;
	switch (d.type)
	{
	case SettingType::Bool:   if (!d.pb) return false; out = cfg.*(d.pb); return true;
	case SettingType::Int:    if (!d.pi) return false; out = cfg.*(d.pi); return true;
	case SettingType::Float:  if (!d.pf) return false; out = cfg.*(d.pf); return true;
	case SettingType::String: if (!d.ps) return false; out = cfg.*(d.ps); return true;
	case SettingType::Enum:
	{
		if (!d.pi) return false;
		const int v = cfg.*(d.pi);
		// The NAME, because that is what a caller can act on; the index rides
		// alongside it in the tool result for anyone who wants the number.
		out = (v >= 0 && v < static_cast<int>(d.options.size()))
		          ? json(d.options[static_cast<std::size_t>(v)])
		          : json(v);
		return true;
	}
	}
	return false;
}

bool writeEditorSetting(EditorConfig& cfg, const SettingDesc& d, const json& in,
                        std::string& outError, bool& outChanged)
{
	outChanged = false;
	if (d.storage != SettingStorage::Config)
	{
		outError = "not stored in the editor config";
		return false;
	}
	if (!d.writable)
	{
		outError = d.readOnlyReason.empty() ? "read-only" : d.readOnlyReason;
		return false;
	}

	switch (d.type)
	{
	case SettingType::Bool:
	{
		if (!in.is_boolean()) { outError = "expected a boolean"; return false; }
		const bool v = in.get<bool>();
		outChanged = (cfg.*(d.pb) != v);
		cfg.*(d.pb) = v;
		return true;
	}
	case SettingType::Int:
	{
		if (!in.is_number_integer()) { outError = "expected an integer"; return false; }
		const long long v = in.get<long long>();
		if (d.hasRange && (static_cast<double>(v) < d.minValue ||
		                   static_cast<double>(v) > d.maxValue))
		{
			outError = "out of range (" + std::to_string(static_cast<long long>(d.minValue))
			         + " … " + std::to_string(static_cast<long long>(d.maxValue)) + ")";
			return false;
		}
		outChanged = (cfg.*(d.pi) != static_cast<int>(v));
		cfg.*(d.pi) = static_cast<int>(v);
		return true;
	}
	case SettingType::Float:
	{
		if (!in.is_number()) { outError = "expected a number"; return false; }
		const double v = in.get<double>();
		if (d.hasRange && (v < d.minValue || v > d.maxValue))
		{
			outError = "out of range (" + std::to_string(d.minValue) + " … "
			         + std::to_string(d.maxValue) + ")";
			return false;
		}
		outChanged = (cfg.*(d.pf) != static_cast<float>(v));
		cfg.*(d.pf) = static_cast<float>(v);
		return true;
	}
	case SettingType::String:
	{
		if (!in.is_string()) { outError = "expected a string"; return false; }
		std::string v = in.get<std::string>();
		outChanged = (cfg.*(d.ps) != v);
		cfg.*(d.ps) = std::move(v);
		return true;
	}
	case SettingType::Enum:
	{
		int v = -1;
		if (in.is_number_integer())
		{
			v = in.get<int>();
			if (v < 0 || v >= static_cast<int>(d.options.size()))
			{
				outError = "not one of the " + std::to_string(d.options.size())
				         + " values (0 … " + std::to_string(d.options.size() - 1) + ")";
				return false;
			}
		}
		else if (in.is_string())
		{
			const std::string want = lowered(in.get<std::string>());
			for (std::size_t i = 0; i < d.options.size(); ++i)
				if (lowered(d.options[i]) == want) { v = static_cast<int>(i); break; }
			if (v < 0)
			{
				outError = "unknown value; expected one of: ";
				for (std::size_t i = 0; i < d.options.size(); ++i)
				{
					if (i) outError += ", ";
					outError += d.options[i];
				}
				return false;
			}
		}
		else
		{
			outError = "expected a name or an index";
			return false;
		}
		outChanged = (cfg.*(d.pi) != v);
		cfg.*(d.pi) = v;
		return true;
	}
	}
	outError = "unsupported setting type";
	return false;
}

} // namespace HE::Ed
