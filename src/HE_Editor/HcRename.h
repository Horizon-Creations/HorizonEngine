#pragma once
#include <string>
#include <vector>

namespace HorizonCode { struct Graph; struct Node; }

// ── Renaming a HorizonCode member, everywhere it is named ────────────────────
// A function, a variable and an event have no identity beyond their name. Inside
// one graph that is easy to keep straight: the declaration and everything using
// it live in the same nodes-vector. Across assets it is not, because the nodes
// that reach into ANOTHER instance carry the name as a plain string:
//
//     CallExternal          "call the function called X on this Target"
//     GetExternal/SetExternal   "read/write the variable called X on this Target"
//     BindEvent/EmitEvent   "the event called X on this Target"
//
// and nothing on those nodes says WHICH class the Target is. So a project-wide
// rename by name alone is not a rename, it is a guess: it would rewrite calls
// that were pointing at a different class with a same-named function, which is
// worse than the breakage it set out to fix.
//
// What the graph CAN say is where the Target came from. A Ref is produced by a
// Create Object (which names its class), a Cast (which names its class), a Ref
// variable (which records its class), a Get Self or a Get Game Instance. That is
// what `targetClassOf` reads. Every hit it can prove is renamed; every node that
// names the old name but whose Target it cannot prove is REPORTED and left
// alone. Both halves matter — the report is what turns a silent break into a
// visible one.
//
// Everything here is pure: a graph in, a plan out, no ImGui, no ContentManager,
// no file system. The sweep that reads and writes the project's assets is the
// caller's business (HcRenameSweep), and this part is what the tests can drive.
namespace HcRename
{
	enum class Member { Function, Variable, Event };

	// What is being renamed. `classKey` is the asset path the renamed member's
	// class is addressed by, which is exactly what Create Object, Cast and
	// Variable::className store: a HorizonCode class asset path, a widget asset
	// path, or the Game Instance's key.
	struct Target
	{
		std::string classKey;
		Member      member = Member::Function;
		std::string oldName;
		std::string newName;
	};

	// How the graph being planned relates to the renamed class.
	enum class Role
	{
		Declares,    // it IS that class: the declaration and its in-graph users
		Overrides,   // it derives from that class and may re-declare the member
		Other,       // any other graph: only its references reach in
	};

	// The class a Ref-consuming node (Call/Get/Set External, Bind/Emit Event)
	// points at, or "" when this graph cannot prove it. `selfKey` is the graph
	// being read, `giKey` the Game Instance's key ("" when there is none).
	//
	// A node that RECORDS its target class (Node::className, written by the
	// pickers) is believed first; the wire is the fallback that keeps every graph
	// authored before those pickers existed working.
	std::string targetClassOf(const HorizonCode::Graph& g, const HorizonCode::Node& n,
	                          const std::string& selfKey, const std::string& giKey);

	// The class a node PRODUCING a Ref yields — the second half of the walk above,
	// exposed because the graph editor's member menus need the same answer to
	// decide which class's functions to offer. One rule, one place: a menu that
	// offered members this could not later find would be exactly the drift this
	// whole file exists to prevent.
	std::string classOfRefSource(const HorizonCode::Graph& g, const HorizonCode::Node& src,
	                             const std::string& selfKey, const std::string& giKey);

	// One thing a rename would touch. `node` is a node id, or 0 when the hit is a
	// DECLARATION that does not live in a node at all: a variable (Graph::variables)
	// or an event (Graph::events), named by `decl`. `what` is the line the dialog
	// shows.
	struct Hit
	{
		int         node = 0;
		std::string decl;
		std::string what;
	};

	struct Plan
	{
		std::vector<Hit> rename;   // provably ours: rewritten on apply
		std::vector<Hit> unsure;   // names the old name, target unprovable: reported only
		// A rename this graph cannot take without breaking something else — today
		// only the Bind Event case below. Nothing in the graph is touched then.
		std::vector<Hit> blocked;
		std::string      blockedWhy;

		bool touches() const { return !rename.empty(); }
		bool anything() const { return !rename.empty() || !unsure.empty() || !blocked.empty(); }
	};

	// Plan one graph's share. `targetKeys` is the renamed class AND every class
	// deriving from it: a call on a Goblin reaches the function Enemy declared,
	// so a graph calling it must follow the rename too.
	Plan planGraph(const HorizonCode::Graph& g, Role role,
	               const std::vector<std::string>& targetKeys,
	               const std::string& graphKey, const std::string& giKey,
	               const Target& t);

	// Write the plan's renames into the graph. Returns true when anything moved.
	bool apply(HorizonCode::Graph& g, const Plan& p, const Target& t);
}
