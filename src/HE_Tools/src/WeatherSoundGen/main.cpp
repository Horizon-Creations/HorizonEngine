// weather_sound_gen — generates the engine's WEATHER SOUNDS as loose .hasset files
// (Audio), written via ContentManager::saveAsset so the byte layout is identical
// to an imported clip.
//
// Usage:  weather_sound_gen <output-dir>
//   <output-dir> is the folder the .hasset files are written into (e.g.
//   EditorDeps/EngineContent/Audio/Weather). It is used verbatim as the
//   ContentManager content root, and each clip is saved under "<Name>.hasset".
//
// The bargain mesh_gen, widget_gen, matfn_gen, mat_gen and landscape_tex_gen make:
// the output is deterministic (fixed seeds, a fixed UUID per clip), the results are
// COMMITTED, and this is not part of the normal build graph. WeatherAudio
// (HE_Scene) plays them whenever a WeatherComponent leaves a sound slot empty, so
// every project hears its weather without a file of its own.
//
// ── Where the sound comes from ───────────────────────────────────────────────
// Every sample is computed here, from a seeded noise source and a few filters. No
// recording, no library, no third-party material — so there is no licence to
// carry: the clips are Horizon Creations' own work under the terms of the engine
// (EditorDeps/EngineContent/Audio/LICENSE-WeatherSounds.txt says so next to them).
//
// ── Why PCM16 and not Ogg Vorbis ─────────────────────────────────────────────
// A Vorbis clip would be ten times smaller, but the tree has no Vorbis encoder
// (stb_vorbis only decodes, AudioImporter says as much) and a generator that needs
// an external tool is a generator nobody can re-run. So the clips are small PCM16
// instead: mono, and each at the lowest rate that keeps what it is made of (wind
// and snow live below 5 kHz, rain and storm hiss does not). The mixer resamples on
// the voice, so a clip's own rate costs nothing at play time. About 1.1 MB in all.
//
// ── Seamless loops by construction, not by trimming ──────────────────────────
// The four beds are played looping, so their last sample must run into their first
// without a click. Nothing is cross-faded or cut: every filter is run over the loop
// THREE times in a row and only the last pass is kept (circular()), so the filter
// state entering sample 0 is exactly the state that left sample n-1, and every
// slow modulation has a whole number of cycles per loop. Wrapping is part of the
// signal, and the seam is just another pair of neighbouring samples — which the
// test (tests/test_engine_weather_sounds.cpp) checks on the committed files.
//
// ── Floating point ───────────────────────────────────────────────────────────
// The maths is double precision through the C++ library's sin/exp/tanh, rounded to
// int16 at the end. Re-running on the same platform gives the same bytes; another
// libm may differ by one LSB in a rare sample. The committed files were made on
// macOS, and nothing compares them byte for byte against a fresh run.

#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <ContentManager/DefaultAssets.h>
#include <Types/UUID.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
constexpr double kPi = 3.14159265358979323846;

// Well-known UUID base for the weather sounds. Same rules as mesh_gen's 0x100,
// widget_gen's 0x200, matfn_gen's 0x300 and mat_gen / landscape_tex_gen's 0x400:
// hi far below the version-4 bit pattern UUID::generate() enforces and clear of
// the DefaultAssets sentinels. Append only; the index IS the UUID.
constexpr uint64_t kSndBaseHi = 0x0000000000000500ULL;
static_assert(HE::kEngineWeatherRainSoundId.hi    == kSndBaseHi + 0 && HE::kEngineWeatherRainSoundId.lo    == 1,
              "Rain is entry 0 of the 0x500 block — DefaultAssets.h must agree");
static_assert(HE::kEngineWeatherWindSoundId.hi    == kSndBaseHi + 1 && HE::kEngineWeatherWindSoundId.lo    == 1,
              "Wind is entry 1 of the 0x500 block — DefaultAssets.h must agree");
static_assert(HE::kEngineWeatherSnowSoundId.hi    == kSndBaseHi + 2 && HE::kEngineWeatherSnowSoundId.lo    == 1,
              "Snow is entry 2 of the 0x500 block — DefaultAssets.h must agree");
static_assert(HE::kEngineWeatherStormSoundId.hi   == kSndBaseHi + 3 && HE::kEngineWeatherStormSoundId.lo   == 1,
              "Storm is entry 3 of the 0x500 block — DefaultAssets.h must agree");
static_assert(HE::kEngineWeatherThunderSoundId.hi == kSndBaseHi + 4 && HE::kEngineWeatherThunderSoundId.lo == 1,
              "Thunder is entry 4 of the 0x500 block — DefaultAssets.h must agree");

// ── Noise and filters ───────────────────────────────────────────────────────

// PCG32 (O'Neill): a small generator whose sequence is the same on every compiler
// and standard library, unlike std::uniform_*_distribution.
struct Pcg
{
    uint64_t state = 0;
    uint64_t inc   = 0;
    explicit Pcg(uint64_t seed)
    {
        inc = (54u << 1u) | 1u;
        next();
        state += seed;
        next();
    }
    uint32_t next()
    {
        const uint64_t old = state;
        state = old * 6364136223846793005ULL + inc;
        const uint32_t xorshifted = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
        const uint32_t rot        = static_cast<uint32_t>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31u));
    }
    double uniform() { return static_cast<double>(next() >> 8) * (1.0 / 16777216.0); }   // [0, 1)
    double white()   { return uniform() * 2.0 - 1.0; }                                    // [-1, 1)
};

struct OnePole
{
    double a = 0.0, y = 0.0;
    OnePole(double cutoffHz, double rate) : a(1.0 - std::exp(-2.0 * kPi * cutoffHz / rate)) {}
    void   setCutoff(double cutoffHz, double rate) { a = 1.0 - std::exp(-2.0 * kPi * cutoffHz / rate); }
    double lp(double x) { y += a * (x - y); return y; }
    double hp(double x) { return x - lp(x); }
};

// Zavalishin's topology-preserving state-variable filter: low, band and high pass
// of the same input. Unlike the Chamberlin form it stays stable while the centre
// frequency moves every sample (the Chamberlin one blew up on the storm's sweep,
// one loud burst in nine seconds of near silence). `damping` is 1/Q; the band
// output peaks at 1/damping.
struct Svf
{
    double ic1 = 0.0, ic2 = 0.0;
    struct Out { double low, band, high; };
    Out step(double x, double g, double damping)
    {
        const double a1 = 1.0 / (1.0 + g * (g + damping));
        const double a2 = g * a1;
        const double a3 = g * a2;
        const double v3 = x - ic2;
        const double v1 = a1 * ic1 + a2 * v3;
        const double v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0 * v1 - ic1;
        ic2 = 2.0 * v2 - ic2;
        return { v2, v1, x - damping * v1 - v2 };
    }
};
// The filter's frequency coefficient for a centre frequency, kept between 40 Hz and a
// fifth of the rate. The floor is not cosmetic: a sweep that dipped below zero made
// tan() negative and the filter blow up by a factor of a million for half a second.
double svfG(double centreHz, double rate) { return std::tan(kPi * std::clamp(centreHz, 40.0, rate * 0.2) / rate); }

// Slow modulation with a whole number of cycles per loop: it cannot break the seam.
double lfo(size_t i, size_t n, int cycles, double phase)
{
    return std::sin(2.0 * kPi * static_cast<double>(cycles) * static_cast<double>(i) / static_cast<double>(n) + phase);
}

// Run `step(i)` over the loop three times and keep the last pass: the filters it
// owns enter the kept pass already in the state sample n-1 leaves them in.
template <class Step>
std::vector<double> circular(size_t n, Step&& step)
{
    std::vector<double> out(n, 0.0);
    for (int pass = 0; pass < 3; ++pass)
        for (size_t i = 0; i < n; ++i)
        {
            const double v = step(i);
            if (pass == 2) out[i] = v;
        }
    return out;
}

std::vector<double> whiteNoise(size_t n, Pcg& rng)
{
    std::vector<double> x(n);
    for (double& v : x) v = rng.white();
    return x;
}

// A scatter of short noise ticks, wrapping around the loop's end — the sound of
// individual drops on a roof or the leaves. `perSecond` ticks, each a burst of
// noise that dies in a few milliseconds; most are soft, a few are loud.
std::vector<double> drops(size_t n, double rate, double perSecond, double tauMinMs, double tauMaxMs, Pcg& rng)
{
    std::vector<double> d(n, 0.0);
    const size_t count = static_cast<size_t>(perSecond * static_cast<double>(n) / rate);
    for (size_t k = 0; k < count; ++k)
    {
        const size_t at  = rng.next() % n;
        const double u   = rng.uniform();
        const double amp = 0.08 + 0.92 * u * u * u;
        const double tau = (tauMinMs + (tauMaxMs - tauMinMs) * rng.uniform()) * 1e-3 * rate;
        const size_t len = static_cast<size_t>(tau * 6.0) + 1;
        for (size_t j = 0; j < len; ++j)
            d[(at + j) % n] += amp * std::exp(-static_cast<double>(j) / tau) * rng.white();
    }
    return d;
}

// Take out the DC, scale to the RMS the clip is meant to have, then pull the whole
// thing down if a peak would pass `ceiling`. Weather sounds are ambience: their
// loudness is set by RMS, so two beds mixed together sit at comparable levels.
void finish(std::vector<double>& x, double targetRms, double ceiling)
{
    double mean = 0.0;
    for (double v : x) mean += v;
    mean /= static_cast<double>(x.size());
    double sq = 0.0;
    for (double& v : x) { v -= mean; sq += v * v; }
    const double rms = std::sqrt(sq / static_cast<double>(x.size()));
    const double k   = rms > 0.0 ? targetRms / rms : 0.0;
    double peak = 0.0;
    for (double& v : x) { v *= k; peak = std::max(peak, std::fabs(v)); }
    if (peak > ceiling)
        for (double& v : x) v *= ceiling / peak;
}

// ── The five sounds ─────────────────────────────────────────────────────────

// Rain: a broad hiss (the sound of thousands of drops too small to tell apart)
// with individual drops on top. A slow, shallow swell keeps it from sounding like
// a test tone.
std::vector<double> makeRain(double rate, double seconds)
{
    const size_t n = static_cast<size_t>(rate * seconds);
    Pcg rng(0x5241494E);   // "RAIN"
    const std::vector<double> a = whiteNoise(n, rng);
    const std::vector<double> b = drops(n, rate, 520.0, 1.2, 4.5, rng);

    OnePole hissLow(9000.0, rate), hissHigh(450.0, rate), dropHigh(1200.0, rate);
    Svf     presence;
    const double g = svfG(3400.0, rate);
    std::vector<double> x = circular(n, [&](size_t i) {
        const double hiss = hissHigh.hp(hissLow.lp(a[i]));
        const Svf::Out p  = presence.step(a[i], g, 1.4);
        const double tick = dropHigh.hp(b[i]);
        const double swell = 1.0 + 0.07 * lfo(i, n, 3, 0.4) + 0.04 * lfo(i, n, 7, 2.1);
        return (0.50 * hiss + 0.55 * p.band + 0.70 * tick) * swell;
    });
    finish(x, 0.16, 0.95);
    return x;
}

// Wind: a low rumble and a resonance that wanders up and down, with a thinner
// whistle an octave and a bit above it. The gusts are slow swells; WeatherAudio
// adds its own on top, so the clip itself stays fairly even.
std::vector<double> makeWind(double rate, double seconds)
{
    const size_t n = static_cast<size_t>(rate * seconds);
    Pcg rng(0x57494E44);   // "WIND"
    const std::vector<double> a = whiteNoise(n, rng);
    const std::vector<double> c = whiteNoise(n, rng);

    OnePole rumbleA(140.0, rate), rumbleB(140.0, rate);
    Svf     body, whistle;
    std::vector<double> x = circular(n, [&](size_t i) {
        const double sweep = 480.0 + 200.0 * lfo(i, n, 2, 0.3) + 90.0 * lfo(i, n, 5, 1.7) + 50.0 * lfo(i, n, 11, 4.0);   // 140..820 Hz
        const Svf::Out lo  = body.step(a[i], svfG(sweep, rate), 0.50);
        const Svf::Out hi  = whistle.step(c[i], svfG(sweep * 2.35, rate), 0.30);
        const double rum   = rumbleB.lp(rumbleA.lp(a[i]));
        const double gust  = 0.78 + 0.16 * lfo(i, n, 3, 0.9) + 0.06 * lfo(i, n, 8, 2.6);
        return (2.4 * rum + 0.85 * lo.band + 0.32 * hi.band) * gust;
    });
    finish(x, 0.14, 0.95);
    return x;
}

// Snow: the hush of a muffled landscape. Soft low noise, a faint airy shimmer, a
// barely there drone, and now and then a tiny glint. Quiet by design.
std::vector<double> makeSnow(double rate, double seconds)
{
    const size_t n = static_cast<size_t>(rate * seconds);
    Pcg rng(0x534E4F57);   // "SNOW"
    const std::vector<double> a = whiteNoise(n, rng);
    const std::vector<double> c = whiteNoise(n, rng);

    // Glints: a decaying sine every so often, wrapping at the loop's end.
    std::vector<double> glint(n, 0.0);
    const size_t glints = static_cast<size_t>(seconds * 4.0);
    for (size_t k = 0; k < glints; ++k)
    {
        const size_t at   = rng.next() % n;
        const double freq = 2600.0 + 1700.0 * rng.uniform();
        const double amp  = 0.25 + 0.75 * rng.uniform();
        const double tau  = (0.020 + 0.030 * rng.uniform()) * rate;
        const size_t len  = static_cast<size_t>(tau * 6.0) + 1;
        for (size_t j = 0; j < len; ++j)
            glint[(at + j) % n] += amp * std::exp(-static_cast<double>(j) / tau)
                                 * std::sin(2.0 * kPi * freq * static_cast<double>(j) / rate);
    }

    // Drone pitches with a whole number of cycles per loop, so the sines meet at the seam.
    const double f1 = std::round(98.0  * seconds) / seconds;
    const double f2 = std::round(147.0 * seconds) / seconds;

    OnePole hushA(1700.0, rate), hushB(1700.0, rate), hushHigh(110.0, rate);
    Svf     air;
    const double gAir = svfG(3600.0, rate);
    std::vector<double> x = circular(n, [&](size_t i) {
        const double t     = static_cast<double>(i) / rate;
        const double hush  = hushHigh.hp(hushB.lp(hushA.lp(a[i])));
        const Svf::Out s   = air.step(c[i], gAir, 1.1);
        const double airy  = s.band * (0.35 + 0.65 * std::pow(0.5 + 0.5 * lfo(i, n, 3, 0.2), 2.0));
        const double drone = 0.22 * (std::sin(2.0 * kPi * f1 * t) + 0.7 * std::sin(2.0 * kPi * f2 * t + 1.1))
                           * (0.6 + 0.4 * lfo(i, n, 2, 0.8));
        const double swell = 1.0 + 0.10 * lfo(i, n, 2, 1.3) + 0.06 * lfo(i, n, 5, 3.0);
        return (1.6 * hush + 0.55 * airy + drone + 0.10 * glint[i]) * swell;
    });
    finish(x, 0.07, 0.95);
    return x;
}

// Storm: wind that howls (a wide, fast sweep), a deep surge underneath, and hard
// rain on top. Louder and rougher than the rain and wind beds, which is the point:
// WeatherAudio fades it in with wind x rain, over the two of them.
std::vector<double> makeStorm(double rate, double seconds)
{
    const size_t n = static_cast<size_t>(rate * seconds);
    Pcg rng(0x53544F52);   // "STOR"
    const std::vector<double> a = whiteNoise(n, rng);
    const std::vector<double> c = whiteNoise(n, rng);
    const std::vector<double> e = whiteNoise(n, rng);
    const std::vector<double> d = drops(n, rate, 380.0, 1.5, 5.0, rng);

    OnePole surgeA(110.0, rate), surgeB(110.0, rate), rainLow(6500.0, rate), rainHigh(500.0, rate), tickHigh(1000.0, rate);
    Svf     howl, shriek;
    std::vector<double> x = circular(n, [&](size_t i) {
        const double sweep = 700.0 + 330.0 * lfo(i, n, 3, 0.5) + 170.0 * lfo(i, n, 8, 2.2) + 80.0 * lfo(i, n, 17, 0.7);   // 120..1280 Hz
        const Svf::Out lo  = howl.step(a[i], svfG(sweep, rate), 0.62);
        const Svf::Out hi  = shriek.step(c[i], svfG(sweep * 2.1, rate), 0.40);
        const double surge = surgeB.lp(surgeA.lp(a[i])) * (0.55 + 0.45 * lfo(i, n, 4, 1.4));
        const double rain  = rainHigh.hp(rainLow.lp(e[i]));
        const double tick  = tickHigh.hp(d[i]);
        const double gust  = 0.66 + 0.24 * lfo(i, n, 5, 0.1) + 0.10 * lfo(i, n, 13, 3.3);
        return (3.0 * surge + 1.00 * lo.band + 0.38 * hi.band + 0.55 * rain + 0.45 * tick) * gust;
    });
    // A storm clips a little at its worst gusts; a soft knee (unity gain for small
    // samples, so the RMS stays what finish() set) keeps that musical instead of
    // flattening the whole clip down to its loudest sample.
    finish(x, 0.17, 4.0);
    for (double& v : x) v = std::tanh(v * 1.35) / 1.35;
    double peak = 0.0;
    for (double v : x) peak = std::max(peak, std::fabs(v));
    if (peak > 0.95) for (double& v : x) v *= 0.95 / peak;
    return x;
}

// Thunder: one roll, not a loop. A sharp crack (a double one, as real thunder
// has), then a low rumble that rises and falls in several lobes, getting darker
// and quieter, and ends at exactly zero. It starts at zero too, so playing it
// from the top can never click. WeatherAudio decides how late it arrives and how
// loud; the clip is always "a strike", at its full level.
std::vector<double> makeThunder(double rate, double seconds)
{
    const size_t n = static_cast<size_t>(rate * seconds);
    Pcg rng(0x54484E44);   // "THND"
    const std::vector<double> a = whiteNoise(n, rng);
    const std::vector<double> c = whiteNoise(n, rng);

    struct Lobe { double t0, rise, tau, amp; };
    std::vector<Lobe> lobes;
    double t = 0.04, amp = 1.0;
    for (int k = 0; k < 7; ++k)
    {
        lobes.push_back({ t, 0.07 + 0.16 * rng.uniform(), 0.30 + 0.55 * rng.uniform(), amp * (0.72 + 0.28 * rng.uniform()) });
        t   += 0.30 + 0.60 * rng.uniform();
        amp *= 0.84;
    }

    OnePole rumbleA(200.0, rate), rumbleB(200.0, rate), crackLow(5200.0, rate), crackHigh(350.0, rate);
    std::vector<double> rumble(n, 0.0), crack(n, 0.0);
    for (size_t i = 0; i < n; ++i)
    {
        const double ts = static_cast<double>(i) / rate;
        // The rumble darkens as it goes: 260 Hz at the strike, 55 Hz by the tail.
        const double cut = 55.0 + 205.0 * std::exp(-ts / 2.0);
        rumbleA.setCutoff(cut, rate);
        rumbleB.setCutoff(cut, rate);
        double env = 0.0;
        for (const Lobe& l : lobes)
            if (ts >= l.t0)
            {
                const double dt = ts - l.t0;
                env += l.amp * (1.0 - std::exp(-dt / (l.rise * 0.45))) * std::exp(-dt / l.tau);
            }
        rumble[i] = rumbleB.lp(rumbleA.lp(a[i])) * env;

        const double c1 = (1.0 - std::exp(-ts / 0.0015)) * std::exp(-ts / 0.045);
        const double d2 = ts - 0.17;
        const double c2 = d2 > 0.0 ? 0.55 * (1.0 - std::exp(-d2 / 0.003)) * std::exp(-d2 / 0.07) : 0.0;
        crack[i] = crackHigh.hp(crackLow.lp(c[i])) * (c1 + c2);
    }
    auto peakOf = [](const std::vector<double>& v) {
        double p = 0.0;
        for (double s : v) p = std::max(p, std::fabs(s));
        return p;
    };
    const double pr = peakOf(rumble), pc = peakOf(crack);

    // The last 0.7 s fade to nothing with a raised cosine, so the tail ends at 0.
    const double fadeStart = seconds - 0.7;
    std::vector<double> x(n);
    for (size_t i = 0; i < n; ++i)
    {
        const double ts   = static_cast<double>(i) / rate;
        const double fade = ts < fadeStart ? 1.0 : 0.5 + 0.5 * std::cos(kPi * (ts - fadeStart) / 0.7);
        x[i] = (0.85 * rumble[i] / pr + 0.55 * crack[i] / pc) * fade;
    }
    x[n - 1] = 0.0;
    const double peak = peakOf(x);
    for (double& v : x) v *= 0.92 / peak;
    return x;
}

// ── The table ───────────────────────────────────────────────────────────────

struct Sound
{
    const char*           name;
    HE::UUID              id;
    int                   rate;       // Hz
    double                seconds;
    bool                  loops;
    std::vector<double> (*make)(double rate, double seconds);
};

// Order = UUID index order; append only, never insert (the index IS the UUID).
// Loop lengths differ on purpose (6, 8, 7, 9 s): beds that play together then
// drift against each other instead of repeating as one block every few seconds.
const Sound kSounds[] = {
    { "Rain",    HE::kEngineWeatherRainSoundId,    22050, 6.0, true,  makeRain    },
    { "Wind",    HE::kEngineWeatherWindSoundId,    11025, 8.0, true,  makeWind    },
    { "Snow",    HE::kEngineWeatherSnowSoundId,    11025, 7.0, true,  makeSnow    },
    { "Storm",   HE::kEngineWeatherStormSoundId,   16000, 9.0, true,  makeStorm   },
    { "Thunder", HE::kEngineWeatherThunderSoundId, 16000, 6.0, false, makeThunder },
};

std::vector<uint8_t> toPcm16(const std::vector<double>& x)
{
    std::vector<uint8_t> bytes(x.size() * 2);
    for (size_t i = 0; i < x.size(); ++i)
    {
        const long v = std::lround(std::clamp(x[i], -1.0, 1.0) * 32767.0);
        const uint16_t u = static_cast<uint16_t>(static_cast<int16_t>(v));
        bytes[i * 2]     = static_cast<uint8_t>(u & 0xFFu);          // little endian, whatever the host is
        bytes[i * 2 + 1] = static_cast<uint8_t>((u >> 8) & 0xFFu);
    }
    return bytes;
}

// |first - last| against the RMS step between neighbours: about 1 for a seam that
// is an ordinary pair of samples, many times that for a click.
double seamRatio(const std::vector<double>& x)
{
    double sq = 0.0;
    for (size_t i = 1; i < x.size(); ++i) { const double d = x[i] - x[i - 1]; sq += d * d; }
    const double rms = std::sqrt(sq / static_cast<double>(x.size() - 1));
    return rms > 0.0 ? std::fabs(x.front() - x.back()) / rms : 0.0;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: weather_sound_gen <output-dir>\n"
                             "  writes Rain / Wind / Snow / Storm / Thunder .hasset into <output-dir>\n"
                             "  (EditorDeps/EngineContent/Audio/Weather)\n");
        return 2;
    }
    const std::string outDir = argv[1];
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    ContentManager cm(outDir);
    const int total = static_cast<int>(std::size(kSounds));
    int ok = 0;
    size_t totalBytes = 0;
    for (size_t index = 0; index < std::size(kSounds); ++index)
    {
        const Sound& s = kSounds[index];
        const std::vector<double> samples = s.make(static_cast<double>(s.rate), s.seconds);

        AudioAsset a;
        a.type       = HE::AssetType::Audio;
        a.name       = s.name;
        a.path       = std::string(s.name) + ".hasset";
        a.id         = s.id;
        a.sampleRate = s.rate;
        a.channels   = 1;
        a.encoding   = AudioEncoding::PCM16;
        a.audioData  = toPcm16(samples);

        if (!cm.saveAsset(a))
        {
            std::fprintf(stderr, "  FAILED to write %s.hasset\n", s.name);
            continue;
        }

        // Read it back through a fresh ContentManager: the file on disk, not the
        // copy in memory, is what the editor and the packer will see.
        ContentManager check(outDir);
        const AudioAsset* t = check.getAudio(check.loadAsset(a.path));
        if (!t || t->id != a.id || t->sampleRate != a.sampleRate || t->channels != 1 ||
            t->encoding != AudioEncoding::PCM16 || t->audioData != a.audioData)
        {
            std::fprintf(stderr, "  %s.hasset does NOT read back as written\n", s.name);
            continue;
        }

        double sq = 0.0, peak = 0.0;
        for (double v : samples) { sq += v * v; peak = std::max(peak, std::fabs(v)); }
        const double rms = std::sqrt(sq / static_cast<double>(samples.size()));
        std::printf("  %-8s %5d Hz mono %4.1f s  %6.1f KB  rms %.3f  peak %.3f  %s\n", s.name, s.rate, s.seconds,
                    static_cast<double>(a.audioData.size()) / 1024.0, rms, peak,
                    s.loops ? "loop" : "one-shot");
        if (s.loops)
            std::printf("           seam: |first-last| = %.2f x the rms step\n", seamRatio(samples));
        totalBytes += a.audioData.size();
        ++ok;
    }

    std::printf("weather_sound_gen: wrote %d/%d sounds (%.2f MB of samples) to %s\n", ok, total,
                static_cast<double>(totalBytes) / (1024.0 * 1024.0), outDir.c_str());
    return ok == total ? 0 : 1;
}
