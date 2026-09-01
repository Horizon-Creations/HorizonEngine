#include "EditorGuides.h"

#include "HcNodeReference.h"

#include <utility>

namespace HE::Ed::Guides
{

// ── Building blocks ──────────────────────────────────────────────────────────

Docs::Run run(std::string text, Docs::Style style, std::string href)
{
	Docs::Run r;
	r.text  = std::move(text);
	r.style = style;
	r.href  = std::move(href);
	return r;
}

Docs::Run link(std::string text, std::string topic)
{
	return run(std::move(text), Docs::Style::Link, std::move(topic));
}

Docs::Block rich(std::vector<Docs::Run> runs)
{
	Docs::Block b;
	b.kind = Docs::BlockKind::Paragraph;
	b.runs = std::move(runs);
	return b;
}

Docs::Block para(std::string text)
{
	return rich({ run(std::move(text)) });
}

Docs::Block lead(std::string text)
{
	Docs::Block b;
	b.kind = Docs::BlockKind::Lead;
	b.runs.push_back(run(std::move(text)));
	return b;
}

Docs::Block heading(std::string text)
{
	Docs::Block b;
	b.kind = Docs::BlockKind::Heading;
	b.runs.push_back(run(std::move(text)));
	return b;
}

namespace
{
	Docs::Cells toCells(std::vector<std::string> items)
	{
		Docs::Cells cells;
		cells.reserve(items.size());
		for (std::string& s : items) cells.push_back(Docs::Cell{ run(std::move(s)) });
		return cells;
	}

	Docs::Block list(Docs::BlockKind kind, std::vector<std::string> items)
	{
		Docs::Block b;
		b.kind  = kind;
		b.items = toCells(std::move(items));
		return b;
	}
} // namespace

Docs::Block bullets(std::vector<std::string> items)
{
	return list(Docs::BlockKind::Bullets, std::move(items));
}

Docs::Block numbers(std::vector<std::string> items)
{
	return list(Docs::BlockKind::Numbers, std::move(items));
}

Docs::Block steps(std::vector<Docs::Block::Step> s)
{
	Docs::Block b;
	b.kind  = Docs::BlockKind::Flow;
	b.steps = std::move(s);
	return b;
}

Docs::Block code(std::string title, std::string body)
{
	Docs::Block b;
	b.kind  = Docs::BlockKind::Code;
	b.title = std::move(title);
	b.text  = std::move(body);
	return b;
}

Docs::Block table(std::vector<std::string> head,
                  std::vector<std::vector<std::string>> rows)
{
	Docs::Block b;
	b.kind = Docs::BlockKind::Table;
	b.head = toCells(std::move(head));
	b.rows.reserve(rows.size());
	for (std::vector<std::string>& r : rows) b.rows.push_back(toCells(std::move(r)));
	return b;
}

Docs::Block callout(Docs::Tone tone, std::string title, std::vector<Docs::Block> body)
{
	Docs::Block b;
	b.kind = Docs::BlockKind::Callout;
	b.tone = tone;
	// The reader draws a callout as a bordered box around its child blocks and
	// never looks at Block::title, so a heading has to BE one of the children.
	// Bold rather than Heading: inside a small box a heading font reads as the
	// start of a new section rather than as the label of this one.
	if (!title.empty())
		b.blocks.push_back(rich({ run(std::move(title), Docs::Style::Bold) }));
	for (Docs::Block& inner : body) b.blocks.push_back(std::move(inner));
	return b;
}

Docs::Block warn(std::string title, std::vector<Docs::Block> body)
{
	return callout(Docs::Tone::Warning, std::move(title), std::move(body));
}

Docs::Block tip(std::string title, std::vector<Docs::Block> body)
{
	return callout(Docs::Tone::Tip, std::move(title), std::move(body));
}

Docs::Block note(std::string title, std::vector<Docs::Block> body)
{
	return callout(Docs::Tone::Note, std::move(title), std::move(body));
}

// ── The search text ──────────────────────────────────────────────────────────
// Section::text is the only thing the index is built from, so a word that is
// not in here cannot be found — and a guide whose Warning callouts are missing
// from the index is a guide whose most useful half is invisible to search.
//
// Hence the recursion: a callout's body is blocks, and a Flow step carries two
// strings, neither of which is a Run. Getting either wrong fails silently, in
// exactly the way these pages exist to warn about.
namespace
{
	void add(std::string& out, const std::string& s)
	{
		if (s.empty()) return;
		if (!out.empty()) out += ' ';
		out += s;
	}

	void flattenRuns(const std::vector<Docs::Run>& runs, std::string& out)
	{
		for (const Docs::Run& r : runs) add(out, r.text);
	}

	void flattenCells(const Docs::Cells& cells, std::string& out)
	{
		for (const Docs::Cell& c : cells) flattenRuns(c, out);
	}

	void flattenBlock(const Docs::Block& b, std::string& out)
	{
		flattenRuns(b.runs, out);
		flattenCells(b.items, out);
		flattenCells(b.head, out);
		for (const Docs::Cells& r : b.rows) flattenCells(r, out);
		add(out, b.title);
		add(out, b.text);
		add(out, b.sub);
		add(out, b.alt);
		for (const Docs::Block::Step& s : b.steps) { add(out, s.label); add(out, s.sub); }
		for (const Docs::Block& inner : b.blocks) flattenBlock(inner, out);
	}
} // namespace

Docs::Section section(std::string id, std::string eyebrow, std::string title,
                      std::vector<Docs::Block> blocks)
{
	Docs::Section s;
	s.id      = std::move(id);
	s.eyebrow = std::move(eyebrow);
	s.title   = std::move(title);
	s.blocks  = std::move(blocks);

	s.text = s.title;
	add(s.text, s.eyebrow);
	for (const Docs::Block& b : s.blocks) flattenBlock(b, s.text);
	return s;
}

Docs::Page page(std::string id, std::string title, std::string summary,
                std::vector<Docs::Section> sections)
{
	Docs::Page p;
	p.id       = std::move(id);
	p.title    = std::move(title);
	p.summary  = std::move(summary);
	p.sections = std::move(sections);
	// No `file`: these pages have no counterpart on the website, so there is no
	// online address to hand out. Library::url() falls back to the docs root,
	// which is the honest answer.
	return p;
}

// ── The worked example ───────────────────────────────────────────────────────
// One complete guide lives here rather than in a track file, as the shape the
// four tracks copy: what you end up with, the steps in order, and a Warning at
// every point where the editor does nothing instead of complaining.
//
// Everything it names was read off the code that draws it — the component menu
// and Details rows in InspectorPanel.cpp, the node display names in the HE::api
// registry (EngineApi.cpp) and HorizonCode's own node names.
namespace
{
	std::string nodes(const std::string& anchor = {})
	{
		return anchor.empty() ? std::string(NodeReference::kPageId)
		                      : std::string(NodeReference::kPageId) + "#" + anchor;
	}

	Docs::Page npcChaseGuide()
	{
		std::vector<Docs::Section> secs;

		secs.push_back(section("what-you-get", "Guide", "What you end up with", {
			lead("An enemy that stands in your level, works out a route to the "
			     "player every frame and walks it — solid, stopped by walls, and "
			     "running the same way in a packaged game as in the editor."),
			para("Four pieces have to be in place, and each one fails quietly on its "
			     "own if the piece before it is missing:"),
			bullets({
				"Ground the NPC can stand on AND that navigation was baked from — "
				"two separate things.",
				"A baked NavMesh, saved with the scene.",
				"An NPC with a body, a Character Controller and a Kinematic Rigid Body.",
				"A Nav Agent, and something that keeps pointing it at the player.",
			}),
			para("Nothing here needs typed code. The chase itself is six nodes in a "
			     "HorizonCode class; the same six calls in Lua or Python are at the "
			     "end of this page."),
		}));

		secs.push_back(section("ground", "Step 1", "Give the ground two separate things", {
			lead("Collision and navigation are different systems and neither implies "
			     "the other. This is the single most common way this guide fails, and "
			     "it fails without a message."),
			steps({
				{ "Select the floor",
				  "In the Outliner, pick the mesh the NPC is meant to walk on." },
				{ "Add Component -> Rigid Body",
				  "Set Body Type to Static. This is the half that stops things falling "
				  "through." },
				{ "Add Component -> Collider",
				  "The shape physics actually tests against. A Box under a flat floor "
				  "is fine." },
				{ "Leave the mesh visible",
				  "The navigation bake collects visible static meshes. A floor hidden "
				  "in the Outliner is not walkable, however solid it is." },
			}),
			warn("A NavMesh does not hold anything up", {
				para("The NavMesh is a map of where walking is allowed. It carries no "
				     "weight. With a perfect bake and no Rigid Body, the NPC drops "
				     "through the floor the moment you press Play — the path is fine, "
				     "the ground is not there."),
			}),
			note("Terrain needs nothing extra", {
				para("Terrain chunks are collected by the bake alongside static meshes, "
				     "so a landscape is walkable without a Rigid Body of its own."),
			}),
		}));

		secs.push_back(section("navmesh", "Step 2", "Bake the NavMesh", {
			lead("One NavMesh covers the whole scene. It does not belong to the floor, "
			     "so it can sit on an entity of its own."),
			steps({
				{ "Create an entity for it",
				  "Any entity will do, including an empty one." },
				{ "Add Component -> Nav Mesh",
				  "Cell Size, Cell Height, Walk Height, Walk Climb, Walk Radius and Max "
				  "Slope appear in the Details panel." },
				{ "Set Walk Height and Walk Radius to your character",
				  "Walk Height is the headroom an agent needs; Walk Radius is how far "
				  "the walkable surface is pulled back from walls so the agent does not "
				  "clip corners. Walk Climb is the tallest step it walks up without a "
				  "ramp." },
				{ "Press Bake",
				  "It collects the scene's static geometry first, then builds from it." },
				{ "Read the two lines above the button",
				  "\"Geometry: N verts N tris\" must show a number greater than zero, "
				  "and \"NavMesh:\" must read \"baked\"." },
				{ "Tick Show NavMesh",
				  "Draws the walkable surface in the viewport. It is the only reliable "
				  "way to see what was actually baked. Editor only — it never appears "
				  "in the game." },
				{ "Save the scene",
				  "Not optional. See the warning below." },
			}),
			warn("\"Geometry: 0 verts\" means the bake found nothing", {
				para("Bake takes visible STATIC meshes and terrain chunks, and nothing "
				     "else. Moving bodies, characters and triggers are left out on "
				     "purpose. A floor authored as a Dynamic or Kinematic Rigid Body is "
				     "therefore invisible to it, and produces an empty NavMesh with no "
				     "error anywhere. Zero verts is the symptom; Body Type is the cause."),
			}),
			warn("Baking and not saving ships a game with no NavMesh", {
				para("The scene file stores the collected GEOMETRY, not the finished "
				     "NavMesh — the engine bakes again when the scene loads. So the bake "
				     "you did in the editor only survives if you also save. A packaged "
				     "build made from an unsaved scene has no NavMesh at all, and every "
				     "agent in it simply stands there."),
			}),
			tip("Bake again after you move the level", {
				para("Nothing rebakes on its own while you edit. A wall you dragged, a "
				     "floor you widened or a ramp you deleted is not in the NavMesh "
				     "until you press Bake again — and save again."),
			}),
		}));

		secs.push_back(section("npc", "Step 3", "Build the NPC", {
			lead("Three components make something that walks: a body to look at, a "
			     "controller to move it, and a collider so the rest of the world "
			     "notices it."),
			steps({
				{ "Create an entity and give it Add Component -> Mesh",
				  "The visible body. Any mesh; a capsule or a character model." },
				{ "Add Component -> Character Controller",
				  "The part that actually walks, is kept on the ground and is stopped "
				  "by walls." },
				{ "Add Component -> Rigid Body, Body Type Kinematic",
				  "The collider everything else sees. This is the same combination the "
				  "engine's own player character is built from." },
				{ "Optional: Add Component -> Collider, Shape Capsule or Sphere",
				  "Only to size the capsule. Without a Collider the character gets a "
				  "default capsule 2.0 m tall with a radius of 0.3 m." },
			}),
			warn("Box and Mesh colliders are ignored on a character", {
				para("A Character Controller understands a capsule. Give it a Box, Mesh, "
				     "Convex Hull or Height Field collider and it falls back to the same "
				     "default capsule as if there were no Collider at all — quietly."),
				para("The entity's scale is not applied to the capsule either. Scaling "
				     "the transform to make a bigger enemy changes the mesh and leaves "
				     "the thing that collides exactly where it was; set the Collider's "
				     "own size instead."),
			}),
			warn("A Nav Agent without a Character Controller never moves", {
				para("A Rigid Body alone is not enough: navigation drives the Character "
				     "Controller, so an agent that has one and not the other does "
				     "nothing at all. It logs a warning every five seconds — the "
				     "notification bell in the top bar is where you will see it."),
			}),
		}));

		secs.push_back(section("agent", "Step 4", "Add the Nav Agent", {
			lead("Add Component -> Nav Agent. Four fields are yours; everything below "
			     "the separator is a live read-out of what the agent is doing right now."),
			table({ "Field", "What it does" }, {
				{ "Target", "Where the agent walks to, as a world position. Moving it "
				            "drops the current path and searches a new one from where "
				            "the agent stands." },
				{ "Speed", "How fast it walks, in metres per second." },
				{ "Stop Dist", "How close to the target counts as arrived, in metres." },
				{ "Auto Start", "Start walking on its own when play begins." },
			}),
			para("Under the separator, \"Path: N pts idx=N MOVING/stopped\" tells you "
			     "whether a route was found and how far along it the agent is, and the "
			     "Go and Stop buttons drive it by hand."),
			heading("Trying it before you script anything"),
			para("Press Go with the scene NOT playing. If the NPC sets off, the NavMesh "
			     "and the agent are talking to each other and the rest is scripting."),
			warn("Go outside of play is a NavMesh probe, not a rehearsal", {
				para("There is no physics world in edit mode, so navigation moves the "
				     "transform directly: the NPC will glide through walls and floors. "
				     "That is expected, and it is not what will happen in play."),
				para("It also writes the entity's AUTHORED position. Press Stop when you "
				     "are done and undo, or you have permanently moved your NPC to "
				     "wherever the probe left it."),
			}),
			heading("Variant A — one fixed destination"),
			para("Set Target, tick Auto Start, press Play. This is the only starter a "
			     "packaged game has, since there is no Details panel to press Go in."),
			warn("Auto Start fires exactly once per session", {
				para("It is latched. Once the agent has been started, stopping it later "
				     "— by arriving, by Stop Moving, by anything — does not start it "
				     "again. Anything that needs to walk more than once needs Move To."),
			}),
		}));

		secs.push_back(section("chase-graph", "Step 5", "Variant B — chase the player", {
			lead("A chase is one line of logic: every frame, ask where the player is and "
			     "send the agent there. Six nodes."),
			steps({
				{ "Add Component -> Script on the NPC",
				  "Pick your HorizonCode class in the Class combo. The same slot takes a "
				  ".lua or .py asset." },
				{ "Add the Tick event to the class",
				  "This is what runs every frame." },
				{ "Get Player Character",
				  "Hands you a reference to the player." },
				{ "Get Entity Of",
				  "Turns that reference into the entity id the rest of the API speaks." },
				{ "Get World Position",
				  "Where the player is, in world space. Read the warning below before "
				  "you pick this node." },
				{ "Break Vector 3",
				  "Move To wants three separate floats, not a vector." },
				{ "Move To",
				  "Leave its Entity pin empty: that means the NPC itself, which is who "
				  "is walking. x, y and z come from Break Vector 3." },
			}),
			warn("Get Position instead of Get World Position", {
				rich({ run("Move To wants a "), run("world", Docs::Style::Bold),
				       run(" position. That is the deliberate exception to this API's "
				           "otherwise local rule, and the two nodes sit next to each "
				           "other in the palette.") }),
				para("If the player is parented under anything at all — a vehicle, a "
				     "platform, a spawn group — Get Position gives you its offset inside "
				     "that parent, and the NPC walks confidently to a spot that is off "
				     "by wherever the parent is. Nothing is logged. The NPC just goes to "
				     "the wrong place."),
			}),
			warn("Ignoring the started pin", {
				para("Move To answers with started. It is false when there is no baked "
				     "NavMesh, when the agent or the target is further than the search "
				     "window of 2 / 4 / 2 metres from the NavMesh, or when the two ends "
				     "sit on walkable islands that are not connected to each other."),
				para("A false leaves the agent standing exactly where it was. Branch on "
				     "it — bark, play an idle, fall back to a patrol — instead of "
				     "dropping it, or the only thing you will see is an enemy doing "
				     "nothing and no clue as to why."),
			}),
			heading("The same chase in Lua"),
			code("enemy.lua",
			     "local M = {}\n"
			     "\n"
			     "function M.onUpdate(self, dt)\n"
			     "    local player = horizon.entity.owned(horizon.player.character())\n"
			     "    local x, y, z = horizon.getPosition(player)\n"
			     "    if not horizon.nav.moveTo(self.entityId, x, y, z) then\n"
			     "        -- no route: idle, bark, give up\n"
			     "    end\n"
			     "end\n"
			     "\n"
			     "return M\n"),
			para("self.entityId is the entity the script sits on. The nav group is "
			     "there in both Lua and Python without any setup, and the calls are "
			     "spelled the same in both — only the entry point differs: Python's "
			     "starter class uses on_update(self, dt), not onUpdate."),
			warn("A text script cannot ask for a world position yet", {
				para("horizon.getPosition answers the LOCAL position, and the transform "
				     "group — the one with the world-space getter — is not exposed to "
				     "Lua or Python. So the Lua version above is only correct while the "
				     "player is not parented under anything."),
				para("The detour: keep the player at the top level of the scene, or "
				     "write the chase as a HorizonCode class, where Get World Position "
				     "is available and this limitation does not exist."),
			}),
			rich({ run("Every node on this page is listed with its pins in the "),
			       link("HorizonCode Node Reference", nodes()), run(".") }),
		}));

		secs.push_back(section("distance", "Step 6", "Knowing when it has arrived", {
			lead("A chase that never ends is not a fight. These are the nodes that tell "
			     "you where the agent is in its journey."),
			table({ "Node", "What it answers" }, {
				{ "Is Moving", "Whether the agent is walking. It goes false by itself on "
				               "arrival — that is how a patrol knows to pick the next "
				               "waypoint." },
				{ "Has Path", "Whether there is a route at all. Check this first." },
				{ "Remaining Distance", "Metres left ALONG the waypoints, not as the "
				                        "crow flies." },
				{ "Set Agent Speed", "Changes the pace without throwing the route away — "
				                     "patrol to charge, in one node." },
				{ "Stop Moving", "Abandons the route and stands still." },
			}),
			warn("Remaining Distance answers -1, not 0, when there is no path", {
				para("So the obvious test — \"remaining distance is under two metres, "
				     "attack\" — is true for every NPC that has no route at all, "
				     "including one that never found the NavMesh. Ask Has Path first, "
				     "then compare."),
			}),
			note("Writing Target does not start a stopped agent", {
				para("You can drive a pursuit by writing the agent's Target instead of "
				     "calling Move To: navigation replans as soon as the target has "
				     "moved more than 0.25 m from the one it planned for. But writing it "
				     "only REPLANS an agent that is already walking. Starting one takes "
				     "Move To, Auto Start or the Go button."),
			}),
		}));

		secs.push_back(section("troubleshooting", "Troubleshooting",
		                       "Press Play — and what to check when nothing happens", {
			lead("Every failure below is silent. None of them crashes, and most of them "
			     "look identical from the outside: an enemy standing still."),
			table({ "What you see", "What to check" }, {
				{ "NPC falls through the floor",
				  "The floor has no Rigid Body / Collider. The NavMesh does not hold "
				  "anything up." },
				{ "NPC never moves, path shows 0 pts",
				  "\"Geometry: 0 verts\" on the Nav Mesh component — the floor is not "
				  "Static, or is hidden." },
				{ "NPC never moves, no warning",
				  "Move To answered false. Wire the started pin up and look at it." },
				{ "NPC never moves, warning every 5 s",
				  "Nav Agent with a Rigid Body but no Character Controller." },
				{ "NPC circles its target forever",
				  "Stop Dist is smaller than one step of movement. Raise it." },
				{ "NPC stops part-way and never arrives",
				  "The route needed more than 256 polygons and was cut short. A warning "
				  "is logged." },
				{ "NPC spawned in mid-air does nothing",
				  "More than 4 m above the NavMesh, so it cannot find the polygon it is "
				  "standing on. Drop it onto the ground." },
				{ "Works in the editor, dead in the packaged game",
				  "The scene was baked but not saved, or Auto Start was never ticked." },
			}),
			tip("The bell is the log", {
				para("Navigation reports its refusals through the notification centre in "
				     "the top bar rather than in the viewport. When an NPC does nothing "
				     "and you cannot see why, that is the first place to look."),
			}),
		}));

		secs.push_back(section("jump", "Player side",
		                       "While you are here: making the player jump", {
			lead("The same Character Controller carries jumping, and it is authored in "
			     "two numbers rather than one."),
			para("On the Character Controller, Jump Speed (m/s) sits directly under "
			     "Gravity (m/s²), and the two together decide how high a jump goes: "
			     "jump speed squared, divided by twice the gravity. The defaults — 5 "
			     "and 9.81 — carry a character about 1.27 m. There is no field for the "
			     "height itself."),
			para("Two nodes fire it: Jump, which uses the Jump Speed on the component, "
			     "and Jump With Speed, which takes the speed on a pin. Both answer "
			     "whether the character actually left the ground — that is the pin to "
			     "hang a sound or a takeoff animation on."),
			warn("A jump that does not fire is diagnosed at Is Grounded, not at speed", {
				para("Under the separator on the Character Controller, Is Grounded, Air "
				     "Time (s) and Velocity are greyed-out live read-outs. Is Grounded "
				     "is the one that answers why nothing happened."),
				para("A jump is still granted for 0.12 s after the feet leave an edge, "
				     "and Air Time is that clock. One jump spends the credit, so holding "
				     "the button down never turns into a double jump."),
				para("A jump speed of zero or below is refused and logged rather than "
				     "silently applied — check the notification bell if a scripted jump "
				     "does nothing at all."),
			}),
		}));

		secs.push_back(section("gaps", "Honestly", "What you do not get yet", {
			lead("So you do not go looking for these: they are not hidden, they are "
			     "not there."),
			bullets({
				"No tags and no teams. An NPC cannot ask which entities are enemies; "
				"Get Player Character is the one entity you can always find, and "
				"anything else has to be a reference you wired up yourself.",
				"No perception. There is no vision cone or hearing. Raycast, from the "
				"NPC towards the player, is the usual stand-in for line of sight.",
				"No patrol component. A route between waypoints is a list you keep in "
				"the class and step through when Is Moving goes false.",
				"No blend spaces and no root motion. The walk animation plays at its own "
				"speed while the agent moves at Speed, so the two are matched by hand "
				"and feet will slide when they disagree.",
				"No particle API. A script cannot fire a muzzle flash or a dust puff; "
				"particle systems are placed and configured on the entity instead.",
			}),
		}));

		return page("guides-npc-chase", "An NPC that chases the player",
		            "A standing enemy that walks a route to the player and is stopped "
		            "by walls — ground, NavMesh, character, agent, and the six nodes "
		            "that aim it.",
		            std::move(secs));
	}

	std::vector<std::string> s_pageIds;
} // namespace

const std::vector<std::string>& pageIds() { return s_pageIds; }

void install(Docs::Library& lib)
{
	// Cleared first: appendPage is replace-by-id and so is idempotent, but this
	// list is not — installing twice (a second reader, a test) would otherwise
	// name every page twice and the sidebar would list it twice.
	s_pageIds.clear();

	std::vector<Docs::Page> all;
	auto take = [&all](std::vector<Docs::Page> pages) {
		for (Docs::Page& p : pages) all.push_back(std::move(p));
	};

	// Append order is sidebar order, so this is the reading order of the whole
	// guides section: get something running, then a character, then a world,
	// then out of the door. The worked example sits with the character guides,
	// which is what it is one of.
	take(firstStepsPages());
	take(characterPages());
	all.push_back(npcChaseGuide());
	take(worldPages());
	take(shippingPages());

	for (Docs::Page& p : all)
	{
		if (p.id.empty()) continue;   // a track that forgot one; not worth a crash
		s_pageIds.push_back(p.id);
		lib.appendPage(std::move(p));
	}
}

} // namespace HE::Ed::Guides
