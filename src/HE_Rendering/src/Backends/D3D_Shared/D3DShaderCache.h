#pragma once

// ─── On-disk FXC bytecode cache for the D3D11 and D3D12 backends (Thema 196) ──
// Measured on NN-WS03 (docs/d3d-startzeit-messung-2026-10-13.md): ~90 % of the
// D3D renderer start is D3DCompile, run from HLSL source on every launch —
// 7.8 s of 8.3 s on D3D11, 8.1 s of 8.9 s on D3D12, the cross-compiled sky
// pixel shader alone 5 s. Nothing caches FXC: the driver cache only sits
// behind Create*Shader / CreateGraphicsPipelineState. So the bytecode is kept
// here, under the user's pref path next to the GL program-binary cache.
//
// The key is everything FXC's output depends on for the calls that use it:
// source bytes, entry, target, both flag words and the identity of the
// d3dcompiler_47.dll actually loaded (it is a system DLL and moves with
// Windows updates). Calls with defines or an include handler are not cached —
// their inputs are not all in the arguments — none of the backends make one.
// A file is only used when its header repeats the request field by field and
// the payload is a DXBC container of the stated size and hash; anything else,
// a collision, a truncated write, a format change, is a miss and recompiles.
//
// HE_D3D_SHADER_CACHE=0 turns it off (the cold path, for measurements).
// HE_D3D_SHADER_CACHE_DIR=<dir> moves it (tests; SDL_GetPrefPath asks the
// shell for the roaming folder and ignores %APPDATA%, so a scratch APPDATA
// does not isolate it).
// HE_D3D_SHADER_DUMP=<dir> writes every compiled source there as .hlsl, so a
// runtime-built shader can be fed to fxc.exe by hand.
#include <Hpak/HpakFormat.h>

#include <SDL3/SDL.h>
#include <d3dcompiler.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace HE::d3dcache
{

namespace fs = std::filesystem;

// Bump when the file layout or the key composition changes.
inline constexpr char     kMagic[8]   = { 'H', 'E', 'D', 'X', 'B', 'C', '0', '2' };
// Past this many files the least recently used quarter goes (a hit touches
// its file). Most blobs are 1-60 KB (the sky ~215 KB); every edited material
// adds a pair.
inline constexpr size_t   kMaxFiles   = 2048;

struct FileHeader
{
    char     magic[8];
    uint64_t key;
    uint64_t srcHash;
    uint64_t srcSize;
    uint32_t flags1;
    uint32_t flags2;
    uint64_t codeSize;
    uint64_t codeHash;      // hash64 of the bytecode: a flipped byte is a miss, not a broken shader
    char     entry[64];
    char     target[16];
};

inline bool enabled()
{
    static const bool on = [] {
        const char* v = std::getenv("HE_D3D_SHADER_CACHE");
        return !(v && v[0] == '0');
    }();
    return on;
}

inline const fs::path& cacheDir()
{
    static const fs::path dir = [] {
        fs::path d;
        if (const char* over = std::getenv("HE_D3D_SHADER_CACHE_DIR"); over && *over) {
            d = over;
        } else if (char* pref = SDL_GetPrefPath("HorizonCreations", "HorizonEngine")) {
            d = fs::path(pref) / "d3dshadercache"; SDL_free(pref);
        } else {
            d = fs::temp_directory_path() / "HorizonEngine" / "d3dshadercache";
        }
        std::error_code ec; fs::create_directories(d, ec);
        return d;
    }();
    return dir;
}

// Size and write time of the d3dcompiler DLL in this process: a new compiler
// may emit different bytecode for the same source, so it starts a new key.
inline uint64_t compilerIdentity()
{
    static const uint64_t id = [] {
        uint64_t h = 47;
        wchar_t path[MAX_PATH] = {};
        if (HMODULE m = GetModuleHandleW(L"d3dcompiler_47.dll"))
            if (GetModuleFileNameW(m, path, MAX_PATH) > 0)
            {
                std::error_code ec;
                const uint64_t size = fs::file_size(path, ec);
                const auto     time = fs::last_write_time(path, ec).time_since_epoch().count();
                const uint64_t parts[2] = { size, static_cast<uint64_t>(time) };
                h ^= Hpak::hash64(reinterpret_cast<const uint8_t*>(parts), sizeof(parts));
            }
        return h;
    }();
    return id;
}

struct Request
{
    const void* src = nullptr;
    size_t      srcSize = 0;
    const char* entry = "";
    const char* target = "";
    uint32_t    flags1 = 0, flags2 = 0;
    uint64_t    srcHash = 0;
    uint64_t    key = 0;
};

inline Request makeRequest(const void* src, size_t srcSize, const char* entry,
                           const char* target, uint32_t flags1, uint32_t flags2)
{
    Request r;
    r.src = src; r.srcSize = srcSize;
    r.entry = entry ? entry : ""; r.target = target ? target : "";
    r.flags1 = flags1; r.flags2 = flags2;
    r.srcHash = Hpak::hash64(static_cast<const uint8_t*>(src), srcSize);

    std::string k;
    const uint64_t words[5] = { r.srcHash, static_cast<uint64_t>(srcSize),
                                (uint64_t(flags2) << 32) | flags1, compilerIdentity(),
                                static_cast<uint64_t>(sizeof(FileHeader)) };
    k.append(reinterpret_cast<const char*>(words), sizeof(words));
    k.append(kMagic, sizeof(kMagic));
    k.append(r.entry).push_back('\0');
    k.append(r.target).push_back('\0');
    r.key = Hpak::hash64(reinterpret_cast<const uint8_t*>(k.data()), k.size());
    return r;
}

inline fs::path fileFor(const Request& r)
{
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.dxbc", static_cast<unsigned long long>(r.key));
    return cacheDir() / name;
}

inline void fillHeader(FileHeader& h, const Request& r, uint64_t codeSize, uint64_t codeHash)
{
    std::memset(&h, 0, sizeof(h));
    std::memcpy(h.magic, kMagic, sizeof(kMagic));
    h.key = r.key; h.srcHash = r.srcHash; h.srcSize = r.srcSize;
    h.flags1 = r.flags1; h.flags2 = r.flags2; h.codeSize = codeSize; h.codeHash = codeHash;
    std::strncpy(h.entry,  r.entry,  sizeof(h.entry)  - 1);
    h.entry[sizeof(h.entry) - 1] = '\0';
    std::strncpy(h.target, r.target, sizeof(h.target) - 1);
    h.target[sizeof(h.target) - 1] = '\0';
}

// The blob for r, or nullptr on any mismatch.
inline ID3DBlob* load(const Request& r)
{
    if (std::strlen(r.entry) >= sizeof(FileHeader::entry) ||
        std::strlen(r.target) >= sizeof(FileHeader::target))
        return nullptr;
    const fs::path p = fileFor(r);
    std::ifstream in(p, std::ios::binary);
    if (!in) return nullptr;
    FileHeader got{}, want{};
    if (!in.read(reinterpret_cast<char*>(&got), sizeof(got))) return nullptr;
    fillHeader(want, r, got.codeSize, got.codeHash);
    if (std::memcmp(&got, &want, sizeof(got)) != 0) return nullptr;
    if (got.codeSize < 4 || got.codeSize > (64ull << 20)) return nullptr;

    std::vector<char> code(static_cast<size_t>(got.codeSize));
    if (!in.read(code.data(), static_cast<std::streamsize>(code.size()))) return nullptr;
    if (in.peek() != std::char_traits<char>::eof()) return nullptr;      // longer than stated
    if (std::memcmp(code.data(), "DXBC", 4) != 0) return nullptr;
    if (Hpak::hash64(reinterpret_cast<const uint8_t*>(code.data()), code.size()) != got.codeHash)
        return nullptr;

    ID3DBlob* blob = nullptr;
    if (FAILED(D3DCreateBlob(code.size(), &blob)) || !blob) return nullptr;
    std::memcpy(blob->GetBufferPointer(), code.data(), code.size());
    in.close();
    std::error_code ec;
    fs::last_write_time(p, fs::file_time_type::clock::now(), ec);         // LRU stamp
    return blob;
}

// Keeps the directory bounded: past kMaxFiles, drop the oldest quarter.
inline void prune()
{
    std::error_code ec;
    std::vector<std::pair<fs::file_time_type, fs::path>> files;
    for (const auto& e : fs::directory_iterator(cacheDir(), ec))
        if (e.is_regular_file(ec) && e.path().extension() == ".dxbc")
            files.emplace_back(e.last_write_time(ec), e.path());
    if (files.size() <= kMaxFiles) return;
    std::sort(files.begin(), files.end());
    const size_t drop = files.size() - kMaxFiles * 3 / 4;
    for (size_t i = 0; i < drop; ++i) fs::remove(files[i].second, ec);
}

// Several editors and games share the directory: write to a private temp
// name, then rename over the final one, so a reader never sees half a file.
inline void store(const Request& r, ID3DBlob* code)
{
    if (!code || code->GetBufferSize() < 4) return;
    if (std::strlen(r.entry) >= sizeof(FileHeader::entry) ||
        std::strlen(r.target) >= sizeof(FileHeader::target))
        return;
    static std::atomic<uint32_t> seq{0};
    static std::atomic<bool>     pruned{false};
    if (!pruned.exchange(true)) prune();

    std::error_code dirEc;
    fs::create_directories(cacheDir(), dirEc);   // cleared while the app runs
    const fs::path finalPath = fileFor(r);
    fs::path tmp = finalPath;
    tmp += "." + std::to_string(GetCurrentProcessId()) + "." + std::to_string(seq++) + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        FileHeader h;
        fillHeader(h, r, code->GetBufferSize(),
                   Hpak::hash64(static_cast<const uint8_t*>(code->GetBufferPointer()),
                                code->GetBufferSize()));
        out.write(reinterpret_cast<const char*>(&h), sizeof(h));
        out.write(static_cast<const char*>(code->GetBufferPointer()),
                  static_cast<std::streamsize>(code->GetBufferSize()));
        if (!out) { out.close(); std::error_code ec; fs::remove(tmp, ec); return; }
    }
    std::error_code ec;
    fs::rename(tmp, finalPath, ec);
    if (ec) fs::remove(tmp, ec);
}

// HE_D3D_SHADER_DUMP=<dir>: the source of every compile, one .hlsl per key.
inline void dumpSource(const Request& r, const char* sourceName)
{
    static const char* dir = std::getenv("HE_D3D_SHADER_DUMP");
    if (!dir || !*dir) return;
    std::string tag = std::string(sourceName ? sourceName : "src") + "_" + r.entry;
    for (char& c : tag)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') c = '_';
    char key[20];
    std::snprintf(key, sizeof(key), "%016llx", static_cast<unsigned long long>(r.key));
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream out(fs::path(dir) / (tag + "_" + key + ".hlsl"), std::ios::binary);
    out.write(static_cast<const char*>(r.src), static_cast<std::streamsize>(r.srcSize));
}

} // namespace HE::d3dcache
