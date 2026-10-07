#include "doctest.h"
#include "TestFsUtil.h"
#include <Audio/AudioEdit.h>
#include <Audio/AudioBusConfig.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/HAsset.h>
#include "ImporterCommon.h"   // importSource — the re-import keeps the edits
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

// The non-destructive edit model of the Audio Editor (Thema 168, step 1): trim,
// volume curve, bus, EQ — the data, its file form and the maths. No UI and no
// playback here; those read this model.

namespace fs = std::filesystem;
using namespace HE;

namespace
{
	// Unique per run: worktrees share TMPDIR, and two he_tests running this file
	// at once must not wipe each other's content root.
	struct TempDir
	{
		fs::path path;
		explicit TempDir(const char* name)
		{
			path = fs::temp_directory_path()
			     / (std::string(name) + "_" + std::to_string(std::random_device{}()));
			he_test::removeAllQuiet(path);
			fs::create_directories(path);
		}
		~TempDir() { he_test::removeAllQuiet(path); }
	};

	// A minimal 16-bit PCM WAV: `frames` frames of a ramp, mono, at `rate`.
	bool writeWav(const fs::path& file, int frames, int rate, int16_t base)
	{
		std::ofstream f(file, std::ios::binary | std::ios::trunc);
		if (!f) return false;
		auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
		auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
		const uint32_t dataBytes = static_cast<uint32_t>(frames) * 2;
		f.write("RIFF", 4); u32(36 + dataBytes); f.write("WAVE", 4);
		f.write("fmt ", 4); u32(16); u16(1); u16(1); u32(static_cast<uint32_t>(rate));
		u32(static_cast<uint32_t>(rate) * 2); u16(2); u16(16);
		f.write("data", 4); u32(dataBytes);
		for (int i = 0; i < frames; ++i) u16(static_cast<uint16_t>(static_cast<int16_t>(base + i)));
		return static_cast<bool>(f);
	}

	AudioEdit sampleEdit()
	{
		AudioEdit e;
		e.trim.startFrame = 4800;
		e.trim.endFrame   = 96000;
		e.envelope.points = { { 0.0, 0.0f, AudioCurveInterp::Smooth },
		                      { 0.5, 1.0f, AudioCurveInterp::Hold },
		                      { 1.5, 0.5f, AudioCurveInterp::Exponential },
		                      { 2.0, 0.0f, AudioCurveInterp::Linear } };
		e.bus = "Music";
		AudioEqBand low;  low.type = AudioEqBandType::LowShelf; low.freqHz = 120.0f; low.gainDb = -6.0f;
		AudioEqBand mid;  mid.freqHz = 2500.0f; mid.gainDb = 3.5f; mid.q = 2.0f; mid.enabled = false;
		AudioEqBand cut;  cut.type = AudioEqBandType::LowPass; cut.freqHz = 9000.0f;
		e.eq.bands = { low, mid, cut };
		return e;
	}

	bool sameEdit(const AudioEdit& a, const AudioEdit& b)
	{
		if (a.trim.startFrame != b.trim.startFrame || a.trim.endFrame != b.trim.endFrame) return false;
		if (a.bus != b.bus || a.eq.enabled != b.eq.enabled) return false;
		if (a.envelope.points.size() != b.envelope.points.size()) return false;
		for (size_t i = 0; i < a.envelope.points.size(); ++i)
		{
			const auto& p = a.envelope.points[i]; const auto& q = b.envelope.points[i];
			if (p.timeSec != q.timeSec || p.gain != q.gain || p.interp != q.interp) return false;
		}
		if (a.eq.bands.size() != b.eq.bands.size()) return false;
		for (size_t i = 0; i < a.eq.bands.size(); ++i)
		{
			const auto& p = a.eq.bands[i]; const auto& q = b.eq.bands[i];
			if (p.type != q.type || p.freqHz != q.freqHz || p.gainDb != q.gainDb
			    || p.q != q.q || p.enabled != q.enabled) return false;
		}
		return true;
	}

	// Stable when both poles lie inside the unit circle (the classic triangle).
	bool stable(const BiquadCoeffs& c)
	{
		return std::abs(c.a2) < 1.0 && std::abs(c.a1) < 1.0 + c.a2;
	}
}

// ─── Trim ─────────────────────────────────────────────────────────────────────

TEST_CASE("AudioTrim: default is the whole clip, ends clamp, an empty trim plays everything")
{
	AudioTrim t;
	CHECK(t.isDefault());
	auto r = t.resolve(1000);
	CHECK(r.begin == 0);   CHECK(r.end == 1000);

	t.startFrame = 100; t.endFrame = 400;
	r = t.resolve(1000);
	CHECK(r.begin == 100); CHECK(r.end == 400);

	t.endFrame = 5000;                     // past the end (clip re-imported shorter)
	r = t.resolve(1000);
	CHECK(r.begin == 100); CHECK(r.end == 1000);

	t.startFrame = 600; t.endFrame = 300;  // inverted: never silence, play it all
	r = t.resolve(1000);
	CHECK(r.begin == 0);   CHECK(r.end == 1000);

	t.startFrame = 2000; t.endFrame = 0;   // start past the end
	r = t.resolve(1000);
	CHECK(r.begin == 0);   CHECK(r.end == 1000);
}

// ─── Envelope ─────────────────────────────────────────────────────────────────

TEST_CASE("AudioEnvelope::evalGain: empty, one point, before/after the ends")
{
	AudioEnvelope env;
	CHECK(env.evalGain(0.0)  == doctest::Approx(1.0f));   // no curve = unity
	CHECK(env.evalGain(42.0) == doctest::Approx(1.0f));

	env.points = { { 1.0, 0.25f } };
	CHECK(env.evalGain(0.0) == doctest::Approx(0.25f));
	CHECK(env.evalGain(1.0) == doctest::Approx(0.25f));
	CHECK(env.evalGain(9.0) == doctest::Approx(0.25f));

	env.points = { { 1.0, 0.2f }, { 2.0, 0.8f } };
	CHECK(env.evalGain(-5.0) == doctest::Approx(0.2f));   // before the first point
	CHECK(env.evalGain(1.0)  == doctest::Approx(0.2f));   // exactly on a point
	CHECK(env.evalGain(2.0)  == doctest::Approx(0.8f));
	CHECK(env.evalGain(30.0) == doctest::Approx(0.8f));   // after the last
}

TEST_CASE("AudioEnvelope::evalGain: every interpolation, the left point owns the segment")
{
	AudioEnvelope env;
	env.points = { { 0.0, 0.0f, AudioCurveInterp::Linear }, { 1.0, 1.0f } };
	CHECK(env.evalGain(0.25) == doctest::Approx(0.25f));
	CHECK(env.evalGain(0.5)  == doctest::Approx(0.5f));

	env.points[0].interp = AudioCurveInterp::Hold;
	CHECK(env.evalGain(0.999) == doctest::Approx(0.0f));   // a step at the next point
	CHECK(env.evalGain(1.0)   == doctest::Approx(1.0f));

	env.points[0].interp = AudioCurveInterp::Smooth;
	CHECK(env.evalGain(0.5)  == doctest::Approx(0.5f));                   // symmetric
	CHECK(env.evalGain(0.25) == doctest::Approx(0.15625f));               // 3f²−2f³
	CHECK(env.evalGain(0.25) < 0.25f);                                    // eases in

	// Exponential = straight in dB: 1.0 → 0.01 (0 → −40 dB) is −20 dB = 0.1 halfway.
	env.points = { { 0.0, 1.0f, AudioCurveInterp::Exponential }, { 1.0, 0.01f } };
	CHECK(env.evalGain(0.5) == doctest::Approx(0.1f).epsilon(1e-4));
	// Fading out to a 0 point: monotonically down, exactly 0 at the point.
	env.points = { { 0.0, 1.0f, AudioCurveInterp::Exponential }, { 1.0, 0.0f } };
	CHECK(env.evalGain(0.0) == doctest::Approx(1.0f));
	CHECK(env.evalGain(0.5) < env.evalGain(0.25));
	CHECK(env.evalGain(0.5) > 0.0f);
	CHECK(env.evalGain(1.0) == 0.0f);
	// Both ends silent: silence, not the −100 dB floor.
	env.points = { { 0.0, 0.0f, AudioCurveInterp::Exponential }, { 1.0, 0.0f } };
	CHECK(env.evalGain(0.5) == 0.0f);
}

TEST_CASE("AudioEnvelope: two points at one time are a jump, sort() keeps their order")
{
	AudioEnvelope env;
	env.points = { { 2.0, 0.5f }, { 1.0, 1.0f }, { 1.0, 0.0f }, { 0.0, 1.0f } };
	env.sort();
	REQUIRE(env.points.size() == 4);
	CHECK(env.points[0].timeSec == 0.0);
	CHECK(env.points[1].gain == 1.0f);   // authored first, stays first
	CHECK(env.points[2].gain == 0.0f);
	CHECK(env.evalGain(0.999) == doctest::Approx(1.0f).epsilon(1e-2));
	CHECK(env.evalGain(1.0)   == doctest::Approx(0.0f));   // the later point wins from t on
	CHECK(env.evalGain(1.5)   == doctest::Approx(0.25f));
}

// ─── EQ coefficients ─────────────────────────────────────────────────────────

TEST_CASE("biquadCoefficients: neutral bands are the exact identity")
{
	AudioEqBand peak;                     // 0 dB peak
	CHECK(biquadCoefficients(peak, 48000.0).isIdentity());
	AudioEqBand shelf; shelf.type = AudioEqBandType::HighShelf; shelf.gainDb = 0.0f;
	CHECK(biquadCoefficients(shelf, 48000.0).isIdentity());
	AudioEqBand off; off.gainDb = 12.0f; off.enabled = false;
	CHECK(biquadCoefficients(off, 48000.0).isIdentity());
	AudioEqBand any; any.gainDb = 6.0f;
	CHECK(biquadCoefficients(any, 0.0).isIdentity());     // no rate, no filter
	// A pass filter is never neutral — it cuts whatever its gain says.
	AudioEqBand lp; lp.type = AudioEqBandType::LowPass;
	CHECK_FALSE(biquadCoefficients(lp, 48000.0).isIdentity());
}

TEST_CASE("biquadCoefficients: measured response matches what each band promises")
{
	const double fs = 48000.0;

	SUBCASE("peak: +gain at the centre, flat far away")
	{
		for (float g : { 6.0f, -9.0f, 12.0f })
		{
			AudioEqBand b; b.freqHz = 1000.0f; b.gainDb = g; b.q = 1.0f;
			const BiquadCoeffs c = biquadCoefficients(b, fs);
			CHECK(stable(c));
			CHECK(c.magnitudeDb(1000.0, fs)  == doctest::Approx(g).epsilon(1e-6));
			CHECK(std::abs(c.magnitudeDb(10.0, fs))    < 0.05);
			CHECK(std::abs(c.magnitudeDb(23900.0, fs)) < 0.3);
		}
	}
	SUBCASE("low shelf: full gain at DC, flat at the top, half way at f0")
	{
		AudioEqBand b; b.type = AudioEqBandType::LowShelf; b.freqHz = 200.0f; b.gainDb = -8.0f;
		const BiquadCoeffs c = biquadCoefficients(b, fs);
		CHECK(stable(c));
		CHECK(c.magnitudeDb(0.0, fs) == doctest::Approx(-8.0).epsilon(1e-6));
		CHECK(std::abs(c.magnitudeDb(20000.0, fs)) < 0.05);
		CHECK(c.magnitudeDb(200.0, fs) == doctest::Approx(-4.0).epsilon(1e-6));   // RBJ: gain/2 at f0
	}
	SUBCASE("high shelf: full gain at Nyquist, flat at DC")
	{
		AudioEqBand b; b.type = AudioEqBandType::HighShelf; b.freqHz = 6000.0f; b.gainDb = 5.0f;
		const BiquadCoeffs c = biquadCoefficients(b, fs);
		CHECK(stable(c));
		CHECK(c.magnitudeDb(fs / 2.0, fs) == doctest::Approx(5.0).epsilon(1e-6));
		CHECK(std::abs(c.magnitudeDb(0.0, fs)) < 1e-9);
		CHECK(c.magnitudeDb(6000.0, fs) == doctest::Approx(2.5).epsilon(1e-6));
	}
	SUBCASE("low/high pass, Butterworth Q: unity in the pass band, −3 dB at f0")
	{
		AudioEqBand lp; lp.type = AudioEqBandType::LowPass; lp.freqHz = 1000.0f; lp.q = 0.70710678f;
		const BiquadCoeffs l = biquadCoefficients(lp, fs);
		CHECK(stable(l));
		CHECK(std::abs(l.magnitudeDb(0.0, fs)) < 1e-9);
		CHECK(l.magnitudeDb(1000.0, fs) == doctest::Approx(-3.0103).epsilon(1e-3));
		CHECK(l.magnitudeDb(10000.0, fs) < -35.0);

		AudioEqBand hp = lp; hp.type = AudioEqBandType::HighPass;
		const BiquadCoeffs h = biquadCoefficients(hp, fs);
		CHECK(stable(h));
		CHECK(std::abs(h.magnitudeDb(fs / 2.0, fs)) < 1e-9);
		CHECK(h.magnitudeDb(1000.0, fs) == doctest::Approx(-3.0103).epsilon(1e-3));
		CHECK(h.magnitudeDb(100.0, fs) < -35.0);
	}
}

TEST_CASE("biquadCoefficients: out-of-range input is clamped, never unstable")
{
	const double fs = 48000.0;
	AudioEqBand b; b.gainDb = 99.0f; b.q = 0.0f; b.freqHz = 1.0e6f;   // all three out of range
	const BiquadCoeffs c = biquadCoefficients(b, fs);
	CHECK(stable(c));
	// Gain clamps to +24, the frequency to kMaxFreqHz (below 0.49·fs at 48 kHz).
	CHECK(c.magnitudeDb(AudioEqBand::kMaxFreqHz, fs) == doctest::Approx(AudioEqBand::kMaxGainDb).epsilon(1e-4));

	// A 44.1 kHz filter clamps a 22 kHz band below ITS Nyquist, not 48 kHz's.
	AudioEqBand hi; hi.type = AudioEqBandType::LowPass; hi.freqHz = 22000.0f;
	CHECK(stable(biquadCoefficients(hi, 44100.0)));
	CHECK(stable(biquadCoefficients(hi, 8000.0)));

	AudioEqBand nan; nan.freqHz = std::nanf(""); nan.gainDb = 6.0f; nan.q = std::nanf("");
	const BiquadCoeffs n = biquadCoefficients(nan, fs);   // falls back to 1 kHz, Q 0.707
	CHECK(stable(n));
	CHECK(n.magnitudeDb(1000.0, fs) == doctest::Approx(6.0).epsilon(1e-4));
}

TEST_CASE("BiquadState: a filtered sine comes out at the level the response promises")
{
	// Runs the filter for real (what playback will do) and checks it against
	// magnitudeDb — the two must agree, or the EQ curve in the editor lies.
	const double fs = 48000.0, f = 1000.0;
	AudioEqBand b; b.freqHz = 1000.0f; b.gainDb = 6.0f; b.q = 1.4f;
	const BiquadCoeffs c = biquadCoefficients(b, fs);
	BiquadState st;
	double peakOut = 0.0;
	for (int i = 0; i < 48000; ++i)
	{
		const float x = static_cast<float>(0.25 * std::sin(2.0 * 3.14159265358979 * f * i / fs));
		const float y = st.process(c, x);
		if (i > 24000) peakOut = std::max(peakOut, static_cast<double>(std::abs(y)));   // settled
	}
	CHECK(20.0 * std::log10(peakOut / 0.25) == doctest::Approx(c.magnitudeDb(f, fs)).epsilon(1e-2));
	st.reset();
	CHECK(st.z1 == 0.0); CHECK(st.z2 == 0.0);
}

TEST_CASE("AudioEq: neutral detection and summed response")
{
	AudioEq eq;
	CHECK(eq.isNeutral());
	AudioEqBand a; a.freqHz = 500.0f;  a.gainDb = 4.0f;
	AudioEqBand b; b.freqHz = 4000.0f; b.gainDb = -2.0f;
	eq.bands = { a, b };
	CHECK_FALSE(eq.isNeutral());
	const double fs = 48000.0;
	const double sum = biquadCoefficients(a, fs).magnitudeDb(1500.0, fs)
	                 + biquadCoefficients(b, fs).magnitudeDb(1500.0, fs);
	CHECK(eq.responseDb(1500.0, fs) == doctest::Approx(sum));

	eq.enabled = false;                       // bypass: neutral, flat
	CHECK(eq.isNeutral());
	CHECK(eq.responseDb(500.0, fs) == 0.0);

	eq.enabled = true;
	eq.bands[0].gainDb = 0.0f; eq.bands[1].enabled = false;
	CHECK(eq.isNeutral());                    // nothing left that would change a sample
}

// ─── Bus ──────────────────────────────────────────────────────────────────────

TEST_CASE("AudioEdit::resolveBus: component, then asset, then master — a missing bus never wins")
{
	AudioBusConfig cfg;
	cfg.add("Music"); cfg.add("SFX");
	CHECK(AudioEdit::resolveBus("",      "",      cfg) == "");
	CHECK(AudioEdit::resolveBus("",      "Music", cfg) == "Music");
	CHECK(AudioEdit::resolveBus("SFX",   "Music", cfg) == "SFX");     // per-source choice wins
	CHECK(AudioEdit::resolveBus("Gone",  "Music", cfg) == "Music");   // deleted bus → next candidate
	CHECK(AudioEdit::resolveBus("",      "Gone",  cfg) == "");        // → master
	CHECK(AudioEdit::resolveBus("Music", "",      AudioBusConfig{}) == "");   // project without buses

	// The live-list form, as the runtime would ask the AudioEngine.
	auto onlyVoice = [](const std::string& n) { return n == "Voice"; };
	CHECK(AudioEdit::resolveBus("", "Voice", onlyVoice) == "Voice");
	CHECK(AudioEdit::resolveBus("", "Music", onlyVoice) == "");
	CHECK(AudioEdit::resolveBus("Music", "Voice", nullptr) == "");   // nothing to ask → master
}

// ─── Serialization ───────────────────────────────────────────────────────────

TEST_CASE("AudioEdit: JSON round trip, default detection")
{
	AudioEdit def;
	CHECK(def.isDefault());
	AudioEdit busOnly; busOnly.bus = "SFX";
	CHECK_FALSE(busOnly.isDefault());
	AudioEdit zeroBand; zeroBand.eq.bands.push_back({});   // placed but not yet dragged
	CHECK_FALSE(zeroBand.isDefault());

	const AudioEdit e = sampleEdit();
	AudioEdit back;
	REQUIRE(back.fromChunkText(e.toChunkText()));
	CHECK(sameEdit(e, back));
}

TEST_CASE("AudioEdit: missing keys read as defaults, junk is tolerated")
{
	AudioEdit e;
	REQUIRE(e.fromChunkText("{}"));
	CHECK(e.isDefault());

	// A file from a later version: an extra key, an unknown band type and
	// interpolation, out-of-range values, a point without a time, too many bands.
	nlohmann::json j = nlohmann::json::parse(R"({
		"version": 7, "futureThing": [1,2,3],
		"trim": { "start": -5, "end": 300 },
		"envelope": [ { "t": 2.0, "gain": 9.0, "interp": "bezier" },
		              { "gain": 0.5 },
		              { "t": 1.0, "gain": -1.0, "interp": "hold" } ],
		"bus": "Music",
		"eq": { "bands": [ { "type": "tilt", "freq": 5.0, "gainDb": 80, "q": 100 } ] }
	})");
	for (int i = 0; i < 12; ++i) j["eq"]["bands"].push_back({ { "type", "peak" } });
	REQUIRE(e.fromChunkText(j.dump()));

	CHECK(e.trim.startFrame == 0);              // negative → default, not a wrapped uint64
	CHECK(e.trim.endFrame   == 300);
	REQUIRE(e.envelope.points.size() == 2);     // the time-less point is dropped
	CHECK(e.envelope.points[0].timeSec == 1.0); // sorted
	CHECK(e.envelope.points[0].gain    == 0.0f);
	CHECK(e.envelope.points[0].interp  == AudioCurveInterp::Hold);
	CHECK(e.envelope.points[1].gain    == AudioEnvelope::kMaxGain);
	CHECK(e.envelope.points[1].interp  == AudioCurveInterp::Linear);
	CHECK(e.bus == "Music");
	REQUIRE(e.eq.bands.size() == AudioEq::kMaxBands);
	CHECK(e.eq.bands[0].type   == AudioEqBandType::Peak);
	CHECK(e.eq.bands[0].freqHz == AudioEqBand::kMinFreqHz);
	CHECK(e.eq.bands[0].gainDb == AudioEqBand::kMaxGainDb);
	CHECK(e.eq.bands[0].q      == AudioEqBand::kMaxQ);
	CHECK(e.eq.enabled);                        // key absent → on

	// Not JSON at all: false, and the default — the clip still plays.
	AudioEdit bad = sampleEdit();
	CHECK_FALSE(bad.fromChunkText("{ not json"));
	CHECK(bad.isDefault());
	CHECK_FALSE(bad.fromChunkText("[1,2]"));
}

TEST_CASE("AudioAsset: edits round-trip through the .hasset, the samples stay byte-identical")
{
	TempDir dir("he_test_audio_edit_rt");
	std::vector<uint8_t> pcm(4000);
	for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = static_cast<uint8_t>(i * 7);

	AudioAsset a;
	a.type = AssetType::Audio; a.name = "Wind"; a.path = "Audio/Wind.hasset";
	a.sampleRate = 48000; a.channels = 2; a.audioData = pcm;
	{
		ContentManager cm(dir.path.string());
		REQUIRE(cm.saveAsset(a));               // never edited
	}
	{
		HAsset::Reader r;
		REQUIRE(r.open((dir.path / a.path).string()));
		CHECK(r.findChunk(HAsset::CHUNK_AUED) == nullptr);   // no chunk for a default edit
		ContentManager cm(dir.path.string());
		const AudioAsset* loaded = cm.getAudio(cm.loadAsset(a.path));
		REQUIRE(loaded);
		CHECK(loaded->edit.isDefault());        // an old/unedited asset loads as "no edits"
	}

	a.edit = sampleEdit();
	{
		ContentManager cm(dir.path.string());
		REQUIRE(cm.saveAsset(a));
	}
	{
		HAsset::Reader r;
		REQUIRE(r.open((dir.path / a.path).string()));
		CHECK(r.findChunk(HAsset::CHUNK_AUED) != nullptr);
		const auto* data = r.findChunk(HAsset::CHUNK_PCMD);
		REQUIRE(data);
		CHECK(data->data == pcm);               // the edit never touches the samples

		ContentManager cm(dir.path.string());
		const AudioAsset* loaded = cm.getAudio(cm.loadAsset(a.path));
		REQUIRE(loaded);
		CHECK(sameEdit(loaded->edit, a.edit));
		CHECK(loaded->audioData == pcm);
	}
}

TEST_CASE("AudioImporter: a re-import replaces the samples and keeps the edits")
{
	TempDir root("he_test_audio_edit_reimport");
	TempDir src("he_test_audio_edit_src");
	const fs::path wav = src.path / "Rain.wav";
	REQUIRE(writeWav(wav, 2000, 22050, 0));

	REQUIRE(Importer::importSource(wav, root.path, "Audio"));
	const std::string rel = "Audio/Rain.hasset";
	REQUIRE(fs::exists(root.path / rel));

	// Edit it, as the Audio Editor will: load, change, save.
	const AudioEdit edit = sampleEdit();
	{
		ContentManager cm(root.path.string());
		const AudioAsset* loaded = cm.getAudio(cm.loadAsset(rel));
		REQUIRE(loaded);
		AudioAsset copy = *loaded;
		copy.edit = edit;
		REQUIRE(cm.saveAsset(copy));
	}

	// The source changes, the asset is re-imported onto itself.
	REQUIRE(writeWav(wav, 3000, 22050, 1000));
	REQUIRE(Importer::reimport(root.path / rel, root.path));

	ContentManager cm(root.path.string());
	const AudioAsset* again = cm.getAudio(cm.loadAsset(rel));
	REQUIRE(again);
	CHECK(again->audioData.size() == 3000 * 2);   // new samples …
	CHECK(sameEdit(again->edit, edit));           // … same edits
}
