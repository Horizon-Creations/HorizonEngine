#include "AudioImporter.h"
#include <cstdint>
#include "ImporterCommon.h"
#include "Diagnostics/Logger.h"

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

bool AudioImporter::decode(const std::filesystem::path& sourcePath, AudioAsset& out)
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

	out.type       = HE::AssetType::Audio;
	out.name       = sourcePath.stem().string();
	out.sampleRate = static_cast<int>(sampleRate);
	out.channels   = static_cast<int>(channels);

	const auto* bytes = reinterpret_cast<const uint8_t*>(samples);
	out.audioData.assign(bytes, bytes + frameCount * channels * sizeof(drwav_int16));
	drwav_free(samples, nullptr);
	return true;
}

std::unique_ptr<AudioAsset> AudioImporter::import(
	const std::filesystem::path& sourcePath,
	const std::filesystem::path& contentRoot,
	const std::filesystem::path& relativeOutputDir,
	const ImportSettings&        settings)
{
	(void)settings; // resampling not implemented yet

	auto asset = std::make_unique<AudioAsset>();
	if (!decode(sourcePath, *asset))
		return nullptr;

	// decode() fills name/rate/channels/PCM; only the on-disk location is the
	// importer's business.
	asset->path = Importer::toAssetPath(relativeOutputDir / (asset->name + ".hasset"));

	const unsigned int sampleRate = static_cast<unsigned int>(asset->sampleRate);
	const unsigned int channels   = static_cast<unsigned int>(asset->channels);

	if (!Importer::writeAsset(*asset, contentRoot))
		return nullptr;

	HE_LOG_INFO(Tool, "%s",
		("AudioImporter: " + sourcePath.filename().string() + " -> " + asset->path
		 + " (" + std::to_string(sampleRate) + " Hz, "
		 + std::to_string(channels) + " ch)").c_str());
	return asset;
}
