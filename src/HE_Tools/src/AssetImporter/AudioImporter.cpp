#include "AudioImporter.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>
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

// ─── PCM conversion ──────────────────────────────────────────────────────────
// Everything below works on interleaved float frames in [-1, 1]; the int16
// asset bytes are unpacked once on the way in and clamped once on the way out.

std::vector<float> unpackPcm16(const std::vector<uint8_t>& bytes)
{
	std::vector<float> out(bytes.size() / sizeof(int16_t));
	for (size_t i = 0; i < out.size(); ++i)
	{
		int16_t s;
		std::memcpy(&s, bytes.data() + i * sizeof(int16_t), sizeof(int16_t));
		out[i] = static_cast<float>(s) / 32768.0f;
	}
	return out;
}

std::vector<uint8_t> packPcm16(const std::vector<float>& samples)
{
	std::vector<uint8_t> out(samples.size() * sizeof(int16_t));
	for (size_t i = 0; i < samples.size(); ++i)
	{
		const float   v = std::clamp(samples[i] * 32767.0f, -32768.0f, 32767.0f);
		const int16_t s = static_cast<int16_t>(std::lrint(v));
		std::memcpy(out.data() + i * sizeof(int16_t), &s, sizeof(int16_t));
	}
	return out;
}

// Stereo→mono averages the pair (a hard-panned source loses 6 dB, the same
// trade every DAW's downmix makes); mono→stereo duplicates, so the clip sits
// dead centre and costs twice the bytes — which is why nothing does it by
// default.
std::vector<float> convertChannels(const std::vector<float>& in, int fromCh, int toCh)
{
	const size_t frames = in.size() / static_cast<size_t>(fromCh);
	std::vector<float> out(frames * static_cast<size_t>(toCh));
	if (fromCh == 2 && toCh == 1)
	{
		for (size_t f = 0; f < frames; ++f)
			out[f] = 0.5f * (in[f * 2] + in[f * 2 + 1]);
	}
	else // 1 → 2
	{
		for (size_t f = 0; f < frames; ++f)
			out[f * 2] = out[f * 2 + 1] = in[f];
	}
	return out;
}

// Windowed-sinc resampler (Blackman window, 24 zero crossings per side at the
// output-side cutoff). The kernel is evaluated per output frame rather than
// tabulated: an import runs once, and 44.1→48 kHz is a 147:160 ratio whose
// phase table would be larger than any clip we care about. Downsampling widens
// the kernel by the ratio so the cutoff stays at the NEW Nyquist — that is the
// band-limiting that linear interpolation lacks. Each output sample's weights
// are normalised to sum to one, which removes the window's passband ripple and
// keeps the edges (where the kernel runs off the clip) at unity gain.
std::vector<float> resampleFrames(const std::vector<float>& in, int channels,
                                  uint32_t fromRate, uint32_t toRate)
{
	const size_t inFrames = in.size() / static_cast<size_t>(channels);
	// Rounded, so a whole-second clip stays a whole second at the new rate.
	const size_t outFrames = static_cast<size_t>(
		(static_cast<uint64_t>(inFrames) * toRate + fromRate / 2) / fromRate);

	const double step   = static_cast<double>(fromRate) / static_cast<double>(toRate);
	const double cutoff = std::min(1.0, 1.0 / step);   // in input Nyquists
	constexpr int kZeroCrossings = 24;
	const double halfWidth = kZeroCrossings / cutoff;  // input samples per side
	const int    reach     = static_cast<int>(std::ceil(halfWidth));

	std::vector<float>  out(outFrames * static_cast<size_t>(channels), 0.0f);
	std::vector<double> acc(static_cast<size_t>(channels));
	for (size_t o = 0; o < outFrames; ++o)
	{
		const double centre = static_cast<double>(o) * step;
		const long   first  = static_cast<long>(std::floor(centre)) - reach + 1;
		const long   last   = static_cast<long>(std::floor(centre)) + reach;
		std::fill(acc.begin(), acc.end(), 0.0);
		double weightSum = 0.0;
		for (long i = first; i <= last; ++i)
		{
			if (i < 0 || i >= static_cast<long>(inFrames)) continue;
			const double d = (static_cast<double>(i) - centre) * cutoff;   // in cutoff periods
			const double w = std::abs(d) / static_cast<double>(kZeroCrossings);   // 0..1 over the kernel
			if (w >= 1.0) continue;
			constexpr double kPi = 3.14159265358979323846;   // M_PI needs _USE_MATH_DEFINES on MSVC
			const double sinc   = (d == 0.0) ? 1.0 : std::sin(kPi * d) / (kPi * d);
			const double window = 0.42 + 0.5 * std::cos(kPi * w) + 0.08 * std::cos(2.0 * kPi * w);
			const double weight = sinc * window;
			weightSum += weight;
			const float* frame = in.data() + static_cast<size_t>(i) * channels;
			for (int c = 0; c < channels; ++c) acc[c] += weight * frame[c];
		}
		if (weightSum > 0.0)
			for (int c = 0; c < channels; ++c)
				out[o * channels + c] = static_cast<float>(acc[c] / weightSum);
	}
	return out;
}

} // namespace

bool AudioImporter::convert(AudioAsset& asset, const ImportSettings& settings)
{
	if (asset.encoding != AudioEncoding::PCM16)
	{
		if (settings.targetSampleRate != 0 || settings.targetChannels != 0)
			HE_LOG_WARN(Tool, "AudioImporter: %s keeps its compressed stream (%d Hz, %d ch) — "
			                  "the requested %u Hz / %u ch conversion only applies to PCM",
			            asset.name.c_str(), asset.sampleRate, asset.channels,
			            settings.targetSampleRate, settings.targetChannels);
		return true;
	}

	const int      fromCh   = asset.channels;
	const uint32_t fromRate = static_cast<uint32_t>(std::max(asset.sampleRate, 0));
	const int      toCh     = settings.targetChannels   != 0 ? settings.targetChannels   : fromCh;
	const uint32_t toRate   = settings.targetSampleRate != 0 ? settings.targetSampleRate : fromRate;
	if (fromCh == toCh && fromRate == toRate) return true;

	if (fromCh < 1 || fromCh > 2 || toCh < 1 || toCh > 2 || fromRate == 0
	    || asset.audioData.size() % (sizeof(int16_t) * static_cast<size_t>(fromCh)) != 0)
	{
		HE_LOG_ERROR(Tool, "AudioImporter: cannot convert %s (%d ch, %u Hz, %zu bytes) to "
		                   "%d ch / %u Hz — only mono and stereo PCM are supported",
		             asset.name.c_str(), fromCh, fromRate, asset.audioData.size(), toCh, toRate);
		return false;
	}

	std::vector<float> samples = unpackPcm16(asset.audioData);
	if (fromCh != toCh)
		samples = convertChannels(samples, fromCh, toCh);
	if (fromRate != toRate)
		samples = resampleFrames(samples, toCh, fromRate, toRate);

	asset.audioData  = packPcm16(samples);
	asset.channels   = toCh;
	asset.sampleRate = static_cast<int>(toRate);
	return true;
}

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
	auto asset = std::make_unique<AudioAsset>();
	if (!decode(sourcePath, *asset))
		return nullptr;

	// The source's own format goes into the log so the line below can say what
	// the conversion did — the asset carries only the stored format afterwards.
	const int sourceRate     = asset->sampleRate;
	const int sourceChannels = asset->channels;
	if (!convert(*asset, settings))
		return nullptr;
	const bool converted = asset->sampleRate != sourceRate || asset->channels != sourceChannels;

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
		 + (converted ? "converted from " + std::to_string(sourceRate) + " Hz / "
		                + std::to_string(sourceChannels) + " ch, "
		              : std::string())
		 + (asset->encoding == AudioEncoding::Vorbis ? "Ogg Vorbis kept compressed, "
		                                             : "PCM, ")
		 + std::to_string(asset->audioData.size() / 1024) + " KiB)").c_str());
	return asset;
}
