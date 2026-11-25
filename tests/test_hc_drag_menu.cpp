#include "doctest.h"

#include "HcEditorUtil.h"

#include <HorizonCode/HorizonCode.h>
#include <HorizonScene/EngineApi.h>

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

// ── The drag-off menu lists every node that fits ─────────────────────────────
// Dragging a wire off a pin and letting go on empty canvas opens a menu of the
// nodes that can take it. Off an OUTPUT of type T it must list every node with
// an input of type T; off an INPUT, every node with an output of type T. The
// menu decides that per node TYPE with dragMatchPin (built-ins) and
// dragMatchApiPin (engine calls); these cases walk every node the registry
// knows, every pin type and every container, and name each one left out.

namespace
{
namespace HC = HorizonCode;
using P  = HC::PinType;
using CK = HC::ContainerKind;
using NT = HC::NodeType;

// Offered by the menu's own sections (variables, functions, references,
// engine calls) or not offered by a drag at all (entry points): the generic
// walk skips them in every host, so they are not its business here.
bool servedElsewhere(NT t)
{
	static const NT kSkip[] = {
		NT::Event, NT::FunctionEntry, NT::FunctionCall, NT::FunctionReturn,
		NT::GetVariable, NT::SetVariable, NT::GetProperty, NT::SetProperty,
		NT::EngineCall, NT::CallExternal, NT::GetExternal, NT::SetExternal,
		NT::BindEvent,
	};
	return std::find(std::begin(kSkip), std::end(kSkip), t) != std::end(kSkip);
}

const P kTypes[] = { P::Float, P::Bool, P::Int, P::String, P::Vec2, P::Color, P::Ref,
                     P::Transform, P::Enum, P::Struct, P::Vec3, P::Vec4, P::Double };
const CK kKinds[] = { CK::None, CK::Array, CK::Set, CK::Map };

const char* kindName(CK k)
{
	switch (k) { case CK::Array: return "[]"; case CK::Set: return "{}"; case CK::Map: return "<>"; default: return ""; }
}
} // namespace

TEST_CASE("HorizonCode drag-off menu: every built-in node with a pin of the dragged type is listed")
{
	std::vector<std::string> missing;
	for (NT t : HC::nodeRegistry())
	{
		if (servedElsewhere(t)) continue;
		// The node as the palette creates it, and as each type parameter makes it:
		// a pin a node CAN have is a pin a drag of that type should find it by.
		std::vector<HC::Node> shapes;
		{ HC::Node n; n.type = t; shapes.push_back(n); }
		for (P p : kTypes)
			for (CK k : kKinds)
			{
				HC::Node n; n.type = t; n.propType = p;
				n.isArray = k != CK::None; n.container = k;
				shapes.push_back(n);
			}
		for (const HC::Node& n : shapes)
		{
			const HC::NodeSig s = HC::signatureOf(n);
			const auto check = [&](const HC::PinDesc& pd, bool pinIsInput) {
				if (pd.type == P::Exec) return;
				// Dragging OFF an output lands on this input, and the other way round.
				const bool dragFromInput = !pinIsInput;
				if (HcEditorUtil::dragMatchPin(t, pd.type, pd.kind(), dragFromInput, false) >= 0) return;
				std::ostringstream o;
				o << HC::nodeDisplayName(t) << ": " << (pinIsInput ? "input " : "output ")
				  << pd.name << " (" << int(pd.type) << kindName(pd.kind()) << ")";
				if (std::find(missing.begin(), missing.end(), o.str()) == missing.end())
					missing.push_back(o.str());
			};
			for (const auto& pd : s.dataIns)  check(pd, true);
			for (const auto& pd : s.dataOuts) check(pd, false);
		}
	}
	std::ostringstream all;
	for (const auto& m : missing) all << "\n  " << m;
	INFO("not listed when dragging that type:" << all.str());
	CHECK(missing.empty());
}

// The sections that list nodes the registry walk cannot see decide with
// dragMatchPinOn on the node as it will be created: a Call bound to one of the
// graph's own functions, and a widget's Get/Set Property bound to an element.
TEST_CASE("HorizonCode drag-off menu: own functions and element properties match data wires")
{
	HC::Node call;
	call.type = NT::FunctionCall;
	call.s = "Damage";
	call.params  = { { "Amount", P::Float, false } };
	call.results = { { "Dead",   P::Bool,  false } };
	CHECK(HcEditorUtil::dragMatchPinOn(call, P::Float, CK::None, /*srcIsInput*/false, false) >= 0);
	CHECK(HcEditorUtil::dragMatchPinOn(call, P::Bool,  CK::None, /*srcIsInput*/true,  false) >= 0);
	CHECK(HcEditorUtil::dragMatchPinOn(call, P::String, CK::None, false, false) < 0);
	CHECK(HcEditorUtil::dragMatchPinOn(call, P::Exec, CK::None, false, true) >= 0);

	HC::Node set;
	set.type = NT::SetProperty; set.elem = 7; set.s = "Text"; set.propType = P::String;
	CHECK(HcEditorUtil::dragMatchPinOn(set, P::String, CK::None, false, false) >= 0);  // feeds Value
	CHECK(HcEditorUtil::dragMatchPinOn(set, P::String, CK::None, true,  false) >= 0);  // passes it on
	HC::Node get = set; get.type = NT::GetProperty;
	CHECK(HcEditorUtil::dragMatchPinOn(get, P::String, CK::None, true,  false) >= 0);
	CHECK(HcEditorUtil::dragMatchPinOn(get, P::String, CK::None, false, false) < 0);   // Get has no input
}

TEST_CASE("HorizonCode drag-off menu: every engine call with a pin of the dragged type is listed")
{
	std::vector<std::string> missing;
	for (const HE::api::ApiFn& fn : HE::api::registry())
	{
		for (const auto& p : fn.params)
			if (HcEditorUtil::dragMatchApiPin(fn, p.type, p.isArray ? CK::Array : CK::None, false, false) < 0)
				missing.push_back(std::string(fn.id) + " param " + p.name);
		for (const auto& r : fn.results)
			if (HcEditorUtil::dragMatchApiPin(fn, r.type, r.isArray ? CK::Array : CK::None, true, false) < 0)
				missing.push_back(std::string(fn.id) + " result " + r.name);
	}
	std::ostringstream all;
	for (const auto& m : missing) all << "\n  " << m;
	INFO("not listed:" << all.str());
	CHECK(missing.empty());
}
