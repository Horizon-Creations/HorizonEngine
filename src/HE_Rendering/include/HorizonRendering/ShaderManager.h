#pragma once
#include <filesystem>
#include <Diagnostics/Logger.h>
#include <string>
#include <fstream>
#include <Types/Enums.h>
#include <vector>

namespace fs = std::filesystem;

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

	//Gibt einen ShaderHandle zurück der eine interne ID enthält, die den Shader repräsentiert. Der Shader wird anhand des übergebenen Pfads geladen.
	virtual ShaderHandle load(const char* path, HE::ShaderType type) = 0;
	//Gibt den mit der übergebenen ID repräsentierten Shader frei. Alle Ressourcen, die mit diesem Shader verbunden sind, sollten ebenfalls freigegeben werden.
	virtual void release(ShaderHandle handle) = 0;
	// Kompiliert den Shader, der durch den übergebenen Handle repräsentiert wird. Gibt true zurück, wenn die Kompilierung erfolgreich war, andernfalls false.
	virtual bool compile(ShaderHandle& handle) = 0;

	// Erstellt ein Shader-Programm aus den übergebenen Shader-Handles. Alle Shader müssen bereits kompiliert sein. Gibt einen ShaderProgramHandle zurück, der das erstellte Programm repräsentiert.
	virtual ShaderProgramHandle createProgram(const std::vector<ShaderHandle>& shaders) = 0;
	// Gibt das mit der übergebenen ID repräsentierte Shader-Programm frei. Alle Ressourcen, die mit diesem Programm verbunden sind, sollten ebenfalls freigegeben werden.
	virtual void releaseProgram(ShaderProgramHandle handle) = 0;

	virtual void cleanup() = 0;
};