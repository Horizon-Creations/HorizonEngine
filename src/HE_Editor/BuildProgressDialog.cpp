#include "BuildProgressDialog.h"
#include "EditorApplication.h"   // AppContext
#include "EditorWidgets.h"       // primaryButton, pinDialogToEditorWindow
#include <Diagnostics/Logger.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <SDL3/SDL_process.h>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#endif

namespace BuildProgressDialog
{
namespace
{
// ── Model ───────────────────────────────────────────────────────────────────
// Written by the export worker, read by the UI thread, all under one mutex.
// It is a handful of strings per second against a 60 Hz reader, so a single
// lock is cheaper than being clever — and the UI copies what it needs under it
// rather than holding it across ImGui calls that can re-enter.

enum StepState { Pending = 0, Running = 1, Done = 2, Failed = 3 };

struct Step
{
	std::string name;
	int         state         = Pending;
	float       progress      = 0.0f;  // 0..1, meaningless while indeterminate
	bool        indeterminate = true;  // no length known yet
	std::string detail;                // "128 / 340", "62 %" — under the label
};

struct LogLine
{
	int         step = 0;
	int         severity = 0;          // 0 info, 1 warning, 2 error
	std::string text;
};

std::mutex            s_mutex;
std::vector<Step>     s_steps;
std::vector<LogLine>  s_log;
int                   s_current  = -1;
bool                  s_running  = false;
bool                  s_finished = false;
bool                  s_success  = false;
std::string           s_message;
std::string           s_activity;        // what the running step is doing right now
std::filesystem::path s_exePath;
bool                  s_runnableHere = false;

// ── UI-thread-only state ────────────────────────────────────────────────────
bool   s_openRequest = false;
bool   s_visible     = false;  // drew this frame (see isOpen)
int    s_selected    = -1;     // -1 = follow the running step
bool   s_userPicked  = false;  // …until the user clicks one
Action s_action      = Action::None;

// The game started from here. Kept so the next launch can release the previous
// handle instead of leaking one per click; SDL_DestroyProcess does not kill the
// child, which is the point — closing the editor should not close the game.
SDL_Process* s_launched = nullptr;

} // namespace

// ─── Model ───────────────────────────────────────────────────────────────────

namespace Build
{

void begin(const std::vector<std::string>& stepNames)
{
	{
		std::lock_guard<std::mutex> lk(s_mutex);
		s_steps.clear();
		s_steps.reserve(stepNames.size());
		for (const std::string& n : stepNames) s_steps.push_back(Step{n});
		s_log.clear();
		s_current      = -1;
		s_running      = true;
		s_finished     = false;
		s_success      = false;
		s_message.clear();
		s_activity.clear();
		s_exePath.clear();
		s_runnableHere = false;
	}
	s_selected   = -1;
	s_userPicked = false;
}

void stepBegin(int index)
{
	std::lock_guard<std::mutex> lk(s_mutex);
	if (index < 0 || index >= static_cast<int>(s_steps.size())) return;
	// Anything still running behind us is done — steps are strictly sequential.
	for (int i = 0; i < index; ++i)
		if (s_steps[i].state == Running)
		{
			s_steps[i].state         = Done;
			s_steps[i].progress      = 1.0f;
			s_steps[i].indeterminate = false;
		}
	s_steps[index].state = Running;
	s_current = index;
}

void stepProgress(float fraction)
{
	std::lock_guard<std::mutex> lk(s_mutex);
	if (s_current < 0 || s_current >= static_cast<int>(s_steps.size())) return;
	Step& st = s_steps[s_current];
	if (fraction < 0.0f || fraction > 1.0f) { st.indeterminate = true; return; }
	st.indeterminate = false;
	st.progress      = fraction;
	char buf[16];
	std::snprintf(buf, sizeof(buf), "%d %%", static_cast<int>(fraction * 100.0f + 0.5f));
	st.detail = buf;
}

void stepProgress(int done, int total)
{
	std::lock_guard<std::mutex> lk(s_mutex);
	if (s_current < 0 || s_current >= static_cast<int>(s_steps.size())) return;
	Step& st = s_steps[s_current];
	if (total <= 0) { st.indeterminate = true; return; }
	st.indeterminate = false;
	st.progress      = std::clamp(static_cast<float>(done) / static_cast<float>(total), 0.0f, 1.0f);
	st.detail        = std::to_string(done) + " / " + std::to_string(total);
}

void setActivity(const std::string& what)
{
	std::lock_guard<std::mutex> lk(s_mutex);
	s_activity = what;
}

void stepFailed(int index)
{
	std::lock_guard<std::mutex> lk(s_mutex);
	if (index < 0 || index >= static_cast<int>(s_steps.size())) return;
	s_steps[index].state         = Failed;
	s_steps[index].indeterminate = false;
}

void log(int severity, const std::string& text)
{
	std::lock_guard<std::mutex> lk(s_mutex);
	// Lines that arrive between steps (the closing summary, a throw caught
	// outside any step) belong to the last step that ran — otherwise they land
	// in a per-step view that shows nothing.
	const int step = s_current >= 0 ? s_current
	                                : std::max(0, static_cast<int>(s_steps.size()) - 1);
	s_log.push_back(LogLine{step, severity, text});
}

void finish(bool success, const std::string& message)
{
	std::lock_guard<std::mutex> lk(s_mutex);
	for (Step& st : s_steps)
	{
		if (st.state != Running) continue;
		st.state         = success ? Done : Failed;
		st.progress      = success ? 1.0f : st.progress;
		st.indeterminate = false;
	}
	if (success)
		for (Step& st : s_steps)
			if (st.state == Pending) { st.state = Done; st.progress = 1.0f; st.indeterminate = false; }

	s_running  = false;
	s_finished = true;
	s_success  = success;
	s_message  = message;
	s_activity.clear();
}

void setLaunchTarget(const std::filesystem::path& executable, bool runnableHere)
{
	std::lock_guard<std::mutex> lk(s_mutex);
	s_exePath      = executable;
	s_runnableHere = runnableHere;
}

bool running()
{
	std::lock_guard<std::mutex> lk(s_mutex);
	return s_running;
}

} // namespace Build

void requestOpen() { s_openRequest = true; }

bool isOpen() { return s_visible; }

Action takeAction()
{
	const Action a = s_action;
	s_action = Action::None;
	return a;
}

// ─── Dialog ──────────────────────────────────────────────────────────────────

#ifdef HE_IMGUI_ENABLED
namespace
{

// Our own, rather than imgui_internal.h's IM_PI: four arc angles are not worth
// a dependency on ImGui's private header.
constexpr float kPi = 3.14159265358979323846f;

ImU32 stateColor(int state)
{
	switch (state)
	{
	case Done:    return IM_COL32( 90, 215,  90, 255);
	case Running: return IM_COL32(255, 205,  80, 255);
	case Failed:  return IM_COL32(240,  90,  90, 255);
	default:      break;
	}
	return IM_COL32(110, 110, 115, 255);
}

// One step: a ring that fills with that step's own progress, the step number
// (or a tick / cross once it is settled) inside it, name and counter below.
// Returns true when it was clicked — the log below follows the selection.
bool drawStepRing(const Step& st, int index, ImVec2 center, float radius, float columnWidth,
                  bool selected)
{
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImU32 col     = stateColor(st.state);
	const ImU32 trackCol = IM_COL32(58, 58, 64, 255);
	const float thick   = 4.0f;

	dl->AddCircleFilled(center, radius - thick * 0.5f,
	                    selected ? IM_COL32(48, 48, 54, 255) : IM_COL32(34, 34, 38, 255), 32);
	dl->AddCircle(center, radius, trackCol, 40, thick);

	if (st.state == Running && st.indeterminate)
	{
		// Length unknown (cmake configuring, codesign running): a quarter arc
		// travelling round the ring says "working" without claiming a number.
		const float t     = static_cast<float>(ImGui::GetTime()) * 2.0f;
		const float start = std::fmod(t, kPi * 2.0f);
		dl->PathArcTo(center, radius, start, start + kPi * 0.5f, 24);
		dl->PathStroke(col, 0, thick);
	}
	else if (st.state == Running || st.state == Done || st.state == Failed)
	{
		const float frac = st.state == Done ? 1.0f : std::clamp(st.progress, 0.0f, 1.0f);
		if (frac > 0.001f)
		{
			const float a0 = -kPi * 0.5f;
			dl->PathArcTo(center, radius, a0, a0 + kPi * 2.0f * frac, 48);
			dl->PathStroke(col, 0, thick);
		}
	}

	// The glyph inside. Drawn with lines rather than a font character: the
	// editor's default font has no tick or cross, and a "v" would read as one
	// only to the person who wrote it.
	if (st.state == Done)
	{
		const float s = radius * 0.42f;
		dl->AddLine(ImVec2(center.x - s, center.y + s * 0.05f),
		            ImVec2(center.x - s * 0.15f, center.y + s * 0.75f), col, 2.5f);
		dl->AddLine(ImVec2(center.x - s * 0.15f, center.y + s * 0.75f),
		            ImVec2(center.x + s, center.y - s * 0.65f), col, 2.5f);
	}
	else if (st.state == Failed)
	{
		const float s = radius * 0.36f;
		dl->AddLine(ImVec2(center.x - s, center.y - s), ImVec2(center.x + s, center.y + s), col, 2.5f);
		dl->AddLine(ImVec2(center.x + s, center.y - s), ImVec2(center.x - s, center.y + s), col, 2.5f);
	}
	else
	{
		char num[8];
		std::snprintf(num, sizeof(num), "%d", index + 1);
		const ImVec2 ts = ImGui::CalcTextSize(num);
		dl->AddText(ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f), col, num);
	}

	// Name + counter, centred under the ring and clipped to the column so a
	// long step name cannot shove its neighbours out of the row.
	const float textTop = center.y + radius + 6.0f;
	const float left    = center.x - columnWidth * 0.5f;
	ImGui::PushClipRect(ImVec2(left, textTop), ImVec2(left + columnWidth, textTop + 44.0f), true);
	auto centeredText = [&](const char* text, ImU32 color, float y) {
		const ImVec2 ts = ImGui::CalcTextSize(text);
		dl->AddText(ImVec2(center.x - ts.x * 0.5f, y), color, text);
	};
	centeredText(st.name.c_str(),
	             st.state == Pending ? IM_COL32(150, 150, 155, 255) : IM_COL32(226, 226, 230, 255),
	             textTop);
	if (!st.detail.empty() && st.state == Running)
		centeredText(st.detail.c_str(), IM_COL32(150, 150, 155, 255),
		             textTop + ImGui::GetTextLineHeight() + 2.0f);
	ImGui::PopClipRect();

	// The whole column is the hit target, not just the ring — a 44 px circle is
	// a small thing to ask someone to hit to read a log.
	ImGui::SetCursorScreenPos(ImVec2(left, center.y - radius));
	ImGui::PushID(index);
	const bool clicked = ImGui::InvisibleButton("##step", ImVec2(columnWidth, radius * 2.0f + 46.0f));
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", st.name.c_str());
	ImGui::PopID();
	return clicked;
}

// Start the exported game. Exe-relative by construction: the runtime resolves
// its pak and project.hcfg from SDL_GetBasePath, so no working directory has to
// be arranged for it.
void launchGame(const std::filesystem::path& exe)
{
	const std::string path = exe.string();
	const char* argv[] = { path.c_str(), nullptr };
	if (s_launched) { SDL_DestroyProcess(s_launched); s_launched = nullptr; }
	s_launched = SDL_CreateProcess(argv, false);
	if (!s_launched)
		HE_LOG_ERROR(Editor, "Build: could not start %s (%s)", path.c_str(), SDL_GetError());
	else
		HE_LOG_INFO(Editor, "Build: started %s", path.c_str());
}

} // namespace
#endif // HE_IMGUI_ENABLED

void render([[maybe_unused]] AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
	s_visible = false;   // set again below if the popup draws this frame
	if (s_openRequest)
	{
		ImGui::OpenPopup("Build##progress");
		s_openRequest = false;
	}

	// While the worker runs this modal is the editor's lock on the project —
	// same reason the export dialog held one: nothing may mutate Content/ under
	// the packer's reads. Another same-level popup force-closes it, so it is
	// re-opened once that one is gone.
	if (Build::running()
	    && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
		ImGui::OpenPopup("Build##progress");

	ImGui::SetNextWindowSize(ImVec2(720.0f, 520.0f), ImGuiCond_Appearing);
	EditorWidgets::pinDialogToEditorWindow();
	if (!ImGui::BeginPopupModal("Build##progress", nullptr, ImGuiWindowFlags_NoSavedSettings))
		return;

	// One copy of the model per frame, taken under the lock and let go of
	// immediately: ImGui calls below can run user code, and holding a worker's
	// mutex across them is how a UI ends up waiting on a compiler.
	s_visible = true;

	std::vector<Step> steps;
	bool running = false, finished = false, success = false;
	std::string message, activity;
	std::filesystem::path exePath;
	bool runnable = false;
	int current = -1;
	{
		std::lock_guard<std::mutex> lk(s_mutex);
		steps    = s_steps;
		running  = s_running;
		finished = s_finished;
		success  = s_success;
		message  = s_message;
		activity = s_activity;
		exePath  = s_exePath;
		runnable = s_runnableHere;
		current  = s_current;
	}

	// Which step's log is shown: the running one, until the user picks another.
	// After a failure that is the step that failed — the log worth reading is
	// never the one that went fine.
	if (!s_userPicked) s_selected = current;
	if (!s_userPicked && finished && !success)
		for (size_t i = 0; i < steps.size(); ++i)
			if (steps[i].state == Failed) { s_selected = static_cast<int>(i); break; }
	if (s_selected < 0 || s_selected >= static_cast<int>(steps.size()))
		s_selected = steps.empty() ? -1 : static_cast<int>(steps.size()) - 1;

	// ── The ring row ─────────────────────────────────────────────────────────
	if (!steps.empty())
	{
		const float radius   = 22.0f;
		const float rowTop   = ImGui::GetCursorScreenPos().y + 8.0f;
		const float avail    = ImGui::GetContentRegionAvail().x;
		const float left     = ImGui::GetCursorScreenPos().x;
		const int   n        = static_cast<int>(steps.size());
		const float column   = avail / static_cast<float>(n);
		const float centerY  = rowTop + radius;

		ImDrawList* dl = ImGui::GetWindowDrawList();
		// Connectors first, so the rings sit on top of them. One step means no
		// connector at all — the loop simply does not run.
		for (int i = 0; i + 1 < n; ++i)
		{
			const float x0 = left + column * (static_cast<float>(i) + 0.5f) + radius + 4.0f;
			const float x1 = left + column * (static_cast<float>(i) + 1.5f) - radius - 4.0f;
			if (x1 <= x0) continue;
			dl->AddLine(ImVec2(x0, centerY), ImVec2(x1, centerY),
			            steps[i].state == Done ? IM_COL32(90, 215, 90, 160)
			                                   : IM_COL32(70, 70, 76, 255), 2.0f);
		}

		for (int i = 0; i < n; ++i)
		{
			const ImVec2 c(left + column * (static_cast<float>(i) + 0.5f), centerY);
			if (drawStepRing(steps[i], i, c, radius, column, i == s_selected))
			{
				s_selected   = i;
				s_userPicked = true;
			}
		}
		ImGui::SetCursorScreenPos(ImVec2(left, centerY + radius + 52.0f));
	}

	ImGui::Separator();

	// ── The selected step's log ──────────────────────────────────────────────
	const std::string stepName = (s_selected >= 0 && s_selected < static_cast<int>(steps.size()))
		? steps[static_cast<size_t>(s_selected)].name : std::string();
	ImGui::TextDisabled("%s", stepName.empty() ? "Output" : stepName.c_str());
	ImGui::SameLine(ImGui::GetContentRegionAvail().x - 40.0f);
	const bool copy = ImGui::SmallButton("Copy");

	// Everything above the button row; the row itself is measured, not guessed,
	// so the log box shrinks with the window instead of pushing the buttons out.
	const float footerH = ImGui::GetFrameHeightWithSpacing()
	                    + ImGui::GetTextLineHeightWithSpacing()
	                    + ImGui::GetStyle().ItemSpacing.y * 2.0f;
	ImGui::BeginChild("##build_step_log", ImVec2(0.0f, -footerH), ImGuiChildFlags_Borders);
	{
		std::string clip;
		std::lock_guard<std::mutex> lk(s_mutex);
		ImGui::PushTextWrapPos(0.0f);
		for (const LogLine& l : s_log)
		{
			if (l.step != s_selected) continue;
			if (copy) { clip += l.text; clip += '\n'; }
			if (l.severity == 2)
				ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "%s", l.text.c_str());
			else if (l.severity == 1)
				ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s", l.text.c_str());
			else
				ImGui::TextUnformatted(l.text.c_str());
		}
		ImGui::PopTextWrapPos();
		if (copy) ImGui::SetClipboardText(clip.c_str());
		// Follow the tail of the step being watched, unless the user scrolled up.
		if (running && s_selected == current && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f)
			ImGui::SetScrollHereY(1.0f);
	}
	ImGui::EndChild();

	// ── Result line + actions ────────────────────────────────────────────────
	if (running)
	{
		EditorWidgets::WrapText wrap(690.0f);
		ImGui::TextDisabled("%s", activity.empty() ? "Working\xe2\x80\xa6" : activity.c_str());
	}
	else if (finished)
	{
		EditorWidgets::WrapText wrap(690.0f);
		if (success) ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "%s", message.c_str());
		else         ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", message.c_str());
	}
	else
	{
		ImGui::TextDisabled("No build has run yet.");
	}

	ImGui::BeginDisabled(running);

	const bool canLaunch = finished && success && runnable && !exePath.empty();
	ImGui::BeginDisabled(!canLaunch);
	if (EditorWidgets::primaryButton("Start Game", ImVec2(120.0f, 0.0f)))
		launchGame(exePath);
	ImGui::EndDisabled();
	if (!canLaunch && finished && success && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("%s", exePath.empty()
			? "This export shipped no game runtime."
			: "The export targets another platform — it cannot run on this machine.");

	ImGui::SameLine();
	// Stays open, unlike the other two: the next run reports into this same
	// window, and closing it here only to reopen it a frame later would flash.
	if (ImGui::Button("Build Again", ImVec2(120.0f, 0.0f)))
		s_action = Action::Rebuild;
	ImGui::SameLine();
	if (ImGui::Button("Build Settings", ImVec2(130.0f, 0.0f)))
	{
		s_action = Action::BackToSetup;
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndDisabled();

	ImGui::SameLine();
	ImGui::BeginDisabled(running);
	if (ImGui::Button("Close", ImVec2(90.0f, 0.0f))) ImGui::CloseCurrentPopup();
	ImGui::EndDisabled();

	ImGui::EndPopup();
#endif
}

} // namespace BuildProgressDialog
