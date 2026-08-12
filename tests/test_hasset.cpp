#include "doctest.h"
#include <ContentManager/HAsset.h>
#include <Types/Enums.h>
#include <Types/UUID.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>

TEST_CASE("HAsset writer/reader round-trip")
{
	const std::string file =
		(std::filesystem::temp_directory_path() / "he_test_roundtrip.hasset").string();

	// Write
	{
		HAsset::Writer w;

		std::vector<uint8_t> meta;
		HAsset::Writer::appendPOD(meta, static_cast<uint16_t>(3)); // arbitrary type
		HAsset::Writer::appendString(meta, "TestAsset");

		std::vector<float> verts = { 1.0f, 2.0f, 3.0f, 4.0f };
		std::vector<uint8_t> vertBuf;
		HAsset::Writer::appendVec(vertBuf, verts);

		w.addChunk(HAsset::CHUNK_META, meta.data(), meta.size());
		w.addChunk(HAsset::CHUNK_VERT, vertBuf.data(), vertBuf.size());
		REQUIRE(w.write(file, 3));
	}

	// Read
	{
		HAsset::Reader r;
		REQUIRE(r.open(file));
		CHECK(r.assetType() == 3);
		CHECK(r.header().version == HAsset::k_version);
		CHECK(r.header().chunk_count == 2);

		const auto* meta = r.findChunk(HAsset::CHUNK_META);
		REQUIRE(meta != nullptr);
		size_t off = 0;
		uint16_t type = 0;
		std::string name;
		REQUIRE(HAsset::Reader::readPOD(meta->data, off, type));
		REQUIRE(HAsset::Reader::readString(meta->data, off, name));
		CHECK(type == 3);
		CHECK(name == "TestAsset");

		const auto* vert = r.findChunk(HAsset::CHUNK_VERT);
		REQUIRE(vert != nullptr);
		off = 0;
		std::vector<float> verts;
		REQUIRE(HAsset::Reader::readVec(vert->data, off, verts));
		REQUIRE(verts.size() == 4);
		CHECK(verts[0] == 1.0f);
		CHECK(verts[3] == 4.0f);

		CHECK(r.findChunk(HAsset::CHUNK_PIXL) == nullptr);
	}

	std::remove(file.c_str());
}

TEST_CASE("HAsset reader rejects garbage")
{
	const std::string file =
		(std::filesystem::temp_directory_path() / "he_test_garbage.hasset").string();
	{
		std::ofstream f(file, std::ios::binary);
		f << "this is not a hasset file at all............";
	}
	HAsset::Reader r;
	CHECK_FALSE(r.open(file));
	std::remove(file.c_str());
}

TEST_CASE("HAsset string read guards against truncation")
{
	std::vector<uint8_t> buf;
	HAsset::Writer::appendString(buf, "hello");
	buf.resize(buf.size() - 2); // truncate payload

	size_t off = 0;
	std::string out;
	CHECK_FALSE(HAsset::Reader::readString(buf, off, out));
}

TEST_CASE("HAsset asset_type is preserved in the file header")
{
	const std::string file =
		(std::filesystem::temp_directory_path() / "he_test_assettype.hasset").string();
	{
		HAsset::Writer w;
		REQUIRE(w.write(file, 7)); // type = 7 (arbitrary)
	}
	HAsset::Reader r;
	REQUIRE(r.open(file));
	CHECK(r.assetType()             == 7);
	CHECK(r.header().version        == HAsset::k_version);
	CHECK(r.header().chunk_count    == 0);
	std::remove(file.c_str());
}

TEST_CASE("HAsset supports empty-payload chunks")
{
	const std::string file =
		(std::filesystem::temp_directory_path() / "he_test_emptychunk.hasset").string();
	{
		HAsset::Writer w;
		w.addChunk(HAsset::CHUNK_META, nullptr, 0); // zero bytes of data
		REQUIRE(w.write(file, 1));
	}
	HAsset::Reader r;
	REQUIRE(r.open(file));
	CHECK(r.header().chunk_count == 1);
	const auto* c = r.findChunk(HAsset::CHUNK_META);
	REQUIRE(c != nullptr);
	CHECK(c->data.empty());
	std::remove(file.c_str());
}

TEST_CASE("HAsset UUID round-trip via appendPOD / readPOD")
{
	const HE::UUID original = HE::UUID::generate();

	std::vector<uint8_t> buf;
	HAsset::Writer::appendPOD(buf, original.hi);
	HAsset::Writer::appendPOD(buf, original.lo);

	size_t off = 0;
	HE::UUID recovered{};
	CHECK(HAsset::Reader::readPOD(buf, off, recovered.hi));
	CHECK(HAsset::Reader::readPOD(buf, off, recovered.lo));
	CHECK(recovered == original);
	CHECK(off == buf.size());
}

TEST_CASE("HAsset readVec guards against a truncated payload")
{
	std::vector<uint8_t> buf;
	std::vector<float> data = { 1.0f, 2.0f, 3.0f };
	HAsset::Writer::appendVec(buf, data);
	buf.resize(buf.size() - 4); // chop the last float

	size_t off = 0;
	std::vector<float> out;
	CHECK_FALSE(HAsset::Reader::readVec(buf, off, out));
}

TEST_CASE("HAsset write fails gracefully on an unwritable path")
{
	// A path whose parent directory does not exist cannot be opened for writing.
	const std::string bad = (std::filesystem::temp_directory_path() /
	                         "nonexistent_dir_he" / "file.hasset").string();
	HAsset::Writer w;
	CHECK_FALSE(w.write(bad, 1));
}

// ── Truncated / corrupt chunk tails ───────────────────────────────────────────
// Chunk formats grow by APPENDING fields, so an older asset simply runs out of
// bytes partway through the read sequence. That has to fail cleanly — but the
// string-vector reader used to resize() to an unvalidated 64-bit count first,
// which for a garbage value throws bad_alloc and aborts the process.
TEST_CASE("readVec(strings) rejects an out-of-range count instead of allocating")
{
    // A count that could never fit: each element needs at least a 4-byte prefix.
    std::vector<uint8_t> buf;
    const uint64_t bogus = 0xFFFFFFFFFFFFULL;
    HAsset::Writer::appendPOD(buf, bogus);
    buf.push_back(0); buf.push_back(0);   // a couple of trailing bytes, far too few

    size_t off = 0;
    std::vector<std::string> out;
    CHECK_FALSE(HAsset::Reader::readVec(buf, off, out));
    CHECK(out.empty());                    // and nothing was allocated

    // A count that fits the buffer but whose strings do not still fails cleanly.
    std::vector<uint8_t> buf2;
    const uint64_t two = 2;
    HAsset::Writer::appendPOD(buf2, two);
    HAsset::Writer::appendString(buf2, "ok");
    // second element missing entirely
    size_t off2 = 0;
    std::vector<std::string> out2;
    CHECK_FALSE(HAsset::Reader::readVec(buf2, off2, out2));

    // The valid case still round-trips.
    std::vector<uint8_t> good;
    const uint64_t n = 2;
    HAsset::Writer::appendPOD(good, n);
    HAsset::Writer::appendString(good, "Grass");
    HAsset::Writer::appendString(good, "Rock");
    size_t off3 = 0;
    std::vector<std::string> out3;
    REQUIRE(HAsset::Reader::readVec(good, off3, out3));
    REQUIRE(out3.size() == 2);
    CHECK(out3[0] == "Grass");
    CHECK(out3[1] == "Rock");
    CHECK(off3 == good.size());
}

// A material written BEFORE a tail field existed must still parse — the reader
// walks past the end of its MTRL chunk and every field after the truncation
// keeps its default. This is the exact shape the packaged-asset tests exercise.
TEST_CASE("material chunk truncated before its tail fields parses cleanly")
{
    std::vector<uint8_t> mtrl;
    HAsset::Writer::appendString(mtrl, "");          // shaderPath
    const uint64_t texCount = 0;
    HAsset::Writer::appendPOD(mtrl, texCount);       // texturePaths
    for (float f : { 1.0f, 0.0f, 0.0f, 0.0f, 0.5f, 1.0f })
        HAsset::Writer::appendPOD(mtrl, f);          // baseColor/metallic/roughness/opacity
    // …and nothing else: no custom shader, no graph data, no layer names.

    size_t o = 0;
    std::string shaderPath; std::vector<std::string> texturePaths;
    CHECK(HAsset::Reader::readString(mtrl, o, shaderPath));
    CHECK(HAsset::Reader::readVec(mtrl, o, texturePaths));
    float v = 0.0f;
    for (int i = 0; i < 6; ++i) CHECK(HAsset::Reader::readPOD(mtrl, o, v));
    // Every tail read past this point fails without touching the process.
    std::string frag, vert;
    CHECK_FALSE(HAsset::Reader::readString(mtrl, o, frag));
    std::vector<std::string> layerNames;
    CHECK_FALSE(HAsset::Reader::readVec(mtrl, o, layerNames));
    CHECK(layerNames.empty());
}

// ── Integer-overflow guards ───────────────────────────────────────────────────
// Every length in a .hasset comes from the file, so the bounds checks must not
// be computable into a pass: `count * sizeof(T)` and `offset + size` both wrap
// in unsigned 64-bit arithmetic. The checks therefore divide/subtract instead.
TEST_CASE("readVec(POD) rejects a count whose byte size would wrap around")
{
    // count * sizeof(float) == 2^64 == 0 (mod 2^64): the old `offset + count *
    // sizeof(T) > buf.size()` check saw a size of ZERO and let this through,
    // then resize()d to 4.6 quintillion floats → bad_alloc out of the parse.
    std::vector<uint8_t> buf;
    const uint64_t wrapping = 0x4000000000000000ULL;
    HAsset::Writer::appendPOD(buf, wrapping);
    for (int i = 0; i < 4; ++i) buf.push_back(0xAB);   // one float of payload

    size_t off = 0;
    std::vector<float> out;
    CHECK_FALSE(HAsset::Reader::readVec(buf, off, out));
    CHECK(out.empty());                                 // nothing was allocated

    // Same trick against an 8-byte element type (2^61 * 8 == 2^64).
    std::vector<uint8_t> buf64;
    HAsset::Writer::appendPOD(buf64, static_cast<uint64_t>(0x2000000000000000ULL));
    for (int i = 0; i < 8; ++i) buf64.push_back(0x11);
    size_t off64 = 0;
    std::vector<uint64_t> out64;
    CHECK_FALSE(HAsset::Reader::readVec(buf64, off64, out64));
    CHECK(out64.empty());

    // A merely absurd (non-wrapping) count is rejected as before …
    std::vector<uint8_t> huge;
    HAsset::Writer::appendPOD(huge, static_cast<uint64_t>(0xFFFFFFFFULL));
    for (int i = 0; i < 4; ++i) huge.push_back(0x00);
    size_t offH = 0;
    std::vector<float> outH;
    CHECK_FALSE(HAsset::Reader::readVec(huge, offH, outH));

    // … and the honest case still round-trips exactly.
    std::vector<uint8_t> good;
    const std::vector<float> src = { 1.0f, 2.0f, 3.0f };
    HAsset::Writer::appendVec(good, src);
    size_t offG = 0;
    std::vector<float> outG;
    REQUIRE(HAsset::Reader::readVec(good, offG, outG));
    REQUIRE(outG.size() == 3);
    CHECK(outG[0] == 1.0f);
    CHECK(outG[2] == 3.0f);
    CHECK(offG == good.size());
}

// Helper: write a hand-crafted .hasset whose single chunk header lies about its
// payload size. Returns the path.
static std::string writeLyingChunkFile(const char* name, uint64_t declaredSize,
                                       size_t actualPayloadBytes)
{
    const std::string file = (std::filesystem::temp_directory_path() / name).string();
    HAsset::FileHeader hdr{};
    std::memcpy(hdr.magic, HAsset::k_magic, 4);
    hdr.version     = HAsset::k_version;
    hdr.asset_type  = 1;
    hdr.chunk_count = 1;
    HAsset::ChunkHeader ch{};
    ch.id   = HAsset::CHUNK_SRC;
    ch.size = declaredSize;

    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    f.write(reinterpret_cast<const char*>(&ch),  sizeof(ch));
    const std::vector<char> payload(actualPayloadBytes, 'x');
    if (!payload.empty()) f.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    return file;
}

TEST_CASE("HAsset open() rejects a chunk size larger than the file")
{
    // 281 TB declared, 4 bytes present: open() used to resize() to the declared
    // size (bad_alloc) and, for merely-truncated files, zero-fill the tail and
    // report success. openData() has always bounded the size; open() had not.
    const std::string absurd = writeLyingChunkFile("he_test_chunk_absurd.hasset",
                                                   0x0000FFFFFFFFFFFFULL, 4);
    HAsset::Reader r1;
    CHECK_FALSE(r1.open(absurd));
    CHECK(r1.chunks().empty());
    std::remove(absurd.c_str());

    // The plausible-but-truncated case fails cleanly too instead of silently
    // handing out a chunk padded with zeroes.
    const std::string trunc = writeLyingChunkFile("he_test_chunk_trunc.hasset", 4096, 8);
    HAsset::Reader r2;
    CHECK_FALSE(r2.open(trunc));
    std::remove(trunc.c_str());

    // An honest file of the same shape still loads.
    const std::string okFile = writeLyingChunkFile("he_test_chunk_ok.hasset", 8, 8);
    HAsset::Reader r3;
    REQUIRE(r3.open(okFile));
    REQUIRE(r3.chunks().size() == 1);
    CHECK(r3.findChunk(HAsset::CHUNK_SRC)->data.size() == 8);
    std::remove(okFile.c_str());

    // A file shorter than the fixed header is not a .hasset at all.
    const std::string stub = (std::filesystem::temp_directory_path() /
                              "he_test_header_stub.hasset").string();
    { std::ofstream f(stub, std::ios::binary | std::ios::trunc); f << "HAST"; }
    HAsset::Reader r4;
    CHECK_FALSE(r4.open(stub));
    std::remove(stub.c_str());
}

TEST_CASE("HAsset chunk_count is capped by what the file can hold")
{
    // 4 billion chunks declared, zero bytes of chunk data: reserving the raw
    // count asks the allocator for ~130 GB of Chunk structs before the read loop
    // can notice the file is empty. The cap keeps the reserve at what the file
    // could physically contain (here: none).
    std::vector<uint8_t> blob(sizeof(HAsset::FileHeader), 0);
    HAsset::FileHeader hdr{};
    std::memcpy(hdr.magic, HAsset::k_magic, 4);
    hdr.version     = HAsset::k_version;
    hdr.asset_type  = 1;
    hdr.chunk_count = 0xFFFFFFFFu;
    std::memcpy(blob.data(), &hdr, sizeof(hdr));

    HAsset::Reader r;
    CHECK_NOTHROW(r.openData(blob));
    CHECK(r.chunks().empty());

    const std::string file = (std::filesystem::temp_directory_path() /
                              "he_test_chunkcount.hasset").string();
    { std::ofstream f(file, std::ios::binary | std::ios::trunc);
      f.write(reinterpret_cast<const char*>(blob.data()),
              static_cast<std::streamsize>(blob.size())); }
    HAsset::Reader r2;
    CHECK_NOTHROW(r2.open(file));
    CHECK(r2.chunks().empty());
    std::remove(file.c_str());
}

// The asset pickers scan whole content trees just to ask "what type is this?".
// Reader::open() answers that by loading every chunk of every file — which on a
// tree with big meshes means reading gigabytes to fill a dropdown. The header-only
// read must give the same answer for a fraction of the I/O.
TEST_CASE("HAsset readAssetTypeFromFile matches Reader::open without loading chunks")
{
    std::vector<uint8_t> payload(1u << 20, 0xAB); // 1 MiB of chunk data
    HAsset::Writer w;
    w.addChunk(HAsset::CHUNK_META, payload.data(), payload.size());
    const uint16_t type = static_cast<uint16_t>(HE::AssetType::StaticMesh);
    const auto bytes = w.toBytes(type);

    const std::string file = (std::filesystem::temp_directory_path() /
                              "he_test_headeronly.hasset").string();
    { std::ofstream f(file, std::ios::binary | std::ios::trunc);
      f.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size())); }

    uint16_t quick = 0;
    REQUIRE(HAsset::readAssetTypeFromFile(file, quick));
    HAsset::Reader r;
    REQUIRE(r.open(file));
    CHECK(quick == r.assetType());
    CHECK(quick == type);

    // A file that is not an .hasset is rejected rather than mis-typed.
    const std::string junk = (std::filesystem::temp_directory_path() /
                              "he_test_headeronly.txt").string();
    { std::ofstream f(junk, std::ios::binary | std::ios::trunc); f << "not an asset at all"; }
    uint16_t ignored = 0xFFFF;
    CHECK_FALSE(HAsset::readAssetTypeFromFile(junk, ignored));
    CHECK_FALSE(HAsset::readAssetTypeFromFile(
        (std::filesystem::temp_directory_path() / "he_test_missing.hasset").string(), ignored));

    std::remove(file.c_str());
    std::remove(junk.c_str());
}

// ── Committing a write ────────────────────────────────────────────────────────
// Included down here so the block above stays untouched: these cases need the
// packer, which the format header itself has no business knowing about.
#include "TestFsUtil.h"
#include <Hpak/HpakReader.h>
#include <Hpak/HpakWriter.h>

// write() used to open the TARGET with ios::trunc: the existing asset was gone
// before the first byte of the new one was written, so anything that interrupted
// the write left a stump. Losing the bytes is the small half — an .hasset carries
// its own UUID in META, and that UUID is what every scene in the project stores,
// so a stump is an asset the whole project has lost track of. It now writes a
// sibling temp file and renames it over the target.
TEST_CASE("HAsset write leaves the original intact when it cannot be committed")
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "he_hasset_atomic";
    he_test::removeAllQuiet(dir);
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / "Rock.hasset";

    // An asset that already exists, and is deliberately BIGGER than its
    // replacement: a truncating write that dies partway leaves a prefix, which a
    // size check alone would not catch.
    {
        HAsset::Writer w;
        const std::vector<uint8_t> payload(4096, 0x5A);
        w.addChunk(HAsset::CHUNK_META, payload.data(), payload.size());
        REQUIRE(w.write(file.string(), 3));
    }
    std::vector<uint8_t> before;
    {
        std::ifstream f(file, std::ios::binary);
        before.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    REQUIRE_FALSE(before.empty());

    // Block the commit by parking a DIRECTORY on the temp name — the one failure
    // that behaves the same on every platform this ships to (a read-only
    // directory is ignored by Windows and by a root-running CI container).
    const std::filesystem::path blocker = file.string() + ".tmp";
    std::filesystem::create_directories(blocker);

    HAsset::Writer w2;
    const std::vector<uint8_t> small(8, 0x11);
    w2.addChunk(HAsset::CHUNK_VERT, small.data(), small.size());
    CHECK_FALSE(w2.write(file.string(), 3));

    std::vector<uint8_t> after;
    {
        std::ifstream f(file, std::ios::binary);
        after.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    CHECK(after == before);                       // byte-identical, not merely present

    // With the blocker gone the same write commits — and leaves no temp file
    // beside the asset for the Content Browser to trip over.
    he_test::removeAllQuiet(blocker);
    CHECK(w2.write(file.string(), 3));
    CHECK_FALSE(std::filesystem::exists(blocker));

    HAsset::Reader r;
    REQUIRE(r.open(file.string()));
    CHECK(r.header().chunk_count == 1);
    REQUIRE(r.findChunk(HAsset::CHUNK_VERT) != nullptr);
    CHECK(r.findChunk(HAsset::CHUNK_VERT)->data.size() == 8);
    CHECK(r.findChunk(HAsset::CHUNK_META) == nullptr); // fully replaced, no old tail

    he_test::removeAllQuiet(dir);
}

// The import source in META is an absolute path on the AUTHOR's machine. It is
// what Reimport needs and what a player must never receive: packed verbatim it
// ships the author's user name and directory layout inside the .hpak. The packer
// blanks it; everything else about the asset stays.
TEST_CASE("packing blanks the import source path out of META")
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "he_hasset_srcstrip";
    const std::filesystem::path pak = std::filesystem::temp_directory_path() / "he_hasset_srcstrip.hpak";
    he_test::removeAllQuiet(dir);
    he_test::removeQuiet(pak);
    std::filesystem::create_directories(dir);

    const HE::UUID    id{ 0x51, 0x52 };
    const std::string source = "/Users/authorname/Art/Meshes/rock.fbx";
    {
        std::vector<uint8_t> meta;
        HAsset::Writer::appendPOD(meta, static_cast<uint16_t>(HE::AssetType::Texture));
        HAsset::Writer::appendPOD(meta, id.hi);
        HAsset::Writer::appendPOD(meta, id.lo);
        HAsset::Writer::appendString(meta, "Rock");
        HAsset::Writer::appendString(meta, "Rock.hasset");
        HAsset::Writer::appendString(meta, source);   // the appended import-source tail
        HAsset::Writer w;
        w.addChunk(HAsset::CHUNK_META, meta.data(), meta.size());
        REQUIRE(w.write((dir / "Rock.hasset").string(),
                        static_cast<uint16_t>(HE::AssetType::Texture)));
    }

    HpakWriter packer;
    Hpak::PackSettings settings;                  // Store, unencrypted: bytes ship verbatim
    REQUIRE(packer.addDirectory(dir, settings) == 1);
    REQUIRE(packer.write(pak.string()));

    // Nowhere in the archive — not in the entry, and not in the file either.
    std::string archive;
    {
        std::ifstream f(pak, std::ios::binary);
        archive.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    CHECK(archive.find(source) == std::string::npos);
    CHECK(archive.find("authorname") == std::string::npos);

    HpakReader reader;
    REQUIRE(reader.open(pak.string()));
    const std::vector<uint8_t> entry = reader.readEntry(id);
    REQUIRE_FALSE(entry.empty());

    HAsset::Reader r;
    REQUIRE(r.openData(entry));
    const auto* meta = r.findChunk(HAsset::CHUNK_META);
    REQUIRE(meta != nullptr);
    size_t   off = sizeof(uint16_t);
    HE::UUID packedId{};
    std::string name, path, packedSource;
    REQUIRE(HAsset::Reader::readPOD(meta->data, off, packedId.hi));
    REQUIRE(HAsset::Reader::readPOD(meta->data, off, packedId.lo));
    REQUIRE(HAsset::Reader::readString(meta->data, off, name));
    REQUIRE(HAsset::Reader::readString(meta->data, off, path));
    CHECK(packedId == id);                        // identity survives …
    CHECK(name == "Rock");                        // … and so does everything the
    CHECK(path == "Rock.hasset");                 //     runtime actually reads
    // The field itself is still THERE, just empty — the tail is append-only, and
    // cutting it off would silently drop any field appended after it later.
    REQUIRE(HAsset::Reader::readString(meta->data, off, packedSource));
    CHECK(packedSource.empty());
    CHECK(off == meta->data.size());

    he_test::removeAllQuiet(dir);
    he_test::removeQuiet(pak);
}
