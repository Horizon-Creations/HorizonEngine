// Test fixture: a minimal native game-logic library for GameLogicLoader tests.
// Built as a SHARED lib by the test CMakeLists; he_tests loads it at runtime and
// verifies the full IGameLogic lifecycle (onStart / onUpdate / onStop) against a
// real HorizonWorld.
#include <IGameLogic.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/NameComponent.h>
#include <HorizonScene/Components/TransformComponent.h>

namespace {

class TestGameLogic final : public IGameLogic {
public:
	void onStart(HorizonWorld& world) override
	{
		auto e = world.createEntity("FromNativeLogic");
		world.addComponent(e, TransformComponent{});
	}

	void onUpdate(HorizonWorld& world, float /*deltaTime*/) override
	{
		for (auto [e, name, t] :
		     world.registry().view<NameComponent, TransformComponent>().each())
		{
			if (name.name == "FromNativeLogic")
				t.position.x += 1.0f;
		}
	}

	void onStop(HorizonWorld& world) override
	{
		world.createEntity("NativeLogicStopped");
	}
};

} // namespace

extern "C" HE_GAME_API IGameLogic* HE_CreateGameLogic()              { return new TestGameLogic(); }
extern "C" HE_GAME_API void        HE_DestroyGameLogic(IGameLogic* p) { delete p; }
