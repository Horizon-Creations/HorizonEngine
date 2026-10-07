// The OnChanged_<Var> bridge to the text languages and the native module
// (docs/bind-to-variable-binding-plan.md §4.5, Thema 137 step 5): a Lua script
// subscribes with horizon.hc.watch, the runtime reports the change at the
// frame-end compare (Runtime::exchangeState), and HcWatchEvents — the same
// dispatcher both applications point Runtime::onVariableChanged at — hands it
// to onChanged_<Var>(self, source, old, new). Driven through the real Lua door
// (the registry row, as a script reaches it) and the real dispatcher, so what
// is tested here is what the hosts run. Python and the loaded C++ module have
// their own cases in test_python_scripting / test_gamelogic_services.
#include "doctest.h"
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HorizonCodeRuntime.h>
#include <HorizonScene/EngineApi.h>
#include <HorizonScene/EntityHost.h>
#include <HorizonScene/HcWatchEvents.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/ScriptContext.h>
#include <HorizonScene/Components/ScriptComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <IGameLogic.h>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using HorizonCode::PinType;
using HorizonCode::Value;
using HorizonCode::Variable;
namespace hc = HE::api::hc;

namespace
{
	// Score is public, Hidden private — the door Get (Ref) uses decides.
	HorizonCode::Graph scoreGraph(int score = 5)
	{
		HorizonCode::Graph g;
		Variable s; s.name = "Score"; s.type = PinType::Int; s.f[0] = (float)score;
		Variable h; h.name = "Hidden"; h.type = PinType::Int; h.access = 1;
		g.variables = { s, h };
		return g;
	}

	// The subscriber: watches _G._target's Score in onStart and records every
	// report into globals.
	const char* kWatcher = R"lua(
local M = {}
function M.onStart(self)
  _G._ok = horizon.hc.watch(self.entityId, _G._target, "Score") and 1 or 0
end
function M.onChanged_Score(self, source, old, new)
  _G._calls  = _G._calls + 1
  _G._source = source
  _G._old    = old
  _G._new    = new
end
return M
)lua";

	struct NativeRecorder final : IGameLogic
	{
		void onStart(HorizonWorld&) override {}
		void onUpdate(HorizonWorld&, float) override {}
		void onStop(HorizonWorld&) override {}
		void onHcVariableChanged(uint32_t entity, const char* name) override
		{ calls.push_back({ entity, name ? name : "" }); }
		struct Call { uint32_t entity; std::string name; };
		std::vector<Call> calls;
	};

	// A host in miniature: the runtime, the script context publishing it, the
	// entity → instance map, and the hook pointed at HcWatchEvents exactly as
	// GameApplication::OnInit points it.
	struct Rig
	{
		HorizonWorld          world;
		HorizonCode::Runtime  rt;
		std::unique_ptr<ScriptContext> ctx;   // destroyed before rt, as in the hosts
		ScriptContext::InstanceMap     instances;
		IGameLogic*                    logic = nullptr;
		EntityHost*                    entities = nullptr;

		Rig()
		{
			ctx = std::make_unique<ScriptContext>(world);
			publish();
			rt.onVariableChanged = [this](HorizonCode::InstanceId owner, const std::string& var,
			                              const Value& old, const Value& now,
			                              const std::vector<uint64_t>& tokens)
			{
				HcWatchEvents::dispatch(rt, owner, var, old, now, tokens,
				                        ctx.get(), &instances, logic);
			};
		}
		~Rig() { ctx.reset(); }

		void publish()
		{
			ScriptContext::HostServices hs;
			hs.runtime  = &rt;
			hs.entities = entities;
			ctx->setHostServices(hs);
		}

		ScriptEngine& lua() { return ctx->engine(); }

		// A Lua watcher on a fresh entity, started; returns its entity.
		uint32_t startWatcher(int target)
		{
			REQUIRE(lua().exec("_G._calls = 0 _G._source = -1 _G._old = -1 _G._new = -1 "
			                   "_G._ok = -1 _G._target = " + std::to_string(target)));
			if (!ctx->isScriptLoaded("watcher")) REQUIRE(ctx->loadScript("watcher", kWatcher));
			const entt::entity e = world.createEntity("Ear");
			const auto id = ctx->createInstance("watcher", e);
			REQUIRE(id != ScriptEngine::kInvalidInstance);
			instances[(uint32_t)e] = id;
			REQUIRE(ctx->callOnStart(id));
			return (uint32_t)e;
		}
		int calls() { return (int)lua().getGlobalNumber("_calls"); }

		HE::api::Ctx apiCtx()
		{
			HE::api::Ctx c{};
			c.world    = &world;
			c.runtime  = &rt;
			c.entities = entities;
			return c;
		}
	};
}

TEST_CASE("hc.watch: a Lua script hears onChanged_<Var>(self, source, old, new) of the Game Instance")
{
	Rig rig;
	const auto gi = rig.rt.setGameInstance(scoreGraph(5));
	rig.startWatcher(0);
	CHECK(rig.lua().getGlobalNumber("_ok") == doctest::Approx(1.0));

	// The first compare only takes the baseline: subscribing reports nothing.
	rig.rt.exchangeState();
	CHECK(rig.calls() == 0);

	rig.rt.setVariable(gi, "Score", Value::ofInt(7));
	CHECK(rig.calls() == 0);                         // not before the frame's end
	CHECK(rig.rt.exchangeState() > 0);
	CHECK(rig.calls() == 1);
	CHECK(rig.lua().getGlobalNumber("_source") == doctest::Approx(0.0));   // the Game Instance
	CHECK(rig.lua().getGlobalNumber("_old") == doctest::Approx(5.0));
	CHECK(rig.lua().getGlobalNumber("_new") == doctest::Approx(7.0));

	// Twice in one frame: one report, old = the value at the last compare.
	rig.rt.setVariable(gi, "Score", Value::ofInt(8));
	rig.rt.setVariable(gi, "Score", Value::ofInt(9));
	rig.rt.exchangeState();
	CHECK(rig.calls() == 2);
	CHECK(rig.lua().getGlobalNumber("_old") == doctest::Approx(7.0));
	CHECK(rig.lua().getGlobalNumber("_new") == doctest::Approx(9.0));

	// Written and put back: nothing.
	rig.rt.setVariable(gi, "Score", Value::ofInt(1));
	rig.rt.setVariable(gi, "Score", Value::ofInt(9));
	rig.rt.exchangeState();
	CHECK(rig.calls() == 2);

	// The reader the script has besides `new`.
	REQUIRE(rig.lua().exec("_G._json = horizon.hc.getJson(0, 'Score')"));
	CHECK(rig.lua().getGlobalString("_json") == "9");

	// unwatch from the script ends it.
	REQUIRE(rig.lua().exec("horizon.hc.unwatch(" + std::to_string(rig.instances.begin()->first) +
	                       ", 0, 'Score')"));
	rig.rt.setVariable(gi, "Score", Value::ofInt(10));
	rig.rt.exchangeState();
	CHECK(rig.calls() == 2);
}

TEST_CASE("hc.watch: what may not be watched says so, and the HorizonCode side stays untouched")
{
	Rig rig;
	const auto gi = rig.rt.setGameInstance(scoreGraph(5));
	HE::api::Ctx c = rig.apiCtx();
	const entt::entity ear = rig.world.createEntity("Ear");
	const int e = (int)(uint32_t)ear;

	CHECK(hc::watch(c, e, 0, "Score"));
	CHECK(hc::watch(c, e, 0, "Score"));               // twice is once
	CHECK_FALSE(hc::watch(c, e, 0, "Hidden"));        // private: no door from outside
	CHECK_FALSE(hc::watch(c, e, 0, "Nope"));
	CHECK_FALSE(hc::watch(c, 0, 0, "Score"));         // no subscriber
	CHECK_FALSE(hc::watch(c, e, 12345, "Score"));     // no entity host: no class to name
	CHECK(hc::getJson(c, 0, "Score") == "5");
	CHECK(hc::getJson(c, 0, "Hidden").empty());       // private reads as nothing too
	CHECK(hc::getJson(c, 0, "Nope").empty());

	HE::api::Ctx none{};
	CHECK_FALSE(hc::watch(none, e, 0, "Score"));      // no runtime: neutral, no crash
	CHECK(hc::getJson(none, 0, "Score").empty());
	hc::unwatch(none, e, 0, "Score");

	// The registry reaches the same functions, and Lua and Python list the group.
	CHECK(HE::api::isScriptGroup("hc"));
	REQUIRE(HE::api::find("hc.watch"));
	REQUIRE(HE::api::find("hc.unwatch"));
	REQUIRE(HE::api::find("hc.getJson"));

	// A subscription is no Notify on Change: no OnChanged_ on the owner. The
	// owner has none here, so watch it through a declared listener instead —
	// "<Var>Changed" must NOT be sent for a pure subscription.
	HorizonCode::Graph listener;
	{
		Variable got; got.name = "Got"; got.type = PinType::Int;
		listener.variables = { got };
		HorizonCode::Node ev; ev.type = HorizonCode::NodeType::Event; ev.s = "ScoreChanged";
		ev.hasArg = true; ev.propType = PinType::Int;
		const int evId = listener.addNode(ev);
		HorizonCode::Node sv; sv.type = HorizonCode::NodeType::SetVariable; sv.s = "Got";
		sv.propType = PinType::Int;
		const int set = listener.addNode(sv);
		REQUIRE(listener.connect(evId, 0, set, 0));
		REQUIRE(listener.connect(evId, 1, set, 2));
	}
	const auto l = rig.rt.add(listener);
	rig.rt.bindEvent(gi, "ScoreChanged", l);
	rig.rt.exchangeState();
	rig.rt.setVariable(gi, "Score", Value::ofInt(6));
	rig.rt.exchangeState();
	CHECK(rig.rt.getVariable(l, "Got").i == 0);
}

TEST_CASE("hc.watch: a script that is gone hears nothing, and its subscription goes with it")
{
	Rig rig;
	const auto gi = rig.rt.setGameInstance(scoreGraph(5));
	const uint32_t e = rig.startWatcher(0);
	const auto id = rig.instances.at(e);
	rig.rt.exchangeState();

	// The host drops the instance (entity destroyed, zone unloaded).
	rig.instances.erase(e);
	rig.rt.setVariable(gi, "Score", Value::ofInt(6));
	rig.rt.exchangeState();
	CHECK(rig.calls() == 0);

	// …and the miss unsubscribed it: the same instance back in the map hears
	// nothing either. Without the drop it would — this is the control.
	rig.instances[e] = id;
	rig.rt.setVariable(gi, "Score", Value::ofInt(7));
	rig.rt.exchangeState();
	CHECK(rig.calls() == 0);
}

TEST_CASE("hc.watch: a script context that lets go of its runtime takes its scripts' subscriptions")
{
	Rig rig;
	const auto gi = rig.rt.setGameInstance(scoreGraph(5));
	NativeRecorder native;
	rig.logic = &native;
	CHECK(rig.rt.watch(gi, "Score", hc::nativeToken()));   // the module's survives the sweep

	SUBCASE("withdrawn services (play stop, shutdown)")
	{
		rig.startWatcher(0);
		rig.rt.exchangeState();
		rig.ctx->setHostServices({});
	}
	SUBCASE("the context destroyed (scene switch)")
	{
		rig.startWatcher(0);
		rig.rt.exchangeState();
		rig.ctx.reset();
		rig.ctx = std::make_unique<ScriptContext>(rig.world);
		rig.publish();
		REQUIRE(rig.lua().exec("_G._calls = 0"));
	}

	// The next scene's script on an entity of the same number must not inherit
	// it: the map says yes, the runtime has to say no.
	std::vector<uint64_t> seen;
	rig.rt.onVariableChanged = [&](HorizonCode::InstanceId, const std::string&, const Value&,
	                               const Value&, const std::vector<uint64_t>& tokens)
	{ seen = tokens; };
	rig.rt.setVariable(gi, "Score", Value::ofInt(6));
	rig.rt.exchangeState();
	CHECK(seen == std::vector<uint64_t>{ hc::nativeToken() });
}

TEST_CASE("hc.watch: an entity's class is a target, and its entity is the source")
{
	const auto dir = std::filesystem::temp_directory_path() / "he_hc_watch_classes";
	std::error_code ec;
	std::filesystem::remove_all(dir, ec);
	std::filesystem::create_directories(dir);
	{
		ContentManager classes(dir.string());
		HorizonCodeClassAsset a;
		a.type      = HE::AssetType::HorizonCodeClass;
		a.name      = "Scorer";
		a.path      = "Scorer.hasset";
		a.baseClass = "Entity";
		a.graphJson = HorizonCode::toJson(scoreGraph(3));
		REQUIRE(classes.saveAsset(a));

		Rig rig;
		auto& reg = rig.world.registry();
		const entt::entity owner = rig.world.createEntity("Scorer1");
		reg.emplace<TransformComponent>(owner);
		ScriptComponent sc;
		sc.scriptAssetId = classes.loadAsset(a.path);
		reg.emplace<ScriptComponent>(owner, sc);

		EntityHost host;
		host.begin(rig.rt, rig.world, classes);
		const auto inst = host.instanceOf(owner);
		REQUIRE(inst != 0);
		rig.entities = &host;
		rig.publish();
		HE::api::Ctx c = rig.apiCtx();
		CHECK(hc::targetInstance(c, (int)(uint32_t)owner) == inst);
		CHECK(hc::sourceEntity(rig.rt, inst) == (uint32_t)owner);

		rig.startWatcher((int)(uint32_t)owner);
		CHECK(rig.lua().getGlobalNumber("_ok") == doctest::Approx(1.0));
		rig.rt.exchangeState();
		rig.rt.setVariable(inst, "Score", Value::ofInt(4));
		rig.rt.exchangeState();
		CHECK(rig.calls() == 1);
		CHECK(rig.lua().getGlobalNumber("_source") == doctest::Approx((double)(uint32_t)owner));
		CHECK(rig.lua().getGlobalNumber("_old") == doctest::Approx(3.0));
		CHECK(rig.lua().getGlobalNumber("_new") == doctest::Approx(4.0));

		rig.ctx.reset();
		host.end();
	}
	std::filesystem::remove_all(dir, ec);
}

TEST_CASE("hc.watch: the native module hears onHcVariableChanged(source, var), and only while loaded")
{
	Rig rig;
	const auto gi = rig.rt.setGameInstance(scoreGraph(5));
	NativeRecorder native;
	rig.logic = &native;
	HE::api::Ctx c = rig.apiCtx();
	CHECK(hc::watchNative(c, 0, "Score"));
	CHECK_FALSE(hc::watchNative(c, 0, "Hidden"));
	rig.rt.exchangeState();

	rig.rt.setVariable(gi, "Score", Value::ofInt(6));
	rig.rt.exchangeState();
	REQUIRE(native.calls.size() == 1);
	CHECK(native.calls[0].entity == 0u);
	CHECK(native.calls[0].name == "Score");

	// No module loaded: the delivery is dropped and so is the subscription —
	// a module loaded later does not hear what this one asked for.
	rig.logic = nullptr;
	rig.rt.setVariable(gi, "Score", Value::ofInt(7));
	rig.rt.exchangeState();
	rig.logic = &native;
	rig.rt.setVariable(gi, "Score", Value::ofInt(8));
	rig.rt.exchangeState();
	CHECK(native.calls.size() == 1);

	// dropNative is the explicit form the hosts call at unload.
	CHECK(hc::watchNative(c, 0, "Score"));
	HcWatchEvents::dropNative(rig.rt);
	rig.rt.setVariable(gi, "Score", Value::ofInt(9));
	rig.rt.exchangeState();
	CHECK(native.calls.size() == 1);
}
