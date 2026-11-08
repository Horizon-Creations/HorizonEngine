#include "doctest.h"
#include "Impostor.h"
#include <HorizonScene/AutoLodApply.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/DefaultAssets.h>
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// A 1 x 1 quad standing in the XY plane at z = 0, facing +Z, pivot at the base, UV 0..1.
StaticMeshAsset quadMesh(float width = 1.0f, float height = 1.0f)
{
	StaticMeshAsset m;
	m.type = HE::AssetType::StaticMesh;
	m.path = "Plants/Leaf.hasset";
	const float x = width * 0.5f;
	const float p[4][3] = { { -x, 0, 0 }, { x, 0, 0 }, { x, height, 0 }, { -x, height, 0 } };
	const float uv[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
	for (int k = 0; k < 4; ++k)
	{
		m.vertices.insert(m.vertices.end(), { p[k][0], p[k][1], p[k][2] });
		m.normals.insert(m.normals.end(), { 0.0f, 0.0f, 1.0f });
		m.uvs.insert(m.uvs.end(), { uv[k][0], uv[k][1] });
	}
	m.indices = { 0, 1, 2, 0, 2, 3 };
	return m;
}

// 4 x 4 texels, rows bottom-up: (tx + ty) even is opaque red/green by row, odd is cut out.
std::vector<uint8_t> checker()
{
	std::vector<uint8_t> px(4 * 4 * 4, 0);
	for (int ty = 0; ty < 4; ++ty)
		for (int tx = 0; tx < 4; ++tx)
		{
			uint8_t* t = &px[(static_cast<size_t>(ty) * 4 + tx) * 4];
			const bool on = ((tx + ty) % 2) == 0;
			t[0] = on ? 200 : 0; t[1] = on ? static_cast<uint8_t>(40 + 50 * ty) : 0; t[2] = 30;
			t[3] = on ? 255 : 0;
		}
	return px;
}

const uint8_t* texel(const Impostor::Baked& b, int x, int y)
{
	return &b.rgba[(static_cast<size_t>(y) * b.width + x) * 4];
}

} // namespace

TEST_CASE("Impostor: the picture is the plant seen from the front, alpha where the texture is, rows bottom-up")
{
	const StaticMeshAsset mesh = quadMesh();
	const std::vector<uint8_t> px = checker();
	Impostor::SectionSource src;
	src.rgba = px.data(); src.width = 4; src.height = 4;
	Impostor::Settings s;
	s.size = 64;
	const Impostor::Baked b = Impostor::bake(mesh, { src }, s);
	REQUIRE(b.ok());
	CHECK(b.width == 128);
	CHECK(b.height == 64);
	CHECK(b.halfWidth == doctest::Approx(0.5f * 1.01f));
	CHECK(b.y0 == doctest::Approx(0.0f));

	// Texel centres of the front view (left half): texture texel (tx, ty) covers atlas
	// columns [tx/4, (tx+1)/4) of the plant's width and rows [ty/4, (ty+1)/4) of its
	// height, bottom-up — modulo the 1 % margin around the plant.
	for (int ty = 0; ty < 4; ++ty)
		for (int tx = 0; tx < 4; ++tx)
		{
			const float plantX = -0.5f + (static_cast<float>(tx) + 0.5f) / 4.0f;
			const float plantY = (static_cast<float>(ty) + 0.5f) / 4.0f;
			const int ax = static_cast<int>((plantX + b.halfWidth) / (2.0f * b.halfWidth) * 64.0f);
			const int ay = static_cast<int>((plantY - b.y0) / (b.y1 - b.y0) * 64.0f);
			const uint8_t* t = texel(b, ax, ay);
			const bool on = ((tx + ty) % 2) == 0;
			CAPTURE(tx); CAPTURE(ty);
			CHECK(t[3] == (on ? 255 : 0));
			if (on)
			{
				CHECK(t[0] == 200);
				CHECK(t[1] == 40 + 50 * ty);
			}
		}
	CHECK(b.coverage[0] > 0.3f);
	CHECK(b.coverage[0] < 0.6f);

	// The side view looks at the quad edge-on: nothing there.
	CHECK(b.coverage[1] == 0.0f);

	// A cut-out texel's colour is bled from its neighbours, not left black: the texture
	// texel (1, 0) is cut out, and its atlas block is x 16..32, y 0..16.
	const uint8_t* cut = texel(b, 18, 8);
	CHECK(cut[3] == 0);
	CHECK(cut[0] > 0);
}

TEST_CASE("Impostor: a slanted edge gets partial alpha, a flat colour section needs no texture")
{
	StaticMeshAsset mesh = quadMesh();
	// Cut the quad down to its lower-left triangle: the diagonal crosses texels.
	mesh.indices = { 0, 1, 3 };
	Impostor::SectionSource flat;
	flat.alphaTest = false;
	flat.tint[0] = 1.0f; flat.tint[1] = 1.0f; flat.tint[2] = 1.0f;
	Impostor::Settings s;
	s.size = 64;
	const Impostor::Baked b = Impostor::bake(mesh, { flat }, s);
	REQUIRE(b.ok());
	int partial = 0, full = 0;
	for (int y = 0; y < b.height; ++y)
		for (int x = 0; x < 64; ++x)
		{
			const uint8_t a = texel(b, x, y)[3];
			if (a == 255) ++full;
			else if (a > 0) ++partial;
		}
	CHECK(partial > 40);          // the diagonal is soft, not a staircase
	CHECK(full > 800);            // roughly half of 64 x 64
	CHECK(texel(b, 32, 5)[0] == 255);   // a white tint over no texture is white
}

TEST_CASE("Impostor: the nearer surface wins and a cut-out one does not hide what is behind it")
{
	// Two quads in front view: the back one (z = -0.2) fully opaque red, the front one
	// (z = +0.2) half cut out (left half) green. Where the front one is cut out, red shows.
	StaticMeshAsset mesh;
	mesh.type = HE::AssetType::StaticMesh;
	auto addQuad = [&](float z) {
		const uint32_t base = static_cast<uint32_t>(mesh.vertices.size() / 3);
		const float p[4][3] = { { -0.5f, 0, z }, { 0.5f, 0, z }, { 0.5f, 1, z }, { -0.5f, 1, z } };
		const float uv[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
		for (int k = 0; k < 4; ++k)
		{
			mesh.vertices.insert(mesh.vertices.end(), { p[k][0], p[k][1], p[k][2] });
			mesh.normals.insert(mesh.normals.end(), { 0.0f, 0.0f, 1.0f });
			mesh.uvs.insert(mesh.uvs.end(), { uv[k][0], uv[k][1] });
		}
		mesh.indices.insert(mesh.indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
	};
	addQuad(-0.2f);
	addQuad(0.2f);
	MeshSection back, front;
	back.indexOffset = 0;  back.indexCount = 6;
	front.indexOffset = 6; front.indexCount = 6;
	mesh.sections = { back, front };

	const uint8_t redPx[4]   = { 255, 0, 0, 255 };
	std::vector<uint8_t> greenHalf(2 * 1 * 4);    // 2 x 1 texels: left opaque green, right cut out
	greenHalf = { 0, 255, 0, 255,   0, 0, 0, 0 };
	Impostor::SectionSource srcBack, srcFront;
	srcBack.rgba = redPx; srcBack.width = 1; srcBack.height = 1;
	srcFront.rgba = greenHalf.data(); srcFront.width = 2; srcFront.height = 1;
	Impostor::Settings s;
	s.size = 64;
	const Impostor::Baked b = Impostor::bake(mesh, { srcBack, srcFront }, s);
	REQUIRE(b.ok());
	const uint8_t* left  = texel(b, 16, 32);
	const uint8_t* right = texel(b, 48, 32);
	CHECK(left[1] == 255);  CHECK(left[0] == 0);       // the nearer, green one
	CHECK(right[0] == 255); CHECK(right[1] == 0);      // red shows through the cut-out
	CHECK(left[3] == 255);  CHECK(right[3] == 255);
}

TEST_CASE("Impostor: the card is two crossed double-faced quads, sized to the plant, wound along their normals")
{
	const StaticMeshAsset card = Impostor::makeCard(0.4f, 0.1f, 1.3f);
	CHECK(card.vertices.size() == 16u * 3);
	CHECK(card.indices.size() == 8u * 3);
	REQUIRE(card.sections.size() == 1u);
	CHECK(card.sections[0].indexCount == 24u);
	float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
	for (size_t i = 0; i < card.vertices.size(); i += 3)
		for (int k = 0; k < 3; ++k)
		{
			lo[k] = std::min(lo[k], card.vertices[i + k]);
			hi[k] = std::max(hi[k], card.vertices[i + k]);
		}
	CHECK(lo[0] == doctest::Approx(-0.4f)); CHECK(hi[0] == doctest::Approx(0.4f));
	CHECK(lo[1] == doctest::Approx(0.1f));  CHECK(hi[1] == doctest::Approx(1.3f));
	CHECK(lo[2] == doctest::Approx(-0.4f)); CHECK(hi[2] == doctest::Approx(0.4f));
	for (size_t t = 0; t < card.indices.size(); t += 3)
	{
		auto P = [&](size_t k) { return glm::vec3(card.vertices[card.indices[t + k] * 3], card.vertices[card.indices[t + k] * 3 + 1], card.vertices[card.indices[t + k] * 3 + 2]); };
		auto N = [&](size_t k) { return glm::vec3(card.normals[card.indices[t + k] * 3], card.normals[card.indices[t + k] * 3 + 1], card.normals[card.indices[t + k] * 3 + 2]); };
		const glm::vec3 geometric = glm::cross(P(1) - P(0), P(2) - P(0));
		CHECK(glm::dot(geometric, N(0)) > 0.0f);   // counter-clockwise seen along the normal
	}
	// Plane A (vertices 0..7) maps the left half of the atlas, plane B the right half.
	for (size_t v = 0; v < 8; ++v)  CHECK(card.uvs[v * 2] <= 0.5f);
	for (size_t v = 8; v < 16; ++v) CHECK(card.uvs[v * 2] >= 0.5f);
}

TEST_CASE("Impostor: write puts texture, cut-out material and card next to the mesh and keeps their ids when baked again")
{
	const fs::path root = fs::temp_directory_path() / "he_test_impostor_write";
	std::error_code ec;
	fs::remove_all(root, ec);
	fs::create_directories(root / "Plants", ec);

	const StaticMeshAsset mesh = quadMesh(0.6f, 0.9f);
	const std::vector<uint8_t> px = checker();
	Impostor::SectionSource src;
	src.rgba = px.data(); src.width = 4; src.height = 4;
	Impostor::Settings s;
	s.size = 32;
	const Impostor::Baked b = Impostor::bake(mesh, { src }, s);
	REQUIRE(b.ok());

	CHECK(Impostor::pathFor("Plants/Leaf.hasset", "") == "Plants/Leaf_Impostor.hasset");
	CHECK(Impostor::pathFor("Plants/Leaf.hasset", "Mat") == "Plants/Leaf_ImpostorMat.hasset");
	CHECK(Impostor::pathFor("Plants/Leaf.hasset", "Card") == "Plants/Leaf_ImpostorCard.hasset");

	const Impostor::Result first = Impostor::write(b, "Plants/Leaf.hasset", root);
	REQUIRE(first.ok());
	CHECK(fs::exists(root / first.texturePath));
	CHECK(fs::exists(root / first.materialPath));
	CHECK(fs::exists(root / first.meshPath));
	const Impostor::Result second = Impostor::write(b, "Plants/Leaf.hasset", root);
	REQUIRE(second.ok());
	CHECK(second.texture == first.texture);
	CHECK(second.mesh == first.mesh);
	CHECK(second.material == first.material);
	CHECK_FALSE(first.material == HE::UUID{});

	ContentManager cm(root.string());
	const HE::UUID matId = cm.loadAsset(first.materialPath);
	const MaterialAsset* mat = cm.getMaterial(matId);
	REQUIRE(mat != nullptr);
	CHECK(mat->blendMode == 1);                       // Masked: the shader discards the cut-out texels
	// doubleSided is not a stored MTRL field (no material keeps it across a save); the card
	// carries both faces of each plane instead, so it shows from behind with culling on.
	REQUIRE(!mat->texturePaths.empty());
	CHECK(mat->texturePaths[0] == first.texturePath);
	CHECK_FALSE(mat->customShaderFragGlsl.empty());

	const HE::UUID texId = cm.loadAsset(first.texturePath);
	const TextureAsset* tex = cm.getTexture(texId);
	REQUIRE(tex != nullptr);
	CHECK(tex->width == 64u);
	CHECK(tex->height == 32u);
	CHECK(tex->srgb);
	CHECK(tex->data == b.rgba);

	const HE::UUID cardId = cm.loadAsset(first.meshPath);
	const StaticMeshAsset* card = cm.getStaticMesh(cardId);
	REQUIRE(card != nullptr);
	CHECK(card->indices.size() == 24u);

	fs::remove_all(root, ec);
}

TEST_CASE("Impostor: the level goes on the far end of a layer, and baking again replaces it instead of adding another")
{
	FoliageLayer layer;
	layer.drawDistance = 100.0f;
	layer.meshAssetId = HE::UUID{ 1, 1 };
	FoliageLod first; first.meshAssetId = HE::UUID{ 9, 9 }; first.distance = 20.0f;
	layer.lods = { first };

	const HE::UUID card{ 5, 5 }, mat{ 6, 6 };
	REQUIRE(HE::AutoLodApply::applyImpostorLevel(layer, card, mat));
	REQUIRE(layer.lods.size() == 2u);
	CHECK(layer.lods[1].meshAssetId == card);
	CHECK(layer.lods[1].materialAssetId == mat);
	CHECK(layer.lods[1].distance > 20.0f);
	CHECK(layer.lods[1].distance < 100.0f);

	// Again, with a new material id: the same level is changed, no third one appears.
	const float d = layer.lods[1].distance;
	REQUIRE(HE::AutoLodApply::applyImpostorLevel(layer, card, HE::UUID{ 7, 7 }));
	CHECK(layer.lods.size() == 2u);
	CHECK(layer.lods[1].materialAssetId == HE::UUID{ 7, 7 });
	CHECK(layer.lods[1].distance == doctest::Approx(d));

	// A chain that ends on the ENGINE card gets the impostor in place of it.
	FoliageLayer withEngineCard;
	withEngineCard.drawDistance = 100.0f;
	FoliageLod engine; engine.meshAssetId = HE::kEngineCrossCardMeshId; engine.distance = 40.0f;
	withEngineCard.lods = { engine };
	REQUIRE(HE::AutoLodApply::applyImpostorLevel(withEngineCard, card, mat));
	CHECK(withEngineCard.lods.size() == 1u);
	CHECK(withEngineCard.lods[0].meshAssetId == card);
	CHECK(withEngineCard.lods[0].distance == doctest::Approx(40.0f));

	// fillFoliageLayer's own cross-card level takes the impostor when it is given one.
	FoliageLayer chain;
	chain.drawDistance = 200.0f;
	chain.materialAssetId = HE::UUID{ 3, 3 };
	HE::AutoLodApply::FoliageOptions fo;
	fo.crossCard = true;
	fo.impostorMesh = card;
	fo.impostorMaterial = mat;
	HE::AutoLodApply::fillFoliageLayer(chain, { { HE::UUID{ 11, 11 }, 0.05f }, { HE::UUID{ 12, 12 }, 0.15f } }, fo);
	REQUIRE(chain.lods.size() == 3u);
	CHECK(chain.lods.back().meshAssetId == card);
	CHECK(chain.lods.back().materialAssetId == mat);
	fo.impostorMesh = HE::UUID{};
	HE::AutoLodApply::fillFoliageLayer(chain, { { HE::UUID{ 11, 11 }, 0.05f }, { HE::UUID{ 12, 12 }, 0.15f } }, fo);
	CHECK(chain.lods.back().meshAssetId == HE::kEngineCrossCardMeshId);
	CHECK(chain.lods.back().materialAssetId == HE::UUID{ 3, 3 });
}
