#include "Backends/D3D12/D3D12ShaderManager.h"
#include <Diagnostics/Logger.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace
{
const char* profileFor(HE::ShaderType type)
{
    switch (type)
    {
        case HE::ShaderType::Vertex:   return "vs_5_0";
        case HE::ShaderType::Fragment: return "ps_5_0";
        case HE::ShaderType::Compute:  return "cs_5_0";
    }
    return "vs_5_0";
}
} // namespace

ShaderHandle D3D12ShaderManager::load(const char* path, HE::ShaderType type)
{
    if (!fs::exists(path))
    {
        HE_LOG_ERROR(RHI, "%s", (std::string("D3D12ShaderManager: shader file not found: ") + path).c_str());
        return { 0, false };
    }
    std::ifstream file(path);
    if (!file.is_open())
    {
        HE_LOG_ERROR(RHI, "%s", (std::string("D3D12ShaderManager: failed to open shader file: ") + path).c_str());
        return { 0, false };
    }
    std::stringstream buffer;
    buffer << file.rdbuf();

    ShaderHandle handle;
    handle.type = type;
    handle.id = m_nextShaderId++;
    m_sources[handle.id] = buffer.str();
    m_loadedShaders.push_back(handle);
    return handle;
}

void D3D12ShaderManager::release(ShaderHandle handle)
{
    // Drop the blob AND the tracking entry — a manager that only forgets the
    // map would keep re-releasing (and growing) across a hot-reload loop.
    m_blobs.erase(handle.id);
    m_sources.erase(handle.id);
    m_loadedShaders.erase(
        std::remove_if(m_loadedShaders.begin(), m_loadedShaders.end(),
                       [&](const ShaderHandle& h) { return h.id == handle.id; }),
        m_loadedShaders.end());
}

bool D3D12ShaderManager::compile(ShaderHandle& handle)
{
    auto src = m_sources.find(handle.id);
    if (src == m_sources.end())
    {
        HE_LOG_ERROR(RHI, "%s", "D3D12ShaderManager: compile() on an unknown shader handle");
        return false;
    }

    Microsoft::WRL::ComPtr<ID3DBlob> blob, err;
    const HRESULT hr = D3DCompile(src->second.c_str(), src->second.size(), nullptr, nullptr, nullptr,
                                  "main", profileFor(handle.type), 0, 0, &blob, &err);
    if (FAILED(hr) || !blob)
    {
        std::string msg = "D3D12ShaderManager: shader compilation failed";
        if (err) msg += ": " + std::string(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize());
        HE_LOG_ERROR(RHI, "%s", msg.c_str());
        return false;
    }

    m_blobs[handle.id] = blob;
    handle.ready = true;
    // Keep the tracked copy in step, so cleanup() sees the same state the caller does.
    for (auto& h : m_loadedShaders)
        if (h.id == handle.id) h.ready = true;
    return true;
}

ShaderProgramHandle D3D12ShaderManager::createProgram(const std::vector<ShaderHandle>& shaders)
{
    // D3D12 has no linked program object: the stages meet in the PSO, which the
    // renderer builds. A program is the grouping of compiled stages, nothing on
    // the GPU.
    for (const auto& s : shaders)
    {
        if (!s.ready || m_blobs.find(s.id) == m_blobs.end())
        {
            HE_LOG_ERROR(RHI, "%s", "D3D12ShaderManager: createProgram() with an uncompiled shader");
            return { 0, false };
        }
    }
    ShaderProgramHandle program;
    program.id = m_nextProgramId++;
    program.shaders = shaders;
    program.ready = true;
    m_loadedPrograms.push_back(program);
    return program;
}

void D3D12ShaderManager::releaseProgram(ShaderProgramHandle handle)
{
    m_loadedPrograms.erase(
        std::remove_if(m_loadedPrograms.begin(), m_loadedPrograms.end(),
                       [&](const ShaderProgramHandle& p) { return p.id == handle.id; }),
        m_loadedPrograms.end());
}

void D3D12ShaderManager::cleanup()
{
    // Programs first (they reference shaders), then every shader. The ComPtr
    // map is what actually holds GPU-facing references — clearing it releases
    // every bytecode blob this manager ever compiled. Reusable afterwards.
    m_loadedPrograms.clear();
    m_loadedShaders.clear();
    m_blobs.clear();
    m_sources.clear();
}

ID3DBlob* D3D12ShaderManager::blobFor(unsigned int id) const
{
    auto it = m_blobs.find(id);
    return it == m_blobs.end() ? nullptr : it->second.Get();
}
