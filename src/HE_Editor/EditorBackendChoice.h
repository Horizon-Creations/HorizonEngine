#pragma once

// ── Which backend the editor runs on ─────────────────────────────────────────
// The config's RHI (Preferences ▸ Rendering API) is what the user picked; a
// headless run may force another one through HE_DUMP_RHI, so a verification
// screenshot can use the user's ACTUAL backend (e.g. Metal on macOS) instead
// of whatever happens to be persisted:
//
//   HE_DUMP_RHI=Metal|OpenGL|GL|Vulkan|D3D11|D3D12|Software|SW
//
// The result is the backend the renderer is created with, and from then on it
// is the ONLY answer to "which backend is this": the ImGui backend
// (EditorApplication::OnInit), EditorUI's per-frame NewFrame/RenderDrawData
// branch, flipY in every texture preview — all of them read it. Until Thema
// 124 the end of OnInit read the config RHI a second time and overwrote it, so
// config RHI 0 plus HE_DUMP_RHI=Metal created a Metal renderer and then drove
// ImGui_ImplOpenGL3_NewFrame without a GL context: SIGSEGV in the first UI
// frame. The dump path quits before that frame and never saw it.
//
// Header-only and free of ImGui so a test reads the environment the same way.

#include "Types/Enums.h"

#include <string_view>

namespace HE::Ed
{

// The backend the editor's renderer is created with: `dumpRhi` when it names
// one, `configured` otherwise (null, empty or unknown — a typo must not take
// the editor somewhere the user never chose).
inline HE::RendererBackend resolveEditorBackend(HE::RendererBackend configured, const char* dumpRhi)
{
	if (!dumpRhi || !*dumpRhi) return configured;
	const std::string_view s(dumpRhi);
	if (s == "Metal")               return HE::RendererBackend::Metal;
	if (s == "OpenGL" || s == "GL") return HE::RendererBackend::OpenGL;
	if (s == "Vulkan")              return HE::RendererBackend::Vulkan;
	if (s == "D3D11")               return HE::RendererBackend::D3D11;
	if (s == "D3D12")               return HE::RendererBackend::D3D12;
	// The CPU rasterizer, so a dump can witness what an application without a
	// GPU actually draws — the whole point of being able to force a backend.
	if (s == "Software" || s == "SW") return HE::RendererBackend::Software;
	return configured;
}

} // namespace HE::Ed
