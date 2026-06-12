#include "Backends/D3D11/D3D11ShaderManager.h"

ShaderHandle D3D11ShaderManager::load(const char* path, HE::ShaderType type)
{
    (void)path;
    (void)type;
    return { 0, false };
}

void D3D11ShaderManager::release(ShaderHandle handle)
{
    (void)handle;
}

bool D3D11ShaderManager::compile(ShaderHandle& handle)
{
    (void)handle;
    return false;
}

ShaderProgramHandle D3D11ShaderManager::createProgram(const std::vector<ShaderHandle>& shaders)
{
    (void)shaders;
    return { 0, false };
}

void D3D11ShaderManager::releaseProgram(ShaderProgramHandle handle)
{
    (void)handle;
}

void D3D11ShaderManager::cleanup()
{
    // TODO: cleanup D3D11 shader resources
}