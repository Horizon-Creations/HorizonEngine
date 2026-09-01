// The Third Person project template — the one preset that claims a new project
// is PLAYABLE, not merely laid out.
//
// So the tests here are deliberately not "the files exist". Files existing was
// never the problem: every other preset already produced a folder tree, and the
// engine still needed a scavenger hunt across five assets in four places before
// anything moved. What is asserted instead is the claim itself, taken apart into
// the four things that have to hold for it:
//
//   1. the scene has ground with COLLISION, or the character falls through it
//   2. the controller is discoverable AS a PlayerController, or PlayerHost never
//      instantiates it and nothing responds to input
//   3. the controller's graph really spawns and possesses a character
//   4. the character it spawns arrives with a body — controller, movement,
//      camera — which it inherits rather than carries
//
// Each of those has failed in this engine at some point, silently.
#include "doctest.h"
#include "TestFsUtil.h"
#include "ProjectManager.h"
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <Types/Enums.h>
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HcClassResolve.h>

#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/EntityHost.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/Components/NameComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/ColliderComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/Components/CharacterControllerComponent.h>
#include <HorizonScene/Components/MovementComponent.h>
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/HierarchyComponent.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

namespace fs = std::filesystem;
using json   = nlohmann::json;

namespace
{
	fs::path makeProject(const char* folder, const char* name,
	                     ProjectScriptLanguage lang = ProjectScriptLanguage::HorizonCode)
	{
		const auto root = fs::temp_directory_path() / folder;
		he_test::removeAllQuiet(root);
		ProjectManager pm;
		REQUIRE(pm.createNewProject(root.string(), name,
		                            ProjectPreset::ThirdPerson, lang));
		return root;
	}
}

TEST_CASE("third person: the six assets a player is made of are all on disk")
{
	const auto root = makeProject("he_tps_files", "Starter");
	const fs::path content = root / "Content";

	CHECK(fs::is_directory(content / "Gameplay"));
	CHECK(fs::is_directory(content / "Input"));
	CHECK(fs::exists(content / "Gameplay" / "PlayerController.hasset"));
	CHECK(fs::exists(content / "Gameplay" / "PlayerCharacter.hasset"));
	CHECK(fs::exists(content / "Input" / "Move.hasset"));
	CHECK(fs::exists(content / "Input" / "Look.hasset"));
	CHECK(fs::exists(content / "Input" / "Jump.hasset"));
	CHECK(fs::exists(content / "Input" / "DefaultMappings.hasset"));

	// The preset survives in the manifest, so a reopened project still knows
	// which template it came from.
	std::ifstream in(root / "Starter.heproj");
	const json manifest = json::parse(in, nullptr, false);
	REQUIRE_FALSE(manifest.is_discarded());
	CHECK(manifest.value("preset", -1) == static_cast<int>(ProjectPreset::ThirdPerson));
}

// MUTATION: in ProjectManager.cpp's `playable` block, drop the "rigidbody" and
// "collider" entries from Ground — the scene still loads, still looks right, and
// the character falls through the floor on the first press of Play.
TEST_CASE("third person: the ground is solid, through the real scene loader")
{
	const auto root = makeProject("he_tps_scene", "Starter");

	std::ifstream in(root / "Content" / "StartupScene.hescene");
	const json sceneJson = json::parse(in, nullptr, false);
	REQUIRE_FALSE(sceneJson.is_discarded());

	// Not by reading the JSON back — by loading it the way the game does. A key
	// the serializer does not recognise is dropped with one warn line, so a
	// misspelling here would pass a JSON-shape assertion and fail in the editor.
	HorizonWorld world;
	SceneSerializer ser;
	REQUIRE(ser.load(world, root / "Content" / "StartupScene.hescene",
	                 HE::SerializeFormat::JSON));

	auto& reg = world.registry();
	Entity ground = entt::null;
	for (auto [e, nc] : reg.view<NameComponent>().each())
		if (nc.name == "Ground") { ground = e; break; }
	REQUIRE((ground != entt::null));

	REQUIRE(reg.all_of<RigidBodyComponent>(ground));
	CHECK(reg.get<RigidBodyComponent>(ground).type == RigidBodyType::Static);
	CHECK(reg.all_of<MeshComponent>(ground));
	// And explicitly NO ColliderComponent. An authored Box uses its own half
	// extents and ignores the entity's scale, so one here would be a 1 m cube in
	// the middle of a 60 m floor. Without one the fallback builds the box from
	// the world scale, which is the visible geometry exactly.
	CHECK_FALSE(reg.all_of<ColliderComponent>(ground));

	// Something at a fixed place to move relative to, and it is solid too — a
	// block you walk through is worse than no block.
	Entity block = entt::null;
	for (auto [e, nc] : reg.view<NameComponent>().each())
		if (nc.name == "Block") { block = e; break; }
	REQUIRE((block != entt::null));
	CHECK(reg.all_of<RigidBodyComponent>(block));

	// The sky is seeded with the cycle ON: without it timeOfDay does nothing and
	// the frame is flat and shadowless.
	bool hasSky = false;
	for (auto [e, nc] : reg.view<NameComponent>().each())
		if (nc.name == "Sky") { hasSky = true; break; }
	CHECK(hasSky);

	// And no camera entity in the scene: the character brings its own, and a
	// second main camera would win or lose on iteration order.
	CHECK(reg.view<CameraComponent>().size() == 0);
}

// The assertion that the whole template stands on. PlayerHost finds a controller
// by RESOLVING each HorizonCode class asset and asking whether its engine base
// is a PlayerController — so a template whose base-class chunk is wrong produces
// a project that opens, looks complete, and never responds to a key.
//
// MUTATION: in scaffoldThirdPersonProject, write "Entity" instead of
// "PlayerController" as the base class — everything below still loads, and
// nothing is ever a player.
TEST_CASE("third person: the controller and character resolve as what they claim to be")
{
	const auto root = makeProject("he_tps_classes", "Starter");
	ContentManager cm((root / "Content").string());

	// COPIED out of the asset, never held across another load. ContentManager
	// hands out pointers into a dense vector, so the very next loadAsset of
	// something not yet registered moves every one of them — reading `controller`
	// after loading the character is a use-after-free, and this test found one by
	// crashing on it.
	std::string controllerBase, controllerPath;
	bool controllerHasBlob = true;
	{
		const HorizonCodeClassAsset* a =
			cm.getHorizonCodeClass(cm.loadAsset("Gameplay/PlayerController.hasset"));
		REQUIRE(a != nullptr);
		controllerBase    = a->baseClass;
		controllerPath    = a->path;
		controllerHasBlob = !a->componentBlob.empty();
	}
	std::string characterBase, characterPath;
	bool characterHasBlob = true;
	{
		const HorizonCodeClassAsset* a =
			cm.getHorizonCodeClass(cm.loadAsset("Gameplay/PlayerCharacter.hasset"));
		REQUIRE(a != nullptr);
		characterBase    = a->baseClass;
		characterPath    = a->path;
		characterHasBlob = !a->componentBlob.empty();
	}
	CHECK(controllerBase == "PlayerController");
	CHECK(characterBase  == "PlayerCharacter");

	// Resolved the way PlayerHost resolves them, not by string comparison: a
	// class deriving from one of these is one too, and that is the test the host
	// actually runs.
	CHECK(HorizonCode::engineClassIsA(
		HorizonCode::resolveClassAsset(cm, controllerPath).engineBase, "PlayerController"));
	CHECK(HorizonCode::engineClassIsA(
		HorizonCode::resolveClassAsset(cm, characterPath).engineBase, "PlayerCharacter"));

	// Neither ships a component list, on purpose: they inherit today's engine
	// defaults rather than freezing the set that existed when this was written.
	CHECK_FALSE(controllerHasBlob);
	CHECK_FALSE(characterHasBlob);
}

// The graphs are hand-written JSON in a C++ string literal, and every way of
// getting that wrong is silent: a misspelled node type drops the node AND its
// links, a missing "hasArg" shifts every pin by two, and a wrong pin index in a
// link connects nothing. So they are parsed and inspected rather than trusted.
//
// MUTATION: change "Create Object" to "Spawn Object" in kControllerGraph — the
// graph still loads, with one node and three links fewer, and no error anywhere.
TEST_CASE("third person: the controller graph really spawns and possesses")
{
	const auto root = makeProject("he_tps_graph", "Starter");
	ContentManager cm((root / "Content").string());

	const HorizonCodeClassAsset* a =
		cm.getHorizonCodeClass(cm.loadAsset("Gameplay/PlayerController.hasset"));
	REQUIRE(a != nullptr);

	HorizonCode::Graph g;
	REQUIRE(HorizonCode::fromJson(a->graphJson, g));

	// Every node survived the round trip. fromJson drops an unrecognised type
	// silently, so the count IS the assertion.
	CHECK(g.nodes.size() == 5);
	CHECK(g.links.size() == 5);

	const HorizonCode::Node* spawn = nullptr;
	const HorizonCode::Node* possess = nullptr;
	bool hasBeginPlay = false;
	for (const HorizonCode::Node& n : g.nodes)
	{
		if (n.type == HorizonCode::NodeType::Event && n.s == "BeginPlay") hasBeginPlay = true;
		if (n.type == HorizonCode::NodeType::CreateObject) spawn = &n;
		if (n.type == HorizonCode::NodeType::EngineCall && n.s == "player.possess") possess = &n;
	}
	CHECK(hasBeginPlay);
	REQUIRE(spawn != nullptr);
	REQUIRE(possess != nullptr);

	// It spawns the character this template ships, by the path the content
	// manager speaks — relative to Content, with no "Content/" prefix.
	CHECK(spawn->s == "Gameplay/PlayerCharacter.hasset");
	// An exec engine call, or the exec chain stops dead at it.
	CHECK(possess->hasArg);
	// Possess takes object references, and its leading parameter is NOT named
	// "entity", so it gets no self-default — the controller has to be wired in.
	REQUIRE(possess->params.size() == 2);
	CHECK(possess->params[0].name == "controller");
	bool controllerWired = false;
	for (const auto& l : g.links)
		if (l.dstNode == possess->id && l.dstPin == 2) controllerWired = true;
	CHECK(controllerWired);
}

// MUTATION: in kCharacterGraph, wire the Vec2 axis straight into Move's
// direction pin and delete the Break/Make pair — the graph loads without a
// complaint (fromJson does not re-check pin types) and the character never moves.
TEST_CASE("third person: the character graph drives Move and Jump from the input actions")
{
	const auto root = makeProject("he_tps_charactergraph", "Starter");
	ContentManager cm((root / "Content").string());

	const HorizonCodeClassAsset* a =
		cm.getHorizonCodeClass(cm.loadAsset("Gameplay/PlayerCharacter.hasset"));
	REQUIRE(a != nullptr);

	HorizonCode::Graph g;
	REQUIRE(HorizonCode::fromJson(a->graphJson, g));
	CHECK(g.nodes.size() == 6);
	CHECK(g.links.size() == 6);

	const HorizonCode::Node* move = nullptr;
	const HorizonCode::Node* jump = nullptr;
	const HorizonCode::Node* moveAction = nullptr;
	bool hasBreak = false, hasMake = false, hasJumpAction = false;
	for (const HorizonCode::Node& n : g.nodes)
	{
		if (n.type == HorizonCode::NodeType::EngineCall && n.s == "locomotion.move") move = &n;
		if (n.type == HorizonCode::NodeType::EngineCall && n.s == "locomotion.jump") jump = &n;
		if (n.type == HorizonCode::NodeType::InputAction && n.s == "Move") moveAction = &n;
		if (n.type == HorizonCode::NodeType::InputAction && n.s == "Jump") hasJumpAction = true;
		if (n.type == HorizonCode::NodeType::BreakVector2) hasBreak = true;
		if (n.type == HorizonCode::NodeType::MakeVector3)  hasMake  = true;
	}
	REQUIRE(move != nullptr);
	REQUIRE(jump != nullptr);
	REQUIRE(moveAction != nullptr);
	CHECK(hasJumpAction);

	// The action node has to declare itself a 2D axis, or it has no value output
	// at all and the pin the bridge reads does not exist.
	CHECK(moveAction->hasArg);
	CHECK(moveAction->propType == HorizonCode::PinType::Vec2);

	// The Vec2 → Vec3 bridge. The graph refuses to convert between them, so
	// without this pair the direction arrives as zero and nothing moves.
	CHECK(hasBreak);
	CHECK(hasMake);

	// The Entity pins stay UNWIRED: empty means "the character this graph
	// belongs to", which is what these verbs are for. A template that wired them
	// anyway would teach the noisier shape.
	for (const HorizonCode::Node* n : { move, jump })
		for (const auto& l : g.links)
			CHECK_FALSE((l.dstNode == n->id && l.dstPin == 2));
}

// End to end, in a real world: spawn the character the controller's graph names
// and check it arrives able to be moved, hit and looked through. This is the one
// that would have caught the whole class of failure the template exists to end —
// the character used to arrive as a bare transform.
//
// MUTATION: in EntityHost::spawn, read a->componentBlob instead of
// inheritedComponents — the template's character loses its entire body.
TEST_CASE("third person: the shipped character spawns with a body it can be played with")
{
	const auto root = makeProject("he_tps_spawn", "Starter");
	ContentManager cm((root / "Content").string());

	HorizonWorld world;
	HorizonCode::Runtime rt;
	EntityHost host;
	host.begin(rt, world, cm);

	const EntityHost::Spawned s = host.spawn("Gameplay/PlayerCharacter.hasset");
	REQUIRE(s.instance != 0);
	REQUIRE((s.entity != entt::null));

	auto& reg = world.registry();
	CHECK(reg.all_of<TransformComponent>(s.entity));
	CHECK(reg.all_of<CharacterControllerComponent>(s.entity));  // Jump asks this
	CHECK(reg.all_of<MovementComponent>(s.entity));             // Move writes here
	CHECK(reg.all_of<ColliderComponent>(s.entity));

	// The camera child, which is the difference between a playable template and
	// a black screen.
	REQUIRE(reg.all_of<HierarchyComponent>(s.entity));
	bool hasCamera = false;
	for (Entity child : reg.get<HierarchyComponent>(s.entity).children)
		if (reg.all_of<CameraComponent>(child) && reg.get<CameraComponent>(child).isMain)
			hasCamera = true;
	CHECK(hasCamera);
}

// The assertion the first version of this file did not make, and the one the
// user's first play session made for me: the character has to STOP falling.
//
// Everything around it was already checked — the scene loads, the ground carries
// a static body, the character spawns with a controller — and the player still
// went through the floor, because the ground's collider was a 1 m cube in the
// middle of a 60 m floor. Checking the components is not checking the physics.
//
// MUTATION: give Ground an explicit Box ColliderComponent again in
// ProjectManager.cpp's `playable` block — the character falls forever.
TEST_CASE("third person: the character lands on the template's ground and stays there")
{
	const auto root = makeProject("he_tps_stand", "Starter");
	ContentManager cm((root / "Content").string());

	HorizonWorld world;
	SceneSerializer ser;
	REQUIRE(ser.load(world, root / "Content" / "StartupScene.hescene",
	                 HE::SerializeFormat::JSON));

	// The character the controller's Begin Play would create, at the same height
	// that graph spawns it from.
	HorizonCode::Runtime rt;
	EntityHost host;
	host.begin(rt, world, cm);
	const float spawn[3] = { 0.0f, 1.2f, 0.0f };
	const EntityHost::Spawned s = host.spawn("Gameplay/PlayerCharacter.hasset",
	                                         entt::null, spawn, nullptr);
	REQUIRE((s.entity != entt::null));

	PhysicsWorld phys;
	phys.initialize(world);
	for (int i = 0; i < 180; ++i) phys.step(world, 1.0f / 60.0f);   // three seconds

	auto& reg = world.registry();
	const float y = reg.get<TransformComponent>(s.entity).position.y;
	// Standing on a floor whose top face is at y = 0. Falling through shows up as
	// a large negative number rather than a near miss, so the window is generous
	// on purpose — this is a "did it stop at all" test, not a precision one.
	CHECK_MESSAGE(y > -1.0f, ("character fell through the floor, y = " +
	                          std::to_string(y)).c_str());
	CHECK(y < 2.0f);
	// And physics agrees it is standing, which is what Jump and the animator ask.
	REQUIRE(reg.all_of<CharacterControllerComponent>(s.entity));
	CHECK(reg.get<CharacterControllerComponent>(s.entity).isGrounded);

	// The far corner of the floor is solid too — the whole 60 m, not a patch in
	// the middle. This is the half that the 1 m collider passed.
	const float corner[3] = { 25.0f, 1.2f, 25.0f };
	const EntityHost::Spawned far = host.spawn("Gameplay/PlayerCharacter.hasset",
	                                           entt::null, corner, nullptr);
	REQUIRE((far.entity != entt::null));
	phys.addEntityTree(world, static_cast<uint32_t>(far.entity));
	for (int i = 0; i < 180; ++i) phys.step(world, 1.0f / 60.0f);
	CHECK(reg.get<TransformComponent>(far.entity).position.y > -1.0f);
}

// The template IS two HorizonCode classes, and PlayerHost only scans HorizonCode
// class assets for a controller. Offered with another language it would produce
// a project that says "playable" and has no player — so the language follows the
// template rather than the picker.
//
// MUTATION: delete the language override in createNewProject — the manifest then
// says Lua, and the Content Browser hides the very entries the project is built
// out of.
TEST_CASE("third person: picking another language does not produce a project without a player")
{
	const auto root = makeProject("he_tps_lang", "Starter", ProjectScriptLanguage::Lua);

	std::ifstream in(root / "Starter.heproj");
	const json manifest = json::parse(in, nullptr, false);
	REQUIRE_FALSE(manifest.is_discarded());
	CHECK(manifest.value("scriptLanguage", std::string()) == "HorizonCode");
}

// The regression guard the Tutorial preset got when it was added, and for the
// same reason: a new template must not change what the old ones produce.
TEST_CASE("third person: the other presets are untouched by it")
{
	const auto root = fs::temp_directory_path() / "he_tps_others";
	he_test::removeAllQuiet(root);

	ProjectManager pm;
	REQUIRE(pm.createNewProject((root / "plain").string(), "Plain",
	                            ProjectPreset::Game, ProjectScriptLanguage::Lua));
	// No gameplay assets, and the language the user actually picked.
	CHECK_FALSE(fs::exists(root / "plain" / "Content" / "Gameplay" / "PlayerController.hasset"));
	CHECK_FALSE(fs::is_directory(root / "plain" / "Content" / "Input"));
	{
		std::ifstream in(root / "plain" / "Plain.heproj");
		const json manifest = json::parse(in, nullptr, false);
		REQUIRE_FALSE(manifest.is_discarded());
		CHECK(manifest.value("scriptLanguage", std::string()) == "Lua");
	}

	REQUIRE(pm.createNewProject((root / "bare").string(), "Bare",
	                            ProjectPreset::Empty, ProjectScriptLanguage::HorizonCode));
	CHECK_FALSE(fs::is_directory(root / "bare" / "Content" / "Gameplay"));
}
