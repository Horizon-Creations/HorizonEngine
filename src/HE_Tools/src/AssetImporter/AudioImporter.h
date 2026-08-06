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

	// Decode a WAV into `out` (interleaved int16 PCM + rate/channels/name) WITHOUT
	// writing anything to disk. Split out of import() so the editor can audition a
	// raw .wav before — or without ever — importing it: engine-content .wav files
	// cannot be imported at all unless HE_ENGINE_CONTENT_EDITABLE is set, and a
	// preview should not depend on that. `out.path` is left alone (the caller owns
	// where, or whether, the asset lands). Returns false and logs on a decode error.
	static bool decode(const std::filesystem::path& sourcePath, AudioAsset& out);

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
