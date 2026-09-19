#pragma once
#include "HorizonRendering/ShaderManager.h"
// windows.h FIRST with the usual guards, so d3dcommon.h and every consumer see
// the same macro state (winuser.h's GetMessage rename, min/max) — the trap the
// WARP tests in he_tests already document.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3dcommon.h>
#include <wrl/client.h>
#include <string>
#include <unordered_map>

// File-based HLSL shader manager for the D3D12 backend, the sibling of
// VulkanShaderManager: load() reads the source, compile() runs it through
// D3DCompile (vs_5_0 / ps_5_0 / cs_5_0, entry "main" — the profiles both D3D
// renderers use). D3D12 has no per-stage shader object: the bytecode blob IS
// the shader, it goes straight into D3D12_GRAPHICS_PIPELINE_STATE_DESC. No
// device is needed here, and none is held.
//
// Ownership: every ID3DBlob lives in m_blobs as a ComPtr. cleanup() drops them
// all — after it no blob created through this manager is referenced by it any
// more (he_tests holds its own ComPtr copy across cleanup() and reads the
// refcount to prove that). The manager is reusable after cleanup().
//
// Note: D3D12Renderer does not route its shaders through this class — it holds
// its blobs, pipeline state objects and root signatures itself (ComPtr members
// of D3D12RendererImpl, released in Shutdown()). PSOs and root signatures are
// not shader-owned and are deliberately NOT managed here.
class D3D12ShaderManager : public ShaderManager
{
public:
	ShaderHandle load(const char* path, HE::ShaderType type) override;
	void release(ShaderHandle handle) override;
	bool compile(ShaderHandle& handle) override;

	ShaderProgramHandle createProgram(const std::vector<ShaderHandle>& shaders) override;
	void releaseProgram(ShaderProgramHandle handle) override;

	void cleanup() override;

	// Introspection for tests and callers that build PSOs themselves.
	// Null when the id is unknown or compile() never succeeded for it.
	ID3DBlob* blobFor(unsigned int id) const;
	size_t loadedShaderCount() const { return m_loadedShaders.size(); }
	size_t loadedProgramCount() const { return m_loadedPrograms.size(); }

private:
	// id → HLSL source text (filled by load(), consumed by compile()).
	std::unordered_map<unsigned int, std::string> m_sources;
	// id → compiled bytecode.
	std::unordered_map<unsigned int, Microsoft::WRL::ComPtr<ID3DBlob>> m_blobs;
	std::vector<ShaderHandle> m_loadedShaders;
	std::vector<ShaderProgramHandle> m_loadedPrograms;
	unsigned int m_nextShaderId = 1;
	unsigned int m_nextProgramId = 1;
};
