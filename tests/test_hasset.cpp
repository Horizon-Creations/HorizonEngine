#include "doctest.h"
#include <ContentManager/HAsset.h>
#include <Types/UUID.h>
#include <cstdio>
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
