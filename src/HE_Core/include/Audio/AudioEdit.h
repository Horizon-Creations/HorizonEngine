#pragma once
#include <Types/Defines.h>
#include <nlohmann/json_fwd.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace HE {

struct AudioBusConfig;

// ─── Non-destructive audio edits ──────────────────────────────────────────────
// What the Audio Editor lets you do to a clip WITHOUT touching its samples: a
// trim, a volume curve, the mixer bus it plays through and an EQ. All of it is
// a description applied on the way out; the PCMD/OGGD bytes the importer wrote
// are never rewritten, so every edit can be taken back and a re-import of the
// source lands under the same edits (AudioImporter carries them over, the way
// writeAsset carries the UUID).
//
// WHERE IT IS STORED: a JSON chunk (CHUNK_AUED) inside the audio .hasset, next
// to the data chunk — not a sidecar file next to the source. The source .wav/.ogg
// is an absolute path that may sit outside the content root (or on another
// machine), and a sidecar would need its own pack, rename, delete and git
// handling; a chunk travels with the asset through all of those for free (the
// pak copies an audio .hasset verbatim). AudioEdit::isDefault() decides whether
// the chunk is written at all, so a clip nobody edited stays byte-identical to
// what it was before this existed.
//
// OLDER ASSETS: no chunk → a default-constructed AudioEdit, which is exactly
// "play the whole clip at unity on master with no EQ" — what they always did.
// Every key inside the chunk is optional for the same reason, so a field added
// later reads as its default from a file written now. An older engine skips
// the unknown chunk id and plays the untrimmed clip.
//
// Why HE_Core: the same road as AudioBusConfig. The editor writes it, the
// importer preserves it, the runtime (HorizonScene) applies it — HE_Core is the
// one module all three see.

// ─── Trim ─────────────────────────────────────────────────────────────────────
// In FRAMES (one frame = one sample per channel — what the editor's playhead and
// waveform count in), half-open [startFrame, endFrame). endFrame == 0 means "to
// the end of the clip": the all-zero default is "untrimmed", and a Vorbis clip,
// whose length is only known once it is decoded, needs no number to say so.
struct HE_API AudioTrim
{
	uint64_t startFrame = 0;
	uint64_t endFrame   = 0;   // exclusive; 0 = through the last frame

	struct Range { uint64_t begin = 0, end = 0; };

	// The trim against a clip of `totalFrames`: both ends clamped into the clip.
	// A trim that leaves nothing (start at or past end — a hand-edited file, or a
	// clip re-imported shorter than the trim) resolves to the WHOLE clip, never
	// to silence: an asset that plays untrimmed is a visible mistake, one that
	// plays nothing is a bug report.
	Range resolve(uint64_t totalFrames) const;

	bool isDefault() const { return startFrame == 0 && endFrame == 0; }
};

// ─── Volume curve (envelope) ─────────────────────────────────────────────────
// How a segment gets from its point to the next one. The LEFT point owns the
// segment: its `interp` shapes the way to the following point.
enum class AudioCurveInterp : uint8_t
{
	Linear      = 0,   // straight line in amplitude
	Hold        = 1,   // keep this point's gain until the next point (a step)
	Smooth      = 2,   // smoothstep in amplitude — eases in and out, no kink
	Exponential = 3,   // straight line in dB — what a natural fade sounds like
};

struct HE_API AudioEnvelopePoint
{
	// Seconds from the start of the ORIGINAL clip, not of the trim: moving the
	// trim must not slide the curve along the audio it was drawn against.
	// Seconds rather than frames so a re-import at another sample rate keeps the
	// curve on the same moment of the sound.
	double           timeSec = 0.0;
	float            gain    = 1.0f;   // linear multiplier, 1 = unity (like every volume here)
	AudioCurveInterp interp  = AudioCurveInterp::Linear;
};

struct HE_API AudioEnvelope
{
	// Kept sorted by time (sort() / fromJson). Two points may share a time —
	// that is a jump, and the later one in the list wins from that time on.
	std::vector<AudioEnvelopePoint> points;

	// Highest gain a point may hold (+12 dB). Enough to lift a quiet take, low
	// enough that a typo does not clip the mix to pieces.
	static constexpr float kMaxGain = 4.0f;

	// Gain at `tSec` (same time base as the points). No points → 1 (an empty
	// curve is "no curve"); before the first point → the first point's gain,
	// after the last → the last one's.
	float evalGain(double tSec) const;

	void sort();
	bool empty() const { return points.empty(); }

	void toJson(nlohmann::json& out) const;    // a JSON array
	void fromJson(const nlohmann::json& in);   // anything but an array → empty
};

// ─── EQ ───────────────────────────────────────────────────────────────────────
// A small parametric EQ: a list of second-order (biquad) bands in series. One
// struct for both places an EQ can sit — an asset (here) and a mixer bus (to be
// added to AudioBusDef with the same JSON shape) — so the editor draws one EQ
// widget and the engine runs one filter.
enum class AudioEqBandType : uint8_t
{
	Peak      = 0,   // bell around freqHz: ±gainDb, width by q
	LowShelf  = 1,   // everything below freqHz by gainDb
	HighShelf = 2,   // everything above freqHz by gainDb
	LowPass   = 3,   // cut above freqHz (gainDb unused; q = resonance)
	HighPass  = 4,   // cut below freqHz (gainDb unused; q = resonance)
};

struct HE_API AudioEqBand
{
	AudioEqBandType type    = AudioEqBandType::Peak;
	float           freqHz  = 1000.0f;
	float           gainDb  = 0.0f;
	float           q       = 0.7071f;   // 1/√2: Butterworth for the passes, an octave-ish bell
	bool            enabled = true;

	// What biquadCoefficients clamps to. Frequencies above the filter's own
	// Nyquist are clamped there too (see biquadCoefficients).
	static constexpr float kMinFreqHz = 10.0f;
	static constexpr float kMaxFreqHz = 22000.0f;
	static constexpr float kMinGainDb = -24.0f;
	static constexpr float kMaxGainDb = 24.0f;
	static constexpr float kMinQ      = 0.1f;
	static constexpr float kMaxQ      = 24.0f;
};

// Normalised biquad (a0 = 1): y = b0·x + b1·x₋₁ + b2·x₋₂ − a1·y₋₁ − a2·y₋₂.
// Default-constructed = the identity (passes the signal through untouched).
struct HE_API BiquadCoeffs
{
	double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;

	bool isIdentity() const { return b0 == 1.0 && b1 == 0.0 && b2 == 0.0 && a1 == 0.0 && a2 == 0.0; }

	// |H(e^jω)| in dB at `freqHz` for a filter running at `sampleRate` — what
	// the EQ widget plots, and what the tests measure the formulas against.
	double magnitudeDb(double freqHz, double sampleRate) const;
};

// The RBJ "Audio EQ Cookbook" coefficients of one band, for a filter running at
// `sampleRate`. That rate is the one the samples have WHERE THE FILTER RUNS —
// the engine resamples every voice to its mixer rate (48 kHz), so the playback
// side passes that, not the asset's own rate. Out-of-range inputs are clamped
// to the kMin*/kMax* above (frequency also to 0.49 · sampleRate). A disabled
// band, a peak/shelf at 0 dB and a sampleRate ≤ 0 return the identity exactly,
// so the playback side can skip them without comparing floats itself.
HE_API BiquadCoeffs biquadCoefficients(const AudioEqBand& band, double sampleRate);

// One channel's filter memory (transposed direct form II — the form that stays
// well-behaved in floating point). A stereo voice keeps one per channel per band.
struct HE_API BiquadState
{
	double z1 = 0.0, z2 = 0.0;
	float process(const BiquadCoeffs& c, float x)
	{
		const double y = c.b0 * x + z1;
		z1 = c.b1 * x - c.a1 * y + z2;
		z2 = c.b2 * x - c.a2 * y;
		return static_cast<float>(y);
	}
	void reset() { z1 = z2 = 0.0; }
};

struct HE_API AudioEq
{
	bool                     enabled = true;   // the bypass switch; bands keep their settings
	std::vector<AudioEqBand> bands;            // in series, in this order

	// The widget offers no more than this; fromJson drops the rest.
	static constexpr size_t kMaxBands = 8;

	// True when running the EQ would change nothing: bypassed, or every band
	// is disabled / at 0 dB. The playback side skips the filter entirely then.
	bool isNeutral() const;

	// Summed response of every active band, in dB (0 when neutral).
	double responseDb(double freqHz, double sampleRate) const;

	void toJson(nlohmann::json& out) const;    // { "enabled": true, "bands": [ … ] }
	void fromJson(const nlohmann::json& in);
};

// ─── The whole edit ───────────────────────────────────────────────────────────
struct HE_API AudioEdit
{
	AudioTrim     trim;
	AudioEnvelope envelope;

	// The mixer bus this clip plays through, BY NAME — buses have no other
	// identity (AudioBusDef is a name and a volume; AudioSourceComponent::busName
	// is a string too). "" = master. A name the project no longer has falls back
	// to master (resolveBus), the same thing the engine has always done for a
	// source naming an unknown bus. Known gap: renaming a bus in the mixer
	// orphans this, exactly like it orphans a source's busName.
	std::string   bus;

	AudioEq       eq;

	// Nothing authored: no trim, no curve, master, no EQ bands. The writer leaves
	// the chunk out for a default edit.
	bool isDefault() const;

	// The curve's gain at `tSec` seconds into the ORIGINAL clip.
	float evalGain(double tSec) const { return envelope.evalGain(tSec); }

	// Which bus a voice of this clip plays through. Precedence: the component's
	// own busName (an explicit per-source choice) if the project has that bus,
	// else the asset's `bus` if the project has it, else "" — master. A name
	// that does not resolve is skipped rather than honoured, so a deleted bus
	// never silences a sound. `busExists` asks whatever knows the live list (the
	// AudioEngine at runtime); the second overload asks a project's config.
	static std::string resolveBus(const std::string& componentBus, const std::string& assetBus,
	                              const std::function<bool(const std::string&)>& busExists);
	static std::string resolveBus(const std::string& componentBus, const std::string& assetBus,
	                              const AudioBusConfig& config);

	// { "version": 1, "trim": {…}, "envelope": […], "bus": "…", "eq": {…} }.
	// Every key optional on read; unknown keys ignored.
	void toJson(nlohmann::json& out) const;
	void fromJson(const nlohmann::json& in);

	// The CHUNK_AUED payload (UTF-8 JSON text). fromChunkText resets to the
	// default first and returns false for text that is not a JSON object —
	// leaving the default, so a damaged chunk costs the edits, not the clip.
	std::string toChunkText() const;
	bool        fromChunkText(const std::string& text);
};

} // namespace HE
