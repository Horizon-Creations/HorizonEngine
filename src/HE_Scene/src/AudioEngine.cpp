// Ogg Vorbis comes from the vendored stb_vorbis.c (public domain). miniaudio
// only compiles its Vorbis backend when the stb_vorbis declarations are visible
// BEFORE its implementation (it keys on STB_VORBIS_INCLUDE_STB_VORBIS_H); the
// function bodies follow after it, once, in this translation unit. This must
// not define STB_VORBIS_NO_STDIO: miniaudio's file-path init names
// stb_vorbis_open_filename and would not compile without it.
#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>

#define MA_IMPLEMENTATION
#define MA_NO_FLAC
#define MA_NO_MP3
#define MA_NO_ENCODING
#include <miniaudio.h>

#undef STB_VORBIS_HEADER_ONLY
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wtautological-compare"
#endif
#include <stb_vorbis.c>
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include <cstdint>

#include "HorizonScene/AudioEngine.h"
#include <Audio/AudioBusConfig.h>
#include <Diagnostics/Log.h>
#include <algorithm>
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
    // Owns the voice's bytes: int16 PCM for a PCM16 clip, the Ogg stream for a
    // Vorbis one. Exactly one of buffer/decoder is live, and `source` points at
    // it — everything after startSound() goes through the data-source interface
    // and never needs to know which.
    std::vector<uint8_t> bytes;
    ma_audio_buffer      buffer;
    ma_decoder           decoder;
    ma_data_source*      source   = nullptr;
    ma_sound             sound;
    bool                 bufferOk  = false;
    bool                 decoderOk = false;
    bool                 soundOk   = false;
    // Set by pauseSound(), cleared by resumeSound(). miniaudio itself has no
    // paused state: a paused voice and a finished one both answer "not playing".
    bool                 paused    = false;
    // The bus the voice was routed through ("" = master, also when the named
    // bus did not exist and the voice fell back). removeBus() stops the voices
    // on a bus before tearing the group down, and this is how it finds them.
    std::string          busName;
    // What the voice attenuates with; only meaningful for a spatial voice.
    AudioAttenuation     attenuation = AudioAttenuation::Linear;
    bool                 spatial     = false;

    // Order matters: the sound reads from the data source, so it goes first.
    void release()
    {
        if (soundOk)   { ma_sound_stop(&sound); ma_sound_uninit(&sound); soundOk = false; }
        if (bufferOk)  { ma_audio_buffer_uninit(&buffer); bufferOk = false; }
        if (decoderOk) { ma_decoder_uninit(&decoder);     decoderOk = false; }
        source = nullptr;
    }
    // A voice that startSound() gives up on half-built still hands its miniaudio
    // objects back; stop()/stopAll() release before erasing, so this is a no-op there.
    ~ActiveSound() { release(); }
};

struct BusData
{
    ma_sound_group group;
    bool           groupOk = false;
    // The volume the bus is meant to have. The group carries 0 while muted,
    // so the fader's value has to live here or a mute would forget it.
    float          volume  = 1.0f;
    bool           muted   = false;
};

struct AudioEngine::Impl
{
    ma_engine                                        engine;
    bool                                             engineOk = false;
    std::unordered_map<uint64_t, std::unique_ptr<ActiveSound>> sounds;
    std::unordered_map<std::string, std::unique_ptr<BusData>>  buses;
    // The master fader, remembered for the same reason a bus's is: the engine
    // carries 0 while muted.
    float masterVolume = 1.0f;
    bool  masterMuted  = false;
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

    m_impl->engineOk     = true;
    m_impl->masterVolume = 1.0f;   // a fresh engine, whatever the last one was left at
    m_impl->masterMuted  = false;
    m_initialized        = true;
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
    bus->volume  = volume < 0.0f ? 0.0f : volume;
    ma_sound_group_set_volume(&bus->group, bus->volume);
    m_impl->buses.emplace(name, std::move(bus));
    HE_LOG_DEBUG(Audio, "Created audio bus '%s' at volume %.2f", name.c_str(), volume);
    return true;
}

void AudioEngine::setBusVolume(const std::string& name, float volume)
{
    if (!m_initialized) return;
    auto it = m_impl->buses.find(name);
    if (it == m_impl->buses.end() || !it->second->groupOk) return;
    BusData& bus = *it->second;
    bus.volume = volume < 0.0f ? 0.0f : volume;
    // A muted bus stays silent; the new value is what unmute restores.
    if (!bus.muted) ma_sound_group_set_volume(&bus.group, bus.volume);
}

float AudioEngine::getBusVolume(const std::string& name) const
{
    if (!m_initialized) return 1.0f;
    auto it = m_impl->buses.find(name);
    if (it == m_impl->buses.end() || !it->second->groupOk) return 1.0f;
    return it->second->volume;
}

bool AudioEngine::hasBus(const std::string& name) const
{
    return m_impl->buses.count(name) > 0;
}

bool AudioEngine::removeBus(const std::string& name)
{
    if (!m_initialized) return false;
    auto it = m_impl->buses.find(name);
    if (it == m_impl->buses.end()) return false;

    // The voices first: a sound whose group is gone reads freed memory on the
    // next mix. release() detaches it from the graph before the group goes.
    size_t stopped = 0;
    for (auto s = m_impl->sounds.begin(); s != m_impl->sounds.end(); )
    {
        if (s->second->busName == name)
        {
            s->second->release();
            s = m_impl->sounds.erase(s);
            ++stopped;
        }
        else ++s;
    }
    if (it->second->groupOk) { ma_sound_group_uninit(&it->second->group); it->second->groupOk = false; }
    m_impl->buses.erase(it);
    HE_LOG_DEBUG(Audio, "Removed audio bus '%s' (%zu voice(s) stopped)", name.c_str(), stopped);
    return true;
}

std::vector<std::string> AudioEngine::busNames() const
{
    std::vector<std::string> names;
    names.reserve(m_impl->buses.size());
    for (const auto& [name, bus] : m_impl->buses) names.push_back(name);
    std::sort(names.begin(), names.end());
    return names;
}

void AudioEngine::setBusMuted(const std::string& name, bool muted)
{
    if (!m_initialized) return;
    auto it = m_impl->buses.find(name);
    if (it == m_impl->buses.end() || !it->second->groupOk) return;
    BusData& bus = *it->second;
    if (bus.muted == muted) return;
    bus.muted = muted;
    ma_sound_group_set_volume(&bus.group, muted ? 0.0f : bus.volume);
}

bool AudioEngine::isBusMuted(const std::string& name) const
{
    auto it = m_impl->buses.find(name);
    return it != m_impl->buses.end() && it->second->muted;
}

void AudioEngine::setMasterVolume(float volume)
{
    if (!m_initialized) return;
    m_impl->masterVolume = volume < 0.0f ? 0.0f : volume;
    if (!m_impl->masterMuted) ma_engine_set_volume(&m_impl->engine, m_impl->masterVolume);
}

float AudioEngine::getMasterVolume() const
{
    if (!m_initialized) return 1.0f;
    return m_impl->masterVolume;
}

void AudioEngine::setMasterMuted(bool muted)
{
    if (!m_initialized || m_impl->masterMuted == muted) return;
    m_impl->masterMuted = muted;
    ma_engine_set_volume(&m_impl->engine, muted ? 0.0f : m_impl->masterVolume);
}

bool AudioEngine::isMasterMuted() const
{
    return m_initialized && m_impl->masterMuted;
}

int AudioEngine::busVoiceCount(const std::string& name) const
{
    int n = 0;
    for (const auto& [handle, snd] : m_impl->sounds)
        if (snd->busName == name) ++n;
    return n;
}

void AudioEngine::applyBusConfig(const HE::AudioBusConfig& config)
{
    if (!m_initialized) return;
    setMasterVolume(config.masterVolume);
    for (const HE::AudioBusDef& def : config.buses)
    {
        if (!createBus(def.name, def.volume)) continue;   // createBus logged it
        setBusVolume(def.name, def.volume);               // an existing bus: only the volume
    }
}

// The falloff of a spatial voice, from startSound() and setSoundAttenuation()
// alike so the two cannot clamp differently. minDist is kept above zero (the
// inverse and exponential curves divide by it) and maxDist above minDist (a
// degenerate pair makes miniaudio attenuate nothing, which is not what a range
// of 0 means to anybody).
static void applyAttenuation(ActiveSound& snd, AudioAttenuation model,
                             float minDist, float maxDist, float rolloff)
{
    ma_attenuation_model m = ma_attenuation_model_linear;
    switch (model)
    {
    case AudioAttenuation::Linear:      m = ma_attenuation_model_linear;      break;
    case AudioAttenuation::Inverse:     m = ma_attenuation_model_inverse;     break;
    case AudioAttenuation::Exponential: m = ma_attenuation_model_exponential; break;
    case AudioAttenuation::None:        m = ma_attenuation_model_none;        break;
    }
    const float lo = minDist > 0.0f ? minDist : 0.01f;
    const float hi = maxDist > lo   ? maxDist : lo + 1.0f;
    snd.attenuation = model;
    ma_sound_set_attenuation_model(&snd.sound, m);
    ma_sound_set_min_distance(&snd.sound, lo);
    ma_sound_set_max_distance(&snd.sound, hi);
    ma_sound_set_rolloff(&snd.sound, rolloff > 0.0f ? rolloff : 0.0f);
}

// Everything the play variants have in common — the byte copy, the data source
// (PCM buffer or Vorbis decoder), the bus routing and the start. Only the
// spatialization flag and the positional setup below differ, so this is
// written once.
uint64_t AudioEngine::startSound(const std::vector<uint8_t>& bytes, AudioEncoding encoding,
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
    if (bytes.empty())
    {
        HE_LOG_WARN(Audio, "%s", "Sound requested with empty clip data — ignored "
                                 "(the clip asset probably failed to decode)");
        return 0;
    }

    auto snd = std::make_unique<ActiveSound>();
    snd->bytes = bytes;
    ma_uint64 frameCount = 0;   // for the trace line only; 0 = unknown (streamed)

    if (encoding == AudioEncoding::Vorbis)
    {
        // The Ogg pages stay compressed in `bytes`; ma_decoder pulls and decodes
        // just the frames the mixer asks for, on the mixer thread. Output format
        // f32 (all stb_vorbis can produce, and what the engine mixes in), rate and
        // channels left at 0 = the stream's own: a resampler INSIDE the decoder
        // would make cursor/length count engine frames and break the source-frame
        // contract in the header — the sound's own resampler handles the rate.
        ma_decoder_config dcfg = ma_decoder_config_init(ma_format_f32, 0, 0);
        dcfg.encodingFormat = ma_encoding_format_vorbis;
        const ma_result rc = ma_decoder_init_memory(snd->bytes.data(), snd->bytes.size(),
                                                    &dcfg, &snd->decoder);
        if (rc != MA_SUCCESS)
        {
            HE_LOG_ERROR(Audio, "Vorbis decoder init failed (%zu bytes, result %d) — "
                                "the clip is not a playable Ogg Vorbis stream",
                         snd->bytes.size(), static_cast<int>(rc));
            return 0;
        }
        snd->decoderOk = true;
        snd->source    = &snd->decoder;
        // Report what the stream says, not what the asset claims, so a stale
        // AUMI chunk cannot hide behind the numbers in the log.
        ma_uint32 rate = 0, ch = 0;
        ma_decoder_get_data_format(&snd->decoder, nullptr, &ch, &rate, nullptr, 0);
        sampleRate = static_cast<int>(rate);
        channels   = static_cast<int>(ch);
        ma_decoder_get_length_in_pcm_frames(&snd->decoder, &frameCount);
    }
    else
    {
        if (sampleRate <= 0 || channels <= 0)
        {
            HE_LOG_WARN(Audio, "Sound requested with invalid format (%d Hz, %d channel(s)) — ignored",
                        sampleRate, channels);
            return 0;
        }

        frameCount = snd->bytes.size() / (sizeof(int16_t) * static_cast<size_t>(channels));

        ma_audio_buffer_config bcfg = ma_audio_buffer_config_init(
            ma_format_s16,
            static_cast<ma_uint32>(channels),
            frameCount,
            snd->bytes.data(),
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
        snd->source   = &snd->buffer;
    }

    // Route through bus if found, otherwise null (master)
    ma_sound_group* busGroup = nullptr;
    if (!busName.empty()) {
        auto it = m_impl->buses.find(busName);
        if (it != m_impl->buses.end() && it->second->groupOk)
        {
            busGroup     = &it->second->group;
            snd->busName = busName;   // recorded only when the routing took
        }
        else
            HE_LOG_WARN(Audio, "Sound routed to unknown bus '%s' — playing on the master bus "
                               "(bus volume/mute will not apply)", busName.c_str());
    }

    // Spatial sounds pass no flag — positioning enabled.
    ma_uint32 flags = spatial ? 0u : MA_SOUND_FLAG_NO_SPATIALIZATION;
    if (ma_sound_init_from_data_source(&m_impl->engine,
                                        snd->source,
                                        flags, busGroup,
                                        &snd->sound) != MA_SUCCESS)
    {
        HE_LOG_ERROR(Audio, "%s", "Sound init from data source failed");
        snd->release();
        return 0;
    }
    snd->soundOk = true;

    ma_sound_set_volume(&snd->sound, volume);
    ma_sound_set_pitch(&snd->sound, pitch);
    ma_sound_set_looping(&snd->sound, loop ? MA_TRUE : MA_FALSE);
    if (spatial)
    {
        ma_sound_set_position(&snd->sound, spatial->x, spatial->y, spatial->z);
        snd->spatial = true;
        applyAttenuation(*snd, spatial->attenuation,
                         spatial->minDist, spatial->maxDist, spatial->rolloff);
    }

    if (ma_sound_start(&snd->sound) != MA_SUCCESS)
    {
        HE_LOG_ERROR(Audio, "%s", "ma_sound_start failed — sound will not be audible");
        snd->release();
        return 0;
    }

    uint64_t handle = m_nextHandle++;
    m_impl->sounds.emplace(handle, std::move(snd));

    HE_LOG_TRACE(Audio, "Started %s %s sound #%llu: %llu frames, %d Hz, %d ch, vol %.2f, "
                        "pitch %.2f%s, bus '%s'",
                 spatial ? "spatial" : "2D",
                 encoding == AudioEncoding::Vorbis ? "Vorbis (streamed)" : "PCM",
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
    return startSound(pcmData, AudioEncoding::PCM16, sampleRate, channels,
                      volume, pitch, loop, busName, nullptr);
}

uint64_t AudioEngine::play(const AudioAsset& clip,
                            float volume, float pitch, bool loop,
                            const std::string& busName)
{
    return startSound(clip.audioData, clip.encoding, clip.sampleRate, clip.channels,
                      volume, pitch, loop, busName, nullptr);
}

void AudioEngine::stop(uint64_t handle)
{
    auto it = m_impl->sounds.find(handle);
    if (it == m_impl->sounds.end()) return;
    it->second->release();
    m_impl->sounds.erase(it);
}

void AudioEngine::stopAll()
{
    for (auto& [handle, snd] : m_impl->sounds)
        snd->release();
    m_impl->sounds.clear();
}

uint64_t AudioEngine::playSpatial(const std::vector<uint8_t>& pcmData,
                                   int sampleRate, int channels,
                                   float volume, float pitch, bool loop,
                                   float x, float y, float z,
                                   float minDist, float maxDist,
                                   const std::string& busName,
                                   AudioAttenuation attenuation, float rolloff)
{
    const SpatialParams sp{ x, y, z, minDist, maxDist, attenuation, rolloff };
    return startSound(pcmData, AudioEncoding::PCM16, sampleRate, channels,
                      volume, pitch, loop, busName, &sp);
}

uint64_t AudioEngine::playSpatial(const AudioAsset& clip,
                                   float volume, float pitch, bool loop,
                                   float x, float y, float z,
                                   float minDist, float maxDist,
                                   const std::string& busName,
                                   AudioAttenuation attenuation, float rolloff)
{
    const SpatialParams sp{ x, y, z, minDist, maxDist, attenuation, rolloff };
    return startSound(clip.audioData, clip.encoding, clip.sampleRate, clip.channels,
                      volume, pitch, loop, busName, &sp);
}

void AudioEngine::setSoundPosition(uint64_t handle, float x, float y, float z)
{
    auto it = m_impl->sounds.find(handle);
    if (it == m_impl->sounds.end()) return;
    if (it->second->soundOk)
        ma_sound_set_position(&it->second->sound, x, y, z);
}

void AudioEngine::setSoundAttenuation(uint64_t handle, AudioAttenuation model,
                                      float minDist, float maxDist, float rolloff)
{
    auto it = m_impl->sounds.find(handle);
    if (it == m_impl->sounds.end() || !it->second->soundOk || !it->second->spatial) return;
    applyAttenuation(*it->second, model, minDist, maxDist, rolloff);
}

AudioAttenuation AudioEngine::getSoundAttenuation(uint64_t handle) const
{
    auto it = m_impl->sounds.find(handle);
    if (it == m_impl->sounds.end()) return AudioAttenuation::Linear;
    return it->second->attenuation;
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
    // A voice that is not running — finished, or already paused — has nothing
    // to hold; marking it paused would make isPaused() claim a finished sound
    // is merely waiting. at_end covers the one mixer period between the last
    // frame and miniaudio actually flipping the node to stopped.
    if (ma_sound_is_playing(&it->second->sound) == MA_FALSE ||
        ma_sound_at_end(&it->second->sound) == MA_TRUE) return;
    // ma_sound_stop only halts playback — the voice, its buffer and its cursor
    // all stay put, which is what makes resumeSound() free.
    ma_sound_stop(&it->second->sound);
    it->second->paused = true;
}

void AudioEngine::resumeSound(uint64_t handle)
{
    auto it = m_impl->sounds.find(handle);
    if (it == m_impl->sounds.end() || !it->second->soundOk) return;
    ma_sound_start(&it->second->sound);
    it->second->paused = false;
}

bool AudioEngine::isPaused(uint64_t handle) const
{
    auto it = m_impl->sounds.find(handle);
    return it != m_impl->sounds.end() && it->second->paused;
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

float AudioEngine::getSoundVolume(uint64_t handle) const
{
    auto it = m_impl->sounds.find(handle);
    if (it == m_impl->sounds.end() || !it->second->soundOk) return 0.0f;
    return ma_sound_get_volume(&it->second->sound);
}

float AudioEngine::getSoundPitch(uint64_t handle) const
{
    auto it = m_impl->sounds.find(handle);
    if (it == m_impl->sounds.end() || !it->second->soundOk) return 1.0f;
    return ma_sound_get_pitch(&it->second->sound);
}

int AudioEngine::getSoundSampleRate(uint64_t handle) const
{
    auto it = m_impl->sounds.find(handle);
    if (it == m_impl->sounds.end() || !it->second->source) return 0;

    // Query through the data-source interface — the exact same call miniaudio makes
    // internally in ma_sound_init_from_data_source() to pick the resampler ratio.
    ma_uint32 rate = 0;
    if (ma_data_source_get_data_format(it->second->source, nullptr, nullptr,
                                        &rate, nullptr, 0) != MA_SUCCESS)
        return 0;
    return static_cast<int>(rate);
}

// ─── Headless mix pull ─────────────────────────────────────────────────────────

uint64_t AudioEngine::readMixedFrames(float* out, uint64_t frameCount)
{
    if (!m_initialized || !out || frameCount == 0) return 0;
    ma_uint64 read = 0;
    if (ma_engine_read_pcm_frames(&m_impl->engine, out, frameCount, &read) != MA_SUCCESS)
        return 0;
    return static_cast<uint64_t>(read);
}

int AudioEngine::outputChannels() const
{
    return m_initialized ? static_cast<int>(ma_engine_get_channels(&m_impl->engine)) : 0;
}

// ─── Offline decode ────────────────────────────────────────────────────────────

bool AudioEngine::decodeToPcm16(const AudioAsset& clip, std::vector<uint8_t>& outPcm)
{
    outPcm.clear();
    if (clip.audioData.empty()) return false;

    if (clip.encoding == AudioEncoding::PCM16)
    {
        outPcm = clip.audioData;
        return true;
    }

    // Native rate/channels (0 = keep), s16 out: one allocation of the whole clip.
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_s16, 0, 0);
    cfg.encodingFormat = ma_encoding_format_vorbis;
    ma_uint64 frames = 0;
    void*     pcm    = nullptr;
    const ma_result rc = ma_decode_memory(clip.audioData.data(), clip.audioData.size(),
                                          &cfg, &frames, &pcm);
    if (rc != MA_SUCCESS || !pcm)
    {
        HE_LOG_ERROR(Audio, "Vorbis decode of '%s' failed (%zu bytes, result %d)",
                     clip.name.c_str(), clip.audioData.size(), static_cast<int>(rc));
        if (pcm) ma_free(pcm, nullptr);
        return false;
    }
    // ma_decode_memory writes the stream's channel count back into cfg.
    const size_t bytes = static_cast<size_t>(frames) * cfg.channels * sizeof(int16_t);
    const auto* p = static_cast<const uint8_t*>(pcm);
    outPcm.assign(p, p + bytes);
    ma_free(pcm, nullptr);
    return true;
}
