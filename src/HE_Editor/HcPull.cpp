#include "HcPull.h"

#include <Types/TypeRegistry.h>

namespace HcPull
{
namespace
{
	using HorizonCode::ContainerKind;
	using HorizonCode::PinType;
	using HorizonCode::Variable;

	// The path separator of the status line and the tooltip. ASCII on purpose:
	// the editor font carries no arrows or guillemets (headless-imgui notes).
	constexpr const char* kSep = " > ";

	std::string typeWord(PinType t, ContainerKind k, const std::string& typeName)
	{
		static const char* kNames[] = { "Exec", "Float", "Bool", "Int", "String", "Vec2", "Color",
		                                "Object", "Transform", "Enum", "Struct", "Vec3", "Vec4",
		                                "Double" };
		const size_t i = (size_t)t;
		std::string w = i < sizeof(kNames) / sizeof(kNames[0]) ? kNames[i] : "?";
		if ((t == PinType::Enum || t == PinType::Struct) && !typeName.empty())
		{
			std::string stem = typeName;
			if (const size_t s = stem.find_last_of("/\\"); s != std::string::npos) stem.erase(0, s + 1);
			if (const size_t d = stem.rfind('.'); d != std::string::npos) stem.erase(d);
			w += " " + stem;
		}
		switch (k)
		{
			case ContainerKind::Array: return w + "[]";
			case ContainerKind::Set:   return "Set of " + w;
			case ContainerKind::Map:   return "Map of " + w;
			default:                   return w;
		}
	}

	bool isScalarStruct(const SourceVar& sv)
	{
		return sv.type == PinType::Struct && sv.kind == ContainerKind::None && !sv.typeName.empty();
	}

	// The field `member` of the struct `typeName`, followed through formerNames.
	bool fieldOf(const std::string& typeName, const std::string& member, HE::StructField& out)
	{
		HE::StructDef def;
		if (!HE::TypeRegistry::instance().getStruct(typeName, def)) return false;
		const HE::StructField* f = def.findField(member);
		if (!f) return false;
		out = *f;
		return true;
	}

	std::string fieldFit(const Variable& target, const HE::StructField& f)
	{
		std::string why;
		HorizonCode::pullShapesCompatible(f.type, f.kind(), f.keyType, f.typeName,
		                                  target.type, target.kind(), target.keyType,
		                                  target.typeName, &why);
		return why;
	}

	const SourceVar* findSource(const std::vector<SourceVar>& vars, const std::string& name)
	{
		for (const SourceVar& sv : vars)
			if (sv.name == name) return &sv;
		return nullptr;
	}

	// The source variable a target pulls into itself when "Add to Target"
	// creates it: the target's own shape and default, public, and nothing of
	// the target's multiplayer/save/pull settings — those are about the
	// target, not the source.
	Variable mirrorOf(const Variable& target, const std::string& name)
	{
		Variable v = target;
		v.name = name;
		v.access = 0;
		v.scope = 0;
		v.replicated = v.repNotify = v.saveGame = false;
		v.pullSource.clear(); v.pullVar.clear(); v.pullMember.clear(); v.pullClass.clear();
		v.bindTo = false; v.pullRef.clear();
		return v;
	}
}

std::vector<SourceVar> publicVariables(const HorizonCode::Graph& g)
{
	std::vector<SourceVar> out;
	for (const Variable& v : g.variables)
		if (v.access == 0 && v.scope == 0)
			out.push_back({ v.name, v.type, v.kind(), v.keyType, v.typeName, v.className });
	return out;
}

std::string fitOf(const Variable& target, const SourceVar& sv, const std::string& member)
{
	if (!member.empty())
	{
		if (!isScalarStruct(sv)) return "not a struct, it has no members";
		HE::StructField f;
		if (!fieldOf(sv.typeName, member, f)) return "no member '" + member + "'";
		return fieldFit(target, f);
	}
	std::string why;
	if (HorizonCode::pullShapesCompatible(sv.type, sv.kind, sv.keyType, sv.typeName,
	                                      target.type, target.kind(), target.keyType,
	                                      target.typeName, &why))
	{
		// A Ref fits any Ref at run time; the class it holds is what the
		// editor can still check, and a known mismatch is worth saying.
		if (sv.type == PinType::Ref && !sv.className.empty() && !target.className.empty() &&
		    sv.className != target.className)
			return "holds " + sv.className + ", needs " + target.className;
		return {};
	}
	if (isScalarStruct(sv) && !fittingMembers(target, sv).empty())
		return typeWord(sv.type, sv.kind, sv.typeName) + ": pick a member";
	return why;
}

std::vector<std::string> fittingMembers(const Variable& target, const SourceVar& sv)
{
	std::vector<std::string> out;
	if (!isScalarStruct(sv)) return out;
	HE::StructDef def;
	if (!HE::TypeRegistry::instance().getStruct(sv.typeName, def)) return out;
	for (const HE::StructField& f : def.fields)
		if (fieldFit(target, f).empty()) out.push_back(f.name);
	return out;
}

Status check(const Variable& target, const std::vector<SourceVar>* source)
{
	using HorizonCode::PullFailure;
	Status st;
	if (target.pullSource.empty()) return st;
	auto fail = [&](PullFailure why, const std::string& detail = {})
	{
		st.ok = false;
		st.why = why;
		st.text = HorizonCode::pullFailureText(why, target.pullSource, target.pullVar,
		                                       target.pullMember, detail, target.pullRef);
		return st;
	};
	if (!HorizonCode::isKnownPullSource(target.pullSource)) return fail(PullFailure::UnknownSource);
	const bool viaRef = target.pullSource == HorizonCode::kPullFromRef;
	if (viaRef && target.pullRef.empty())
	{
		st.text = "pick the object reference variable to bind through";
		return st;
	}
	const std::string label = HorizonCode::pullSourceLabel(target.pullSource, target.pullRef);
	const std::string path = label + kSep +
	                         (target.pullVar.empty() ? std::string("?") : target.pullVar) +
	                         (target.pullMember.empty() ? std::string() : kSep + target.pullMember);
	if (target.pullVar.empty())
	{
		st.text = "pick a variable of the " + label;
		return st;
	}
	if (!source)
	{
		// A creator without an expected class, or a reference of no known
		// class: whatever it holds at run time decides.
		st.ok = true;
		st.text = path + (viaRef ? " (class unknown, checked at run time)"
		                         : " (checked when it is created)");
		return st;
	}
	const SourceVar* sv = findSource(*source, target.pullVar);
	if (!sv) return fail(PullFailure::NoPublicVariable);
	if (!target.pullMember.empty())
	{
		if (!isScalarStruct(*sv)) return fail(PullFailure::NotAStruct);
		HE::StructField f;
		if (!fieldOf(sv->typeName, target.pullMember, f)) return fail(PullFailure::NoSuchMember);
		if (const std::string why = fieldFit(target, f); !why.empty())
			return fail(PullFailure::TypeMismatch, why);
		if (f.name != target.pullMember) st.liveMember = f.name;
		st.ok = true;
		st.text = path + " (" + typeWord(f.type, f.kind(), f.typeName) + ")";
		return st;
	}
	if (const std::string why = fitOf(target, *sv); !why.empty())
		return fail(PullFailure::TypeMismatch, why);
	st.ok = true;
	st.text = path + " (" + typeWord(sv->type, sv->kind, sv->typeName) + ")";
	return st;
}

std::string describe(const Variable& v)
{
	if (v.pullSource.empty()) return {};
	std::string s = (v.bindTo ? "Bound to " : "Pulled from ") +
	                HorizonCode::pullSourceLabel(v.pullSource, v.pullRef);
	if (v.pullSource == HorizonCode::kPullFromCreator && !v.pullClass.empty())
	{
		std::string stem = v.pullClass;
		if (const size_t p = stem.find_last_of("/\\"); p != std::string::npos) stem.erase(0, p + 1);
		if (const size_t d = stem.rfind('.'); d != std::string::npos) stem.erase(d);
		s += " (" + stem + ")";
	}
	s += kSep + v.pullVar;
	if (!v.pullMember.empty()) s += kSep + v.pullMember;
	return s;
}

std::vector<std::string> refVariables(const HorizonCode::Graph& owner)
{
	std::vector<std::string> out;
	for (const Variable& v : owner.variables)
		if (v.scope == 0 && v.type == PinType::Ref && !v.isArray) out.push_back(v.name);
	return out;
}

std::string refClassOf(const HorizonCode::Graph& owner, const std::string& ref)
{
	const Variable* r = owner.findVariable(ref);
	return r && r->scope == 0 && r->type == PinType::Ref && !r->isArray ? r->className
	                                                                    : std::string();
}

std::string sourceClassOf(const Variable& v, const HorizonCode::Graph& owner)
{
	if (v.pullSource == HorizonCode::kPullFromCreator) return v.pullClass;
	if (v.pullSource == HorizonCode::kPullFromRef)     return refClassOf(owner, v.pullRef);
	return {};
}

Add addToTarget(HorizonCode::Graph& source, const Variable& target,
                const std::string& structType, std::string* what)
{
	auto say = [&](const std::string& s) { if (what) *what = s; };
	if (target.pullVar.empty()) return Add::Refused;
	Variable* existing = source.findVariable(target.pullVar);
	if (existing && existing->scope != 0)
	{
		say("'" + target.pullVar + "' is a function-local there");
		return Add::Refused;
	}

	if (target.pullMember.empty())
	{
		if (existing)
		{
			const SourceVar sv{ existing->name, existing->type, existing->kind(), existing->keyType,
			                    existing->typeName, existing->className };
			if (!fitOf(target, sv).empty())
			{
				say("'" + target.pullVar + "' already exists there with another type");
				return Add::Refused;
			}
			if (existing->access == 0) return Add::NothingToDo;
			existing->access = 0;
			say("made '" + target.pullVar + "' public");
			return Add::MadePublic;
		}
		source.variables.push_back(mirrorOf(target, target.pullVar));
		say("added public variable '" + target.pullVar + "'");
		return Add::AddedVariable;
	}

	// A member pull.
	if (existing)
	{
		if (existing->type != PinType::Struct || existing->isArray || existing->typeName.empty())
		{
			say("'" + target.pullVar + "' exists there and is not a struct");
			return Add::Refused;
		}
		HE::StructField f;
		if (!fieldOf(existing->typeName, target.pullMember, f)) return Add::NeedsMemberInStruct;
		if (!fieldFit(target, f).empty())
		{
			say("member '" + target.pullMember + "' exists with another type");
			return Add::Refused;
		}
		if (existing->access == 0) return Add::NothingToDo;
		existing->access = 0;
		say("made '" + target.pullVar + "' public");
		return Add::MadePublic;
	}
	if (structType.empty()) return Add::NeedsStructType;
	HE::StructField f;
	if (!fieldOf(structType, target.pullMember, f) || !fieldFit(target, f).empty())
	{
		say("that struct has no fitting member '" + target.pullMember + "'");
		return Add::Refused;
	}
	Variable v;
	v.name = target.pullVar;
	v.type = PinType::Struct;
	v.typeName = structType;
	// The field starts where the target would have: pulling it changes
	// nothing until somebody sets it, which is what adding a source should do.
	if (!target.isArray) v.structDefaults[f.name] = HorizonCode::variableDefaultValue(target);
	source.variables.push_back(std::move(v));
	say("added public struct variable '" + target.pullVar + "'");
	return Add::AddedStructVar;
}

bool addMember(HE::StructDef& def, const Variable& target, const std::string& member)
{
	if (member.empty() || def.findField(member)) return false;
	HE::StructField f;
	f.name = member;
	f.type = target.type;
	f.isArray = target.isArray;
	f.container = target.container;
	f.keyType = target.keyType;
	f.keyTypeName = target.keyTypeName;
	f.typeName = target.typeName;
	f.defaultValue = HorizonCode::variableDefaultValue(target);
	def.fields.push_back(std::move(f));
	return true;
}

std::vector<std::string> structsWithMember(const Variable& target, const std::string& member)
{
	std::vector<std::string> out;
	for (const HE::StructDef& def : HE::TypeRegistry::instance().structs())
		if (const HE::StructField* f = def.findField(member); f && fieldFit(target, *f).empty())
			out.push_back(def.assetPath);
	return out;
}

} // namespace HcPull
