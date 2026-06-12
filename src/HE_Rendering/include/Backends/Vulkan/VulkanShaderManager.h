#include "HorizonRendering/ShaderManager.h"
#include <vulkan/vulkan.h>
#include <unordered_map>

class VulkanShaderManager : public ShaderManager
{
	public:
	ShaderHandle load(const char* path, HE::ShaderType type) override;
	void release(ShaderHandle handle) override;
	bool compile(ShaderHandle& handle) override;

	ShaderProgramHandle createProgram(const std::vector<ShaderHandle>& shaders) override;
	void releaseProgram(ShaderProgramHandle handle) override;

	void cleanup() override;

	// Vulkan-specific: set device for cleanup
	void setDevice(VkDevice device) { m_device = device; }

private:
	VkDevice m_device = VK_NULL_HANDLE;
	std::unordered_map<unsigned int, VkShaderModule> m_shaderModules;
	std::vector<ShaderHandle> m_loadedShaders;
	std::vector<ShaderProgramHandle> m_loadedPrograms;
};