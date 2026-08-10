#include "CollabActivityBar.h"

#include "CollabController.h"
#include "EditorApplication.h"   // AppContext

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#endif

#include <filesystem>
#include <string>
#include <vector>

namespace CollabActivityBar
{
#ifdef HE_IMGUI_ENABLED
namespace
{
	// How long a line stays up. News about somebody else's work is ambient and
	// goes; news about your own asset is the only notification you will get, so
	// it stays long enough to be read after looking away.
	constexpr double kNewsSeconds   = 9.0;
	constexpr double kPersonalSeconds = 25.0;
	// The last second is spent fading, so the line does not vanish mid-glance.
	constexpr double kFadeSeconds   = 1.0;

	constexpr int kMaxLabelChars = 34;

	struct Batch
	{
		std::string author;      // empty once more than one person contributed
		int         count = 0;
		std::string lastName;    // the file, for the single-asset case
		double      since = 0.0;
	};

	Batch       s_batch;         // coalesced arrivals
	std::string s_personal;      // about an asset of ours
	double      s_personalSince = 0.0;

	std::string elide(std::string s)
	{
		if (static_cast<int>(s.size()) > kMaxLabelChars)
			s = s.substr(0, kMaxLabelChars - 1) + "\xE2\x80\xA6";  // U+2026
		return s;
	}

	// One line for however many arrived. The single-asset case names the file,
	// because that is the case where the name is the useful part; past that the
	// count is, and a list would not fit a 24px footer anyway.
	std::string batchText()
	{
		if (s_batch.count <= 0) return {};
		const std::string who = s_batch.author.empty() ? std::string("Someone")
		                                               : s_batch.author;
		if (s_batch.count == 1)
			return elide(who + " created " + s_batch.lastName);
		return elide(who + " created " + std::to_string(s_batch.count) + " assets");
	}

	// Drain the controller into the local batch. Done here rather than in the
	// controller because the fade needs a clock, and the controller has none —
	// it is pumped from the network, not from the frame.
	void collect(AppContext& ctx)
	{
		if (!ctx.collab) return;
		// Noticed here rather than through a hook the session would have to
		// remember to call: a line about a session you have left has no author
		// and no context, and leaving it to fade would be 25 seconds of it.
		static bool s_wasInSession = false;
		const bool inSession = ctx.collab->inSession();
		if (s_wasInSession && !inSession) Reset();
		s_wasInSession = inSession;

		const auto& fresh = ctx.collab->createdAssetNotices();
		if (!fresh.empty())
		{
			const double now = ImGui::GetTime();
			// A new arrival restarts the clock: a steady trickle should read as
			// one ongoing thing rather than flicker in and out.
			if (s_batch.count == 0) { s_batch = {}; s_batch.since = now; }
			else                     s_batch.since = now;

			for (const auto& n : fresh)
			{
				++s_batch.count;
				s_batch.lastName = std::filesystem::path(n.path).filename().string();
				if (s_batch.count == 1)                 s_batch.author = n.byName;
				else if (s_batch.author != n.byName)    s_batch.author.clear();
			}
			ctx.collab->clearCreatedAssetNotices();
		}

		if (!ctx.collab->assetNotice().empty())
		{
			s_personal      = ctx.collab->assetNotice();
			s_personalSince = ImGui::GetTime();
			ctx.collab->clearAssetNotice();
		}
	}

	// The host's in-tray, as one line. Does NOT fade and cannot be dismissed:
	// it is a count of things waiting for a decision, and it goes away by being
	// decided. A client sees the mirror of it — what it is waiting for.
	std::string queueText(AppContext& ctx)
	{
		if (!ctx.collab || !ctx.collab->inSession()) return {};

		// Waiting on US, from two directions that are one thing to the reader:
		// deletes and renames the host answers, and assets someone is asking us
		// to hand over. Both are a decision nobody else can make, so they are
		// counted together rather than as two competing lines — and the edit
		// half reaches everyone, host or not.
		std::size_t n = ctx.collab->pendingEditRequests().size();
		if (ctx.collab->isHost()) n += ctx.collab->pendingAssetOps().size();
		if (n > 0)
		{
			return n == 1 ? std::string("1 request waiting")
			              : std::to_string(n) + " requests waiting";
		}

		// Nothing to decide — then what we are waiting for, if anything.
		const std::size_t ours = ctx.collab->pendingRequestsOfOurs();
		if (ours == 0) return {};
		return ours == 1 ? std::string("1 request pending")
		                 : std::to_string(ours) + " requests pending";
	}

	// 1 while fresh, falling to 0 across the last second. Returns 0 once expired,
	// which is also the signal to drop the entry.
	float alphaFor(double since, double lifetime)
	{
		const double age = ImGui::GetTime() - since;
		if (age >= lifetime) return 0.0f;
		if (age <= lifetime - kFadeSeconds) return 1.0f;
		return static_cast<float>((lifetime - age) / kFadeSeconds);
	}
}
#endif // HE_IMGUI_ENABLED

float FooterWidth(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
	collect(ctx);

	if (!s_personal.empty() && alphaFor(s_personalSince, kPersonalSeconds) <= 0.0f)
		s_personal.clear();
	if (s_batch.count > 0 && alphaFor(s_batch.since, kNewsSeconds) <= 0.0f)
		s_batch = {};

	// Requests outrank both. They are the only item here that is WORK rather
	// than news, and unlike the other two they do not fade — a queue that
	// quietly disappeared would be a queue nobody answers.
	if (const std::string q = queueText(ctx); !q.empty())
		return ImGui::CalcTextSize(q.c_str()).x;

	// The personal notice wins the rest: it is the one the user cannot find out
	// any other way.
	const std::string text = !s_personal.empty() ? elide(s_personal) : batchText();
	if (text.empty()) return 0.0f;
	return ImGui::CalcTextSize(text.c_str()).x;
#else
	(void)ctx;
	return 0.0f;
#endif
}

bool DrawFooter(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
	// Requests first, and in amber whether they are yours to answer or somebody
	// else's to answer for you — either way it is unfinished business.
	if (const std::string q = queueText(ctx); !q.empty())
	{
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.72f, 0.35f, 1.0f));
		ImGui::TextUnformatted(q.c_str());
		ImGui::PopStyleColor();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(ctx.collab && ctx.collab->isHost()
				? "Someone is waiting for you to approve a delete or rename.\n"
				  "Click to open Collaboration."
				: "Waiting for the host to answer. Click to open Collaboration.");
		return ImGui::IsItemClicked();
	}

	const bool  personal = !s_personal.empty();
	const std::string text = personal ? elide(s_personal) : batchText();
	if (text.empty()) return false;

	const float a = personal ? alphaFor(s_personalSince, kPersonalSeconds)
	                         : alphaFor(s_batch.since, kNewsSeconds);
	// Amber for your own asset, ordinary disabled grey for other people's work —
	// the colour is the difference between "you may need to act" and "for your
	// information".
	const ImVec4 col = personal ? ImVec4(1.00f, 0.72f, 0.35f, a)
	                            : ImVec4(0.65f, 0.65f, 0.65f, a * 0.9f);
	ImGui::PushStyleColor(ImGuiCol_Text, col);
	ImGui::TextUnformatted(text.c_str());
	ImGui::PopStyleColor();

	const bool clicked = ImGui::IsItemClicked();
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("%s", personal
			? "About an asset you created. Click to open Collaboration."
			: "New assets from the session. Click to open Collaboration.");
	}
	// Clicking is also an acknowledgement: the user has seen it, so it should
	// not sit there for the rest of its lifetime.
	if (clicked)
	{
		if (personal) s_personal.clear();
		else          s_batch = {};
	}
	return clicked;
#else
	(void)ctx;
	return false;
#endif
}

void Reset()
{
#ifdef HE_IMGUI_ENABLED
	s_batch = {};
	s_personal.clear();
#endif
}

} // namespace CollabActivityBar
