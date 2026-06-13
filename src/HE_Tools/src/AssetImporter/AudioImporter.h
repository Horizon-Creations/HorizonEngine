#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include "ContentManager/Assets.h"

// Imports WAV into an AudioAsset (PCM data is stored as interleaved int16).
// OGG/MP3 support can be added later via stb_vorbis / dr_mp3.
class AudioImporter {
public:
	// Note: sample rate / channel conversion is not implemented yet — the
	// source format is stored as-is and these settings are ignored.
	struct ImportSettings {
		uint32_t targetSampleRate = 48000;
		uint16_t targetChannels   = 2;
		bool     mono             = false;
	};

	// Returns the imported asset (already written to disk) or nullptr.
	static std::unique_ptr<AudioAsset> import(
		const std::filesystem::path& sourcePath,
		const std::filesystem::path& contentRoot,
		const std::filesystem::path& relativeOutputDir,
		const ImportSettings&        settings);

	static std::unique_ptr<AudioAsset> import(
		const std::filesystem::path& sourcePath,
		const std::filesystem::path& contentRoot,
		const std::filesystem::path& relativeOutputDir = {})
	{ return import(sourcePath, contentRoot, relativeOutputDir, ImportSettings{}); }
};
