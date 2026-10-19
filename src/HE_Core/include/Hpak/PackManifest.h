#pragma once
#include <Hpak/HpakWriter.h>
#include <Types/Defines.h>
#include <Types/UUID.h>
#include <cstdint>
#include <filesystem>
#include <utility>
#include <vector>

class HpakReader;

// ─── Incremental-pack manifest ────────────────────────────────────────────────
// Sidecar "<name>.hpak.manifest" written next to a pak: per entry the hash of the
// rewritten blob it was packed from, the hash of the SOURCE it came from and the
// references the pipeline resolved on the way (what lets an unchanged asset skip
// the pipeline altogether, see Hpak::CachedEntry), plus two fingerprints that gate
// reuse — the pak's tocHash (the manifest must describe exactly this pak, not a
// stale or hand-swapped one) and a settings fingerprint (codec/level/encrypt/key,
// cook and texture-compression settings, shader backends, pipeline version).
//
// Shared by the editor's export (ProjectExporter) and the hpak_packer CLI
// (--cache), so both gate reuse the same way and read each other's manifests.
namespace HE
{
// The settings half of the gate. Two packs may share entries only when this
// matches: a different codec, key or texture target produces different stored
// bytes for the same source.
HE_API uint64_t packSettingsFingerprint(const Hpak::PackSettings& settings);

// Opens the previous pak at `pakPath` into `previousPak` and fills `out` from the
// manifest at `manifestPath` (out.previousPak then points at `previousPak`), when
// both exist, the manifest describes exactly that pak and was written with these
// settings. False otherwise, with `out` to be ignored: pack from scratch.
// `previousPak` has to stay open until addDirectories() is done, and be closed
// BEFORE write() replaces the file (Windows refuses to replace an open file).
HE_API bool openIncrementalCache(const std::filesystem::path& pakPath,
                                 const std::filesystem::path& manifestPath,
                                 const Hpak::PackSettings&    settings,
                                 HpakReader&                  previousPak,
                                 Hpak::IncrementalCache&      out);

// After write(): reopens the new pak for its tocHash and writes the manifest the
// NEXT incremental pack reads. Best effort — false (and no manifest, or the old
// one left in place) just means the next pack starts from scratch.
HE_API bool writeIncrementalManifest(
	const std::filesystem::path&                                pakPath,
	const std::filesystem::path&                                manifestPath,
	const Hpak::PackSettings&                                   settings,
	const std::vector<std::pair<HE::UUID, Hpak::CachedEntry>>& infos);
} // namespace HE
