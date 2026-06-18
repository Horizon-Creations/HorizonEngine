#include <Hpak/ProjectConfig.h>
#include <ContentManager/HAsset.h>
#include <fstream>
#include <cstring>

static constexpr char     k_magic[4] = {'H','C','F','G'};
static constexpr uint16_t k_version  = 1;

bool ProjectConfigLoader::save(const std::filesystem::path& dir, const ProjectConfig& cfg)
{
    std::ofstream f(dir / "project.hcfg", std::ios::binary | std::ios::trunc);
    if (!f) return false;

    std::vector<uint8_t> buf;
    buf.insert(buf.end(), k_magic, k_magic + 4);
    HAsset::Writer::appendPOD(buf, k_version);
    const uint16_t reserved = 0;
    HAsset::Writer::appendPOD(buf, reserved);

    HAsset::Writer::appendString(buf, cfg.projectName);
    HAsset::Writer::appendString(buf, cfg.hpakFilename);
    HAsset::Writer::appendString(buf, cfg.mainSceneName);
    buf.insert(buf.end(), cfg.projectUuidBytes, cfg.projectUuidBytes + 16);
    const uint32_t flags = cfg.enableModSupport ? 1u : 0u;
    HAsset::Writer::appendPOD(buf, flags);

    f.write(reinterpret_cast<const char*>(buf.data()),
            static_cast<std::streamsize>(buf.size()));
    return f.good();
}

bool ProjectConfigLoader::load(const std::filesystem::path& dir, ProjectConfig& out)
{
    std::ifstream f(dir / "project.hcfg", std::ios::binary);
    if (!f) return false;

    std::vector<uint8_t> buf;
    buf.assign(std::istreambuf_iterator<char>(f),
               std::istreambuf_iterator<char>{});
    if (buf.size() < 8) return false;
    if (std::memcmp(buf.data(), k_magic, 4) != 0) return false;

    size_t off = 4;
    uint16_t version = 0, reserved = 0;
    if (!HAsset::Reader::readPOD(buf, off, version))  return false;
    if (!HAsset::Reader::readPOD(buf, off, reserved)) return false;
    if (version != k_version) return false;

    if (!HAsset::Reader::readString(buf, off, out.projectName))   return false;
    if (!HAsset::Reader::readString(buf, off, out.hpakFilename))  return false;
    if (!HAsset::Reader::readString(buf, off, out.mainSceneName)) return false;
    if (off + 16 > buf.size()) return false;
    std::memcpy(out.projectUuidBytes, buf.data() + off, 16);
    off += 16;
    uint32_t flags = 0;
    if (!HAsset::Reader::readPOD(buf, off, flags)) return false;
    out.enableModSupport = (flags & 1u) != 0;
    return true;
}
