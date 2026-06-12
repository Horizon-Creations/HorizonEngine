#include "HorizonRendering/ShaderManager.h"

class D3D12ShaderManager : public ShaderManager
{
	public:
	ShaderHandle load(const char* path, HE::ShaderType type) override;
	void release(ShaderHandle handle) override;
	bool compile(ShaderHandle& handle) override;

	ShaderProgramHandle createProgram(const std::vector<ShaderHandle>& shaders) override;
	void releaseProgram(ShaderProgramHandle handle) override;

	void cleanup() override;
};