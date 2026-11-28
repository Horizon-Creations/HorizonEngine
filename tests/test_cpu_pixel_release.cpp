#include "doctest.h"
#include "TestFsUtil.h"
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <Hpak/HpakWriter.h>

// The shipped game keeps each block-compressed texture once. A backend tells the content manager
// when it has uploaded one; the manager frees the CPU bytes where nothing on the CPU reads them
// and gives them back from the pak when a second upload asks.

namespace fs = std::filesystem;

namespace
{

std::vector<uint8_t> fileBytes(const fs::path& p)
{
	std::ifstream f(p, std::ios::binary);
	return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// A texture asset of the given format and layer count with recognisable bytes.
TextureAsset makeTexture(const char* name, TextureFormat format, uint32_t layers = 1,
                         const std::vector<float>& means = {})
{
	TextureAsset t;
	t.type = HE::AssetType::Texture; t.name = name; t.path = std::string(name) + ".hasset";
	t.width = 8; t.height = 8; t.channels = 4; t.format = format; t.layers = layers;
	t.sliceMeans = means;
	const size_t bytes = format == TextureFormat::RGBA8 ? 8 * 8 * 4 * layers : 64 * layers;
	t.data.resize(bytes);
	for (size_t i = 0; i < bytes; ++i) t.data[i] = static_cast<uint8_t>(i * 7 + 3);
	return t;
}

// Saves the textures loose, packs them (Store) and returns the pak path and the ids in order.
struct Packed
{
	fs::path dir, pak;
	std::vector<HE::UUID> ids;
	std::vector<std::vector<uint8_t>> original;
	~Packed() { he_test::removeAllQuiet(dir); }
};

void build(Packed& p, const char* tag, std::vector<TextureAsset> textures)
{
	p.dir = fs::temp_directory_path() / (std::string("he_cpupix_") + tag);
	he_test::removeAllQuiet(p.dir);
	fs::create_directories(p.dir);
	HpakWriter packer;
	{
		ContentManager cm(p.dir.string());
		for (TextureAsset& t : textures)
		{
			REQUIRE(cm.saveAsset(t));
			p.ids.push_back(t.id);
			p.original.push_back(t.data);
			Hpak::PackSettings s; s.codec = Hpak::Codec::Store;
			packer.addEntry(t.id, fileBytes(p.dir / t.path), s);
		}
	}
	p.pak = p.dir / "tex.hpak";
	REQUIRE(packer.write(p.pak.string()));
}

} // namespace

TEST_CASE("CPU pixels: off by default, a compressed texture from a pak is freed when enabled and comes back on request")
{
	Packed p;
	build(p, "basic", { makeTexture("astc", TextureFormat::ASTC_4x4) });
	ContentManager cm;
	REQUIRE(cm.mountPak(p.pak.string()));
	REQUIRE(cm.ensureResident(p.ids[0]));
	REQUIRE(cm.getTexture(p.ids[0]) != nullptr);
	CHECK(cm.getTexture(p.ids[0])->data == p.original[0]);

	// Off: nothing is released.
	CHECK_FALSE(cm.releaseCpuPixels(p.ids[0]));
	CHECK(cm.getTexture(p.ids[0])->data == p.original[0]);

	cm.setReleaseCpuPixelsAfterUpload(true);
	CHECK(cm.releaseCpuPixels(p.ids[0]));
	CHECK(cm.getTexture(p.ids[0])->data.empty());
	CHECK(cm.getTexture(p.ids[0])->data.capacity() == 0);      // the memory is really gone
	CHECK_FALSE(cm.releaseCpuPixels(p.ids[0]));                // nothing left to release

	// A second upload asks for the bytes and gets exactly the packed ones.
	const TextureAsset* back = cm.restoreCpuPixels(p.ids[0]);
	REQUIRE(back != nullptr);
	CHECK(back->data == p.original[0]);
	CHECK(cm.releaseCpuPixels(p.ids[0]));                      // and can go again
	CHECK(cm.restoreCpuPixels(p.ids[0])->data == p.original[0]);

	// An unknown id is a null, not a crash.
	CHECK(cm.restoreCpuPixels(HE::UUID{ 7, 7 }) == nullptr);
	CHECK_FALSE(cm.releaseCpuPixels(HE::UUID{ 7, 7 }));
}

TEST_CASE("CPU pixels: what the CPU reads is never freed")
{
	Packed p;
	build(p, "kept", {
		makeTexture("rgba", TextureFormat::RGBA8),                                  // terrain weights, trails, widgets
		makeTexture("arrNoMeans", TextureFormat::ASTC_4x4, 2),                      // the GI reads its pixels
		makeTexture("arrMeans", TextureFormat::ASTC_4x4, 2, { 1, 1, 1, 0, 0, 0 }),  // means are baked: free
	});
	ContentManager cm;
	cm.setReleaseCpuPixelsAfterUpload(true);
	REQUIRE(cm.mountPak(p.pak.string()));
	for (const HE::UUID& id : p.ids) REQUIRE(cm.ensureResident(id));

	CHECK_FALSE(cm.releaseCpuPixels(p.ids[0]));
	CHECK(cm.getTexture(p.ids[0])->data == p.original[0]);
	CHECK_FALSE(cm.releaseCpuPixels(p.ids[1]));
	CHECK(cm.getTexture(p.ids[1])->data == p.original[1]);
	CHECK(cm.releaseCpuPixels(p.ids[2]));
	CHECK(cm.getTexture(p.ids[2])->data.empty());
	CHECK(cm.restoreCpuPixels(p.ids[2])->data == p.original[2]);
}

TEST_CASE("CPU pixels: a loose texture (no pak to give the bytes back) is never freed")
{
	Packed p;
	build(p, "loose", { makeTexture("astc", TextureFormat::ASTC_4x4) });
	ContentManager cm(p.dir.string());
	cm.setReleaseCpuPixelsAfterUpload(true);
	const HE::UUID id = cm.loadAsset("astc.hasset");
	REQUIRE(cm.getTexture(id) != nullptr);
	CHECK_FALSE(cm.releaseCpuPixels(id));
	CHECK(cm.getTexture(id)->data == p.original[0]);
}
