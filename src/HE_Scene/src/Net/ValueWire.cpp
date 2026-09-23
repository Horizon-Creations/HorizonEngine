#include "HorizonScene/Net/ValueWire.h"

#include <cstring>

namespace HE::Net::Game {

using HorizonCode::PinType;
using HorizonCode::Value;
using CK = HorizonCode::ContainerKind;

namespace {

// The last PinType. A byte off the wire above this names no type, and reading
// it as one would index past every switch in the engine.
constexpr std::uint8_t kMaxPinType = static_cast<std::uint8_t>(PinType::Vec4);

// Recursion bound. A struct whose field is a struct is ordinary; a stream that
// claims a thousand levels of it is not, and the reader would recurse until the
// stack ran out — the one way a malformed datagram could still take the process
// down after every length is checked.
constexpr int kMaxDepth = 16;

bool writeValueAt(BitWriter& w, const Value& v, int depth);
bool readValueAt(BitReader& r, Value& out, int depth);

// The scalar payload, without the type/flags header. Split out because a
// container writes the header once and then only payloads.
bool writeScalarPayload(BitWriter& w, const Value& v, PinType t, int depth)
{
	switch (t)
	{
		case PinType::Bool:   w.writeBool(v.b); return true;
		case PinType::Int:    w.writeUInt32(static_cast<std::uint32_t>(v.i)); return true;
		case PinType::Float:  w.writeFloat(v.f); return true;
		case PinType::String: w.writeString(v.s); return true;
		case PinType::Vec2:   w.writeFloat(v.v2.x); w.writeFloat(v.v2.y); return true;
		case PinType::Vec3:   w.writeFloat(v.v3.x); w.writeFloat(v.v3.y); w.writeFloat(v.v3.z);
		                      return true;
		case PinType::Vec4:   w.writeFloat(v.v4.x); w.writeFloat(v.v4.y);
		                      w.writeFloat(v.v4.z); w.writeFloat(v.v4.w); return true;
		case PinType::Color:  w.writeFloat(v.col.x); w.writeFloat(v.col.y);
		                      w.writeFloat(v.col.z); w.writeFloat(v.col.w); return true;
		case PinType::Transform:
			w.writeFloat(v.tpos.x); w.writeFloat(v.tpos.y); w.writeFloat(v.tpos.z);
			w.writeFloat(v.trot.x); w.writeFloat(v.trot.y); w.writeFloat(v.trot.z);
			w.writeFloat(v.tscl.x); w.writeFloat(v.tscl.y); w.writeFloat(v.tscl.z);
			return true;
		case PinType::Enum:
			// The definition's path travels with EVERY entry rather than once in
			// the table. It is a handful of bytes on a value that changes rarely,
			// and it is what makes an enum delta self-describing: the alternative
			// is a receiver that resolves the entry against whatever definition
			// its table happened to name, which is wrong in exactly the case that
			// matters (two builds, one renamed asset).
			w.writeString(v.typeName);
			w.writeUInt32(static_cast<std::uint32_t>(v.i));
			return true;
		case PinType::Struct:
		{
			w.writeString(v.typeName);
			// Definition order, which is the order the fields sit in `items`
			// (HorizonCode.h: "never keyed by position in persisted JSON" applies
			// to the ASSET; in memory the vector IS definition order, and both
			// ends resolved it against the same TypeRegistry).
			if (v.items.size() > 0xFFFFu) return false;
			w.writeUInt16(static_cast<std::uint16_t>(v.items.size()));
			for (const Value& f : v.items)
				if (!writeValueAt(w, f, depth + 1)) return false;
			return true;
		}
		case PinType::Ref:
		case PinType::Exec:
		default:
			return false;   // see the header: a Ref names nothing over there
	}
}

bool readScalarPayload(BitReader& r, Value& out, PinType t, int depth)
{
	out.type = t;
	switch (t)
	{
		case PinType::Bool:   return r.readBool(out.b);
		case PinType::Int:
		{
			std::uint32_t u = 0;
			if (!r.readUInt32(u)) return false;
			out.i = static_cast<int>(u);
			return true;
		}
		case PinType::Float:  return r.readFloat(out.f);
		case PinType::String: return r.readString(out.s);
		case PinType::Vec2:   return r.readFloat(out.v2.x) && r.readFloat(out.v2.y);
		case PinType::Vec3:   return r.readFloat(out.v3.x) && r.readFloat(out.v3.y) &&
		                             r.readFloat(out.v3.z);
		case PinType::Vec4:   return r.readFloat(out.v4.x) && r.readFloat(out.v4.y) &&
		                             r.readFloat(out.v4.z) && r.readFloat(out.v4.w);
		case PinType::Color:  return r.readFloat(out.col.x) && r.readFloat(out.col.y) &&
		                             r.readFloat(out.col.z) && r.readFloat(out.col.w);
		case PinType::Transform:
			return r.readFloat(out.tpos.x) && r.readFloat(out.tpos.y) && r.readFloat(out.tpos.z) &&
			       r.readFloat(out.trot.x) && r.readFloat(out.trot.y) && r.readFloat(out.trot.z) &&
			       r.readFloat(out.tscl.x) && r.readFloat(out.tscl.y) && r.readFloat(out.tscl.z);
		case PinType::Enum:
		{
			std::uint32_t u = 0;
			if (!r.readString(out.typeName) || !r.readUInt32(u)) return false;
			out.i = static_cast<int>(u);
			return true;
		}
		case PinType::Struct:
		{
			std::uint16_t count = 0;
			if (!r.readString(out.typeName) || !r.readUInt16(count)) return false;
			out.items.clear();
			out.items.reserve(count);
			for (std::uint16_t i = 0; i < count; ++i)
			{
				Value f;
				if (!readValueAt(r, f, depth + 1)) return false;
				out.items.push_back(std::move(f));
			}
			return true;
		}
		case PinType::Ref:
		case PinType::Exec:
		default:
			return false;
	}
}

bool writeValueAt(BitWriter& w, const Value& v, int depth)
{
	if (depth > kMaxDepth) return false;
	if (!isReplicableType(v.type)) return false;

	const CK kind = v.kind();
	if (kind == CK::Map && !isReplicableType(v.keyType)) return false;

	w.writeByte(static_cast<std::uint8_t>(v.type));
	// flags: bit0 = is a container at all, bits1..2 = which one. Two bits and
	// not "one byte per kind" because that is the shape the plan wrote down and
	// because the remaining five bits are the only room a later version has.
	const std::uint8_t flags =
		static_cast<std::uint8_t>((kind != CK::None ? 1u : 0u) |
		                          (static_cast<std::uint8_t>(kind) << 1));
	w.writeByte(flags);

	if (kind == CK::None)
		return writeScalarPayload(w, v, v.type, depth);

	if (kind == CK::Map)
	{
		w.writeByte(static_cast<std::uint8_t>(v.keyType));
		w.writeString(v.keyTypeName);
	}
	// Enum/Struct elements name their definition per element (writeScalarPayload),
	// so the container header needs nothing beyond the count.
	const std::size_t count = v.items.size();
	if (count > 0xFFFFu) return false;
	if (kind == CK::Map && v.keys.size() != count) return false;
	w.writeUInt16(static_cast<std::uint16_t>(count));
	for (std::size_t i = 0; i < count; ++i)
	{
		if (kind == CK::Map && !writeValueAt(w, v.keys[i], depth + 1)) return false;
		if (!writeScalarPayload(w, v.items[i], v.type, depth)) return false;
	}
	return true;
}

bool readValueAt(BitReader& r, Value& out, int depth)
{
	if (depth > kMaxDepth) return false;

	std::uint8_t typeByte = 0, flags = 0;
	if (!r.readByte(typeByte) || !r.readByte(flags)) return false;
	if (typeByte > kMaxPinType) return false;
	const auto type = static_cast<PinType>(typeByte);
	if (!isReplicableType(type)) return false;

	out = Value{};
	out.type = type;

	if ((flags & 0x1u) == 0)
		return readScalarPayload(r, out, type, depth);

	const auto kind = static_cast<CK>((flags >> 1) & 0x3u);
	if (kind == CK::None) return false;          // "container" with no kind: malformed
	out.isArray   = true;
	out.container = kind;

	if (kind == CK::Map)
	{
		std::uint8_t keyByte = 0;
		if (!r.readByte(keyByte) || keyByte > kMaxPinType) return false;
		out.keyType = static_cast<PinType>(keyByte);
		if (!isReplicableType(out.keyType)) return false;
		if (!r.readString(out.keyTypeName)) return false;
	}

	std::uint16_t count = 0;
	if (!r.readUInt16(count)) return false;
	out.items.reserve(count);
	if (kind == CK::Map) out.keys.reserve(count);
	for (std::uint16_t i = 0; i < count; ++i)
	{
		if (kind == CK::Map)
		{
			Value k;
			if (!readValueAt(r, k, depth + 1)) return false;
			out.keys.push_back(std::move(k));
		}
		Value e;
		if (!readScalarPayload(r, e, type, depth)) return false;
		out.items.push_back(std::move(e));
	}
	// The element type's definition name, lifted off the first element so a
	// container value looks the way an authored one does (Value::ofArray takes
	// it). An empty container has none, and neither does an authored empty one.
	if ((type == PinType::Enum || type == PinType::Struct) && !out.items.empty())
		out.typeName = out.items.front().typeName;
	return true;
}

} // namespace

bool isReplicableType(PinType t)
{
	return t != PinType::Ref && t != PinType::Exec;
}

const char* replicationRefusalReason(PinType t)
{
	if (t == PinType::Ref)
		return "An Object reference is a handle into THIS machine's memory and means "
		       "nothing on another one. Replicate what identifies the object instead "
		       "(a name, an id) and look it up on the other side.";
	if (t == PinType::Exec)
		return "Exec is not a value.";
	return "";
}

bool writeValue(BitWriter& w, const Value& v) { return writeValueAt(w, v, 0); }
bool readValue(BitReader& r, Value& out)      { return readValueAt(r, out, 0); }

bool valueTypesMatch(const Value& a, const Value& b)
{
	if (a.type != b.type) return false;
	if (a.kind() != b.kind()) return false;
	if (a.kind() == CK::Map && a.keyType != b.keyType) return false;
	// Enum and Struct are only the same property when they name the same
	// definition: two Structs of different shapes have nothing in common but
	// the word.
	if ((a.type == PinType::Enum || a.type == PinType::Struct) &&
	    !a.typeName.empty() && !b.typeName.empty() && a.typeName != b.typeName)
		return false;
	return true;
}

bool valuesEqual(const Value& a, const Value& b)
{
	if (a.type != b.type) return false;
	const CK kind = a.kind();
	if (kind != b.kind()) return false;

	if (kind == CK::None)
	{
		if (a.type == PinType::Struct)
		{
			// scalarValueEquals has no Struct case, which is the whole reason
			// this function exists (see the header).
			if (a.typeName != b.typeName) return false;
			if (a.items.size() != b.items.size()) return false;
			for (std::size_t i = 0; i < a.items.size(); ++i)
				if (!valuesEqual(a.items[i], b.items[i])) return false;
			return true;
		}
		if (a.type == PinType::Enum && a.typeName != b.typeName) return false;
		return HorizonCode::scalarValueEquals(a, b, a.type);
	}

	if (a.items.size() != b.items.size()) return false;
	if (kind == CK::Map)
	{
		if (a.keyType != b.keyType) return false;
		if (a.keys.size() != b.keys.size()) return false;
		// Position by position, NOT set-wise: iteration order is insertion order
		// and it is part of a map's observable value here (the containers plan's
		// §1.2), so two maps holding the same pairs in a different order really
		// are different and really do need to replicate.
		for (std::size_t i = 0; i < a.keys.size(); ++i)
			if (!valuesEqual(a.keys[i], b.keys[i])) return false;
	}
	for (std::size_t i = 0; i < a.items.size(); ++i)
		if (!valuesEqual(a.items[i], b.items[i])) return false;
	return true;
}

} // namespace HE::Net::Game
