#include "Backends/D3D11/D3D11ShaderManager.h"
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

ShaderHandle D3D11ShaderManager::load(const char* path, HE::ShaderType type)
{
    if (!fs::exists(path))
    {
        HE_LOG_ERROR(RHI, "%s", (std::string("D3D11ShaderManager: shader file not found: ") + path).c_str());
        return { 0, false };
    }
    std::ifstream file(path);
    if (!file.is_open())
    {
        HE_LOG_ERROR(RHI, "%s", (std::string("D3D11ShaderManager: failed to open shader file: ") + path).c_str());
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

void D3D11ShaderManager::release(ShaderHandle handle)
{
    // Drop the GPU objects AND the tracking entry — a manager that only forgets
    // the map would keep re-releasing (and growing) across a hot-reload loop.
    m_shaders.erase(handle.id);
    m_blobs.erase(handle.id);
    m_sources.erase(handle.id);
    m_loadedShaders.erase(
        std::remove_if(m_loadedShaders.begin(), m_loadedShaders.end(),
                       [&](const ShaderHandle& h) { return h.id == handle.id; }),
        m_loadedShaders.end());
}

bool D3D11ShaderManager::compile(ShaderHandle& handle)
{
    auto src = m_sources.find(handle.id);
    if (src == m_sources.end())
    {
        HE_LOG_ERROR(RHI, "%s", "D3D11ShaderManager: compile() on an unknown shader handle");
        return false;
    }

    Microsoft::WRL::ComPtr<ID3DBlob> blob, err;
    const HRESULT hr = D3DCompile(src->second.c_str(), src->second.size(), nullptr, nullptr, nullptr,
                                  "main", profileFor(handle.type), 0, 0, &blob, &err);
    if (FAILED(hr) || !blob)
    {
        std::string msg = "D3D11ShaderManager: shader compilation failed";
        if (err) msg += ": " + std::string(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize());
        HE_LOG_ERROR(RHI, "%s", msg.c_str());
        return false;
    }

    if (m_device)
    {
        Microsoft::WRL::ComPtr<ID3D11DeviceChild> obj;
        HRESULT chr = E_FAIL;
        switch (handle.type)
        {
            case HE::ShaderType::Vertex:
            {
                Microsoft::WRL::ComPtr<ID3D11VertexShader> vs;
                chr = m_device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &vs);
                obj = vs;
                break;
            }
            case HE::ShaderType::Fragment:
            {
                Microsoft::WRL::ComPtr<ID3D11PixelShader> ps;
                chr = m_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &ps);
                obj = ps;
                break;
            }
            case HE::ShaderType::Compute:
            {
                Microsoft::WRL::ComPtr<ID3D11ComputeShader> cs;
                chr = m_device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &cs);
                obj = cs;
                break;
            }
        }
        if (FAILED(chr) || !obj)
        {
            HE_LOG_ERROR(RHI, "%s", "D3D11ShaderManager: shader object creation failed");
            return false;
        }
        m_shaders[handle.id] = obj;
    }
    else
    {
        HE_LOG_WARN(RHI, "%s", "D3D11ShaderManager: device not set, bytecode compiled but no shader object created");
    }

    m_blobs[handle.id] = blob;
    handle.ready = true;
    // Keep the tracked copy in step, so cleanup() sees the same state the caller does.
    for (auto& h : m_loadedShaders)
        if (h.id == handle.id) h.ready = true;
    return true;
}

ShaderProgramHandle D3D11ShaderManager::createProgram(const std::vector<ShaderHandle>& shaders)
{
    // D3D11 has no linked program object: stages are bound one by one at draw
    // time. A program is the grouping of compiled stages, nothing on the GPU.
    for (const auto& s : shaders)
    {
        if (!s.ready || m_blobs.find(s.id) == m_blobs.end())
        {
            HE_LOG_ERROR(RHI, "%s", "D3D11ShaderManager: createProgram() with an uncompiled shader");
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

void D3D11ShaderManager::releaseProgram(ShaderProgramHandle handle)
{
    m_loadedPrograms.erase(
        std::remove_if(m_loadedPrograms.begin(), m_loadedPrograms.end(),
                       [&](const ShaderProgramHandle& p) { return p.id == handle.id; }),
        m_loadedPrograms.end());
}

void D3D11ShaderManager::cleanup()
{
    // Programs first (they reference shaders), then every shader; the containers
    // are emptied outright afterwards so nothing survives that the loops missed.
    // The ComPtr maps are what actually holds GPU references — clearing them
    // releases every blob and shader object this manager ever created. The
    // device stays: the manager is reusable after cleanup().
    m_loadedPrograms.clear();
    m_loadedShaders.clear();
    m_shaders.clear();
    m_blobs.clear();
    m_sources.clear();
}

ID3DBlob* D3D11ShaderManager::blobFor(unsigned int id) const
{
    auto it = m_blobs.find(id);
    return it == m_blobs.end() ? nullptr : it->second.Get();
}

ID3D11DeviceChild* D3D11ShaderManager::shaderFor(unsigned int id) const
{
    auto it = m_shaders.find(id);
    return it == m_shaders.end() ? nullptr : it->second.Get();
}
