#pragma once
#include "HorizonRendering/ShaderManager.h"
#include <unordered_map>

// Compiles Metal Shading Language source at runtime via MTLDevice
// newLibraryWithSource. Handles map to retained id<MTLFunction> objects
// stored internally; "programs" pair a vertex and a fragment function
// (pipeline-state creation happens later, when vertex layout is known).
class MetalShaderManager : public ShaderManager
{
public:
	MetalShaderManager() = default;
	~MetalShaderManager() override;

	// Must be called by MetalRenderer before any load(). device is id<MTLDevice>.
	void setDevice(void* device) { m_device = device; }

	ShaderHandle load(const char* path, HE::ShaderType type) override;
	void release(ShaderHandle handle) override;
	bool compile(ShaderHandle& handle) override;

	ShaderProgramHandle createProgram(const std::vector<ShaderHandle>& shaders) override;
	void releaseProgram(ShaderProgramHandle handle) override;

	void cleanup() override;

private:
	void* m_device = nullptr;            // id<MTLDevice> (borrowed from MetalRenderer)
	unsigned int m_nextId = 1;

	// id → MSL source, pending until compile()
	std::unordered_map<unsigned int, std::string> m_pendingSources;
	// id → id<MTLFunction> (retained via CFBridgingRetain)
	std::unordered_map<unsigned int, void*> m_functions;
};
