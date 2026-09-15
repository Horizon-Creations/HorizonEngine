#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include "ContentManager/Assets.h"
#include "ImporterCommon.h"   // Importer::OutputTargets

// Imports WAV and Ogg Vorbis into an AudioAsset. A .wav is decoded to
// interleaved int16 PCM (AudioEncoding::PCM16); an .ogg is validated by a full
// decode pass but stored AS IS (AudioEncoding::Vorbis) — the AudioEngine decodes
// it on the fly at playback, so the asset costs its compressed size in the pak
// and in RAM. MP3 could follow the same shape via dr_mp3.
class AudioImporter {
public:
	// Note: sample rate / channel conversion is not implemented yet — the
	// source format is stored as-is and these settings are ignored.
	struct ImportSettings {
		uint32_t targetSampleRate = 48000;
		uint16_t targetChannels   = 2;
		bool     mono             = false;
	};

	// True for the source extensions this importer accepts (.wav, .ogg; case-
	// insensitive). The one place the list lives — classifySource and the
	// asset_compiler ask here.
	static bool isSupportedSource(const std::filesystem::path& sourcePath);

	// Load a source file into `out` (bytes by encoding + rate/channels/name)
	// WITHOUT writing anything to disk. A .wav lands as int16 PCM, an .ogg keeps
	// its Ogg bytes with encoding = Vorbis (AudioEngine::decodeToPcm16 turns
	// those into samples when something needs them). Split out of import() so the
	// editor can audition a raw file before — or without ever — importing it:
	// engine-content sources cannot be imported at all unless
	// HE_ENGINE_CONTENT_EDITABLE is set, and a preview should not depend on that.
	// `out.path` is left alone (the caller owns where, or whether, the asset
	// lands). Returns false and logs on a decode error or an unknown extension.
	static bool decode(const std::filesystem::path& sourcePath, AudioAsset& out);

	// Returns the imported asset (already written to disk) or nullptr.
	// `outputs.asset` pins the output onto a file that already exists (a re-import
	// of an asset the user renamed); empty means "named after the source".
	static std::unique_ptr<AudioAsset> import(
		const std::filesystem::path&   sourcePath,
		const std::filesystem::path&   contentRoot,
		const std::filesystem::path&   relativeOutputDir,
		const ImportSettings&          settings,
		const Importer::OutputTargets& outputs = {});

	static std::unique_ptr<AudioAsset> import(
		const std::filesystem::path& sourcePath,
		const std::filesystem::path& contentRoot,
		const std::filesystem::path& relativeOutputDir = {})
	{ return import(sourcePath, contentRoot, relativeOutputDir, ImportSettings{}); }
};
