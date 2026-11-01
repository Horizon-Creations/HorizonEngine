#include "doctest.h"
#include "TestFsUtil.h"

#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/HAsset.h>
#include <Hpak/HpakReader.h>
#include <Hpak/HpakWriter.h>
#include <MaterialGraph/MaterialGraph.h>

#include <filesystem>
#include <string>

// ── Landscape Foliage material node (Thema 327, step 3) ──────────────────────
// A metadata node: no pins, nothing emitted. These tests pin the three ways it can
// silently go missing: the demand-driven emit never visits it (so the generator
// scans), the MTRL tail field is lost behind a packer cut, and an instance forgets
// it. The shader text must stay byte-identical with or without the node.

namespace fs = std::filesystem;
using HE::MaterialGraph;
using HE::MatNodeType;

namespace
{
	const HE::UUID kSet { 0x4B0u, 1u };

	int addFoliageNode(MaterialGraph& g, const HE::UUID& set)
	{
		const int id = g.addNode(MatNodeType::LandscapeFoliage);
		g.findNode(id)->s = HE::matFoliageSetIdToString(set);
		return id;
	}

	// A small landscape material: a two-layer blend into BaseColor.
	MaterialGraph landscapeGraph()
	{
		MaterialGraph g = MaterialGraph::makeDefault();
		int out = 0;
		for (const auto& n : g.nodes) if (n.type == MatNodeType::Output) out = n.id;
		const int lb = g.addNode(MatNodeType::LandscapeLayerBlend);
		const int a  = g.addNode(MatNodeType::ConstColor);
		g.findNode(a)->p[0] = 0.2f; g.findNode(a)->p[1] = 0.6f; g.findNode(a)->p[2] = 0.1f;
		REQUIRE(g.connect(a, 0, lb, 0));
		REQUIRE(g.connect(lb, 0, out, HE::kMatOutputBaseColorPin));
		return g;
	}
}

TEST_CASE("landscape foliage node: it is a registered, pinless, paramless node")
{
	const HE::MatNodeDesc& d = HE::matNodeDesc(MatNodeType::LandscapeFoliage);
	CHECK(d.type == MatNodeType::LandscapeFoliage);
	CHECK(d.inputs.empty());
	CHECK(d.outputs.empty());
	CHECK(d.paramCount == 0);
	CHECK(std::string(d.category) == "Landscape");
	const HE::MatNodeDesc* byName = HE::matNodeDescByName(d.name);
	REQUIRE(byName != nullptr);
	CHECK(byName->type == MatNodeType::LandscapeFoliage);
}

TEST_CASE("landscape foliage node: the set id round-trips through its text form")
{
	const HE::UUID ids[] = { { 0x4B0u, 1u }, { 0xFFFFFFFFFFFFFFFFull, 0x8000000000000001ull },
	                         { 0x0123456789ABCDEFull, 0xFEDCBA9876543210ull } };
	for (const HE::UUID& id : ids)
	{
		const std::string s = HE::matFoliageSetIdToString(id);
		CHECK(s.size() == 32);
		CHECK(HE::matFoliageSetIdFromString(s) == id);
	}
	CHECK(HE::matFoliageSetIdToString(HE::UUID{}).empty());
	CHECK(HE::matFoliageSetIdFromString("") == HE::UUID{});
	CHECK(HE::matFoliageSetIdFromString("not a uuid") == HE::UUID{});
	CHECK(HE::matFoliageSetIdFromString(std::string(31, 'a')) == HE::UUID{});
	CHECK(HE::matFoliageSetIdFromString(std::string(31, 'a') + "g") == HE::UUID{});
	// Upper case parses to the same id.
	std::string upper = HE::matFoliageSetIdToString(ids[2]);
	for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	CHECK(HE::matFoliageSetIdFromString(upper) == ids[2]);
}

TEST_CASE("landscape foliage node: an UNWIRED node still reaches the generator, and the shader is unchanged")
{
	MaterialGraph plain = landscapeGraph();
	MaterialGraph withNode = landscapeGraph();
	addFoliageNode(withNode, kSet);

	const HE::MatShaderGen a = HE::generateFragment(plain);
	const HE::MatShaderGen b = HE::generateFragment(withNode);
	CHECK(a.foliageSetId == HE::UUID{});
	CHECK(b.foliageSetId == kSet);
	// Nothing is emitted for it: both stages, the deferred variant and the layer list.
	CHECK(a.glsl == b.glsl);
	CHECK(a.glslGBuffer == b.glslGBuffer);
	CHECK(a.layerNames == b.layerNames);
	CHECK(a.params.size() == b.params.size());
}

TEST_CASE("landscape foliage node: lowest node id wins, and an empty or malformed node is skipped")
{
	MaterialGraph g = landscapeGraph();
	const int blank = g.addNode(MatNodeType::LandscapeFoliage);          // s empty
	const int junk  = g.addNode(MatNodeType::LandscapeFoliage);
	g.findNode(junk)->s = "zzzz";
	const HE::UUID first { 0x4B0u, 7u }, second { 0x4B0u, 8u };
	addFoliageNode(g, first);
	addFoliageNode(g, second);
	(void)blank;
	CHECK(HE::generateFragment(g).foliageSetId == first);

	// Removing the winner hands it to the next one.
	for (const auto& n : g.nodes)
		if (n.type == MatNodeType::LandscapeFoliage && HE::matFoliageSetIdFromString(n.s) == first)
		{ g.removeNode(n.id); break; }
	CHECK(HE::generateFragment(g).foliageSetId == second);
}

TEST_CASE("landscape foliage node: the graph JSON keeps the node and its set")
{
	MaterialGraph g = landscapeGraph();
	addFoliageNode(g, kSet);
	MaterialGraph back;
	REQUIRE(HE::materialGraphFromJson(HE::materialGraphToJson(g), back));
	int seen = 0;
	for (const auto& n : back.nodes)
		if (n.type == MatNodeType::LandscapeFoliage)
		{ ++seen; CHECK(HE::matFoliageSetIdFromString(n.s) == kSet); }
	CHECK(seen == 1);
	CHECK(HE::generateFragment(back).foliageSetId == kSet);
	// It has no pins, so no link can be made to or from it.
	const int id = back.addNode(MatNodeType::LandscapeFoliage);
	const int c  = back.addNode(MatNodeType::ConstFloat);
	CHECK_FALSE(back.connect(c, 0, id, 0));
	CHECK_FALSE(back.connect(id, 0, c, 0));
}

TEST_CASE("landscape foliage node: MaterialAsset.foliageSetId is regenerated at load, saved, and old files read as none")
{
	const fs::path root = fs::temp_directory_path() / "he_test_lf_node" / "Content";
	he_test::removeAllQuiet(root.parent_path());
	fs::create_directories(root);
	{
		ContentManager cm(root.string());
		MaterialAsset w;
		w.type = HE::AssetType::Material;
		w.name = "WithNode"; w.path = "WithNode.hasset";
		MaterialGraph g = landscapeGraph();
		addFoliageNode(g, kSet);
		w.nodeGraphJson = HE::materialGraphToJson(g);
		REQUIRE(cm.saveAsset(w));
		// No graph (a packaged material): the saved field is what loads.
		MaterialAsset p;
		p.type = HE::AssetType::Material;
		p.name = "Packed"; p.path = "Packed.hasset";
		p.foliageSetId = kSet;
		REQUIRE(cm.saveAsset(p));
		// Neither: none.
		MaterialAsset n;
		n.type = HE::AssetType::Material;
		n.name = "Plain"; n.path = "Plain.hasset";
		REQUIRE(cm.saveAsset(n));
	}
	ContentManager fresh(root.string());
	const MaterialAsset* w = fresh.getMaterial(fresh.loadAsset("WithNode.hasset"));
	REQUIRE(w);
	CHECK(w->foliageSetId == kSet);
	CHECK(w->graphLayerNames.size() == 2); // the node did not disturb the paint layers (a fresh blend has two)
	const MaterialAsset* p = fresh.getMaterial(fresh.loadAsset("Packed.hasset"));
	REQUIRE(p);
	CHECK(p->foliageSetId == kSet);
	const MaterialAsset* n = fresh.getMaterial(fresh.loadAsset("Plain.hasset"));
	REQUIRE(n);
	CHECK(n->foliageSetId == HE::UUID{});
	he_test::removeAllQuiet(root.parent_path());
}

TEST_CASE("landscape foliage node: a material saved before the field existed loads with none")
{
	// Cut the new field off a freshly written MTRL: that is exactly an old file.
	const fs::path root = fs::temp_directory_path() / "he_test_lf_node_old" / "Content";
	he_test::removeAllQuiet(root.parent_path());
	fs::create_directories(root);
	{
		ContentManager cm(root.string());
		MaterialAsset p;
		p.type = HE::AssetType::Material;
		p.name = "Old"; p.path = "Old.hasset";
		p.foliageSetId = kSet;
		p.customShaderDispGlsl = "// disp";
		REQUIRE(cm.saveAsset(p));
	}
	HAsset::Reader r;
	REQUIRE(r.open((root / "Old.hasset").string()));
	std::vector<uint8_t> meta, mtrl;
	{
		const auto* cm = r.findChunk(HAsset::CHUNK_META);
		const auto* cx = r.findChunk(HAsset::CHUNK_MTRL);
		REQUIRE(cm); REQUIRE(cx);
		meta = cm->data;
		mtrl = cx->data;
	}
	REQUIRE(mtrl.size() > 16);
	mtrl.resize(mtrl.size() - 16); // hi + lo
	HAsset::Writer w;
	w.addChunk(HAsset::CHUNK_META, meta.data(), meta.size());
	w.addChunk(HAsset::CHUNK_MTRL, mtrl.data(), mtrl.size());
	const std::vector<uint8_t> bytes = w.toBytes(static_cast<uint16_t>(HE::AssetType::Material));
	{
		std::FILE* f = std::fopen((root / "Old.hasset").string().c_str(), "wb");
		REQUIRE(f);
		std::fwrite(bytes.data(), 1, bytes.size(), f);
		std::fclose(f);
	}
	ContentManager fresh(root.string());
	const MaterialAsset* m = fresh.getMaterial(fresh.loadAsset("Old.hasset"));
	REQUIRE(m);
	CHECK(m->foliageSetId == HE::UUID{});
	CHECK(m->customShaderDispGlsl == "// disp"); // the field before it is intact
	he_test::removeAllQuiet(root.parent_path());
}

TEST_CASE("landscape foliage node: an instance carries its parent's set")
{
	const fs::path root = fs::temp_directory_path() / "he_test_lf_node_inst" / "Content";
	he_test::removeAllQuiet(root.parent_path());
	fs::create_directories(root);
	{
		ContentManager cm(root.string());
		MaterialAsset parent;
		parent.type = HE::AssetType::Material;
		parent.name = "Parent"; parent.path = "Parent.hasset";
		MaterialGraph g = landscapeGraph();
		addFoliageNode(g, kSet);
		parent.nodeGraphJson = HE::materialGraphToJson(g);
		REQUIRE(cm.saveAsset(parent));
		MaterialAsset inst;
		inst.type = HE::AssetType::Material;
		inst.name = "Inst"; inst.path = "Inst.hasset";
		inst.parentMaterialPath = "Parent.hasset";
		REQUIRE(cm.saveAsset(inst));
	}
	ContentManager fresh(root.string());
	const HE::UUID parentId = fresh.loadAsset("Parent.hasset");
	REQUIRE(fresh.getMaterial(parentId));
	const HE::UUID instId = fresh.loadAsset("Inst.hasset");
	fresh.syncMaterialInstance(instId);
	const MaterialAsset* inst = fresh.getMaterial(instId);
	REQUIRE(inst);
	CHECK(inst->foliageSetId == kSet);
	CHECK(inst->graphLayerNames == fresh.getMaterial(parentId)->graphLayerNames);
	he_test::removeAllQuiet(root.parent_path());
}

TEST_CASE("landscape foliage node: the packer ships the set behind the approx block")
{
	const fs::path dir = fs::temp_directory_path() / "he_test_lf_node_pack";
	he_test::removeAllQuiet(dir);
	fs::create_directories(dir);
	const HE::UUID matId { 0x4B1u, 0xC0DEu };
	{
		ContentManager cm(dir.string());
		MaterialAsset p;
		p.id = matId;
		p.type = HE::AssetType::Material;
		p.name = "Pk"; p.path = "Pk.hasset";
		p.customShaderFragGlsl = "void main(){}";
		p.customShaderDispGlsl = "// disp";
		p.foliageSetId = kSet;
		REQUIRE(cm.saveAsset(p));
	}
	HpakWriter packer;
	Hpak::PackSettings s;
	s.codec = Hpak::Codec::Store;
	REQUIRE(packer.addDirectory(dir, s, nullptr) == 1);
	const std::string pak = (dir / "out.hpak").string();
	REQUIRE(packer.write(pak));

	HpakReader reader;
	REQUIRE(reader.open(pak));
	const std::vector<uint8_t> blob = reader.readEntry(matId);
	REQUIRE(!blob.empty());
	ContentManager game(dir.string());
	REQUIRE(game.loadAssetFromMemory(blob) == matId);
	const MaterialAsset* m = game.getMaterial(matId);
	REQUIRE(m);
	CHECK(m->customShaderDispGlsl == "// disp");
	CHECK(m->foliageSetId == kSet);
	he_test::removeAllQuiet(dir);
}
