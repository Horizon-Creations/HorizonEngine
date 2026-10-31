#pragma once
#include <cstdint>
#include <string>

// ─── A tile landscape: the world-scale mode (Thema 326) ──────────────────────
// Marks an entity as the scene's world-scale landscape. It does NOT replace TerrainComponent, which stays the
// single-landscape case; a scene can have either. The data is a pack file (LandscapeSource.h, written by the editor's
// import in step 6 or by a tool), streamed around the camera by HE::LandscapeRuntime (LandscapeRuntime.h).
//
// Not serialized yet: SceneSerializer, the component key table, the inspector and the editor's import come with
// the editor step. Until then a scene gets one by code (tests, tools, a game's setup).
struct LandscapeWorldComponent {
	bool        enabled       = true;
	std::string pack;                    // project-relative or absolute path of the pack file
	float       loadRadius    = 8000.0f; // metres around the camera whose tiles are kept in memory
	uint32_t    budgetMB      = 350;     // decoded tiles in memory, resident and loading
	// Heights are 16 bit; a stored value v is metres = heightMinM + v / 65535 * heightScaleM. The pack does not carry this yet.
	float       heightMinM    = 0.0f;
	float       heightScaleM  = 3000.0f;
};
