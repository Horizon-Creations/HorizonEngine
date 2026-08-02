#include "doctest.h"
#include <ContentManager/HAsset.h>
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
