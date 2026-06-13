#include "AudioImporter.h"
#include "ImporterCommon.h"
#include "Diagnostics/Logger.h"

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

std::unique_ptr<AudioAsset> AudioImporter::import(
	const std::filesystem::path& sourcePath,
	const std::filesystem::path& contentRoot,
	const std::filesystem::path& relativeOutputDir,
	const ImportSettings&        settings)
{
	(void)settings; // resampling not implemented yet

	unsigned int   channels   = 0;
	unsigned int   sampleRate = 0;
	drwav_uint64   frameCount = 0;
	drwav_int16*   samples    = drwav_open_file_and_read_pcm_frames_s16(
		sourcePath.string().c_str(), &channels, &sampleRate, &frameCount, nullptr);

	if (!samples)
	{
		Logger::Log(Logger::LogLevel::Error,
			("AudioImporter: failed to decode " + sourcePath.string()).c_str());
		return nullptr;
	}

	auto asset = std::make_unique<AudioAsset>();
	asset->type       = HE::AssetType::Audio;
	asset->name       = sourcePath.stem().string();
	asset->path       = Importer::toAssetPath(relativeOutputDir / (asset->name + ".hasset"));
	asset->sampleRate = static_cast<int>(sampleRate);
	asset->channels   = static_cast<int>(channels);

	const auto* bytes = reinterpret_cast<const uint8_t*>(samples);
	asset->audioData.assign(bytes, bytes + frameCount * channels * sizeof(drwav_int16));
	drwav_free(samples, nullptr);

	if (!Importer::writeAsset(*asset, contentRoot))
		return nullptr;

	Logger::Log(Logger::LogLevel::Info,
		("AudioImporter: " + sourcePath.filename().string() + " -> " + asset->path
		 + " (" + std::to_string(sampleRate) + " Hz, "
		 + std::to_string(channels) + " ch)").c_str());
	return asset;
}
