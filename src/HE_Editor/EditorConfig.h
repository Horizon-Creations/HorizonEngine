#pragma once

// ─── The editor's own settings, on their own ─────────────────────────────────
// EditorConfig and the mode enum it carries used to live in EditorApplication.h,
// between the SDL dialog bridge and the AppContext. They are moved out here for
// one reason: EditorApplication.h pulls in ImGui, SDL, the renderer factory, the
// physics world and two dozen other headers, and that put the editor's settings
// out of reach of anything that is not the editor — including the test binary,
// which is where "does settings_set actually land in the field it names" is a
// question that can be asked at all (EditorSettingsCatalog.h).
//
// Nothing else changed: EditorApplication.h includes this file at the spot the
// struct used to occupy, so every translation unit that saw these two names
// before still sees them.

#include <string>

enum class EditorMode
{
	View,
	Landscape,
};

struct EditorConfig
{
	bool KeepCPUAssets = false;
	bool KeepCPUAssetsInfoAcknowledged = false;
	int  ContentBrowserRefreshRate = 60;

	// Content browser tree-panel width (-1 = auto on first frame)
	float CbTreeWidth = -1.0f;

	// Collaboration: announce a hosted session on the local network, and listen
	// for other people's. On by default — it is the only route that works when
	// the router will not forward a port, and it carries no secret (the join
	// code is never announced). Off is for networks you would rather not be
	// visible on at all.
	bool CollabLanDiscovery = true;

	// Collaboration: also send the BIG assets — meshes, textures, audio, fonts —
	// over the session instead of leaving them to source control. Off by default,
	// and deliberately so: those are the files that are measured in hundreds of
	// megabytes, and somebody on a metered or slow connection has to be able to
	// say no.
	//
	// It is the HOST's decision for the session, because it is the host that
	// decides what the session carries; a guest whose own setting disagrees is
	// refused at the join and asked first (see JoinRejectReason). It cannot be
	// changed while a session is running — half a session with one rule and half
	// with another is a set of peers that quietly hold different files.
	bool CollabSyncLargeAssets = false;

	// The biggest single asset this editor will send or accept over a session,
	// in megabytes. It was a hard 64 shared with the session snapshot, which is
	// the wrong number to be fixed: with large-asset sync on, a 90 MB cooked
	// mesh is an ordinary file and simply never arrived — refused, not
	// truncated, and only a log line said so.
	//
	// Raising it is not free, which is why the setting explains itself rather
	// than just offering a number: a transfer is held WHOLE in memory on both
	// ends and queued a second time on the sender, and while it is going out it
	// is ahead of everything else the session wants to say. The cap is also the
	// only thing bounding what one peer can make another allocate.
	int CollabMaxAssetMB = 64;

	// The MCP bridge: let an external client (a Claude instance, through the
	// stdio shim) drive this editor — read what is open, and from a later step
	// on place and move objects in the scene.
	//
	// OFF by default, and this default is the security model's first line. When
	// it is on, a listener on 127.0.0.1 accepts anything that can read the
	// endpoint file's token, and that file is readable by this user's processes.
	// That is a decision a human makes for a session, not one they inherit from
	// an installer. HE_MCP=1 turns it on for a headless run without touching the
	// stored config; HE_MCP_PORT pins the port.
	bool McpServerEnabled = false;
	// 0 = let the OS pick. The port is published in the endpoint file, so nobody
	// has to know it in advance; pinning one is for a client that cannot read
	// the file.
	int  McpPort = 0;

	// Preferences (Edit > Preferences)
	float UiFontScale       = 1.0f;   // global editor font scale (style.FontScaleMain)
	float EditorCameraSpeed = 6.0f;   // editor fly-camera speed, world units/second
	float MaxFps            = 0.0f;   // VSync-off frame cap (0 = unlimited); paces mouse-look
	// Pointer-device grammar for the preview panes (see EditorInput.h):
	// 0 = Auto (detect trackpad), 1 = Mouse, 2 = Trackpad.
	int   PointerInput      = 0;
	// Gamepad deadzones, mirrored into Input each frame (Input owns the live
	// values; these are just what survives a restart). Sticks radial, trigger
	// scalar — see Input.h for why the shapes differ.
	float GamepadStickDeadzone   = 0.15f;
	float GamepadTriggerDeadzone = 0.05f;

	// Post-process: bloom (pushed to the renderer each frame via SetBloomSettings)
	bool  BloomEnabled   = true;
	float BloomThreshold = 1.0f;
	float BloomIntensity = 0.6f;

	// Post-process: SSAO (pushed to the renderer each frame via SetSSAOSettings)
	bool  SSAOEnabled   = true;
	float SSAORadius    = 0.5f;   // hemisphere sampling radius, view-space units
	float SSAOIntensity = 1.0f;   // 0 = off … 1 = full ambient occlusion
	int   SSAOMethod    = 0;      // AO method: 0 = SSAO, 1 = HBAO, 2 = GTAO (planned)

	// Anti-aliasing (pushed each frame via SetAntiAliasingSettings, see
	// docs/anti-aliasing-plan.md). `AntiAliasing` holds an HE AAMethod int —
	// 0 Off, 1 FXAA, 2 SMAA, 3 TAA, 4 MetalFX — and defaults to FXAA because
	// that is what the engine did before the setting existed. Modes the backend
	// cannot do are greyed out in Preferences and fall back at push time.
	int   AntiAliasing        = 1;
	float AASharpness         = 0.35f; // temporal modes only
	float RenderScale         = 1.0f;  // < 1 upscales, > 1 supersamples
	bool  SpecularAA          = true;  // roughness regularization (shading aliasing)
	float SpecularAAStrength  = 1.0f;

	// GPU weather particles: simulate rain/snow on the GPU (transform feedback / compute)
	// instead of the CPU pool. Default on; the backend's supportsGpuParticles gates it
	// (GL + Metal = yes, so it's the path used unless the user turns it off).
	bool  GpuParticles  = true;

	// Render path (pushed to the renderer each frame via SetRenderPath): 0 =
	// Forward (default), 1 = Deferred (G-buffer + fullscreen lighting resolve,
	// Metal + OpenGL). The backend's supportsDeferredRendering gates it.
	int   RenderPath = 0;

	// Screen-space reflections (pushed each frame via SetSSRSettings). v1 only
	// effective on Metal in the deferred render path; supportsScreenSpaceReflections
	// gates the toggle. Off by default (like GI).
	bool  SSREnabled      = false;
	float SSRIntensity    = 1.0f;
	float SSRMaxRoughness = 0.6f;
	int   SSRQuality      = 1;   // 0 Low (16 steps, raw) / 1 Med (32+blur) / 2 High (64+glossy)

	// Global Illumination: ray-traced DDGI (pushed to the renderer each frame via
	// SetGISettings). Metal-only; the backend's supportsGlobalIllumination gates
	// it (needs a ray-tracing-capable GPU + macOS 12+). Off by default — strictly
	// opt-in. When on and supported, COMPLETELY REPLACES CSM shadows and AO/ambient.
	bool  GlobalIlluminationEnabled = false;
	float GIIndirectIntensity       = 1.0f;
	float GILightRadius             = 0.5f;   // degrees — sun angular radius (shadow penumbra softness)

	// Ray-traced GI reflections (pushed each frame via SetGIReflectionSettings).
	// Real scene rays against the GI acceleration structure instead of the sky
	// cubemap; supportsGIReflections gates the toggle (Metal tile deferred +
	// HW RT, or an OpenGL 4.3 context on Windows/Linux).
	bool  GIReflectionsEnabled = false;
	float GIReflIntensity      = 1.0f;
	float GIReflMaxRoughness   = 0.6f;
	int   GIReflQuality        = 1;   // 0 raw / 1 blur / 2 glossy+temporal (SSR-style tiers)
	int   GIReflBounces        = 1;   // 1-4 mirror bounces (Metal only — the GL kernel traces one segment)
	// Post-trace blur. TEST DEFAULT OFF: the blur is meant to shrink as the tier
	// rises, and the fastest way to tell whether it is what a soft reflection is
	// actually made of is to look with it gone. Flip the checkbox in Preferences
	// (or this default) to bring it back.
	bool  GIReflBlur           = false;

	// NOTE: environment / sky settings (day-night, sun, moon, clouds, fog, night
	// sky, wind) are scene data now — they live on the World root entity as an
	// EnvironmentComponent, are edited in its Details panel and persist with the
	// scene. They are no longer editor preferences.

	// Quick Settings = the engine settings the user pinned in Preferences. Stored
	// as a comma-separated list of stable setting keys (see DrawEngineSettings).
	std::string QuickSettingsFavorites = "backend,vsync,grid,bloom,ssao";

	EditorMode mode = EditorMode::View;

	// New-landscape creation-form parameters. Transient (not serialised) — shared
	// here so the renderer can draw a 3D grid preview of the terrain-to-be while
	// the Landscape creation form is open. Mirrors TerrainComponent's noise fields.
	struct NewTerrainParams
	{
		float sizeX       = 100.0f;
		float sizeZ       = 100.0f;
		int   resolution  = 128;
		float heightScale = 20.0f;
		int   seed        = 0;     // 0 = flat
		int   octaves     = 4;
		float frequency   = 1.0f;
		float lacunarity  = 2.0f;
		float gain        = 0.5f;
	};
	NewTerrainParams newTerrain;

	std::string modeString() const
	{
		switch (mode)
		{
		case EditorMode::View:      return "View";
		case EditorMode::Landscape: return "Landscape";
		default:                   return "Unknown";
		}
	}
};
