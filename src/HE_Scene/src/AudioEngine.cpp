#define MA_IMPLEMENTATION
#define MA_NO_FLAC
#define MA_NO_MP3
#define MA_NO_ENCODING
#include <miniaudio.h>
#include <cstdint>

#include "HorizonScene/AudioEngine.h"
#include <Diagnostics/Log.h>
#include <unordered_map>
#include <cstring>
#include <string>

// A scene that keeps starting sounds without ever stopping them (a looping clip
// re-triggered every frame) leaks voices until the mixer chokes. Warn once the
// count gets unreasonable rather than letting the audio quietly fall apart.
static constexpr size_t kVoiceWarnThreshold = 128;

// ─── PIMPL ────────────────────────────────────────────────────────────────────

struct ActiveSound
{
    std::vector<uint8_t> pcmCopy;   // owns the PCM bytes
    ma_audio_buffer      buffer;
    ma_sound             sound;
    bool                 bufferOk = false;
    bool                 soundOk  = false;
};

struct BusData
{
    ma_sound_group group;
    bool           groupOk = false;
};

struct AudioEngine::Impl
{
    ma_engine                                        engine;
    bool                                             engineOk = false;
    std::unordered_map<uint64_t, std::unique_ptr<ActiveSound>> sounds;
    std::unordered_map<std::string, std::unique_ptr<BusData>>  buses;
};

// ─── AudioEngine ─────────────────────────────────────────────────────────────

AudioEngine::AudioEngine() : m_impl(std::make_unique<Impl>()) {}

AudioEngine::~AudioEngine() { shutdown(); }

bool AudioEngine::init(bool noDevice)
{
    if (m_initialized) return true;

    ma_engine_config cfg = ma_engine_config_init();
    if (noDevice)
    {
        cfg.noDevice   = MA_TRUE;
        cfg.channels   = 2;
        cfg.sampleRate = 48000;
    }

    const ma_result rc = ma_engine_init(&cfg, &m_impl->engine);
    if (rc != MA_SUCCESS)
    {
        // Worth an error even in noDevice mode: without it the whole game is
        // silent and nothing anywhere says why.
        HE_LOG_ERROR(Audio, "miniaudio engine init failed (%s), result %d — audio disabled",
                     noDevice ? "no-device mode" : "device mode", static_cast<int>(rc));
        return false;
    }

    m_impl->engineOk = true;
    m_initialized    = true;
    HE_LOG_INFO(Audio, "Audio engine ready: %u Hz, %u channel(s)%s",
                ma_engine_get_sample_rate(&m_impl->engine),
                ma_engine_get_channels(&m_impl->engine),
                noDevice ? " (no output device — headless)" : "");
    return true;
}

void AudioEngine::shutdown()
{
    if (!m_initialized) return;
    HE_LOG_INFO(Audio, "Audio engine shutting down (%zu active voice(s), %zu bus(es))",
                m_impl->sounds.size(), m_impl->buses.size());
    stopAll();
    // Uninit buses before engine teardown
    for (auto& [name, bus] : m_impl->buses)
        if (bus->groupOk) { ma_sound_group_uninit(&bus->group); bus->groupOk = false; }
    m_impl->buses.clear();
    ma_engine_uninit(&m_impl->engine);
    m_impl->engineOk  = false;
    m_initialized     = false;
}

// ─── Bus management ────────────────────────────────────────────────────────────
bool AudioEngine::createBus(const std::string& name, float volume)
{
    if (!m_initialized || name.empty()) return false;
    if (m_impl->buses.count(name)) return true; // idempotent

    auto bus = std::make_unique<BusData>();
    if (ma_sound_group_init(&m_impl->engine, 0, nullptr, &bus->group) != MA_SUCCESS)
    {
        HE_LOG_ERROR(Audio, "Failed to create audio bus '%s' — sounds routed to it "
                            "will fall back to the master bus", name.c_str());
        return false;
    }
    bus->groupOk = true;
    ma_sound_group_set_volume(&bus->group, volume);
    m_impl->buses.emplace(name, std::move(bus));
    HE_LOG_DEBUG(Audio, "Created audio bus '%s' at volume %.2f", name.c_str(), volume);
    return true;
}

void AudioEngine::setBusVolume(const std::string& name, float volume)
{
    if (!m_initialized) return;
    auto it = m_impl->buses.find(name);
    if (it != m_impl->buses.end() && it->second->groupOk)
        ma_sound_group_set_volume(&it->second->group, volume);
}

float AudioEngine::getBusVolume(const std::string& name) const
{
    if (!m_initialized) return 1.0f;
    auto it = m_impl->buses.find(name);
    if (it == m_impl->buses.end() || !it->second->groupOk) return 1.0f;
    return ma_sound_group_get_volume(&it->second->group);
}

bool AudioEngine::hasBus(const std::string& name) const
{
    return m_impl->buses.count(name) > 0;
}

// Everything play() and playSpatial() have in common — the PCM copy, the audio
// buffer, the bus routing and the start. Only the spatialization flag and the
// positional setup below differ, so this is written once.
uint64_t AudioEngine::startSound(const std::vector<uint8_t>& pcmData,
                                  int sampleRate, int channels,
                                  float volume, float pitch, bool loop,
                                  const std::string& busName,
                                  const SpatialParams* spatial)
{
    if (!m_initialized)
    {
        HE_LOG_THROTTLE(Audio, Warning, 5.0, "%s",
                        "Sound requested but the audio engine is not initialised — ignored");
        return 0;
    }
    if (pcmData.empty())
    {
        HE_LOG_WARN(Audio, "%s", "Sound requested with empty PCM data — ignored "
                                 "(the clip asset probably failed to decode)");
        return 0;
    }
    if (sampleRate <= 0 || channels <= 0)
    {
        HE_LOG_WARN(Audio, "Sound requested with invalid format (%d Hz, %d channel(s)) — ignored",
                    sampleRate, channels);
        return 0;
    }

    auto snd = std::make_unique<ActiveSound>();
    snd->pcmCopy = pcmData;

    const ma_uint64 frameCount =
        snd->pcmCopy.size() / (sizeof(int16_t) * static_cast<size_t>(channels));

    ma_audio_buffer_config bcfg = ma_audio_buffer_config_init(
        ma_format_s16,
        static_cast<ma_uint32>(channels),
        frameCount,
        snd->pcmCopy.data(),
        nullptr);
    // ma_audio_buffer_config_init() hardcodes sampleRate = 0 (a documented
    // miniaudio TODO for 0.12), and a zero rate makes ma_sound_init_from_data_source()
    // fall back to the engine's rate and skip the resampler entirely — a 44.1 kHz
    // clip would then play back ~9% too fast on the 48 kHz engine. Set it explicitly.
    bcfg.sampleRate = static_cast<ma_uint32>(sampleRate);

    if (ma_audio_buffer_init(&bcfg, &snd->buffer) != MA_SUCCESS)
    {
        HE_LOG_ERROR(Audio, "Audio buffer init failed (%llu frames, %d Hz, %d channel(s))",
                     static_cast<unsigned long long>(frameCount), sampleRate, channels);
        return 0;
    }
    snd->bufferOk = true;

    // Route through bus if found, otherwise null (master)
    ma_sound_group* busGroup = nullptr;
    if (!busName.empty()) {
        auto it = m_impl->buses.find(busName);
        if (it != m_impl->buses.end() && it->second->groupOk)
            busGroup = &it->second->group;
        else
            HE_LOG_WARN(Audio, "Sound routed to unknown bus '%s' — playing on the master bus "
                               "(bus volume/mute will not apply)", busName.c_str());
    }

    // Spatial sounds pass no flag — positioning enabled.
    ma_uint32 flags = spatial ? 0u : MA_SOUND_FLAG_NO_SPATIALIZATION;
    if (ma_sound_init_from_data_source(&m_impl->engine,
                                        &snd->buffer,
                                        flags, busGroup,
                                        &snd->sound) != MA_SUCCESS)
    {
        HE_LOG_ERROR(Audio, "%s", "Sound init from data source failed");
        ma_audio_buffer_uninit(&snd->buffer);
        return 0;
    }
    snd->soundOk = true;

    ma_sound_set_volume(&snd->sound, volume);
    ma_sound_set_pitch(&snd->sound, pitch);
    ma_sound_set_looping(&snd->sound, loop ? MA_TRUE : MA_FALSE);
    if (spatial)
    {
        ma_sound_set_position(&snd->sound, spatial->x, spatial->y, spatial->z);
        ma_sound_set_attenuation_model(&snd->sound, ma_attenuation_model_linear);
        ma_sound_set_min_distance(&snd->sound, spatial->minDist > 0.0f ? spatial->minDist : 0.01f);
        ma_sound_set_max_distance(&snd->sound, spatial->maxDist > spatial->minDist
                                                   ? spatial->maxDist : spatial->minDist + 1.0f);
    }

    if (ma_sound_start(&snd->sound) != MA_SUCCESS)
    {
        HE_LOG_ERROR(Audio, "%s", "ma_sound_start failed — sound will not be audible");
        ma_sound_uninit(&snd->sound);
        ma_audio_buffer_uninit(&snd->buffer);
        return 0;
    }

    uint64_t handle = m_nextHandle++;
    m_impl->sounds.emplace(handle, std::move(snd));

    HE_LOG_TRACE(Audio, "Started %s sound #%llu: %llu frames, %d Hz, %d ch, vol %.2f, "
                        "pitch %.2f%s, bus '%s'",
                 spatial ? "spatial" : "2D",
                 static_cast<unsigned long long>(handle),
                 static_cast<unsigned long long>(frameCount), sampleRate, channels,
                 volume, pitch, loop ? ", looping" : "",
                 busName.empty() ? "master" : busName.c_str());

    if (m_impl->sounds.size() >= kVoiceWarnThreshold)
        HE_LOG_THROTTLE(Audio, Warning, 10.0,
                        "%zu simultaneous voices are alive — sounds are being started "
                        "faster than they are stopped", m_impl->sounds.size());
    return handle;
}

uint64_t AudioEngine::play(const std::vector<uint8_t>& pcmData,
                            int sampleRate, int channels,
                            float volume, float pitch, bool loop,
                            const std::string& busName)
{
    return startSound(pcmData, sampleRate, channels, volume, pitch, loop, busName, nullptr);
}

void AudioEngine::stop(uint64_t handle)
{
    auto it = m_impl->sounds.find(handle);
    if (it == m_impl->sounds.end()) return;
    auto& snd = *it->second;
    if (snd.soundOk)  { ma_sound_stop(&snd.sound);  ma_sound_uninit(&snd.sound);  }
    if (snd.bufferOk) { ma_audio_buffer_uninit(&snd.buffer); }
    m_impl->sounds.erase(it);
}

void AudioEngine::stopAll()
{
    for (auto& [handle, snd] : m_impl->sounds)
    {
        if (snd->soundOk)  { ma_sound_stop(&snd->sound);  ma_sound_uninit(&snd->sound);  }
        if (snd->bufferOk) { ma_audio_buffer_uninit(&snd->buffer); }
    }
    m_impl->sounds.clear();
}

uint64_t AudioEngine::playSpatial(const std::vector<uint8_t>& pcmData,
                                   int sampleRate, int channels,
                                   float volume, float pitch, bool loop,
                                   float x, float y, float z,
                                   float minDist, float maxDist,
                                   const std::string& busName)
{
    const SpatialParams sp{ x, y, z, minDist, maxDist };
    return startSound(pcmData, sampleRate, channels, volume, pitch, loop, busName, &sp);
}

void AudioEngine::setSoundPosition(uint64_t handle, float x, float y, float z)
{
    auto it = m_impl->sounds.find(handle);
    if (it == m_impl->sounds.end()) return;
    if (it->second->soundOk)
        ma_sound_set_position(&it->second->sound, x, y, z);
}

void AudioEngine::setListenerTransform(float px, float py, float pz,
                                        float fx, float fy, float fz,
                                        float ux, float uy, float uz)
{
    if (!m_initialized) return;
    ma_engine_listener_set_position(&m_impl->engine, 0, px, py, pz);
    ma_engine_listener_set_direction(&m_impl->engine, 0, fx, fy, fz);
    ma_engine_listener_set_world_up(&m_impl->engine, 0, ux, uy, uz);
}

bool AudioEngine::isPlaying(uint64_t handle) const
{
    auto it = m_impl->sounds.find(handle);
    if (it == m_impl->sounds.end()) return false;
    return ma_sound_is_playing(&it->second->sound) == MA_TRUE;
}

uint64_t AudioEngine::getSoundCursorFrames(uint64_t handle) const
{
    auto it = m_impl->sounds.find(handle);
    if (it == m_impl->sounds.end() || !it->second->soundOk) return 0;

    ma_uint64 cursor = 0;
    if (ma_sound_get_cursor_in_pcm_frames(&it->second->sound, &cursor) != MA_SUCCESS)
        return 0;
    return static_cast<uint64_t>(cursor);
}

uint64_t AudioEngine::getSoundLengthFrames(uint64_t handle) const
{
    auto it = m_impl->sounds.find(handle);
    if (it == m_impl->sounds.end() || !it->second->soundOk) return 0;

    ma_uint64 length = 0;
    if (ma_sound_get_length_in_pcm_frames(&it->second->sound, &length) != MA_SUCCESS)
        return 0;
    return static_cast<uint64_t>(length);
}

void AudioEngine::seekSound(uint64_t handle, uint64_t frame)
{
    auto it = m_impl->sounds.find(handle);
    if (it == m_impl->sounds.end() || !it->second->soundOk) return;
    ma_sound_seek_to_pcm_frame(&it->second->sound, static_cast<ma_uint64>(frame));
}

void AudioEngine::pauseSound(uint64_t handle)
{
    auto it = m_impl->sounds.find(handle);
    if (it == m_impl->sounds.end() || !it->second->soundOk) return;
    // ma_sound_stop only halts playback — the voice, its buffer and its cursor
    // all stay put, which is what makes resumeSound() free.
    ma_sound_stop(&it->second->sound);
}

void AudioEngine::resumeSound(uint64_t handle)
{
    auto it = m_impl->sounds.find(handle);
    if (it == m_impl->sounds.end() || !it->second->soundOk) return;
    ma_sound_start(&it->second->sound);
}

void AudioEngine::setSoundLooping(uint64_t handle, bool loop)
{
    auto it = m_impl->sounds.find(handle);
    if (it == m_impl->sounds.end() || !it->second->soundOk) return;
    ma_sound_set_looping(&it->second->sound, loop ? MA_TRUE : MA_FALSE);
}

void AudioEngine::setSoundVolume(uint64_t handle, float volume)
{
    auto it = m_impl->sounds.find(handle);
    if (it == m_impl->sounds.end() || !it->second->soundOk) return;
    ma_sound_set_volume(&it->second->sound, volume);
}

void AudioEngine::setSoundPitch(uint64_t handle, float pitch)
{
    auto it = m_impl->sounds.find(handle);
    if (it == m_impl->sounds.end() || !it->second->soundOk) return;
    ma_sound_set_pitch(&it->second->sound, pitch);
}

int AudioEngine::getSoundSampleRate(uint64_t handle) const
{
    auto it = m_impl->sounds.find(handle);
    if (it == m_impl->sounds.end() || !it->second->bufferOk) return 0;

    // Query through the data-source interface — the exact same call miniaudio makes
    // internally in ma_sound_init_from_data_source() to pick the resampler ratio.
    ma_uint32 rate = 0;
    if (ma_data_source_get_data_format(&it->second->buffer, nullptr, nullptr,
                                        &rate, nullptr, 0) != MA_SUCCESS)
        return 0;
    return static_cast<int>(rate);
}
