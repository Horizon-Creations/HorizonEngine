#pragma once
#include <Types/Enums.h>
#include <vector>

struct ShaderHandle
{
	unsigned int id = 0;
	bool ready = false;
	HE::ShaderType type = HE::ShaderType::Vertex;
};

struct ShaderProgramHandle
{
	unsigned int id = 0;
	bool ready = false;
	std::vector<ShaderHandle> shaders;
};

class ShaderManager
{
public:
	virtual ~ShaderManager() = default;

	// Loads the shader at the given path and returns a handle carrying the
	// backend-internal id that represents it.
	virtual ShaderHandle load(const char* path, HE::ShaderType type) = 0;
	// Releases the shader the handle represents. Every resource tied to that
	// shader should be released with it.
	virtual void release(ShaderHandle handle) = 0;
	// Compiles the shader the handle represents. Returns true on success.
	virtual bool compile(ShaderHandle& handle) = 0;

	// Links a shader program from the given handles — all of them must already be
	// compiled. Returns a handle representing the created program.
	virtual ShaderProgramHandle createProgram(const std::vector<ShaderHandle>& shaders) = 0;
	// Releases the program the handle represents. Every resource tied to that
	// program should be released with it.
	virtual void releaseProgram(ShaderProgramHandle handle) = 0;

	virtual void cleanup() = 0;
};
