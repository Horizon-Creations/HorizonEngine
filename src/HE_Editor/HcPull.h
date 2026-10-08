#pragma once
#include <HorizonCode/HorizonCode.h>
#include <string>
#include <vector>

namespace HE { struct StructDef; }

// ── Pull on Construct, the editor's half (docs/state-driven-data-exchange-design.md §2.9)
// What the variable details show about a pull — which source variables fit,
// whether the authored one still resolves, and what "Add to Target" writes into
// the source — decided WITHOUT ImGui and without the content system, so the
// decisions can be asserted in he_tests the way HcRename's are. HcPullUi draws
// it; the sentences come from HorizonCode::pullFailureText, which is what the
// runtime warns with, so the editor's red line and the log say the same thing.
namespace HcPull
{
	// One PUBLIC instance variable of a source class, with the whole shape —
	// the class registry's MemberVar has no typeName, and without it one struct
	// cannot be told from another (design §2.10).
	struct SourceVar
	{
		std::string                name;
		HorizonCode::PinType       type = HorizonCode::PinType::Float;
		HorizonCode::ContainerKind kind = HorizonCode::ContainerKind::None;
		HorizonCode::PinType       keyType = HorizonCode::PinType::String;
		std::string                typeName;
		std::string                className;
	};
	// The variables a reference may read — access 0, scope 0, the same door
	// Get (Ref) and the runtime's pull use.
	std::vector<SourceVar> publicVariables(const HorizonCode::Graph& g);

	// Can `sv` (or, with `member`, that field of it) land in `target`? Empty =
	// yes; otherwise the tooltip ("Float, needs Int", "Struct RunStats: pick a
	// member"). Member names resolve through formerNames like at run time.
	std::string fitOf(const HorizonCode::Variable& target, const SourceVar& sv,
	                  const std::string& member = {});
	// The struct fields of `sv` that fit `target`, live names, definition order.
	// Empty when sv is no scalar struct or its definition is not registered.
	std::vector<std::string> fittingMembers(const HorizonCode::Variable& target,
	                                        const SourceVar& sv);

	// The status line under the pickers.
	struct Status
	{
		bool                     ok = false;
		HorizonCode::PullFailure why = HorizonCode::PullFailure::None;
		std::string              text;   // ok: "Game Instance › LastRun › Score (Int)"
		// The authored member resolved under a FORMER name; this is the live one
		// the editor writes back (empty when nothing to rewrite).
		std::string              liveMember;
	};
	// `source` = the source class's variables, or null when the class is not
	// known at edit time (a Creator pull without a class): that is not an error,
	// the runtime decides, and the line says so.
	Status check(const HorizonCode::Variable& target, const std::vector<SourceVar>* source);

	// "Pulled from Game Instance › LastRun › Score" — the variable list's tooltip
	// ("Bound to …" for Bind To).
	std::string describe(const HorizonCode::Variable& v);

	// ── Bind To (docs/bind-to-variable-binding-plan.md §5.1) ─────────────────
	// The object references a Bind To may read through: the owner's scalar Ref
	// instance variables, declaration order. Only the owner's own level — an
	// inherited one is typed in by name and checked at run time.
	std::vector<std::string> refVariables(const HorizonCode::Graph& owner);
	// The class (asset path) the reference `ref` of `owner` is declared to hold
	// — its className — or "" when it names no Ref of the graph or holds any.
	std::string refClassOf(const HorizonCode::Graph& owner, const std::string& ref);
	// The class a pull or binding of `v` reads from and "Add to Target" writes
	// into, when it is one asset: the expected creator class, or the
	// reference's class. "" for the Game Instance and for unknown classes.
	std::string sourceClassOf(const HorizonCode::Variable& v, const HorizonCode::Graph& owner);

	// ── Add to Target ────────────────────────────────────────────────────────
	// The source lacks what the pull names; write it into the source's graph.
	enum class Add
	{
		AddedVariable,     // a public variable of the target's shape and default
		AddedStructVar,    // a public Struct variable of `structType` (member pulls)
		MadePublic,        // it was there but private: access set to public
		NeedsStructType,   // a member pull, the variable is missing: which struct?
		NeedsMemberInStruct, // the struct variable exists, the FIELD is missing: addMember
		NothingToDo,       // already resolves
		Refused,           // name clash with something else, or no variable named
	};
	// `structType` is only read for a member pull whose variable is missing:
	// the definition to declare it as (one of structsWithMember). `what`
	// receives the line the editor logs.
	Add addToTarget(HorizonCode::Graph& source, const HorizonCode::Variable& target,
	                const std::string& structType = {}, std::string* what = nullptr);
	// The member case where the variable exists but its struct lacks the field:
	// append a field of the target's type and default to `def`. False when the
	// field is already there (live or former name) or the target is a container
	// shape a field cannot mirror.
	bool addMember(HE::StructDef& def, const HorizonCode::Variable& target,
	               const std::string& member);
	// Registered struct definitions with a field `member` that fits `target` —
	// what the "Struct type" combo of the member case offers.
	std::vector<std::string> structsWithMember(const HorizonCode::Variable& target,
	                                           const std::string& member);
}
