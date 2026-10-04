// Extract on Destruct, the editor's decisions (HcExtract; design §3.8): which
// rows the table shows, which variables fit which member, what Auto-Map fills,
// what "New Struct from Variables…" and "Create OnDestroyed Event" write.
// ImGui-free, so asserted here instead of clicked through.
#include "doctest.h"
#include "HcExtract.h"
#include "HcRename.h"
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HorizonCodeRuntime.h>
#include <Types/TypeRegistry.h>
#include <string>
#include <vector>

using namespace HorizonCode;

namespace
{
	constexpr const char* kReport = "test/extract_editor/EnemyReport.hasset";

	Variable var(const std::string& name, PinType t, float def = 0.0f)
	{
		Variable v; v.name = name; v.type = t; v.f[0] = def;
		return v;
	}

	// EnemyReport { Kills: Int (was "Frags"), Tier: Int, Who: Ref, Bonus: Float = 2.5 }
	HE::StructDef registerReport()
	{
		HE::StructDef def;
		def.name = "EnemyReport"; def.assetPath = kReport;
		HE::StructField kills; kills.name = "Kills"; kills.type = PinType::Int;
		kills.defaultValue = Value::ofInt(0); kills.formerNames = { "Frags" };
		HE::StructField tier; tier.name = "Tier"; tier.type = PinType::Int;
		tier.defaultValue = Value::ofInt(0);
		HE::StructField who; who.name = "Who"; who.type = PinType::Ref;
		HE::StructField bonus; bonus.name = "Bonus"; bonus.type = PinType::Float;
		bonus.defaultValue = Value::ofFloat(2.5f);
		def.fields = { kills, tier, who, bonus };
		HE::TypeRegistry::instance().registerStruct(def);
		return def;
	}

	Graph enemy()
	{
		Graph g;
		g.variables = { var("kills", PinType::Int), var("LootTier", PinType::Int),
		                var("Bonus", PinType::String), var("Health", PinType::Float, 3.0f) };
		return g;
	}

	const HcExtract::Row* row(const std::vector<HcExtract::Row>& rs, const std::string& m)
	{
		for (const auto& r : rs)
			if (r.member == m) return &r;
		return nullptr;
	}
}

TEST_CASE("HcExtract: choosing a struct pre-fills by name, case-insensitive, only where it fits")
{
	const HE::StructDef def = registerReport();
	Graph g = enemy();
	REQUIRE(HcExtract::chooseStruct(g, def));
	CHECK(g.extract.structPath == kReport);
	// kills → Kills (case-insensitive), Who → Self (a Ref member named Who),
	// Bonus is a String here and does not fit the Float member: left at default.
	const auto rs = HcExtract::rows(g, def);
	REQUIRE(rs.size() == 4);
	CHECK(row(rs, "Kills")->var == "kills");
	CHECK(row(rs, "Tier")->var.empty());
	CHECK(row(rs, "Who")->var == kExtractSelf);
	CHECK(row(rs, "Bonus")->var.empty());
	CHECK(row(rs, "Bonus")->defaultText == "default 2.5");
	for (const auto& r : rs) CHECK(r.error.empty());

	// Correcting one row by hand; choosing the same struct again changes nothing.
	CHECK(HcExtract::assign(g, "Tier", "LootTier"));
	CHECK_FALSE(HcExtract::chooseStruct(g, def));
	CHECK(row(HcExtract::rows(g, def), "Tier")->var == "LootTier");

	// Auto-Map leaves mapped rows alone.
	CHECK(HcExtract::autoMapByName(g, def) == 0);
}

TEST_CASE("HcExtract: fits, broken rows and members removed from the struct")
{
	const HE::StructDef def = registerReport();
	Graph g = enemy();
	g.extract.structPath = kReport;
	g.extract.map = { { "Kills", "Health" },     // Float into Int: converts, fine
	                  { "Tier", "Bonus" },       // String into Int: red
	                  { "Bonus", "Gone" },       // deleted variable: red
	                  { "Who", "kills" },        // a variable into the Ref member: red
	                  { "Removed", "kills" } };  // member not in the struct any more
	const auto rs = HcExtract::rows(g, def);
	REQUIRE(rs.size() == 5);
	CHECK(row(rs, "Kills")->error.empty());
	CHECK(row(rs, "Tier")->error.find("does not fit") != std::string::npos);
	CHECK(row(rs, "Bonus")->error.find("no variable 'Gone'") != std::string::npos);
	CHECK_FALSE(row(rs, "Who")->error.empty());
	const HcExtract::Row* removed = row(rs, "Removed");
	REQUIRE(removed != nullptr);
	CHECK(removed->removed);
	CHECK(removed->error.find("removed from struct") != std::string::npos);
	// Clearing it removes the entry for good.
	CHECK(HcExtract::assign(g, "Removed", ""));
	CHECK(HcExtract::rows(g, def).size() == 4);

	// The table and the runtime say the same thing about the same entry.
	CHECK(HcExtract::fitOf(def.fields[1], g.variables[2]) == "String, needs Int");
	CHECK(HcExtract::selfFit(def.fields[2]).empty());
	CHECK_FALSE(HcExtract::selfFit(def.fields[0]).empty());
}

TEST_CASE("HcExtract: an entry saved under a former member name still lands in its row")
{
	const HE::StructDef def = registerReport();
	Graph g = enemy();
	g.extract.structPath = kReport;
	g.extract.map = { { "Frags", "kills" } };   // Kills was called Frags
	const auto rs = HcExtract::rows(g, def);
	REQUIRE(rs.size() == 4);
	CHECK(row(rs, "Kills")->var == "kills");
	CHECK(row(rs, "Frags") == nullptr);
}

TEST_CASE("HcExtract: inherited and private variables are offered; locals are not")
{
	const HE::StructDef def = registerReport();
	Graph g;
	Variable own = var("Tier", PinType::Int); own.access = 1;   // private: still the class's own
	Variable local = var("Kills", PinType::Int); local.scope = 7;
	g.variables = { own, local };
	g.inherited = { var("Kills", PinType::Int) };
	const auto vars = HcExtract::classVariables(g);
	REQUIRE(vars.size() == 2);
	CHECK(vars[0].decl.name == "Tier");
	CHECK_FALSE(vars[0].inherited);
	CHECK(vars[1].decl.name == "Kills");
	CHECK(vars[1].inherited);
	REQUIRE(HcExtract::chooseStruct(g, def));
	const auto rs = HcExtract::rows(g, def);
	CHECK(row(rs, "Kills")->var == "Kills");   // the inherited one, not the local
	CHECK(row(rs, "Tier")->var == "Tier");
}

TEST_CASE("HcExtract: New Struct from Variables mirrors the ticked variables and maps them 1:1")
{
	Graph g = enemy();
	Variable mood; mood.name = "Mood"; mood.type = PinType::Enum; mood.typeName = "x/Mood.hasset";
	mood.s = "Angry";
	g.variables.push_back(mood);
	const std::string path = "test/extract_editor/Report2.hasset";
	HE::StructDef def = HcExtract::structFromVariables(g, { "Health", "kills", "Mood", "nope", "kills" },
	                                                  "Report2", path);
	CHECK(def.name == "Report2");
	CHECK(def.assetPath == path);
	REQUIRE(def.fields.size() == 3);           // unknown and duplicate skipped
	CHECK(def.fields[0].name == "Health");
	CHECK(def.fields[0].type == PinType::Float);
	CHECK(def.fields[0].defaultValue.f == 3.0f);
	CHECK(def.fields[2].type == PinType::Enum);
	CHECK(def.fields[2].defaultValue.s == "Angry");
	HE::TypeRegistry::instance().registerStruct(def);
	REQUIRE(HcExtract::chooseStruct(g, def));
	REQUIRE(g.extract.map.size() == 3);
	for (const auto& r : HcExtract::rows(g, def))
	{
		CHECK(r.var == r.member);
		CHECK(r.error.empty());
	}
	// The runtime builds it from exactly that.
	Runtime rt;
	const InstanceId id = rt.add(g);
	const Value v = rt.buildExtract(id);
	REQUIRE(v.items.size() == 3);
	CHECK(v.items[0].f == 3.0f);

	CHECK(HcExtract::isValidStructName("EnemyReport"));
	CHECK(HcExtract::isValidStructName("_x1"));
	CHECK_FALSE(HcExtract::isValidStructName("1abc"));
	CHECK_FALSE(HcExtract::isValidStructName("Enemy Report"));
	CHECK_FALSE(HcExtract::isValidStructName(""));
}

TEST_CASE("HcExtract: the variable list's mark and a rename that follows")
{
	const HE::StructDef def = registerReport();
	Graph g = enemy();
	g.extract.structPath = kReport;
	g.extract.map = { { "Kills", "kills" }, { "Tier", "kills" } };
	CHECK(HcExtract::describe(g, "kills") == "Extracted to EnemyReport.Kills, EnemyReport.Tier");
	CHECK(HcExtract::describe(g, "Health").empty());
	CHECK(HcExtract::renameVariable(g, "kills", "Frags") == 2);
	CHECK(HcExtract::extractedTo(g, "Frags").size() == 2);

	// Through the rename dialog too: the class's own declaration renamed, its
	// table follows in the same apply.
	Graph h = enemy();
	h.extract.structPath = kReport;
	h.extract.map = { { "Kills", "kills" } };
	HcRename::Target t;
	t.member = HcRename::Member::Variable;
	t.oldName = "kills";
	t.newName = "KillCount";
	HcRename::Plan p;
	p.rename.push_back({ 0, "kills", "variable kills" });
	CHECK(HcRename::apply(h, p, t));
	REQUIRE(h.findVariable("KillCount") != nullptr);
	CHECK(h.extract.map[0].var == "KillCount");
	(void)def;
}

TEST_CASE("HcExtract: Create OnDestroyed Event declares it once, typed, with a handler")
{
	Graph listener;
	std::string what;
	CHECK(HcExtract::createOnDestroyedEvent(listener, kReport, &what) == HcExtract::Listener::Created);
	const EventDecl* d = listener.findEvent(kOnDestroyed);
	REQUIRE(d != nullptr);
	CHECK(d->hasArg);
	CHECK(d->argType == PinType::Struct);
	CHECK(d->typeName == kReport);
	int handlers = 0;
	for (const Node& n : listener.nodes)
		if (n.type == NodeType::Event && n.s == kOnDestroyed)
		{
			++handlers;
			CHECK(n.hasArg);
			CHECK(n.typeName == kReport);
		}
	CHECK(handlers == 1);

	// Again: nothing new. Another struct: refused, not retyped.
	CHECK(HcExtract::createOnDestroyedEvent(listener, kReport) == HcExtract::Listener::AlreadyThere);
	CHECK(HcExtract::createOnDestroyedEvent(listener, "other/S.hasset", &what) ==
	      HcExtract::Listener::WrongType);
	CHECK(what.find("already has OnDestroyed (EnemyReport)") != std::string::npos);
	std::string arg;
	REQUIRE(HcExtract::onDestroyedArg(listener, arg));
	CHECK(arg == kReport);

	// The node deleted, the declaration kept: the handler comes back.
	listener.nodes.clear();
	CHECK(HcExtract::createOnDestroyedEvent(listener, kReport) == HcExtract::Listener::HandlerAdded);

	// "No data" for senders without Extract.
	Graph plain;
	CHECK(HcExtract::createOnDestroyedEvent(plain, "") == HcExtract::Listener::Created);
	REQUIRE(plain.findEvent(kOnDestroyed) != nullptr);
	CHECK_FALSE(plain.findEvent(kOnDestroyed)->hasArg);

	// The listener created this way actually receives the runtime's event.
	registerReport();
	Graph sender;
	sender.variables = { var("kills", PinType::Int, 5.0f) };
	sender.extract.structPath = kReport;
	sender.extract.map = { { "Kills", "kills" } };
	Graph l2;
	HcExtract::createOnDestroyedEvent(l2, kReport);
	Graph back;
	REQUIRE(fromJson(toJson(l2), back));   // what a saved listener asset holds
	Runtime rt;
	const InstanceId s = rt.add(sender);
	const InstanceId l = rt.add(back);
	rt.bindEvent(s, kOnDestroyed, l);
	rt.destroy(s);   // no crash, handler found by name and type-checked
	CHECK_FALSE(rt.alive(s));
}

// ── The drawing, one headless frame ─────────────────────────────────────────
// HcExtractUi only shows HcExtract's answers, but it is ImGui code in a 220 px
// sidebar: draw every part once (struct chosen, a broken row, a removed member,
// the Bind Event helper) and check nothing runs past the panel's edge.
#include "HcExtractUi.h"
#include <imgui.h>
#include <imgui_internal.h>   // the window's running extent (DC.CursorMaxPos)
#include <algorithm>

TEST_CASE("HcExtractUi: the section, the table and the Bind Event helper fit a 220 px sidebar")
{
	const HE::StructDef def = registerReport();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.DisplaySize = ImVec2(1280.0f, 720.0f);
	io.DeltaTime = 1.0f / 60.0f;
	io.IniFilename = nullptr;
	io.LogFilename = nullptr;
	io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

	Graph g = enemy();
	REQUIRE(HcExtract::chooseStruct(g, def));
	g.extract.map.push_back({ "Tier", "Bonus" });     // red row (String into Int)
	g.extract.map.push_back({ "Removed", "kills" });  // removed from struct
	Node bind; bind.type = NodeType::BindEvent; bind.id = 99;

	float maxRight = 0.0f, contentRight = 0.0f;
	for (int frame = 0; frame < 2; ++frame)
	{
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0, 0));
		ImGui::SetNextWindowSize(ImVec2(220.0f, 700.0f));
		ImGui::Begin("side", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);
		contentRight = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
		ImGui::PushTextWrapPos(0.0f);
		// The furthest right ANY item of a part reached (the window's running
		// extent), per part, so a failure names the part. Not GetItemRectMax:
		// a Selectable's hover rect reaches into the window padding by design.
		auto extent = [] { return ImGui::GetCurrentWindow()->DC.CursorMaxPos.x; };
		CHECK_FALSE(HcExtractUi::drawSidebarEntry(g, true, nullptr));
		HcExtractUi::drawSidebarEntry(g, false, "not here");
		const float afterSidebar = extent();
		CHECK_FALSE(HcExtractUi::drawDetails(g, "TEST_VAR"));   // nothing clicked, nothing changed
		const float afterDetails = extent();
		CHECK_FALSE(HcExtractUi::drawVariableLine(g, g.variables[0]));
		CHECK_FALSE(HcExtractUi::drawBindEventHelper(g, bind, std::string()));
		const float afterBind = extent();
		INFO("sidebar=", afterSidebar, " details=", afterDetails, " bind=", afterBind);
		maxRight = std::max(maxRight, afterBind);
		ImGui::PopTextWrapPos();
		ImGui::End();
		ImGui::EndFrame();
	}
	INFO("maxRight=", maxRight, " contentRight=", contentRight);
	CHECK(maxRight <= contentRight + 1.0f);
	CHECK(HcExtractUi::listNote(g, g.variables[0], "").find("(extracted)") != std::string::npos);
	CHECK(HcExtractUi::listNote(g, g.variables[0], "Int  (pulled)") == "Int  (pulled, extracted)");
	CHECK(HcExtractUi::listNote(g, g.variables[3], "").empty());   // Health: not in the table
	ImGui::DestroyContext();
}
