#include <Hpak/ProjectConfig.h>
#include <cstdint>
#include <ContentManager/HAsset.h>
#include <fstream>
#include <cstring>

static constexpr char     k_magic[4] = {'H','C','F','G'};
// v3: appends defaultSaveTemplate (string) after startupSceneUuid.
// v4: appends theme + themeMode (two strings) after that.
static constexpr uint16_t k_version  = 4;

bool ProjectConfigLoader::save(const std::filesystem::path& dir, const ProjectConfig& cfg)
{
    std::ofstream f(dir / "project.hcfg", std::ios::binary | std::ios::trunc);
    if (!f) return false;

    std::vector<uint8_t> buf;
    buf.insert(buf.end(), k_magic, k_magic + 4);
    // Backward compatibility for user-dropped prebuilt runtime bundles
    // (GameRuntimes/<Platform>/ can lag the editor): an old runtime rejects any
    // version it doesn't know and boots pak-less. So the v3 tail is only
    // written when it actually carries something — a project without a default
    // save template keeps emitting plain v2, which every runtime reads.
    // Each tail is written only when it CARRIES something, so a project that
    // uses none of it keeps emitting the plain v2 every runtime can read.
    const bool hasTheme = !cfg.theme.empty() || !cfg.themeMode.empty();
    const uint16_t version = hasTheme ? 4
                           : cfg.defaultSaveTemplate.empty() ? 2 : 3;
    HAsset::Writer::appendPOD(buf, version);
    const uint16_t reserved = 0;
    HAsset::Writer::appendPOD(buf, reserved);

    HAsset::Writer::appendString(buf, cfg.projectName);
    HAsset::Writer::appendString(buf, cfg.hpakFilename);
    HAsset::Writer::appendString(buf, cfg.mainSceneName);
    buf.insert(buf.end(), cfg.projectUuidBytes, cfg.projectUuidBytes + 16);
    // Bits 16/32 are the application flags (see ProjectConfig). 32 stores the
    // NEGATION of advancedShaderEffects so that every config ever written
    // before it existed — bit clear — reads back as enabled.
    const uint32_t flags = (cfg.enableModSupport      ? 1u  : 0u)
                         | (cfg.encrypted             ? 2u  : 0u)
                         | (cfg.hasPackedScene        ? 4u  : 0u)
                         | (cfg.horizonCodeCompiled   ? 8u  : 0u)
                         | (cfg.appMode               ? 16u : 0u)
                         | (cfg.advancedShaderEffects ? 0u  : 32u)
                         // Bits 64/128/256 are the permission block. Stored
                         // STRAIGHT, unlike 32 above: a build written before
                         // they existed has them clear, and clear is also what
                         // they should mean — a project that never asked for
                         // file or process access does not get it.
                         | (cfg.allowFiles            ? 64u : 0u)
                         | (cfg.allowProcesses        ? 128u: 0u)
                         | (cfg.allowNetwork          ? 256u: 0u)
                         // Bits 512/1024 are the font scripts, one bit per
                         // script, in the same order as HE::UIFontScripts. Two
                         // fit here; a third would need its own bit rather than
                         // widening this shift, so the mask is masked.
                         | ((cfg.fontScripts & 3u) << 9);
    HAsset::Writer::appendPOD(buf, flags);
    buf.insert(buf.end(), cfg.encKey, cfg.encKey + 32);
    buf.insert(buf.end(), cfg.startupSceneUuid, cfg.startupSceneUuid + 16);
    if (version >= 3)
        HAsset::Writer::appendString(buf, cfg.defaultSaveTemplate);
    if (version >= 4)
    {
        HAsset::Writer::appendString(buf, cfg.theme);
        HAsset::Writer::appendString(buf, cfg.themeMode);
    }

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
    // Every version this build knows. Each adds a tail; an older one simply has
    // fewer, and the reader stops where that version stopped.
    if (version != 2 && version != 3 && version != 4) return false;

    if (!HAsset::Reader::readString(buf, off, out.projectName))   return false;
    if (!HAsset::Reader::readString(buf, off, out.hpakFilename))  return false;
    if (!HAsset::Reader::readString(buf, off, out.mainSceneName)) return false;
    if (off + 16 > buf.size()) return false;
    std::memcpy(out.projectUuidBytes, buf.data() + off, 16);
    off += 16;
    uint32_t flags = 0;
    if (!HAsset::Reader::readPOD(buf, off, flags)) return false;
    out.enableModSupport      = (flags & 1u) != 0;
    out.encrypted             = (flags & 2u) != 0;
    out.hasPackedScene        = (flags & 4u) != 0;
    out.horizonCodeCompiled   = (flags & 8u) != 0;
    out.appMode               = (flags & 16u) != 0;
    out.advancedShaderEffects = (flags & 32u) == 0;   // stored negated, see save()
    out.allowFiles            = (flags & 64u) != 0;
    out.allowProcesses        = (flags & 128u) != 0;
    out.allowNetwork          = (flags & 256u) != 0;
    out.fontScripts           = (flags >> 9) & 3u;
    if (off + 32 > buf.size()) return false;
    std::memcpy(out.encKey, buf.data() + off, 32);
    off += 32;
    if (off + 16 > buf.size()) return false;
    std::memcpy(out.startupSceneUuid, buf.data() + off, 16);
    off += 16;
    out.defaultSaveTemplate.clear();
    if (version >= 3 && !HAsset::Reader::readString(buf, off, out.defaultSaveTemplate))
        return false;
    out.theme.clear();
    out.themeMode.clear();
    if (version >= 4)
    {
        if (!HAsset::Reader::readString(buf, off, out.theme))     return false;
        if (!HAsset::Reader::readString(buf, off, out.themeMode)) return false;
    }
    return true;
}
