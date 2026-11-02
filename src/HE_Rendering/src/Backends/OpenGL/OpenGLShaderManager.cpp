#include "Backends/OpenGL/OpenGLShaderManager.h"
#include <glad/glad.h>
#include <Diagnostics/Logger.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

// File-local — the shared HorizonRendering/ShaderManager.h used to leak this
// alias into every consumer of the public header (down to HE_Editor); each
// backend shader manager that wants `fs::` declares it for itself instead.
namespace fs = std::filesystem;

ShaderHandle OpenGLShaderManager::load(const char* path, HE::ShaderType type)
{
	if (!fs::exists(path))
	{
		HE_LOG_ERROR(RHI, "%s", (std::string("OpenGLShaderManager: shader file not found: ") + path).c_str());
		return { 0, false };// invalid handle
	}
	std::ifstream file(path);
	if (!file.is_open())
	{
		HE_LOG_ERROR(RHI, "%s", (std::string("OpenGLShaderManager: failed to open shader file: ") + path).c_str());
		return { 0, false };// invalid handle
	}
	std::stringstream buffer;
	buffer << file.rdbuf();
	std::string source = buffer.str();
	const char* src = source.c_str();
	ShaderHandle handle;
	handle.type = type;
	switch (handle.type)
	{
		case HE::ShaderType::Vertex:
			handle.id = glCreateShader(GL_VERTEX_SHADER);
			break;
		case HE::ShaderType::Fragment:
			handle.id = glCreateShader(GL_FRAGMENT_SHADER);
			break;
		case HE::ShaderType::Compute:
			handle.id = glCreateShader(GL_COMPUTE_SHADER);
			break;
	}
	glShaderSource(handle.id, 1, &src, nullptr);
	m_loadedShaders.push_back(handle);
	return handle;
}

void OpenGLShaderManager::release(ShaderHandle handle)
{
	glDeleteShader(handle.id);
	m_loadedShaders.erase(
		std::remove_if(m_loadedShaders.begin(), m_loadedShaders.end(),
			[&](const ShaderHandle& h) { return h.id == handle.id; }),
		m_loadedShaders.end());
}

bool OpenGLShaderManager::compile(ShaderHandle& handle)
{
	glCompileShader(handle.id);
	GLint success;
	glGetShaderiv(handle.id, GL_COMPILE_STATUS, &success);
	if (!success)
	{
		GLchar infoLog[512];
		glGetShaderInfoLog(handle.id, 512, nullptr, infoLog);
		HE_LOG_ERROR(RHI, "%s", (std::string("OpenGLShaderManager: shader compilation failed: ") + infoLog).c_str());
		return false;
	}
	handle.ready = true;
	return true;
}

ShaderProgramHandle OpenGLShaderManager::createProgram(const std::vector<ShaderHandle>& shaders)
{
	ShaderProgramHandle program;
	program.id = glCreateProgram();
	for (const auto& shader : shaders)
		glAttachShader(program.id, shader.id);
	glLinkProgram(program.id);
	GLint success;
	glGetProgramiv(program.id, GL_LINK_STATUS, &success);
	if (!success)
	{
		GLchar infoLog[512];
		glGetProgramInfoLog(program.id, 512, nullptr, infoLog);
		HE_LOG_ERROR(RHI, "%s", (std::string("OpenGLShaderManager: shader program linking failed: ") + infoLog).c_str());
		return { 0, false };// invalid handle
	}
	program.ready = true;
	program.shaders = shaders;
	m_loadedPrograms.push_back(program);
	return program;
}

void OpenGLShaderManager::releaseProgram(ShaderProgramHandle handle)
{
	for (const auto& shader : handle.shaders)
		glDetachShader(handle.id, shader.id);
	glDeleteProgram(handle.id);
	m_loadedPrograms.erase(
		std::remove_if(m_loadedPrograms.begin(), m_loadedPrograms.end(),
			[&](const ShaderProgramHandle& p) { return p.id == handle.id; }),
		m_loadedPrograms.end());
}

void OpenGLShaderManager::cleanup()
{
	// release()/releaseProgram() erase from m_loadedShaders/m_loadedPrograms as they
	// go, so iterate copies here rather than the members themselves — looping over
	// a vector while its own erase() shrinks it under the range-for is UB.
	const auto programs = m_loadedPrograms;
	for (const auto& program : programs)
		releaseProgram(program);
	const auto shaders = m_loadedShaders;
	for (const auto& shader : shaders)
		release(shader);
}