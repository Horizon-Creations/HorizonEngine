#pragma once

// ─── Layer 3a — a HorizonCode Value on the wire, and two of them compared ────
// Property replication needs exactly two things of a value that neither the
// snapshot path nor the JSON path can give it:
//
//   ENCODE/DECODE onto a BitWriter. The snapshot's quantized position is no
//   model here — a property is Bool, Int, String, a Struct, a Map of Enums, and
//   it has to arrive EXACTLY as it was written, because "health lost a bit of
//   precision" is a bug and "the door is 0.998 open" is nonsense. So: no
//   quantization, the type as the first byte, and recursion through containers
//   and struct fields.
//
//   COMPARE, recursively. Dirty tracking is "is this different from what I last
//   sent" (plan §6.2), and the answer has to be exact for the same reason.
//
// WHY THE TYPE BYTE TRAVELS. It costs one byte per value and buys the check
// the plan asks for in §6.2: a client that receives a value whose type does not
// match the property it declared drops the delta with a log line instead of
// writing an Int into a String and finding out three frames later. The table
// (kMsgPropertyTable) already named the types; the byte is what lets the
// receiver NOTICE when the two sides disagree, which is precisely the case
// where trusting the table would be wrong.
//
// WHY NOT `scalarValueEquals` ALONE. It exists (HorizonCode.h) and it is the
// right leaf comparison — it is what Set/Map membership runs on, so using it
// here keeps "equal" meaning one thing across the engine. But it has no Struct
// case and does not descend into containers (its own callers note this), and a
// struct property that compared equal to everything would never replicate. The
// recursion is the part that is missing, and it is the only part added here.
//
// WHAT IS NOT SUPPORTED. `Ref`: an InstanceId is a local handle that names
// nothing on the other machine (plan §6.1). writeValue refuses it and returns
// false rather than writing a number that would resolve to a stranger's object.
// A Map with Ref keys is refused for the same reason.

#include <HorizonCode/HorizonCode.h>
#include <Net/BitStream.h>

#include <string>
#include <vector>

namespace HE::Net::Game {

// Is this type allowed to cross? Everything but Ref (plan §6.1). Also the
// answer the inspector's checkbox and the HC variable list give a user, so the
// rule is stated once.
bool isReplicableType(HorizonCode::PinType t);

// Why not, in one sentence, for a tooltip and a log line. Empty when it may.
const char* replicationRefusalReason(HorizonCode::PinType t);

// Encode `v` (scalar or container, recursive). False = refused (a Ref anywhere
// in it); nothing is written in that case that a reader would accept, but the
// writer may already hold partial bits — callers write into a FRESH BitWriter
// and throw it away on false, which is what every caller here does.
bool writeValue(BitWriter& w, const HorizonCode::Value& v);

// Decode into `out`. False = the stream ran out, or carried a type byte that is
// not a PinType, or a Ref. Never throws, never asserts: the bytes come from the
// network, and a malformed one is an ordinary event.
bool readValue(BitReader& r, HorizonCode::Value& out);

// Exact equality, recursing through containers and struct fields. Different
// types are never equal — including "Array of Int" against "scalar Int", which
// is the shape a mis-declared variable has.
bool valuesEqual(const HorizonCode::Value& a, const HorizonCode::Value& b);

// Do these two describe the SAME property? Type, container kind and, for the
// user-defined types, the definition name. This is the check a client runs
// before applying a delta (plan §6.2 "dessen Typ nicht passt"); it deliberately
// ignores the payload, so an empty array still matches a full one.
bool valueTypesMatch(const HorizonCode::Value& a, const HorizonCode::Value& b);

// ── A call's arguments as JSON, for the native module boundary (plan §7.2) ───
// IGameLogic::onRpc takes a string and not a Value list, the same trade
// IGameLogic::onRep makes and for the same reason: a HorizonCode::Value is a
// C++ type with strings and vectors in it, and this interface crosses into a
// hot-loaded dylib that is rebuilt on its own schedule. A JSON array costs a
// parse and survives that boundary; a vector of Values does not.
//
// Always a JSON ARRAY, one element per argument, in call order. Scalars become
// the obvious JSON scalar (an Enum its integer, a Vec3 a three-element array);
// Array and Set become arrays, Map an array of {"key":…,"value":…} pairs so the
// authored order survives — the lesson `horizoncode-containers` records about
// nlohmann sorting object keys. A Ref never gets this far (writeValue refused
// it) and reads as null if one somehow does.
std::string argsToJson(const std::vector<HorizonCode::Value>& args);

} // namespace HE::Net::Game
