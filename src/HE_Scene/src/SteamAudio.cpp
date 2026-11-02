#include "HorizonScene/SteamAudio.h"

#include <Diagnostics/Log.h>

#include <algorithm>
#include <cmath>
#include <vector>

#if defined(HE_HAVE_STEAM_AUDIO)
#include <phonon.h>
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif
#endif

namespace HE {

// ─── Math (always compiled) ──────────────────────────────────────────────────

namespace {

struct V3 { float x, y, z; };
V3    sub(V3 a, V3 b)   { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
float dot(V3 a, V3 b)   { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3    cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
float len(V3 a)         { return std::sqrt(dot(a, a)); }
V3    scaled(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }

} // namespace

SpatialDirection listenerRelativeDirection(float dx, float dy, float dz,
                                           float fx, float fy, float fz,
                                           float ux, float uy, float uz)
{
    V3 ahead = { fx, fy, fz };
    V3 up    = { ux, uy, uz };
    if (len(ahead) < 1e-6f) ahead = { 0.0f, 0.0f, -1.0f };
    ahead = scaled(ahead, 1.0f / len(ahead));
    V3 right = cross(ahead, up);
    if (len(right) < 1e-6f)
    {
        // Up is zero or parallel to ahead: the frame is undefined. The world
        // axes are the least surprising answer and keep the maths finite.
        up    = { 0.0f, 1.0f, 0.0f };
        right = cross(ahead, up);
        if (len(right) < 1e-6f) { ahead = { 0.0f, 0.0f, -1.0f }; right = { 1.0f, 0.0f, 0.0f }; }
    }
    right = scaled(right, 1.0f / len(right));
    up    = cross(right, ahead);   // re-orthogonalised

    const V3 d = { dx, dy, dz };
    if (len(d) < 1e-6f) return { 0.0f, 0.0f, -1.0f };
    return { dot(d, right), dot(d, up), -dot(d, ahead) };
}

#if defined(HE_HAVE_STEAM_AUDIO)

// ─── Library loading ─────────────────────────────────────────────────────────

namespace {

#if defined(_WIN32)
constexpr const char* kLibName = "phonon.dll";
#elif defined(__APPLE__)
constexpr const char* kLibName = "libphonon.dylib";
#else
constexpr const char* kLibName = "libphonon.so";
#endif

using LibHandle = void*;

LibHandle openLibrary(const std::string& path)
{
#if defined(_WIN32)
    return reinterpret_cast<LibHandle>(LoadLibraryA(path.c_str()));
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void closeLibrary(LibHandle h)
{
    if (!h) return;
#if defined(_WIN32)
    FreeLibrary(reinterpret_cast<HMODULE>(h));
#else
    dlclose(h);
#endif
}

void* findSymbol(LibHandle h, const char* name)
{
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(h), name));
#else
    return dlsym(h, name);
#endif
}

// The directory the engine library (the one holding this code) lives in; the
// game ships libphonon right beside it.
std::string moduleDirectory()
{
    std::string path;
#if defined(_WIN32)
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&moduleDirectory), &mod))
    {
        char buf[MAX_PATH * 4] = {};
        const DWORD n = GetModuleFileNameA(mod, buf, static_cast<DWORD>(sizeof(buf)));
        if (n > 0 && n < sizeof(buf)) path.assign(buf, n);
    }
    const size_t slash = path.find_last_of("\\/");
#else
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&moduleDirectory), &info) && info.dli_fname)
        path = info.dli_fname;
    const size_t slash = path.find_last_of('/');
#endif
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

// The SDK entry points this backend uses, resolved by name.
struct Api
{
    decltype(&iplContextCreate)         contextCreate = nullptr;
    decltype(&iplContextRelease)        contextRelease = nullptr;
    decltype(&iplHRTFCreate)            hrtfCreate = nullptr;
    decltype(&iplHRTFRelease)           hrtfRelease = nullptr;
    decltype(&iplBinauralEffectCreate)  effectCreate = nullptr;
    decltype(&iplBinauralEffectRelease) effectRelease = nullptr;
    decltype(&iplBinauralEffectApply)   effectApply = nullptr;
    decltype(&iplBinauralEffectReset)   effectReset = nullptr;
    decltype(&iplDirectEffectCreate)    directCreate = nullptr;
    decltype(&iplDirectEffectRelease)   directRelease = nullptr;
    decltype(&iplDirectEffectApply)     directApply = nullptr;
    decltype(&iplDirectEffectReset)     directReset = nullptr;
    decltype(&iplAirAbsorptionCalculate) airCalc = nullptr;
    decltype(&iplReflectionEffectCreate)  reflCreate = nullptr;
    decltype(&iplReflectionEffectRelease) reflRelease = nullptr;
    decltype(&iplReflectionEffectApply)   reflApply = nullptr;
    decltype(&iplReflectionEffectReset)   reflReset = nullptr;

    bool load(LibHandle h, std::string& error)
    {
        const struct { void** slot; const char* name; } table[] = {
            { reinterpret_cast<void**>(&contextCreate),  "iplContextCreate" },
            { reinterpret_cast<void**>(&contextRelease), "iplContextRelease" },
            { reinterpret_cast<void**>(&hrtfCreate),     "iplHRTFCreate" },
            { reinterpret_cast<void**>(&hrtfRelease),    "iplHRTFRelease" },
            { reinterpret_cast<void**>(&effectCreate),   "iplBinauralEffectCreate" },
            { reinterpret_cast<void**>(&effectRelease),  "iplBinauralEffectRelease" },
            { reinterpret_cast<void**>(&effectApply),    "iplBinauralEffectApply" },
            { reinterpret_cast<void**>(&effectReset),    "iplBinauralEffectReset" },
            { reinterpret_cast<void**>(&directCreate),   "iplDirectEffectCreate" },
            { reinterpret_cast<void**>(&directRelease),  "iplDirectEffectRelease" },
            { reinterpret_cast<void**>(&directApply),    "iplDirectEffectApply" },
            { reinterpret_cast<void**>(&directReset),    "iplDirectEffectReset" },
            { reinterpret_cast<void**>(&airCalc),        "iplAirAbsorptionCalculate" },
            { reinterpret_cast<void**>(&reflCreate),     "iplReflectionEffectCreate" },
            { reinterpret_cast<void**>(&reflRelease),    "iplReflectionEffectRelease" },
            { reinterpret_cast<void**>(&reflApply),      "iplReflectionEffectApply" },
            { reinterpret_cast<void**>(&reflReset),      "iplReflectionEffectReset" },
        };
        for (const auto& e : table)
        {
            *e.slot = findSymbol(h, e.name);
            if (!*e.slot) { error = std::string("symbol ") + e.name + " not found in " + kLibName; return false; }
        }
        return true;
    }
};

} // namespace

// The loaded library, the context and the HRTF. Shared by SteamAudio and every
// Binaural it made, so the library cannot be unloaded under a live effect: it
// goes when the last of them does, whatever order they are destroyed in.
struct SteamAudioCore
{
    LibHandle   lib = nullptr;
    Api         api;
    IPLContext  context = nullptr;
    IPLHRTF     hrtf = nullptr;
    IPLAudioSettings audio{};

    ~SteamAudioCore()
    {
        if (hrtf)    api.hrtfRelease(&hrtf);
        if (context) api.contextRelease(&context);
        closeLibrary(lib);
    }
};

struct SteamAudio::Impl
{
    std::shared_ptr<SteamAudioCore> core;
    int         frameSize = 0;
    std::string error;
};

struct SteamAudio::Binaural::Impl
{
    std::shared_ptr<SteamAudioCore> core;
    IPLBinauralEffect effect = nullptr;
    IPLDirectEffect   direct = nullptr;   // optional: a voice without it just skips the stage
    IPLReflectionEffect reverb = nullptr; // optional, likewise (parametric reverb)
    int               reverbIrSize = 0;
    int               frameSize = 0;
    std::vector<float> left, right, directOut, reverbOut;

    ~Impl()
    {
        if (reverb) core->api.reflRelease(&reverb);
        if (direct) core->api.directRelease(&direct);
        if (effect) core->api.effectRelease(&effect);
    }
};

bool SteamAudio::compiledIn() { return true; }

const char* SteamAudio::version()
{
    static const std::string v = std::to_string(STEAMAUDIO_VERSION_MAJOR) + "." +
                                 std::to_string(STEAMAUDIO_VERSION_MINOR) + "." +
                                 std::to_string(STEAMAUDIO_VERSION_PATCH);
    return v.c_str();
}

SteamAudio::SteamAudio() : m_impl(std::make_unique<Impl>()) {}
SteamAudio::~SteamAudio() = default;

bool SteamAudio::init(int sampleRate, int frameSize)
{
    if (m_impl->core) return true;
    m_impl->error.clear();
    if (sampleRate <= 0 || frameSize <= 0)
    {
        m_impl->error = "invalid mixer format for Steam Audio";
        return false;
    }

    // Beside the engine library first (the shipped layout), then the current
    // directory, then the SDK folder the build used (a dev tree), then the
    // loader's own search.
    std::vector<std::string> candidates;
    const std::string dir = moduleDirectory();
    if (!dir.empty()) candidates.push_back(dir + "/" + kLibName);
    candidates.push_back(std::string("./") + kLibName);
#if defined(HE_STEAM_AUDIO_LIB_DIR)
    candidates.push_back(std::string(HE_STEAM_AUDIO_LIB_DIR) + "/" + kLibName);
#endif
    candidates.push_back(kLibName);

    auto core = std::make_shared<SteamAudioCore>();
    for (const std::string& c : candidates)
        if ((core->lib = openLibrary(c))) break;
    if (!core->lib)
    {
        m_impl->error = std::string("could not load ") + kLibName + " (searched beside the engine library)";
        return false;
    }
    if (!core->api.load(core->lib, m_impl->error)) return false;

    IPLContextSettings cs{};
    cs.version = STEAMAUDIO_VERSION;
    if (core->api.contextCreate(&cs, &core->context) != IPL_STATUS_SUCCESS)
    {
        core->context = nullptr;
        m_impl->error = "iplContextCreate failed (library/header version mismatch?)";
        return false;
    }
    core->audio.samplingRate = sampleRate;
    core->audio.frameSize    = frameSize;
    IPLHRTFSettings hs{};
    hs.type     = IPL_HRTFTYPE_DEFAULT;
    hs.volume   = 1.0f;
    hs.normType = IPL_HRTFNORMTYPE_NONE;
    if (core->api.hrtfCreate(core->context, &core->audio, &hs, &core->hrtf) != IPL_STATUS_SUCCESS)
    {
        core->hrtf = nullptr;
        m_impl->error = "iplHRTFCreate failed";
        return false;
    }

    m_impl->core      = std::move(core);
    m_impl->frameSize = frameSize;
    HE_LOG_INFO(Audio, "Steam Audio %s ready (%d Hz, %d-frame blocks)", version(), sampleRate, frameSize);
    return true;
}

void SteamAudio::shutdown()
{
    m_impl->core.reset();   // the library stays loaded while a Binaural still holds it
    m_impl->frameSize = 0;
}

bool SteamAudio::isReady() const { return m_impl->core != nullptr; }
int  SteamAudio::frameSize() const { return m_impl->frameSize; }
const std::string& SteamAudio::lastError() const { return m_impl->error; }

SteamAudio::Binaural::Binaural() : m_impl(std::make_unique<Impl>()) {}
SteamAudio::Binaural::~Binaural() = default;

std::unique_ptr<SteamAudio::Binaural> SteamAudio::createBinaural()
{
    if (!m_impl->core) return nullptr;
    std::unique_ptr<Binaural> b(new Binaural());
    b->m_impl->core      = m_impl->core;
    b->m_impl->frameSize = m_impl->frameSize;
    b->m_impl->left.resize(static_cast<size_t>(m_impl->frameSize));
    b->m_impl->right.resize(static_cast<size_t>(m_impl->frameSize));
    b->m_impl->directOut.resize(static_cast<size_t>(m_impl->frameSize));
    b->m_impl->reverbOut.resize(static_cast<size_t>(m_impl->frameSize));
    IPLBinauralEffectSettings es{};
    es.hrtf = m_impl->core->hrtf;
    if (m_impl->core->api.effectCreate(m_impl->core->context, &m_impl->core->audio, &es,
                                       &b->m_impl->effect) != IPL_STATUS_SUCCESS)
    {
        b->m_impl->effect = nullptr;
        m_impl->error = "iplBinauralEffectCreate failed";
        return nullptr;
    }
    IPLDirectEffectSettings ds{};
    ds.numChannels = 1;
    if (m_impl->core->api.directCreate(m_impl->core->context, &m_impl->core->audio, &ds,
                                       &b->m_impl->direct) != IPL_STATUS_SUCCESS)
        b->m_impl->direct = nullptr;   // the HRTF still works; direct-path options just do nothing
    IPLReflectionEffectSettings rs{};
    rs.type        = IPL_REFLECTIONEFFECTTYPE_PARAMETRIC;
    rs.irSize      = 2 * static_cast<IPLint32>(m_impl->core->audio.samplingRate);   // up to a two second tail
    rs.numChannels = 1;
    if (m_impl->core->api.reflCreate(m_impl->core->context, &m_impl->core->audio, &rs,
                                     &b->m_impl->reverb) == IPL_STATUS_SUCCESS)
        b->m_impl->reverbIrSize = rs.irSize;
    else
        b->m_impl->reverb = nullptr;
    return b;
}

void SteamAudio::Binaural::airAbsorption(float sx, float sy, float sz, float lx, float ly, float lz,
                                         float out[3]) const
{
    out[0] = out[1] = out[2] = 1.0f;
    if (!m_impl->core) return;
    IPLAirAbsorptionModel model{};
    model.type = IPL_AIRABSORPTIONTYPE_DEFAULT;
    m_impl->core->api.airCalc(m_impl->core->context, { sx, sy, sz }, { lx, ly, lz }, &model, out);
}

bool SteamAudio::Binaural::process(const float* mono, float* stereoOut, int frames,
                                   const SpatialDirection& direction, const DirectParams* direct)
{
    Impl& s = *m_impl;
    if (!s.effect || !mono || !stereoOut || frames <= 0 || frames % s.frameSize != 0)
        return false;

    IPLBinauralEffectParams params{};
    params.direction     = { direction.x, direction.y, direction.z };
    params.interpolation = IPL_HRTFINTERPOLATION_BILINEAR;
    params.spatialBlend  = 1.0f;
    params.hrtf          = s.core->hrtf;
    params.peakDelays    = nullptr;

    for (int base = 0; base < frames; base += s.frameSize)
    {
        // The SDK reads the input through a table of channel pointers; the
        // mono block is passed in place (it never writes to its input).
        float* inPtr = const_cast<float*>(mono + base);
        if (direct && s.direct && (direct->airAbsorption || direct->occlusion))
        {
            IPLDirectEffectParams dp{};
            int flags = 0;
            if (direct->airAbsorption) flags |= IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION;
            if (direct->occlusion)     flags |= IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION | IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION;
            dp.flags               = static_cast<IPLDirectEffectFlags>(flags);
            dp.transmissionType    = IPL_TRANSMISSIONTYPE_FREQDEPENDENT;
            dp.distanceAttenuation = 1.0f;   // the voice node already applied the distance curve
            dp.directivity         = 1.0f;
            dp.occlusion           = direct->visibility;
            for (int b = 0; b < 3; ++b)
            {
                dp.airAbsorption[b] = direct->airBands[b];
                dp.transmission[b]  = direct->transmission[b];
            }
            float* dIn[1]  = { inPtr };
            float* dOut[1] = { s.directOut.data() };
            IPLAudioBuffer din { 1, s.frameSize, dIn };
            IPLAudioBuffer dout{ 1, s.frameSize, dOut };
            s.core->api.directApply(s.direct, &dp, &din, &dout);
            inPtr = s.directOut.data();
        }
        float* inCh[1]  = { inPtr };
        float* outCh[2] = { s.left.data(), s.right.data() };
        IPLAudioBuffer in { 1, s.frameSize, inCh };
        IPLAudioBuffer out{ 2, s.frameSize, outCh };
        s.core->api.effectApply(s.effect, &params, &in, &out);
        // The room's reverb: the same (direct-processed) voice through the parametric
        // reflection effect, mixed into both ears without a direction.
        float wet = 0.0f;
        if (direct && direct->reflections && s.reverb)
        {
            IPLReflectionEffectParams rp{};
            rp.type        = IPL_REFLECTIONEFFECTTYPE_PARAMETRIC;
            rp.numChannels = 1;
            rp.irSize      = s.reverbIrSize;
            for (int b = 0; b < 3; ++b) rp.reverbTimes[b] = direct->reverbTimes[b];
            float* rOut[1] = { s.reverbOut.data() };
            IPLAudioBuffer rin { 1, s.frameSize, inCh };
            IPLAudioBuffer rout{ 1, s.frameSize, rOut };
            s.core->api.reflApply(s.reverb, &rp, &rin, &rout, nullptr);
            wet = direct->reverbWet;
        }
        for (int i = 0; i < s.frameSize; ++i)
        {
            const float r = wet * s.reverbOut[static_cast<size_t>(i)];
            stereoOut[(base + i) * 2]     = s.left[static_cast<size_t>(i)] + r;
            stereoOut[(base + i) * 2 + 1] = s.right[static_cast<size_t>(i)] + r;
        }
    }
    return true;
}

void SteamAudio::Binaural::reset()
{
    if (m_impl->effect) m_impl->core->api.effectReset(m_impl->effect);
    if (m_impl->direct) m_impl->core->api.directReset(m_impl->direct);
    if (m_impl->reverb) m_impl->core->api.reflReset(m_impl->reverb);
}

#else // !HE_HAVE_STEAM_AUDIO ───────────────────────────────────────────────────

// Built without the SDK (HE_ENABLE_STEAM_AUDIO=OFF, an app runtime flavour, or a
// platform the SDK does not cover): the same interface, never ready.

struct SteamAudio::Impl { std::string error; };
struct SteamAudio::Binaural::Impl {};

bool        SteamAudio::compiledIn() { return false; }
const char* SteamAudio::version()    { return ""; }

SteamAudio::SteamAudio() : m_impl(std::make_unique<Impl>()) {}
SteamAudio::~SteamAudio() = default;

bool SteamAudio::init(int, int)
{
    m_impl->error = "this build does not include Steam Audio (HE_ENABLE_STEAM_AUDIO=OFF)";
    return false;
}
void SteamAudio::shutdown() {}
bool SteamAudio::isReady() const { return false; }
int  SteamAudio::frameSize() const { return 0; }
const std::string& SteamAudio::lastError() const { return m_impl->error; }

SteamAudio::Binaural::Binaural() = default;
SteamAudio::Binaural::~Binaural() = default;
bool SteamAudio::Binaural::process(const float*, float*, int, const SpatialDirection&, const DirectParams*) { return false; }
void SteamAudio::Binaural::airAbsorption(float, float, float, float, float, float, float out[3]) const
{
    out[0] = out[1] = out[2] = 1.0f;
}
void SteamAudio::Binaural::reset() {}
std::unique_ptr<SteamAudio::Binaural> SteamAudio::createBinaural() { return nullptr; }

#endif

} // namespace HE
