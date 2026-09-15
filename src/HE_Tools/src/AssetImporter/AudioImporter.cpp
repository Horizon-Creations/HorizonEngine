#include "AudioImporter.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include "ImporterCommon.h"
#include "Diagnostics/Logger.h"

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

// The same stb_vorbis.c the AudioEngine plays with (src/HE_Scene/vendor), here
// only to prove an .ogg decodes before it becomes an asset. HorizonImporters is
// a static library that never links HorizonScene (asset_compiler has no engine),
// so the functions are compiled a second time. The editor links both, and that
// is harmless: the definitions are identical, and an executable's own symbols
// win over a shared library's exports (macOS two-level namespace, ELF
// interposition; a PE import library only supplies what nothing else defines).
// No STB_VORBIS_NO_STDIO: keep the same configuration as the engine's copy so
// the two builds cannot drift apart.
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wtautological-compare"
#endif
#include "stb_vorbis.c"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace {

std::string lowerExtension(const std::filesystem::path& p)
{
	std::string ext = p.extension().string();
	std::transform(ext.begin(), ext.end(), ext.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return ext;
}

bool decodeWav(const std::filesystem::path& sourcePath, AudioAsset& out)
{
	unsigned int   channels   = 0;
	unsigned int   sampleRate = 0;
	drwav_uint64   frameCount = 0;
	drwav_int16*   samples    = drwav_open_file_and_read_pcm_frames_s16(
		sourcePath.string().c_str(), &channels, &sampleRate, &frameCount, nullptr);

	if (!samples)
	{
		HE_LOG_ERROR(Tool, "%s",
			("AudioImporter: failed to decode " + sourcePath.string()).c_str());
		return false;
	}

	out.encoding   = AudioEncoding::PCM16;
	out.sampleRate = static_cast<int>(sampleRate);
	out.channels   = static_cast<int>(channels);

	const auto* bytes = reinterpret_cast<const uint8_t*>(samples);
	out.audioData.assign(bytes, bytes + frameCount * channels * sizeof(drwav_int16));
	drwav_free(samples, nullptr);
	return true;
}

// The Ogg bytes go into the asset untouched; the decode pass is the validation
// (a truncated or non-Vorbis file is refused here, not discovered as silence at
// play time) and the source of the frame count in the log.
bool loadOgg(const std::filesystem::path& sourcePath, AudioAsset& out)
{
	std::ifstream in(sourcePath, std::ios::binary);
	if (!in)
	{
		HE_LOG_ERROR(Tool, "%s",
			("AudioImporter: cannot open " + sourcePath.string()).c_str());
		return false;
	}
	std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
	                            std::istreambuf_iterator<char>());

	// stb_vorbis sizes its memory streams with an int.
	if (bytes.empty() || bytes.size() > static_cast<size_t>(INT32_MAX))
	{
		HE_LOG_ERROR(Tool, "AudioImporter: %s is %s", sourcePath.string().c_str(),
		             bytes.empty() ? "empty" : "too large for the Vorbis decoder (> 2 GiB)");
		return false;
	}

	int error = 0;
	stb_vorbis* v = stb_vorbis_open_memory(bytes.data(), static_cast<int>(bytes.size()),
	                                       &error, nullptr);
	if (!v)
	{
		HE_LOG_ERROR(Tool, "AudioImporter: %s is not an Ogg Vorbis stream (stb_vorbis error %d)",
		             sourcePath.string().c_str(), error);
		return false;
	}
	const stb_vorbis_info info = stb_vorbis_get_info(v);

	// Decode everything once, into a scratch block, and count — the decoder
	// walks every page, so a stream that is cut off or corrupt mid-way shows up
	// as a short count against the header's length rather than as a good import.
	const unsigned int declared = stb_vorbis_stream_length_in_samples(v);
	uint64_t decoded = 0;
	{
		constexpr int kScratchFrames = 4096;
		std::vector<float> scratch(static_cast<size_t>(kScratchFrames) * info.channels);
		for (;;)
		{
			const int n = stb_vorbis_get_samples_float_interleaved(
				v, info.channels, scratch.data(), kScratchFrames * info.channels);
			if (n <= 0) break;
			decoded += static_cast<uint64_t>(n);
		}
	}
	stb_vorbis_close(v);

	if (info.channels <= 0 || info.sample_rate == 0 || decoded == 0)
	{
		HE_LOG_ERROR(Tool, "AudioImporter: %s decoded to nothing (%d ch, %u Hz, %llu frames)",
		             sourcePath.string().c_str(), info.channels, info.sample_rate,
		             static_cast<unsigned long long>(decoded));
		return false;
	}
	if (declared != 0 && decoded < declared)
		HE_LOG_WARN(Tool, "AudioImporter: %s declares %u frames but only %llu decode — "
		                  "the file is probably truncated; importing what plays",
		            sourcePath.string().c_str(), declared,
		            static_cast<unsigned long long>(decoded));

	out.encoding   = AudioEncoding::Vorbis;
	out.sampleRate = static_cast<int>(info.sample_rate);
	out.channels   = info.channels;
	out.audioData  = std::move(bytes);
	return true;
}

} // namespace

bool AudioImporter::isSupportedSource(const std::filesystem::path& sourcePath)
{
	const std::string ext = lowerExtension(sourcePath);
	return ext == ".wav" || ext == ".ogg";
}

bool AudioImporter::decode(const std::filesystem::path& sourcePath, AudioAsset& out)
{
	const std::string ext = lowerExtension(sourcePath);
	bool ok = false;
	if      (ext == ".wav") ok = decodeWav(sourcePath, out);
	else if (ext == ".ogg") ok = loadOgg(sourcePath, out);
	else
		HE_LOG_ERROR(Tool, "AudioImporter: unsupported audio source '%s' (expected .wav or .ogg)",
		             sourcePath.string().c_str());
	if (!ok) return false;

	out.type = HE::AssetType::Audio;
	out.name = sourcePath.stem().string();
	return true;
}

std::unique_ptr<AudioAsset> AudioImporter::import(
	const std::filesystem::path&   sourcePath,
	const std::filesystem::path&   contentRoot,
	const std::filesystem::path&   relativeOutputDir,
	const ImportSettings&          settings,
	const Importer::OutputTargets& outputs)
{
	(void)settings; // resampling not implemented yet

	auto asset = std::make_unique<AudioAsset>();
	if (!decode(sourcePath, *asset))
		return nullptr;

	// decode() fills name/rate/channels/PCM; only the on-disk location is the
	// importer's business — and the name goes with it, because a re-import that
	// lands on a renamed file must not write the source's stem into its META.
	const auto out = Importer::resolveOutput(outputs.asset, relativeOutputDir, asset->name);
	asset->name = out.name;
	asset->path = out.path;

	const unsigned int sampleRate = static_cast<unsigned int>(asset->sampleRate);
	const unsigned int channels   = static_cast<unsigned int>(asset->channels);

	if (!Importer::writeAsset(*asset, contentRoot, sourcePath))
		return nullptr;

	HE_LOG_INFO(Tool, "%s",
		("AudioImporter: " + sourcePath.filename().string() + " -> " + asset->path
		 + " (" + std::to_string(sampleRate) + " Hz, "
		 + std::to_string(channels) + " ch, "
		 + (asset->encoding == AudioEncoding::Vorbis ? "Ogg Vorbis kept compressed, "
		                                             : "PCM, ")
		 + std::to_string(asset->audioData.size() / 1024) + " KiB)").c_str());
	return asset;
}
