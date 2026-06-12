#include "Backends/OpenGL/OpenGLShaderManager.h"
#include <glad/glad.h>
#include <sstream>

ShaderHandle OpenGLShaderManager::load(const char* path, HE::ShaderType type)
{
	if (!fs::exists(path))
	{
		Logger::Log(Logger::LogLevel::Error, (std::string("OpenGLShaderManager: shader file not found: ") + path).c_str());
		return { 0, false };// invalid handle
	}
	std::ifstream file(path);
	if (!file.is_open())
	{
		Logger::Log(Logger::LogLevel::Error, (std::string("OpenGLShaderManager: failed to open shader file: ") + path).c_str());
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
	return handle;
}

void OpenGLShaderManager::release(ShaderHandle handle)
{
	glDeleteShader(handle.id);
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
		Logger::Log(Logger::LogLevel::Error, (std::string("OpenGLShaderManager: shader compilation failed: ") + infoLog).c_str());
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
		Logger::Log(Logger::LogLevel::Error, (std::string("OpenGLShaderManager: shader program linking failed: ") + infoLog).c_str());
		return { 0, false };// invalid handle
	}
	program.ready = true;
	program.shaders = shaders;
	return program;
}

void OpenGLShaderManager::releaseProgram(ShaderProgramHandle handle)
{
	for (const auto& shader : handle.shaders)
		glDetachShader(handle.id, shader.id);
	glDeleteProgram(handle.id);
}

void OpenGLShaderManager::cleanup()
{
	for (const auto& program : m_loadedPrograms)
		releaseProgram(program);
	for (const auto& shader : m_loadedShaders)
		release(shader);
}