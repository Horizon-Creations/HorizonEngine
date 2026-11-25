#pragma once
#include "TaskBoardTeam.h"
#include "TeamController.h"

#include <string>
#include <vector>

// ── The task board's team link, on the editor's TeamController ───────────────
// What TaskBoardTeam needs from a team connection (TaskBoardTeam.h), answered by
// the team server client the editor already keeps signed in. With it the Tasks
// assignee field picks members of the team, and "Mine" knows who "me" is.
//
// connected() waits for the roster: an edit made between the login and the
// LIST_MEMBERS reply would find nobody on the team to tell.
//
// shortcut: notices are not carried. The team server has no NOTIFY request yet
// (op 20 went to SC_TICKET; docs/taskboard-team-interop-2026-10-15.md "Wire" is the
// design), so send() drops a notice as delivered rather than letting the outbox
// hold it for nothing, and poll() never has one. Upgrade when the server gains the
// op: send() = a TeamController request, poll() = its event queue.
class TeamControllerTaskLink final : public HE::TaskTeamLink
{
public:
	explicit TeamControllerTaskLink(const TeamController& team) : m_team(team) {}

	bool connected() const override { return m_team.signedIn() && !m_team.members().empty(); }
	std::string self() const override { return m_team.member(); }
	std::vector<HE::TaskTeamMember> members() const override
	{
		std::vector<HE::TaskTeamMember> out;
		for (const HE::Net::TeamMember& m : m_team.members()) out.push_back({ m.name, m.online });
		return out;
	}
	bool send(const std::string&, const std::string&) override { return true; }
	bool poll(Incoming&) override { return false; }

private:
	const TeamController& m_team;
};
