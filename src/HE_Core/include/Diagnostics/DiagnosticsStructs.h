#pragma once
#include <string>
#include <vector>
#include "Types/Defines.h"
#include "Types/Enums.h"

struct EngineStatus
{
	HE::RendererBackend      selectedRHI;
	// No `currentOS` here: it went with getCurrentOS(), which had no callers and
	// defaulted every platform to Windows before persisting it. The rest of the
	// engine branches on platform with `#ifdef _WIN32`/`__APPLE__` at the use site,
	// so a never-assigned runtime field only invited that bug back.
	std::string              startupPath;
	std::string              lastProjectPath;
	std::vector<std::string> knownProjects;   // most-recent first, max 10
};

// Content-browser tree nodes. These live in `namespace HE` and not at global
// scope for a specific reason: `File` and `Folder` are about as generic as an
// identifier gets, and every translation unit that sees this header also sees
// <filesystem>. At global scope they are a standing invitation for an ambiguity
// or — worse — a silent bind to the wrong type. See docs/coding-conventions.md §1.
namespace HE
{
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
}