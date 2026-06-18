#include "doctest.h"
#include <Hpak/HpakWriter.h>
#include <Hpak/HpakReader.h>
#include <Hpak/KeyDerivation.h>
#include <ContentManager/HAsset.h>
#include <ContentManager/ContentManager.h>
#include <filesystem>
#include <fstream>
#include <cstring>

// Detect whether the test binary was built with LZ4 support.
// We check the kFlagCompressed constant — it's always defined,
// but compression only works at runtime when HE_HAVE_LZ4 is set.
// Test macros guard accordingly so CI without lz4 still passes.
#ifdef HE_HAVE_LZ4
#  define HE_LZ4_AVAILABLE 1
#else
#  define HE_LZ4_AVAILABLE 0
#endif

// ─── Helpers ─────────────────────────────────────────────────────────────────

static std::vector<uint8_t> makeMaterialBlob(const HE::UUID& id,
                                              const std::string& name = "test_mat",
                                              float r = 1.f, float g = 0.f, float b = 0.f)
{
    std::vector<uint8_t> meta;
    const uint16_t typeVal = static_cast<uint16_t>(HE::AssetType::Material);
    HAsset::Writer::appendPOD(meta, typeVal);
    HAsset::Writer::appendPOD(meta, id.hi);
    HAsset::Writer::appendPOD(meta, id.lo);
    HAsset::Writer::appendString(meta, name);
    HAsset::Writer::appendString(meta, "mem://" + name);

    std::vector<uint8_t> mtrl;
    HAsset::Writer::appendString(mtrl, "");
    const uint64_t texCount = 0;
    HAsset::Writer::appendPOD(mtrl, texCount);
    HAsset::Writer::appendPOD(mtrl, r);
    HAsset::Writer::appendPOD(mtrl, g);
    HAsset::Writer::appendPOD(mtrl, b);
    float metallic = 0.f, roughness = 0.5f, opacity = 1.f;
    HAsset::Writer::appendPOD(mtrl, metallic);
    HAsset::Writer::appendPOD(mtrl, roughness);
    HAsset::Writer::appendPOD(mtrl, opacity);

    HAsset::Writer w;
    w.addChunk(HAsset::CHUNK_META, meta.data(), meta.size());
    w.addChunk(HAsset::CHUNK_MTRL, mtrl.data(), mtrl.size());
    return w.toBytes(static_cast<uint16_t>(HE::AssetType::Material));
}

// ─── Tests ───────────────────────────────────────────────────────────────────

TEST_CASE("HpakWriter/HpakReader round-trip single entry")
{
    const HE::UUID id{0xDEADBEEF12345678ULL, 0x0102030405060708ULL};
    auto blob = makeMaterialBlob(id, "mat_single");

    HpakWriter packer;
    packer.addEntry(id, blob);
    CHECK(packer.entryCount() == 1);

    auto tmp = std::filesystem::temp_directory_path() / "he_test_single.hpak";
    REQUIRE(packer.write(tmp.string()));

    HpakReader reader;
    REQUIRE(reader.open(tmp.string()));
    REQUIRE(reader.hasEntry(id));

    const auto out = reader.readEntry(id);
    CHECK(out == blob);

    std::filesystem::remove(tmp);
}

TEST_CASE("HpakWriter/HpakReader multiple entries data integrity")
{
    const HE::UUID id1{1,1}, id2{2,2}, id3{3,3};
    const auto b1 = makeMaterialBlob(id1, "mat1", 1.f,0.f,0.f);
    const auto b2 = makeMaterialBlob(id2, "mat2", 0.f,1.f,0.f);
    const auto b3 = makeMaterialBlob(id3, "mat3", 0.f,0.f,1.f);

    HpakWriter packer;
    packer.addEntry(id1, b1);
    packer.addEntry(id2, b2);
    packer.addEntry(id3, b3);

    auto tmp = std::filesystem::temp_directory_path() / "he_test_multi.hpak";
    REQUIRE(packer.write(tmp.string()));

    HpakReader reader;
    REQUIRE(reader.open(tmp.string()));

    const auto ids = reader.enumerate();
    REQUIRE(ids.size() == 3);

    CHECK(reader.readEntry(id1) == b1);
    CHECK(reader.readEntry(id2) == b2);
    CHECK(reader.readEntry(id3) == b3);

    std::filesystem::remove(tmp);
}

TEST_CASE("HpakReader returns empty for missing entry")
{
    const HE::UUID id{42,42};
    const std::vector<uint8_t> data = {1,2,3};

    HpakWriter packer;
    packer.addEntry(id, data);

    auto tmp = std::filesystem::temp_directory_path() / "he_test_missing.hpak";
    REQUIRE(packer.write(tmp.string()));

    HpakReader reader;
    REQUIRE(reader.open(tmp.string()));

    const HE::UUID missing{99,99};
    CHECK(!reader.hasEntry(missing));
    CHECK(reader.readEntry(missing).empty());

    std::filesystem::remove(tmp);
}

TEST_CASE("Empty HpakWriter writes and reads back correctly")
{
    HpakWriter packer;
    auto tmp = std::filesystem::temp_directory_path() / "he_test_empty.hpak";
    REQUIRE(packer.write(tmp.string()));

    HpakReader reader;
    REQUIRE(reader.open(tmp.string()));
    CHECK(reader.enumerate().empty());

    std::filesystem::remove(tmp);
}

TEST_CASE("HpakWriter encryption round-trip")
{
    const HE::UUID id{0xABCD,0xEF01};
    const std::vector<uint8_t> data = {10,20,30,40,50,60,70,80};

    Hpak::PackSettings settings;
    settings.encrypt = true;
    for (int i = 0; i < 32; ++i) settings.key[i] = static_cast<uint8_t>(i + 1);

    HpakWriter packer;
    packer.addEntry(id, data, settings);

    auto tmp = std::filesystem::temp_directory_path() / "he_test_enc.hpak";
    REQUIRE(packer.write(tmp.string()));

    HpakReader reader;
    REQUIRE(reader.open(tmp.string()));

    // Without key returns XOR-scrambled bytes
    const auto garbled = reader.readEntry(id);
    CHECK(garbled != data);
    CHECK(garbled.size() == data.size());

    // With correct key returns original
    const auto decrypted = reader.readEntry(id, settings.key);
    CHECK(decrypted == data);

    std::filesystem::remove(tmp);
}

TEST_CASE("KeyDerivation is deterministic")
{
    const uint8_t salt[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    uint8_t key1[32] = {}, key2[32] = {};

    KeyDerivation::derive("my_secret_passphrase", salt, key1);
    KeyDerivation::derive("my_secret_passphrase", salt, key2);

    CHECK(std::memcmp(key1, key2, 32) == 0);
}

TEST_CASE("KeyDerivation different secrets yield different keys")
{
    const uint8_t salt[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    uint8_t key1[32] = {}, key2[32] = {};

    KeyDerivation::derive("secret_alpha", salt, key1);
    KeyDerivation::derive("secret_beta",  salt, key2);

    CHECK(std::memcmp(key1, key2, 32) != 0);
}

TEST_CASE("KeyDerivation different salts yield different keys")
{
    const uint8_t salt1[16] = {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};
    const uint8_t salt2[16] = {2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2};
    uint8_t key1[32] = {}, key2[32] = {};

    KeyDerivation::derive("same_secret", salt1, key1);
    KeyDerivation::derive("same_secret", salt2, key2);

    CHECK(std::memcmp(key1, key2, 32) != 0);
}

TEST_CASE("ContentManager::loadPak registers assets")
{
    const HE::UUID matId{0xCAFEBABE11111111ULL, 0x2222222222222222ULL};
    const auto blob = makeMaterialBlob(matId, "pak_mat", 0.5f, 0.7f, 0.9f);

    HpakWriter packer;
    packer.addEntry(matId, blob);

    auto tmp = std::filesystem::temp_directory_path() / "he_test_cm_pak.hpak";
    REQUIRE(packer.write(tmp.string()));

    ContentManager cm;
    REQUIRE(cm.loadPak(tmp.string()));
    REQUIRE(!cm.loadPak("nonexistent.hpak")); // bad path returns false

    const MaterialAsset* mat = cm.getMaterial(matId);
    REQUIRE(mat != nullptr);
    CHECK(mat->baseColor[0] == doctest::Approx(0.5f));
    CHECK(mat->baseColor[1] == doctest::Approx(0.7f));
    CHECK(mat->baseColor[2] == doctest::Approx(0.9f));

    std::filesystem::remove(tmp);
}

TEST_CASE("ContentManager::loadPak skips already-loaded UUIDs")
{
    const HE::UUID matId{0xBEEFCAFE,0xDEAD1234ULL};
    const auto blob = makeMaterialBlob(matId, "dup_mat");

    HpakWriter packer;
    packer.addEntry(matId, blob);

    auto tmp = std::filesystem::temp_directory_path() / "he_test_cm_skip.hpak";
    REQUIRE(packer.write(tmp.string()));

    ContentManager cm;
    cm.loadPak(tmp.string());

    // Loading the same pak again should not fail or duplicate
    CHECK(cm.loadPak(tmp.string()));
    CHECK(cm.getMaterial(matId) != nullptr);

    std::filesystem::remove(tmp);
}

TEST_CASE("HAsset::Writer::toBytes / Reader::openData round-trip")
{
    const HE::UUID id{0x1234567890ABCDEFULL, 0xFEDCBA9876543210ULL};

    std::vector<uint8_t> meta;
    const uint16_t typeVal = static_cast<uint16_t>(HE::AssetType::Material);
    HAsset::Writer::appendPOD(meta, typeVal);
    HAsset::Writer::appendPOD(meta, id.hi);
    HAsset::Writer::appendPOD(meta, id.lo);
    HAsset::Writer::appendString(meta, "blob_mat");
    HAsset::Writer::appendString(meta, "mem://blob_mat");

    HAsset::Writer w;
    w.addChunk(HAsset::CHUNK_META, meta.data(), meta.size());

    const auto bytes = w.toBytes(typeVal);
    CHECK(!bytes.empty());

    HAsset::Reader r;
    REQUIRE(r.openData(bytes));
    CHECK(r.assetType() == typeVal);
    REQUIRE(r.findChunk(HAsset::CHUNK_META) != nullptr);
}

TEST_CASE("HpakWriter::addDirectory skips non-.hasset files")
{
    auto tmpDir = std::filesystem::temp_directory_path() / "he_test_hpak_dir";
    std::filesystem::create_directories(tmpDir);

    // Write a non-.hasset file
    { std::ofstream f(tmpDir / "notes.txt"); f << "not an asset"; }
    // Write a fake .hasset (no META → skipped)
    { std::ofstream f(tmpDir / "dummy.hasset", std::ios::binary); f << "FAKE"; }

    HpakWriter packer;
    const int added = packer.addDirectory(tmpDir);
    CHECK(added == 0); // fake data has no valid META UUID

    std::filesystem::remove_all(tmpDir);
}

// ─── LZ4 compression tests ────────────────────────────────────────────────────

TEST_CASE("HpakWriter compression flag: compress=false stores raw data")
{
    const HE::UUID id{0x1234,0x5678};
    const std::vector<uint8_t> data(256, 0xAB); // easily compressible

    Hpak::PackSettings settings;
    settings.compress = false; // explicit off

    HpakWriter packer;
    packer.addEntry(id, data, settings);

    auto tmp = std::filesystem::temp_directory_path() / "he_test_nocomp.hpak";
    REQUIRE(packer.write(tmp.string()));

    HpakReader reader;
    REQUIRE(reader.open(tmp.string()));
    const auto out = reader.readEntry(id);
    CHECK(out == data);

    std::filesystem::remove(tmp);
}

#if HE_LZ4_AVAILABLE
TEST_CASE("HpakWriter LZ4 compression round-trip")
{
    const HE::UUID id{0xC0C0,0xD0D0};
    // Highly compressible payload (repeated bytes compress very well)
    std::vector<uint8_t> data(4096, 0x42);

    Hpak::PackSettings settings;
    settings.compress = true;

    HpakWriter packer;
    packer.addEntry(id, data, settings);

    auto tmp = std::filesystem::temp_directory_path() / "he_test_lz4.hpak";
    REQUIRE(packer.write(tmp.string()));

    // The on-disk file should be smaller than uncompressed (4096 bytes of 0x42)
    const auto fileSize = std::filesystem::file_size(tmp);
    CHECK(fileSize < sizeof(Hpak::FileHeader) + sizeof(Hpak::EntryDesc) + data.size());

    HpakReader reader;
    REQUIRE(reader.open(tmp.string()));
    const auto out = reader.readEntry(id);
    REQUIRE(out.size() == data.size());
    CHECK(out == data);

    std::filesystem::remove(tmp);
}

TEST_CASE("HpakWriter LZ4 compression + encryption round-trip")
{
    const HE::UUID id{0xE0E0,0xF0F0};
    std::vector<uint8_t> data(2048);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i & 0xFF);

    Hpak::PackSettings settings;
    settings.compress = true;
    settings.encrypt  = true;
    for (int i = 0; i < 32; ++i) settings.key[i] = static_cast<uint8_t>(i + 7);

    HpakWriter packer;
    packer.addEntry(id, data, settings);

    auto tmp = std::filesystem::temp_directory_path() / "he_test_lz4enc.hpak";
    REQUIRE(packer.write(tmp.string()));

    HpakReader reader;
    REQUIRE(reader.open(tmp.string()));

    // Without key: still encrypted → result is not the original
    const auto garbled = reader.readEntry(id);
    CHECK(garbled != data);

    // With key: decrypt then decompress → original
    const auto out = reader.readEntry(id, settings.key);
    REQUIRE(out.size() == data.size());
    CHECK(out == data);

    std::filesystem::remove(tmp);
}

TEST_CASE("HpakWriter: compress=true on already-small data still round-trips")
{
    const HE::UUID id{0x1111,0x2222};
    const std::vector<uint8_t> data = {1, 2, 3, 4, 5}; // may not compress smaller

    Hpak::PackSettings settings;
    settings.compress = true;

    HpakWriter packer;
    packer.addEntry(id, data, settings);

    auto tmp = std::filesystem::temp_directory_path() / "he_test_lz4tiny.hpak";
    REQUIRE(packer.write(tmp.string()));

    HpakReader reader;
    REQUIRE(reader.open(tmp.string()));
    const auto out = reader.readEntry(id);
    CHECK(out == data);

    std::filesystem::remove(tmp);
}
#endif // HE_LZ4_AVAILABLE
