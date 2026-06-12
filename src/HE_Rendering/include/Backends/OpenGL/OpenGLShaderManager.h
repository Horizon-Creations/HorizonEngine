#include "HorizonRendering/ShaderManager.h"

class OpenGLShaderManager : public ShaderManager
{
public:
	ShaderHandle load(const char* path, HE::ShaderType type) override;
	void release(ShaderHandle handle) override;
	bool compile(ShaderHandle& handle) override;

	ShaderProgramHandle createProgram(const std::vector<ShaderHandle>& shaders) override;
	void releaseProgram(ShaderProgramHandle handle) override;

	void cleanup() override;
private:
	std::vector<ShaderHandle> m_loadedShaders;
	std::vector<ShaderProgramHandle> m_loadedPrograms;
};