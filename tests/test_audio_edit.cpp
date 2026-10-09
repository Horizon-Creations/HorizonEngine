#include "doctest.h"
#include "TestFsUtil.h"
#include <Audio/AudioEdit.h>
#include <Audio/AudioBusConfig.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/HAsset.h>
#include "ImporterCommon.h"   // importSource — the re-import keeps the edits
#include "AudioImporter.h"    // extractRange — Extract Selection
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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

// ─── Cutting (Thema 168, step 3): Trim to Selection and Extract ─────────────
// The Audio Editor's two cuts. Trim is an edit of the asset (AudioTrim, stored
// in CHUNK_AUED); Extract writes the selected frames as a NEW asset through
// AudioImporter. Both promise that the samples they were cut from never change.

namespace
{
	// 16-bit PCM WAV with `channels` channels; every sample is distinct from its
	// neighbours (frame and channel both move it), so a slice that is one frame
	// or one channel off compares unequal.
	bool writeWavN(const fs::path& file, int frames, int rate, int channels)
	{
		std::ofstream f(file, std::ios::binary | std::ios::trunc);
		if (!f) return false;
		auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
		auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
		const uint32_t blockAlign = static_cast<uint32_t>(channels) * 2;
		const uint32_t dataBytes  = static_cast<uint32_t>(frames) * blockAlign;
		f.write("RIFF", 4); u32(36 + dataBytes); f.write("WAVE", 4);
		f.write("fmt ", 4); u32(16); u16(1); u16(static_cast<uint16_t>(channels));
		u32(static_cast<uint32_t>(rate)); u32(static_cast<uint32_t>(rate) * blockAlign);
		u16(static_cast<uint16_t>(blockAlign)); u16(16);
		f.write("data", 4); u32(dataBytes);
		for (int i = 0; i < frames; ++i)
			for (int c = 0; c < channels; ++c)
				u16(static_cast<uint16_t>(static_cast<int16_t>(i * 7 + c * 3001 + 1)));
		return static_cast<bool>(f);
	}

	std::vector<uint8_t> fileBytes(const fs::path& file)
	{
		std::ifstream f(file, std::ios::binary);
		return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	}

	size_t filesIn(const fs::path& dir)
	{
		size_t n = 0;
		for (const auto& e : fs::directory_iterator(dir)) n += e.is_regular_file() ? 1 : 0;
		return n;
	}
}

TEST_CASE("AudioTrim::fromRange: stores no more than it must")
{
	const uint64_t total = 1000;

	// The whole clip, an empty range and a range past the end that clamps to the
	// whole clip are all "untrimmed" — trimming to everything leaves an
	// untouched asset untouched (no edit chunk).
	CHECK(AudioTrim::fromRange(0, total, total).isDefault());
	CHECK(AudioTrim::fromRange(0, total + 50, total).isDefault());
	CHECK(AudioTrim::fromRange(300, 300, total).isDefault());
	CHECK(AudioTrim::fromRange(700, 200, total).isDefault());   // backwards = empty

	// A head cut ends "at the end": 0, which survives a longer re-import.
	const AudioTrim head = AudioTrim::fromRange(250, total, total);
	CHECK(head.startFrame == 250);
	CHECK(head.endFrame   == 0);

	const AudioTrim mid = AudioTrim::fromRange(100, 200, total);
	CHECK(mid.startFrame == 100);
	CHECK(mid.endFrame   == 200);
	const AudioTrim::Range r = mid.resolve(total);
	CHECK(r.begin == 100);
	CHECK(r.end   == 200);

	const AudioTrim tail = AudioTrim::fromRange(0, 1, total);   // a single frame
	CHECK(tail.resolve(total).begin == 0);
	CHECK(tail.resolve(total).end   == 1);
}

TEST_CASE("AudioEnvelope::slice and AudioEdit::forRange: what an extract carries")
{
	SUBCASE("an empty curve stays empty")
	{
		CHECK(AudioEnvelope{}.slice(0.5, 1.5).empty());
	}
	SUBCASE("the cut-out range sounds as it did, from 0")
	{
		AudioEnvelope e;
		e.points = { { 0.0, 1.0f, AudioCurveInterp::Linear },
		             { 1.0, 0.5f, AudioCurveInterp::Exponential },
		             { 2.0, 0.125f, AudioCurveInterp::Hold },
		             { 3.0, 1.0f, AudioCurveInterp::Linear } };
		const AudioEnvelope s = e.slice(0.5, 2.5);
		// Boundary points at 0 and 2.0, the two inner points shifted by 0.5.
		REQUIRE(s.points.size() == 4);
		CHECK(s.points.front().timeSec == doctest::Approx(0.0));
		CHECK(s.points.back().timeSec  == doctest::Approx(2.0));
		CHECK(s.points[1].timeSec == doctest::Approx(0.5));
		CHECK(s.points[2].timeSec == doctest::Approx(1.5));
		CHECK(s.points.front().interp == AudioCurveInterp::Linear);   // the segment it cuts
		CHECK(s.points.back().interp  == AudioCurveInterp::Hold);
		// Linear, Exponential and Hold pieces are exact: the slice at t equals
		// the original at t + 0.5 everywhere in the range.
		for (double t = 0.0; t <= 2.0; t += 0.0625)
			CHECK(s.evalGain(t) == doctest::Approx(e.evalGain(t + 0.5)).epsilon(1e-5));
	}
	SUBCASE("points on the ends are kept, not doubled")
	{
		AudioEnvelope e;
		e.points = { { 1.0, 0.25f, AudioCurveInterp::Linear }, { 2.0, 1.0f, AudioCurveInterp::Linear } };
		const AudioEnvelope s = e.slice(1.0, 2.0);
		REQUIRE(s.points.size() == 2);
		CHECK(s.points[0].gain == doctest::Approx(0.25f));
		CHECK(s.points[1].timeSec == doctest::Approx(1.0));
	}
	SUBCASE("forRange: bus and EQ come along, the trim does not")
	{
		const AudioEdit src = sampleEdit();
		const AudioEdit out = src.forRange(0.25, 1.75);
		CHECK(out.trim.isDefault());
		CHECK(out.bus == src.bus);
		CHECK(out.eq.bands.size() == src.eq.bands.size());
		CHECK(out.eq.enabled == src.eq.enabled);
		CHECK(out.evalGain(1.0) == doctest::Approx(src.evalGain(1.25)).epsilon(1e-5));
	}
}

TEST_CASE("Trim saved through the ContentManager: only the edit chunk changes")
{
	TempDir root("he_test_audio_trim_save");
	TempDir src("he_test_audio_trim_src");
	const fs::path wav = src.path / "Wind.wav";
	REQUIRE(writeWavN(wav, 9000, 48000, 2));
	REQUIRE(Importer::importSource(wav, root.path, "Audio"));
	const fs::path file = root.path / "Audio/Wind.hasset";

	std::vector<uint8_t> pcmBefore;
	REQUIRE(Importer::readAssetChunk(file, HAsset::CHUNK_PCMD, pcmBefore));
	const std::string sourceBefore = Importer::sourceFileOf(file);
	CHECK(!sourceBefore.empty());

	// What the Audio Editor does: the loaded asset IS the edit buffer.
	{
		ContentManager cm(root.path.string());
		const UUID id = cm.loadAsset("Audio/Wind.hasset");
		AudioAsset* a = cm.getAudioMutable(id);
		REQUIRE(a);
		a->edit.trim = AudioTrim::fromRange(1234, 5678, audioPcmFrameCount(*a));
		REQUIRE(cm.saveAsset(*a));
	}

	std::vector<uint8_t> pcmAfter;
	REQUIRE(Importer::readAssetChunk(file, HAsset::CHUNK_PCMD, pcmAfter));
	const bool samplesKept = pcmAfter == pcmBefore;   // a bool: CHECK on the vectors prints megabytes
	CHECK(samplesKept);                               // the samples are not cut
	CHECK(Importer::sourceFileOf(file) == sourceBefore);   // Reimport still knows its source

	ContentManager cm(root.path.string());
	const AudioAsset* back = cm.getAudio(cm.loadAsset("Audio/Wind.hasset"));
	REQUIRE(back);
	CHECK(back->edit.trim.startFrame == 1234);
	CHECK(back->edit.trim.endFrame   == 5678);
	CHECK(audioPcmFrameCount(*back) == 9000);
}

TEST_CASE("AudioImporter::extractRange: sample-exact, a new asset, the original byte-identical")
{
	TempDir root("he_test_audio_extract");
	TempDir src("he_test_audio_extract_src");
	const int frames = 48001, rate = 48000, channels = 2;
	const fs::path wav = src.path / "Rain.wav";
	REQUIRE(writeWavN(wav, frames, rate, channels));
	REQUIRE(Importer::importSource(wav, root.path, "Audio"));
	const std::string origRel  = "Audio/Rain.hasset";
	const fs::path    origFile = root.path / origRel;

	// Give the original an edit, so there is something for the extract to carry.
	{
		ContentManager cm(root.path.string());
		AudioAsset* a = cm.getAudioMutable(cm.loadAsset(origRel));
		REQUIRE(a);
		a->edit = sampleEdit();
		REQUIRE(cm.saveAsset(*a));
	}
	const std::vector<uint8_t> origBefore = fileBytes(origFile);
	REQUIRE(!origBefore.empty());

	ContentManager cm(root.path.string());
	const UUID origId = cm.loadAsset(origRel);
	const AudioAsset* clip = cm.getAudio(origId);
	REQUIRE(clip);
	REQUIRE(audioPcmFrameCount(*clip) == static_cast<size_t>(frames));
	const size_t bpf = sizeof(int16_t) * channels;
	// Copied out before anything below runs: the pointer is the ContentManager's.
	const std::vector<uint8_t> origPcm  = clip->audioData;
	const AudioEdit            origEdit = clip->edit;

	auto loadExtract = [&](const std::string& rel, AudioAsset& out)
	{
		ContentManager fresh(root.path.string());
		const AudioAsset* a = fresh.getAudio(fresh.loadAsset(rel));
		if (!a) return false;
		out = *a;
		return true;
	};

	SUBCASE("an odd range up to the very last frame")
	{
		const uint64_t b = 1237, e = static_cast<uint64_t>(frames);
		AudioImporter::ExtractResult r;
		REQUIRE(AudioImporter::extractRange(*clip, b, e, root.path, "Audio", "Rain_extract",
		                                    origEdit.forRange(double(b) / rate, double(e) / rate), r));
		CHECK(r.path == "Audio/Rain_extract.hasset");
		CHECK(r.frames == e - b);
		CHECK(r.id != UUID{});
		CHECK(r.id != origId);

		AudioAsset x;
		REQUIRE(loadExtract(r.path, x));
		CHECK(x.id == r.id);
		CHECK(x.encoding == AudioEncoding::PCM16);
		CHECK(x.sampleRate == rate);
		CHECK(x.channels == channels);
		CHECK(audioPcmFrameCount(x) == e - b);   // the extracted length
		// Sample-exact: the bytes are the original's [b, e) and nothing else.
		const std::vector<uint8_t> expect(origPcm.begin() + std::ptrdiff_t(b * bpf),
		                                  origPcm.begin() + std::ptrdiff_t(e * bpf));
		const bool exact = x.audioData == expect;
		CHECK(exact);
		// Carried: bus and EQ; not carried: the trim, and the source — a Reimport
		// must not turn the extract back into the whole recording.
		CHECK(x.edit.bus == "Music");
		CHECK(x.edit.eq.bands.size() == origEdit.eq.bands.size());
		CHECK(x.edit.trim.isDefault());
		CHECK(Importer::sourceFileOf(root.path / r.path).empty());
	}
	SUBCASE("from frame 0, then a name collision, then the whole clip")
	{
		AudioImporter::ExtractResult r1, r2, r3;
		REQUIRE(AudioImporter::extractRange(*clip, 0, 1, root.path, "Audio", "Rain_extract", {}, r1));
		REQUIRE(AudioImporter::extractRange(*clip, 0, 1, root.path, "Audio", "Rain_extract", {}, r2));
		REQUIRE(AudioImporter::extractRange(*clip, 0, frames, root.path, "Audio", "Rain_extract", {}, r3));
		CHECK(r1.path == "Audio/Rain_extract.hasset");
		CHECK(r2.path == "Audio/Rain_extract_2.hasset");   // never over an existing file
		CHECK(r3.path == "Audio/Rain_extract_3.hasset");
		CHECK(r1.id != r2.id);

		AudioAsset one, all;
		REQUIRE(loadExtract(r2.path, one));
		REQUIRE(audioPcmFrameCount(one) == 1);
		CHECK(std::equal(one.audioData.begin(), one.audioData.end(), origPcm.begin()));
		REQUIRE(loadExtract(r3.path, all));
		const bool whole = all.audioData == origPcm;
		CHECK(whole);
	}
	SUBCASE("nothing is written for a range it cannot cut")
	{
		const size_t before = filesIn(root.path / "Audio");
		AudioImporter::ExtractResult r;
		CHECK_FALSE(AudioImporter::extractRange(*clip, 500, 500, root.path, "Audio", "Rain_extract", {}, r));
		CHECK_FALSE(AudioImporter::extractRange(*clip, 600, 500, root.path, "Audio", "Rain_extract", {}, r));
		CHECK_FALSE(AudioImporter::extractRange(*clip, 0, frames + 1, root.path, "Audio", "Rain_extract", {}, r));
		AudioAsset vorbis = *clip;
		vorbis.encoding = AudioEncoding::Vorbis;   // a compressed stream cannot be cut here
		CHECK_FALSE(AudioImporter::extractRange(vorbis, 0, 10, root.path, "Audio", "Rain_extract", {}, r));
		CHECK(filesIn(root.path / "Audio") == before);
		CHECK(r.path.empty());
	}

	// Whatever was extracted, the clip it was cut from is the same file, byte
	// for byte — not merely the same samples.
	const bool untouched = fileBytes(origFile) == origBefore;
	CHECK(untouched);
}

// ─── The curve applied to samples (Thema 168, step 4) ────────────────────────
// AudioEnvelope::rampedGain / apply / applyPcm16: the one gain function behind
// the engine's voices, the editor's preview and Extract's "bake the curve in".

namespace
{
	// Every interpolation, with points on the 128-frame grid at 48 kHz
	// (t·48000 a multiple of 128: 0, 1, 2, 3, 3.2 s) and a jump at 3.2 s.
	AudioEnvelope gridCurve()
	{
		AudioEnvelope e;
		e.points = { { 0.0, 0.0f,  AudioCurveInterp::Linear },
		             { 1.0, 1.0f,  AudioCurveInterp::Smooth },
		             { 2.0, 0.25f, AudioCurveInterp::Exponential },
		             { 3.0, 2.0f,  AudioCurveInterp::Hold },
		             { 3.2, 0.5f,  AudioCurveInterp::Linear } };
		return e;
	}
}

TEST_CASE("AudioEnvelope::rampedGain: the curve's value at every point, unity without a curve")
{
	const double   rate = 48000.0;
	const AudioEnvelope e = gridCurve();
	// "Kurvenwert an den Punkten": on a point the gain IS the point's gain —
	// through evalGain and through the playback function, for every interpolation.
	for (const AudioEnvelopePoint& p : e.points)
	{
		const uint64_t f = static_cast<uint64_t>(std::llround(p.timeSec * rate));
		REQUIRE(f % AudioEnvelope::kRampFrames == 0);
		CHECK(e.evalGain(p.timeSec) == doctest::Approx(p.gain).epsilon(1e-6));
		CHECK(e.rampedGain(f, rate) == doctest::Approx(p.gain).epsilon(1e-6));
	}
	// Inside a Linear segment the ramp IS the line, at any frame.
	for (uint64_t f : { 1ull, 127ull, 129ull, 12345ull, 47999ull })
		CHECK(e.rampedGain(f, rate) == doctest::Approx(e.evalGain(double(f) / rate)).epsilon(1e-6));
	// Before the first point and after the last the ends hold.
	CHECK(e.rampedGain(10'000'000, rate) == doctest::Approx(0.5f));

	// No curve: exactly 1, whatever the frame.
	const AudioEnvelope none;
	CHECK(none.rampedGain(0, rate) == 1.0f);
	CHECK(none.rampedGain(123456, rate) == 1.0f);
	CHECK(e.rampedGain(500, 0.0) == 1.0f);   // no rate, no curve
}

TEST_CASE("AudioEnvelope::rampedGain: a step becomes a ramp — no frame jumps more than one block's share")
{
	// The click guard. A Hold step 2.0 -> 0.5 at 3.2 s, and a 0 -> 4 jump
	// between two points at one time: as samples, neither may move by more
	// than (jump / kRampFrames) from one frame to the next.
	const double rate = 48000.0;
	AudioEnvelope e = gridCurve();
	e.points.push_back({ 4.0, 0.0f, AudioCurveInterp::Linear });
	e.points.push_back({ 4.0, AudioEnvelope::kMaxGain, AudioCurveInterp::Linear });   // a jump
	e.sort();

	const auto worstStep = [&](uint64_t f0, uint64_t f1)
	{
		double worst = 0.0;
		for (uint64_t f = f0; f + 1 < f1; ++f)
			worst = std::max(worst, std::fabs(double(e.rampedGain(f + 1, rate)) - double(e.rampedGain(f, rate))));
		return worst;
	};
	const double R = double(AudioEnvelope::kRampFrames);
	CHECK(worstStep(153600 - 300, 153600 + 300) <= 1.5 / R + 1e-6);   // the Hold step at 3.2 s
	CHECK(worstStep(192000 - 300, 192000 + 300) <= 4.0 / R + 1e-6);   // the jump at 4 s
	// … and the ramp really gets there: one block on, the new gain holds exactly.
	CHECK(e.rampedGain(153600, rate) == doctest::Approx(0.5f));
	CHECK(e.rampedGain(192000, rate) == doctest::Approx(AudioEnvelope::kMaxGain));
	// Negative control: evalGain itself — the curve as authored — DOES jump.
	CHECK(std::fabs(e.evalGain(192000.0 / rate) - e.evalGain(191999.0 / rate)) > 3.9);
}

TEST_CASE("AudioEnvelope::apply: the same samples however the reads are sliced, nothing touched without a curve")
{
	const double rate = 48000.0;
	const int    ch   = 2;
	const AudioEnvelope e = gridCurve();
	const uint64_t first = 47000, frames = 3000;   // straddles the point at 1 s
	std::vector<float> src(size_t(frames) * ch);
	for (size_t i = 0; i < src.size(); ++i) src[i] = std::sin(double(i) * 0.01) * 0.8f;

	std::vector<float> whole = src;
	e.apply(whole.data(), frames, ch, first, rate);
	for (uint64_t f = 0; f < frames; ++f)
		for (int c = 0; c < ch; ++c)
			REQUIRE(whole[size_t(f) * ch + c] == src[size_t(f) * ch + c] * e.rampedGain(first + f, rate));

	// The mixer reads in whatever pieces it likes; the result may not care.
	std::vector<float> pieces = src;
	uint64_t done = 0;
	for (uint64_t n : { 1ull, 7ull, 128ull, 129ull, 333ull, 1000ull })
	{
		const uint64_t k = std::min(n, frames - done);
		e.apply(pieces.data() + size_t(done) * ch, k, ch, first + done, rate);
		done += k;
	}
	e.apply(pieces.data() + size_t(done) * ch, frames - done, ch, first + done, rate);
	const bool same = pieces == whole;
	CHECK(same);

	// No curve: not even multiplied by one.
	std::vector<float> untouched = src;
	AudioEnvelope{}.apply(untouched.data(), frames, ch, first, rate);
	const bool unchanged = untouched == src;
	CHECK(unchanged);
}

TEST_CASE("AudioEnvelope::applyPcm16: rounded, clamped at full scale, and the clamps counted")
{
	AudioEnvelope up;
	up.points = { { 0.0, AudioEnvelope::kMaxGain, AudioCurveInterp::Linear } };   // +12 dB flat
	std::vector<int16_t> s = { 1000, -1000, 9000, -9000, 32767, -32768 };
	CHECK(up.applyPcm16(s.data(), 3, 2, 0, 48000.0) == 4);
	CHECK(s[0] == 4000);
	CHECK(s[1] == -4000);
	CHECK(s[2] == 32767);
	CHECK(s[3] == -32768);
	CHECK(s[4] == 32767);
	CHECK(s[5] == -32768);

	AudioEnvelope half;
	half.points = { { 0.0, 0.5f, AudioCurveInterp::Linear } };
	std::vector<int16_t> h = { 3, -3, 101 , 0 };
	CHECK(half.applyPcm16(h.data(), 2, 2, 0, 48000.0) == 0);
	CHECK(h[0] == 2);    // 1.5 rounds away from zero
	CHECK(h[1] == -2);
	CHECK(h[2] == 51);
}

TEST_CASE("AudioImporter::extractRange: baking the curve multiplies it into the samples")
{
	TempDir root("he_test_audio_bake");
	fs::create_directories(root.path / "Audio");
	const int rate = 48000, channels = 2;
	const uint64_t frames = 96000;
	AudioAsset clip;
	clip.type       = AssetType::Audio;
	clip.name       = "Wind";
	clip.sampleRate = rate;
	clip.channels   = channels;
	clip.encoding   = AudioEncoding::PCM16;
	clip.audioData.resize(size_t(frames) * channels * sizeof(int16_t));
	auto* pcm = reinterpret_cast<int16_t*>(clip.audioData.data());
	for (size_t i = 0; i < size_t(frames) * channels; ++i)
		pcm[i] = static_cast<int16_t>((int(i % 2000) - 1000) * 12);
	clip.edit.envelope.points = { { 0.25, 1.0f, AudioCurveInterp::Linear },
	                              { 0.75, 0.0f, AudioCurveInterp::Smooth },
	                              { 1.5,  2.0f, AudioCurveInterp::Linear } };

	const uint64_t b = 6001, e = 80013;   // off the grid on purpose
	const AudioEdit carried = clip.edit.forRange(double(b) / rate, double(e) / rate);

	AudioImporter::ExtractResult baked, plain;
	REQUIRE(AudioImporter::extractRange(clip, b, e, root.path, "Audio", "Wind_extract", carried, baked,
	                                    &clip.edit.envelope));
	REQUIRE(AudioImporter::extractRange(clip, b, e, root.path, "Audio", "Wind_extract", carried, plain));

	// Copies: a second loadAsset may move the first asset (ContentManager.h).
	ContentManager cm(root.path.string());
	const UUID xId = cm.loadAsset(baked.path);
	const UUID yId = cm.loadAsset(plain.path);
	REQUIRE(cm.getAudio(xId));
	REQUIRE(cm.getAudio(yId));
	const AudioAsset xa = *cm.getAudio(xId), ya = *cm.getAudio(yId);
	const AudioAsset* x = &xa;
	const AudioAsset* y = &ya;
	REQUIRE(audioPcmFrameCount(*x) == e - b);

	// Baked: every sample is the original's at the same ORIGINAL frame, times
	// the gain a voice of the original applies there.
	const auto* xs = reinterpret_cast<const int16_t*>(x->audioData.data());
	size_t wrong = 0;
	for (uint64_t f = 0; f < e - b; ++f)
		for (int c = 0; c < channels; ++c)
		{
			const double want = double(pcm[size_t(b + f) * channels + c]) *
			                    double(clip.edit.envelope.rampedGain(b + f, rate));
			const long   w    = std::clamp(std::lround(want), -32768L, 32767L);
			if (xs[size_t(f) * channels + c] != w) ++wrong;
		}
	CHECK(wrong == 0);
	CHECK(x->edit.envelope.empty());   // the curve is in the samples now, not on top
	CHECK(baked.clampedSamples == 0);  // 2x of ±12000 stays inside full scale

	// Not baked: the samples untouched, the curve rides along as an edit.
	const std::vector<uint8_t> expect(clip.audioData.begin() + std::ptrdiff_t(b * channels * 2),
	                                  clip.audioData.begin() + std::ptrdiff_t(e * channels * 2));
	const bool copied = y->audioData == expect;
	CHECK(copied);
	CHECK(y->edit.envelope.points.size() == carried.envelope.points.size());
}
