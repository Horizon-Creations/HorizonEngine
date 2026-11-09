#include <Hpak/ReachFilter.h>
#include <Hpak/HpakFormat.h>          // Hpak::globMatch
#include <ContentManager/HAsset.h>    // chunk ids, header layout
#include <Types/Enums.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <unordered_map>
#include <unordered_set>

namespace HE::reach
{

namespace
{

// A byte that can sit inside a stored path. Deliberately generous (spaces, brackets, the
// high half for UTF-8 names): the scan only ever LOOKS UP what it cuts out, so a few extra
// characters cost nothing, a missing one would cut a real path short.
inline bool isPathChar(uint8_t c)
{
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return true;
    if (c >= 0x80) return true;
    switch (c)
    {
    case '_': case '-': case '.': case '/': case '\\': case ' ': case '(': case ')': case '+':
    case '&': case '\'': case ',': case '[': case ']': case '@': case '#': case '~': case '!':
        return true;
    default: return false;
    }
}

inline uint64_t load64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
inline uint64_t swap64(uint64_t v)
{
    return ((v & 0x00000000000000FFull) << 56) | ((v & 0x000000000000FF00ull) << 40)
         | ((v & 0x0000000000FF0000ull) << 24) | ((v & 0x00000000FF000000ull) << 8)
         | ((v & 0x000000FF00000000ull) >> 8)  | ((v & 0x0000FF0000000000ull) >> 24)
         | ((v & 0x00FF000000000000ull) >> 40) | ((v & 0xFF00000000000000ull) >> 56);
}
inline uint32_t bloomSlot(uint64_t hi)
{
    return static_cast<uint32_t>((hi ^ (hi >> 16) ^ (hi >> 32) ^ (hi >> 48)) & 0xFFFFu);
}

// The chunks whose bytes are payload, not structure: geometry, pixels, audio, fonts,
// keyframes and the shader text the pack bakes itself. None of them names another asset.
bool isPayloadChunk(uint32_t id)
{
    using namespace HAsset;
    return id == CHUNK_META
        || id == CHUNK_VERT || id == CHUNK_INDX || id == CHUNK_NORM || id == CHUNK_TEXC
        || id == CHUNK_MVBO || id == CHUNK_BONE || id == CHUNK_BWGT
        || id == CHUNK_PIXL || id == CHUNK_TXSM
        || id == CHUNK_PCMD || id == CHUNK_OGGD || id == CHUNK_MP3D || id == CHUNK_FLCD
        || id == CHUNK_FNTD || id == CHUNK_ANIM
        || id == CHUNK_PSHD || id == CHUNK_PPSD;
}

// Kinds that point at nothing at all.
bool pointsAtNothing(uint16_t type)
{
    return type == static_cast<uint16_t>(HE::AssetType::Texture)
        || type == static_cast<uint16_t>(HE::AssetType::Audio)
        || type == static_cast<uint16_t>(HE::AssetType::Font);
}

struct Index
{
    const std::vector<Node>* nodes = nullptr;
    // basename ("Rock.hasset") -> nodes carrying a path with that basename
    std::unordered_map<std::string, std::vector<int>> byBasename;
    std::unordered_map<HE::UUID, int>                 byUuid;
    std::vector<uint8_t>                              bloom = std::vector<uint8_t>(65536, 0);

    static std::string baseOf(const std::string& p)
    {
        const size_t s = p.find_last_of("/\\");
        return s == std::string::npos ? p : p.substr(s + 1);
    }

    void build(const std::vector<Node>& ns)
    {
        nodes = &ns;
        for (int i = 0; i < static_cast<int>(ns.size()); ++i)
        {
            const Node& n = ns[static_cast<size_t>(i)];
            if (!n.path.empty())     byBasename[baseOf(n.path)].push_back(i);
            if (!n.metaPath.empty() && n.metaPath != n.path) byBasename[baseOf(n.metaPath)].push_back(i);
            if (!(n.id == HE::UUID{}))
            {
                byUuid.emplace(n.id, i);
                bloom[bloomSlot(n.id.hi)] = 1;
            }
        }
    }

    bool endsWith(const uint8_t* p, size_t end, size_t regionStart, const std::string& v) const
    {
        if (v.empty() || v.size() > end - regionStart) return false;
        return std::memcmp(p + end - v.size(), v.data(), v.size()) == 0;
    }

    // Everything `p[0..n)` names, appended to `out` (duplicates allowed; the caller marks).
    void scan(const uint8_t* p, size_t n, std::vector<int>& out) const
    {
        if (!p || n < 8) return;

        // ── By path: every ".hasset" / ".hescene" is the end of a stored path. ──────────
        for (size_t i = 0; i + 7 <= n; ++i)
        {
            if (p[i] != '.') continue;
            size_t end = 0;
            if (i + 7 <= n && std::memcmp(p + i, ".hasset", 7) == 0)       end = i + 7;
            else if (i + 8 <= n && std::memcmp(p + i, ".hescene", 8) == 0) end = i + 8;
            else continue;

            // The whole path-looking run before the extension (a "Content/" prefix, folders),
            // and inside it the basename: cut at every start so a stray length byte in front
            // of the name ("0Test.hasset") still finds "Test.hasset".
            size_t regionStart = i;
            while (regionStart > 0 && i - regionStart < 300 && isPathChar(p[regionStart - 1])) --regionStart;
            size_t nameStop = i;   // first position of the run's last path component
            while (nameStop > regionStart && p[nameStop - 1] != '/' && p[nameStop - 1] != '\\') --nameStop;
            const bool bare = nameStop == regionStart; // no folder part: "Cube.hasset" on its own
            for (size_t s = nameStop; s < i; ++s)
            {
                const std::string key(reinterpret_cast<const char*>(p + s), end - s);
                const auto it = byBasename.find(key);
                if (it == byBasename.end()) continue;
                for (int idx : it->second)
                {
                    const Node& nd = (*nodes)[static_cast<size_t>(idx)];
                    // A folder part in the text must match the node's folder part; a bare name
                    // matches every node of that name (errs toward keeping).
                    if (bare || endsWith(p, end, regionStart, nd.path) || endsWith(p, end, regionStart, nd.metaPath))
                        out.push_back(idx);
                }
            }
        }

        // ── By UUID, binary: 16 raw bytes, little-endian (chunks) or big-endian (CBOR). ──
        if (n >= 16)
            for (size_t i = 0; i + 16 <= n; ++i)
            {
                const uint64_t a = load64(p + i);
                if (bloom[bloomSlot(a)])
                {
                    const auto it = byUuid.find(HE::UUID{ a, load64(p + i + 8) });
                    if (it != byUuid.end()) out.push_back(it->second);
                }
                const uint64_t ab = swap64(a);
                if (bloom[bloomSlot(ab)])
                {
                    const auto it = byUuid.find(HE::UUID{ ab, swap64(load64(p + i + 8)) });
                    if (it != byUuid.end()) out.push_back(it->second);
                }
            }

        // ── By UUID, CBOR array: 0x82 (array of 2) then two unsigned ints, each in its SHORTEST ──
        // form (a 1-byte value, 0x18 + 1, 0x19 + 2, 0x1a + 4 or 0x1b + 8 bytes). A random id is
        // always the 8-byte form, but the engine's own assets have small ids ({0x412, 1} is
        // 82 19 04 12 01) and the raw 16-byte scans above never see those.
        auto cborUint = [&](size_t& i, uint64_t& v) -> bool
        {
            if (i >= n) return false;
            const uint8_t b = p[i++];
            if ((b >> 5) != 0) return false;             // major type 0 only
            const uint8_t info = b & 0x1F;
            if (info < 24) { v = info; return true; }
            const size_t len = info == 24 ? 1 : info == 25 ? 2 : info == 26 ? 4 : info == 27 ? 8 : 0;
            if (len == 0 || i + len > n) return false;
            uint64_t acc = 0;
            for (size_t k = 0; k < len; ++k) acc = (acc << 8) | p[i + k];
            i += len;
            v = acc;
            return true;
        };
        for (size_t i = 0; i + 3 <= n; ++i)
        {
            if (p[i] != 0x82) continue;
            size_t j = i + 1;
            uint64_t hi = 0, lo = 0;
            if (!cborUint(j, hi) || !bloom[bloomSlot(hi)] || !cborUint(j, lo)) continue;
            const auto it = byUuid.find(HE::UUID{ hi, lo });
            if (it != byUuid.end()) out.push_back(it->second);
        }

        // ── By UUID, text: "[hi, lo]" and {"hi":hi,"lo":lo} as decimal digit runs. ──────
        auto digitRun = [&](size_t& i, uint64_t& v) -> bool
        {
            const size_t s = i;
            uint64_t acc = 0;
            while (i < n && p[i] >= '0' && p[i] <= '9')
            {
                const uint64_t d = static_cast<uint64_t>(p[i] - '0');
                if (acc > (UINT64_MAX - d) / 10) { while (i < n && p[i] >= '0' && p[i] <= '9') ++i; return false; }
                acc = acc * 10 + d; ++i;
            }
            v = acc;
            return i > s;
        };
        for (size_t i = 0; i < n;)
        {
            if (!(p[i] >= '0' && p[i] <= '9') || (i > 0 && p[i - 1] >= '0' && p[i - 1] <= '9')) { ++i; continue; }
            uint64_t hi = 0;
            if (!digitRun(i, hi) || !bloom[bloomSlot(hi)]) continue;
            size_t j = i;
            for (int steps = 0; j < n && steps < 40; ++steps, ++j)
            {
                const uint8_t c = p[j];
                if (!(c == ' ' || c == ',' || c == '\t' || c == '\r' || c == '\n' || c == '"' || c == ':'
                      || c == 'l' || c == 'o')) break;
            }
            uint64_t lo = 0;
            size_t k = j;
            if (k < n && p[k] >= '0' && p[k] <= '9' && digitRun(k, lo))
            {
                const auto it = byUuid.find(HE::UUID{ hi, lo });
                if (it != byUuid.end()) out.push_back(it->second);
            }
        }
    }

    // A node's bytes: an asset is walked chunk by chunk (payload chunks skipped, no copy),
    // anything else (a scene, a script, a manifest) is scanned whole.
    void scanBlob(const uint8_t* p, size_t n, uint16_t type, std::vector<int>& out, size_t& scanned) const
    {
        if (!p || n == 0) return;
        const bool isAsset = n >= sizeof(HAsset::FileHeader) && std::memcmp(p, HAsset::k_magic, 4) == 0;
        if (!isAsset) { scanned += n; scan(p, n, out); return; }
        if (pointsAtNothing(type)) return;
        HAsset::FileHeader h;
        std::memcpy(&h, p, sizeof(h));
        if (pointsAtNothing(h.asset_type)) return;
        size_t o = sizeof(HAsset::FileHeader);
        for (uint32_t c = 0; c < h.chunk_count && o + sizeof(HAsset::ChunkHeader) <= n; ++c)
        {
            HAsset::ChunkHeader ch;
            std::memcpy(&ch, p + o, sizeof(ch));
            o += sizeof(ch);
            if (ch.size > n - o) break;
            if (!isPayloadChunk(ch.id)) { scanned += static_cast<size_t>(ch.size); scan(p + o, static_cast<size_t>(ch.size), out); }
            o += static_cast<size_t>(ch.size);
        }
    }
};

} // namespace

Result computeReachable(const std::vector<Node>& nodes, const Roots& roots)
{
    Result res;
    res.reachable.assign(nodes.size(), 0);
    Index ix;
    ix.build(nodes);

    std::vector<int> queue;
    auto mark = [&](int idx)
    {
        if (idx < 0 || idx >= static_cast<int>(nodes.size()) || res.reachable[static_cast<size_t>(idx)]) return;
        res.reachable[static_cast<size_t>(idx)] = 1;
        ++res.nodesReachable;
        queue.push_back(idx);
    };

    // Roots by name, id, folder, glob and kind.
    const std::unordered_set<uint16_t> rootTypes(roots.types.begin(), roots.types.end());
    for (int i = 0; i < static_cast<int>(nodes.size()); ++i)
    {
        const Node& n = nodes[static_cast<size_t>(i)];
        bool root = rootTypes.count(n.type) != 0;
        if (!root)
            for (const std::string& pr : roots.paths)
                if (pr == n.path || (!n.metaPath.empty() && pr == n.metaPath)) { root = true; break; }
        if (!root)
            for (const std::string& pre : roots.pathPrefixes)
                if (!pre.empty() && n.path.rfind(pre, 0) == 0) { root = true; break; }
        if (!root)
            for (const std::string& g : roots.globs)
                if (!g.empty() && Hpak::globMatch(g, n.path)) { root = true; break; }
        if (!root)
            for (const HE::UUID& u : roots.uuids)
                if (!(u == HE::UUID{}) && u == n.id) { root = true; break; }
        if (root) mark(i);
    }

    // Roots that are blobs.
    std::vector<int> found;
    for (const auto& [p, n] : roots.blobs)
    {
        found.clear();
        ix.scanBlob(p, n, 0, found, res.bytesScanned);
        for (int idx : found) mark(idx);
    }

    // Breadth-first over the references of everything reached.
    while (!queue.empty())
    {
        const int cur = queue.back();
        queue.pop_back();
        const Node& n = nodes[static_cast<size_t>(cur)];
        found.clear();
        ix.scanBlob(n.data, n.size, n.type, found, res.bytesScanned);
        for (int idx : found) mark(idx);
    }
    return res;
}

void collectProjectTextRoots(const std::filesystem::path& contentDir,
                                    std::vector<std::vector<uint8_t>>& out)
{
    namespace fs = std::filesystem;
    constexpr uintmax_t kMaxBytes = 64u << 20;
    const auto take = [&](const fs::path& p)
    {
        std::error_code ec;
        const uintmax_t sz = fs::file_size(p, ec);
        if (ec || sz == 0 || sz > kMaxBytes) return;
        std::ifstream f(p, std::ios::binary);
        if (!f) return;
        out.emplace_back((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };
    const auto isCode = [](const fs::path& p)
    {
        const std::string e = p.extension().string();
        return e == ".lua" || e == ".py" || e == ".hcode";
    };
    std::error_code ec;
    for (fs::recursive_directory_iterator it(contentDir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && isCode(it->path())) take(it->path());
    ec.clear();
    const fs::path root = contentDir.parent_path();
    for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && (it->path().extension() == ".heproj" || isCode(it->path())))
            take(it->path());
    ec.clear();
    const fs::path cfg = root / "Config";
    for (fs::recursive_directory_iterator it(cfg, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) take(it->path());
}

Roots engineImplicitRoots()
{
    Roots r;
    // Found by type, not by reference (PlayerHost.cpp, TypeRegistry::refreshFromContent): a
    // PlayerController class and an input action are used because they EXIST.
    for (HE::AssetType t : { HE::AssetType::InputAction, HE::AssetType::InputMappingContext,
                             HE::AssetType::HorizonCodeClass, HE::AssetType::StructType,
                             HE::AssetType::EnumType, HE::AssetType::SaveGameTemplate,
                             HE::AssetType::Theme })
        r.types.push_back(static_cast<uint16_t>(t));
    // Systems that load engine content by a path or id they hold themselves.
    r.pathPrefixes = {
        "Engine/Audio/Weather/",          // WeatherAudio, DefaultAssets.h 0x500..0x504
        "Engine/MaterialFunctions/",      // WeatherMaterialFunctions.h, ProceduralSurfaceFunctions.h
        "Engine/Meshes/",                 // LayerMeshPresets.h builds these names from a material's group
    };
    // The landscape water's material, its falls' particle graphs (and through them the spray material) and an
    // obstacle's foam material are NOT here: they are references the scene writes even at their defaults
    // (TerrainComponent "waterMaterial"/"waterFall*", WaterObstacleComponent "foamMaterial"), and the export
    // re-serializes every scene before this scan, so the scan finds them like any other asset reference.
    r.uuids = {
        HE::UUID{ 0x000000000000041FULL, 0x0000000000000001ULL },   // TrailClipmap.h kTrailTextureId
    };
    return r;
}

} // namespace HE::reach
