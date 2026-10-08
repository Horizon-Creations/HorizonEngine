#include "Audio/AudioEdit.h"
#include "Audio/AudioBusConfig.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <complex>

namespace HE {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Floor of the dB-linear (Exponential) segment: a point at gain 0 is -inf dB,
// which no line can start from. -100 dB is inaudible and already below what
// int16 output can resolve (-96 dB), so the fade sounds the same.
constexpr double kExpFloorDb = -100.0;

double gainToDb(double g) { return g <= 1.0e-5 ? kExpFloorDb : 20.0 * std::log10(g); }
double dbToGain(double db) { return std::pow(10.0, db / 20.0); }

template <typename T>
T clampTo(T v, T lo, T hi) { return std::min(std::max(v, lo), hi); }

// Enum ↔ string. Strings rather than numbers in the file so it reads and
// hand-edits like the rest of the project's JSON; an unknown string (a type a
// newer editor added) falls back to the default instead of failing the load.
const char* interpName(AudioCurveInterp i)
{
	switch (i)
	{
	case AudioCurveInterp::Hold:        return "hold";
	case AudioCurveInterp::Smooth:      return "smooth";
	case AudioCurveInterp::Exponential: return "exponential";
	case AudioCurveInterp::Linear:      break;
	}
	return "linear";
}

AudioCurveInterp interpFromName(const std::string& s)
{
	if (s == "hold")        return AudioCurveInterp::Hold;
	if (s == "smooth")      return AudioCurveInterp::Smooth;
	if (s == "exponential") return AudioCurveInterp::Exponential;
	return AudioCurveInterp::Linear;
}

const char* bandTypeName(AudioEqBandType t)
{
	switch (t)
	{
	case AudioEqBandType::LowShelf:  return "lowShelf";
	case AudioEqBandType::HighShelf: return "highShelf";
	case AudioEqBandType::LowPass:   return "lowPass";
	case AudioEqBandType::HighPass:  return "highPass";
	case AudioEqBandType::Peak:      break;
	}
	return "peak";
}

AudioEqBandType bandTypeFromName(const std::string& s)
{
	if (s == "lowShelf")  return AudioEqBandType::LowShelf;
	if (s == "highShelf") return AudioEqBandType::HighShelf;
	if (s == "lowPass")   return AudioEqBandType::LowPass;
	if (s == "highPass")  return AudioEqBandType::HighPass;
	return AudioEqBandType::Peak;
}

float numberOr(const nlohmann::json& o, const char* key, float fallback)
{
	const auto it = o.find(key);
	return it != o.end() && it->is_number() ? it->get<float>() : fallback;
}

// A float that is NaN or infinite is not a setting anybody made; it reads as
// the fallback rather than poisoning a filter or the curve.
float finiteOr(float v, float fallback) { return std::isfinite(v) ? v : fallback; }

bool isGainBand(AudioEqBandType t)
{
	return t == AudioEqBandType::Peak || t == AudioEqBandType::LowShelf
	    || t == AudioEqBandType::HighShelf;
}

} // namespace

// ─── Trim ─────────────────────────────────────────────────────────────────────

AudioTrim::Range AudioTrim::resolve(uint64_t totalFrames) const
{
	Range r;
	r.begin = std::min(startFrame, totalFrames);
	r.end   = (endFrame == 0 || endFrame > totalFrames) ? totalFrames : endFrame;
	if (r.begin >= r.end)
		r = Range{ 0, totalFrames };
	return r;
}

AudioTrim AudioTrim::fromRange(uint64_t begin, uint64_t end, uint64_t totalFrames)
{
	end   = std::min(end, totalFrames);
	begin = std::min(begin, end);
	if (begin >= end || (begin == 0 && end == totalFrames)) return AudioTrim{};
	AudioTrim t;
	t.startFrame = begin;
	t.endFrame   = end == totalFrames ? 0 : end;
	return t;
}

// ─── Envelope ─────────────────────────────────────────────────────────────────

void AudioEnvelope::sort()
{
	// Stable: two points at the same time stay in authored order, which is what
	// decides which side of the jump is which.
	std::stable_sort(points.begin(), points.end(),
	                 [](const AudioEnvelopePoint& a, const AudioEnvelopePoint& b)
	                 { return a.timeSec < b.timeSec; });
}

float AudioEnvelope::evalGain(double tSec) const
{
	if (points.empty()) return 1.0f;

	// First point strictly after t: the segment is [i-1, i].
	const auto next = std::upper_bound(points.begin(), points.end(), tSec,
	                                   [](double t, const AudioEnvelopePoint& p) { return t < p.timeSec; });
	if (next == points.begin()) return points.front().gain;
	if (next == points.end())   return points.back().gain;

	const AudioEnvelopePoint& a = *(next - 1);
	const AudioEnvelopePoint& b = *next;
	const double span = b.timeSec - a.timeSec;   // > 0: a.time <= t < b.time
	const double f    = clampTo((tSec - a.timeSec) / span, 0.0, 1.0);

	switch (a.interp)
	{
	case AudioCurveInterp::Hold:
		return a.gain;
	case AudioCurveInterp::Smooth:
	{
		const double s = f * f * (3.0 - 2.0 * f);
		return static_cast<float>(a.gain + (b.gain - a.gain) * s);
	}
	case AudioCurveInterp::Exponential:
	{
		if (f <= 0.0) return a.gain;
		const double db = gainToDb(a.gain) + (gainToDb(b.gain) - gainToDb(a.gain)) * f;
		// Both ends silent: true silence rather than -100 dB. (A fade TO a 0
		// point reaches 0 because the point itself starts the next segment.)
		return db <= kExpFloorDb ? 0.0f : static_cast<float>(dbToGain(db));
	}
	case AudioCurveInterp::Linear:
		break;
	}
	return static_cast<float>(a.gain + (b.gain - a.gain) * f);
}

AudioEnvelope AudioEnvelope::slice(double t0Sec, double t1Sec) const
{
	AudioEnvelope out;
	if (points.empty()) return out;
	if (t1Sec < t0Sec) std::swap(t0Sec, t1Sec);

	// The interpolation of the segment that `t` falls in: its left point's, or
	// Linear before the first point (where the curve is flat anyway).
	auto interpAt = [&](double t)
	{
		const auto next = std::upper_bound(points.begin(), points.end(), t,
		                                   [](double x, const AudioEnvelopePoint& p) { return x < p.timeSec; });
		return next == points.begin() ? AudioCurveInterp::Linear : (next - 1)->interp;
	};

	bool startPinned = false, endPinned = false;
	for (const AudioEnvelopePoint& p : points)
	{
		if (p.timeSec < t0Sec || p.timeSec > t1Sec) continue;
		startPinned = startPinned || p.timeSec == t0Sec;
		endPinned   = endPinned   || p.timeSec == t1Sec;
		AudioEnvelopePoint q = p;
		q.timeSec = p.timeSec - t0Sec;
		out.points.push_back(q);
	}
	if (!startPinned)
		out.points.insert(out.points.begin(),
		                  AudioEnvelopePoint{ 0.0, evalGain(t0Sec), interpAt(t0Sec) });
	if (!endPinned)
		out.points.push_back(AudioEnvelopePoint{ t1Sec - t0Sec, evalGain(t1Sec), interpAt(t1Sec) });
	return out;
}

void AudioEnvelope::toJson(nlohmann::json& out) const
{
	out = nlohmann::json::array();
	for (const AudioEnvelopePoint& p : points)
		out.push_back({ { "t", p.timeSec }, { "gain", p.gain }, { "interp", interpName(p.interp) } });
}

void AudioEnvelope::fromJson(const nlohmann::json& in)
{
	points.clear();
	if (!in.is_array()) return;
	for (const nlohmann::json& j : in)
	{
		if (!j.is_object()) continue;
		const auto t = j.find("t");
		if (t == j.end() || !t->is_number()) continue;   // a point without a time is no point
		AudioEnvelopePoint p;
		p.timeSec = t->get<double>();
		if (!std::isfinite(p.timeSec)) continue;
		p.timeSec = std::max(0.0, p.timeSec);
		p.gain    = clampTo(finiteOr(numberOr(j, "gain", 1.0f), 1.0f), 0.0f, kMaxGain);
		const auto it = j.find("interp");
		if (it != j.end() && it->is_string()) p.interp = interpFromName(it->get<std::string>());
		points.push_back(p);
	}
	sort();
}

// ─── EQ ───────────────────────────────────────────────────────────────────────

double BiquadCoeffs::magnitudeDb(double freqHz, double sampleRate) const
{
	if (sampleRate <= 0.0) return 0.0;
	const double w = 2.0 * kPi * freqHz / sampleRate;
	const std::complex<double> z1 = std::polar(1.0, -w);       // e^{-jω}
	const std::complex<double> z2 = z1 * z1;
	const std::complex<double> h  = (b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2);
	const double mag = std::abs(h);
	return mag <= 1.0e-12 ? -240.0 : 20.0 * std::log10(mag);
}

BiquadCoeffs biquadCoefficients(const AudioEqBand& band, double sampleRate)
{
	if (!band.enabled || !(sampleRate > 0.0)) return {};

	const double gainDb = clampTo(static_cast<double>(finiteOr(band.gainDb, 0.0f)),
	                              static_cast<double>(AudioEqBand::kMinGainDb),
	                              static_cast<double>(AudioEqBand::kMaxGainDb));
	// A peak or shelf at 0 dB is the identity on paper; returning it exactly
	// (instead of b == a after rounding) lets the playback side skip the band.
	if (isGainBand(band.type) && std::abs(gainDb) < 1.0e-3) return {};

	const double nyquistCap = 0.49 * sampleRate;
	const double f0 = clampTo(static_cast<double>(finiteOr(band.freqHz, 1000.0f)),
	                          static_cast<double>(AudioEqBand::kMinFreqHz),
	                          std::min(static_cast<double>(AudioEqBand::kMaxFreqHz), nyquistCap));
	const double q  = clampTo(static_cast<double>(finiteOr(band.q, 0.7071f)),
	                          static_cast<double>(AudioEqBand::kMinQ),
	                          static_cast<double>(AudioEqBand::kMaxQ));

	const double w0    = 2.0 * kPi * f0 / sampleRate;
	const double cw    = std::cos(w0);
	const double sw    = std::sin(w0);
	const double alpha = sw / (2.0 * q);
	const double A     = std::pow(10.0, gainDb / 40.0);   // amplitude, sqrt of the linear gain

	double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
	switch (band.type)
	{
	case AudioEqBandType::Peak:
		b0 = 1.0 + alpha * A;  b1 = -2.0 * cw;  b2 = 1.0 - alpha * A;
		a0 = 1.0 + alpha / A;  a1 = -2.0 * cw;  a2 = 1.0 - alpha / A;
		break;
	case AudioEqBandType::LowShelf:
	{
		const double k = 2.0 * std::sqrt(A) * alpha;
		b0 =        A * ((A + 1.0) - (A - 1.0) * cw + k);
		b1 =  2.0 * A * ((A - 1.0) - (A + 1.0) * cw);
		b2 =        A * ((A + 1.0) - (A - 1.0) * cw - k);
		a0 =             (A + 1.0) + (A - 1.0) * cw + k;
		a1 =     -2.0 * ((A - 1.0) + (A + 1.0) * cw);
		a2 =             (A + 1.0) + (A - 1.0) * cw - k;
		break;
	}
	case AudioEqBandType::HighShelf:
	{
		const double k = 2.0 * std::sqrt(A) * alpha;
		b0 =        A * ((A + 1.0) + (A - 1.0) * cw + k);
		b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cw);
		b2 =        A * ((A + 1.0) + (A - 1.0) * cw - k);
		a0 =             (A + 1.0) - (A - 1.0) * cw + k;
		a1 =      2.0 * ((A - 1.0) - (A + 1.0) * cw);
		a2 =             (A + 1.0) - (A - 1.0) * cw - k;
		break;
	}
	case AudioEqBandType::LowPass:
		b0 = (1.0 - cw) * 0.5;  b1 = 1.0 - cw;     b2 = (1.0 - cw) * 0.5;
		a0 = 1.0 + alpha;       a1 = -2.0 * cw;    a2 = 1.0 - alpha;
		break;
	case AudioEqBandType::HighPass:
		b0 = (1.0 + cw) * 0.5;  b1 = -(1.0 + cw);  b2 = (1.0 + cw) * 0.5;
		a0 = 1.0 + alpha;       a1 = -2.0 * cw;    a2 = 1.0 - alpha;
		break;
	}

	BiquadCoeffs c;
	c.b0 = b0 / a0;  c.b1 = b1 / a0;  c.b2 = b2 / a0;
	c.a1 = a1 / a0;  c.a2 = a2 / a0;
	return c;
}

bool AudioEq::isNeutral() const
{
	if (!enabled) return true;
	for (const AudioEqBand& b : bands)
	{
		if (!b.enabled) continue;
		if (!isGainBand(b.type)) return false;   // a pass filter always does something
		if (std::abs(finiteOr(b.gainDb, 0.0f)) >= 1.0e-3f) return false;
	}
	return true;
}

double AudioEq::responseDb(double freqHz, double sampleRate) const
{
	if (!enabled) return 0.0;
	double db = 0.0;
	for (const AudioEqBand& b : bands)
	{
		const BiquadCoeffs c = biquadCoefficients(b, sampleRate);
		if (!c.isIdentity()) db += c.magnitudeDb(freqHz, sampleRate);
	}
	return db;
}

void AudioEq::toJson(nlohmann::json& out) const
{
	out = nlohmann::json::object();
	out["enabled"] = enabled;
	nlohmann::json arr = nlohmann::json::array();
	for (const AudioEqBand& b : bands)
		arr.push_back({ { "type",    bandTypeName(b.type) },
		                { "freq",    b.freqHz },
		                { "gainDb",  b.gainDb },
		                { "q",       b.q },
		                { "enabled", b.enabled } });
	out["bands"] = std::move(arr);
}

void AudioEq::fromJson(const nlohmann::json& in)
{
	*this = AudioEq{};
	if (!in.is_object()) return;
	if (const auto it = in.find("enabled"); it != in.end() && it->is_boolean())
		enabled = it->get<bool>();
	const auto arr = in.find("bands");
	if (arr == in.end() || !arr->is_array()) return;
	for (const nlohmann::json& j : *arr)
	{
		if (bands.size() >= kMaxBands) break;
		if (!j.is_object()) continue;
		AudioEqBand b;
		if (const auto t = j.find("type"); t != j.end() && t->is_string())
			b.type = bandTypeFromName(t->get<std::string>());
		// Stored as authored (within the documented range); biquadCoefficients
		// additionally clamps against the rate the filter actually runs at.
		b.freqHz = clampTo(finiteOr(numberOr(j, "freq", b.freqHz), b.freqHz),
		                   AudioEqBand::kMinFreqHz, AudioEqBand::kMaxFreqHz);
		b.gainDb = clampTo(finiteOr(numberOr(j, "gainDb", b.gainDb), 0.0f),
		                   AudioEqBand::kMinGainDb, AudioEqBand::kMaxGainDb);
		b.q      = clampTo(finiteOr(numberOr(j, "q", b.q), b.q),
		                   AudioEqBand::kMinQ, AudioEqBand::kMaxQ);
		if (const auto e = j.find("enabled"); e != j.end() && e->is_boolean())
			b.enabled = e->get<bool>();
		bands.push_back(b);
	}
}

// ─── The whole edit ───────────────────────────────────────────────────────────

bool AudioEdit::isDefault() const
{
	// eq.bands rather than eq.isNeutral(): a band somebody placed at 0 dB is
	// still authored (they will drag it next), so the chunk keeps it.
	return trim.isDefault() && envelope.empty() && bus.empty() && eq.bands.empty() && eq.enabled;
}

AudioEdit AudioEdit::forRange(double t0Sec, double t1Sec) const
{
	AudioEdit out;
	out.envelope = envelope.slice(t0Sec, t1Sec);
	out.bus      = bus;
	out.eq       = eq;
	return out;
}

std::string AudioEdit::resolveBus(const std::string& componentBus, const std::string& assetBus,
                                  const std::function<bool(const std::string&)>& busExists)
{
	if (!componentBus.empty() && busExists && busExists(componentBus)) return componentBus;
	if (!assetBus.empty()     && busExists && busExists(assetBus))     return assetBus;
	return {};
}

std::string AudioEdit::resolveBus(const std::string& componentBus, const std::string& assetBus,
                                  const AudioBusConfig& config)
{
	return resolveBus(componentBus, assetBus,
	                  [&config](const std::string& n) { return config.find(n) != nullptr; });
}

void AudioEdit::toJson(nlohmann::json& out) const
{
	out = nlohmann::json::object();
	out["version"] = 1;
	out["trim"]    = { { "start", trim.startFrame }, { "end", trim.endFrame } };
	envelope.toJson(out["envelope"]);
	out["bus"] = bus;
	eq.toJson(out["eq"]);
}

void AudioEdit::fromJson(const nlohmann::json& in)
{
	*this = AudioEdit{};
	if (!in.is_object()) return;

	if (const auto t = in.find("trim"); t != in.end() && t->is_object())
	{
		// Unsigned only: a negative frame from a hand-edited file reads as 0
		// (the default) instead of wrapping to an absurd uint64.
		if (const auto s = t->find("start"); s != t->end() && s->is_number_unsigned())
			trim.startFrame = s->get<uint64_t>();
		if (const auto e = t->find("end"); e != t->end() && e->is_number_unsigned())
			trim.endFrame = e->get<uint64_t>();
	}
	if (const auto e = in.find("envelope"); e != in.end())
		envelope.fromJson(*e);
	if (const auto b = in.find("bus"); b != in.end() && b->is_string())
		bus = b->get<std::string>();
	if (const auto q = in.find("eq"); q != in.end())
		eq.fromJson(*q);
}

std::string AudioEdit::toChunkText() const
{
	nlohmann::json j;
	toJson(j);
	return j.dump();
}

bool AudioEdit::fromChunkText(const std::string& text)
{
	*this = AudioEdit{};
	const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
	if (!j.is_object()) return false;   // includes the discarded value of a parse error
	fromJson(j);
	return true;
}

} // namespace HE
