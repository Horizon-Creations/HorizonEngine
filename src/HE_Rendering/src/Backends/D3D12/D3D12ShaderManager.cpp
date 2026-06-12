#include "Backends/D3D12/D3D12ShaderManager.h"

ShaderHandle D3D12ShaderManager::load(const char* path, HE::ShaderType type)
{
    (void)path;
    (void)type;
    return { 0, false };
}

void D3D12ShaderManager::release(ShaderHandle handle)
{
    (void)handle;
}

bool D3D12ShaderManager::compile(ShaderHandle& handle)
{
    (void)handle;
    return false;
}

ShaderProgramHandle D3D12ShaderManager::createProgram(const std::vector<ShaderHandle>& shaders)
{
    (void)shaders;
    return { 0, false };
}

void D3D12ShaderManager::releaseProgram(ShaderProgramHandle handle)
{
    (void)handle;
}

void D3D12ShaderManager::cleanup()
{
    // TODO: cleanup D3D12 shader resources
}