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
#include <atomic>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <cstring>
#include <string>

// A scene that keeps starting sounds without ever stopping them (a looping clip
// re-triggered every frame) leaks voices until the mixer chokes. Warn once the
// count gets unreasonable rather than letting the audio quietly fall apart.
static constexpr size_t kVoiceWarnThreshold = 128;

// ─── Volume curve stage ──────────────────────────────────────────────────────
// A clip's volume curve (AudioEdit::envelope) as a miniaudio data source in
// front of the voice's own (the PCM buffer or the Vorbis decoder): it reads the
// inner source, converts to f32 and multiplies by AudioEnvelope::apply. The
// ma_sound reads this one instead, so everything miniaudio does on top — the
// resampler, pitch, volume, spatialisation, the bus — sees the curved signal.
//
// Range and looping stay on the INNER source: its range is the trim, and this
// stage reports its cursor and length unchanged, so the cursor/length/seek
// contract of the header holds with or without a curve. Looping is driven from
// out here (miniaudio's ma_data_source_read_pcm_frames seeks this source back
// to 0 at the inner's end, and onSeek passes that on), which is also why the
// gain must be a pure function of the frame: after the wrap the next read asks
// the inner cursor and lands on the curve's start again.
//
// The same stage runs the clip's EQ (AudioEdit::eq), after the curve: one
// stage in front of the voice rather than two, so a clip with either gets one
// extra read and a clip with neither gets none. It filters at the INNER
// source's rate — the clip's own, before miniaudio's resampler — so the
// coefficients are computed for that rate (AudioEdit.h says so), and a voice
// pitched up an octave hears its EQ an octave up with it, like a filter baked
// into the clip would be.

// The EQ as the mixer thread runs it: coefficients computed on the caller's
// thread (biquadCoefficients is trig — not for the audio thread), identity
// bands left out, so an EQ whose bands are all at 0 dB costs a copy of nothing.
struct EqChain
{
    std::vector<HE::BiquadCoeffs> bands;
};

static std::shared_ptr<const EqChain> makeEqChain(const HE::AudioEq& eq, double sampleRate)
{
    if (eq.isNeutral()) return nullptr;
    auto chain = std::make_shared<EqChain>();
    for (const HE::AudioEqBand& b : eq.bands)
    {
        if (chain->bands.size() >= HE::AudioEq::kMaxBands) break;
        const HE::BiquadCoeffs c = HE::biquadCoefficients(b, sampleRate);
        if (!c.isIdentity()) chain->bands.push_back(c);
    }
    if (chain->bands.empty()) return nullptr;
    return chain;
}

// Run `eq` over `frames` interleaved frames in place. `state` holds kMaxBands
// filters per channel, band-major; it is flushed of denormals afterwards — a
// filter ringing out into silence decays towards them, and they cost x86 a
// hundred times a normal multiply.
static void runEq(const EqChain& eq, std::vector<HE::BiquadState>& state, float* f,
                  uint64_t frames, uint32_t ch)
{
    for (size_t b = 0; b < eq.bands.size(); ++b)
    {
        const HE::BiquadCoeffs& c  = eq.bands[b];
        HE::BiquadState*        st = state.data() + b * ch;
        for (uint64_t i = 0; i < frames; ++i)
            for (uint32_t k = 0; k < ch; ++k)
                f[i * ch + k] = st[k].process(c, f[i * ch + k]);
    }
    for (HE::BiquadState& s : state)
    {
        if (std::fabs(s.z1) < 1.0e-20) s.z1 = 0.0;
        if (std::fabs(s.z2) < 1.0e-20) s.z2 = 0.0;
    }
}

static bool sameEq(const HE::AudioEq& a, const HE::AudioEq& b)
{
    if (a.enabled != b.enabled || a.bands.size() != b.bands.size()) return false;
    for (size_t i = 0; i < a.bands.size(); ++i)
    {
        const HE::AudioEqBand& x = a.bands[i];
        const HE::AudioEqBand& y = b.bands[i];
        if (x.type != y.type || x.freqHz != y.freqHz || x.gainDb != y.gainDb ||
            x.q != y.q || x.enabled != y.enabled)
            return false;
    }
    return true;
}

// ─── Bus EQ ──────────────────────────────────────────────────────────────────
// A mixer bus's EQ (AudioBusDef::eq) as a node of miniaudio's graph between the
// bus's group and the endpoint: group → this → endpoint. It filters the summed
// bus at the mixer's rate, after every voice on it has been resampled, panned
// and mixed. Inserted only when a bus first gets a non-neutral EQ; a bus that
// never had one keeps its group wired straight to the endpoint. Once in, it
// stays until the bus goes; with a neutral EQ it copies its input unchanged.
struct BusEqNode
{
    ma_node_base                   base;     // first: miniaudio hands &base back
    bool                           nodeOk   = false;
    ma_uint32                      channels = 0;
    // Same handover as a voice's EQ (EnvelopeSource): the mixer thread swaps
    // `pending` in with try_lock and never frees a chain itself.
    std::shared_ptr<const EqChain> eq;
    std::shared_ptr<const EqChain> pending;
    std::mutex                     pendingMutex;
    std::atomic<bool>              hasPending{ false };
    std::vector<HE::BiquadState>   state;
};

static void busEqProcess(ma_node* node, const float** in, ma_uint32* /*inCount*/,
                         float** out, ma_uint32* outCount)
{
    BusEqNode& n = *reinterpret_cast<BusEqNode*>(node);
    if (n.hasPending.load(std::memory_order_acquire))
    {
        std::unique_lock<std::mutex> lock(n.pendingMutex, std::try_to_lock);
        if (lock.owns_lock())
        {
            std::swap(n.eq, n.pending);
            n.hasPending.store(false, std::memory_order_release);
        }
    }
    // Input and output run at one rate, so the output count is the frame count.
    const ma_uint32 frames = *outCount;
    std::memcpy(out[0], in[0], sizeof(float) * frames * n.channels);
    if (n.eq) runEq(*n.eq, n.state, out[0], frames, n.channels);
}

static ma_node_vtable g_busEqVtable = {
    busEqProcess,
    nullptr,   // same rate in and out: no input-count callback
    1, 1,      // one bus in (the group), one out (to the endpoint)
    0
};

struct EnvelopeSource;
struct EnvelopeNode
{
    ma_data_source_base base;     // first: miniaudio hands &base back to the callbacks
    EnvelopeSource*     owner;
};

struct EnvelopeSource
{
    EnvelopeNode     node{};
    bool             nodeOk      = false;
    ma_data_source*  inner       = nullptr;
    ma_format        innerFormat = ma_format_unknown;
    ma_uint32        channels    = 0;
    ma_uint32        sampleRate  = 0;
    // Original-clip frame of the inner source's frame 0: 0 for an asset (its
    // bytes ARE the clip), the selection's start for the editor's preview copy.
    uint64_t         frameOffset = 0;

    // The curve the mixer thread applies. Only the mixer thread touches
    // `curve`; setSoundEnvelope parks a new one in `pending` and the next read
    // takes it, without ever waiting for the lock (try_lock — a read that
    // misses it keeps the old curve for one more period).
    std::shared_ptr<const HE::AudioEnvelope> curve;
    std::mutex                               pendingMutex;
    std::shared_ptr<const HE::AudioEnvelope> pending;
    std::atomic<bool>                        hasPending{ false };

    // The EQ, handed over the same way as the curve (its own pending slot, so a
    // curve edit and an EQ edit in one frame do not overwrite each other).
    // `eqState` is the filter memory, kMaxBands per channel, band-major, sized
    // once at start: it is kept across a swap, so dragging a band while the
    // clip plays changes the sound without resetting the filter into a click.
    std::shared_ptr<const EqChain> eq;
    std::shared_ptr<const EqChain> eqPending;
    std::atomic<bool>              hasEqPending{ false };
    std::vector<HE::BiquadState>   eqState;

    // Inner-format frames for one chunk of a read; sized once at start so the
    // mixer thread never allocates. Unused when the inner source is already f32.
    static constexpr ma_uint64 kChunkFrames = 512;
    std::vector<uint8_t>       scratch;

    void release()
    {
        if (nodeOk) { ma_data_source_uninit(&node.base); nodeOk = false; }
    }
    ~EnvelopeSource() { release(); }
};

static EnvelopeSource& envOf(ma_data_source* ds)
{
    return *reinterpret_cast<EnvelopeNode*>(ds)->owner;
}

static ma_result envRead(ma_data_source* ds, void* out, ma_uint64 frameCount, ma_uint64* framesRead)
{
    EnvelopeSource& e = envOf(ds);
    if (framesRead) *framesRead = 0;

    if (e.hasPending.load(std::memory_order_acquire))
    {
        std::unique_lock<std::mutex> lock(e.pendingMutex, std::try_to_lock);
        if (lock.owns_lock())
        {
            // Swapped, not moved: the old curve stays parked in `pending` and is
            // freed by the next setSoundEnvelope on the caller's thread, so the
            // mixer thread never runs a deallocation.
            std::swap(e.curve, e.pending);
            e.hasPending.store(false, std::memory_order_release);
        }
    }
    if (e.hasEqPending.load(std::memory_order_acquire))
    {
        std::unique_lock<std::mutex> lock(e.pendingMutex, std::try_to_lock);
        if (lock.owns_lock())
        {
            std::swap(e.eq, e.eqPending);   // the old chain is freed by the caller, as above
            e.hasEqPending.store(false, std::memory_order_release);
        }
    }

    // Where in the ORIGINAL clip this read starts: the inner cursor counts from
    // its range (the trim), so the range's start goes back on.
    ma_uint64 cursor = 0, rangeBeg = 0, rangeEnd = 0;
    ma_data_source_get_cursor_in_pcm_frames(e.inner, &cursor);
    ma_data_source_get_range_in_pcm_frames(e.inner, &rangeBeg, &rangeEnd);
    const uint64_t first = e.frameOffset + rangeBeg + cursor;

    const HE::AudioEnvelope* curve = e.curve.get();
    const EqChain*           eq    = e.eq.get();
    float*    dst   = static_cast<float*>(out);
    ma_uint64 total = 0;
    ma_result rc    = MA_SUCCESS;
    while (total < frameCount)
    {
        const bool direct = e.innerFormat == ma_format_f32 || dst == nullptr;
        const ma_uint64 want = direct ? frameCount - total
                                      : std::min<ma_uint64>(frameCount - total, EnvelopeSource::kChunkFrames);
        ma_uint64 got = 0;
        void* target = dst == nullptr ? nullptr
                     : direct         ? static_cast<void*>(dst + total * e.channels)
                                      : static_cast<void*>(e.scratch.data());
        rc = ma_data_source_read_pcm_frames(e.inner, target, want, &got);
        if (dst && got > 0)
        {
            float* f = dst + total * e.channels;
            if (!direct)
                ma_pcm_convert(f, ma_format_f32, e.scratch.data(), e.innerFormat,
                               got * e.channels, ma_dither_mode_none);
            if (curve)
                curve->apply(f, got, static_cast<int>(e.channels), first + total,
                             static_cast<double>(e.sampleRate));
            if (eq)
                runEq(*eq, e.eqState, f, got, e.channels);
        }
        total += got;
        if (rc != MA_SUCCESS || got < want) break;
    }
    if (framesRead) *framesRead = total;
    if (rc == MA_SUCCESS && total == 0) rc = MA_AT_END;
    return rc;
}

static ma_result envSeek(ma_data_source* ds, ma_uint64 frame)
{
    return ma_data_source_seek_to_pcm_frame(envOf(ds).inner, frame);
}

static ma_result envFormat(ma_data_source* ds, ma_format* format, ma_uint32* channels,
                           ma_uint32* sampleRate, ma_channel* channelMap, size_t channelMapCap)
{
    EnvelopeSource& e = envOf(ds);
    if (format)     *format     = ma_format_f32;
    if (channels)   *channels   = e.channels;
    if (sampleRate) *sampleRate = e.sampleRate;
    if (channelMap && channelMapCap > 0)
        ma_data_source_get_data_format(e.inner, nullptr, nullptr, nullptr, channelMap, channelMapCap);
    return MA_SUCCESS;
}

static ma_result envCursor(ma_data_source* ds, ma_uint64* cursor)
{
    return ma_data_source_get_cursor_in_pcm_frames(envOf(ds).inner, cursor);
}

static ma_result envLength(ma_data_source* ds, ma_uint64* length)
{
    return ma_data_source_get_length_in_pcm_frames(envOf(ds).inner, length);
}

static ma_data_source_vtable g_envelopeVtable = {
    envRead, envSeek, envFormat, envCursor, envLength,
    nullptr,   // onSetLooping: the base keeps the flag, the read loop above acts on it
    0
};

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
    // The volume-curve/EQ stage in front of `source`, when the voice has one
    // (EnvelopeSource above); the sound then reads from it instead.
    std::unique_ptr<EnvelopeSource> env;
    // Whether the stage runs a (non-neutral) EQ right now, as the caller last
    // set it — what hasSoundEq answers without touching the mixer's copy.
    bool                 eqActive    = false;

    ma_data_source* voiceSource() { return env ? &env->node.base : source; }

    // Order matters: the sound reads from the curve stage, the curve stage from
    // the data source, so they go in that order.
    void release()
    {
        if (soundOk)   { ma_sound_stop(&sound); ma_sound_uninit(&sound); soundOk = false; }
        env.reset();
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
    // The bus's EQ as last set (setBusEq compares against it, so the mixer
    // re-applying the project every frame costs a compare), whether it is
    // filtering, and the node that runs it once there has been one.
    HE::AudioEq                eq;
    bool                       eqActive = false;
    std::unique_ptr<BusEqNode> eqNode;

    // Group first — it is what feeds the node — then the node.
    void release()
    {
        if (groupOk) { ma_sound_group_uninit(&group); groupOk = false; }
        if (eqNode && eqNode->nodeOk) { ma_node_uninit(&eqNode->base, nullptr); eqNode->nodeOk = false; }
        eqNode.reset();
    }
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
        bus->release();
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
    it->second->release();
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
        setBusEq(def.name, def.eq);                       // a compare when unchanged
    }
}

bool AudioEngine::setBusEq(const std::string& name, const HE::AudioEq& eq)
{
    if (!m_initialized) return false;
    auto it = m_impl->buses.find(name);
    if (it == m_impl->buses.end() || !it->second->groupOk) return false;
    BusData& bus = *it->second;
    if (sameEq(bus.eq, eq)) return true;
    bus.eq = eq;

    std::shared_ptr<const EqChain> chain =
        makeEqChain(eq, static_cast<double>(ma_engine_get_sample_rate(&m_impl->engine)));
    bus.eqActive = chain != nullptr;
    if (!bus.eqNode)
    {
        if (!chain) return true;   // neutral, and never filtered: the group stays wired as it is
        auto node = std::make_unique<BusEqNode>();
        node->channels = ma_engine_get_channels(&m_impl->engine);
        node->state.assign(HE::AudioEq::kMaxBands * node->channels, HE::BiquadState{});
        node->eq = chain;   // no thread sees the node yet
        ma_node_config ncfg = ma_node_config_init();
        ncfg.vtable          = &g_busEqVtable;
        ncfg.pInputChannels  = &node->channels;
        ncfg.pOutputChannels = &node->channels;
        if (ma_node_init(ma_engine_get_node_graph(&m_impl->engine), &ncfg, nullptr, &node->base) != MA_SUCCESS)
        {
            bus.eqActive = false;
            HE_LOG_ERROR(Audio, "Could not set up the EQ of bus '%s' — it plays unfiltered", name.c_str());
            return false;
        }
        node->nodeOk = true;
        // Downstream first, so the group is never attached to a node that leads
        // nowhere; re-attaching the group's output detaches it from the endpoint.
        ma_node_attach_output_bus(&node->base, 0, ma_engine_get_endpoint(&m_impl->engine), 0);
        ma_node_attach_output_bus(&bus.group, 0, &node->base, 0);
        bus.eqNode = std::move(node);
        HE_LOG_DEBUG(Audio, "Bus '%s' now runs through an EQ", name.c_str());
        return true;
    }
    BusEqNode& n = *bus.eqNode;
    std::lock_guard<std::mutex> lock(n.pendingMutex);
    n.pending = std::move(chain);
    n.hasPending.store(true, std::memory_order_release);
    return true;
}

bool AudioEngine::hasBusEq(const std::string& name) const
{
    auto it = m_impl->buses.find(name);
    return it != m_impl->buses.end() && it->second->eqActive;
}

int AudioEngine::outputSampleRate() const
{
    return m_initialized ? static_cast<int>(ma_engine_get_sample_rate(&m_impl->engine)) : 0;
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
                                  const SpatialParams* spatial,
                                  const HE::AudioTrim* trim,
                                  const HE::AudioEnvelope* envelope,
                                  uint64_t envelopeOffset,
                                  const HE::AudioEq* eq)
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

    // The asset's trim: the data source's range, which miniaudio honours for the
    // PCM buffer and the Vorbis decoder alike — reading, looping (loop points are
    // relative to the range) and seeking all stay inside it, and cursor/length
    // count from its start. Set before the sound exists, so not a frame outside
    // the range is ever mixed. A Vorbis stream whose length the decoder cannot
    // tell (0) keeps "to the end" as an open end rather than resolving against 0.
    bool trimmed = false;
    if (trim && !trim->isDefault())
    {
        ma_uint64 beg = trim->startFrame;
        ma_uint64 end = trim->endFrame == 0 ? ~static_cast<ma_uint64>(0) : trim->endFrame;
        if (frameCount > 0)
        {
            const HE::AudioTrim::Range r = trim->resolve(frameCount);
            beg = r.begin;
            end = r.end;
        }
        trimmed = ma_data_source_set_range_in_pcm_frames(snd->source, beg, end) == MA_SUCCESS;
        if (!trimmed)
            HE_LOG_WARN(Audio, "Could not apply the clip's trim [%llu, %llu) — playing it whole",
                        static_cast<unsigned long long>(beg), static_cast<unsigned long long>(end));
    }

    // The volume curve and the EQ, as a stage the sound reads through
    // (EnvelopeSource). Built after the trim so it sees the trimmed inner source.
    if (envelope || eq)
    {
        auto env = std::make_unique<EnvelopeSource>();
        env->inner       = snd->source;
        env->frameOffset = envelopeOffset;
        ma_data_source_get_data_format(snd->source, &env->innerFormat, &env->channels,
                                       &env->sampleRate, nullptr, 0);
        if (envelope && !envelope->empty())
            env->curve = std::make_shared<const HE::AudioEnvelope>(*envelope);
        if (eq)
            env->eq = makeEqChain(*eq, static_cast<double>(env->sampleRate));
        env->eqState.assign(HE::AudioEq::kMaxBands * env->channels, HE::BiquadState{});
        env->scratch.resize(static_cast<size_t>(EnvelopeSource::kChunkFrames) * env->channels *
                            ma_get_bytes_per_sample(env->innerFormat));
        env->node.owner = env.get();
        ma_data_source_config dscfg = ma_data_source_config_init();
        dscfg.vtable = &g_envelopeVtable;
        if (env->channels == 0 || env->sampleRate == 0 ||
            ma_data_source_init(&dscfg, &env->node.base) != MA_SUCCESS)
        {
            HE_LOG_ERROR(Audio, "%s", "Could not set up the clip's volume curve — playing it without");
        }
        else
        {
            env->nodeOk   = true;
            snd->eqActive = env->eq != nullptr;
            snd->env      = std::move(env);
        }
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
                                        snd->voiceSource(),
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
    const bool snd_hasCurve = snd->env && snd->env->curve;
    const bool snd_hasEq    = snd->eqActive;
    m_impl->sounds.emplace(handle, std::move(snd));

    HE_LOG_TRACE(Audio, "Started %s %s sound #%llu: %llu frames%s, %d Hz, %d ch, vol %.2f, "
                        "pitch %.2f%s%s%s, bus '%s'",
                 spatial ? "spatial" : "2D",
                 encoding == AudioEncoding::Vorbis ? "Vorbis (streamed)" : "PCM",
                 static_cast<unsigned long long>(handle),
                 static_cast<unsigned long long>(frameCount), trimmed ? " (trimmed)" : "",
                 sampleRate, channels,
                 volume, pitch, loop ? ", looping" : "",
                 snd_hasCurve ? ", volume curve" : "",
                 snd_hasEq ? ", EQ" : "",
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

std::string AudioEngine::routeFor(const std::string& requestedBus, const std::string& assetBus) const
{
    return HE::AudioEdit::resolveBus(requestedBus, assetBus,
                                     [this](const std::string& n) { return hasBus(n); });
}

// The bus an asset voice plays through, with the reason in the log when a name
// did not resolve — the fallback is silent in the mix, so it must not be in the
// log too. Throttled: a source re-triggered every frame would flood it.
static std::string routeAsset(const AudioEngine& engine, const std::string& requestedBus,
                              const AudioAsset& clip)
{
    const std::string bus = engine.routeFor(requestedBus, clip.edit.bus);
    if (!requestedBus.empty() && bus != requestedBus)
        HE_LOG_THROTTLE(Audio, Warning, 5.0, "Sound routed to unknown bus '%s' — playing on %s%s%s",
                        requestedBus.c_str(), bus.empty() ? "the master bus" : "the clip's bus '",
                        bus.c_str(), bus.empty() ? "" : "'");
    else if (requestedBus.empty() && !clip.edit.bus.empty() && bus.empty())
        HE_LOG_THROTTLE(Audio, Warning, 5.0, "Clip '%s' names bus '%s', which the mixer does not have "
                        "(renamed or removed?) — playing on the master bus",
                        clip.name.c_str(), clip.edit.bus.c_str());
    return bus;
}

uint64_t AudioEngine::play(const AudioAsset& clip,
                            float volume, float pitch, bool loop,
                            const std::string& busName)
{
    return startSound(clip.audioData, clip.encoding, clip.sampleRate, clip.channels,
                      volume, pitch, loop, routeAsset(*this, busName, clip), nullptr, &clip.edit.trim,
                      clip.edit.envelope.empty() ? nullptr : &clip.edit.envelope, 0,
                      clip.edit.eq.isNeutral() ? nullptr : &clip.edit.eq);
}

uint64_t AudioEngine::play(const std::vector<uint8_t>& pcmData, int sampleRate, int channels,
                           const HE::AudioEnvelope& envelope, uint64_t envelopeFrameOffset,
                           float volume, float pitch, bool loop, const std::string& busName,
                           const HE::AudioEq* eq)
{
    // The stage is always in (envelope non-null), the EQ in it when one is
    // given — setSoundEq can put one in later either way.
    return startSound(pcmData, AudioEncoding::PCM16, sampleRate, channels,
                      volume, pitch, loop, busName, nullptr, nullptr,
                      &envelope, envelopeFrameOffset, eq);
}

bool AudioEngine::setSoundEq(uint64_t handle, const HE::AudioEq& eq)
{
    auto it = m_impl->sounds.find(handle);
    if (it == m_impl->sounds.end() || !it->second->env) return false;
    EnvelopeSource& e = *it->second->env;
    std::shared_ptr<const EqChain> next = makeEqChain(eq, static_cast<double>(e.sampleRate));
    it->second->eqActive = next != nullptr;
    std::lock_guard<std::mutex> lock(e.pendingMutex);
    e.eqPending = std::move(next);
    e.hasEqPending.store(true, std::memory_order_release);
    return true;
}

bool AudioEngine::hasSoundEq(uint64_t handle) const
{
    auto it = m_impl->sounds.find(handle);
    return it != m_impl->sounds.end() && it->second->eqActive;
}

std::string AudioEngine::getSoundBus(uint64_t handle) const
{
    auto it = m_impl->sounds.find(handle);
    return it == m_impl->sounds.end() ? std::string() : it->second->busName;
}

bool AudioEngine::setSoundEnvelope(uint64_t handle, const HE::AudioEnvelope& envelope)
{
    auto it = m_impl->sounds.find(handle);
    if (it == m_impl->sounds.end() || !it->second->env) return false;
    EnvelopeSource& e = *it->second->env;
    std::shared_ptr<const HE::AudioEnvelope> next;
    if (!envelope.empty()) next = std::make_shared<const HE::AudioEnvelope>(envelope);
    std::lock_guard<std::mutex> lock(e.pendingMutex);
    e.pending = std::move(next);
    e.hasPending.store(true, std::memory_order_release);
    return true;
}

bool AudioEngine::hasSoundEnvelope(uint64_t handle) const
{
    auto it = m_impl->sounds.find(handle);
    return it != m_impl->sounds.end() && it->second->env != nullptr;
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
                      volume, pitch, loop, routeAsset(*this, busName, clip), &sp, &clip.edit.trim,
                      clip.edit.envelope.empty() ? nullptr : &clip.edit.envelope, 0,
                      clip.edit.eq.isNeutral() ? nullptr : &clip.edit.eq);
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
