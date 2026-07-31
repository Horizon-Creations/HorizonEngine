#include "HcGraphShortcuts.h"

namespace HcGraphShortcuts
{
namespace
{
using NT = HC::NodeType;

// B/S/D/F/O are Unreal's Blueprint bindings, key for key — anyone coming from
// Blueprints keeps their muscle memory. P/V/N are the HorizonCode additions:
// Print is the debug node you place all day, Is Valid guards every object
// reference before it is touched, and Not is the one logic node you constantly
// splice into an existing wire.
//
// NOT bound on purpose: literals (Const Float/Int/…) — an unwired simple data
// input edits its value right on the pin (Node::pinDefaults), so a literal node
// is the exception now, not the rule.
const std::vector<Binding> kBindings = {
	{ 'B', NT::Branch,   "B" },
	{ 'S', NT::Sequence, "S" },
	{ 'D', NT::Delay,    "D" },
	{ 'F', NT::ForEach,  "F" },
	{ 'O', NT::DoOnce,   "O" },
	{ 'P', NT::Print,    "P" },
	{ 'V', NT::IsValid,  "V" },
	{ 'N', NT::Not,      "N" },
};

const std::vector<char> kReserved = { 'G', 'E', 'Q' };
} // namespace

const std::vector<Binding>& bindings() { return kBindings; }

const char* hintFor(HC::NodeType t)
{
	for (const Binding& b : kBindings)
		if (b.type == t) return b.hint;
	return nullptr;
}

const std::vector<char>& reservedKeys() { return kReserved; }

} // namespace HcGraphShortcuts
