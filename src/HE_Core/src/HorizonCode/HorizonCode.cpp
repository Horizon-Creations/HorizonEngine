#include <HorizonCode/HorizonCode.h>
#include <cstdint>
#include <Application/InputAssets.h>   // the ONE spelling of the input event names
#include <Diagnostics/Logger.h>
#include <Scripting/ScriptTypes.h>   // scriptLogLine — the Print node's language tag
#include <Types/TypeRegistry.h>   // struct/enum definitions (field/entry resolution)
#include <GraphCommon/GraphJson.h>
#include <GraphCommon/GraphModel.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <mutex>
#include <array>
#include <cmath>
#include <cstddef>   // std::ptrdiff_t — iterator arithmetic in the container nodes
#include <cstdio>
#include <cstring>
#include <unordered_set>
#include <vector>

namespace HorizonCode {

using T = NodeType;
using P = PinType;
using CK = ContainerKind;

// ── Container helpers (Set / Map) ────────────────────────────────────────────
// Sets and maps iterate in INSERTION ORDER (see ContainerKind in the header), so
// both are plain parallel vectors and every lookup here is a linear scan by
// value equality. That is deliberate: a hash index would have to be rebuilt on
// every copy — the container nodes are PURE and copy their input — and the graphs
// that hold thousands of entries are the ones that should be in C++ anyway.
// Generated code uses the same shape (hc::Set/hc::Map), which is what makes the
// two backends order-identical by construction rather than by agreement.
namespace
{
// Index of `needle` among `hay`, or -1.
int indexOfValue(const std::vector<Value>& hay, const Value& needle, PinType t)
{
    for (size_t i = 0; i < hay.size(); ++i)
        if (scalarValueEquals(hay[i], needle, t)) return (int)i;
    return -1;
}

// Drop later duplicates, keeping the FIRST occurrence's position — the rule a
// Set Add follows, applied to a whole list (a seeded default, a boundary
// conversion, an array a script handed over).
void dedupeSetItems(Value& set)
{
    std::vector<Value> out;
    for (Value& it : set.items)
        if (indexOfValue(out, it, set.type) < 0) out.push_back(std::move(it));
    set.items = std::move(out);
}

// Same for a map: a repeated key keeps its first position and takes the LAST
// value written to it, which is what a sequence of Map Set calls would leave.
void dedupeMapKeys(Value& map)
{
    std::vector<Value> ks, vs;
    for (size_t i = 0; i < map.keys.size() && i < map.items.size(); ++i)
    {
        const int at = indexOfValue(ks, map.keys[i], map.keyType);
        if (at < 0) { ks.push_back(std::move(map.keys[i])); vs.push_back(std::move(map.items[i])); }
        else        { vs[(size_t)at] = std::move(map.items[i]); }
    }
    map.keys = std::move(ks); map.items = std::move(vs);
}
} // namespace

// ── Node signatures ──────────────────────────────────────────────────────────

namespace
{
// A Cast node's output pin reads "As Goblin", but PinDesc::name is a borrowed
// const char* that has to outlive the call — so the readable name cannot be
// composed here. It is MIRRORED onto the node instead (params[0].name), exactly
// the way EngineCall mirrors its ApiFn's parameters: the pin then resolves from
// the node alone, with no class registry in reach and no temporary to dangle.
// The editor writes the mirror when the target changes; an empty one (a
// hand-built node, an older document) simply falls back to the bare label.
const char* castOutPinName(const Node& n)
{
    return (n.params.empty() || n.params[0].name.empty()) ? "As" : n.params[0].name.c_str();
}

// The one switch that knows every node's pins. Fills `s` IN PLACE — clearing its
// vectors without shrinking them — so the allocation-free accessors below can
// reuse a single scratch NodeSig instead of building four heap vectors per query.
void signatureInto(const Node& n, NodeSig& s)
{
    s.execIns.clear(); s.execOuts.clear(); s.dataIns.clear(); s.dataOuts.clear();
    // A pin's user-defined type: the node's own definition path, or null when it
    // names none (PinDesc's contract — see HorizonCode.h). Spelled once here
    // instead of at every descriptor below; `defOf` is for the mirrored
    // param/result/field lists, which carry their own paths.
    auto defOf = [](const std::string& t) -> const char* { return t.empty() ? nullptr : t.c_str(); };
    const char* const tn = defOf(n.typeName);
    // A Map node's KEY definition (Enum keys), the same way `tn` is the value's.
    const char* const ktn = defOf(n.keyTypeName);
    // One pin mirroring a declared param/result — container fields and all.
    // Spelled once instead of at each of the six mirror sites below, which is
    // also what keeps them from drifting the next time a field is added.
    auto paramPin = [&defOf](const FuncParam& p) -> PinDesc
    { return { p.name.c_str(), p.type, p.isArray, defOf(p.typeName),
               p.container, p.keyType, defOf(p.keyTypeName) }; };
    switch (n.type)
    {
    case T::Event:
        s.execOuts = { { "", P::Exec } };
        // "Value" is right for an event whose payload is whatever it carries,
        // but the per-frame ones all carry the same thing and should say so —
        // reading "Value: Float" off a Tick tells you nothing you could not
        // already see. The literal outlives the call, like every borrowed pin
        // name in here.
        if (n.hasArg)
        {
            const bool perFrame = (n.s == "Tick" || n.s == "Update");
            s.dataOuts = { { perFrame ? "Delta Time" : "Value", n.propType, false, tn } };
        }
        break;
    case T::InputAction:
        // Three shapes, from two fields that both already persist: hasArg says
        // "this action is an axis", propType which KIND of axis. A digital
        // action has the press/release pair; an axis has one chain per frame
        // carrying its value, Float for one dimension and Vec2 for two.
        if (n.hasArg)
        {
            s.execOuts = { { "", P::Exec } };
            s.dataOuts = { { "Value", n.propType == P::Vec2 ? P::Vec2 : P::Float } };
        }
        else
        {
            s.execOuts = { { "Pressed", P::Exec }, { "Released", P::Exec } };
        }
        break;
    case T::FunctionEntry:
        s.execOuts = { { "", P::Exec } };
        for (const auto& p : n.params) s.dataOuts.push_back(paramPin(p));
        break;
    case T::FunctionCall:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "", P::Exec } };
        for (const auto& p : n.params)  s.dataIns.push_back(paramPin(p));
        for (const auto& r : n.results) s.dataOuts.push_back(paramPin(r));
        break;
    case T::FunctionReturn:
        s.execIns = { { "", P::Exec } };
        for (const auto& r : n.results) s.dataIns.push_back(paramPin(r));
        break;
    case T::Branch:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "True", P::Exec }, { "False", P::Exec } };
        s.dataIns  = { { "Cond", P::Bool } };
        break;
    case T::Sequence:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "Then 0", P::Exec }, { "Then 1", P::Exec } };
        break;
    case T::GetProperty:
        s.dataOuts = { { "Value", n.propType, false, tn } };
        break;
    case T::SetProperty:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "", P::Exec } };
        s.dataIns  = { { "Value", n.propType, false, tn } };
        s.dataOuts = { { "Value", n.propType, false, tn } }; // pass the set value through
        break;
    case T::GetVariable:
        s.dataOuts = { { "Value", n.propType, n.isArray, tn, n.container, n.keyType, ktn } };
        break;
    case T::SetVariable:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "", P::Exec } };
        s.dataIns  = { { "Value", n.propType, n.isArray, tn, n.container, n.keyType, ktn } };
        // pass the set value through
        s.dataOuts = { { "Value", n.propType, n.isArray, tn, n.container, n.keyType, ktn } };
        break;
    case T::ShowSelf:
    case T::HideSelf:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "", P::Exec } };
        break;
    case T::CreateWidget:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "", P::Exec } };
        s.dataOuts = { { "Widget", P::Ref } };
        break;
    case T::ShowWidget:
    case T::HideWidget:
    case T::DestroyWidget:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "", P::Exec } };
        s.dataIns  = { { "Widget", P::Ref } };
        break;
    case T::CreateObject:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "", P::Exec } };
        // Optional placement. WIRED means "put it there"; UNWIRED means "leave
        // it where the class authored it" — the interpreter and the codegen both
        // decide on the LINK, never on the value, so an unwired pin is not the
        // same as one wired to (0,0,0). Adding these shifted the Object output's
        // absolute pin index (2 → 4); fromJson migrates graphs saved before that.
        s.dataIns  = { { "Location", P::Vec3 }, { "Rotation", P::Vec3 } };
        s.dataOuts = { { "Object", P::Ref } };
        break;
    case T::DestroyObject:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "", P::Exec } };
        s.dataIns  = { { "Object", P::Ref } };
        break;
    case T::GetExternal:
        s.dataIns  = { { "Target", P::Ref } };
        s.dataOuts = { { "Value", n.propType, false, tn } };
        break;
    case T::SetExternal:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "", P::Exec } };
        s.dataIns  = { { "Target", P::Ref }, { "Value", n.propType, false, tn } };
        s.dataOuts = { { "Value", n.propType, false, tn } }; // pass the set value through
        break;
    case T::BindEvent:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "", P::Exec } };
        s.dataIns  = { { "Target", P::Ref } };
        break;
    case T::CallExternal:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "", P::Exec } };
        s.dataIns  = { { "Target", P::Ref } };
        for (const auto& p : n.params)  s.dataIns.push_back(paramPin(p));
        for (const auto& r : n.results) s.dataOuts.push_back(paramPin(r));
        break;
    case T::EmitEvent:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "", P::Exec } };
        if (n.hasArg) s.dataIns = { { "Arg", n.propType, false, tn } };
        break;
    case T::EngineCall:
        // hasArg = the registry entry's isExec: side-effecting calls get exec pins,
        // pure calls (getters/math) are compact data nodes. params → data-ins,
        // results → data-outs (mirrored on the node from the ApiFn descriptor).
        if (n.hasArg) { s.execIns = { { "", P::Exec } }; s.execOuts = { { "", P::Exec } }; }
        for (const auto& p : n.params)  s.dataIns.push_back(paramPin(p));
        for (const auto& r : n.results) s.dataOuts.push_back(paramPin(r));
        break;
    case T::GetGameInstance: s.dataOuts = { { "Game Instance", P::Ref } }; break;
    case T::GetSelf:         s.dataOuts = { { "Self", P::Ref } };          break;
    case T::ConstFloat:  s.dataOuts = { { "", P::Float } };  break;
    case T::ConstBool:   s.dataOuts = { { "", P::Bool } };   break;
    case T::ConstInt:    s.dataOuts = { { "", P::Int } };    break;
    case T::ConstString: s.dataOuts = { { "", P::String } }; break;
    case T::ConstVec2:   s.dataOuts = { { "", P::Vec2 } };   break;
    case T::ConstColor:  s.dataOuts = { { "", P::Color } };  break;
    case T::ConstTransform: s.dataOuts = { { "", P::Transform } }; break;
    case T::ArrayMake:
        s.dataOuts = { { "Array", n.propType, true, tn } };
        break;
    case T::ArrayLength:
        s.dataIns  = { { "Array", n.propType, true, tn } };
        s.dataOuts = { { "Length", P::Int } };
        break;
    case T::ArrayGet:
        s.dataIns  = { { "Array", n.propType, true, tn }, { "Index", P::Int } };
        s.dataOuts = { { "Element", n.propType, false, tn } };
        break;
    case T::ArrayAdd:
        s.dataIns  = { { "Array", n.propType, true, tn }, { "Value", n.propType, false, tn } };
        s.dataOuts = { { "Array", n.propType, true, tn } };
        break;
    case T::ArraySet:
        s.dataIns  = { { "Array", n.propType, true, tn }, { "Index", P::Int }, { "Value", n.propType, false, tn } };
        s.dataOuts = { { "Array", n.propType, true, tn } };
        break;
    case T::ArrayInsert:
        s.dataIns  = { { "Array", n.propType, true, tn }, { "Index", P::Int }, { "Value", n.propType, false, tn } };
        s.dataOuts = { { "Array", n.propType, true, tn } };
        break;
    case T::ArrayRemove:
        s.dataIns  = { { "Array", n.propType, true, tn }, { "Index", P::Int } };
        s.dataOuts = { { "Array", n.propType, true, tn } };
        break;
    case T::ArrayContains:
        s.dataIns  = { { "Array", n.propType, true, tn }, { "Value", n.propType, false, tn } };
        s.dataOuts = { { "Contains", P::Bool } };
        break;
    case T::ArrayIndexOf:
        s.dataIns  = { { "Array", n.propType, true, tn }, { "Value", n.propType, false, tn } };
        s.dataOuts = { { "Index", P::Int } };
        break;
    case T::ForEach:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "Body", P::Exec }, { "Done", P::Exec } };
        s.dataIns  = { { "Array", n.propType, true, tn } };
        s.dataOuts = { { "Element", n.propType, false, tn }, { "Index", P::Int } };
        break;

    // ── Set<T> ───────────────────────────────────────────────────────────────
    // Every container pin is ContainerKind::Set; the element pins are scalars of
    // the same type, exactly as with the Array nodes.
#define HC_SET_PIN(label) PinDesc{ label, n.propType, true, tn, HorizonCode::ContainerKind::Set }
    case T::SetMake:
        s.dataOuts = { HC_SET_PIN("Set") };
        break;
    case T::SetLength:
        s.dataIns  = { HC_SET_PIN("Set") };
        s.dataOuts = { { "Length", P::Int } };
        break;
    case T::SetClear:
        s.dataIns  = { HC_SET_PIN("Set") };
        s.dataOuts = { HC_SET_PIN("Set") };
        break;
    case T::SetAdd:
    case T::SetRemove:
        s.dataIns  = { HC_SET_PIN("Set"), { "Value", n.propType, false, tn } };
        s.dataOuts = { HC_SET_PIN("Set") };
        break;
    case T::SetContains:
        s.dataIns  = { HC_SET_PIN("Set"), { "Value", n.propType, false, tn } };
        s.dataOuts = { { "Contains", P::Bool } };
        break;
    case T::SetToArray:
        s.dataIns  = { HC_SET_PIN("Set") };
        s.dataOuts = { { "Array", n.propType, true, tn } };
        break;
    case T::ForEachSet:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "Body", P::Exec }, { "Done", P::Exec } };
        s.dataIns  = { HC_SET_PIN("Set") };
        s.dataOuts = { { "Element", n.propType, false, tn }, { "Index", P::Int } };
        break;
    case T::SetFromArray:
        // Set To Array read backwards, and the same pin descriptors — which is
        // what makes the pair round-trip on the canvas.
        s.dataIns  = { { "Array", n.propType, true, tn } };
        s.dataOuts = { HC_SET_PIN("Set") };
        break;
    case T::SetUnion:
    case T::SetIntersect:
    case T::SetDifference:
        // "A" and "B" and not "Set"/"Other": Difference is the one op where the
        // two sides are not interchangeable, and a pin pair that reads the same
        // on all three is what stops someone wiring it the wrong way round.
        s.dataIns  = { HC_SET_PIN("A"), HC_SET_PIN("B") };
        s.dataOuts = { HC_SET_PIN("Set") };
        break;
#undef HC_SET_PIN

    // ── Map<K,V> ─────────────────────────────────────────────────────────────
    // `propType`/`tn` are the VALUE type, `keyType`/`ktn` the key — so a map pin
    // needs both, and the key pins are scalars of the latter.
#define HC_MAP_PIN(label) PinDesc{ label, n.propType, true, tn, \
                                   HorizonCode::ContainerKind::Map, n.keyType, ktn }
#define HC_KEY_PIN(label) PinDesc{ label, n.keyType, false, ktn }
    case T::MapMake:
        s.dataOuts = { HC_MAP_PIN("Map") };
        break;
    case T::MapLength:
        s.dataIns  = { HC_MAP_PIN("Map") };
        s.dataOuts = { { "Length", P::Int } };
        break;
    case T::MapClear:
        s.dataIns  = { HC_MAP_PIN("Map") };
        s.dataOuts = { HC_MAP_PIN("Map") };
        break;
    case T::MapSet:
        s.dataIns  = { HC_MAP_PIN("Map"), HC_KEY_PIN("Key"),
                       { "Value", n.propType, false, tn } };
        s.dataOuts = { HC_MAP_PIN("Map") };
        break;
    case T::MapRemove:
        s.dataIns  = { HC_MAP_PIN("Map"), HC_KEY_PIN("Key") };
        s.dataOuts = { HC_MAP_PIN("Map") };
        break;
    case T::MapContains:
        s.dataIns  = { HC_MAP_PIN("Map"), HC_KEY_PIN("Key") };
        s.dataOuts = { { "Contains", P::Bool } };
        break;
    case T::MapGet:
        // "Default" is a full pin, not an implicit type zero: a miss on a map of
        // scores wants -1 far more often than it wants 0, and the alternative
        // (Contains → Branch → Get) is three nodes for one lookup.
        s.dataIns  = { HC_MAP_PIN("Map"), HC_KEY_PIN("Key"),
                       { "Default", n.propType, false, tn } };
        s.dataOuts = { { "Value", n.propType, false, tn } };
        break;
    case T::MapKeys:
        s.dataIns  = { HC_MAP_PIN("Map") };
        s.dataOuts = { { "Keys", n.keyType, true, ktn } };
        break;
    case T::MapValues:
        s.dataIns  = { HC_MAP_PIN("Map") };
        s.dataOuts = { { "Values", n.propType, true, tn } };
        break;
    case T::ForEachMap:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "Body", P::Exec }, { "Done", P::Exec } };
        s.dataIns  = { HC_MAP_PIN("Map") };
        s.dataOuts = { HC_KEY_PIN("Key"), { "Value", n.propType, false, tn },
                       { "Index", P::Int } };
        break;
    case T::MapFromArrays:
        s.dataIns  = { { "Keys", n.keyType, true, ktn },
                       { "Values", n.propType, true, tn } };
        s.dataOuts = { HC_MAP_PIN("Map") };
        break;
    case T::MapBreak:
        // Exactly Map Keys' and Map Values' output pins, in that order: Break Map
        // IS the two of them on one node, so the pins have to be the same ones or
        // the pair would disagree about what a map comes apart into.
        s.dataIns  = { HC_MAP_PIN("Map") };
        s.dataOuts = { { "Keys", n.keyType, true, ktn },
                       { "Values", n.propType, true, tn } };
        break;
    case T::MapFindByValue:
        // "Found" is a pin and not "an empty Key means no": a map of Strings can
        // legitimately hold "" under some key, so the miss needs its own answer.
        s.dataIns  = { HC_MAP_PIN("Map"), { "Value", n.propType, false, tn } };
        s.dataOuts = { HC_KEY_PIN("Key"), { "Found", P::Bool } };
        break;
    case T::MapRemoveByValue:
        s.dataIns  = { HC_MAP_PIN("Map"), { "Value", n.propType, false, tn } };
        s.dataOuts = { HC_MAP_PIN("Map") };
        break;
#undef HC_MAP_PIN
#undef HC_KEY_PIN
    case T::Delay:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "Completed", P::Exec } };
        // "Real Time" appended, never inserted: Duration stays data-in 0, so
        // every graph saved before this pin existed keeps its wire and its
        // inline default. Unwired and unset it reads false — the old behaviour.
        s.dataIns  = { { "Duration", P::Float }, { "Real Time", P::Bool } };
        break;
    case T::IsValid:
        s.dataIns  = { { "Target", P::Ref } };
        s.dataOuts = { { "Valid", P::Bool } };
        break;
    case T::Cast:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "Success", P::Exec }, { "Failure", P::Exec } };
        s.dataIns  = { { "Object", P::Ref } };
        // The output pin's NAME carries the target class ("As Goblin"), which is
        // why it borrows the node's own string rather than a literal — same
        // lifetime rule as every other borrowed name in here.
        s.dataOuts = { { castOutPinName(n), P::Ref } };
        break;
    case T::DoOnce:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "Then", P::Exec } };
        break;
    case T::FlipFlop:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "A", P::Exec }, { "B", P::Exec } };
        s.dataOuts = { { "Is A", P::Bool } };
        break;
    case T::Add: case T::Subtract: case T::Multiply: case T::Divide:
        s.dataIns  = { { "A", P::Float }, { "B", P::Float } };
        s.dataOuts = { { "", P::Float } };
        break;
    // Vector assembly. 3 and 4 share PinType::Color — see the NodeType comment.
    case T::MakeVector2:
        s.dataIns  = { { "X", P::Float }, { "Y", P::Float } };
        s.dataOuts = { { "", P::Vec2 } };
        break;
    case T::MakeVector3:
        s.dataIns  = { { "X", P::Float }, { "Y", P::Float }, { "Z", P::Float } };
        s.dataOuts = { { "", P::Vec3 } };
        break;
    case T::MakeVector4:
        s.dataIns  = { { "X", P::Float }, { "Y", P::Float }, { "Z", P::Float }, { "W", P::Float } };
        s.dataOuts = { { "", P::Vec4 } };
        break;
    case T::BreakVector2:
        s.dataIns  = { { "", P::Vec2 } };
        s.dataOuts = { { "X", P::Float }, { "Y", P::Float } };
        break;
    case T::BreakVector3:
        s.dataIns  = { { "", P::Vec3 } };
        s.dataOuts = { { "X", P::Float }, { "Y", P::Float }, { "Z", P::Float } };
        break;
    case T::BreakVector4:
        s.dataIns  = { { "", P::Vec4 } };
        s.dataOuts = { { "X", P::Float }, { "Y", P::Float }, { "Z", P::Float }, { "W", P::Float } };
        break;
    case T::Greater: case T::Less: case T::Equals:
        s.dataIns  = { { "A", P::Float }, { "B", P::Float } };
        s.dataOuts = { { "", P::Bool } };
        break;
    case T::And: case T::Or:
        s.dataIns  = { { "A", P::Bool }, { "B", P::Bool } };
        s.dataOuts = { { "", P::Bool } };
        break;
    case T::Not:
        s.dataIns  = { { "", P::Bool } };
        s.dataOuts = { { "", P::Bool } };
        break;
    case T::Concat:
        s.dataIns  = { { "A", P::String }, { "B", P::String } };
        s.dataOuts = { { "", P::String } };
        break;
    case T::ToString:
        s.dataIns  = { { "", P::Float } };
        s.dataOuts = { { "", P::String } };
        break;
    case T::Print:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "", P::Exec } };
        s.dataIns  = { { "", P::String } };
        break;
    case T::MakeStruct:
        // params mirror the struct's fields (synced from the TypeRegistry), so
        // pins resolve without the registry — same idea as EngineCall.
        for (const auto& p : n.params)
            s.dataIns.push_back({ p.name.c_str(), p.type, p.isArray,
                                  defOf(p.typeName) });
        s.dataOuts = { { "Struct", P::Struct, false, tn } };
        break;
    case T::BreakStruct:
        s.dataIns = { { "Struct", P::Struct, false, tn } };
        for (const auto& p : n.params)
            s.dataOuts.push_back({ p.name.c_str(), p.type, p.isArray,
                                   defOf(p.typeName) });
        break;
    case T::GetStructField:
        // params[0] mirrors the chosen field; one output, so a graph reads a
        // single property without breaking the whole struct apart.
        s.dataIns = { { "Struct", P::Struct, false, tn } };
        if (!n.params.empty())
            s.dataOuts.push_back({ n.params[0].name.c_str(), n.params[0].type,
                                   n.params[0].isArray,
                                   defOf(n.params[0].typeName) });
        break;
    case T::SetStructField:
        // params[0] mirrors the chosen field (name + type).
        s.dataIns = { { "Struct", P::Struct, false, tn } };
        if (!n.params.empty())
            s.dataIns.push_back({ n.params[0].name.c_str(), n.params[0].type,
                                  n.params[0].isArray,
                                  defOf(n.params[0].typeName) });
        s.dataOuts = { { "Struct", P::Struct, false, tn } };
        break;
    case T::ConstEnum:
        s.dataOuts = { { "", P::Enum, false, tn } };
        break;
    case T::SwitchOnEnum:
        // params mirror the entry names; the trailing Default catches values no
        // entry claims (or a missing definition).
        s.execIns = { { "", P::Exec } };
        for (const auto& p : n.params) s.execOuts.push_back({ p.name.c_str(), P::Exec });
        s.execOuts.push_back({ "Default", P::Exec });
        s.dataIns = { { "Value", P::Enum, false, tn } };
        break;
    case T::EnumToInt:
        s.dataIns  = { { "", P::Enum, false, tn } };
        s.dataOuts = { { "", P::Int } };
        break;
    case T::IntToEnum:
        s.dataIns  = { { "", P::Int } };
        s.dataOuts = { { "", P::Enum, false, tn } };
        break;
    case T::EnumToString:
        s.dataIns  = { { "", P::Enum, false, tn } };
        s.dataOuts = { { "", P::String } };
        break;
    default: break;
    }
}

// Scratch signature for the counting / single-pin queries below. Its vectors keep
// their capacity across calls, so after the first call signatureInto() only
// assigns and pushes into storage it already owns — zero allocations in the hot
// path (Graph::connect queries it 6× per attempted link, the Runner once per
// executed node, traceBody once per visited node). thread_local because the
// editor and the runtime can query concurrently; every accessor copies what it
// needs out before returning, so callers never alias the scratch.
NodeSig& sigScratch()
{
    static thread_local NodeSig s;
    return s;
}
} // namespace

NodeSig signatureOf(const Node& n)
{
    NodeSig s;
    signatureInto(n, s);
    return s;
}

NodeSigCounts signatureCountsOf(const Node& n)
{
    NodeSig& s = sigScratch();
    signatureInto(n, s);
    return { (int)s.execIns.size(), (int)s.execOuts.size(),
             (int)s.dataIns.size(), (int)s.dataOuts.size() };
}

bool dataPinDescOf(const Node& n, bool input, int index, PinDesc& out)
{
    if (index < 0) return false;
    NodeSig& s = sigScratch();
    signatureInto(n, s);
    const std::vector<PinDesc>& pins = input ? s.dataIns : s.dataOuts;
    if (index >= (int)pins.size()) return false;
    // PinDesc::name points at a string literal or at a string owned by `n` — never
    // at the scratch itself — so the copy stays valid after the next query.
    out = pins[(size_t)index];
    return true;
}

// ╔══════════════════════════════════════════════════════════════════════════╗
// ║  THESE STRINGS ARE AN ON-DISK FORMAT.                                    ║
// ║                                                                          ║
// ║  toJson() writes a node's type as its DISPLAY NAME and fromJson() looks  ║
// ║  it back up by that same string. Renaming an entry below therefore makes ║
// ║  every saved graph containing that node UNREADABLE — the node is silently ║
// ║  dropped on load (fromJson `continue`s on an unknown name) and every link ║
// ║  attached to it goes with it. "Array Add" → "Array Append" already cost   ║
// ║  us exactly that; it survives only through kLegacyNodeNames below.        ║
// ║                                                                          ║
// ║  So: if you rename a node, ADD ITS OLD STRING TO kLegacyNodeNames in the  ║
// ║  same commit. (The proper fix — a stable serialName per type, independent ║
// ║  of the UI label — is a format migration; see nodeTypeFromStoredName.)    ║
// ╚══════════════════════════════════════════════════════════════════════════╝
const char* nodeDisplayName(NodeType t)
{
    switch (t)
    {
        case T::Event:        return "Event";
        case T::FunctionEntry:return "Function";
        case T::FunctionCall: return "Call Function";
        case T::Branch:       return "Branch";
        case T::Sequence:     return "Sequence";
        case T::GetProperty:  return "Get Property";
        case T::SetProperty:  return "Set Property";
        case T::GetVariable:  return "Get Variable";
        case T::SetVariable:  return "Set Variable";
        case T::ShowSelf:   return "Show Self";
        case T::HideSelf:   return "Hide Self";
        case T::CreateWidget: return "Create Widget";
        case T::ShowWidget: return "Show Widget";
        case T::HideWidget: return "Hide Widget";
        case T::DestroyWidget:return "Destroy Widget";
        case T::CreateObject: return "Create Object";
        case T::DestroyObject:return "Destroy Object";
        case T::GetExternal:  return "Get (Ref)";
        case T::SetExternal:  return "Set (Ref)";
        case T::ConstFloat:   return "Float";
        case T::ConstBool:    return "Bool";
        case T::ConstInt:     return "Int";
        case T::ConstString:  return "String";
        case T::ConstVec2:    return "Vec2";
        case T::ConstColor:   return "Color";
        case T::ConstTransform: return "Transform";
        case T::ArrayMake:    return "Make Array";
        case T::ArrayLength:  return "Array Length";
        case T::ArrayGet:     return "Array Get";
        case T::ArrayAdd:     return "Array Append";
        case T::ArraySet:     return "Array Set";
        case T::ArrayInsert:  return "Array Insert";
        case T::ArrayRemove:  return "Array Remove";
        case T::ArrayContains:return "Array Contains";
        case T::ArrayIndexOf: return "Array Index Of";
        case T::ForEach:      return "For Each";
        case T::SetMake:      return "Make Set";
        case T::SetAdd:       return "Set Add";
        case T::SetRemove:    return "Set Remove";
        case T::SetContains:  return "Set Contains";
        case T::SetLength:    return "Set Length";
        case T::SetClear:     return "Set Clear";
        case T::SetToArray:   return "Set To Array";
        case T::ForEachSet:   return "For Each Set";
        case T::MapMake:      return "Make Map";
        case T::MapSet:       return "Map Set";
        case T::MapRemove:    return "Map Remove";
        case T::MapContains:  return "Map Contains";
        case T::MapLength:    return "Map Length";
        case T::MapClear:     return "Map Clear";
        case T::MapGet:       return "Map Get";
        case T::MapKeys:      return "Map Keys";
        case T::MapValues:    return "Map Values";
        case T::ForEachMap:   return "For Each Map";
        case T::SetFromArray: return "Set From Array";
        case T::SetUnion:     return "Set Union";
        case T::SetIntersect: return "Set Intersect";
        case T::SetDifference:return "Set Difference";
        case T::MapFromArrays:return "Make Map From Arrays";
        case T::MapBreak:     return "Break Map";
        case T::MapFindByValue:  return "Map Find By Value";
        case T::MapRemoveByValue:return "Map Remove By Value";
        case T::Delay:        return "Delay";
        case T::IsValid:      return "Is Valid";
        // Deliberately NOT "Cast To <Class>": this string is the node's key on
        // disk (see the boxed warning above), so it must not depend on which
        // class the node happens to target. The editor composes the title.
        case T::Cast:         return "Cast";
        // Fixed, like Cast's: the ACTION is in `s`, and this string is what the
        // node serializes as. The editor titles it "Input: <action>".
        case T::InputAction:  return "Input Action";
        case T::DoOnce:       return "Do Once";
        case T::FlipFlop:     return "Flip Flop";
        case T::Add:          return "Add";
        case T::Subtract:     return "Subtract";
        case T::Multiply:     return "Multiply";
        case T::Divide:       return "Divide";
        case T::Greater:      return "Greater";
        case T::Less:         return "Less";
        case T::Equals:       return "Equals";
        case T::And:          return "And";
        case T::Or:           return "Or";
        case T::Not:          return "Not";
        case T::Concat:       return "Concat";
        case T::ToString:     return "To String";
        case T::BindEvent:      return "Bind Event";
        case T::EmitEvent:      return "Emit Event";
        case T::CallExternal:   return "Call Function (Ref)";
        case T::GetGameInstance:return "Get Game Instance";
        case T::GetSelf:        return "Get Self";
        case T::Print:        return "Print";
        case T::FunctionReturn:return "Return";
        case T::EngineCall:   return "Engine Call";
        case T::MakeStruct:     return "Make Struct";
        case T::BreakStruct:    return "Break Struct";
        case T::GetStructField: return "Get Struct Field";
        case T::SetStructField: return "Set Struct Field";
        case T::ConstEnum:      return "Enum Value";
        case T::SwitchOnEnum:   return "Switch on Enum";
        case T::EnumToInt:      return "Enum to Int";
        case T::IntToEnum:      return "Int to Enum";
        case T::EnumToString:   return "Enum to String";
        case T::MakeVector2:    return "Make Vector 2";
        case T::MakeVector3:    return "Make Vector 3";
        case T::MakeVector4:    return "Make Vector 4";
        case T::BreakVector2:   return "Break Vector 2";
        case T::BreakVector3:   return "Break Vector 3";
        case T::BreakVector4:   return "Break Vector 4";
        default:              return "?";
    }
}

const char* nodeSearchAliases(NodeType t)
{
    // Search-only synonyms — see the declaration for WHY they cannot simply be
    // added to the display name. Lower case and space-separated; a caller
    // matches against these the same way it matches the name. Only nodes whose
    // name is not the word a user would reach for need a row here.
    switch (t)
    {
        // The container ops whose names come from the DATA STRUCTURE rather
        // than from the verb: "Map Set" is an insert, "Set Add" a put, and
        // "Map Get" a lookup. Every one of these names is a key on disk.
        case T::MapSet:       return "add insert put store";
        case T::SetAdd:       return "insert put";
        case T::MapGet:       return "find lookup read at";
        case T::MapContains:  return "has find key";
        case T::SetContains:  return "has find member";
        case T::MapRemove:    return "delete erase";
        case T::SetRemove:    return "delete erase";
        case T::ArrayAdd:     return "append push add";
        // The new arrivals. Union/Intersect/Difference get their symbols and
        // their everyday words: nobody searches for "difference" when they
        // mean "subtract these".
        case T::SetFromArray: return "array to set convert dedupe unique distinct";
        case T::SetUnion:     return "merge combine or plus join";
        case T::SetIntersect: return "common shared and both";
        case T::SetDifference:return "subtract minus without except";
        case T::MapFromArrays:return "zip pair build map from arrays";
        case T::MapBreak:     return "split keys values map to arrays unpack";
        case T::MapFindByValue:   return "search reverse lookup key of value";
        case T::MapRemoveByValue: return "delete erase purge by value";
        // The other two halves of the conversions, so one search finds the pair.
        case T::SetToArray:   return "set to array convert list";
        case T::MapKeys:      return "keys to array";
        case T::MapValues:    return "values to array";
        default: return "";
    }
}

const char* nodeTooltip(NodeType t)
{
    switch (t)
    {
        case T::Event:
            return "Entry point: starts an exec chain when the host fires the named event.\n"
                   "Events with a payload expose it as a data output.";
        case T::FunctionEntry:
            return "Entry of a user-defined function. Its declared inputs appear as data\n"
                   "outputs here; add a Return node to hand results back to the caller.";
        case T::FunctionCall:
            return "Calls a function declared in this graph. Wire its inputs, run it via the\n"
                   "exec pin, and read the declared results from the data outputs.";
        case T::FunctionReturn:
            return "Writes the owning function's results and ends the exec chain.\n"
                   "One data input per declared result.";
        case T::Branch:
            return "If/else: routes execution to True or False depending on the Bool input.";
        case T::Sequence:
            return "Runs its exec outputs in order (Then 0, Then 1, …), one after another.";
        case T::Delay:
            return "Pauses the exec chain and resumes from Completed after Duration seconds.\n"
                   "Retriggering while already pending is ignored.\n"
                   "Real Time counts real seconds instead of game seconds: the wait\n"
                   "ignores Set Time Scale and keeps running while the game is paused.";
        case T::DoOnce:
            return "Lets execution through only the FIRST time it is reached;\n"
                   "every later trigger is swallowed.";
        case T::FlipFlop:
            return "Alternates between its A and B exec outputs on each trigger.\n"
                   "The Is A output tells which side just ran.";
        case T::GetProperty:
            return "Reads a property of the chosen target element; the value type follows\n"
                   "the property. Pure — evaluated whenever the output is used.";
        case T::SetProperty:
            return "Writes the wired value to a property of the chosen target element\n"
                   "when executed.";
        case T::GetVariable:
            return "Reads a graph variable (persistent per running instance).\n"
                   "Pure — evaluated whenever the output is used.";
        case T::SetVariable:
            return "Writes the wired value to a graph variable when executed.";
        case T::ShowSelf:
            return "Makes THIS widget visible (only meaningful inside a widget graph).";
        case T::HideSelf:
            return "Hides THIS widget (only meaningful inside a widget graph).";
        case T::CreateWidget:
            return "Instantiates the chosen widget asset and outputs its Widget id —\n"
                   "feed that into Show/Hide/Destroy Widget.";
        case T::ShowWidget:
            return "Shows the widget identified by the Widget input (from Create Widget).";
        case T::HideWidget:
            return "Hides the widget identified by the Widget input (from Create Widget).";
        case T::DestroyWidget:
            return "Destroys the widget identified by the Widget input; its id becomes invalid.";
        case T::CreateObject:
            return "Instantiates the chosen HorizonCode class as a live object and outputs\n"
                   "a reference to it (its Construct event fires).\n"
                   "Location and Rotation (euler degrees) place the new object; leave a pin\n"
                   "UNWIRED to keep the placement the class was authored with.";
        case T::DestroyObject:
            return "Destroys the object referenced by the input (its Destruct event fires);\n"
                   "the reference becomes invalid.";
        case T::GetExternal:
            return "Reads a PUBLIC variable on the referenced instance.\n"
                   "Target expects an object reference; the output takes the variable's type.";
        case T::SetExternal:
            return "Writes a PUBLIC variable on the referenced instance when executed.\n"
                   "Target expects an object reference.";
        case T::ConstFloat:  return "Float literal. Edit the value on the node body.";
        case T::ConstBool:   return "Bool literal. Edit the value on the node body.";
        case T::ConstInt:    return "Int literal. Edit the value on the node body.";
        case T::ConstString: return "String literal. Edit the text on the node body.";
        case T::ConstVec2:   return "Vec2 literal (x, y). Edit the values on the node body.";
        case T::ConstColor:  return "Color literal (RGBA). Edit the swatch on the node body.";
        case T::ConstTransform:
            return "Transform literal: position, rotation (euler degrees) and scale,\n"
                   "edited on the node body.";
        case T::Add:      return "A + B (Float).";
        case T::Subtract: return "A - B (Float).";
        case T::Multiply: return "A * B (Float).";
        case T::Divide:   return "A / B (Float). Division by zero yields 0.";
        case T::Greater:  return "True when A > B.";
        case T::Less:     return "True when A < B.";
        case T::Equals:   return "True when A equals B.";
        case T::And:      return "True when both A and B are true.";
        case T::Or:       return "True when A or B (or both) is true.";
        case T::Not:      return "Inverts the Bool input.";
        case T::Concat:   return "Joins strings A and B into one string.";
        case T::ToString: return "Converts the input value to its text representation.";
        case T::Print:
            return "Writes the input string to the engine log when executed. Debug aid.";
        case T::BindEvent:
            return "Subscribes: when the Target instance emits the named event, this\n"
                   "instance's matching Event node fires.";
        case T::EmitEvent:
            return "Broadcasts the named event (with an optional payload) to every\n"
                   "instance bound to this one via Bind Event.";
        case T::CallExternal:
            return "Calls a PUBLIC function on the referenced instance. Target expects an\n"
                   "object reference; params and results mirror that function's signature.";
        case T::GetGameInstance:
            return "Outputs a reference to the app-wide GameInstance — global state and\n"
                   "functions that outlive scene changes.";
        case T::GetSelf:
            return "Outputs a reference to this running instance (e.g. to pass to Bind\n"
                   "Event or store in a variable).";
        case T::EngineCall:
            return "Calls an engine API function (transform, input, scene, math, …).\n"
                   "Pins mirror the function's parameters and results.";
        case T::ArrayMake:   return "Outputs a new empty array of the chosen element type.";
        case T::ArrayLength: return "Outputs the number of elements in the input array.";
        case T::ArrayGet:
            return "Outputs the element at Index (0-based). Out-of-range yields the\n"
                   "element type's default value.";
        case T::ArrayAdd:
            return "Outputs a COPY of the input array with Value appended at the end.\n"
                   "The original array is not modified.";
        case T::ArraySet:
            return "Outputs a COPY of the array with the element at Index replaced by Value.";
        case T::ArrayInsert:
            return "Outputs a COPY of the array with Value inserted at Index\n"
                   "(later elements shift right).";
        case T::ArrayRemove:
            return "Outputs a COPY of the array with the element at Index removed.";
        case T::ArrayContains:
            return "True when the array holds an element equal to Value.";
        case T::ArrayIndexOf:
            return "Outputs the index of the first element equal to Value, or -1 if absent.";
        case T::ForEach:
            return "Runs Body once per array element (Element + Index data outputs),\n"
                   "then continues from Done.";
        case T::SetMake:
            return "Outputs a new empty set of the chosen element type. A set holds no\n"
                   "duplicates and iterates in the order elements were first added.";
        case T::SetAdd:
            return "Outputs a COPY of the set with Value added. Adding an element the set\n"
                   "already has changes nothing — it keeps its original position.";
        case T::SetRemove:
            return "Outputs a COPY of the set without Value. The order of the rest is kept.";
        case T::SetContains:
            return "True when the set holds an element equal to Value.";
        case T::SetLength:
            return "Outputs the number of elements in the input set.";
        case T::SetClear:
            return "Outputs an empty set of the same element type.";
        case T::SetToArray:
            return "Outputs the set's elements as an array, in iteration order (the order\n"
                   "they were first added).";
        case T::ForEachSet:
            return "Runs Body once per set element (Element + Index data outputs), in the\n"
                   "order elements were first added, then continues from Done.";
        case T::MapMake:
            return "Outputs a new empty map of the chosen key and value types. Keys may be\n"
                   "Int, String, Enum or Object; the map iterates in insertion order.";
        case T::MapSet:
            return "Outputs a COPY of the map with Key set to Value. A key the map already\n"
                   "has is updated IN PLACE and keeps its position.";
        case T::MapRemove:
            return "Outputs a COPY of the map without Key. The order of the rest is kept.";
        case T::MapContains:
            return "True when the map holds the given Key.";
        case T::MapLength:
            return "Outputs the number of key/value pairs in the input map.";
        case T::MapClear:
            return "Outputs an empty map of the same key and value types.";
        case T::MapGet:
            return "Outputs the value stored under Key, or the Default input when the map\n"
                   "does not hold that key.";
        case T::MapKeys:
            return "Outputs the map's keys as an array, in insertion order.";
        case T::MapValues:
            return "Outputs the map's values as an array, in the SAME order as Map Keys —\n"
                   "index i of one belongs to index i of the other.";
        case T::ForEachMap:
            return "Runs Body once per key/value pair (Key + Value + Index data outputs),\n"
                   "in insertion order, then continues from Done.";
        case T::SetFromArray:
            return "Outputs the array's elements as a set. Duplicates disappear — the FIRST\n"
                   "occurrence in the array decides the position, so the set iterates in the\n"
                   "order the array first mentioned each element.";
        case T::SetUnion:
            return "Outputs a set holding everything in A or in B: A's elements in A's order,\n"
                   "then the elements only B has, in B's order.";
        case T::SetIntersect:
            return "Outputs a set holding only the elements BOTH sets have, in A's order.";
        case T::SetDifference:
            return "Outputs A without the elements B has, in A's order. The one set operation\n"
                   "whose sides are NOT interchangeable — swapping A and B is a different\n"
                   "question.";
        case T::MapFromArrays:
            return "Builds a map by pairing Keys and Values BY INDEX. Different lengths: the\n"
                   "shorter one wins and the surplus is dropped, rather than inventing values.\n"
                   "A key listed twice keeps its FIRST position and takes the LAST value — the\n"
                   "same rule Map Set follows.";
        case T::MapBreak:
            return "Splits a map into its Keys and its Values as two arrays, in insertion\n"
                   "order and index-parallel — index i of one belongs to index i of the other.\n"
                   "Make Map From Arrays puts them back together.";
        case T::MapFindByValue:
            return "Searches the map by VALUE instead of by key: outputs the first key\n"
                   "holding it, in insertion order, and Found = false when no pair does.\n"
                   "Struct values never compare equal, so a map of structs never finds\n"
                   "anything — search on a field you can compare instead.";
        case T::MapRemoveByValue:
            return "Outputs a COPY of the map without EVERY pair holding that value (Map\n"
                   "Remove drops one key; this one drops all matches). The order of the rest\n"
                   "is kept. Struct values never compare equal, so nothing is removed.";
        case T::IsValid:
            return "True when the Target reference points to a live instance — the guard\n"
                   "to run before touching an object that may have been destroyed.";
        case T::Cast:
            return "Checked downcast: continues from Success when Object really is the\n"
                   "chosen class (or derives from it) and from Failure otherwise. The\n"
                   "output reference is only valid on the Success branch.";
        case T::InputAction:
            return "Entered when the chosen Input Action fires: Pressed when it goes down,\n"
                   "Released when it comes up. An AXIS action has neither — it runs once a\n"
                   "frame and hands you its Value instead (a 2D axis hands you both\n"
                   "components at once).\n"
                   "A value from a MOUSE source is a displacement, not a rate: it already\n"
                   "says how far the mouse moved this frame, so do NOT multiply it by delta\n"
                   "time the way you would a key or stick axis — that makes it depend on\n"
                   "the frame rate, and backwards at that.";
        case T::MakeStruct:
            return "Builds a value of the chosen Struct asset: one input per field,\n"
                   "unwired fields fall back to their declared defaults.";
        case T::BreakStruct:
            return "Splits a struct value into its fields — one output per field.";
        case T::GetStructField:
            return "Reads ONE field out of a struct — pick which on the node.\n"
                   "Use Break Struct when you want every field at once.";
        case T::SetStructField:
            return "Outputs a COPY of the struct with the chosen field replaced by Value.\n"
                   "The original struct is not modified.";
        case T::ConstEnum:
            return "Enum literal of the chosen Enum asset. Pick the entry on the node body.";
        case T::SwitchOnEnum:
            return "Routes execution to the exec output matching the enum value;\n"
                   "unmatched values (or a missing definition) take Default.";
        case T::EnumToInt:    return "Outputs the enum value's underlying Int.";
        case T::IntToEnum:    return "Reinterprets an Int as the chosen Enum asset's value.";
        case T::EnumToString: return "Outputs the NAME of the enum entry matching the value\n"
                   "(empty when no entry matches).";
        case T::MakeVector2:  return "Builds a Vec2 from two floats.";
        case T::MakeVector3:  return "Builds a Vec3 from three floats — the type every engine\n"
                   "node that takes a position, velocity or direction uses.";
        case T::MakeVector4:  return "Builds a Vec4 from four floats. For a colour with alpha,\n"
                   "use a Color instead — Vec4 and Color convert either way.";
        case T::BreakVector2: return "Splits a Vec2 into X and Y.";
        case T::BreakVector3: return "Splits a Vec3 into X, Y and Z.";
        case T::BreakVector4: return "Splits a Vec4 into X, Y, Z and W.";
        default: return "";
    }
}

std::vector<std::string> inputActionEventNames(const Node& n)
{
    if (n.type != T::InputAction || n.s.empty()) return {};
    if (!n.hasArg) return { HE::inputEventPressed(n.s), HE::inputEventReleased(n.s) };
    // A two-dimensional axis answers to its OWN name, never to ".Axis" — see
    // inputEventAxis2D for why a Vec2 must not arrive where a Float is read.
    return { n.propType == P::Vec2 ? HE::inputEventAxis2D(n.s) : HE::inputEventAxis(n.s) };
}

int inputActionChainFor(const Node& n, const std::string& eventName)
{
    const std::vector<std::string> names = inputActionEventNames(n);
    for (size_t i = 0; i < names.size(); ++i)
        if (names[i] == eventName) return (int)i;
    return -1;
}

const char* nodeCategory(NodeType t)
{
    switch (t)
    {
        case T::Event:         return "Events";
        case T::InputAction:   return "Input";
        case T::FunctionEntry: return "Functions";
        case T::FunctionCall:  return "Functions";
        case T::FunctionReturn:return "Functions";
        case T::Branch:
        case T::Sequence:
        case T::Delay:
        case T::DoOnce:
        case T::FlipFlop:      return "Flow";
        case T::IsValid:
        case T::Cast:          return "Reference";
        case T::GetProperty:
        case T::SetProperty:   return "Property";
        case T::GetVariable:
        case T::SetVariable:   return "Variables";
        case T::ShowSelf:
        case T::HideSelf:    return "Widget";
        case T::CreateWidget:
        case T::ShowWidget:
        case T::HideWidget:
        case T::DestroyWidget: return "UI";
        case T::ConstFloat: case T::ConstBool: case T::ConstInt:
        case T::ConstString: case T::ConstVec2: case T::ConstColor:
        case T::ConstTransform: return "Literals";
        case T::Add: case T::Subtract: case T::Multiply: case T::Divide:
        case T::Greater: case T::Less: case T::Equals: return "Math";
        // "Math" and not a new "Vector" category on purpose: the category list
        // is HOST data (LevelScriptPanel/UIEditorPanel each carry their own),
        // so a new one has to be added to every host or the nodes are invisible
        // in that editor. Math is already in all of them.
        case T::MakeVector2: case T::MakeVector3: case T::MakeVector4:
        case T::BreakVector2: case T::BreakVector3: case T::BreakVector4: return "Math";
        case T::And: case T::Or: case T::Not: return "Logic";
        case T::Concat: case T::ToString: return "String";
        case T::BindEvent:
        case T::EmitEvent:      return "Events";
        case T::CallExternal:
        case T::GetGameInstance:
        case T::GetSelf:
        case T::CreateObject:
        case T::DestroyObject:
        case T::GetExternal:
        case T::SetExternal:    return "Reference";
        case T::Print: return "Debug";
        case T::EngineCall: return "Engine";
        case T::ArrayMake: case T::ArrayLength: case T::ArrayGet: case T::ArrayAdd:
        case T::ArraySet: case T::ArrayInsert: case T::ArrayRemove:
        case T::ArrayContains: case T::ArrayIndexOf: case T::ForEach: return "Array";
        case T::SetMake: case T::SetAdd: case T::SetRemove: case T::SetContains:
        case T::SetLength: case T::SetClear: case T::SetToArray:
        case T::ForEachSet:
        // Set From Array lives with the SETS and not with the arrays: it is the
        // node that PRODUCES one, which is what a reader is looking for.
        case T::SetFromArray: case T::SetUnion: case T::SetIntersect:
        case T::SetDifference: return "Set";
        case T::MapMake: case T::MapSet: case T::MapRemove: case T::MapContains:
        case T::MapLength: case T::MapClear: case T::MapGet: case T::MapKeys:
        case T::MapValues: case T::ForEachMap:
        case T::MapFromArrays: case T::MapBreak:
        case T::MapFindByValue: case T::MapRemoveByValue: return "Map";
        case T::MakeStruct: case T::BreakStruct:
        case T::GetStructField: case T::SetStructField: return "Structs";
        case T::ConstEnum: case T::SwitchOnEnum:
        case T::EnumToInt: case T::IntToEnum: case T::EnumToString: return "Enums";
        default: return "Misc";
    }
}

const std::vector<NodeType>& nodeRegistry()
{
    static const std::vector<NodeType> kAll = []
    {
        std::vector<NodeType> v;
        for (int i = 0; i < (int)T::COUNT; ++i) v.push_back((NodeType)i);
        return v;
    }();
    return kAll;
}

// ── Pin ranges (unified index space: [execIns][execOuts][dataIns][dataOuts]) ──

namespace
{
struct PinRanges { int execIn0, execOut0, dataIn0, dataOut0, end; };
// All three go through the allocation-free accessors (see HorizonCode.h): they
// are the interpreter's and the editor's per-node hot path.
PinRanges pinRanges(const Node& n)
{
    const NodeSigCounts s = signatureCountsOf(n);
    PinRanges r;
    r.execIn0  = 0;
    r.execOut0 = r.execIn0  + s.execIns;
    r.dataIn0  = r.execOut0 + s.execOuts;
    r.dataOut0 = r.dataIn0  + s.dataIns;
    r.end      = r.dataOut0 + s.dataOuts;
    return r;
}
PinType dataPinType(const Node& n, bool input, int index)
{
    PinDesc d;
    return dataPinDescOf(n, input, index, d) ? d.type : P::Float;
}
bool dataPinIsArray(const Node& n, bool input, int index)
{
    PinDesc d;
    return dataPinDescOf(n, input, index, d) && d.isArray;
}
} // namespace

// ── Graph container ──────────────────────────────────────────────────────────

Node*       Graph::findNode(int id)       { return HE::graph::findNodeById(nodes, id); }
const Node* Graph::findNode(int id) const { return HE::graph::findNodeById(nodes, id); }

int Graph::addNode(Node n)
{
    return HE::graph::appendNode(nodes, nextId, std::move(n));
}

void Graph::removeNode(int id)
{
    // A deleted function takes its local variables AND its body with it. The
    // body nodes live with subgraph == the entry's id; once the entry is gone
    // no tab can show them and no entry point can reach them — without this
    // they would sit invisibly in the file forever.
    if (const Node* n = findNode(id); n && n->type == NodeType::FunctionEntry)
    {
        variables.erase(std::remove_if(variables.begin(), variables.end(),
            [&](const Variable& v){ return v.scope == id; }), variables.end());
        std::vector<int> body;
        for (const Node& bn : nodes)
            if (bn.subgraph == id && bn.id != id) body.push_back(bn.id);
        for (const int bid : body)
            HE::graph::removeNodeAndLinks(nodes, links, bid);
    }
    HE::graph::removeNodeAndLinks(nodes, links, id);
}

Variable*       Graph::findVariable(const std::string& name)
{ for (auto& v : variables) if (v.name == name) return &v; return nullptr; }
const Variable* Graph::findVariable(const std::string& name) const
{ for (const auto& v : variables) if (v.name == name) return &v; return nullptr; }

const Variable* Graph::findVariableOrInherited(const std::string& name) const
{
    // Own declaration first — a class's own list is the nearer one, the same way
    // the runtime searches its levels leaf-first.
    if (const Variable* v = findVariable(name)) return v;
    for (const auto& v : inherited) if (v.name == name) return &v;
    return nullptr;
}

Value variableDefaultValue(const Variable& v)
{
    if (v.isArray)
    {
        // Seed from the editor-authored slots (each already a scalar of v.type).
        Value r; r.isArray = true; r.container = v.kind(); r.type = v.type;
        r.items = v.defaultItems;
        for (Value& it : r.items) { it.isArray = false; it.container = CK::None; it.type = v.type; }
        if (r.container == CK::Map)
        {
            r.keyType = v.keyType; r.keyTypeName = v.keyTypeName;
            r.keys = v.defaultKeys;
            for (Value& k : r.keys) { k.isArray = false; k.container = CK::None; k.type = v.keyType; }
            // A hand-edited asset could carry lists of different lengths, and a
            // map whose keys and values disagree is not a map at all — truncate
            // to the pairs that exist rather than reading past one of them.
            const size_t n = std::min(r.keys.size(), r.items.size());
            r.keys.resize(n); r.items.resize(n);
            dedupeMapKeys(r);
        }
        else if (r.container == CK::Set)
        {
            dedupeSetItems(r);
        }
        return r;
    }
    switch (v.type)
    {
        case P::Float:  return Value::ofFloat(v.f[0]);
        case P::Bool:   return Value::ofBool(v.f[0] != 0.0f);
        case P::Int:    return Value::ofInt((int)v.f[0]);
        case P::String: return Value::ofString(v.s);
        case P::Vec2:   return Value::ofVec2({ v.f[0], v.f[1] });
        case P::Color:  return Value::ofColor({ v.f[0], v.f[1], v.f[2], v.f[3] });
        case P::Vec3:   return Value::ofVec3({ v.f[0], v.f[1], v.f[2] });
        case P::Vec4:   return Value::ofVec4({ v.f[0], v.f[1], v.f[2], v.f[3] });
        case P::Ref:    return Value::ofRef(0);
        case P::Transform: return Value::ofTransform(v.tpos, v.trot, v.tscl);
        case P::Enum:
        {
            // The stored default is the entry NAME in `s` (renumber-safe);
            // resolve against the current definition, first entry as fallback.
            Value r; r.type = P::Enum; r.typeName = v.typeName;
            HE::EnumDef def;
            if (HE::TypeRegistry::instance().getEnum(v.typeName, def))
            {
                if (const HE::EnumEntry* e = def.findEntry(v.s)) r.i = e->value;
                else if (!def.entries.empty())                   r.i = def.entries.front().value;
            }
            return r;
        }
        case P::Struct:
        {
            // The definition's own field defaults, then this graph's overrides
            // on top — matched BY NAME against the current definition.
            Value r = HE::TypeRegistry::instance().makeDefaultValue(v.typeName);
            if (!v.structDefaults.empty())
            {
                HE::StructDef def;
                if (HE::TypeRegistry::instance().getStruct(v.typeName, def))
                    for (size_t i = 0; i < def.fields.size() && i < r.items.size(); ++i)
                        if (auto it = v.structDefaults.find(def.fields[i].name);
                            it != v.structDefaults.end())
                        {
                            Value ov = it->second;
                            if (def.fields[i].type == P::Enum)
                            {
                                // Stored as the entry NAME (renumber-safe).
                                HE::EnumDef ed;
                                ov.type = P::Enum; ov.typeName = def.fields[i].typeName;
                                if (HE::TypeRegistry::instance().getEnum(ov.typeName, ed))
                                {
                                    if (const HE::EnumEntry* en = ed.findEntry(it->second.s)) ov.i = en->value;
                                    else continue;   // stale entry name → keep the definition default
                                }
                            }
                            else
                            {
                                ov.type = def.fields[i].type;
                            }
                            r.items[i] = std::move(ov);
                        }
            }
            return r;
        }
        default:        return Value::ofFloat(v.f[0]);
    }
}

std::vector<int> duplicateNodes(Graph& g, const std::vector<int>& ids, float dx, float dy)
{
    std::vector<int> fresh;
    std::unordered_map<int, int> remap;   // old id → clone id
    for (int id : ids)
    {
        const Node* src = g.findNode(id);
        if (!src) continue;
        // Handler/function names must stay unique per graph — don't clone those.
        if (src->type == NodeType::Event || src->type == NodeType::FunctionEntry) continue;
        Node copy = *src;                  // params/results/subgraph/payloads ride along
        copy.x += dx; copy.y += dy;
        const int nid = g.addNode(std::move(copy));
        remap[id] = nid;
        fresh.push_back(nid);
    }
    // Re-create the links INSIDE the duplicated set (external wires stay on the
    // originals — a duplicate shouldn't steal or share the source's inputs).
    std::vector<Link> cloned;
    for (const Link& l : g.links)
    {
        auto s = remap.find(l.srcNode), d = remap.find(l.dstNode);
        if (s != remap.end() && d != remap.end())
            cloned.push_back({ s->second, l.srcPin, d->second, l.dstPin });
    }
    for (const Link& l : cloned) g.links.push_back(l);
    return fresh;
}

void adoptForEachElementType(Graph& g, int srcNode, int srcPin, int dstNode, int dstPin)
{
    Node* dst = g.findNode(dstNode);
    const Node* src = g.findNode(srcNode);
    // The three loop nodes share this: each is generic until its container input
    // is wired, and each has that input as its FIRST data-in. ForEach Map
    // additionally adopts the key type — its Key output is as generic as the
    // Value one, and leaving it at the default String would silently mistype
    // every wire off it.
    if (!dst || !src ||
        (dst->type != NodeType::ForEach && dst->type != NodeType::ForEachSet &&
         dst->type != NodeType::ForEachMap))
        return;
    // Unified pins of all three: execIn 0, Body 1, Done 2, container-in 3.
    if (dstPin != 3) return;
    PinDesc sd;
    if (!dataPinDescOf(*src, /*input=*/false, srcPin - pinRanges(*src).dataOut0, sd) ||
        !sd.isArray)
        return;
    // Only a container of the loop's OWN kind teaches it anything — a set does
    // not tell a For Each Map what its keys are.
    const ContainerKind want = dst->type == NodeType::ForEach    ? ContainerKind::Array
                             : dst->type == NodeType::ForEachSet ? ContainerKind::Set
                                                                 : ContainerKind::Map;
    if (sd.kind() != want) return;
    if (want == ContainerKind::Map &&
        (sd.keyType != dst->keyType ||
         (sd.keyTypeName && *sd.keyTypeName && dst->keyTypeName != sd.keyTypeName)))
    {
        dst->keyType     = sd.keyType;
        dst->keyTypeName = sd.keyTypeName ? sd.keyTypeName : "";
        // The Key output (data-out 0) changed type — its wires no longer typecheck.
        g.links.erase(std::remove_if(g.links.begin(), g.links.end(),
            [&](const Link& l){ return l.srcNode == dstNode && l.srcPin == 4; }),
            g.links.end());
    }

    const PinType elem = sd.type;
    const std::string elemDef = sd.typeName ? sd.typeName : "";
    if (elem != dst->propType || (!elemDef.empty() && elemDef != dst->typeName))
    {
        dst->propType = elem;
        // Enum/Struct elements: carry the DEFINITION along, not just the kind —
        // without it the Element pin is a user-defined type nobody can resolve
        // (no field pins, "definition missing" in every panel that looks it up).
        if (elem == PinType::Enum || elem == PinType::Struct) dst->typeName = elemDef;
        else                                                  dst->typeName.clear();
        // The Element output changed type: drop its links (they no longer
        // typecheck). It is data-out 0 (pin 4) on the array/set loops and
        // data-out 1 (pin 5) on the map one, whose data-out 0 is the Key.
        const int elemPin = want == ContainerKind::Map ? 5 : 4;
        g.links.erase(std::remove_if(g.links.begin(), g.links.end(),
            [&](const Link& l){ return l.srcNode == dstNode && l.srcPin == elemPin; }),
            g.links.end());
    }
    // Object arrays: carry the element class along so the Element pin offers the
    // class's members. GetVariable/SetVariable → the variable's class; the array
    // ops keep their element class path in s.
    if (elem == PinType::Ref)
    {
        std::string cls;
        if (src->type == NodeType::GetVariable || src->type == NodeType::SetVariable)
        { if (const Variable* v = g.findVariable(src->s)) cls = v->className; }
        else if (src->type != NodeType::ForEach && src->type != NodeType::ForEachSet &&
                 src->type != NodeType::ForEachMap)
            cls = src->s;
        if (!cls.empty()) dst->s = cls;
    }
}

bool Graph::connect(int srcNode, int srcPin, int dstNode, int dstPin)
{
    const Node* s = findNode(srcNode);
    const Node* d = findNode(dstNode);
    if (!s || !d || srcNode == dstNode) return false;

    const PinRanges sr = pinRanges(*s);
    const PinRanges dr = pinRanges(*d);

    const bool srcIsExecOut = srcPin >= sr.execOut0 && srcPin < sr.dataIn0;
    const bool srcIsDataOut = srcPin >= sr.dataOut0 && srcPin < sr.end;
    const bool dstIsExecIn  = dstPin >= dr.execIn0  && dstPin < dr.execOut0;
    const bool dstIsDataIn  = dstPin >= dr.dataIn0  && dstPin < dr.dataOut0;

    if (srcIsExecOut && dstIsExecIn)
    {
        // An exec OUTPUT is a single "what runs next" pointer, so it is the SOURCE
        // side that gets replaced here — the one place HorizonCode's link semantics
        // differ from Material/ParticleGraph (which have no exec pins at all).
        HE::graph::disconnectOutput(links, srcNode, srcPin);
        links.push_back({ srcNode, srcPin, dstNode, dstPin });
        return true;
    }
    if (srcIsDataOut && dstIsDataIn)
    {
        const int si = srcPin - sr.dataOut0, di = dstPin - dr.dataIn0;
        // Elementary types convert on the wire: the reader coerces to ITS pin
        // type (Runner::evalInput), and generated C++ emits the cast — so a
        // Float output feeding an Int input needs no node in between.
        if (!canConvertPinType(dataPinType(*s, false, si), dataPinType(*d, true, di)) ||
            dataPinIsArray(*s, false, si) != dataPinIsArray(*d, true, di)) // container ≠ scalar
            return false;
        // Containers of different KINDS never join: an array is ordered and
        // indexable, a set deduplicates, a map is keyed — and coerce passes a
        // container through untouched, so an accepted wire would be a
        // reinterpretation rather than a conversion. A map additionally has to
        // agree on the KEY (and, for enum keys, on its definition).
        {
            PinDesc sd{}, dd{};
            dataPinDescOf(*s, false, si, sd);
            dataPinDescOf(*d, true,  di, dd);
            if (sd.kind() != dd.kind()) return false;
            // …and neither do containers of different ELEMENT types, for exactly
            // the same reason one line up: the convertibility test above is about
            // a value coerce() will convert on arrival, and coerce leaves a
            // container alone. An accepted Array<Float> → Array<Int> wire would
            // hand the reader elements still tagged Float while every comparison
            // it makes runs on its own pin type — scalarValueEquals(Int) reads
            // a.i, a Float value's i is 0, so every element looks equal to every
            // other and a five-element array dedupes to one. A refused wire is a
            // question the author can answer; that one is a silent wrong answer.
            if (sd.kind() != ContainerKind::None && sd.type != dd.type) return false;
            if (sd.kind() == ContainerKind::Map)
            {
                if (sd.keyType != dd.keyType) return false;
                const std::string_view ka = sd.keyTypeName ? sd.keyTypeName : "";
                const std::string_view kb = dd.keyTypeName ? dd.keyTypeName : "";
                if (!ka.empty() && !kb.empty() && ka != kb) return false;
            }
        }
        // User-defined types connect only to the SAME definition: a Struct pin
        // for PlayerStats must not accept an Inventory, and enums likewise. An
        // EMPTY typeName is the generic boundary (e.g. the save.setStruct
        // engine call, whose registry params carry no definition) — it accepts
        // anything, like an untyped Object reference; the callee validates.
        {
            const PinType pt = dataPinType(*s, false, si);
            if (pt == P::Enum || pt == P::Struct)
            {
                PinDesc sd{}, dd{};
                dataPinDescOf(*s, false, si, sd);
                dataPinDescOf(*d, true,  di, dd);
                const std::string_view a = sd.typeName ? sd.typeName : "";
                const std::string_view b = dd.typeName ? dd.typeName : "";
                if (!a.empty() && !b.empty() && a != b) return false;
            }
        }
        HE::graph::disconnectInput(links, dstNode, dstPin); // an input holds one link
        links.push_back({ srcNode, srcPin, dstNode, dstPin });
        return true;
    }
    return false;
}

// ── JSON ─────────────────────────────────────────────────────────────────────

// ── Scalar Value ⇄ JSON (array-default slots) ────────────────────────────────
namespace
{
nlohmann::json scalarValueToJson(const Value& v, PinType t)
{
    switch (t)
    {
        case P::Float:  return v.f;
        case P::Bool:   return v.b;
        case P::Int:    return v.i;
        case P::String: return v.s;
        case P::Vec2:   return nlohmann::json::array({ v.v2.x, v.v2.y });
        case P::Color:  return nlohmann::json::array({ v.col.x, v.col.y, v.col.z, v.col.w });
        case P::Vec3:   return nlohmann::json::array({ v.v3.x, v.v3.y, v.v3.z });
        case P::Vec4:   return nlohmann::json::array({ v.v4.x, v.v4.y, v.v4.z, v.v4.w });
        case P::Ref:    return v.ref;
        case P::Enum:   return v.s;   // the entry NAME (renumber-safe)
        case P::Struct:
        {
            // Name-keyed field object, like everything else about user types.
            nlohmann::json o = nlohmann::json::object();
            HE::StructDef def;
            if (HE::TypeRegistry::instance().getStruct(v.typeName, def))
                for (size_t i = 0; i < def.fields.size() && i < v.items.size(); ++i)
                    o[def.fields[i].name] = scalarValueToJson(v.items[i], def.fields[i].type);
            return o;
        }
        case P::Transform:
            return nlohmann::json::array({ v.tpos.x, v.tpos.y, v.tpos.z,
                                           v.trot.x, v.trot.y, v.trot.z,
                                           v.tscl.x, v.tscl.y, v.tscl.z });
        default:        return v.f;
    }
}
Value scalarValueFromJson(const nlohmann::json& j, PinType t)
{
    Value v; v.type = t;
    switch (t)
    {
        case P::Float:  if (j.is_number()) v.f = j.get<float>(); break;
        case P::Bool:   if (j.is_boolean()) v.b = j.get<bool>(); break;
        case P::Int:    if (j.is_number()) v.i = j.get<int>(); break;
        case P::String: if (j.is_string()) v.s = j.get<std::string>(); break;
        case P::Vec2:   if (j.is_array() && j.size() >= 2)
                            v.v2 = { j[0].get<float>(), j[1].get<float>() }; break;
        case P::Color:  if (j.is_array() && j.size() >= 4)
                            v.col = { j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>() }; break;
        case P::Vec3:   if (j.is_array() && j.size() >= 3)
                            v.v3 = { j[0].get<float>(), j[1].get<float>(), j[2].get<float>() }; break;
        case P::Vec4:   if (j.is_array() && j.size() >= 4)
                            v.v4 = { j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>() }; break;
        case P::Ref:    if (j.is_number()) v.ref = j.get<uint32_t>(); break;
        case P::Enum:   if (j.is_string()) v.s = j.get<std::string>(); break;  // entry NAME
        case P::Transform:
            if (j.is_array() && j.size() >= 9)
            {
                v.tpos = { j[0].get<float>(), j[1].get<float>(), j[2].get<float>() };
                v.trot = { j[3].get<float>(), j[4].get<float>(), j[5].get<float>() };
                v.tscl = { j[6].get<float>(), j[7].get<float>(), j[8].get<float>() };
            }
            break;
        default: break;
    }
    return v;
}

// ── Stored node type name → NodeType ─────────────────────────────────────────
// Nodes serialize by DISPLAY NAME (see the banner at nodeDisplayName), so every
// rename of a node label leaves saved graphs referring to the OLD string. This
// table is where those old strings keep working — data-driven, one line per
// rename, forever. Never delete an entry: the graph that still uses it may be in
// a user's project, not in this repo.
//
// FOLLOW-UP (deliberately not done here): give every NodeType a stable
// serialName decoupled from the UI label and write THAT, keeping this table as
// the read-side alias map for everything written so far. That flips what
// toJson() emits, so it needs a format version on the graph and a shipped-editor
// compatibility story — too big to slip into a refactor. Until then the rule
// above ("rename ⇒ add an alias in the same commit") is the whole contract.
struct LegacyNodeName { const char* stored; NodeType type; };
constexpr LegacyNodeName kLegacyNodeNames[] = {
    { "Array Add", NodeType::ArrayAdd },   // renamed to "Array Append"
};

bool nodeTypeFromStoredName(const std::string& stored, NodeType& out)
{
    for (NodeType t : nodeRegistry())
        if (stored == nodeDisplayName(t)) { out = t; return true; }
    for (const LegacyNodeName& a : kLegacyNodeNames)
        if (stored == a.stored) { out = a.type; return true; }
    return false;
}
// ── Item-level JSON ─────────────────────────────────────────────────────────
// One node / variable as the object that goes into the document's array. The
// document serializers below are written on top of these, so the per-item form
// collaboration sends over the wire and the on-disk form are the SAME bytes by
// construction — a second, parallel serializer would drift the first time a
// field was added to only one of them.
nlohmann::json nodeToJsonObj(const Node& n)
{
    nlohmann::json e = {
        { "id",   n.id },
        { "type", nodeDisplayName(n.type) }, // by name → schema-evolution safe
        { "pos",  { n.x, n.y } },
    };
    if (n.elem)              e["elem"]     = n.elem;
    if (!n.s.empty())        e["s"]        = n.s;
    if (n.propType != P::Float) e["propType"] = (int)n.propType;
    if (n.hasArg)            e["hasArg"]   = n.hasArg;
    if (n.access)            e["access"]   = n.access;
    if (n.overridable)       e["virtual"]  = true;
    if (n.f[0] || n.f[1] || n.f[2] || n.f[3])
        e["f"] = { n.f[0], n.f[1], n.f[2], n.f[3] };
    if (n.type == NodeType::ConstTransform)
        e["xform"] = { n.tpos.x, n.tpos.y, n.tpos.z, n.trot.x, n.trot.y, n.trot.z,
                       n.tscl.x, n.tscl.y, n.tscl.z };
    auto dumpParams = [](const std::vector<FuncParam>& ps)
    {
        nlohmann::json a = nlohmann::json::array();
        for (const auto& p : ps)
        {
            nlohmann::json pe = { { "name", p.name }, { "type", (int)p.type } };
            if (p.isArray) pe["arr"] = true;
            if (!p.typeName.empty()) pe["typeName"] = p.typeName;
            // "ctr" is written only for Set/Map — an Array param keeps exactly
            // the bytes it had before containers existed.
            if (p.container != ContainerKind::None && p.container != ContainerKind::Array)
            {
                pe["ctr"] = (int)p.container;
                if (p.container == ContainerKind::Map)
                {
                    pe["keyType"] = (int)p.keyType;
                    if (!p.keyTypeName.empty()) pe["keyTypeName"] = p.keyTypeName;
                }
            }
            a.push_back(std::move(pe));
        }
        return a;
    };
    if (!n.params.empty())  e["params"]  = dumpParams(n.params);
    if (!n.results.empty()) e["results"] = dumpParams(n.results);
    if (n.subgraph)         e["subgraph"] = n.subgraph;
    if (n.isArray)          e["arr"]     = true;
    if (n.container != ContainerKind::None && n.container != ContainerKind::Array)
    {
        e["ctr"] = (int)n.container;
        if (n.container == ContainerKind::Map)
        {
            e["keyType"] = (int)n.keyType;
            if (!n.keyTypeName.empty()) e["keyTypeName"] = n.keyTypeName;
        }
    }
    if (!n.typeName.empty()) e["typeName"] = n.typeName;
    // Written under the same key a variable uses for the same thing, and omitted
    // when empty: an older editor reading this file simply ignores it, and a
    // graph that never picked a target class stays byte-identical.
    if (!n.className.empty()) e["className"] = n.className;
    if (!n.pinDefaults.empty())
    {
        nlohmann::json pd = nlohmann::json::array();
        for (const auto& [idx, val] : n.pinDefaults)
            pd.push_back({ { "i", idx }, { "t", (int)val.type },
                           { "v", scalarValueToJson(val, val.type) } });
        e["pinDefaults"] = std::move(pd);
    }
return e;
}

nlohmann::json variableToJsonObj(const Variable& v)
{
    nlohmann::json e = { { "name", v.name }, { "type", (int)v.type } };
    if (v.f[0] || v.f[1] || v.f[2] || v.f[3]) e["f"] = { v.f[0], v.f[1], v.f[2], v.f[3] };
    if (!v.s.empty()) e["s"] = v.s;
    if (v.access)     e["access"] = v.access;
    if (v.scope)      e["scope"] = v.scope;   // function-local (FunctionEntry id)
    if (v.isArray)    e["arr"] = true;
    if (v.isArray && !v.defaultItems.empty())
    {
        nlohmann::json items = nlohmann::json::array();
        for (const Value& it : v.defaultItems) items.push_back(scalarValueToJson(it, v.type));
        e["items"] = std::move(items);
    }
    if (v.container != ContainerKind::None && v.container != ContainerKind::Array)
    {
        e["ctr"] = (int)v.container;
        if (v.container == ContainerKind::Map)
        {
            e["keyType"] = (int)v.keyType;
            if (!v.keyTypeName.empty()) e["keyTypeName"] = v.keyTypeName;
            // Parallel to "items", NEVER a JSON object: nlohmann's object is a
            // sorted std::map, so an object would silently alphabetize the keys
            // and the persisted order would stop being the insertion order the
            // whole feature is built on. Keys are also not all strings.
            if (!v.defaultKeys.empty())
            {
                nlohmann::json keys = nlohmann::json::array();
                for (const Value& k : v.defaultKeys) keys.push_back(scalarValueToJson(k, v.keyType));
                e["keys"] = std::move(keys);
            }
        }
    }
    if (v.type == PinType::Transform)
        e["xform"] = { v.tpos.x, v.tpos.y, v.tpos.z, v.trot.x, v.trot.y, v.trot.z,
                       v.tscl.x, v.tscl.y, v.tscl.z };
    if (!v.className.empty()) e["className"] = v.className;
    if (!v.typeName.empty())  e["typeName"]  = v.typeName;
    if (!v.structDefaults.empty())
    {
        // Name-keyed, and each value tagged with its own type so a definition
        // edit can't reinterpret what was authored here.
        nlohmann::json sd = nlohmann::json::object();
        for (const auto& [field, val] : v.structDefaults)
            sd[field] = { { "t", (int)val.type }, { "v", scalarValueToJson(val, val.type) } };
        e["structDefaults"] = std::move(sd);
    }
return e;
}

// False = the object does not describe a node we know (unknown/removed type),
// which the document loader skips and the delta layer treats as "ignore".
bool nodeFromJsonObj(const nlohmann::json& e, Node& n)
{
    n.id = e.value("id", 0);
    if (!nodeTypeFromStoredName(e.value("type", std::string()), n.type)) return false;
    if (const auto& p = e.value("pos", nlohmann::json::array()); p.size() >= 2)
    { n.x = p[0].get<float>(); n.y = p[1].get<float>(); }
    n.elem     = e.value("elem", 0);
    n.s        = e.value("s", std::string());
    n.propType = (PinType)e.value("propType", (int)P::Float);
    n.hasArg   = e.value("hasArg", false);
    n.access   = e.value("access", 0);
    // Absent means "not overridable" — every graph authored before this
    // existed keeps exactly the behaviour it had.
    n.overridable = e.value("virtual", false);
    if (const auto& f = e.value("f", nlohmann::json::array()); f.size() >= 4)
        for (int i = 0; i < 4; ++i) n.f[i] = f[i].get<float>();
    if (const auto& x = e.value("xform", nlohmann::json::array()); x.size() >= 9)
    {
        n.tpos = { x[0].get<float>(), x[1].get<float>(), x[2].get<float>() };
        n.trot = { x[3].get<float>(), x[4].get<float>(), x[5].get<float>() };
        n.tscl = { x[6].get<float>(), x[7].get<float>(), x[8].get<float>() };
    }
    auto loadParams = [](const nlohmann::json& a, std::vector<FuncParam>& ps)
    {
        for (const auto& pe : a)
        {
            FuncParam p;
            p.name = pe.value("name", std::string());
            p.type = (PinType)pe.value("type", (int)P::Float);
            p.isArray = pe.value("arr", false);
            p.typeName = pe.value("typeName", std::string());
            p.container = (ContainerKind)pe.value("ctr", (int)ContainerKind::None);
            // A kind present means container, full stop: the two-field state
            // "Set but not a container" must not survive a load.
            if (p.container != ContainerKind::None) p.isArray = true;
            p.keyType = (PinType)pe.value("keyType", (int)P::String);
            p.keyTypeName = pe.value("keyTypeName", std::string());
            ps.push_back(std::move(p));
        }
    };
    loadParams(e.value("params",  nlohmann::json::array()), n.params);
    loadParams(e.value("results", nlohmann::json::array()), n.results);
    n.subgraph = e.value("subgraph", 0);
    n.isArray  = e.value("arr", false);
    n.container = (ContainerKind)e.value("ctr", (int)ContainerKind::None);
    if (n.container != ContainerKind::None) n.isArray = true;   // see loadParams
    n.keyType = (PinType)e.value("keyType", (int)P::String);
    n.keyTypeName = e.value("keyTypeName", std::string());
    n.typeName = e.value("typeName", std::string());
    n.className = e.value("className", std::string());
    if (const auto& pd = e.value("pinDefaults", nlohmann::json::array()); pd.is_array())
        for (const auto& entry : pd)
        {
            if (!entry.is_object()) continue;
            const int idx = entry.value("i", -1);
            if (idx < 0) continue;
            const PinType t = (PinType)entry.value("t", (int)P::Float);
            n.pinDefaults[idx] = scalarValueFromJson(entry.value("v", nlohmann::json()), t);
        }
return true;
}

// False = no usable variable in this object (a variable is keyed by its name,
// so a nameless one has no identity).
bool variableFromJsonObj(const nlohmann::json& e, Variable& v)
{
    v.name = e.value("name", std::string());
    if (v.name.empty()) return false;
    v.type = (PinType)e.value("type", (int)P::Float);
    v.s    = e.value("s", std::string());
    v.access = e.value("access", 0);
    v.scope  = e.value("scope", 0);
    v.isArray = e.value("arr", false);
    v.container = (ContainerKind)e.value("ctr", (int)ContainerKind::None);
    if (v.container != ContainerKind::None) v.isArray = true;   // see loadParams
    v.keyType = (PinType)e.value("keyType", (int)P::String);
    v.keyTypeName = e.value("keyTypeName", std::string());
    if (v.isArray)
        if (const auto& items = e.value("items", nlohmann::json::array()); items.is_array())
            for (const auto& it : items) v.defaultItems.push_back(scalarValueFromJson(it, v.type));
    if (v.container == ContainerKind::Map)
        if (const auto& keys = e.value("keys", nlohmann::json::array()); keys.is_array())
            for (const auto& k : keys) v.defaultKeys.push_back(scalarValueFromJson(k, v.keyType));
    v.className = e.value("className", std::string());
    v.typeName  = e.value("typeName", std::string());
    if (const auto& sd = e.value("structDefaults", nlohmann::json::object()); sd.is_object())
        for (auto it = sd.begin(); it != sd.end(); ++it)
        {
            if (!it->is_object()) continue;
            const PinType t = (PinType)it->value("t", (int)P::Float);
            v.structDefaults[it.key()] = scalarValueFromJson(it->value("v", nlohmann::json()), t);
        }
    if (const auto& f = e.value("f", nlohmann::json::array()); f.size() >= 4)
        for (int i = 0; i < 4; ++i) v.f[i] = f[i].get<float>();
    if (const auto& x = e.value("xform", nlohmann::json::array()); x.size() >= 9)
    {
        v.tpos = { x[0].get<float>(), x[1].get<float>(), x[2].get<float>() };
        v.trot = { x[3].get<float>(), x[4].get<float>(), x[5].get<float>() };
        v.tscl = { x[6].get<float>(), x[7].get<float>(), x[8].get<float>() };
    }
return true;
}
} // namespace

// ── Document JSON — assembled from the item writers above ───────────────────
std::string toJson(const Graph& g)
{
    nlohmann::json j;
    j["nextId"] = g.nextId;

    nlohmann::json jn = nlohmann::json::array();
    for (const Node& n : g.nodes) jn.push_back(nodeToJsonObj(n));
    j["nodes"] = std::move(jn);

    nlohmann::json jl = nlohmann::json::array();
    for (const Link& l : g.links) // ARRAY link form — see GraphJson.h
        jl.push_back(HE::graph::linkToArray(l.srcNode, l.srcPin, l.dstNode, l.dstPin));
    j["links"] = std::move(jl);

    nlohmann::json jv = nlohmann::json::array();
    for (const Variable& v : g.variables) jv.push_back(variableToJsonObj(v));
    j["variables"] = std::move(jv);

    // The class's declared events. Name-keyed like everything else: renaming one
    // rewrites the nodes, never the meaning of what is already on disk.
    if (!g.events.empty())
    {
        nlohmann::json je = nlohmann::json::array();
        for (const EventDecl& e : g.events)
        {
            nlohmann::json o = { { "name", e.name } };
            if (e.hasArg)
            {
                o["hasArg"] = true;
                o["argType"] = (int)e.argType;
                if (!e.typeName.empty()) o["typeName"] = e.typeName;
            }
            je.push_back(std::move(o));
        }
        j["events"] = std::move(je);
    }
    return j.dump(2);
}

bool fromJson(const std::string& json, Graph& out)
{
    nlohmann::json j;
    if (!HE::graph::parseGraphObject(json, j)) return false;

    Graph g;
    g.nextId = j.value("nextId", 1);
    for (const auto& e : j.value("nodes", nlohmann::json::array()))
    {
        Node n;
        if (!nodeFromJsonObj(e, n)) continue;   // unknown/removed node type
        HE::graph::bumpNextId(g.nextId, n.id);
        g.nodes.push_back(std::move(n));
    }
    for (const auto& e : j.value("links", nlohmann::json::array()))
    {
        Link l;
        if (!HE::graph::linkFromArray(e, l.srcNode, l.srcPin, l.dstNode, l.dstPin)) continue;
        // Unlike Material/ParticleGraph, HorizonCode drops links whose endpoints
        // didn't survive the node loop (an unknown/removed node type) — a dangling
        // exec link would otherwise stall the interpreter's chain walk.
        if (g.findNode(l.srcNode) && g.findNode(l.dstNode))
            g.links.push_back(l);
    }
    // MIGRATION — Create Object gained the Location/Rotation data inputs, and a
    // node's pin numbers are absolute (pinRanges: dataOut0 = dataIn0 + dataIns),
    // so its Object output moved from pin 2 to pin 4. Pin counts are NOT stored
    // per node for this type — the layout is derived from NodeType alone — so an
    // old file is recognised by the only thing that gives it away: a link
    // ORIGINATING at pin 2, which in the new layout is the Location *input*.
    // Links never originate at an input, so this is unambiguous and idempotent.
    // Without it, codegen would emit `n<id>_o-2` (srcPin - dataOut0) and fail to
    // compile for every graph that ever wired a spawned object up.
    for (Link& l : g.links)
    {
        const Node* src = g.findNode(l.srcNode);
        if (src && src->type == NodeType::CreateObject && l.srcPin == 2)
            l.srcPin = 4;
    }
    for (const auto& e : j.value("variables", nlohmann::json::array()))
    {
        Variable v;
        if (!variableFromJsonObj(e, v)) continue;
        g.variables.push_back(std::move(v));
    }
    for (const auto& e : j.value("events", nlohmann::json::array()))
    {
        if (!e.is_object()) continue;
        EventDecl d;
        d.name = e.value("name", std::string());
        if (d.name.empty()) continue;
        d.hasArg  = e.value("hasArg", false);
        d.argType = (PinType)e.value("argType", (int)P::Float);
        d.typeName = e.value("typeName", std::string());
        if (!g.findEvent(d.name)) g.events.push_back(std::move(d));
    }
    inferEventDecls(g);        // graphs older than declared events arrive with one
    // Both syncs below rebuild pin layouts from definitions that may have changed
    // shape since this graph was saved (a function edited elsewhere, a struct
    // field or enum entry deleted in the type editor). The stored mirrors ARE the
    // layout the stored links were addressed against — snapshot them first so
    // every wire follows its pin instead of silently sliding onto a neighbour.
    std::vector<int> mirrored;
    for (const Node& n : g.nodes)
        switch (n.type)
        {
        case NodeType::FunctionEntry: case NodeType::FunctionCall:
        case NodeType::FunctionReturn:
            mirrored.push_back(n.id); break;
        case NodeType::MakeStruct: case NodeType::BreakStruct:
        case NodeType::SwitchOnEnum:
            // Only nodes that already KNOW their definition — those carry the
            // stored mirror the links were addressed against. A bare node
            // (older build, no typeName yet) is repaired by inferUserTypeNames
            // below, and that repair remaps its links itself; snapshotting it
            // here would remap them twice and drop them as stale.
            if (!n.typeName.empty()) mirrored.push_back(n.id);
            break;
        default: break;
        }
    const LinkRemapSnapshot preSync = captureLinkRemapSnapshot(g, mirrored);
    syncFunctionSignatures(g); // reconcile call/return pins with their entries
    inferUserTypeNames(g);     // recover Enum/Struct definitions from the wiring
    syncTypeSignatures(g);     // re-mirror struct/enum pins from the TypeRegistry
    remapLinksFromSnapshot(g, preSync); // wires follow their pins (or drop, visibly)
    assignSubgraphs(g);        // migrate flat graphs → per-function sub-graphs
    out = std::move(g);
    return true;
}

// ── Item-level JSON, public (collaboration addresses single items) ──────────
std::string nodeToJson(const Node& n)     { return nodeToJsonObj(n).dump(); }
std::string variableToJson(const Variable& v) { return variableToJsonObj(v).dump(); }

bool nodeFromJson(const std::string& json, Node& out)
{
    nlohmann::json j;
    if (!HE::graph::parseGraphObject(json, j)) return false;
    Node n;
    if (!nodeFromJsonObj(j, n)) return false;
    out = std::move(n);
    return true;
}

bool variableFromJson(const std::string& json, Variable& out)
{
    nlohmann::json j;
    if (!HE::graph::parseGraphObject(json, j)) return false;
    Variable v;
    if (!variableFromJsonObj(j, v)) return false;
    out = std::move(v);
    return true;
}

void syncFunctionSignatures(Graph& g)
{
    // The FunctionEntry owns each function's interface; mirror it onto the calls
    // and returns of the same name so their pins match. Calls/returns whose
    // function lives in another graph (no local entry) keep their own mirror.
    for (Node& n : g.nodes)
    {
        if (n.type != NodeType::FunctionCall && n.type != NodeType::FunctionReturn) continue;
        const Node* entry = nullptr;
        for (const Node& e : g.nodes)
            if (e.type == NodeType::FunctionEntry && e.s == n.s) { entry = &e; break; }
        if (!entry) continue;
        n.params  = entry->params;
        n.results = entry->results;
    }
}

// Binding a definition to a node CHANGES ITS PIN LAYOUT — syncTypeSignatures
// gives Make Struct one data-in per field, Switch on Enum one exec-out per
// entry — while the persisted links still address the old, bare layout. Every
// affected link end has to move with its pin, or a wire quietly lands somewhere
// else: a Switch's Default exec-out would become its FIRST entry's branch, which
// is a misroute nobody sees, not an error anybody reports.
//
// `nodes` are ones that just went from no definition to one, so their "before"
// is always the bare shape — that is what makes the mapping closed.
void remapLinksForMirror(Graph& g, const std::vector<int>& nodes)
{
    struct Shape { PinRanges before; NodeType type; size_t mirrored = 0; };
    std::unordered_map<int, Shape> shapes;
    for (const int id : nodes)
        if (const Node* n = g.findNode(id))
            shapes[id] = Shape{ pinRanges(*n), n->type, 0 };

    syncTypeSignatures(g);   // the pins the definition implies

    for (auto& [id, sh] : shapes)
        if (const Node* n = g.findNode(id)) sh.mirrored = n->params.size();

    auto remap = [&](int nodeId, int pin) -> int
    {
        const auto it = shapes.find(nodeId);
        if (it == shapes.end()) return pin;
        const Node* n = g.findNode(nodeId);
        if (!n) return pin;
        const Shape&    sh = it->second;
        const PinRanges a  = sh.before, b = pinRanges(*n);
        // Switch on Enum keeps Default LAST, so it slides past the entries that
        // just appeared — the one place a pin moves WITHIN its own region.
        if (sh.type == NodeType::SwitchOnEnum && pin >= a.execOut0 && pin < a.dataIn0)
            return b.execOut0 + (int)sh.mirrored + (pin - a.execOut0);
        if (pin < a.execOut0) return b.execIn0  + (pin - a.execIn0);
        if (pin < a.dataIn0)  return b.execOut0 + (pin - a.execOut0);
        if (pin < a.dataOut0) return b.dataIn0  + (pin - a.dataIn0);
        return b.dataOut0 + (pin - a.dataOut0);
    };

    for (Link& l : g.links)
    {
        l.srcPin = remap(l.srcNode, l.srcPin);
        l.dstPin = remap(l.dstNode, l.dstPin);
    }
}

LinkRemapSnapshot captureLinkRemapSnapshot(const Graph& g, const std::vector<int>& nodeIds)
{
    LinkRemapSnapshot snap;
    auto names = [](const std::vector<PinDesc>& v)
    {
        std::vector<std::string> out;
        out.reserve(v.size());
        for (const PinDesc& p : v) out.emplace_back(p.name ? p.name : "");
        return out;
    };
    for (const int id : nodeIds)
    {
        const Node* n = g.findNode(id);
        if (!n) continue;
        const NodeSig s = signatureOf(*n);
        LinkRemapSnapshot::Sig sig;
        sig.execIns  = names(s.execIns);
        sig.execOuts = names(s.execOuts);
        sig.dataIns  = names(s.dataIns);
        sig.dataOuts = names(s.dataOuts);
        snap.sigs.emplace(id, std::move(sig));
    }
    return snap;
}

void remapLinksFromSnapshot(Graph& g, const LinkRemapSnapshot& snap)
{
    if (snap.sigs.empty()) return;

    // The layouts as they are NOW, for the same nodes.
    std::vector<int> ids;
    ids.reserve(snap.sigs.size());
    for (const auto& [id, sig] : snap.sigs) ids.push_back(id);
    const LinkRemapSnapshot now = captureLinkRemapSnapshot(g, ids);

    // Move one link end onto the pin that carries its old pin's NAME today.
    // Returns false when the pin is genuinely gone — the caller drops the link,
    // which is visible and safe; the silent alternative is the wire sliding
    // onto whatever pin inherited the index.
    auto remapEnd = [&](int nodeId, int& pin) -> bool
    {
        const auto itOld = snap.sigs.find(nodeId);
        if (itOld == snap.sigs.end()) return true;   // node wasn't re-mirrored
        const auto itNew = now.sigs.find(nodeId);
        if (itNew == now.sigs.end()) return false;   // node no longer exists
        const auto& o = itOld->second;
        const auto& c = itNew->second;
        const std::vector<std::string>* oldRegs[4] = { &o.execIns, &o.execOuts, &o.dataIns, &o.dataOuts };
        const std::vector<std::string>* newRegs[4] = { &c.execIns, &c.execOuts, &c.dataIns, &c.dataOuts };
        int base = 0, newBase = 0;
        for (int r = 0; r < 4; ++r)
        {
            const std::vector<std::string>& ov = *oldRegs[r];
            const std::vector<std::string>& nv = *newRegs[r];
            if (pin < base + (int)ov.size())
            {
                const int idx = pin - base;
                const std::string& name = ov[idx];
                // Duplicate names stay deterministic: this pin is the k-th
                // occurrence of `name`, so it maps to the k-th occurrence now.
                int occ = 0;
                for (int i = 0; i < idx; ++i) if (ov[i] == name) ++occ;
                int seen = 0;
                for (int i = 0; i < (int)nv.size(); ++i)
                    if (nv[i] == name && seen++ == occ) { pin = newBase + i; return true; }
                // No pin of that name any more. Same region size means an
                // in-place rename/retype — keeping the index is what preserves
                // the user's wires there. A changed size means the pin was
                // removed: drop.
                if (nv.size() == ov.size()) { pin = newBase + idx; return true; }
                return false;
            }
            base    += (int)ov.size();
            newBase += (int)nv.size();
        }
        return false;   // pin was already beyond the old layout — stale link
    };

    std::vector<Link> kept;
    kept.reserve(g.links.size());
    for (Link l : g.links)
        if (remapEnd(l.srcNode, l.srcPin) && remapEnd(l.dstNode, l.dstPin))
            kept.push_back(l);
    g.links = std::move(kept);
}

EventDecl* Graph::findEvent(const std::string& name)
{
    for (auto& e : events) if (e.name == name) return &e;
    return nullptr;
}
const EventDecl* Graph::findEvent(const std::string& name) const
{
    for (const auto& e : events) if (e.name == name) return &e;
    return nullptr;
}

const std::vector<EngineEventDesc>& engineEvents()
{
    static const std::vector<EngineEventDesc> k = {
        { "Construct",            "onConstruct",          P::Exec,   false },
        { "Destruct",             "onDestruct",           P::Exec,   false },
        { "Tick",                 "onTick",               P::Float,  false },
        { "BeginPlay",            "onBeginPlay",          P::Exec,   false },
        { "OnClicked",            "onClicked",            P::Exec,   true  },
        { "OnPressed",            "onPressed",            P::Exec,   true  },
        { "OnReleased",           "onReleased",           P::Exec,   true  },
        { "OnHovered",            "onHovered",            P::Exec,   true  },
        { "OnUnhovered",          "onUnhovered",          P::Exec,   true  },
        { "OnMouseEnter",         "onMouseEnter",         P::Exec,   true  },
        { "OnMouseLeave",         "onMouseLeave",         P::Exec,   true  },
        { "OnFocused",            "onFocused",            P::Exec,   true  },
        { "OnUnfocused",          "onUnfocused",          P::Exec,   true  },
        { "OnTextChanged",        "onTextChanged",        P::String, true  },
        { "OnTextCommitted",      "onTextCommitted",      P::String, true  },
        { "OnValueChanged",       "onValueChanged",       P::Float,  true  },
        { "OnCheckChanged",       "onCheckChanged",       P::Bool,   true  },
        { "OnSelectionChanged",   "onSelectionChanged",   P::Int,    true  },
        // A ListView asking to have one row filled in. The argument is the ITEM
        // index, not the row: the list holds no data, so this is the only
        // question it can ask and the answer is whatever the owner keeps.
        { "OnRowBind",            "onRowBind",            P::Int,    true  },
        // …and a row being opened rather than merely picked (double-click,
        // Enter). Separate from the selection because "which one" and "go" are
        // two different answers in every list that has ever existed.
        { "OnRowActivated",       "onRowActivated",       P::Int,    true  },
        // The other mouse button, on an element. Its own event rather than a
        // flag on OnClicked, because a right-click means something different
        // everywhere it means anything: it opens a menu, it never presses.
        { "OnRightClicked",       "onRightClicked",       P::Exec,   true  },
        // A dialog, popup or menu closing. Fired on the widget's OWN graph and
        // not addressed to an element, because what closed is the whole thing.
        { "OnDismissed",          "onDismissed",          P::Exec,   false },
        { "OnInit",               "onInit",               P::Exec,   false },
        { "OnShutdown",           "onShutdown",           P::Exec,   false },
        { "OnWindowFocusChanged", "onWindowFocusChanged", P::Bool,   false },
        { "OnLevelLoaded",        "onLevelLoaded",        P::Exec,   false },
        { "OnLevelUnloaded",      "onLevelUnloaded",      P::Exec,   false },
        // Physics contacts on an Entity class. The argument is the OTHER entity
        // as an Int — the same way every entity id travels through HorizonCode
        // (HE::api's entity/transform groups take P::Int). It is also all there
        // is to carry: PhysicsWorld::CollisionEvent holds two entity ids and no
        // contact point or normal.
        //
        // Overlap = a contact where one side is a trigger (ColliderComponent::
        // isTrigger); Hit = a blocking contact. The split happens in
        // PhysicsWorld, so a graph never has to ask which kind it got.
        { "OnBeginOverlap",       "onBeginOverlap",       P::Int,    false },
        { "OnEndOverlap",         "onEndOverlap",         P::Int,    false },
        { "OnHit",                "onHit",                P::Int,    false },
        { "OnHitEnd",             "onHitEnd",             P::Int,    false },
    };
    return k;
}
const EngineEventDesc* findEngineEvent(const std::string& name)
{
    for (const EngineEventDesc& e : engineEvents())
        if (name == e.name) return &e;
    return nullptr;
}

// ── the engine's own class taxonomy ─────────────────────────────────────────
// Each row lists only what it ADDS to its base; engineClassEvents/Members walk
// the chain. Entity is where the game lifecycle starts, because BeginPlay and
// Tick only mean something for a class the world actually runs — a plain Object
// is created and destroyed on demand by whoever holds its reference.
const std::vector<EngineClassDesc>& engineClasses()
{
    static const std::vector<EngineClassDesc> k = {
        { "Object",           "",       { "Construct", "Destruct" }, {} },
        { "Entity",           "Object", { "BeginPlay", "Tick",
                                          "OnBeginOverlap", "OnEndOverlap",
                                          "OnHit", "OnHitEnd" },
                                        { { "Get Owning Entity", "entity.owned", 0 } } },
        { "PlayerCharacter",  "Entity", {},
                                        { { "Get Controller", "player.controllerOf", 0 } } },
        // Movement reads hang off Entity's "Get Owning Entity", not here: they
        // take an entity, and every Entity has one. Dragging off a character
        // reference therefore reads as the traversal it is — the character, its
        // entity, its speed — with no per-class table to keep in step.
        { "PlayerController", "Entity", {},
                                        { { "Possess",   "player.possess",   0 },
                                          { "Un Possess", "player.unpossess", 0 },
                                          { "Get Possessed Character", "player.possessed", 0 } } },
    };
    return k;
}

namespace
{
// "" is how every asset predating the taxonomy spells "Object" on disk, and
// resolving it here rather than at each call site is what keeps those assets
// from falling out of every chain walk below.
const char* normalizedClassName(const std::string& name)
{
    return name.empty() ? "Object" : name.c_str();
}
}

const EngineClassDesc* findEngineClass(const std::string& name)
{
    const char* n = normalizedClassName(name);
    for (const EngineClassDesc& c : engineClasses())
        if (std::strcmp(n, c.name) == 0) return &c;
    return nullptr;
}

bool engineClassIsA(const std::string& derived, const std::string& base)
{
    const char* d = normalizedClassName(derived);
    const char* b = normalizedClassName(base);
    if (std::strcmp(d, b) == 0) return true;

    // Unknown names never enter the loop below (findEngineClass returns null),
    // so a garbage baseClass answers "yes" only to the equality above.
    for (const EngineClassDesc* c = findEngineClass(d); c && *c->base; )
    {
        if (std::strcmp(c->base, b) == 0) return true;
        c = findEngineClass(c->base);
    }
    return false;
}

namespace
{
// Base-first walk shared by the two accessors: collect the chain from the named
// class up to the root, then append each row's own additions in reverse so the
// result reads Object's entries first.
std::vector<const EngineClassDesc*> classChain(const std::string& name)
{
    std::vector<const EngineClassDesc*> chain;
    for (const EngineClassDesc* c = findEngineClass(name); c; c = *c->base ? findEngineClass(c->base) : nullptr)
        chain.push_back(c);
    std::reverse(chain.begin(), chain.end());
    return chain;
}
}

std::vector<const char*> engineClassEvents(const std::string& name)
{
    std::vector<const char*> out;
    for (const EngineClassDesc* c : classChain(name))
        out.insert(out.end(), c->events.begin(), c->events.end());
    return out;
}

std::vector<EngineClassMember> engineClassMembers(const std::string& name)
{
    std::vector<EngineClassMember> out;
    for (const EngineClassDesc* c : classChain(name))
        out.insert(out.end(), c->members.begin(), c->members.end());
    return out;
}

// ── interned event ids ───────────────────────────────────────────────────────
// Append-only and process-global: an id handed out stays valid for the session,
// so the listener tables and the generated code can hold on to one. Guarded
// because graphs load on worker threads (asset streaming, the export worker).
namespace
{
struct EventInternTable
{
    std::mutex                                    mutex;
    std::vector<std::string>                      names{ std::string() };  // id 0 = "" (none)
    std::unordered_map<std::string, EventId>      ids;
};
EventInternTable& interns()
{
    static EventInternTable t;
    return t;
}
}

EventId eventId(const std::string& name)
{
    if (name.empty()) return 0;
    EventInternTable& t = interns();
    std::lock_guard<std::mutex> lk(t.mutex);
    if (const auto it = t.ids.find(name); it != t.ids.end()) return it->second;
    const EventId id = (EventId)t.names.size();
    t.names.push_back(name);
    t.ids.emplace(name, id);
    return id;
}
std::string eventName(EventId id)
{
    EventInternTable& t = interns();
    std::lock_guard<std::mutex> lk(t.mutex);
    return id < t.names.size() ? t.names[id] : std::string();
}

void inferEventDecls(Graph& g)
{
    auto declare = [&g](const Node& n)
    {
        if (n.s.empty() || findEngineEvent(n.s)) return;   // engine events are not ours
        if (g.findEvent(n.s)) return;
        EventDecl d;
        d.name = n.s;
        d.hasArg = n.hasArg;
        d.argType = n.propType;
        d.typeName = n.typeName;
        g.events.push_back(std::move(d));
    };
    for (const Node& n : g.nodes)
        if (n.type == NodeType::Event || n.type == NodeType::EmitEvent) declare(n);
}

bool canConvertPinType(PinType from, PinType to)
{
    if (from == to) return true;
    auto numeric = [](PinType t)
    { return t == P::Float || t == P::Int || t == P::Bool; };
    if (numeric(from) && numeric(to)) return true;
    // Enum is int-backed, so it reads as a number and a number can name one.
    if (from == P::Enum && (to == P::Float || to == P::Int)) return true;
    if (to == P::Enum && (from == P::Float || from == P::Int)) return true;
    // Vec3/Vec4/Color are three views of the same numbers. They interconvert so
    // that a graph authored while Color WAS the vec3 type keeps its wires — links
    // are restored from JSON without re-checking pin types, so the conversion is
    // what actually carries those graphs, not this predicate. Vec2 stays out:
    // nothing ever converted into it, and each rule here is duplicated in two
    // coerce implementations that must not drift.
    auto vectorish = [](PinType t)
    { return t == P::Vec3 || t == P::Vec4 || t == P::Color; };
    if (vectorish(from) && vectorish(to)) return true;
    return false;
}

bool conversionNodeFor(PinType from, ContainerKind fromKind,
                       PinType to, ContainerKind toKind, NodeType& out)
{
    // Exec is control flow, not a value. Nothing converts it.
    if (from == P::Exec || to == P::Exec) return false;
    // The two container crossings a node answers, both decided BEFORE the
    // implicit test below: the two sides carry the same element type, which
    // canConvertPinType calls convertible — true for a scalar wire, false the
    // moment the kinds differ (see Graph::connect).
    //
    // Element types must MATCH here, not merely convert: the conversion node
    // changes the container, never the elements, so an Array<Float> reaching a
    // Set<Int> pin is not a job Set From Array can do. Graph::connect refuses
    // that wire for the same reason.
    if (fromKind == CK::Set && toKind == CK::Array && from == to)
    {
        out = T::SetToArray;
        return true;
    }
    // …and back. Set From Array exists now, so the drag that used to be a dead
    // end is one node away. It deduplicates on the way, which is what the target
    // pin asked for by being a Set.
    if (fromKind == CK::Array && toKind == CK::Set && from == to)
    {
        out = T::SetFromArray;
        return true;
    }
    // Every other kind mismatch — Array↔Map, container↔scalar — would take more
    // than one node, or means nothing at all.
    if (fromKind != CK::None || toKind != CK::None) return false;
    // The wire already carries the value: a node here would convert nothing.
    // This is also what keeps Enum to Int / Int to Enum out — the number and its
    // enum reach each other implicitly.
    if (canConvertPinType(from, to)) return false;

    if (to == P::String)
    {
        // Enum first: it is int-backed, so it would coerce straight into To
        // String's Float pin and print the NUMBER where the name was meant.
        if (from == P::Enum) { out = T::EnumToString; return true; }
        // To String's input is Float; Int and Bool reach it through the very
        // coercion Graph::connect performs, so both halves provably connect.
        if (from == P::Float || from == P::Int || from == P::Bool)
        { out = T::ToString; return true; }
    }
    return false;
}

bool connectWithConversion(Graph& g, int srcNode, int srcPin, int dstNode, int dstPin,
                           const std::vector<NodeType>& excluded)
{
    const Node* s = g.findNode(srcNode);
    const Node* d = g.findNode(dstNode);
    if (!s || !d || srcNode == dstNode) return false;

    // The pin ROLES are re-checked here instead of being read out of connect's
    // failure: it answers false for an exec pin, for a reversed drag and for a
    // type mismatch alike, and only the last of those is ours to repair.
    const PinRanges sr = pinRanges(*s);
    const PinRanges dr = pinRanges(*d);
    if (srcPin < sr.dataOut0 || srcPin >= sr.end)      return false;
    if (dstPin < dr.dataIn0  || dstPin >= dr.dataOut0) return false;

    PinDesc sd{}, dd{};
    if (!dataPinDescOf(*s, false, srcPin - sr.dataOut0, sd) ||
        !dataPinDescOf(*d, true,  dstPin - dr.dataIn0,  dd)) return false;

    NodeType convType{};
    if (!conversionNodeFor(sd.type, sd.kind(), dd.type, dd.kind(), convType)) return false;
    // A type this frontend hides from its palette gets no back door here either.
    for (NodeType x : excluded) if (x == convType) return false;

    Node conv;
    conv.type     = convType;
    conv.subgraph = s->subgraph;        // a drawn wire never leaves its sub-graph
    conv.x = (s->x + d->x) * 0.5f;      // graph coordinates: half-way down the wire
    conv.y = (s->y + d->y) * 0.5f;
    // Set To Array builds BOTH its pins out of propType — left at the default it
    // would advertise a Set<Float>, and neither half would land.
    if (convType == T::SetToArray) conv.propType = sd.type;
    // The definition, off the source pin — Enum/Struct pins are the only ones it
    // means anything on (PinDesc's contract). inferUserTypeNames below covers
    // what this cannot: a source at the generic boundary (an engine call's
    // untyped Enum) whose definition sits further up the wire.
    if (sd.typeName && (sd.type == P::Enum || sd.type == P::Struct))
        conv.typeName = sd.typeName;

    // Ranges off the seeded node, before it moves into the graph: these three
    // types have a fixed layout (one data-in, one data-out, no exec pins and no
    // mirrored params), so nothing can shift it afterwards.
    const PinRanges cr = pinRanges(conv);
    const int convId = g.addNode(std::move(conv));   // `s` and `d` dangle from here

    if (!g.connect(srcNode, srcPin, convId, cr.dataIn0) ||
        !g.connect(convId, cr.dataOut0, dstNode, dstPin))
    {
        // Half a conversion is litter on the canvas. Every one of connect's
        // rejections happens before it touches an existing link, so dropping the
        // node (and with it the first wire) restores the graph exactly.
        g.removeNode(convId);
        return false;
    }
    inferUserTypeNames(g);
    return true;
}

void inferUserTypeNames(Graph& g)
{
    // Declared variables are authoritative for their own Get/Set nodes.
    for (Node& n : g.nodes)
        if ((n.type == NodeType::GetVariable || n.type == NodeType::SetVariable) &&
            n.typeName.empty())
            if (const Variable* v = g.findVariable(n.s)) n.typeName = v->typeName;

    // Then propagate along wires until nothing new is learned (a chain of array
    // ops picks its element definition up one hop at a time). Matched PER PIN,
    // not per node: an array node advertises its element kind in propType, but
    // Make/Break/Get/SetStructField and the enum nodes leave propType alone and
    // put everything in typeName — the very nodes that report the definition as
    // missing. Their Struct/Enum pins are what has to be looked at.
    std::vector<int> repaired;
    for (bool changed = true; changed; )
    {
        changed = false;
        for (Node& n : g.nodes)
        {
            if (!n.typeName.empty()) continue;
            const NodeSig  mine = signatureOf(n);
            const PinRanges mr  = pinRanges(n);
            std::string found;
            bool ambiguous = false;
            for (const Link& l : g.links)
            {
                const bool weAreSrc = l.srcNode == n.id;
                if (!weAreSrc && l.dstNode != n.id) continue;
                // Our end has to BE a user-defined-type pin.
                const int ourPin = weAreSrc ? l.srcPin : l.dstPin;
                const std::vector<PinDesc>& ours = weAreSrc ? mine.dataOuts : mine.dataIns;
                const int ourIdx = ourPin - (weAreSrc ? mr.dataOut0 : mr.dataIn0);
                if (ourIdx < 0 || ourIdx >= (int)ours.size()) continue;
                const PinType want = ours[(size_t)ourIdx].type;
                if (want != P::Enum && want != P::Struct) continue;

                const Node* peer = g.findNode(weAreSrc ? l.dstNode : l.srcNode);
                if (!peer || peer->id == n.id) continue;
                const PinRanges pr = pinRanges(*peer);
                const int idx = weAreSrc ? l.dstPin - pr.dataIn0 : l.srcPin - pr.dataOut0;
                PinDesc pd{};
                if (!dataPinDescOf(*peer, /*input=*/weAreSrc, idx, pd)) continue;
                if (pd.type != want || !pd.typeName || !*pd.typeName) continue;
                if (found.empty())            found = pd.typeName;
                else if (found != pd.typeName) { ambiguous = true; break; }
            }
            if (ambiguous || found.empty()) continue;
            n.typeName = found;
            repaired.push_back(n.id);
            changed = true;
        }
    }
    if (!repaired.empty()) remapLinksForMirror(g, repaired);
}

void syncTypeSignatures(Graph& g)
{
    auto& reg = HE::TypeRegistry::instance();
    auto fieldsToParams = [](const HE::StructDef& def)
    {
        std::vector<FuncParam> ps;
        ps.reserve(def.fields.size());
        for (const HE::StructField& f : def.fields)
            ps.push_back({ f.name, f.type, f.isArray, f.typeName });
        return ps;
    };
    for (Node& n : g.nodes)
    {
        switch (n.type)
        {
        case NodeType::MakeStruct:
        case NodeType::BreakStruct:
        {
            HE::StructDef def;
            if (!n.typeName.empty() && reg.getStruct(n.typeName, def))
                n.params = fieldsToParams(def);
            break;   // missing def: keep the stored mirror (may load later)
        }
        case NodeType::GetStructField:
        case NodeType::SetStructField:
        {
            // Revalidate the chosen field (params[0]) against the current def:
            // retype a renamed-type field, keep the mirror if the def is gone,
            // drop the choice entirely if the FIELD is gone.
            HE::StructDef def;
            if (n.typeName.empty() || !reg.getStruct(n.typeName, def)) break;
            if (n.params.empty()) break;
            if (const HE::StructField* f = def.findField(n.params[0].name))
                n.params[0] = { f->name, f->type, f->isArray, f->typeName };
            else
                n.params.clear();
            break;
        }
        case NodeType::SwitchOnEnum:
        {
            HE::EnumDef def;
            if (n.typeName.empty() || !reg.getEnum(n.typeName, def)) break;
            std::vector<FuncParam> ps;
            ps.reserve(def.entries.size());
            for (const HE::EnumEntry& e : def.entries)
                ps.push_back({ e.name, PinType::Exec, false, {} });
            n.params = std::move(ps);
            break;
        }
        case NodeType::ConstEnum:
        {
            // Clamp a stale stored value onto a live entry so the dropdown and
            // the emitted literal never disagree.
            HE::EnumDef def;
            if (n.typeName.empty() || !reg.getEnum(n.typeName, def) || def.entries.empty()) break;
            if (!def.findValue((int)n.f[0])) n.f[0] = (float)def.entries.front().value;
            break;
        }
        default: break;
        }
    }
}

namespace
{
// The exec-forward closure from `start` plus every data node feeding those exec
// nodes — i.e. the body of the graph rooted at `start` (an Event or a
// FunctionEntry). Used to partition a flat graph into per-function sub-graphs.
std::unordered_set<int> traceBody(const Graph& g, int start)
{
    std::unordered_set<int> body; std::vector<int> stack;
    body.insert(start); stack.push_back(start);
    // Exec-forward.
    while (!stack.empty())
    {
        const int id = stack.back(); stack.pop_back();
        const Node* n = g.findNode(id); if (!n) continue;
        const PinRanges r = pinRanges(*n);
        for (const auto& l : g.links)
            if (l.srcNode == id && l.srcPin >= r.execOut0 && l.srcPin < r.dataIn0)
                if (body.insert(l.dstNode).second) stack.push_back(l.dstNode);
    }
    // Data producers feeding any node already in the body.
    stack.assign(body.begin(), body.end());
    while (!stack.empty())
    {
        const int id = stack.back(); stack.pop_back();
        const Node* n = g.findNode(id); if (!n) continue;
        const PinRanges r = pinRanges(*n);
        for (const auto& l : g.links)
            if (l.dstNode == id && l.dstPin >= r.dataIn0 && l.dstPin < r.dataOut0)
                if (body.insert(l.srcNode).second) stack.push_back(l.srcNode);
    }
    return body;
}
} // namespace

void assignSubgraphs(Graph& g)
{
    for (const Node& n : g.nodes) if (n.subgraph != 0) return; // already partitioned
    bool hasFn = false;
    for (const Node& n : g.nodes) if (n.type == NodeType::FunctionEntry) { hasFn = true; break; }
    if (!hasFn) return;

    // Nodes reachable from an Event stay in the event graph (subgraph 0).
    std::unordered_set<int> eventOwned;
    for (const Node& n : g.nodes)
        if (n.type == NodeType::Event)
        { auto b = traceBody(g, n.id); eventOwned.insert(b.begin(), b.end()); }

    // Each function claims its own body (minus anything an event already owns).
    for (Node& e : g.nodes)
    {
        if (e.type != NodeType::FunctionEntry) continue;
        e.subgraph = e.id;
        for (int m : traceBody(g, e.id))
        {
            if (m == e.id || eventOwned.count(m)) continue;
            if (Node* mn = g.findNode(m); mn && mn->subgraph == 0) mn->subgraph = e.id;
        }
    }
}

// ── Interpreter ──────────────────────────────────────────────────────────────

namespace
{
constexpr int kMaxSteps = 4096;
constexpr int kMaxDepth = 64;

} // namespace

// Element-type equality for Contains / IndexOf, set membership and map keys
// (scalar values only). Public — see the declaration in HorizonCode.h.
bool scalarValueEquals(const Value& a, const Value& b, PinType t)
{
    switch (t)
    {
        case P::Float:  return a.f == b.f;
        case P::Bool:   return a.b == b.b;
        case P::Int:    return a.i == b.i;
        case P::String: return a.s == b.s;
        case P::Vec2:   return a.v2 == b.v2;
        case P::Color:  return a.col == b.col;
        case P::Vec3:   return a.v3 == b.v3;
        case P::Vec4:   return a.v4 == b.v4;
        case P::Ref:    return a.ref == b.ref;
        case P::Transform: return a.tpos == b.tpos && a.trot == b.trot && a.tscl == b.tscl;
        case P::Enum:   return a.i == b.i;
        default:        return false;
    }
}

namespace
{
// ── THE coercion rule. Two other places implement it and MUST match: ─────────
//   • HorizonCodeGenSupport.h `hc::coerce*` — the generated-C++ backend. It is a
//     DELIBERATE duplicate, not an oversight: generated code must produce the
//     byte-identical result the interpreter would, without linking the
//     interpreter. Change one → change the other, or a packaged build silently
//     diverges from what the editor previewed.
//   • UIWidgetBinding.cpp `uiHcValueToProp` — the widget-property bridge, which
//     coerces into UIPropValue instead of Value but follows the same rule.
// Only Float↔Int↔Bool convert (an Enum counts as its Int); any other mismatch
// yields the target's zero. Coercing INTO Enum/Struct never invents a typeName —
// wiring already type-checked the definition, so a same-type value passes
// through above and a mismatch degrades to a typed empty value.
Value coerce(Value v, PinType want)
{
    if (v.isArray) return v;   // arrays are never scalar-coerced (pass through whole)
    if (v.type == want) return v;
    Value r; r.type = want;
    switch (want)
    {
        case P::Float:  r.f = v.type == P::Bool ? (v.b ? 1.0f : 0.0f)
                            : v.type == P::Int ? (float)v.i
                            : v.type == P::Enum ? (float)v.i : 0.0f; break;
        case P::Int:    r.i = v.type == P::Float ? (int)v.f
                            : v.type == P::Bool ? (v.b ? 1 : 0)
                            : v.type == P::Enum ? v.i : 0; break;
        case P::Bool:   r.b = v.type == P::Float ? v.f != 0.0f
                            : v.type == P::Int ? v.i != 0 : false; break;
        case P::Enum:   r.i = v.type == P::Int ? v.i
                            : v.type == P::Float ? (int)v.f : 0; break;
        // Vector ↔ colour. Widening pads, narrowing drops — and the pad differs
        // by TARGET, not by source: a vector's fourth component is 0 (a direction
        // has no w), a colour's is 1 (opaque). Anything that is not one of the
        // three yields the target's zero, like every other mismatch here.
        case P::Vec3:
            r.v3 = v.type == P::Vec4  ? glm::vec3(v.v4)
                 : v.type == P::Color ? glm::vec3(v.col) : glm::vec3(0.0f);
            break;
        case P::Vec4:
            r.v4 = v.type == P::Vec3  ? glm::vec4(v.v3, 0.0f)
                 : v.type == P::Color ? v.col : glm::vec4(0.0f);
            break;
        case P::Color:
            r.col = v.type == P::Vec3 ? glm::vec4(v.v3, 1.0f)
                  : v.type == P::Vec4 ? v.v4 : glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
            break;
        default: break;
    }
    return r;
}
} // namespace

Runner::Runner(const Graph& graph, Context ctx) : m_graph(graph), m_ctx(std::move(ctx)) {}

Runner::CallFrame* Runner::frameFor(int fnEntryId)
{
    for (auto it = m_callStack.rbegin(); it != m_callStack.rend(); ++it)
        if (it->fnEntryId == fnEntryId) return &*it;
    return nullptr;
}

namespace
{
// Seed a fresh frame's local-variable store from the function's declared
// locals (Variable::scope == the FunctionEntry's id) — per invocation, like
// Runtime::add seeds the instance store from the scope-0 variables.
void seedLocals(const Graph& g, int fnEntryId,
                std::unordered_map<std::string, Value>& locals)
{
    for (const auto& v : g.variables)
        if (v.scope == fnEntryId) locals[v.name] = variableDefaultValue(v);
}
} // namespace

const Link* Runner::execLinkFrom(int nodeId, int pin) const
{
    for (const auto& l : m_graph.links)
        if (l.srcNode == nodeId && l.srcPin == pin) return &l;
    return nullptr;
}

void Runner::fireEvent(const std::string& eventName, int elem, const Value& arg)
{
    m_steps = 0;
    m_eventArg = arg;
    m_execOutputs.clear();
    m_callStack.clear();
    for (const auto& n : m_graph.nodes)
    {
        // An Input Action node answers to the names its action produces, and
        // enters the chain the name selects: Pressed, Released or the axis one.
        if (n.type == T::InputAction)
        {
            const int chain = inputActionChainFor(n, eventName);
            if (chain >= 0) runExecChain(n, pinRanges(n).execOut0 + chain, 0);
            continue;
        }
        if (n.type != T::Event || n.s != eventName) continue;
        if (n.elem != 0 && n.elem != elem) continue;
        runExecChain(n, pinRanges(n).execOut0, 0);
    }
}

void Runner::resumeFrom(int nodeId)
{
    const Node* n = m_graph.findNode(nodeId);
    if (!n) return;
    // A fresh run, exactly like a fire: the original run's event arg and
    // exec-output caches are gone (the chain re-reads live state instead).
    m_steps = 0;
    m_eventArg = Value{};
    m_execOutputs.clear();
    m_callStack.clear();
    runExecChain(*n, pinRanges(*n).execOut0, 0);
}

bool Runner::callFunction(const std::string& name, bool requirePublic,
                          const std::vector<Value>& args, std::vector<Value>* results)
{
    for (const auto& n : m_graph.nodes)
    {
        if (n.type != T::FunctionEntry || n.s != name) continue;
        if (requirePublic && n.access != 0) return false;
        m_steps = 0;
        m_execOutputs.clear();
        m_callStack.clear();
        // Seed the frame: passed args coerced to the param types (missing ones fall
        // back to a typed default), typed result slots for any Return to fill, and
        // the function's locals at their declared defaults.
        CallFrame frame;
        frame.fnEntryId = n.id;
        seedLocals(m_graph, n.id, frame.locals);
        frame.args.resize(n.params.size());
        for (size_t i = 0; i < n.params.size(); ++i)
        {
            if (i < args.size()) frame.args[i] = coerce(args[i], n.params[i].type);
            else                 frame.args[i].type = n.params[i].type; // typed default
        }
        frame.results.resize(n.results.size());
        for (size_t i = 0; i < n.results.size(); ++i) frame.results[i].type = n.results[i].type;
        m_callStack.push_back(std::move(frame));
        runExecChain(n, pinRanges(n).execOut0, 0);
        if (results) *results = m_callStack.back().results;
        m_callStack.pop_back();
        return true;
    }
    return false;
}

void Runner::runExecChain(const Node& from, int execOutPin, int depth)
{
    if (depth > kMaxDepth) return;
    const Link* l = execLinkFrom(from.id, execOutPin);
    while (l)
    {
        if (++m_steps > kMaxSteps)
        {
            HE_LOG_WARN(HorizonCode, "%s",
                "HorizonCode: execution step limit hit — aborting run");
            return;
        }
        const Node* n = m_graph.findNode(l->dstNode);
        if (!n) return;
        execNode(*n, depth);
        if (n->type == T::Branch || n->type == T::Sequence || n->type == T::ForEach ||
            n->type == T::ForEachSet || n->type == T::ForEachMap ||
            n->type == T::Delay || n->type == T::DoOnce || n->type == T::FlipFlop ||
            n->type == T::SwitchOnEnum || n->type == T::Cast)
            return; // they steer their own exec-outs internally (Delay: later)
        l = execLinkFrom(n->id, pinRanges(*n).execOut0);
    }
}

void Runner::execNode(const Node& n, int depth)
{
    if (depth > kMaxDepth) return;
    switch (n.type)
    {
    case T::Branch:
    {
        const bool cond = evalInput(n, 0, depth + 1).b;
        const PinRanges r = pinRanges(n);
        runExecChain(n, r.execOut0 + (cond ? 0 : 1), depth + 1);
        break;
    }
    case T::Sequence:
    {
        const PinRanges r = pinRanges(n);
        runExecChain(n, r.execOut0 + 0, depth + 1);
        runExecChain(n, r.execOut0 + 1, depth + 1);
        break;
    }
    case T::Cast:
    {
        const Value  v  = evalInput(n, 0, depth + 1);
        // A reference that is 0, dead, or of another class all take Failure —
        // the same three cases hc::castRef folds together in generated code.
        const bool   ok = v.type == P::Ref && v.ref != 0 && m_ctx.isA && m_ctx.isA(v.ref, n.s);
        // Cached like every other exec node's output, so a downstream read on
        // the Success branch gets the reference without re-running the check.
        m_execOutputs[n.id] = { ok ? v : Value::ofRef(0) };
        const PinRanges r = pinRanges(n);
        runExecChain(n, r.execOut0 + (ok ? 0 : 1), depth + 1);
        break;
    }
    case T::SetProperty:
        if (m_ctx.setProperty)
            m_ctx.setProperty(n.elem, n.s, coerce(evalInput(n, 0, depth + 1), n.propType));
        break;
    case T::SetVariable:
        // A function-local writes the innermost frame of its owning function;
        // outside that function the write is dropped. Everything else goes to
        // the instance store (which also creates undeclared names, as before).
        if (const Variable* v = m_graph.findVariable(n.s); v && v->scope != 0)
        {
            if (CallFrame* f = frameFor(v->scope))
                f->locals[n.s] = coerce(evalInput(n, 0, depth + 1), n.propType);
        }
        else if (m_ctx.setVariable)
            m_ctx.setVariable(n.s, coerce(evalInput(n, 0, depth + 1), n.propType));
        break;
    case T::ShowSelf: if (m_ctx.showSelf) m_ctx.showSelf(); break;
    case T::HideSelf: if (m_ctx.hideSelf) m_ctx.hideSelf(); break;
    case T::CreateWidget:
    {
        // The widget id doubles as its runtime reference (widget id == scriptId),
        // so a created widget is a first-class Ref object.
        const int id = m_ctx.createWidget ? m_ctx.createWidget(n.s) : 0;
        m_execOutputs[n.id] = { Value::ofRef((uint32_t)id) }; // cached for the data output
        break;
    }
    case T::ShowWidget:  if (m_ctx.showWidget)    m_ctx.showWidget((int)evalInput(n, 0, depth + 1).ref);    break;
    case T::HideWidget:  if (m_ctx.hideWidget)    m_ctx.hideWidget((int)evalInput(n, 0, depth + 1).ref);    break;
    case T::DestroyWidget: if (m_ctx.destroyWidget) m_ctx.destroyWidget((int)evalInput(n, 0, depth + 1).ref); break;
    case T::CreateObject:
    {
        // Placement is decided by the WIRE, not the value: an unwired pin hands
        // the host nullptr ("leave it where the class authored it"), so a graph
        // that predates these pins spawns exactly where it always did. The
        // locals must outlive the call — hence named, not evalInput(...).v3.
        glm::vec3 pos{ 0.0f }, rot{ 0.0f };
        const bool hasPos = inputLinked(n, 0), hasRot = inputLinked(n, 1);
        if (hasPos) pos = evalInput(n, 0, depth + 1).v3;
        if (hasRot) rot = evalInput(n, 1, depth + 1).v3;
        const uint32_t ref = m_ctx.createObject
            ? m_ctx.createObject(n.s, hasPos ? &pos.x : nullptr, hasRot ? &rot.x : nullptr)
            : 0u;
        if (ref == 0u)
            HE_LOG_ERROR(HorizonCode, "%s",
                ("HorizonCode: Create Object failed — class '" + n.s + "' not found").c_str());
        m_execOutputs[n.id] = { Value::ofRef(ref) }; // cached for the data output
        break;
    }
    case T::DestroyObject: if (m_ctx.destroyObject) m_ctx.destroyObject(evalInput(n, 0, depth + 1).ref); break;
    case T::SetExternal:
        if (m_ctx.setExternal)
            m_ctx.setExternal(evalInput(n, 0, depth + 1).ref, n.s,
                              coerce(evalInput(n, 1, depth + 1), n.propType));
        break;
    case T::BindEvent:
        if (m_ctx.bindEvent)
            m_ctx.bindEvent(evalInput(n, 0, depth + 1).ref, n.s);
        break;
    case T::EmitEvent:
        if (m_ctx.emitEvent)
            m_ctx.emitEvent(n.s, n.hasArg ? coerce(evalInput(n, 0, depth + 1), n.propType) : Value{});
        break;
    case T::CallExternal:
        if (m_ctx.callExternal)
        {
            // dataIn 0 = Target (Ref); 1.. = the callee's parameters.
            std::vector<Value> args(n.params.size());
            for (size_t i = 0; i < n.params.size(); ++i)
                args[i] = coerce(evalInput(n, (int)i + 1, depth + 1), n.params[i].type);
            m_execOutputs[n.id] = m_ctx.callExternal(evalInput(n, 0, depth + 1).ref, n.s, args);
        }
        break;
    case T::FunctionCall:
    {
        const Node* entry = nullptr;
        for (const auto& fn : m_graph.nodes)
            if (fn.type == T::FunctionEntry && fn.s == n.s) { entry = &fn; break; }
        if (!entry)
        {
            // Not in THIS class's graph — so it is inherited, and the instance
            // has to resolve it against the levels this Runner cannot see. The
            // arguments are still evaluated in the caller's context, exactly as
            // the local path does before pushing its frame.
            if (m_ctx.callOwn)
            {
                std::vector<Value> args(n.params.size());
                for (size_t i = 0; i < n.params.size(); ++i)
                    args[i] = coerce(evalInput(n, (int)i, depth + 1), n.params[i].type);
                std::vector<Value> res;
                if (m_ctx.callOwn(n.s, args, &res))
                    m_execOutputs[n.id] = std::move(res);
            }
            break;
        }
        // Build the call frame: evaluate arguments in the CALLER's context (before
        // pushing, so the caller's own params still resolve), seed typed results
        // and the callee's locals at their declared defaults.
        CallFrame frame;
        frame.fnEntryId = entry->id;
        seedLocals(m_graph, entry->id, frame.locals);
        frame.args.resize(n.params.size());
        for (size_t i = 0; i < n.params.size(); ++i)
            frame.args[i] = coerce(evalInput(n, (int)i, depth + 1), n.params[i].type);
        frame.results.resize(n.results.size());
        for (size_t i = 0; i < n.results.size(); ++i) frame.results[i].type = n.results[i].type;
        m_callStack.push_back(std::move(frame));
        runExecChain(*entry, pinRanges(*entry).execOut0, depth + 1);
        // Cache the returned values as this call's data outputs, then pop.
        m_execOutputs[n.id] = std::move(m_callStack.back().results);
        m_callStack.pop_back();
        break;
    }
    case T::FunctionReturn:
        // Write the current invocation's return values (read back by the call).
        if (!m_callStack.empty())
        {
            CallFrame& f = m_callStack.back();
            for (size_t i = 0; i < n.results.size() && i < f.results.size(); ++i)
                f.results[i] = coerce(evalInput(n, (int)i, depth + 1), n.results[i].type);
        }
        break;
    case T::EngineCall:
        // Side-effecting engine call: evaluate the argument pins, dispatch through
        // the registry, and cache the results for downstream data reads (like a
        // FunctionCall). Pure engine calls have no exec pin and never reach here.
        if (m_ctx.callApi)
        {
            std::vector<Value> args(n.params.size());
            for (size_t i = 0; i < n.params.size(); ++i)
                args[i] = coerce(evalInput(n, (int)i, depth + 1), n.params[i].type);
            m_execOutputs[n.id] = m_ctx.callApi(n.s, args);
        }
        break;
    case T::ForEach:
    {
        // Evaluate the array once, then run the Body chain per element with the
        // current Element + Index cached as this node's data outputs (read back
        // by evalData, like a call's results). Done fires after the last element.
        const Value arr = evalInput(n, 0, depth + 1);
        const PinRanges r = pinRanges(n);
        for (size_t i = 0; i < arr.items.size(); ++i)
        {
            m_execOutputs[n.id] = { arr.items[i], Value::ofInt((int)i) };
            runExecChain(n, r.execOut0 + 0, depth + 1);   // Body
        }
        runExecChain(n, r.execOut0 + 1, depth + 1);        // Done
        break;
    }
    case T::ForEachSet:
    {
        // Identical to ForEach — the set's items ARE its iteration order (see
        // ContainerKind). A separate node rather than a mode on ForEach so no
        // graph that already wires a ForEach can change shape underneath it.
        const Value set = evalInput(n, 0, depth + 1);
        const PinRanges r = pinRanges(n);
        for (size_t i = 0; i < set.items.size(); ++i)
        {
            m_execOutputs[n.id] = { set.items[i], Value::ofInt((int)i) };
            runExecChain(n, r.execOut0 + 0, depth + 1);   // Body
        }
        runExecChain(n, r.execOut0 + 1, depth + 1);        // Done
        break;
    }
    case T::ForEachMap:
    {
        const Value map = evalInput(n, 0, depth + 1);
        const PinRanges r = pinRanges(n);
        const size_t count = std::min(map.keys.size(), map.items.size());
        for (size_t i = 0; i < count; ++i)
        {
            m_execOutputs[n.id] = { map.keys[i], map.items[i], Value::ofInt((int)i) };
            runExecChain(n, r.execOut0 + 0, depth + 1);   // Body
        }
        runExecChain(n, r.execOut0 + 1, depth + 1);        // Done
        break;
    }
    case T::Print:
        // Tagged with the PROJECT's language, not with "[Widget] ": Print dates
        // from when HorizonCode drove nothing but widgets, and it now runs in
        // level scripts, classes and the GameInstance too. HE::scriptLogLine is
        // shared with generated C++ (hc::print) and with ScriptApi::log — the
        // one place the prefix is decided, so the three cannot diverge again.
        HE_LOG_INFO(HorizonCode, "%s",
            HE::scriptLogLine(coerce(evalInput(n, 0, depth + 1), P::String).s).c_str());
        break;
    case T::Delay:
        // Latent: hand the continuation to the host scheduler and stop the
        // chain here — Runtime::update resumes from our exec-out later.
        if (m_ctx.scheduleResume)
            m_ctx.scheduleResume(n.id, evalInput(n, 0, depth + 1).f,
                                 coerce(evalInput(n, 1, depth + 1), P::Bool).b);
        break;
    case T::DoOnce:
        // Let the chain through only the first time (per instance; node state
        // persists across runs and resets with reseedVariables).
        if (m_ctx.getNodeState && m_ctx.getNodeState(n.id).b) break;
        if (m_ctx.setNodeState) m_ctx.setNodeState(n.id, Value::ofBool(true));
        runExecChain(n, pinRanges(n).execOut0, depth + 1);
        break;
    case T::FlipFlop:
    {
        // Alternate A/B, starting with A. The IsA data-out reports which side
        // THIS execution took (stored state = the side just taken).
        const bool wasA = m_ctx.getNodeState && m_ctx.getNodeState(n.id).b;
        const bool isA  = !wasA;
        if (m_ctx.setNodeState) m_ctx.setNodeState(n.id, Value::ofBool(isA));
        runExecChain(n, pinRanges(n).execOut0 + (isA ? 0 : 1), depth + 1);
        break;
    }
    case T::SwitchOnEnum:
    {
        // Match the value against the CURRENT definition, then route to the
        // exec-out whose mirrored entry name matches — renumber-safe. No match
        // (or missing def) → the trailing Default.
        const int val = evalInput(n, 0, depth + 1).i;
        const PinRanges r = pinRanges(n);
        int branch = (int)n.params.size();                 // Default
        HE::EnumDef def;
        if (HE::TypeRegistry::instance().getEnum(n.typeName, def))
            if (const HE::EnumEntry* e = def.findValue(val))
                for (size_t i = 0; i < n.params.size(); ++i)
                    if (n.params[i].name == e->name) { branch = (int)i; break; }
        runExecChain(n, r.execOut0 + branch, depth + 1);
        break;
    }
    default: break;
    }
}

Value Runner::evalInput(const Node& n, int dataInIndex, int depth)
{
    const PinRanges r = pinRanges(n);
    const int pin = r.dataIn0 + dataInIndex;
    for (const auto& l : m_graph.links)
    {
        if (l.dstNode != n.id || l.dstPin != pin) continue;
        const Node* src = m_graph.findNode(l.srcNode);
        if (!src) break;
        // Coerce to THIS pin's type: equal types pass through untouched (the
        // common case costs nothing), and a converting wire lands as the value
        // the reader declared it wants.
        return coerce(evalData(*src, l.srcPin - pinRanges(*src).dataOut0, depth),
                      dataPinType(n, true, dataInIndex));
    }
    // Unwired: the pin's inline default (editor-authored) before the type's zero.
    if (auto it = n.pinDefaults.find(dataInIndex); it != n.pinDefaults.end())
        return coerce(it->second, dataPinType(n, true, dataInIndex));
    // The zero of a CONTAINER pin is the empty container of its kind, not a
    // scalar zero — "Map Length of an unwired Map" has to be 0, not garbage, and
    // generated C++ starts such a pin at an empty hc::Map for the same reason.
    PinDesc d{};
    if (dataPinDescOf(n, true, dataInIndex, d) && d.isArray)
    {
        Value v; v.isArray = true; v.container = d.kind(); v.type = d.type;
        if (d.typeName) v.typeName = d.typeName;
        if (v.container == CK::Map)
        {
            v.keyType = d.keyType;
            if (d.keyTypeName) v.keyTypeName = d.keyTypeName;
        }
        return v;
    }
    Value v; v.type = dataPinType(n, true, dataInIndex);
    return v;
}

bool Runner::inputLinked(const Node& n, int dataInIndex) const
{
    const int pin = pinRanges(n).dataIn0 + dataInIndex;
    for (const auto& l : m_graph.links)
        if (l.dstNode == n.id && l.dstPin == pin && m_graph.findNode(l.srcNode))
            return true;
    return false;
}

Value Runner::evalData(const Node& n, int dataOutPin, int depth)
{
    if (depth > kMaxDepth || ++m_steps > kMaxSteps) return {};
    switch (n.type)
    {
    case T::Event:       return coerce(m_eventArg, n.propType);
    // The axis value rides in on the same event argument the host sends; a
    // digital action has no data-out to reach this at all.
    case T::InputAction: return coerce(m_eventArg, n.propType == P::Vec2 ? P::Vec2 : P::Float);
    case T::ConstFloat:  return Value::ofFloat(n.f[0]);
    case T::ConstBool:   return Value::ofBool(n.f[0] != 0.0f);
    case T::ConstInt:    return Value::ofInt((int)n.f[0]);
    case T::ConstString: return Value::ofString(n.s);
    case T::ConstVec2:   return Value::ofVec2({ n.f[0], n.f[1] });
    case T::ConstColor:  return Value::ofColor({ n.f[0], n.f[1], n.f[2], n.f[3] });
    case T::ConstTransform: return Value::ofTransform(n.tpos, n.trot, n.tscl);
    case T::ConstEnum:
    {
        Value v; v.type = P::Enum; v.typeName = n.typeName; v.i = (int)n.f[0];
        return v;
    }
    case T::EnumToInt: return Value::ofInt(evalInput(n, 0, depth + 1).i);
    case T::IntToEnum:
    {
        Value v; v.type = P::Enum; v.typeName = n.typeName;
        v.i = evalInput(n, 0, depth + 1).i;
        return v;
    }
    case T::EnumToString:
    {
        const int val = evalInput(n, 0, depth + 1).i;
        HE::EnumDef def;
        if (HE::TypeRegistry::instance().getEnum(n.typeName, def))
            if (const HE::EnumEntry* e = def.findValue(val))
                return Value::ofString(e->name);
        return Value::ofString("");
    }
    case T::MakeStruct:
    {
        // Field values in the node's mirrored order — which syncTypeSignatures
        // keeps equal to DEFINITION order, the layout every consumer resolves
        // against. Unwired fields fall back to their declared defaults (the
        // whole-struct default is built first, then wired pins overwrite).
        Value v = HE::TypeRegistry::instance().makeDefaultValue(n.typeName);
        v.typeName = n.typeName;
        if (v.items.size() < n.params.size()) v.items.resize(n.params.size());
        const PinRanges r = pinRanges(n);
        for (size_t i = 0; i < n.params.size(); ++i)
        {
            // Only overwrite fields that are actually wired or carry an inline
            // default — an untouched pin keeps the definition's default.
            const int pin = r.dataIn0 + (int)i;
            bool wired = false;
            for (const auto& l : m_graph.links)
                if (l.dstNode == n.id && l.dstPin == pin) { wired = true; break; }
            if (wired || n.pinDefaults.count((int)i))
                v.items[i] = coerce(evalInput(n, (int)i, depth + 1), n.params[i].type);
        }
        return v;
    }
    case T::BreakStruct:
    {
        // Resolve the requested field BY NAME against the current definition —
        // the value's items are in def order, so a def edit mid-session can't
        // silently hand back the wrong field.
        if (dataOutPin < 0 || dataOutPin >= (int)n.params.size()) return {};
        const Value v = evalInput(n, 0, depth + 1);
        HE::StructDef def;
        if (HE::TypeRegistry::instance().getStruct(n.typeName, def))
        {
            for (size_t i = 0; i < def.fields.size(); ++i)
                if (def.fields[i].name == n.params[dataOutPin].name)
                    return i < v.items.size()
                        ? coerce(v.items[i], n.params[dataOutPin].type)
                        : coerce(Value{}, n.params[dataOutPin].type);
        }
        // Def missing: trust the mirrored order (best effort).
        return dataOutPin < (int)v.items.size()
            ? coerce(v.items[dataOutPin], n.params[dataOutPin].type)
            : coerce(Value{}, n.params[dataOutPin].type);
    }
    case T::GetStructField:
    {
        // Resolve BY NAME against the current definition (like BreakStruct), so
        // a field inserted since this graph was authored can't shift the read.
        if (n.params.empty()) return {};
        const Value v = evalInput(n, 0, depth + 1);
        HE::StructDef def;
        if (HE::TypeRegistry::instance().getStruct(n.typeName, def))
        {
            for (size_t i = 0; i < def.fields.size(); ++i)
                if (def.fields[i].name == n.params[0].name)
                    return i < v.items.size()
                        ? coerce(v.items[i], n.params[0].type)
                        : coerce(Value{}, n.params[0].type);
        }
        return coerce(Value{}, n.params[0].type);          // def missing → typed zero
    }
    case T::SetStructField:
    {
        Value v = evalInput(n, 0, depth + 1);              // a copy (pure)
        if (n.params.empty()) return v;
        if (v.type != P::Struct || v.typeName != n.typeName)
        {
            v = HE::TypeRegistry::instance().makeDefaultValue(n.typeName);
            v.typeName = n.typeName;
        }
        HE::StructDef def;
        if (HE::TypeRegistry::instance().getStruct(n.typeName, def))
        {
            for (size_t i = 0; i < def.fields.size(); ++i)
                if (def.fields[i].name == n.params[0].name)
                {
                    if (v.items.size() < def.fields.size()) v.items.resize(def.fields.size());
                    v.items[i] = coerce(evalInput(n, 1, depth + 1), n.params[0].type);
                    break;
                }
        }
        return v;
    }
    case T::ArrayMake:
    {
        Value r; r.isArray = true; r.type = n.propType; return r;   // empty array of the element type
    }
    case T::ArrayLength:
        return Value::ofInt((int)evalInput(n, 0, depth + 1).items.size());
    case T::ArrayGet:
    {
        const Value arr = evalInput(n, 0, depth + 1);
        const int idx = evalInput(n, 1, depth + 1).i;
        if (idx >= 0 && idx < (int)arr.items.size()) return arr.items[idx];
        HE_LOG_WARN(HorizonCode, "%s",
            ("HorizonCode: Array Get index " + std::to_string(idx) + " out of range (size " +
             std::to_string(arr.items.size()) + ")").c_str());
        Value def; def.type = n.propType; return def;   // out of range → element default
    }
    case T::ArrayAdd:
    {
        Value arr = evalInput(n, 0, depth + 1);          // a copy (pure: returns a new array)
        arr.isArray = true; arr.type = n.propType;
        arr.items.push_back(coerce(evalInput(n, 1, depth + 1), n.propType));
        return arr;
    }
    case T::ArraySet:
    {
        Value arr = evalInput(n, 0, depth + 1);
        arr.isArray = true; arr.type = n.propType;
        const int idx = evalInput(n, 1, depth + 1).i;
        if (idx >= 0 && idx < (int)arr.items.size())
            arr.items[idx] = coerce(evalInput(n, 2, depth + 1), n.propType);
        return arr;                                       // out of range → unchanged copy
    }
    case T::ArrayInsert:
    {
        Value arr = evalInput(n, 0, depth + 1);
        arr.isArray = true; arr.type = n.propType;
        int idx = evalInput(n, 1, depth + 1).i;
        if (idx < 0) idx = 0;
        if (idx > (int)arr.items.size()) idx = (int)arr.items.size();  // clamp → append
        arr.items.insert(arr.items.begin() + idx, coerce(evalInput(n, 2, depth + 1), n.propType));
        return arr;
    }
    case T::ArrayRemove:
    {
        Value arr = evalInput(n, 0, depth + 1);
        arr.isArray = true; arr.type = n.propType;
        const int idx = evalInput(n, 1, depth + 1).i;
        if (idx >= 0 && idx < (int)arr.items.size())
            arr.items.erase(arr.items.begin() + idx);
        return arr;                                       // out of range → unchanged copy
    }
    case T::ArrayContains:
    {
        const Value arr = evalInput(n, 0, depth + 1);
        const Value key = coerce(evalInput(n, 1, depth + 1), n.propType);
        for (const Value& v : arr.items)
            if (scalarValueEquals(v, key, n.propType)) return Value::ofBool(true);
        return Value::ofBool(false);
    }
    case T::ArrayIndexOf:
    {
        const Value arr = evalInput(n, 0, depth + 1);
        const Value key = coerce(evalInput(n, 1, depth + 1), n.propType);
        for (size_t i = 0; i < arr.items.size(); ++i)
            if (scalarValueEquals(arr.items[i], key, n.propType)) return Value::ofInt((int)i);
        return Value::ofInt(-1);                           // not found
    }

    // ── Set<T> ───────────────────────────────────────────────────────────────
    // Pure and copy-semantic like the Array nodes: each op re-stamps the copy's
    // kind and element type, so a set arriving from a dynamic boundary (a script
    // handing over a list) still leaves as a well-formed set.
    case T::SetMake:
        return Value::ofSet(n.propType, n.typeName);
    case T::SetLength:
        return Value::ofInt((int)evalInput(n, 0, depth + 1).items.size());
    case T::SetClear:
        return Value::ofSet(n.propType, n.typeName);
    case T::SetAdd:
    {
        Value set = evalInput(n, 0, depth + 1);
        set.isArray = true; set.container = CK::Set; set.type = n.propType;
        const Value v = coerce(evalInput(n, 1, depth + 1), n.propType);
        // Already present → unchanged, and specifically NOT moved to the back:
        // insertion order means the order of FIRST insertion.
        if (indexOfValue(set.items, v, n.propType) < 0) set.items.push_back(v);
        return set;
    }
    case T::SetRemove:
    {
        Value set = evalInput(n, 0, depth + 1);
        set.isArray = true; set.container = CK::Set; set.type = n.propType;
        const Value v = coerce(evalInput(n, 1, depth + 1), n.propType);
        if (const int at = indexOfValue(set.items, v, n.propType); at >= 0)
            set.items.erase(set.items.begin() + at);       // the rest keeps its order
        return set;
    }
    case T::SetContains:
    {
        const Value set = evalInput(n, 0, depth + 1);
        const Value v = coerce(evalInput(n, 1, depth + 1), n.propType);
        return Value::ofBool(indexOfValue(set.items, v, n.propType) >= 0);
    }
    case T::SetToArray:
    {
        Value arr = evalInput(n, 0, depth + 1);
        arr.isArray = true; arr.container = CK::Array; arr.type = n.propType;
        arr.keys.clear();
        return arr;                                        // items are already in order
    }
    case T::SetFromArray:
    {
        Value set = evalInput(n, 0, depth + 1);
        set.isArray = true; set.container = CK::Set; set.type = n.propType;
        set.typeName = n.typeName;
        set.keys.clear();
        // An array MAY hold duplicates and a set may not, so something has to
        // go. dedupeSetItems keeps the FIRST occurrence in place — the same rule
        // a chain of Set Add nodes would produce, which is what makes the two
        // ways of building a set from a list agree.
        dedupeSetItems(set);
        return set;
    }
    case T::SetUnion:
    case T::SetIntersect:
    case T::SetDifference:
    {
        // A's ORDER is the result's order in all three: A is what the reader
        // wired first, and with an insertion-ordered container the order is a
        // promised property, not an incidental one. Union appends B's extras
        // behind it, exactly as folding Set Add over B would.
        const Value a = evalInput(n, 0, depth + 1);
        const Value b = evalInput(n, 1, depth + 1);
        Value out = Value::ofSet(n.propType, n.typeName);
        for (const Value& v : a.items)
        {
            const bool inB = indexOfValue(b.items, v, n.propType) >= 0;
            const bool want = n.type == T::SetUnion ? true
                            : n.type == T::SetIntersect ? inB : !inB;
            // A hand-fed input (a script's list) may carry duplicates; the
            // membership test keeps the OUTPUT a set regardless.
            if (want && indexOfValue(out.items, v, n.propType) < 0) out.items.push_back(v);
        }
        if (n.type == T::SetUnion)
            for (const Value& v : b.items)
                if (indexOfValue(out.items, v, n.propType) < 0) out.items.push_back(v);
        return out;
    }

    // ── Map<K,V> ─────────────────────────────────────────────────────────────
    case T::MapMake:
    case T::MapClear:
        return Value::ofMap(n.keyType, n.propType, n.keyTypeName, n.typeName);
    case T::MapLength:
        return Value::ofInt((int)evalInput(n, 0, depth + 1).items.size());
    case T::MapSet:
    {
        Value map = evalInput(n, 0, depth + 1);
        map.isArray = true; map.container = CK::Map; map.type = n.propType;
        map.keyType = n.keyType; map.keyTypeName = n.keyTypeName;
        const Value k = coerce(evalInput(n, 1, depth + 1), n.keyType);
        const Value v = coerce(evalInput(n, 2, depth + 1), n.propType);
        if (const int at = indexOfValue(map.keys, k, n.keyType); at >= 0)
            map.items[(size_t)at] = v;                     // update IN PLACE, key keeps its slot
        else { map.keys.push_back(k); map.items.push_back(v); }
        return map;
    }
    case T::MapRemove:
    {
        Value map = evalInput(n, 0, depth + 1);
        map.isArray = true; map.container = CK::Map; map.type = n.propType;
        map.keyType = n.keyType; map.keyTypeName = n.keyTypeName;
        const Value k = coerce(evalInput(n, 1, depth + 1), n.keyType);
        if (const int at = indexOfValue(map.keys, k, n.keyType); at >= 0)
        {
            map.keys.erase(map.keys.begin() + at);
            if ((size_t)at < map.items.size()) map.items.erase(map.items.begin() + at);
        }
        return map;
    }
    case T::MapContains:
    {
        const Value map = evalInput(n, 0, depth + 1);
        const Value k = coerce(evalInput(n, 1, depth + 1), n.keyType);
        return Value::ofBool(indexOfValue(map.keys, k, n.keyType) >= 0);
    }
    case T::MapGet:
    {
        const Value map = evalInput(n, 0, depth + 1);
        const Value k = coerce(evalInput(n, 1, depth + 1), n.keyType);
        const int at = indexOfValue(map.keys, k, n.keyType);
        // A miss is NOT a warning like Array Get's: the Default pin exists
        // precisely so that "no entry yet" is an ordinary, expected answer.
        if (at >= 0 && (size_t)at < map.items.size()) return map.items[(size_t)at];
        return coerce(evalInput(n, 2, depth + 1), n.propType);
    }
    case T::MapKeys:
    {
        const Value map = evalInput(n, 0, depth + 1);
        Value r = Value::ofArray(n.keyType, n.keyTypeName);
        r.items = map.keys;
        r.items.resize(std::min(map.keys.size(), map.items.size()));
        return r;
    }
    case T::MapValues:
    {
        const Value map = evalInput(n, 0, depth + 1);
        Value r = Value::ofArray(n.propType, n.typeName);
        // Truncated to the pairs that exist, so Keys and Values are always
        // index-parallel even if a hand-edited asset was not.
        r.items = map.items;
        r.items.resize(std::min(map.keys.size(), map.items.size()));
        return r;
    }
    case T::MapBreak:
    {
        // Map Keys and Map Values on one node — including their truncation, so
        // the two nodes and this one cannot answer differently.
        const Value map = evalInput(n, 0, depth + 1);
        const size_t pairs = std::min(map.keys.size(), map.items.size());
        // Pin 1 is Values, anything else Keys — spelled the way the codegen
        // spells it, so even an out-of-range read cannot differ between them.
        const bool wantKeys = dataOutPin != 1;
        Value r = wantKeys ? Value::ofArray(n.keyType, n.keyTypeName)
                           : Value::ofArray(n.propType, n.typeName);
        r.items = wantKeys ? map.keys : map.items;
        r.items.resize(pairs);
        return r;
    }
    case T::MapFromArrays:
    {
        const Value ks = evalInput(n, 0, depth + 1);
        const Value vs = evalInput(n, 1, depth + 1);
        Value map = Value::ofMap(n.keyType, n.propType, n.keyTypeName, n.typeName);
        // UNEQUAL LENGTHS: the shorter one wins. A missing value would have to be
        // invented (the type's zero), and a map that silently holds a key nobody
        // gave a value to is worse than a pair that never appears — Map Keys and
        // Map Values already truncate to the pairs that exist, for the same
        // reason, and this is the node that undoes them.
        const auto pairs = (std::ptrdiff_t)std::min(ks.items.size(), vs.items.size());
        map.keys.assign(ks.items.begin(), ks.items.begin() + pairs);
        map.items.assign(vs.items.begin(), vs.items.begin() + pairs);
        // A REPEATED KEY: the house rule is Map Set's — the key keeps its first
        // position and takes the LAST value written to it — and dedupeMapKeys is
        // where that rule already lives, so this is not a second copy of it.
        dedupeMapKeys(map);
        return map;
    }
    case T::MapFindByValue:
    {
        // The reverse lookup: keys are unique and values are not, so the answer
        // is the FIRST pair in iteration order. Same equality as Map Get's keys
        // and Array Contains (scalarValueEquals) — Struct values compare unequal
        // there, so a map of structs never finds anything, which the tooltip says.
        const Value map = evalInput(n, 0, depth + 1);
        const Value want = coerce(evalInput(n, 1, depth + 1), n.propType);
        const size_t pairs = std::min(map.keys.size(), map.items.size());
        for (size_t i = 0; i < pairs; ++i)
            if (scalarValueEquals(map.items[i], want, n.propType))
                return dataOutPin == 1 ? Value::ofBool(true) : map.keys[i];
        // A miss answers with the KEY TYPE's zero, which is what an unwired key
        // pin reads — never a half-filled Value of some other type.
        return dataOutPin == 1 ? Value::ofBool(false) : coerce(Value{}, n.keyType);
    }
    case T::MapRemoveByValue:
    {
        // Removes EVERY match, unlike Map Remove (one key, one pair): a value
        // can sit under any number of keys, and "remove the first one" would be
        // a rule nobody could predict the effect of. Survivors keep their order.
        const Value map = evalInput(n, 0, depth + 1);
        const Value want = coerce(evalInput(n, 1, depth + 1), n.propType);
        Value out = Value::ofMap(n.keyType, n.propType, n.keyTypeName, n.typeName);
        const size_t pairs = std::min(map.keys.size(), map.items.size());
        for (size_t i = 0; i < pairs; ++i)
            if (!scalarValueEquals(map.items[i], want, n.propType))
            { out.keys.push_back(map.keys[i]); out.items.push_back(map.items[i]); }
        return out;
    }

    case T::GetProperty:
    {
        Value v = m_ctx.getProperty ? m_ctx.getProperty(n.elem, n.s) : Value{};
        return coerce(v, n.propType);
    }
    case T::GetVariable:
    {
        // Function-local: read the innermost frame of the owning function;
        // no frame (node ran outside its function) → the type's default.
        if (const Variable* var = m_graph.findVariable(n.s); var && var->scope != 0)
        {
            if (CallFrame* f = frameFor(var->scope))
                if (auto it = f->locals.find(n.s); it != f->locals.end())
                    return coerce(it->second, n.propType);
            return coerce(Value{}, n.propType);
        }
        Value v = m_ctx.getVariable ? m_ctx.getVariable(n.s) : Value{};
        return coerce(v, n.propType);
    }
    // Set-node pass-through: the value output re-reads the Value input (like a
    // C++ assignment expression returning the assigned value). No side effect.
    case T::SetVariable:
    case T::SetProperty:
        return coerce(evalInput(n, 0, depth + 1), n.propType);
    case T::SetExternal:
        return coerce(evalInput(n, 1, depth + 1), n.propType); // dataIn 1 = Value (0 = Target)
    case T::GetGameInstance: return m_ctx.getGameInstance ? m_ctx.getGameInstance() : Value::ofRef(0);
    case T::GetSelf:         return m_ctx.getSelf ? m_ctx.getSelf() : Value::ofRef(0);
    case T::CreateWidget:
    case T::CreateObject:
    {
        // Return the ref produced when this node ran (don't create again).
        auto it = m_execOutputs.find(n.id);
        return (it != m_execOutputs.end() && !it->second.empty()) ? it->second[0] : Value::ofRef(0);
    }
    case T::FunctionEntry:
    {
        // A function's input parameter: read it from THIS function's call
        // frame, not blindly from the top of the stack — with nested calls
        // (A calls B) a data wire evaluated inside B that reaches A's entry
        // must yield A's arguments, not B's. Same lookup the locals use.
        if (const CallFrame* f = frameFor(n.id);
            f && dataOutPin >= 0 && dataOutPin < (int)f->args.size())
            return f->args[dataOutPin];
        return {};
    }
    case T::FunctionCall:
    case T::CallExternal:
    case T::ForEach:    // Element + Index of the current iteration (cached per pass)
    case T::ForEachSet: // ditto
    case T::ForEachMap: // Key + Value + Index of the current pair
    {
        // A (local or cross-instance) call's return value: read the cached results.
        auto it = m_execOutputs.find(n.id);
        if (it != m_execOutputs.end() && dataOutPin >= 0 && dataOutPin < (int)it->second.size())
            return it->second[dataOutPin];
        return {};
    }
    case T::EngineCall:
    {
        // Exec engine call: return the value cached when the node ran. Pure engine
        // call (no exec pin): evaluate the inputs and dispatch now — re-evaluatable
        // because it has no side effect.
        if (n.hasArg)
        {
            auto it = m_execOutputs.find(n.id);
            if (it != m_execOutputs.end() && dataOutPin >= 0 && dataOutPin < (int)it->second.size())
                return it->second[dataOutPin];
            return {};
        }
        if (m_ctx.callApi)
        {
            std::vector<Value> args(n.params.size());
            for (size_t i = 0; i < n.params.size(); ++i)
                args[i] = coerce(evalInput(n, (int)i, depth + 1), n.params[i].type);
            std::vector<Value> res = m_ctx.callApi(n.s, args);
            if (dataOutPin >= 0 && dataOutPin < (int)res.size()) return res[dataOutPin];
        }
        return {};
    }
    case T::GetExternal:
    {
        Value v = m_ctx.getExternal ? m_ctx.getExternal(evalInput(n, 0, depth + 1).ref, n.s) : Value{};
        return coerce(v, n.propType);
    }
    case T::IsValid:
        return Value::ofBool(m_ctx.isValid && m_ctx.isValid(evalInput(n, 0, depth + 1).ref));
    case T::Cast:
    {
        // The cast reference the node cached when it ran. Reading it without
        // having run the node — which only a wire off the Failure branch, or a
        // pure read from elsewhere, can do — yields Ref 0, the same "no object"
        // an outright failure produces.
        auto it = m_execOutputs.find(n.id);
        if (it != m_execOutputs.end() && !it->second.empty()) return it->second[0];
        return Value::ofRef(0);
    }
    case T::FlipFlop:
        // Which side the last execution took (A = true); false before any run.
        return Value::ofBool(m_ctx.getNodeState ? m_ctx.getNodeState(n.id).b : false);
    case T::MakeVector2:
        return Value::ofVec2({ evalInput(n, 0, depth + 1).f, evalInput(n, 1, depth + 1).f });
    case T::MakeVector3:
        return Value::ofVec3({ evalInput(n, 0, depth + 1).f, evalInput(n, 1, depth + 1).f,
                               evalInput(n, 2, depth + 1).f });
    case T::MakeVector4:
        return Value::ofVec4({ evalInput(n, 0, depth + 1).f, evalInput(n, 1, depth + 1).f,
                               evalInput(n, 2, depth + 1).f, evalInput(n, 3, depth + 1).f });
    case T::BreakVector2:
    {
        const glm::vec2 v = evalInput(n, 0, depth + 1).v2;
        return Value::ofFloat(dataOutPin == 1 ? v.y : v.x);
    }
    case T::BreakVector3:
    {
        const glm::vec3 v = evalInput(n, 0, depth + 1).v3;
        switch (dataOutPin)
        {
            case 1:  return Value::ofFloat(v.y);
            case 2:  return Value::ofFloat(v.z);
            default: return Value::ofFloat(v.x);
        }
    }
    case T::BreakVector4:
    {
        const glm::vec4 v = evalInput(n, 0, depth + 1).v4;
        switch (dataOutPin)
        {
            case 1:  return Value::ofFloat(v.y);
            case 2:  return Value::ofFloat(v.z);
            case 3:  return Value::ofFloat(v.w);
            default: return Value::ofFloat(v.x);
        }
    }
    case T::Add:      return Value::ofFloat(evalInput(n, 0, depth + 1).f + evalInput(n, 1, depth + 1).f);
    case T::Subtract: return Value::ofFloat(evalInput(n, 0, depth + 1).f - evalInput(n, 1, depth + 1).f);
    case T::Multiply: return Value::ofFloat(evalInput(n, 0, depth + 1).f * evalInput(n, 1, depth + 1).f);
    case T::Divide:
    {
        const float b = evalInput(n, 1, depth + 1).f;
        return Value::ofFloat(b != 0.0f ? evalInput(n, 0, depth + 1).f / b : 0.0f);
    }
    case T::Greater:  return Value::ofBool(evalInput(n, 0, depth + 1).f >  evalInput(n, 1, depth + 1).f);
    case T::Less:     return Value::ofBool(evalInput(n, 0, depth + 1).f <  evalInput(n, 1, depth + 1).f);
    case T::Equals:   return Value::ofBool(std::fabs(evalInput(n, 0, depth + 1).f - evalInput(n, 1, depth + 1).f) < 1e-6f);
    case T::And:      return Value::ofBool(evalInput(n, 0, depth + 1).b && evalInput(n, 1, depth + 1).b);
    case T::Or:       return Value::ofBool(evalInput(n, 0, depth + 1).b || evalInput(n, 1, depth + 1).b);
    case T::Not:      return Value::ofBool(!evalInput(n, 0, depth + 1).b);
    case T::Concat:   return Value::ofString(evalInput(n, 0, depth + 1).s + evalInput(n, 1, depth + 1).s);
    case T::ToString:
    {
        char buf[48];
        std::snprintf(buf, sizeof buf, "%g", evalInput(n, 0, depth + 1).f);
        return Value::ofString(buf);
    }
    default:
        (void)dataOutPin;
        return {};
    }
}

} // namespace HorizonCode
