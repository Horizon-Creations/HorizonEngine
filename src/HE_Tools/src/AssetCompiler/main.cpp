// asset_compiler — Usage: asset_compiler <source_dir> <output_dir> [--force]
//
// Walks <source_dir> recursively, dispatches each source file by extension to
// the matching importer and writes .hasset files into <output_dir>, mirroring
// the source directory layout. A file is skipped when its primary .hasset
// output is newer than the source (pass --force to re-import everything).

#include "../AssetImporter/AnimationClipImporter.h"
#include "../AssetImporter/AudioImporter.h"
#include "../AssetImporter/FontImporter.h"
#include "../AssetImporter/ImporterCommon.h"
#include "../AssetImporter/MaterialImporter.h"
#include "../AssetImporter/MeshImporter.h"
#include "../AssetImporter/SkeletalMeshImporter.h"
#include "../AssetImporter/TextureImporter.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace
{
	enum class SourceKind { None, Texture, Mesh, SkeletalMesh, Material, Audio, Font };

	SourceKind classify(const fs::path& file)
	{
		std::string ext = file.extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(),
		               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

		if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" ||
		    ext == ".bmp" || ext == ".psd" || ext == ".hdr")
			return SourceKind::Texture;
		// A glTF carrying a skin has to go to SkeletalMeshImporter: MeshImporter
		// ignores JOINTS_0/WEIGHTS_0 and the skeleton, so a rigged character would
		// import as bind-pose geometry registered as a StaticMesh — with a success
		// message and no way to ever pick it as a SkeletalMesh.
		if (ext == ".gltf" || ext == ".glb")
			return Importer::gltfHasSkin(file) ? SourceKind::SkeletalMesh : SourceKind::Mesh;
		if (ext == ".hmat")
			return SourceKind::Material;
		if (ext == ".wav")
			return SourceKind::Audio;
		if (ext == ".ttf" || ext == ".otf")
			return SourceKind::Font;
		return SourceKind::None;
	}

	// The .hasset an importer writes first — the timestamp probe below compares
	// against it. Only SkeletalMeshImporter deviates from "<stem>.hasset".
	fs::path primaryOutput(SourceKind kind, const fs::path& source)
	{
		const std::string stem = source.stem().string();
		return (kind == SourceKind::SkeletalMesh) ? stem + "_skeletal.hasset"
		                                          : stem + ".hasset";
	}

	bool isUpToDate(const fs::path& source, const fs::path& output)
	{
		std::error_code ec;
		const auto outTime = fs::last_write_time(output, ec);
		if (ec) return false;
		const auto srcTime = fs::last_write_time(source, ec);
		if (ec) return false;
		return outTime >= srcTime;
	}
}

int main(int argc, char** argv)
{
	bool force = false;
	std::vector<std::string> args;
	for (int i = 1; i < argc; ++i)
	{
		if (std::string(argv[i]) == "--force") force = true;
		else args.emplace_back(argv[i]);
	}

	if (args.size() != 2)
	{
		std::cerr << "Usage: asset_compiler <source_dir> <output_dir> [--force]\n";
		return 1;
	}

	const fs::path sourceDir = args[0];
	const fs::path outputDir = args[1];

	if (!fs::is_directory(sourceDir))
	{
		std::cerr << "asset_compiler: not a directory: " << sourceDir << "\n";
		return 1;
	}
	fs::create_directories(outputDir);

	int imported = 0, skipped = 0, failed = 0;

	for (const auto& entry : fs::recursive_directory_iterator(sourceDir))
	{
		if (!entry.is_regular_file())
			continue;

		const SourceKind kind = classify(entry.path());
		if (kind == SourceKind::None)
			continue;

		const fs::path relDir  = fs::relative(entry.path().parent_path(), sourceDir);
		const fs::path relOut  = (relDir == ".") ? fs::path{} : relDir;
		const fs::path primary = outputDir / relOut / primaryOutput(kind, entry.path());

		if (!force && isUpToDate(entry.path(), primary))
		{
			++skipped;
			continue;
		}

		bool ok = false;
		switch (kind)
		{
		case SourceKind::Texture:  ok = TextureImporter::import(entry.path(), outputDir, relOut) != nullptr; break;
		case SourceKind::Mesh:     ok = MeshImporter::import(entry.path(), outputDir, relOut)    != nullptr; break;
		case SourceKind::Material: ok = MaterialImporter::import(entry.path(), outputDir, relOut)!= nullptr; break;
		case SourceKind::Audio:    ok = AudioImporter::import(entry.path(), outputDir, relOut)   != nullptr; break;
		case SourceKind::Font:     ok = FontImporter::import(entry.path(), outputDir, relOut)    != nullptr; break;
		case SourceKind::SkeletalMesh:
		{
			ok = SkeletalMeshImporter::import(entry.path(), outputDir, relOut) != nullptr;
			// A rigged glTF normally ships its animations in the same file; the
			// clips are separate assets the AnimatorStateMachine references.
			if (ok)
			{
				const auto clips = AnimationClipImporter::importAndWrite(entry.path(), outputDir, relOut);
				imported += clips.written;
				failed   += clips.failed;
			}
			break;
		}
		case SourceKind::None:     break;
		}

		ok ? ++imported : ++failed;
		if (!ok)
			std::cerr << "asset_compiler: import failed: " << entry.path() << "\n";
	}

	std::cout << "asset_compiler: " << imported << " imported, "
	          << skipped << " up-to-date, " << failed << " failed\n";
	return failed == 0 ? 0 : 2;
}
