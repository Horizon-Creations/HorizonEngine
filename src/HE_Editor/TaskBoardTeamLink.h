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
// Notices go out as the team server's NOTIFY (op 21) and come back as its NOTICE
// event (kind 10). Against a server from before that op, send() drops the notice
// as delivered rather than letting the outbox hold it for nothing; the assignee
// picker and "Mine" work either way.
class TeamControllerTaskLink final : public HE::TaskTeamLink
{
public:
	explicit TeamControllerTaskLink(TeamController& team) : m_team(team) {}

	bool connected() const override { return m_team.signedIn() && !m_team.members().empty(); }
	std::string self() const override { return m_team.member(); }
	std::vector<HE::TaskTeamMember> members() const override
	{
		std::vector<HE::TaskTeamMember> out;
		for (const HE::Net::TeamMember& m : m_team.members()) out.push_back({ m.name, m.online });
		return out;
	}
	bool send(const std::string& member, const std::string& payload) override
	{
		if (!m_team.noticesSupported()) return true;   // an old server: nothing will ever carry it
		return m_team.sendNotice(member, payload);
	}
	bool poll(Incoming& out) override
	{
		TeamController::IncomingNotice n;
		if (!m_team.pollNotice(n)) return false;
		out.from    = std::move(n.from);
		out.payload = std::move(n.payload);
		return true;
	}

private:
	TeamController& m_team;
};
