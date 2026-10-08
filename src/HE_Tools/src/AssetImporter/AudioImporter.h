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
	// Optional conversion of the stored PCM. Both fields default to 0 = "keep
	// the source's own", and that is NOT a stopgap: the AudioEngine resamples
	// and channel-converts every clip on the mixer thread anyway (the sound's
	// own resampler runs off the rate the asset carries), so a 44.1 kHz mono
	// clip plays correctly on the 48 kHz stereo engine without any help from
	// here. Setting a target is a size/consistency choice — a stereo→mono
	// downmix halves a voice-over's footprint, one project-wide rate saves the
	// mixer a resampler per voice — not a correctness one.
	//
	// Only PCM16 (.wav) is converted. An .ogg stays the compressed stream it
	// was (there is no Vorbis encoder in the engine, and a re-encode would cost
	// quality); a target set on an .ogg import is logged and ignored.
	struct ImportSettings {
		uint32_t targetSampleRate = 0;   // Hz; 0 = keep the source rate
		uint16_t targetChannels   = 0;   // 1 or 2; 0 = keep the source layout
	};

	// Converts `asset` (PCM16 only) in place to the rate/channels the settings
	// ask for, updating sampleRate/channels/audioData together. Channels first
	// (stereo→mono averages, mono→stereo duplicates), then the rate: a windowed-
	// sinc resampler, so downsampling is band-limited rather than aliased. A
	// no-op when nothing differs. Returns false — leaving the asset untouched —
	// for a layout it cannot handle (more than two channels either side, an odd
	// byte count). Public so the tests can check the numbers without a disk.
	static bool convert(AudioAsset& asset, const ImportSettings& settings);

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

	// ─── Extract ─────────────────────────────────────────────────────────────
	// The Audio Editor's "Extract Selection": a range of a clip becomes a NEW
	// audio asset of its own. The clip it was cut from is only read — its file is
	// never opened for writing, so it stays byte-identical.
	//
	// The new asset is int16 PCM made of exactly the frames [beginFrame,
	// endFrame) of `clip`, which must be PCM16 (the editor hands in its decoded
	// copy of a Vorbis clip — there is no Vorbis encoder to cut the stream with).
	// It gets a fresh UUID and a file name nothing in the folder has yet
	// (uniqueOutputPath), and records NO source file: the source the original was
	// imported from is the whole clip, and a Reimport that went back to it would
	// silently replace the extract with everything it was cut from.
	struct ExtractResult
	{
		std::string path;     // content-relative, forward slashes
		HE::UUID    id;
		uint64_t    frames = 0;
	};
	// `edit` is what the new asset carries (AudioEdit::forRange of the original's
	// edit — bus, EQ and the curve over the range). False, writing nothing, for a
	// clip that is not PCM16, an empty or out-of-range selection, or a failed write.
	static bool extractRange(const AudioAsset&            clip,
	                         uint64_t                     beginFrame,
	                         uint64_t                     endFrame,
	                         const std::filesystem::path& contentRoot,
	                         const std::filesystem::path& relativeOutputDir,
	                         const std::string&           stem,
	                         const HE::AudioEdit&         edit,
	                         ExtractResult&               out);

	// The first of "<dir>/<stem>.hasset", "<dir>/<stem>_2.hasset", "_3", … that
	// does not exist under `contentRoot` (content-relative, forward slashes).
	static std::string uniqueOutputPath(const std::filesystem::path& contentRoot,
	                                    const std::filesystem::path& relativeOutputDir,
	                                    const std::string&           stem);
};
