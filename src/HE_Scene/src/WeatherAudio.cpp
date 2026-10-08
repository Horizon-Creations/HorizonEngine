#include "HorizonScene/WeatherAudio.h"
#include "HorizonScene/AudioEngine.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/Components/EnvironmentComponent.h"
#include <ContentManager/ContentManager.h>
#include <ContentManager/DefaultAssets.h>
#include <Diagnostics/Log.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>

namespace WeatherAudio
{
    namespace
    {
        float saturate(float x) { return std::clamp(x, 0.0f, 1.0f); }

        float smoothstep(float edge0, float edge1, float x)
        {
            const float t = saturate((x - edge0) / (edge1 - edge0));
            return t * t * (3.0f - 2.0f * t);
        }

        // The bus a weather voice asks for: the component's own when the mixer has
        // it, else none — which lets the clip's own bus apply, and master after it,
        // without the "unknown bus" warning a bus that was never created would earn
        // on every start.
        std::string busFor(const AudioEngine& engine, const std::string& wanted)
        {
            return (!wanted.empty() && engine.hasBus(wanted)) ? wanted : std::string{};
        }

        // A resident clip with samples, or nullptr. ensureResident loads a local
        // asset on the spot and starts the download of a remote one (returning
        // false then), so a clip can show up a few frames later.
        const AudioAsset* findClip(ContentManager& content, const HE::UUID& id)
        {
            const AudioAsset* clip = content.getAudio(id);
            if (!clip) { content.ensureResident(id); clip = content.getAudio(id); }
            return (clip && !clip->audioData.empty()) ? clip : nullptr;
        }

        void stopThunder(State& s, AudioEngine& engine)
        {
            for (uint64_t h : s.thunder) engine.stop(h);
            s.thunder.clear();
            s.pending.clear();
        }

        void stopBed(State::Bed& bed, AudioEngine& engine)
        {
            if (bed.handle != 0) engine.stop(bed.handle);
            bed.handle  = 0;
            bed.applied = -1.0f;
        }

        void warnMissing(HE::UUID& warned, const HE::UUID& id, const char* what)
        {
            if (warned == id) return;
            warned = id;
            HE_LOG_WARN(Audio, "Weather %s sound is not available (asset %016llx-%016llx): that sound stays silent",
                        what, static_cast<unsigned long long>(id.hi), static_cast<unsigned long long>(id.lo));
        }
    }

    const char* layerName(Layer layer)
    {
        switch (layer)
        {
            case Layer::Rain:  return "rain";
            case Layer::Wind:  return "wind";
            case Layer::Snow:  return "snow";
            case Layer::Storm: return "storm";
            default:           return "?";
        }
    }

    // ── Curves ──────────────────────────────────────────────────────────────

    float loudness(float amount)
    {
        const float a = saturate(amount);
        return std::sqrt(a) * smoothstep(0.0f, 0.1f, a);
    }

    float windAmount(float windSpeed)
    {
        return saturate((windSpeed - 1.0f) / 1.6f);
    }

    float stormAmount(float rainAmount, float windSpeed)
    {
        return saturate((windSpeed - 1.6f) / 1.0f) * saturate(rainAmount);
    }

    float windGust(float weatherTime)
    {
        // 0.91 +- 0.06 +- 0.03: the two swells never line up past 1.0 or under 0.82.
        return 0.91f + 0.06f * std::sin(weatherTime * 0.7f) + 0.03f * std::sin(weatherTime * 1.9f + 1.3f);
    }

    float LoopGains::of(Layer layer) const
    {
        switch (layer)
        {
            case Layer::Rain:  return rain;
            case Layer::Wind:  return wind;
            case Layer::Snow:  return snow;
            case Layer::Storm: return storm;
            default:           return 0.0f;
        }
    }

    LoopGains loopGains(const WeatherComponent& w, const EnvironmentComponent* env)
    {
        LoopGains g;
        if (!w.soundEnabled) return g;

        float rain = 0.0f, snow = 0.0f;
        if (env) { rain = env->rainAmount; snow = env->snowAmount; }
        else
        {
            rain = (w.curPrecipType == PrecipType::Rain) ? w.curPrecip : 0.0f;
            snow = (w.curPrecipType == PrecipType::Snow) ? w.curPrecip : 0.0f;
        }
        const float wind  = w.curWindSpeed;
        const float gust  = windGust(w.weatherTime);
        const float scale = kHeadroom * saturate(w.soundVolume);

        g.rain  = scale * loudness(rain);
        g.snow  = scale * loudness(snow);
        g.wind  = scale * loudness(windAmount(wind)) * gust;
        g.storm = scale * loudness(stormAmount(rain, wind)) * gust;
        return g;
    }

    float slew(float current, float target, float dt, float riseSeconds, float fallSeconds)
    {
        if (dt <= 0.0f) return current;
        if (target > current)
        {
            const float step = dt / std::max(riseSeconds, 1e-4f);
            return std::min(current + step, target);
        }
        const float step = dt / std::max(fallSeconds, 1e-4f);
        return std::max(current - step, target);
    }

    float thunderDelay(float distance)
    {
        return std::max(distance, 0.0f) / kSpeedOfSound;
    }

    float thunderLevel(float distance)
    {
        if (distance <= kThunderReferenceDist) return 1.0f;
        return std::max(kThunderReferenceDist / distance, kThunderFloorLevel);
    }

    Strike makeStrike(std::mt19937& rng)
    {
        std::uniform_real_distribution<float> dist(kThunderMinDistance, kThunderMaxDistance);
        Strike s;
        s.distance = dist(rng);
        s.delay    = thunderDelay(s.distance);
        s.level    = thunderLevel(s.distance);
        return s;
    }

    HE::UUID soundFor(const WeatherComponent& w, Layer layer)
    {
        const auto pick = [](const HE::UUID& own, const HE::UUID& fallback)
        { return own == HE::UUID{} ? fallback : own; };
        switch (layer)
        {
            case Layer::Rain:  return pick(w.rainSound,  HE::kEngineWeatherRainSoundId);
            case Layer::Wind:  return pick(w.windSound,  HE::kEngineWeatherWindSoundId);
            case Layer::Snow:  return pick(w.snowSound,  HE::kEngineWeatherSnowSoundId);
            case Layer::Storm: return pick(w.stormSound, HE::kEngineWeatherStormSoundId);
            default:           return {};
        }
    }

    HE::UUID thunderSoundFor(const WeatherComponent& w)
    {
        return w.thunderSound == HE::UUID{} ? HE::kEngineWeatherThunderSoundId : w.thunderSound;
    }

    // ── Runtime ─────────────────────────────────────────────────────────────

    State::State(uint32_t seed)
        : rng(seed != 0 ? seed : std::random_device{}())
    {
    }

    void stop(State& state, AudioEngine& engine)
    {
        for (State::Bed& bed : state.beds)
        {
            stopBed(bed, engine);
            bed.gain  = 0.0f;
            bed.retry = 0.0f;
        }
        stopThunder(state, engine);
        state.seenValid = false;
    }

    void update(State& state, HorizonWorld& world, AudioEngine& engine,
                ContentManager& content, const Frame& frame)
    {
        if (!engine.isInitialized()) return;

        auto& reg = world.registry();
        WeatherComponent* w = nullptr;
        for (auto [e, wc] : reg.view<WeatherComponent>().each()) { w = &wc; break; }
        if (!w) { stop(state, engine); return; }

        const Entity envEntity = world.environmentEntity();
        const EnvironmentComponent* env = (envEntity == entt::null)
            ? nullptr : reg.try_get<EnvironmentComponent>(envEntity);

        const bool  on     = frame.audible && w->soundEnabled;
        const float realDt = std::max(frame.realDt, 0.0f);
        const float gameDt = std::max(frame.gameDt, 0.0f);
        const std::string bus = busFor(engine, w->soundBus);

        // ── Beds ────────────────────────────────────────────────────────────
        const LoopGains target = on ? loopGains(*w, env) : LoopGains{};
        for (int i = 0; i < kLayerCount; ++i)
        {
            const Layer layer = static_cast<Layer>(i);
            State::Bed& bed   = state.beds[static_cast<size_t>(i)];
            const HE::UUID id = soundFor(*w, layer);

            // The slot was re-assigned under a running voice: drop it, the new
            // clip fades in from silence like any other.
            if (bed.handle != 0 && bed.asset != id) { stopBed(bed, engine); bed.gain = 0.0f; }
            // A voice something else stopped (stopAll, a removed bus).
            if (bed.handle != 0 && !engine.isPlaying(bed.handle)) stopBed(bed, engine);

            const float want = target.of(layer);
            bed.gain = slew(bed.gain, want, realDt, kRiseSeconds, on ? kFallSeconds : kMuteFadeSeconds);

            if (bed.gain <= kSilent && want <= kSilent)
            {
                // Nothing to hear and nothing coming: no voice.
                stopBed(bed, engine);
                bed.gain  = 0.0f;
                bed.retry = 0.0f;
                continue;
            }

            if (bed.handle == 0)
            {
                // Only a failed look waits: a voice that died (stopAll, a removed
                // bus) or a slot that was re-assigned starts again on the spot.
                bed.retry -= realDt;
                if (bed.retry > 0.0f) continue;

                const AudioAsset* clip = findClip(content, id);
                if (clip) bed.handle = engine.play(*clip, bed.gain, 1.0f, /*loop=*/true, bus);
                if (bed.handle == 0)
                {
                    warnMissing(bed.warned, id, layerName(layer));
                    bed.retry = kRetrySeconds;
                    continue;
                }
                bed.asset   = id;
                bed.applied = bed.gain;
                bed.retry   = 0.0f;
                continue;
            }

            if (std::abs(bed.gain - bed.applied) > 1e-4f)
            {
                engine.setSoundVolume(bed.handle, bed.gain);
                bed.applied = bed.gain;
            }
        }

        // ── Thunder ─────────────────────────────────────────────────────────
        // Strikes are counted, not flagged: this may run on any frame relative to
        // the tick. The first sight of a component (and a counter that went
        // backwards: a reloaded scene) only takes the count, so a load does not
        // replay the strikes of the previous one.
        if (!state.seenValid || w->strikeCount < state.seenStrikes)
        {
            state.seenStrikes = w->strikeCount;
            state.seenValid   = true;
        }
        else if (w->strikeCount != state.seenStrikes)
        {
            const uint32_t fresh = w->strikeCount - state.seenStrikes;
            state.seenStrikes    = w->strikeCount;
            if (on)
            {
                for (uint32_t k = 0; k < fresh && static_cast<int>(state.pending.size()) < kMaxPendingStrikes; ++k)
                {
                    const Strike s = makeStrike(state.rng);
                    state.pending.push_back({ s.delay, s.level });
                }
            }
        }

        if (!on)
        {
            // Paused or switched off: nothing is on its way and nothing rings on.
            stopThunder(state, engine);
            return;
        }

        state.thunder.erase(std::remove_if(state.thunder.begin(), state.thunder.end(),
                                [&](uint64_t h) { return !engine.isPlaying(h); }),
                            state.thunder.end());

        for (size_t i = 0; i < state.pending.size();)
        {
            State::Pending& p = state.pending[i];
            p.delay -= gameDt;
            if (p.delay > 0.0f) { ++i; continue; }

            const float level = p.level;
            state.pending.erase(state.pending.begin() + static_cast<std::ptrdiff_t>(i));
            if (static_cast<int>(state.thunder.size()) >= kMaxThunderVoices) continue;

            const HE::UUID id = thunderSoundFor(*w);
            const AudioAsset* clip = findClip(content, id);
            if (!clip) { warnMissing(state.thunderWarned, id, "thunder"); continue; }

            std::uniform_real_distribution<float> jitter(0.92f, 1.08f);
            const float pitch = jitter(state.rng);
            const uint64_t h = engine.play(*clip, level * saturate(w->soundVolume), pitch, /*loop=*/false, bus);
            if (h != 0) state.thunder.push_back(h);
        }
    }
}
