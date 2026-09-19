#pragma once
#include "HorizonRendering/ShaderManager.h"
// windows.h FIRST with the usual guards, so d3d11.h and every consumer see the
// same macro state (winuser.h's GetMessage rename, min/max) — the trap the
// WARP tests in he_tests already document.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <string>
#include <unordered_map>

// File-based HLSL shader manager for the D3D11 backend, the sibling of
// VulkanShaderManager: load() reads the source, compile() runs it through
// D3DCompile (vs_5_0 / ps_5_0 / cs_5_0, entry "main" — the profiles both D3D
// renderers use) and, once a device is set, creates the ID3D11*Shader object.
//
// Ownership: every ID3DBlob and every shader object lives in the maps below as
// a ComPtr. cleanup() drops them all — after it no GPU object created through
// this manager is referenced by it any more (he_tests holds its own ComPtr copy
// across cleanup() and reads the refcount to prove that). The device is kept:
// a manager is reusable after cleanup(), which is what a hot-reload loop needs.
//
// Note: D3D11Renderer does not route its shaders through this class — it holds
// them itself (ComPtr members of D3D11RendererImpl, released in Shutdown()).
// Pipeline state (input layouts, blend/raster/depth states) is not shader-owned
// on D3D11 and stays with the renderer.
class D3D11ShaderManager : public ShaderManager
{
public:
	ShaderHandle load(const char* path, HE::ShaderType type) override;
	void release(ShaderHandle handle) override;
	bool compile(ShaderHandle& handle) override;

	ShaderProgramHandle createProgram(const std::vector<ShaderHandle>& shaders) override;
	void releaseProgram(ShaderProgramHandle handle) override;

	void cleanup() override;

	// D3D11-specific: without a device compile() stops after D3DCompile (bytecode
	// only), with one it also creates the shader object. Kept across cleanup().
	void setDevice(ID3D11Device* device) { m_device = device; }

	// Introspection for tests and callers that build pipelines themselves.
	// Null when the id is unknown or that stage was never produced.
	ID3DBlob* blobFor(unsigned int id) const;
	ID3D11DeviceChild* shaderFor(unsigned int id) const;
	size_t loadedShaderCount() const { return m_loadedShaders.size(); }
	size_t loadedProgramCount() const { return m_loadedPrograms.size(); }

private:
	Microsoft::WRL::ComPtr<ID3D11Device> m_device;
	// id → HLSL source text (filled by load(), consumed by compile()).
	std::unordered_map<unsigned int, std::string> m_sources;
	// id → compiled bytecode (D3D12 keeps blobs the same way; D3D11 needs them
	// for CreateInputLayout, so they stay alive next to the shader object).
	std::unordered_map<unsigned int, Microsoft::WRL::ComPtr<ID3DBlob>> m_blobs;
	// id → ID3D11VertexShader / PixelShader / ComputeShader (all ID3D11DeviceChild).
	std::unordered_map<unsigned int, Microsoft::WRL::ComPtr<ID3D11DeviceChild>> m_shaders;
	std::vector<ShaderHandle> m_loadedShaders;
	std::vector<ShaderProgramHandle> m_loadedPrograms;
	unsigned int m_nextShaderId = 1;
	unsigned int m_nextProgramId = 1;
};
