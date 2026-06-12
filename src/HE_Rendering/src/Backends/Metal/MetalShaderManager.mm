#include "Backends/Metal/MetalShaderManager.h"
#include <sstream>

#import <Metal/Metal.h>

MetalShaderManager::~MetalShaderManager()
{
	cleanup();
}

ShaderHandle MetalShaderManager::load(const char* path, HE::ShaderType type)
{
	if (!fs::exists(path))
	{
		Logger::Log(Logger::LogLevel::Error, (std::string("MetalShaderManager: shader file not found: ") + path).c_str());
		return { 0, false };
	}
	std::ifstream file(path);
	if (!file.is_open())
	{
		Logger::Log(Logger::LogLevel::Error, (std::string("MetalShaderManager: failed to open shader file: ") + path).c_str());
		return { 0, false };
	}
	std::stringstream buffer;
	buffer << file.rdbuf();

	ShaderHandle handle;
	handle.id   = m_nextId++;
	handle.type = type;
	m_pendingSources[handle.id] = buffer.str();
	return handle;
}

void MetalShaderManager::release(ShaderHandle handle)
{
	m_pendingSources.erase(handle.id);
	auto it = m_functions.find(handle.id);
	if (it != m_functions.end())
	{
		CFBridgingRelease(it->second);
		m_functions.erase(it);
	}
}

bool MetalShaderManager::compile(ShaderHandle& handle)
{
	if (!m_device)
	{
		Logger::Log(Logger::LogLevel::Error, "MetalShaderManager: no MTLDevice set");
		return false;
	}
	auto srcIt = m_pendingSources.find(handle.id);
	if (srcIt == m_pendingSources.end())
	{
		Logger::Log(Logger::LogLevel::Error, "MetalShaderManager: compile called on unknown handle");
		return false;
	}

	@autoreleasepool
	{
		id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
		NSError* error = nil;
		NSString* source = [NSString stringWithUTF8String:srcIt->second.c_str()];
		id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
		if (!library)
		{
			Logger::Log(Logger::LogLevel::Error,
				(std::string("MetalShaderManager: shader compilation failed: ")
				 + (error ? [[error localizedDescription] UTF8String] : "unknown error")).c_str());
			return false;
		}

		// Convention: entry points are named by stage, matching the engine's
		// ShaderType — vertexMain / fragmentMain / computeMain.
		const char* entry = nullptr;
		switch (handle.type)
		{
			case HE::ShaderType::Vertex:   entry = "vertexMain";   break;
			case HE::ShaderType::Fragment: entry = "fragmentMain"; break;
			case HE::ShaderType::Compute:  entry = "computeMain";  break;
		}
		id<MTLFunction> fn = [library newFunctionWithName:[NSString stringWithUTF8String:entry]];
		if (!fn)
		{
			Logger::Log(Logger::LogLevel::Error,
				(std::string("MetalShaderManager: entry point not found: ") + entry).c_str());
			return false;
		}

		m_functions[handle.id] = (void*)CFBridgingRetain(fn);
		m_pendingSources.erase(srcIt);
		handle.ready = true;
		return true;
	}
}

ShaderProgramHandle MetalShaderManager::createProgram(const std::vector<ShaderHandle>& shaders)
{
	// Metal has no link step — a "program" is just the validated set of stage
	// functions. The render-pipeline state is built later, when the vertex
	// layout and attachment formats are known.
	for (const auto& shader : shaders)
	{
		if (!shader.ready || !m_functions.count(shader.id))
		{
			Logger::Log(Logger::LogLevel::Error, "MetalShaderManager: createProgram called with uncompiled shader");
			return { 0, false };
		}
	}
	ShaderProgramHandle program;
	program.id      = m_nextId++;
	program.ready   = true;
	program.shaders = shaders;
	return program;
}

void MetalShaderManager::releaseProgram(ShaderProgramHandle handle)
{
	for (const auto& shader : handle.shaders)
		release(shader);
}

void MetalShaderManager::cleanup()
{
	for (auto& [id, fn] : m_functions)
		CFBridgingRelease(fn);
	m_functions.clear();
	m_pendingSources.clear();
}
