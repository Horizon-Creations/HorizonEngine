#include "BuildProgressDialog.h"
#include "EditorApplication.h"   // AppContext
#include "EditorHelp.h"          // "Build Window/<label>" scope for its buttons
#include "EditorWidgets.h"       // primaryButton, pinDialogToEditorWindow
#include "NotificationStore.h"    // a build that ends while its window is put away says so
#include <Diagnostics/Logger.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
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
// Under the same lock as the rest of the model: begin() runs on the UI thread,
// but the reader is the UI thread too and one lock for one model is the rule
// this file already follows.
Kind                  s_kind = Kind::Export;
unsigned long long    s_runSerial = 0;   // begin() calls so far — see outcome()
// The HorizonCode classes this run ships interpreted instead of compiled. Kept
// beyond finish() on purpose — the export settings dialog reads it back when
// the user returns to fix them.
std::string                           s_hcHeadline;
std::vector<HcFallbackReport::Notice> s_hcInterpreted;

// ── UI-thread-only state ────────────────────────────────────────────────────
bool   s_openRequest = false;
bool   s_visible     = false;  // drew this frame (see isOpen)
bool   s_hidden      = false;  // the user put a RUNNING build away (see hidden())
int    s_selected    = -1;     // -1 = follow the running step
bool   s_userPicked  = false;  // …until the user clicks one
Action s_action      = Action::None;

// The game started from here. Kept so the next launch can release the previous
// handle instead of leaking one per click; SDL_DestroyProcess does not kill the
// child, which is the point — closing the editor should not close the game.
SDL_Process* s_launched = nullptr;

} // namespace

std::optional<float> toolchainProgress(const std::string& line)
{
	const auto open = line.find('[');
	if (open == std::string::npos || open > 4) return std::nullopt;
	const auto close = line.find(']', open);
	if (close == std::string::npos || close <= open + 1 || close - open > 16) return std::nullopt;

	std::string in;
	for (size_t i = open + 1; i < close; ++i)
		if (!std::isspace(static_cast<unsigned char>(line[i]))) in += line[i];
	if (in.empty()) return std::nullopt;

	if (in.back() == '%')
	{
		in.pop_back();
		if (in.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
		return std::clamp(std::stof(in) / 100.0f, 0.0f, 1.0f);
	}
	const auto slash = in.find('/');
	if (slash == std::string::npos) return std::nullopt;
	const std::string a = in.substr(0, slash), b = in.substr(slash + 1);
	if (a.empty() || b.empty() ||
	    a.find_first_not_of("0123456789") != std::string::npos ||
	    b.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
	const float total = std::stof(b);
	if (total <= 0.0f) return std::nullopt;
	return std::clamp(std::stof(a) / total, 0.0f, 1.0f);
}

// ─── Model ───────────────────────────────────────────────────────────────────

namespace Build
{

void begin(const std::vector<std::string>& stepNames, Kind kind)
{
	{
		std::lock_guard<std::mutex> lk(s_mutex);
		s_kind = kind;
		++s_runSerial;
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
		// The previous run's fallbacks are not this run's — a rebuild after a
		// fix must not keep showing the classes it just repaired.
		s_hcHeadline.clear();
		s_hcInterpreted.clear();
	}
	s_selected   = -1;
	s_userPicked = false;
	s_hidden     = false;   // a new run is shown; the window opens for it
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

void log(int severity, const std::string& text, int stepOverride)
{
	std::lock_guard<std::mutex> lk(s_mutex);
	// Lines that arrive between steps (the closing summary, a throw caught
	// outside any step) belong to the last step that ran — otherwise they land
	// in a per-step view that shows nothing.
	int step = s_current >= 0 ? s_current
	                          : std::max(0, static_cast<int>(s_steps.size()) - 1);
	if (stepOverride >= 0 && stepOverride < static_cast<int>(s_steps.size())) step = stepOverride;
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

void setInterpretedClasses(const std::string& headline,
                           const std::vector<HcFallbackReport::Notice>& classes)
{
	std::lock_guard<std::mutex> lk(s_mutex);
	s_hcHeadline    = headline;
	s_hcInterpreted = classes;
}

bool running()
{
	std::lock_guard<std::mutex> lk(s_mutex);
	return s_running;
}

} // namespace Build

InterpretedSummary interpretedClasses()
{
	std::lock_guard<std::mutex> lk(s_mutex);
	return InterpretedSummary{ s_hcHeadline, s_hcInterpreted };
}

Snapshot snapshot()
{
	std::lock_guard<std::mutex> lk(s_mutex);
	Snapshot out;
	// `hasRun` is the difference between "the last build failed" and "nothing
	// has ever been built here", and the model only knows it as "there are no
	// steps and nothing is running".
	out.hasRun       = !s_steps.empty() || s_running;
	out.running      = s_running;
	out.kind         = s_kind;
	out.current      = s_current;
	out.activity     = s_activity;
	out.finished     = s_finished;
	out.success      = s_success;
	out.message      = s_message;
	out.executable   = s_exePath;
	out.runnableHere = s_runnableHere;
	out.interpreted  = InterpretedSummary{ s_hcHeadline, s_hcInterpreted };
	out.steps.reserve(s_steps.size());
	for (const Step& s : s_steps)
		out.steps.push_back(StepView{ s.name, s.state, s.progress, s.indeterminate, s.detail });
	out.log.reserve(s_log.size());
	for (const LogLine& l : s_log)
		out.log.push_back(LogView{ l.step, l.severity, l.text });
	return out;
}

Outcome outcome()
{
	std::lock_guard<std::mutex> lk(s_mutex);
	return Outcome{ s_runSerial, s_finished, s_success };
}

void requestOpen() { s_openRequest = true; s_hidden = false; }

bool hidden() { return s_hidden && Build::running(); }

bool exportRunning()
{
	std::lock_guard<std::mutex> lk(s_mutex);
	return s_running && s_kind == Kind::Export;
}

float overallProgress(const std::vector<StepView>& steps, bool* indeterminate)
{
	if (indeterminate) *indeterminate = false;
	if (steps.empty()) return 0.0f;
	float sum = 0.0f;
	bool  running = false, runningIndeterminate = false;
	for (const StepView& st : steps)
	{
		if (st.state == Done || st.state == Failed) sum += 1.0f;
		else if (st.state == Running)
		{
			running = true;
			if (st.indeterminate) runningIndeterminate = true;
			else sum += std::clamp(st.progress, 0.0f, 1.0f);
		}
	}
	// Only "unknown" when it is the running step that does not know: a run that is
	// 3 steps in of 4 still says 75 % whatever the last one is doing.
	if (indeterminate) *indeterminate = running && runningIndeterminate && sum <= 0.0f;
	return std::clamp(sum / static_cast<float>(steps.size()), 0.0f, 1.0f);
}

bool isOpen() { return s_visible; }

Action takeAction()
{
	const Action a = s_action;
	s_action = Action::None;
	return a;
}

Kind runKind()
{
	std::lock_guard<std::mutex> lk(s_mutex);
	return s_kind;
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

	// Not SDL_CreateProcess(argv, false): that one makes the child INHERIT the
	// editor's standard handles, and on Windows SDL does it by duplicating
	// GetStdHandle(STD_INPUT/OUTPUT/ERROR_HANDLE). The editor is a GUI-subsystem
	// program with no console, so those handles are NULL/invalid and the call fails
	// with "DuplicateHandle(): Das Handle ist ungültig" before the game is even looked
	// for — which reads as "the editor cannot find the exe" when the file is right there.
	// The game gets no stdio there (it has no console either); elsewhere the terminal
	// the editor was started from keeps receiving its log, as before.
	SDL_PropertiesID props = SDL_CreateProperties();
	SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, const_cast<char**>(argv));
#ifdef _WIN32
	SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER,  SDL_PROCESS_STDIO_NULL);
	SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_NULL);
	SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
	// A GUI program started from a GUI program: no console window flashing up.
	SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
#else
	SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER,  SDL_PROCESS_STDIO_INHERITED);
	SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_INHERITED);
	SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_INHERITED);
#endif
	s_launched = SDL_CreateProcessWithProperties(props);
	SDL_DestroyProperties(props);

	if (!s_launched)
	{
		const std::string why = SDL_GetError();
		HE_LOG_ERROR(Editor, "Build: could not start %s (%s)", path.c_str(), why.c_str());
		// The user pressed Start Game (or its toast button) and is owed an answer on
		// screen, not only a line in the log.
		HE::Ed::NoteSpec note;
		note.level     = HE::Ed::NoteLevel::Important;
		note.title     = "Could not start the game";
		note.text      = "\"" + exe.filename().string() + "\" did not start.";
		note.detail    = why;
		note.assetPath = path;
		note.key       = "build.launch";
		HE::Ed::notify(std::move(note));
	}
	else
	{
		HE_LOG_INFO(Editor, "Build: started %s", path.c_str());
		HE::Ed::resolveNotification("build.launch");
	}
}


// ── A run that ended with nobody watching ───────────────────────────────────
// Polled every frame from render(). `watched` is whether the window was up on the
// previous frame, i.e. whether the user saw the result land. A game-logic build
// is left to GameLogicBuildPanel, which posts its own.
unsigned long long s_announcedRun = 0;

void announceFinish(bool watched)
{
	const Outcome o = outcome();
	if (!o.finished || o.run == s_announcedRun) return;
	s_announcedRun = o.run;
	if (watched) return;

	const Snapshot snap = snapshot();
	if (snap.kind != Kind::Export) return;

	// The model's message carries "Error: " / "OK: " for the result line; the title
	// already says which.
	std::string text = snap.message;
	for (const char* prefix : { "Error: ", "OK: " })
		if (text.rfind(prefix, 0) == 0) { text.erase(0, std::string(prefix).size()); break; }
	if (text.empty()) text = o.success ? "The export finished." : "The export failed.";

	HE::Ed::NoteSpec note;
	note.level = HE::Ed::NoteLevel::Important;
	note.title = o.success ? "Export finished" : "Export failed";
	note.text  = text;
	note.key   = "build.finished";
	const bool canLaunch = o.success && snap.runnableHere && !snap.executable.empty();
	if (canLaunch)
	{
		const std::filesystem::path exe = snap.executable;
		note.actions.push_back({ "Start Game", [exe] { launchGame(exe); }, true });
	}
	note.actions.push_back({ "Show Build", [] { requestOpen(); }, !canLaunch });
	HE::Ed::notify(std::move(note));
}

// ── The footer cluster of a hidden run ──────────────────────────────────────
// One read of the model per ImGui frame, shared by FooterWidth and DrawFooter: a
// worker moving the step between the two calls would change the label's width after
// the footer had already right-aligned against the old one.
struct Brief
{
	int         frame = -1;
	float       fraction = 0.0f;
	bool        indeterminate = false;
	std::string label;
};
Brief s_brief;

constexpr float kFooterBarW = 90.0f;
constexpr float kFooterBarH = 10.0f;
constexpr float kFooterGap  = 6.0f;
constexpr std::size_t kFooterLabelChars = 26;

const Brief& brief()
{
	const int frame = ImGui::GetFrameCount();
	if (s_brief.frame == frame) return s_brief;
	s_brief.frame = frame;

	std::vector<StepView> steps;
	std::string           stepName;
	Kind                  kind;
	{
		std::lock_guard<std::mutex> lk(s_mutex);
		kind = s_kind;
		for (std::size_t i = 0; i < s_steps.size(); ++i)
		{
			const Step& st = s_steps[i];
			steps.push_back(StepView{ "", st.state, st.progress, st.indeterminate, "" });
			if (static_cast<int>(i) == s_current) stepName = st.name;
		}
	}
	s_brief.fraction = overallProgress(steps, &s_brief.indeterminate);
	std::string label = kind == Kind::Export ? "Export" : "Build";
	if (!stepName.empty()) label += ": " + stepName;
	if (label.size() > kFooterLabelChars) label = label.substr(0, kFooterLabelChars - 1) + "\xE2\x80\xA6";
	s_brief.label = std::move(label);
	return s_brief;
}

std::string percentText(const Brief& b)
{
	if (b.indeterminate) return "\xE2\x80\xA6";
	char buf[16];
	std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(b.fraction * 100.0f + 0.5f));
	return buf;
}

} // namespace
#endif // HE_IMGUI_ENABLED

float FooterWidth([[maybe_unused]] AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
	if (!hidden()) return 0.0f;
	const Brief& b = brief();
	// Same item sequence as DrawFooter: label, gap, bar, gap, percent. The percent
	// is measured at its widest so the cluster does not breathe as it counts up.
	return ImGui::CalcTextSize(b.label.c_str()).x + kFooterGap + kFooterBarW + kFooterGap +
	       ImGui::CalcTextSize("100%").x;
#else
	return 0.0f;
#endif
}

void DrawFooter([[maybe_unused]] AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
	if (!hidden()) return;
	const Brief& b = brief();

	// Painted by hand, in a slot one text line tall, for the reason the download
	// queue's bar is (EngineContentSyncBar.cpp): a framed ProgressBar does not fit
	// the 24 px footer and drags the baseline of everything after it.
	const ImVec2 clusterMin = ImGui::GetCursorScreenPos();
	ImGui::TextUnformatted(b.label.c_str());
	ImGui::SameLine(0.0f, kFooterGap);

	const float  lineH   = ImGui::GetTextLineHeight();
	const ImVec2 slotPos = ImGui::GetCursorScreenPos();
	ImGui::Dummy(ImVec2(kFooterBarW, lineH));
	const float  top = slotPos.y + (lineH - kFooterBarH) * 0.5f;
	const ImVec2 p0(slotPos.x, top);
	const ImVec2 p1(slotPos.x + kFooterBarW, top + kFooterBarH);
	const float  radius = kFooterBarH * 0.5f;

	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImGuiCol_FrameBg), radius);
	const ImU32 fill = ImGui::GetColorU32(ImGuiCol_PlotHistogram);
	if (!b.indeterminate)
	{
		const float w = kFooterBarW * b.fraction;
		if (w > 0.0f)
			dl->AddRectFilled(p0, ImVec2(p0.x + w, p1.y), fill, std::min(radius, w * 0.5f));
	}
	else
	{
		constexpr float kSegW = 0.3f, kPeriod = 1.6f;
		const float t  = static_cast<float>(std::fmod(ImGui::GetTime(), kPeriod)) / kPeriod;
		const float x0 = p0.x + (kFooterBarW * (1.0f + kSegW)) * t - kFooterBarW * kSegW;
		const float l = std::max(x0, p0.x), r = std::min(x0 + kFooterBarW * kSegW, p1.x);
		if (r > l) dl->AddRectFilled(ImVec2(l, p0.y), ImVec2(r, p1.y), fill, std::min(radius, (r - l) * 0.5f));
	}
	ImGui::SameLine(0.0f, kFooterGap);
	ImGui::TextDisabled("%s", percentText(b).c_str());

	// The whole cluster is the button: a click brings the window back. Hover-tested
	// against the painted rectangle plus IsWindowHovered, as the download queue is,
	// so a floating panel lying over the footer does not steal or double the hint.
	const ImVec2 clusterMax = ImGui::GetItemRectMax();
	if (ImGui::IsWindowHovered() &&
	    ImGui::IsMouseHoveringRect(ImVec2(clusterMin.x - kFooterGap, clusterMin.y), clusterMax))
	{
		ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		if (ImGui::BeginTooltip())
		{
			ImGui::TextUnformatted("The build is still running. Click to show its window.");
			ImGui::EndTooltip();
		}
		if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) requestOpen();
	}
#endif
}

void render([[maybe_unused]] AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
	// Whether the window was up on the last frame, before it is reset below: a run
	// that ends under the user's eyes needs no notification.
	announceFinish(s_visible);
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
	if (Build::running() && !s_hidden
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
	// Not "Build": that scope belongs to the Build MENU in the menu bar, and its
	// entries are about starting an export, not about the window that watches one.
	HE::Ed::Help::Scope helpScope("Build Window");

	std::vector<Step> steps;
	bool running = false, finished = false, success = false;
	std::string message, activity;
	std::filesystem::path exePath;
	bool runnable = false;
	int current = -1;
	Kind kind = Kind::Export;
	std::string hcHeadline;
	std::vector<HcFallbackReport::Notice> hcInterpreted;
	{
		std::lock_guard<std::mutex> lk(s_mutex);
		kind     = s_kind;
		steps    = s_steps;
		running  = s_running;
		finished = s_finished;
		success  = s_success;
		message  = s_message;
		activity = s_activity;
		exePath  = s_exePath;
		runnable = s_runnableHere;
		current  = s_current;
		hcHeadline    = s_hcHeadline;
		hcInterpreted = s_hcInterpreted;
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

	// ── What shipped interpreted instead of compiled ─────────────────────────
	// Between the rings and the log, not in the log: the log belongs to ONE step
	// and scrolls, and a compiler that emits two hundred lines after the codegen
	// carries this one out of sight. It is also the only thing a green, finished
	// export can be quietly worth less for, so it goes where the result line is
	// read. Text only — nothing here is a control, so nothing needs a help entry.
	//
	// The height is bounded and its own child, taken BEFORE the log's
	// `-footerH` child is opened: a band that grew with the number of classes
	// would push the buttons off the bottom, which is the exact failure this
	// window was built to replace.
	if (!hcHeadline.empty())
	{
		const ImVec4 warn(1.0f, 0.8f, 0.3f, 1.0f);   // the log's warning colour
		{
			// Scoped to the headline alone: the wrap position is absolute in
			// WINDOW space, and leaving it pushed across the child below would
			// wrap that child's lines against the outer window's column.
			EditorWidgets::WrapText wrap(690.0f);
			ImGui::TextColored(warn, "%s", hcHeadline.c_str());
		}
		if (!hcInterpreted.empty())
		{
			const float rows = static_cast<float>(std::min<size_t>(hcInterpreted.size(), 4));
			ImGui::BeginChild("##hc_interpreted",
			                  ImVec2(0.0f, rows * ImGui::GetTextLineHeightWithSpacing()
			                               + ImGui::GetStyle().FramePadding.y * 2.0f),
			                  ImGuiChildFlags_Borders);
			for (const HcFallbackReport::Notice& n : hcInterpreted)
				ImGui::TextColored(warn, "%s", HcFallbackReport::describe(n).c_str());
			ImGui::EndChild();
		}
		ImGui::Separator();
	}

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
	ImGui::BeginChild("##build_step_log", ImVec2(0.0f, -footerH), ImGuiChildFlags_Borders,
	                  ImGuiWindowFlags_HorizontalScrollbar);
	{
		std::string clip;
		std::lock_guard<std::mutex> lk(s_mutex);
		auto drawLine = [](const LogLine& l)
		{
			if (l.severity == 2)
				ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "%s", l.text.c_str());
			else if (l.severity == 1)
				ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s", l.text.c_str());
			else
				ImGui::TextUnformatted(l.text.c_str());
		};

		// The step's own lines, in order. Pointers are good for this frame only —
		// we hold the model's lock until the end of the block, so the worker cannot
		// grow (and move) the vector underneath them.
		std::vector<const LogLine*> lines;
		for (const LogLine& l : s_log)
			if (l.step == s_selected) lines.push_back(&l);
		if (copy)
			for (const LogLine* l : lines) { clip += l->text; clip += '\n'; }

		// A pack step lists every asset it ships — thousands of lines in a real
		// project, redrawn sixty times a second. Past this size the log is drawn
		// through a clipper, which needs one fixed line height: no wrapping, and
		// the child scrolls sideways instead. Short logs (compiler output
		// included) keep their wrapping.
		constexpr size_t kClipFrom = 400;
		if (lines.size() > kClipFrom)
		{
			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(lines.size()));
			while (clipper.Step())
				for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
					drawLine(*lines[static_cast<size_t>(i)]);
			clipper.End();
		}
		else
		{
			ImGui::PushTextWrapPos(0.0f);
			for (const LogLine* l : lines) drawLine(*l);
			ImGui::PopTextWrapPos();
		}
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
	// The tooltip that stood here fired only while the button was disabled, and
	// the help lookup fires then too (its hover flags allow a disabled item on
	// purpose), so both would have shown at once. The entry says both cases.

	ImGui::SameLine();
	// Stays open, unlike the other two: the next run reports into this same
	// window, and closing it here only to reopen it a frame later would flash.
	if (EditorWidgets::button("Build Again", ImVec2(120.0f, 0.0f)))
		s_action = Action::Rebuild;
	// Only an export HAS settings. A game-logic build takes its orders from the
	// project (its Source/ folder and the toolchain), so the row is left out
	// rather than offered and then refused.
	if (kind == Kind::Export)
	{
		ImGui::SameLine();
		if (EditorWidgets::button("Build Settings", ImVec2(130.0f, 0.0f)))
		{
			s_action = Action::BackToSetup;
			ImGui::CloseCurrentPopup();
		}
	}
	ImGui::EndDisabled();

	ImGui::SameLine();
	if (running)
	{
		// The run goes on without the window: the footer shows its progress, a click
		// there brings it back, and the end of the run raises a notification.
		if (EditorWidgets::button("Hide", ImVec2(90.0f, 0.0f)))
		{
			s_hidden = true;
			ImGui::CloseCurrentPopup();
		}
	}
	else if (ImGui::Button("Close", ImVec2(90.0f, 0.0f)))
	{
		ImGui::CloseCurrentPopup();
	}

	ImGui::EndPopup();
#endif
}

} // namespace BuildProgressDialog
