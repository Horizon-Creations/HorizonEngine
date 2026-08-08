#pragma once
#include <string>
#include <vector>
#include "Types/Defines.h"
#include "Types/Enums.h"
#include "Types/UUID.h"

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

		// Set only for synthetic "Engine/" entries contributed by the EngineContent
		// SFTP manifest (see HE::Cs::EngineContentManifest) for an asset that is not
		// present in any local root yet. `fullPath` is then the path it will occupy
		// ONCE downloaded (the shared EngineContent cache — see ContentManager), not
		// an existing file. remoteUuid is that asset's UUID, used to look the entry
		// back up in the manifest when a download is requested.
		bool     isRemoteOnly = false;
		HE::UUID remoteUuid;
	};

	struct Folder
	{
		std::string name;
		std::string fullPath;
		std::vector<Folder*> subfolders;
		std::vector<File*> files;
	};

	// A minimal, network-agnostic description of one EngineContent asset that
	// exists on the configured SFTP server but not (yet) locally — enough for
	// GlobalState::refreshEngineFolder() to show it in the Content Browser tree.
	// Deliberately just {path, uuid}: HE_Core must not know about HE_ContentSync
	// (that module is editor-only and optional — see the HE_HAVE_LIBSSH2 guard),
	// so the editor converts its HE::Cs::EngineContentManifest into a vector of
	// these before calling refreshEngineFolder(), rather than GlobalState taking
	// the manifest type directly.
	struct RemoteEngineAsset
	{
		std::string relativePath;   // relative to the EngineContent root, e.g. "Materials/DefaultCube.hasset"
		HE::UUID    uuid;
	};
}