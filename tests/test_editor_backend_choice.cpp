#include "doctest.h"
#include "EditorBackendChoice.h"

// HE_DUMP_RHI against the config's RHI (Thema 124, A6). The editor creates its
// renderer with resolveEditorBackend() and keeps that answer for the ImGui
// backend and every per-frame branch; the crash this guards against was the
// config RHI coming back after OnInit while the renderer was the forced one.

using HE::RendererBackend;
using HE::Ed::resolveEditorBackend;

TEST_CASE("resolveEditorBackend: no override keeps the configured backend")
{
	for (RendererBackend cfg : { RendererBackend::OpenGL, RendererBackend::Vulkan, RendererBackend::D3D11,
	                             RendererBackend::D3D12, RendererBackend::Metal, RendererBackend::Software })
	{
		CHECK(resolveEditorBackend(cfg, nullptr) == cfg);
		CHECK(resolveEditorBackend(cfg, "") == cfg);
	}
}

TEST_CASE("resolveEditorBackend: every spelling wins over the config")
{
	// Config OpenGL (RHI 0) is the case that crashed: the override has to be
	// the answer, not the config.
	const RendererBackend cfg = RendererBackend::OpenGL;
	CHECK(resolveEditorBackend(cfg, "Metal")    == RendererBackend::Metal);
	CHECK(resolveEditorBackend(cfg, "Vulkan")   == RendererBackend::Vulkan);
	CHECK(resolveEditorBackend(cfg, "D3D11")    == RendererBackend::D3D11);
	CHECK(resolveEditorBackend(cfg, "D3D12")    == RendererBackend::D3D12);
	CHECK(resolveEditorBackend(cfg, "Software") == RendererBackend::Software);
	CHECK(resolveEditorBackend(cfg, "SW")       == RendererBackend::Software);

	// …and the other way round, back to GL from a non-GL config.
	CHECK(resolveEditorBackend(RendererBackend::Metal, "OpenGL") == RendererBackend::OpenGL);
	CHECK(resolveEditorBackend(RendererBackend::Metal, "GL")     == RendererBackend::OpenGL);
}

TEST_CASE("resolveEditorBackend: an unknown name falls back to the config")
{
	// Names are case-sensitive, as they always were; a typo keeps the user's
	// own backend instead of guessing.
	CHECK(resolveEditorBackend(RendererBackend::D3D12, "metal")  == RendererBackend::D3D12);
	CHECK(resolveEditorBackend(RendererBackend::D3D12, "Metal ") == RendererBackend::D3D12);
	CHECK(resolveEditorBackend(RendererBackend::D3D12, "DX12")   == RendererBackend::D3D12);
}
