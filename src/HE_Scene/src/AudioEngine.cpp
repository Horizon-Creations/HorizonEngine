#define MA_IMPLEMENTATION
#define MA_NO_FLAC
#define MA_NO_MP3
#define MA_NO_ENCODING
#include <miniaudio.h>

#include "HorizonScene/AudioEngine.h"
#include <unordered_map>
#include <cstring>

// ─── PIMPL ────────────────────────────────────────────────────────────────────

struct ActiveSound
{
    std::vector<uint8_t> pcmCopy;   // owns the PCM bytes
    ma_audio_buffer      buffer;
    ma_sound             sound;
    bool                 bufferOk = false;
    bool                 soundOk  = false;
};

struct AudioEngine::Impl
{
    ma_engine                                        engine;
    bool                                             engineOk = false;
    std::unordered_map<uint64_t, std::unique_ptr<ActiveSound>> sounds;
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

    if (ma_engine_init(&cfg, &m_impl->engine) != MA_SUCCESS)
        return false;

    m_impl->engineOk = true;
    m_initialized    = true;
    return true;
}

void AudioEngine::shutdown()
{
    if (!m_initialized) return;
    stopAll();
    ma_engine_uninit(&m_impl->engine);
    m_impl->engineOk  = false;
    m_initialized     = false;
}

uint64_t AudioEngine::play(const std::vector<uint8_t>& pcmData,
                            int sampleRate, int channels,
                            float volume, float pitch, bool loop)
{
    if (!m_initialized || pcmData.empty()) return 0;
    if (sampleRate <= 0 || channels <= 0)  return 0;

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

    if (ma_audio_buffer_init(&bcfg, &snd->buffer) != MA_SUCCESS)
        return 0;
    snd->bufferOk = true;

    ma_uint32 flags = MA_SOUND_FLAG_NO_SPATIALIZATION;
    if (ma_sound_init_from_data_source(&m_impl->engine,
                                        &snd->buffer,
                                        flags, nullptr,
                                        &snd->sound) != MA_SUCCESS)
    {
        ma_audio_buffer_uninit(&snd->buffer);
        return 0;
    }
    snd->soundOk = true;

    ma_sound_set_volume(&snd->sound, volume);
    ma_sound_set_pitch(&snd->sound, pitch);
    ma_sound_set_looping(&snd->sound, loop ? MA_TRUE : MA_FALSE);

    if (ma_sound_start(&snd->sound) != MA_SUCCESS)
    {
        ma_sound_uninit(&snd->sound);
        ma_audio_buffer_uninit(&snd->buffer);
        return 0;
    }

    uint64_t handle = m_nextHandle++;
    m_impl->sounds.emplace(handle, std::move(snd));
    return handle;
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
                                   float minDist, float maxDist)
{
    if (!m_initialized || pcmData.empty()) return 0;
    if (sampleRate <= 0 || channels <= 0)  return 0;

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

    if (ma_audio_buffer_init(&bcfg, &snd->buffer) != MA_SUCCESS)
        return 0;
    snd->bufferOk = true;

    // No NO_SPATIALIZATION flag — spatial positioning enabled
    if (ma_sound_init_from_data_source(&m_impl->engine,
                                        &snd->buffer,
                                        0, nullptr,
                                        &snd->sound) != MA_SUCCESS)
    {
        ma_audio_buffer_uninit(&snd->buffer);
        return 0;
    }
    snd->soundOk = true;

    ma_sound_set_volume(&snd->sound, volume);
    ma_sound_set_pitch(&snd->sound, pitch);
    ma_sound_set_looping(&snd->sound, loop ? MA_TRUE : MA_FALSE);
    ma_sound_set_position(&snd->sound, x, y, z);
    ma_sound_set_attenuation_model(&snd->sound, ma_attenuation_model_linear);
    ma_sound_set_min_distance(&snd->sound, minDist > 0.0f ? minDist : 0.01f);
    ma_sound_set_max_distance(&snd->sound, maxDist > minDist ? maxDist : minDist + 1.0f);

    if (ma_sound_start(&snd->sound) != MA_SUCCESS)
    {
        ma_sound_uninit(&snd->sound);
        ma_audio_buffer_uninit(&snd->buffer);
        return 0;
    }

    uint64_t handle = m_nextHandle++;
    m_impl->sounds.emplace(handle, std::move(snd));
    return handle;
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
