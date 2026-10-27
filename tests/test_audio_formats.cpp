#include "doctest.h"
#include "TestFsUtil.h"
#include "fixtures/tone_flac.h"
#include "AudioImporter.h"
#include "ImporterCommon.h"
#include <Audio/AudioBusConfig.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/HAsset.h>
#include <HorizonScene/AudioEngine.h>
#include <HorizonScene/SteamAudio.h>
#include <HorizonScene/AudioOcclusion.h>
#include <HorizonScene/AudioRoom.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// MP3 and FLAC as audio sources, and the optional Steam Audio backend. The FLAC
// stream is a baked-in fixture; the tests own no MP3 encoder, so the MP3 is built
// here from valid but silent Layer III frames (see makeSilentMp3) — enough to
// prove detection, format, length and streaming, not a signal's amplitude.

namespace
{
	struct TempDir
	{
		fs::path path;
		explicit TempDir(const char* name)
		{
			path = fs::temp_directory_path() / (std::string(name) + "_" + std::to_string(std::rand()));
			std::error_code ec;
			fs::remove_all(path, ec);
			fs::create_directories(path, ec);
		}
		~TempDir() { he_test::removeAllQuiet(path); }
	};

	bool writeFile(const fs::path& file, const uint8_t* data, size_t size)
	{
		std::ofstream f(file, std::ios::binary | std::ios::trunc);
		if (!f) return false;
		f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
		return static_cast<bool>(f);
	}

	// MPEG-2 Layer III, 22050 Hz, mono, 32 kbit/s, no CRC: 104-byte frames of
	// 576 samples. Header FF F3 40 C0, a zeroed side info and main data decode
	// to silence — a stream every conforming decoder accepts.
	constexpr int kMp3Frames = 12;
	std::vector<uint8_t> makeSilentMp3()
	{
		std::vector<uint8_t> out;
		for (int i = 0; i < kMp3Frames; ++i)
		{
			std::vector<uint8_t> frame(104, 0);
			frame[0] = 0xFF; frame[1] = 0xF3; frame[2] = 0x40; frame[3] = 0xC0;
			out.insert(out.end(), frame.begin(), frame.end());
		}
		return out;
	}

	AudioAsset flacClip()
	{
		AudioAsset a;
		a.type       = HE::AssetType::Audio;
		a.encoding   = AudioEncoding::Flac;
		a.sampleRate = he_test::kToneFlacSampleRate;
		a.channels   = he_test::kToneFlacChannels;
		a.audioData.assign(he_test::kToneFlac, he_test::kToneFlac + he_test::kToneFlacSize);
		return a;
	}

	AudioAsset mp3Clip()
	{
		AudioAsset a;
		a.type       = HE::AssetType::Audio;
		a.encoding   = AudioEncoding::Mp3;
		a.sampleRate = 22050;
		a.channels   = 1;
		a.audioData  = makeSilentMp3();
		return a;
	}

	int16_t peakOf(const std::vector<uint8_t>& pcm)
	{
		int peak = 0;
		for (size_t i = 0; i + 1 < pcm.size(); i += 2)
		{
			int16_t s; std::memcpy(&s, pcm.data() + i, 2);
			peak = std::max(peak, std::abs(static_cast<int>(s)));
		}
		return static_cast<int16_t>(peak);
	}
}

// ─── Source detection ────────────────────────────────────────────────────────

TEST_CASE("AudioImporter accepts .mp3 and .flac next to .wav and .ogg, any case")
{
	for (const char* ext : { "a.wav", "a.ogg", "a.mp3", "a.flac", "A.MP3", "B.FlAc" })
		CHECK(AudioImporter::isSupportedSource(ext));
	CHECK_FALSE(AudioImporter::isSupportedSource("a.aiff"));
	CHECK_FALSE(AudioImporter::isSupportedSource("a.m4a"));
}

// ─── FLAC ────────────────────────────────────────────────────────────────────

TEST_CASE("FLAC: decodeToPcm16 yields the tone, losslessly sized")
{
	std::vector<uint8_t> pcm;
	REQUIRE(AudioEngine::decodeToPcm16(flacClip(), pcm));
	CHECK(pcm.size() / sizeof(int16_t) == he_test::kToneFlacFrames);
	const int peak = peakOf(pcm);
	CHECK(peak >= he_test::kToneFlacPeakMin);
	CHECK(peak <= he_test::kToneFlacPeakMax);
}

TEST_CASE("FLAC: a clip plays as a streamed voice with the stream's own length and rate")
{
	AudioEngine engine;
	REQUIRE(engine.init(true));
	const uint64_t h = engine.play(flacClip());
	REQUIRE(h != 0);
	CHECK(engine.getSoundLengthFrames(h) == he_test::kToneFlacFrames);
	CHECK(engine.getSoundSampleRate(h)   == he_test::kToneFlacSampleRate);

	// The mixer really pulls decoded samples out of it.
	std::vector<float> mix(2 * 2048, 0.0f);
	REQUIRE(engine.readMixedFrames(mix.data(), 2048) == 2048);
	float peak = 0.0f;
	for (float f : mix) peak = std::max(peak, std::abs(f));
	CHECK(peak > 0.1f);
	engine.shutdown();
}

TEST_CASE("FLAC: junk bytes are refused, not played")
{
	AudioAsset junk = flacClip();
	junk.audioData.assign(512, 0x5A);
	std::vector<uint8_t> pcm;
	CHECK_FALSE(AudioEngine::decodeToPcm16(junk, pcm));

	AudioEngine engine;
	REQUIRE(engine.init(true));
	CHECK(engine.play(junk) == 0);
	engine.shutdown();
}

TEST_CASE("AudioImporter keeps a .flac compressed and the asset round-trips through the .hasset")
{
	TempDir src("he_test_flac_src"), content("he_test_flac_content");
	const fs::path file = src.path / "tone.flac";
	REQUIRE(writeFile(file, he_test::kToneFlac, he_test::kToneFlacSize));

	// A target is ignored for a compressed source, like for an .ogg.
	AudioImporter::ImportSettings s;
	s.targetSampleRate = 8000;
	auto imported = AudioImporter::import(file, content.path, "Audio", s);
	REQUIRE(imported != nullptr);
	CHECK(imported->encoding   == AudioEncoding::Flac);
	CHECK(imported->sampleRate == he_test::kToneFlacSampleRate);
	CHECK(imported->channels   == he_test::kToneFlacChannels);
	CHECK(imported->audioData.size() == he_test::kToneFlacSize);

	// On disk: the FLAC bytes in their own chunk, no PCM chunk beside them.
	std::vector<uint8_t> chunk;
	REQUIRE(Importer::readAssetChunk(content.path / imported->path, HAsset::CHUNK_FLCD, chunk));
	CHECK(chunk.size() == he_test::kToneFlacSize);
	CHECK_FALSE(Importer::readAssetChunk(content.path / imported->path, HAsset::CHUNK_PCMD, chunk));
}

// ─── MP3 ─────────────────────────────────────────────────────────────────────

TEST_CASE("MP3: decodes, reports its format and plays as a streamed voice")
{
	const AudioAsset clip = mp3Clip();
	std::vector<uint8_t> pcm;
	REQUIRE(AudioEngine::decodeToPcm16(clip, pcm));
	// 12 frames x 576 samples, give or take the decoder's own priming.
	const size_t frames = pcm.size() / sizeof(int16_t);
	CHECK(frames >= static_cast<size_t>(kMp3Frames - 2) * 576);
	CHECK(frames <= static_cast<size_t>(kMp3Frames + 2) * 576);
	CHECK(peakOf(pcm) == 0);   // silence in, silence out

	AudioEngine engine;
	REQUIRE(engine.init(true));
	const uint64_t h = engine.play(clip);
	REQUIRE(h != 0);
	CHECK(engine.getSoundSampleRate(h) == 22050);
	engine.setSoundLooping(h, true);
	engine.setSoundPitch(h, 1.25f);
	engine.stop(h);
	engine.shutdown();
}

TEST_CASE("AudioImporter keeps an .mp3 compressed; garbage is not an import")
{
	TempDir src("he_test_mp3_src"), content("he_test_mp3_content");
	const std::vector<uint8_t> mp3 = makeSilentMp3();
	const fs::path good = src.path / "silence.mp3";
	REQUIRE(writeFile(good, mp3.data(), mp3.size()));

	AudioAsset decoded;
	REQUIRE(AudioImporter::decode(good, decoded));
	CHECK(decoded.encoding   == AudioEncoding::Mp3);
	CHECK(decoded.sampleRate == 22050);
	CHECK(decoded.channels   == 1);
	CHECK(decoded.audioData  == mp3);

	auto imported = AudioImporter::import(good, content.path, "Audio", {});
	REQUIRE(imported != nullptr);
	std::vector<uint8_t> chunk;
	REQUIRE(Importer::readAssetChunk(content.path / imported->path, HAsset::CHUNK_MP3D, chunk));
	CHECK(chunk == mp3);

	const std::vector<uint8_t> junk(2048, 0x5A);
	const fs::path bad = src.path / "junk.mp3";
	REQUIRE(writeFile(bad, junk.data(), junk.size()));
	AudioAsset rejected;
	CHECK_FALSE(AudioImporter::decode(bad, rejected));
}

// ─── Project setting ─────────────────────────────────────────────────────────

TEST_CASE("AudioBusConfig: the spatial backend is a project setting that defaults away")
{
	HE::AudioBusConfig cfg;
	CHECK(cfg.spatialBackend == HE::AudioSpatialBackend::Miniaudio);
	CHECK(cfg.isDefault());
	nlohmann::json j;
	cfg.toJson(j);
	CHECK_FALSE(j.contains("spatial"));   // a project that never chose it is unchanged on disk

	cfg.spatialBackend = HE::AudioSpatialBackend::SteamAudio;
	CHECK_FALSE(cfg.isDefault());
	nlohmann::json j2;
	cfg.toJson(j2);
	CHECK(j2["spatial"] == "steamaudio");

	HE::AudioBusConfig back;
	back.fromJson(j2);
	CHECK(back.spatialBackend == HE::AudioSpatialBackend::SteamAudio);

	// An unknown value reads as the default rather than failing the project.
	HE::AudioBusConfig odd;
	odd.fromJson(nlohmann::json{ { "spatial", "fmod" } });
	CHECK(odd.spatialBackend == HE::AudioSpatialBackend::Miniaudio);
}

TEST_CASE("AudioBusConfig: the Steam Audio options are written only when on and survive a round trip")
{
	HE::AudioBusConfig cfg;
	nlohmann::json none;
	cfg.toJson(none);
	CHECK_FALSE(none.contains("steam"));

	cfg.steam.occlusion = true;
	cfg.steam.pathing   = true;
	CHECK_FALSE(cfg.isDefault());
	nlohmann::json j;
	cfg.toJson(j);
	REQUIRE(j.contains("steam"));
	CHECK(j["steam"].contains("occlusion"));
	CHECK_FALSE(j["steam"].contains("airAbsorption"));

	HE::AudioBusConfig back;
	back.fromJson(j);
	CHECK(back.steam == cfg.steam);
	CHECK_FALSE(back.steam.airAbsorption);
	CHECK_FALSE(back.steam.reflections);

	// Junk values read as off.
	HE::AudioBusConfig odd;
	odd.fromJson(nlohmann::json{ { "steam", { { "occlusion", "yes" }, { "reflections", 1 } } } });
	CHECK_FALSE(odd.steam.any());

	// The engine keeps the ask whatever the backend does.
	AudioEngine engine;
	REQUIRE(engine.init(true));
	engine.applyBusConfig(cfg);
	CHECK(engine.steamOptions() == cfg.steam);
	engine.shutdown();
}

// ─── Steam Audio ─────────────────────────────────────────────────────────────

TEST_CASE("listenerRelativeDirection puts a source in the listener's frame")
{
	// Listener at the origin looking down -z with +y up: the SDK's own frame.
	auto d = HE::listenerRelativeDirection(1, 0, 0,   0, 0, -1,   0, 1, 0);
	CHECK(d.x == doctest::Approx(1.0f));  CHECK(d.y == doctest::Approx(0.0f));  CHECK(d.z == doctest::Approx(0.0f));
	d = HE::listenerRelativeDirection(0, 0, -5,   0, 0, -1,   0, 1, 0);
	CHECK(d.z == doctest::Approx(-5.0f)); CHECK(d.x == doctest::Approx(0.0f));
	d = HE::listenerRelativeDirection(0, 2, 0,   0, 0, -1,   0, 1, 0);
	CHECK(d.y == doctest::Approx(2.0f));

	// Turn the listener to look along +x: a source on +x is now straight ahead,
	// and one on -z (the old "ahead") is to the listener's left.
	d = HE::listenerRelativeDirection(3, 0, 0,   1, 0, 0,   0, 1, 0);
	CHECK(d.z == doctest::Approx(-3.0f)); CHECK(d.x == doctest::Approx(0.0f));
	d = HE::listenerRelativeDirection(0, 0, -3,   1, 0, 0,   0, 1, 0);
	CHECK(d.x == doctest::Approx(-3.0f));

	// Degenerate input stays finite: the source is the listener, and a zero
	// forward/up falls back to the world axes.
	d = HE::listenerRelativeDirection(0, 0, 0,   0, 0, -1,   0, 1, 0);
	CHECK(d.z == doctest::Approx(-1.0f));
	d = HE::listenerRelativeDirection(1, 0, 0,   0, 0, 0,   0, 0, 0);
	CHECK(std::isfinite(d.x)); CHECK(d.x == doctest::Approx(1.0f));
}

TEST_CASE("AudioEngine: asking for Steam Audio never breaks playback, whatever the build")
{
	AudioEngine engine;
	REQUIRE(engine.init(true));
	CHECK(engine.spatialBackend() == HE::AudioSpatialBackend::Miniaudio);

	const bool honoured = engine.setSpatialBackend(HE::AudioSpatialBackend::SteamAudio);
	CHECK(engine.requestedSpatialBackend() == HE::AudioSpatialBackend::SteamAudio);
	if (AudioEngine::steamAudioCompiledIn())
	{
		// The build includes the SDK, and the library sits beside the test binary
		// (or in the SDK folder): it must have loaded.
		INFO(engine.spatialBackendStatus());
		CHECK(honoured);
		CHECK(engine.spatialBackend() == HE::AudioSpatialBackend::SteamAudio);
	}
	else
	{
		CHECK_FALSE(honoured);
		CHECK(engine.spatialBackend() == HE::AudioSpatialBackend::Miniaudio);
		CHECK(engine.spatialBackendStatus().find("not in this build") != std::string::npos);
	}

	// Either way, sound still plays.
	std::vector<uint8_t> pcm(4800 * 2, 0);
	CHECK(engine.playSpatial(pcm, 48000, 1, 1.0f, 1.0f, false, 1, 0, 0) != 0);

	// And back: always possible.
	CHECK(engine.setSpatialBackend(HE::AudioSpatialBackend::Miniaudio));
	CHECK(engine.spatialBackend() == HE::AudioSpatialBackend::Miniaudio);
	engine.shutdown();
}

TEST_CASE("AudioEngine: applyBusConfig carries the project's spatial backend")
{
	AudioEngine engine;
	REQUIRE(engine.init(true));
	HE::AudioBusConfig cfg;
	cfg.spatialBackend = HE::AudioSpatialBackend::SteamAudio;
	engine.applyBusConfig(cfg);
	CHECK(engine.requestedSpatialBackend() == HE::AudioSpatialBackend::SteamAudio);
	cfg.spatialBackend = HE::AudioSpatialBackend::Miniaudio;
	engine.applyBusConfig(cfg);
	CHECK(engine.requestedSpatialBackend() == HE::AudioSpatialBackend::Miniaudio);
	CHECK(engine.spatialBackend() == HE::AudioSpatialBackend::Miniaudio);
	engine.shutdown();
}

TEST_CASE("SteamAudio: without the SDK it reports itself unavailable instead of failing")
{
	if (HE::SteamAudio::compiledIn()) return;   // the case below covers a build that has it
	HE::SteamAudio sa;
	CHECK_FALSE(sa.init(48000, 256));
	CHECK_FALSE(sa.isReady());
	CHECK_FALSE(sa.lastError().empty());
	CHECK(sa.createBinaural() == nullptr);
	CHECK(std::string(HE::SteamAudio::version()).empty());
}

TEST_CASE("SteamAudio: the binaural renderer puts a source on the right into the right ear")
{
	if (!HE::SteamAudio::compiledIn()) return;

	HE::SteamAudio sa;
	INFO(sa.lastError());
	REQUIRE(sa.init(48000, 256));
	CHECK(sa.isReady());
	CHECK(sa.frameSize() == 256);
	auto fx = sa.createBinaural();
	REQUIRE(fx != nullptr);

	constexpr int kFrames = 256 * 8;
	std::vector<float> mono(kFrames);
	for (int i = 0; i < kFrames; ++i)
		mono[static_cast<size_t>(i)] = 0.5f * std::sin(2.0f * 3.14159265f * 500.0f * static_cast<float>(i) / 48000.0f);

	auto energy = [&](float x, float y, float z, double& l, double& r) {
		fx->reset();
		std::vector<float> out(static_cast<size_t>(kFrames) * 2, 0.0f);
		REQUIRE(fx->process(mono.data(), out.data(), kFrames, HE::SpatialDirection{ x, y, z }));
		l = r = 0.0;
		for (int i = 256; i < kFrames; ++i)   // skip the first block: filter warm-up
		{
			l += static_cast<double>(out[static_cast<size_t>(i) * 2])     * out[static_cast<size_t>(i) * 2];
			r += static_cast<double>(out[static_cast<size_t>(i) * 2 + 1]) * out[static_cast<size_t>(i) * 2 + 1];
		}
	};

	// How lopsided the two ears are, -1 (all left) .. +1 (all right).
	auto lean = [&](float x, float y, float z) {
		double l = 0, r = 0;
		energy(x, y, z, l, r);
		return (r - l) / (r + l);
	};
	const double right = lean(1, 0, 0);    // hard right
	const double left  = lean(-1, 0, 0);   // hard left
	const double ahead = lean(0, 0, -1);   // dead ahead
	INFO("lean right=" << right << " left=" << left << " ahead=" << ahead);
	CHECK(right > 0.3);
	CHECK(left  < -0.3);
	// The measured default HRTF is not perfectly symmetric, so "ahead" is not
	// exactly 0 — but it is far closer to the middle than either side.
	CHECK(std::abs(ahead) < 0.5 * std::min(right, -left));

	// A partial block is refused rather than shifting the filter memory.
	std::vector<float> out(2 * 100);
	CHECK_FALSE(fx->process(mono.data(), out.data(), 100, HE::SpatialDirection{}));

	// The effect keeps the library alive past shutdown of its parent.
	sa.shutdown();
	CHECK_FALSE(sa.isReady());
	std::vector<float> again(static_cast<size_t>(256) * 2);
	CHECK(fx->process(mono.data(), again.data(), 256, HE::SpatialDirection{}));
}

TEST_CASE("AudioEngine: with Steam Audio active a spatial voice is rendered through the HRTF")
{
	if (!HE::SteamAudio::compiledIn()) return;

	AudioEngine engine;
	REQUIRE(engine.init(true));
	REQUIRE(engine.outputChannels() == 2);
	INFO(engine.spatialBackendStatus());
	REQUIRE(engine.setSpatialBackend(HE::AudioSpatialBackend::SteamAudio));
	engine.setListenerTransform(0, 0, 0, 0, 0, -1, 0, 1, 0);

	std::vector<uint8_t> pcm(48000 * 2);   // one second of 500 Hz, 16-bit mono, looped
	for (int i = 0; i < 48000; ++i)
	{
		const int16_t v = static_cast<int16_t>(12000.0f * std::sin(2.0f * 3.14159265f * 500.0f * static_cast<float>(i) / 48000.0f));
		std::memcpy(pcm.data() + static_cast<size_t>(i) * 2, &v, 2);
	}

	// Plays one voice at (x,y,z) for a while and reports (left, right) energy.
	auto play = [&](float x, float y, float z, AudioAttenuation att, double& l, double& r) {
		const uint64_t h = engine.playSpatial(pcm, 48000, 1, 1.0f, 1.0f, true, x, y, z, 1.0f, 100.0f, "", att, 1.0f);
		REQUIRE(h != 0);
		std::vector<float> mix(2 * 4096);
		REQUIRE(engine.readMixedFrames(mix.data(), 4096) == 4096);   // settle: one block of latency, filter warm-up
		REQUIRE(engine.readMixedFrames(mix.data(), 4096) == 4096);
		l = r = 0.0;
		for (int i = 0; i < 4096; ++i)
		{
			l += static_cast<double>(mix[static_cast<size_t>(i) * 2])     * mix[static_cast<size_t>(i) * 2];
			r += static_cast<double>(mix[static_cast<size_t>(i) * 2 + 1]) * mix[static_cast<size_t>(i) * 2 + 1];
		}
		engine.stop(h);
	};

	double l = 0, r = 0;
	play(4, 0, 0, AudioAttenuation::None, l, r);
	CHECK(l + r > 1.0);                 // it is audible at all
	CHECK(r > 2.0 * l);                 // a source on the right is mostly in the right ear
	const double rightTotal = l + r;

	play(-4, 0, 0, AudioAttenuation::None, l, r);
	CHECK(l > 2.0 * r);

	// Distance falloff still applies (the node does it, miniaudio's spatializer is out).
	play(40, 0, 0, AudioAttenuation::Linear, l, r);
	CHECK(l + r < 0.5 * rightTotal);

	// Moving the voice while it plays moves the sound.
	{
		const uint64_t h = engine.playSpatial(pcm, 48000, 1, 1.0f, 1.0f, true, 4, 0, 0, 1.0f, 100.0f, "",
		                                      AudioAttenuation::None, 1.0f);
		REQUIRE(h != 0);
		std::vector<float> mix(2 * 4096);
		engine.readMixedFrames(mix.data(), 4096);
		engine.setSoundPosition(h, -4, 0, 0);
		engine.readMixedFrames(mix.data(), 4096);   // the move reaches the next block
		engine.readMixedFrames(mix.data(), 4096);
		double ll = 0, rr = 0;
		for (int i = 0; i < 4096; ++i)
		{
			ll += static_cast<double>(mix[static_cast<size_t>(i) * 2])     * mix[static_cast<size_t>(i) * 2];
			rr += static_cast<double>(mix[static_cast<size_t>(i) * 2 + 1]) * mix[static_cast<size_t>(i) * 2 + 1];
		}
		CHECK(ll > 2.0 * rr);

		// Switching the backend off with the voice still running must not pull the
		// library from under it; the voice carries on, new ones use miniaudio.
		CHECK(engine.setSpatialBackend(HE::AudioSpatialBackend::Miniaudio));
		CHECK(engine.readMixedFrames(mix.data(), 4096) == 4096);
		engine.stop(h);
	}
	engine.shutdown();
}

namespace
{
	// One second of a mono 16-bit sine at `hz`, looped by the caller.
	std::vector<uint8_t> sinePcm(float hz)
	{
		std::vector<uint8_t> pcm(48000 * 2);
		for (int i = 0; i < 48000; ++i)
		{
			const int16_t v = static_cast<int16_t>(12000.0f * std::sin(2.0f * 3.14159265f * hz * static_cast<float>(i) / 48000.0f));
			std::memcpy(pcm.data() + static_cast<size_t>(i) * 2, &v, 2);
		}
		return pcm;
	}

	// Mixed energy (both ears) of a looped voice at (x,0,0), listener at the origin looking down -Z.
	double voiceEnergy(AudioEngine& engine, const std::vector<uint8_t>& pcm, float x)
	{
		const uint64_t h = engine.playSpatial(pcm, 48000, 1, 1.0f, 1.0f, true, x, 0.0f, 0.0f, 1.0f, 1000.0f, "",
		                                      AudioAttenuation::None, 1.0f);
		REQUIRE(h != 0);
		std::vector<float> mix(2 * 4096);
		REQUIRE(engine.readMixedFrames(mix.data(), 4096) == 4096);   // settle
		REQUIRE(engine.readMixedFrames(mix.data(), 4096) == 4096);
		double e = 0.0;
		for (float v : mix) e += static_cast<double>(v) * v;
		engine.stop(h);
		return e;
	}
}

TEST_CASE("Steam Audio option: air absorption dulls a far voice's highs and leaves its lows")
{
	if (!HE::SteamAudio::compiledIn()) return;

	AudioEngine engine;
	REQUIRE(engine.init(true));
	engine.setListenerTransform(0, 0, 0, 0, 0, -1, 0, 1, 0);
	INFO(engine.spatialBackendStatus());
	REQUIRE(engine.setSpatialBackend(HE::AudioSpatialBackend::SteamAudio));

	const std::vector<uint8_t> low  = sinePcm(150.0f);
	const std::vector<uint8_t> high = sinePcm(6000.0f);

	// Off: distance changes nothing (attenuation None), so a far voice is as loud as a near one.
	const double lowNear0 = voiceEnergy(engine, low, 2.0f),  lowFar0 = voiceEnergy(engine, low, 150.0f);
	const double hiNear0  = voiceEnergy(engine, high, 2.0f), hiFar0  = voiceEnergy(engine, high, 150.0f);
	CHECK(hiFar0 > 0.5 * hiNear0);
	CHECK(lowFar0 > 0.5 * lowNear0);

	// On: the far highs go, the far lows mostly stay.
	HE::AudioSteamOptions opt;
	opt.airAbsorption = true;
	engine.setSteamOptions(opt);
	const double lowNear = voiceEnergy(engine, low, 2.0f),  lowFar = voiceEnergy(engine, low, 150.0f);
	const double hiNear  = voiceEnergy(engine, high, 2.0f), hiFar  = voiceEnergy(engine, high, 150.0f);
	INFO("low near/far " << lowNear << " / " << lowFar << ", high near/far " << hiNear << " / " << hiFar);
	CHECK(hiFar < 0.2 * hiNear);          // the highs lose most of their energy at 150 m
	CHECK(lowFar > 0.5 * lowNear);        // the lows barely notice
	CHECK(hiFar / hiNear < lowFar / lowNear);

	// The option is read when a voice starts: turning it off again restores a new voice.
	engine.setSteamOptions({});
	CHECK(voiceEnergy(engine, high, 150.0f) > 0.5 * hiNear0);
	engine.shutdown();
}

TEST_CASE("Occlusion estimate: a wall between source and listener blocks the rays, a gap lets some through")
{
	using HE::estimateOcclusion;
	const glm::vec3 listener(0, 1.6f, 0), source(10, 1.6f, 0);

	// Open air.
	CHECK(estimateOcclusion(listener, source, [](const glm::vec3&, const glm::vec3&, float) { return false; }).visibility == 1.0f);

	// A wall across the whole line (x = 5): every ray is stopped.
	auto wall = [](const glm::vec3& from, const glm::vec3& dir, float maxDist) {
		if (std::abs(dir.x) < 1.0e-5f) return false;
		const float t = (5.0f - from.x) / dir.x;
		return t > 0.0f && t < maxDist;
	};
	const HE::OcclusionResult blocked = estimateOcclusion(listener, source, wall);
	CHECK(blocked.visibility == 0.0f);
	CHECK(blocked.transmission[0] > blocked.transmission[2]);   // bass passes better than treble

	// A pillar that only covers the centre: some rays still get past.
	auto pillar = [](const glm::vec3& from, const glm::vec3& dir, float maxDist) {
		if (std::abs(dir.x) < 1.0e-5f) return false;
		const float t = (5.0f - from.x) / dir.x;
		if (!(t > 0.0f && t < maxDist)) return false;
		const glm::vec3 p = from + dir * t;
		return std::abs(p.z) < 0.1f && std::abs(p.y - 1.6f) < 0.1f;
	};
	const float partial = estimateOcclusion(listener, source, pillar).visibility;
	CHECK(partial > 0.0f);
	CHECK(partial < 1.0f);

	// On top of the listener there is nothing between.
	CHECK(estimateOcclusion(listener, listener + glm::vec3(0.2f, 0, 0), wall).visibility == 1.0f);
}

TEST_CASE("Steam Audio option: occlusion dulls and dims a voice that the geometry hides")
{
	if (!HE::SteamAudio::compiledIn()) return;

	AudioEngine engine;
	REQUIRE(engine.init(true));
	engine.setListenerTransform(0, 0, 0, 0, 0, -1, 0, 1, 0);
	REQUIRE(engine.setSpatialBackend(HE::AudioSpatialBackend::SteamAudio));
	HE::AudioSteamOptions opt;
	opt.occlusion = true;
	engine.setSteamOptions(opt);

	const std::vector<uint8_t> low  = sinePcm(150.0f);
	const std::vector<uint8_t> high = sinePcm(6000.0f);
	const float wall[3] = { HE::kOcclusionTransmission[0], HE::kOcclusionTransmission[1], HE::kOcclusionTransmission[2] };

	auto energy = [&](const std::vector<uint8_t>& pcm, float visibility) {
		const uint64_t h = engine.playSpatial(pcm, 48000, 1, 1.0f, 1.0f, true, 4.0f, 0.0f, 0.0f, 1.0f, 1000.0f, "",
		                                      AudioAttenuation::None, 1.0f);
		REQUIRE(h != 0);
		engine.setSoundOcclusion(h, visibility, wall);
		std::vector<float> mix(2 * 4096);
		REQUIRE(engine.readMixedFrames(mix.data(), 4096) == 4096);
		REQUIRE(engine.readMixedFrames(mix.data(), 4096) == 4096);
		double e = 0.0;
		for (float v : mix) e += static_cast<double>(v) * v;
		engine.stop(h);
		return e;
	};

	const double lowOpen = energy(low, 1.0f),  lowWall = energy(low, 0.0f);
	const double hiOpen  = energy(high, 1.0f), hiWall  = energy(high, 0.0f);
	INFO("low open/wall " << lowOpen << " / " << lowWall << ", high open/wall " << hiOpen << " / " << hiWall);
	CHECK(lowWall < 0.5 * lowOpen);          // dimmed
	CHECK(lowWall > 0.01 * lowOpen);         // but bass still leaks through
	CHECK(hiWall < 0.05 * hiOpen);           // the treble is mostly gone
	CHECK(hiWall / hiOpen < lowWall / lowOpen);

	// Without the option the same call changes nothing.
	engine.setSteamOptions({});
	CHECK(energy(high, 0.0f) > 0.5 * hiOpen);
	engine.shutdown();
}

TEST_CASE("Room estimate: a closed room rings, open ground and a single wall barely do")
{
	using HE::estimateRoom;
	// Open ground: nothing in range.
	const HE::RoomEstimate open = estimateRoom(glm::vec3(0), [](const glm::vec3&, const glm::vec3&, float) { return -1.0f; });
	CHECK(open.wet == 0.0f);
	CHECK(open.reverbTimes[1] == 0.0f);

	// A cube room, 4 m to every wall along the ray's own axis (a ray to a corner is longer).
	auto box = [](float half) {
		return [half](const glm::vec3&, const glm::vec3& d, float maxDist) {
			const float m = std::max({ std::abs(d.x), std::abs(d.y), std::abs(d.z) });
			const float t = half / m;
			return t < maxDist ? t : -1.0f;
		};
	};
	const HE::RoomEstimate small = estimateRoom(glm::vec3(0), box(2.0f));
	const HE::RoomEstimate big   = estimateRoom(glm::vec3(0), box(10.0f));
	CHECK(small.closedFraction == doctest::Approx(1.0f));
	CHECK(small.reverbTimes[1] > 0.1f);
	CHECK(big.reverbTimes[1] > small.reverbTimes[1]);               // a bigger room rings longer
	CHECK(small.reverbTimes[0] > small.reverbTimes[2]);             // the bass outlasts the treble
	CHECK(small.wet > 0.2f);

	// One wall only (a quarter of the rays hit): little reverb.
	auto oneWall = [](const glm::vec3&, const glm::vec3& d, float maxDist) {
		if (d.x <= 0.5f) return -1.0f;
		const float t = 5.0f / d.x;
		return t < maxDist ? t : -1.0f;
	};
	const HE::RoomEstimate wall = estimateRoom(glm::vec3(0), oneWall);
	CHECK(wall.closedFraction < 0.5f);
	CHECK(wall.reverbTimes[1] < 0.3f * big.reverbTimes[1]);
	CHECK(wall.wet < small.wet);
}

TEST_CASE("Steam Audio option: reflections leave a reverb tail after the sound has ended")
{
	if (!HE::SteamAudio::compiledIn()) return;

	AudioEngine engine;
	REQUIRE(engine.init(true));
	engine.setListenerTransform(0, 0, 0, 0, 0, -1, 0, 1, 0);
	REQUIRE(engine.setSpatialBackend(HE::AudioSpatialBackend::SteamAudio));

	// A 50 ms noise burst, then the clip is over.
	std::vector<uint8_t> burst(2400 * 2);
	uint32_t seed = 12345;
	for (int i = 0; i < 2400; ++i)
	{
		seed = seed * 1664525u + 1013904223u;
		const int16_t v = static_cast<int16_t>((static_cast<int>(seed >> 16) & 0x3FFF) - 0x2000);
		std::memcpy(burst.data() + static_cast<size_t>(i) * 2, &v, 2);
	}

	// Energy of the mix 0.2 s..0.4 s after the start (the burst is long over by then).
	auto tail = [&]() {
		const uint64_t h = engine.playSpatial(burst, 48000, 1, 1.0f, 1.0f, false, 3.0f, 0.0f, 0.0f, 1.0f, 1000.0f, "",
		                                      AudioAttenuation::None, 1.0f);
		REQUIRE(h != 0);
		std::vector<float> mix(2 * 4800);
		REQUIRE(engine.readMixedFrames(mix.data(), 4800) == 4800);   // the burst
		double e = 0.0;
		for (int pass = 0; pass < 2; ++pass)
		{
			REQUIRE(engine.readMixedFrames(mix.data(), 4800) == 4800);
			for (float v : mix) e += static_cast<double>(v) * v;
		}
		engine.stop(h);
		return e;
	};

	const float times[3] = { 1.6f, 1.4f, 1.0f };
	engine.setRoomReverb(times, 0.5f);

	const double dry = tail();                  // option off: the clip is over, silence
	HE::AudioSteamOptions opt;
	opt.reflections = true;
	engine.setSteamOptions(opt);
	const double wet = tail();
	INFO("tail energy off/on " << dry << " / " << wet);
	CHECK(dry < 1.0e-6);
	CHECK(wet > 1.0e-3);

	// No room (an open field): the same option leaves no tail.
	const float none[3] = { 0, 0, 0 };
	engine.setRoomReverb(none, 0.0f);
	CHECK(tail() < 1.0e-6);
	engine.shutdown();
}
