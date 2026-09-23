#pragma once

// ─── Replicated variables an entity carries, for the frontends that have no
//     variables the engine can see ──────────────────────────────────────────
// A HorizonCode class declares its variables in its graph, and the engine can
// read them: the checkbox in the variable list is the whole declaration (plan
// §6.1). A Lua or Python script's locals live inside its interpreter, and a C++
// GameLogic's members live in a hot-loaded dylib — the engine cannot see either,
// which is the same gap Memory `savegames-v2` calls "Script-Var-Capture". So
// those three declare EXPLICITLY, with net.declareVar, and the value lives
// here rather than in their own storage.
//
// ORDER IS INSERTION ORDER, and that is load-bearing rather than tidy: the
// property table (kMsgPropertyTable) sends names once and every delta afterwards
// addresses them by INDEX, so both sides have to agree on what index 3 is. A
// std::unordered_map would give the host one order and, on a different libc,
// give nobody the same one twice. A vector of pairs is the whole implementation
// and a linear scan is the whole lookup; a class with fifty replicated
// variables is a class that wants a struct.
//
// THE COMPONENT IS NOT THE REPLICATION SWITCH. NetworkComponent::replicates is
// (step 4). An entity with declared variables and the switch off replicates
// nothing; the values are simply local state, which is the honest reading of
// "this entity is not on the network".

#include <HorizonCode/HorizonCode.h>

#include <string>
#include <vector>

struct ReplicatedVarsComponent
{
	struct Entry
	{
		std::string           name;
		HorizonCode::Value    value;
		// Call OnRep on the clients when this one changes (plan §6.4). Per
		// variable, because a value a graph polls every frame wants no callback
		// and a door's state does.
		bool                  notify = false;
	};
	std::vector<Entry> entries;

	// Index of `name`, or -1. The one lookup; everything else goes through it.
	int indexOf(const std::string& name) const
	{
		for (std::size_t i = 0; i < entries.size(); ++i)
			if (entries[i].name == name) return static_cast<int>(i);
		return -1;
	}

	// Declare, or re-declare. A second declaration of a name that exists keeps
	// the CURRENT value and only updates `notify` — re-running OnInit after a
	// hot reload must not silently reset a health bar to its default, and a
	// script that declares in OnInit is the normal case, not the exception.
	// Returns the index.
	int declare(const std::string& name, const HorizonCode::Value& initial, bool notify)
	{
		if (const int at = indexOf(name); at >= 0)
		{
			entries[static_cast<std::size_t>(at)].notify = notify;
			return at;
		}
		entries.push_back(Entry{ name, initial, notify });
		return static_cast<int>(entries.size()) - 1;
	}

	const HorizonCode::Value* find(const std::string& name) const
	{
		const int at = indexOf(name);
		return at >= 0 ? &entries[static_cast<std::size_t>(at)].value : nullptr;
	}
	HorizonCode::Value* find(const std::string& name)
	{
		const int at = indexOf(name);
		return at >= 0 ? &entries[static_cast<std::size_t>(at)].value : nullptr;
	}
};
