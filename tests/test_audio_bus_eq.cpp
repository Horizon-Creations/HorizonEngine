#include "doctest.h"
#include <HorizonScene/AudioEngine.h>
#include <HorizonScene/AudioSystem.h>
#include <HorizonScene/HorizonWorld.h>
#include <ContentManager/ContentManager.h>
#include <Audio/AudioBusConfig.h>
#include <Audio/AudioEdit.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

// ─── Bus routing and EQ of an audio asset (Audio Editor, Thema 168 step 5) ───
// What the Audio Editor sets on an asset — the mixer bus it plays through and
// its EQ — as the engine plays it. Routing is checked by where the voice lands
// (getSoundBus, busVoiceCount); the EQ by what comes out of the mixer: a sine
// played with and without the EQ, the level ratio held against the response
// AudioEq::responseDb promises at the rate the filter runs at (the clip's).
// All in noDevice mode, where readMixedFrames is the output device.

namespace
{
	constexpr double kPi = 3.14159265358979323846;

	// A looping stereo sine: `cycles` whole periods in `frames`, so the loop
	// seam is seamless and the steady state is a pure tone.
	AudioAsset sineClip(int rate, int frames, int cycles, double amp = 0.25)
	{
		AudioAsset a;
		a.type       = HE::AssetType::Audio;
		a.name       = "Sine";
		a.sampleRate = rate;
		a.channels   = 2;
		a.encoding   = AudioEncoding::PCM16;
		a.audioData.resize(static_cast<size_t>(frames) * 2 * sizeof(int16_t));
		auto* s = reinterpret_cast<int16_t*>(a.audioData.data());
		for (int f = 0; f < frames; ++f)
		{
			const double v = amp * std::sin(2.0 * kPi * double(cycles) * double(f) / double(frames));
			s[f * 2 + 0] = s[f * 2 + 1] = static_cast<int16_t>(std::lround(v * 32767.0));
		}
		return a;
	}

	AudioAsset silentClip(int frames = 4800)
	{
		AudioAsset a;
		a.type = HE::AssetType::Audio; a.name = "Click";
		a.sampleRate = 48000; a.channels = 2; a.encoding = AudioEncoding::PCM16;
		a.audioData.assign(static_cast<size_t>(frames) * 4, 0);
		return a;
	}

	std::vector<float> pull(AudioEngine& engine, uint64_t frames)
	{
		std::vector<float> out(static_cast<size_t>(frames) * 2, 0.0f);
		uint64_t done = 0;
		while (done < frames)
		{
			const uint64_t n   = std::min<uint64_t>(480, frames - done);
			const uint64_t got = engine.readMixedFrames(out.data() + done * 2, n);
			if (got == 0) break;
			done += got;
		}
		return out;
	}

	// RMS of the left channel over [from, to) of a pulled mix.
	double rmsOf(const std::vector<float>& mix, size_t from, size_t to)
	{
		double acc = 0.0;
		for (size_t i = from; i < to; ++i) acc += double(mix[i * 2]) * double(mix[i * 2]);
		return std::sqrt(acc / double(to - from));
	}

	// Level of `clip` through the engine, in dB relative to the same clip with
	// no EQ. The filter's start-up transient is skipped (the first 0.1 s).
	double measuredEqDb(const AudioAsset& plain, const HE::AudioEq& eq)
	{
		auto levelOf = [](const AudioAsset& clip) {
			AudioEngine engine;
			REQUIRE(engine.init(true));
			const uint64_t h = engine.play(clip, 1.0f, 1.0f, /*loop=*/true);
			REQUIRE(h != 0);
			const std::vector<float> mix = pull(engine, 24000);
			engine.shutdown();
			return rmsOf(mix, 4800, 24000);
		};
		AudioAsset withEq = plain;
		withEq.edit.eq    = eq;
		return 20.0 * std::log10(levelOf(withEq) / levelOf(plain));
	}

	HE::AudioEqBand band(HE::AudioEqBandType type, float freq, float gainDb, float q)
	{
		HE::AudioEqBand b;
		b.type = type; b.freqHz = freq; b.gainDb = gainDb; b.q = q;
		return b;
	}
}

TEST_CASE("AudioEngine: an asset plays on its own bus, a source's bus overrides it, a missing one falls through")
{
	AudioEngine engine;
	REQUIRE(engine.init(true));
	REQUIRE(engine.createBus("Music"));
	REQUIRE(engine.createBus("SFX"));

	AudioAsset clip = silentClip();
	clip.edit.bus = "Music";

	SUBCASE("the asset's bus when the source names none")
	{
		const uint64_t h = engine.play(clip);
		REQUIRE(h != 0);
		CHECK(engine.getSoundBus(h) == "Music");
		CHECK(engine.busVoiceCount("Music") == 1);
		CHECK(engine.busVoiceCount("") == 0);
	}
	SUBCASE("the source's own bus wins over the asset's")
	{
		const uint64_t h = engine.play(clip, 1.0f, 1.0f, false, "SFX");
		CHECK(engine.getSoundBus(h) == "SFX");
		CHECK(engine.busVoiceCount("SFX") == 1);
		CHECK(engine.busVoiceCount("Music") == 0);
	}
	SUBCASE("a source naming a bus that does not exist gets the asset's, not master")
	{
		const uint64_t h = engine.play(clip, 1.0f, 1.0f, false, "Gone");
		CHECK(engine.getSoundBus(h) == "Music");
	}
	SUBCASE("an asset naming a deleted bus plays on master — and is heard")
	{
		clip.edit.bus = "Ambience";   // never created: a bus removed or renamed in the mixer
		const uint64_t h = engine.play(clip);
		REQUIRE(h != 0);
		CHECK(engine.getSoundBus(h) == "");
		CHECK(engine.busVoiceCount("") == 1);
		CHECK(engine.isPlaying(h));
	}
	SUBCASE("a bus removed while the project runs: the next voice falls back to master")
	{
		REQUIRE(engine.removeBus("Music"));
		const uint64_t h = engine.play(clip);
		CHECK(engine.getSoundBus(h) == "");
		CHECK(engine.busVoiceCount("") == 1);
	}
	SUBCASE("spatial voices route the same way")
	{
		const uint64_t a = engine.playSpatial(clip, 1.0f, 1.0f, false, 0.0f, 0.0f, 0.0f);
		const uint64_t b = engine.playSpatial(clip, 1.0f, 1.0f, false, 0.0f, 0.0f, 0.0f,
		                                      1.0f, 20.0f, "SFX");
		CHECK(engine.getSoundBus(a) == "Music");
		CHECK(engine.getSoundBus(b) == "SFX");
	}
	SUBCASE("routeFor is the same rule, for the editor's preview")
	{
		CHECK(engine.routeFor("", "Music") == "Music");
		CHECK(engine.routeFor("SFX", "Music") == "SFX");
		CHECK(engine.routeFor("Gone", "Music") == "Music");
		CHECK(engine.routeFor("", "Gone") == "");
		CHECK(engine.routeFor("", "") == "");
	}
	engine.shutdown();
}

TEST_CASE("AudioSystem: a source with no bus of its own plays on its asset's bus, one with a bus keeps it")
{
	HorizonWorld   world;
	ContentManager content;
	AudioAsset clip = silentClip();
	clip.edit.bus = "Music";
	const HE::UUID id = content.registerAudio(clip);

	AudioSourceComponent src;
	src.playOnStart = true;
	src.assetId     = id;
	auto plain = world.createEntity("Plain");
	world.registry().emplace<AudioSourceComponent>(plain, src);
	src.busName = "Voice";
	auto over = world.createEntity("Override");
	world.registry().emplace<AudioSourceComponent>(over, src);
	src.busName = "Deleted";
	auto stale = world.createEntity("Stale");
	world.registry().emplace<AudioSourceComponent>(stale, src);

	AudioEngine engine;
	REQUIRE(engine.init(true));
	HE::AudioBusConfig cfg;
	cfg.add("Music"); cfg.add("Voice");
	engine.applyBusConfig(cfg);
	AudioSystem::playOnStart(world, engine, &content);

	auto& reg = world.registry();
	CHECK(engine.getSoundBus(reg.get<AudioSourceComponent>(plain).handle) == "Music");
	CHECK(engine.getSoundBus(reg.get<AudioSourceComponent>(over).handle)  == "Voice");
	CHECK(engine.getSoundBus(reg.get<AudioSourceComponent>(stale).handle) == "Music");
	engine.shutdown();
}

TEST_CASE("AudioEngine: an asset's EQ is what the mixer puts out, at the response it promises")
{
	// 1 s at 48 kHz (the engine's own rate). Each tone gets an integer number of
	// cycles in the clip so the loop is seamless: 100, 1000 and 6000 Hz.
	HE::AudioEq eq;
	eq.bands = { band(HE::AudioEqBandType::HighPass, 250.0f, 0.0f, 0.7071f),
	             band(HE::AudioEqBandType::Peak,     1000.0f, 9.0f, 1.4f) };
	for (const int f : { 100, 1000, 6000 })
	{
		CAPTURE(f);
		const AudioAsset clip = sineClip(48000, 48000, f);
		const double want = eq.responseDb(f, 48000.0);
		const double got  = measuredEqDb(clip, eq);
		CHECK(std::fabs(got - want) < 0.2);
	}
	// What the numbers above are: the high-pass takes 100 Hz down by ~12 dB, the
	// bell lifts 1 kHz by its 9 dB (the high-pass costs it a hair), and 6 kHz
	// is all but untouched. Checked so the comparison cannot pass on two flat lines.
	CHECK(eq.responseDb(100.0, 48000.0)  < -10.0);
	CHECK(eq.responseDb(1000.0, 48000.0) > 8.5);
	CHECK(std::fabs(eq.responseDb(6000.0, 48000.0)) < 0.6);
}

TEST_CASE("AudioEngine: the EQ runs at the clip's rate, before the resampler")
{
	// A 24 kHz clip on the 48 kHz engine. Near the clip's Nyquist the bilinear
	// transform warps a band a lot, so the response at the clip's rate and at
	// the mixer's are far apart at 9 kHz — the measurement must match the first.
	HE::AudioEq eq;
	eq.bands = { band(HE::AudioEqBandType::HighShelf, 6000.0f, -12.0f, 0.7071f) };
	const AudioAsset clip = sineClip(24000, 24000, 9000);
	const double atClipRate  = eq.responseDb(9000.0, 24000.0);
	const double atMixerRate = eq.responseDb(9000.0, 48000.0);
	REQUIRE(std::fabs(atClipRate - atMixerRate) > 1.0);
	const double got = measuredEqDb(clip, eq);
	CHECK(std::fabs(got - atClipRate) < 0.3);
	CHECK(std::fabs(got - atMixerRate) > 0.7);
}

TEST_CASE("AudioEngine: a neutral EQ takes the untouched path, bit for bit")
{
	AudioAsset clip = sineClip(48000, 4800, 100);
	clip.edit.eq.bands = { band(HE::AudioEqBandType::Peak, 1000.0f, 0.0f, 1.0f),
	                       band(HE::AudioEqBandType::LowShelf, 200.0f, 6.0f, 0.7f) };
	clip.edit.eq.bands[1].enabled = false;
	REQUIRE(clip.edit.eq.isNeutral());

	AudioAsset bypassed = clip;
	bypassed.edit.eq.bands[1].enabled = true;
	bypassed.edit.eq.enabled = false;   // the bypass switch: bands kept, nothing run
	REQUIRE(bypassed.edit.eq.isNeutral());

	std::vector<float> ref;
	{
		AudioEngine engine;
		REQUIRE(engine.init(true));
		AudioAsset none = clip;
		none.edit.eq = HE::AudioEq{};
		REQUIRE(engine.play(none) != 0);
		ref = pull(engine, 4000);
		engine.shutdown();
	}
	for (const AudioAsset* a : { &clip, &bypassed })
	{
		AudioEngine engine;
		REQUIRE(engine.init(true));
		const uint64_t h = engine.play(*a);
		REQUIRE(h != 0);
		CHECK_FALSE(engine.hasSoundEnvelope(h));   // no stage at all
		CHECK_FALSE(engine.hasSoundEq(h));
		CHECK(pull(engine, 4000) == ref);
		engine.shutdown();
	}

	// Negative control: the same clip with the shelf live does change the output.
	AudioEngine engine;
	REQUIRE(engine.init(true));
	clip.edit.eq.bands[1].enabled = true;
	const uint64_t h = engine.play(clip);
	CHECK(engine.hasSoundEq(h));
	CHECK(pull(engine, 4000) != ref);
	engine.shutdown();
}

TEST_CASE("AudioEngine: the preview's EQ can be switched in while it plays, and out again")
{
	AudioEngine engine;
	REQUIRE(engine.init(true));
	const AudioAsset clip = sineClip(48000, 48000, 1000);
	const uint64_t h = engine.play(clip.audioData, clip.sampleRate, clip.channels,
	                               HE::AudioEnvelope{}, 0, 1.0f, 1.0f, true, {});
	REQUIRE(h != 0);
	CHECK(engine.hasSoundEnvelope(h));   // the stage is in for a preview, curve or not
	CHECK_FALSE(engine.hasSoundEq(h));

	const double before = rmsOf(pull(engine, 9600), 2400, 9600);

	HE::AudioEq eq;
	eq.bands = { band(HE::AudioEqBandType::Peak, 1000.0f, -12.0f, 1.0f) };
	REQUIRE(engine.setSoundEq(h, eq));
	CHECK(engine.hasSoundEq(h));
	const double cut = rmsOf(pull(engine, 9600), 2400, 9600);
	CHECK(20.0 * std::log10(cut / before) == doctest::Approx(-12.0).epsilon(0.02));

	REQUIRE(engine.setSoundEq(h, HE::AudioEq{}));
	CHECK_FALSE(engine.hasSoundEq(h));
	const double after = rmsOf(pull(engine, 9600), 2400, 9600);
	CHECK(20.0 * std::log10(after / before) == doctest::Approx(0.0).scale(1.0).epsilon(0.05));

	// An asset voice without a curve or an EQ has no stage to put one in.
	const uint64_t plain = engine.play(clip);
	CHECK_FALSE(engine.setSoundEq(plain, eq));
	CHECK_FALSE(engine.setSoundEq(987654, eq));
	engine.shutdown();
}
