#pragma once
#include <string>
#include <vector>
#include "Types/Defines.h"
#include "Types/Enums.h"

struct EngineStatus
{
	bool                     isRunning;
	HE::GraphicsAPI          selectedRHI;
	HE::OS                   currentOS;
	std::string              startupPath;
	std::string              lastProjectPath;
	std::vector<std::string> knownProjects;   // most-recent first, max 10
};

struct File
{
	std::string name;
	std::string fullPath;
	std::string extension;
};

struct Folder
{
	std::string name;
	std::string fullPath;
	std::vector<Folder*> subfolders;
	std::vector<File*> files;
};