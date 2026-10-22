#include <Hpak/PackManifest.h>
#include <Hpak/HpakFormat.h>
#include <Hpak/HpakReader.h>
#include <nlohmann/json.hpp>
#include <cstring>
#include <fstream>

// Moved here from ProjectExporter.cpp unchanged (TLS-203), so the hpak_packer CLI
// gates and writes the cache exactly like the editor's export does.

namespace
{
// Bump when the pack pipeline's OUTPUT changes for the same input — a new cook, a
// different ref bake, a changed shader precompile or material fold. The incremental
// cache skips an unchanged asset's whole pipeline (Hpak::CachedEntry), so it cannot
// notice a change in the CODE that produced the entry; this is how it is told.
// "Incremental" off in the export dialog (or no --cache for the packer) is the
// manual escape hatch.
constexpr uint8_t kPackPipelineVersion = 3; // 3: hpak v3 hash (manifest hashes changed)

bool loadPakManifest(const std::filesystem::path& path,
                     uint64_t expectPakTocHash, uint64_t expectSettingsFp,
                     Hpak::IncrementalCache& out)
{
    std::ifstream in(path);
    if (!in.is_open()) return false;
    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;

    auto u64 = [&](const char* key, uint64_t& out) -> bool {
        auto it = j.find(key);
        if (it == j.end() || !it->is_number_unsigned()) return false;
        out = it->get<uint64_t>();
        return true;
    };
    uint64_t tocHash = 0, fp = 0;
    if (!u64("pakTocHash", tocHash) || tocHash != expectPakTocHash) return false;
    if (!u64("settingsFp", fp) || fp != expectSettingsFp) return false;

    auto it = j.find("entries");
    if (it == j.end() || !it->is_array()) return false;
    for (const auto& e : *it)
    {
        if (!e.is_object()) continue;
        auto hi = e.find("hi"); auto lo = e.find("lo"); auto h = e.find("srcHash");
        if (hi == e.end() || lo == e.end() || h == e.end()) continue;
        if (!hi->is_number_unsigned() || !lo->is_number_unsigned() || !h->is_number_unsigned())
            continue;
        const HE::UUID id{hi->get<uint64_t>(), lo->get<uint64_t>()};
        out.srcHashes[id] = h->get<uint64_t>();

        // Level-1 data (absent in a manifest written before it existed → inHash 0,
        // which never matches, so such an entry just takes the level-2 path).
        Hpak::CachedEntry ce;
        ce.srcHash = h->get<uint64_t>();
        const auto ih = e.find("inHash");
        if (ih != e.end() && ih->is_number_unsigned()) ce.inHash = ih->get<uint64_t>();
        if (const auto refs = e.find("refs"); refs != e.end() && refs->is_array())
            for (const auto& r : *refs)
                if (r.is_array() && r.size() == 3 && r[0].is_string()
                    && r[1].is_number_unsigned() && r[2].is_number_unsigned())
                    ce.deps.refs.emplace_back(r[0].get<std::string>(),
                        HE::UUID{ r[1].get<uint64_t>(), r[2].get<uint64_t>() });
        if (const auto par = e.find("parents"); par != e.end() && par->is_array())
            for (const auto& r : *par)
                if (r.is_array() && r.size() == 2 && r[0].is_string() && r[1].is_number_unsigned())
                    ce.deps.parents.emplace_back(r[0].get<std::string>(), r[1].get<uint64_t>());
        out.entries[id] = std::move(ce);
    }
    return !out.srcHashes.empty();
}

bool savePakManifest(const std::filesystem::path& path,
                     uint64_t pakTocHash, uint64_t settingsFp,
                     const std::vector<std::pair<HE::UUID, Hpak::CachedEntry>>& infos)
{
    nlohmann::json j;
    j["pakTocHash"] = pakTocHash;
    j["settingsFp"] = settingsFp;
    nlohmann::json entries = nlohmann::json::array();
    for (const auto& [id, ce] : infos)
    {
        nlohmann::json e = { {"hi", id.hi}, {"lo", id.lo}, {"srcHash", ce.srcHash},
                             {"inHash", ce.inHash} };
        if (!ce.deps.refs.empty())
        {
            nlohmann::json refs = nlohmann::json::array();
            for (const auto& [p, u] : ce.deps.refs) refs.push_back({ p, u.hi, u.lo });
            e["refs"] = std::move(refs);
        }
        if (!ce.deps.parents.empty())
        {
            nlohmann::json par = nlohmann::json::array();
            for (const auto& [p, h] : ce.deps.parents) par.push_back({ p, h });
            e["parents"] = std::move(par);
        }
        entries.push_back(std::move(e));
    }
    j["entries"] = std::move(entries);

    // Best-effort cache metadata: failures are ignored (next export packs full).
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) return false;
    out << j.dump();
    return static_cast<bool>(out);
}
} // namespace

namespace HE
{
uint64_t packSettingsFingerprint(const Hpak::PackSettings& s)
{
    uint8_t buf[8 + 32 + 1];
    buf[0] = static_cast<uint8_t>(s.codec);
    buf[1] = static_cast<uint8_t>(s.level);
    buf[2] = s.encrypt ? 1 : 0;
    // cook toggle (bit0) + texture-compression target (bits1-3) + its quality
    // (bits4-5) + cook-the-arrays (bit6) → changing any of them re-packs; a quality change that did not
    // would leave the previous export's blocks in the pak, encoded at the old one.
    buf[3] = static_cast<uint8_t>((s.cook ? 1 : 0) | ((s.textureCompression & 0x7) << 1)
                                  | ((s.textureQuality & 0x3) << 4)
                                  | ((s.cookTextureArrays ? 1 : 0) << 6));
    std::memcpy(buf + 4, &s.shaderBackends, 4); // precompiled-shader backend set → re-pack materials
    std::memcpy(buf + 8, s.key, 32);  // all-zero when not encrypting
    buf[8 + 32] = kPackPipelineVersion;
    return Hpak::hash64(buf, sizeof(buf));
}

bool openIncrementalCache(const std::filesystem::path& pakPath,
                          const std::filesystem::path& manifestPath,
                          const Hpak::PackSettings&    settings,
                          HpakReader&                  previousPak,
                          Hpak::IncrementalCache&      out)
{
    std::error_code ec;
    if (!std::filesystem::exists(pakPath, ec) || !previousPak.open(pakPath.string()))
        return false;
    if (!loadPakManifest(manifestPath, previousPak.tocHash(),
                         packSettingsFingerprint(settings), out))
        return false;
    out.previousPak = &previousPak;
    return true;
}

bool writeIncrementalManifest(const std::filesystem::path& pakPath,
                              const std::filesystem::path& manifestPath,
                              const Hpak::PackSettings&    settings,
                              const std::vector<std::pair<HE::UUID, Hpak::CachedEntry>>& infos)
{
    HpakReader newPak;
    if (!newPak.open(pakPath.string())) return false;
    return savePakManifest(manifestPath, newPak.tocHash(), packSettingsFingerprint(settings),
                           infos);
}
} // namespace HE
