#pragma once
#include "TaskBoardTeam.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

// ── A team server in a box, for the task board's team half ───────────────────
// What TaskBoardTeam needs from the team server, and nothing else: a roster
// with presence, and a notice to a member that reaches every editor that member
// is signed in with, `latencyMs` later, and only while they are online (the
// server does not keep mail). The causing editor never gets its own notice back.
//
// ONE clock for everything: every pump in a test goes through pump(), which
// only moves forward. A helper that restarts its own clock at 1 makes every
// time already recorded look 2^64 ms old (topic 197 step 17).
namespace he_test
{

class FakeTaskTeam
{
public:
	struct Link;

	std::uint64_t nowMs = 1'000;
	std::uint64_t latencyMs = 40;

	// A member of the team; offline until one of their links is connected.
	void addMember(const std::string& name) { m_members.push_back({ name }); }

	// An editor signed in (or not yet) as `member`.
	Link& editor(const std::string& member)
	{
		m_links.push_back(std::make_unique<Link>(*this, member));
		return *m_links.back();
	}

	// Moves the clock forward by `ms` (never back), delivers what is due and
	// pumps every bridge given, in that order — the editor's frame.
	void pump(std::uint64_t ms, std::initializer_list<HE::TaskBoardTeam*> bridges = {})
	{
		nowMs += ms;
		deliver();
		for (HE::TaskBoardTeam* b : bridges) b->update(nowMs);
		deliver();
	}

	// How many notices the server carried, per receiving editor and in total.
	int carried = 0;

	struct Link : HE::TaskTeamLink
	{
		Link(FakeTaskTeam& t, std::string m) : team(t), member(std::move(m)) {}
		FakeTaskTeam& team;
		std::string   member;
		bool          signedIn = true;
		// False: signed in, but the roster has not arrived yet (TeamClient
		// between "logged-in" and the LIST_MEMBERS reply).
		bool          rosterIn = true;
		std::deque<Incoming> inbox;
		int           received = 0;
		// What send() was asked, in order (to, payload).
		std::vector<std::pair<std::string, std::string>> sent;

		bool connected() const override { return signedIn; }
		std::string self() const override { return signedIn ? member : std::string{}; }
		std::vector<HE::TaskTeamMember> members() const override
		{
			std::vector<HE::TaskTeamMember> out;
			if (!signedIn || !rosterIn) return out;
			for (const auto& m : team.m_members) out.push_back({ m.name, team.online(m.name) });
			return out;
		}
		bool send(const std::string& to, const std::string& payload) override
		{
			if (!signedIn) return false;
			sent.emplace_back(to, payload);
			team.m_wire.push_back({ team.nowMs + team.latencyMs, member, to, payload, this });
			return true;
		}
		bool poll(Incoming& out) override
		{
			if (inbox.empty()) return false;
			out = std::move(inbox.front());
			inbox.pop_front();
			return true;
		}
	};

	bool online(const std::string& name) const
	{
		return std::any_of(m_links.begin(), m_links.end(), [&](const std::unique_ptr<Link>& l) {
			return l->signedIn && HE::sameTaskMember(l->member, name);
		});
	}

private:
	struct Member { std::string name; };
	struct InFlight
	{
		std::uint64_t at;
		std::string   from, to, payload;
		const Link*   cause;
	};

	void deliver()
	{
		for (auto it = m_wire.begin(); it != m_wire.end();)
		{
			if (it->at > nowMs) { ++it; continue; }
			for (auto& l : m_links)
				if (l.get() != it->cause && l->signedIn && HE::sameTaskMember(l->member, it->to))
				{
					l->inbox.push_back({ it->from, it->payload });
					++l->received;
					++carried;
				}
			it = m_wire.erase(it);
		}
	}

	std::vector<Member>                m_members;
	std::vector<std::unique_ptr<Link>> m_links;
	std::vector<InFlight>              m_wire;
};

} // namespace he_test
