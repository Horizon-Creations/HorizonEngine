#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct AppContext;
struct EditorConfig;

// ── Reward moments: small, optional feedback for work that just went right ────
// Topic 75 ("Gamification"). Which editor events get a reward, where exactly
// each one is hooked, what the feedback looks like and where the switch lives.
// Every hook site carries a one-line "Reward moment (EditorRewards.h)" comment
// pointing back here.
//
// What this is NOT, and must never grow into: points, levels, leaderboards,
// achievement popups, anything modal, anything that takes focus, anything that
// delays the action it rewards. The editor is a professional tool first; the
// whole feature switches off completely, and while it is off it costs nothing.
//
// ── The three moments ────────────────────────────────────────────────────────
// All three fire on the UI thread, once per USER action, and only on success.
//
// 1. SAVED — the user saved what they were working on.
//    Fires from EditorUI.cpp, never from EditorApplication::saveSceneToPath:
//    that function is also what MCP's scene.saveScene and the level-script
//    hc.save call, and a save an agent did is not a save the user did.
//      • doSaveScene           — Ctrl/Cmd+S on the scene with a known path.
//                                Only if ctx.sceneDirty was true BEFORE the
//                                call: saving a clean scene rewrites the file
//                                anyway, and that is the same no-op keystroke
//                                as the view-only tab below.
//      • PendingFileOp::SaveScene handler — the async Save-As result.
//      • doSaveActiveTab       — Ctrl/Cmd+S on an asset tab (incl. the Skeletal
//                                Mesh viewer's clip). Only if the tab HAD unsaved
//                                edits (tabHasUnsavedEdits before the call):
//                                saveAsset answers true for a view-only tab that
//                                had nothing to write, and a reward for a no-op
//                                keystroke is noise.
//      • doSaveAll             — ONE moment for the whole batch, not one per
//                                asset; fires if something was written and
//                                nothing failed. It writes the scene through the
//                                quiet writeScene, not doSaveScene.
//      • the "save, then act" guard (Save button of the unsaved-changes prompt)
//        counts too; its assets and its scene land in the same frame and are
//        one moment (see "once" below). The feedback never delays
//        runGuardedAction.
//    AppContext::saveSceneToPath returns bool for this, so the UI knows whether
//    the scene save worked instead of guessing from ctx.sceneDirty afterwards.
//    NOT a moment: the solo autosave (SceneAutosave — a recovery copy, it
//    bypasses saveSceneToPath on purpose), MCP saves, hc.save of the level script.
//
// 2. BUILD SUCCEEDED — a build finished without errors.
//    Both kinds report into BuildProgressDialog, but from different threads:
//    Game Logic calls Build::finish from GameLogicBuildPanel::render (UI thread),
//    the export calls it from its worker (ExportDialogPanel.cpp). So the hook is
//    ONE edge detector on the UI thread over BuildProgressDialog::outcome() — a
//    run serial plus finished/success, NOT snapshot(), which copies the whole
//    log and has no business in a per-frame poll. It fires once per run serial,
//    so a run that began and ended between two frames still counts and a second
//    finish() of the same run does not. EditorUI polls it every frame outside
//    the footer block (the project hub skips the footer), and the serial is
//    consumed even while the feature is off, so switching it on never rewards
//    a build from before.
//    Decision: an MCP-started Game Logic build (McpToolsBuild → the same
//    GameLogicBuildPanel::start) counts as well — it is the user's project and
//    the user sees the Build window finish; there is no separate path to skip.
//    A failed build is NOT a moment: no line, nothing counted, and the existing
//    Problem notification stays exactly as it is. It only gets a tone of its
//    own (topic 95, "The tones" below) — information for someone looking
//    elsewhere, not a reward. There is no "cancel" to tell apart: every
//    finish(false) is a build that went wrong.
//
// 3. ASSETS IMPORTED — one or more source files became assets.
//    Three callers of Importer::importSource do an import the user asked for:
//      • EditorUI.cpp, PendingFileOp::ImportAsset — File ▸ Import Asset batch.
//        ONE moment per batch for the non-texture files, carrying the count,
//        only if > 0. Textures of the batch wait for their colour space:
//      • TextureColourSpaceDialog::applyImport — the confirmed texture batch,
//        one moment with its count, only if > 0.
//      • ContentBrowserPanel.cpp, context menu "Import" — one file; fires only
//        when importSource returned true.
//    NOT a moment: Reimport (it refreshes an existing asset), a colour-space
//    retag, collab-received assets. An OS file drop is not an import at all — it
//    is handed to the preview's widget manager (EditorApplication.cpp,
//    SDL_EVENT_DROP_COMPLETE).
//
// Once: fire() takes at most one moment per ImGui frame — the first. A user
// action is one frame of UI code, so this is what keeps the guard's assets +
// scene, or a menu shortcut that might reach two handlers, from pulsing and
// chiming twice.
//
// ── Several moments close together (topic 95) ────────────────────────────────
// Cmd+S is a habit; a dozen saves a minute must not be a dozen chimes. All of
// this lives in Feed (ImGui-free, tested with a hand clock), in this order:
//   1. Once per frame, as above — the first moment of a frame wins.
//   2. SAME KIND MERGES: the same moment again while its line still shows
//      (hold + fade) adds its count ("Imported 1 asset" → "Imported 3 assets";
//      a second "Saved" keeps "Saved") and restarts the hold. Never a tone.
//   3. RANK Build > Import > Save: a lower moment does not replace a higher
//      line that still shows — it is counted (the tally ran before the Feed),
//      not shown, never heard, and the hold is NOT restarted. A higher one
//      replaces a lower line and may sound under 4 and 5. Once a line has
//      faded out, anything replaces it.
//   4. TONE GAP: kToneGapSec between two tones, whatever the moments. A moment
//      in the gap is shown, not heard.
//   5. SAVE GAP: kSaveToneGapSec between two SAVE tones on top of that, so
//      saving every minute gives an occasional tick, not one per keystroke.
//   Both gaps run from the last tone that PLAYED: a silent moment (sound off,
//   inside a gap, merged, outranked) never pushes the next tone further away.
//   6. The caller decides whether a moment may sound at all (toneWanted: the
//      switches, volume > 0, not during Play-in-Editor — the speakers belong to
//      the game then — and the build tones only with the editor in the
//      background) and hands that in as soundWanted. Autosave, MCP saves and
//      hc.save are not moments at all (above), so they are silent by
//      construction.
//   The failed-build tone has no moment and no line; it goes straight to
//   takeTone() and so keeps rule 4 with the others.
//
// ── What the feedback is ─────────────────────────────────────────────────────
// Visual (RewardsVisual, with the master on): the footer's centred "Ready" label
// becomes the moment's line — "Saved", "Build succeeded", "Imported 12 assets" —
// in the build window's "done" green, holds for kHoldSec, then cross-fades back
// to "Ready" over kFadeSec. A thin line under it shrinks to nothing over the
// same time: the one moving thing, and it moves at the bottom edge of the
// window. No window, no popup, no focus change, no layout shift. Words, not a
// check-mark glyph: the editor font (Roboto Condensed Bold) is not known to
// carry U+2713, and a missing glyph renders as "?". The editor draws every
// frame (only the game sets Application::setEventDriven), so the fade needs no
// redraw request.
//
// Sound (RewardsSound, OFF by default — an open-plan office is the normal
// case): see "The tones" below. gain = gainFor(RewardsVolume), the square of
// the slider, so half way sounds like half as loud; volume 0 plays nothing.
// Each tone's "Preview" in the settings plays it once at that gain, past the
// Feed and past its own switch.
//
// ── The tones (topic 95) ─────────────────────────────────────────────────────
// Synthesised in code as mono PCM16, so no asset ships and no licence applies.
// Shared rules, held by the tests: at most 0.45 s, a peak well below full
// scale, at least 4 ms of fade-in and a tail that lands on exactly zero (no
// click either end), nothing below ~600 Hz (bass booms on some laptop speakers
// and vanishes on others), all notes from A major pentatonic so two tones that
// do meet still agree.
//   Save          saveTickPcm16      S1 "tick": a short E6 with a breath of
//                                    band-passed noise, 60 ms, the quietest —
//                                    it is the one heard most often.
//   Build         chimePcm16         A5 then E6, 0.42 s: the topic-75 chime.
//                                    Only while NO editor window has keyboard
//                                    focus: a build you watched finish needs no
//                                    tone, one you walked away from does.
//   Build failed  buildFailedPcm16   E6 then B5, a fourth DOWN, softer onset: the
//                                    chime turned downwards, open rather than
//                                    "wrong" — no buzzer, no low note, quieter
//                                    than the chime. Same focus rule as
//                                    the build tone (the Build window already
//                                    says so to someone looking at it).
//   Import        importPopPcm16     I1 "pop": a sine gliding 1.4 → 0.9 kHz in
//                                    70 ms. The count does not change the sound.
//
// Routing: the tones play on an AudioEngine of their own (AppContext::
// uiAudioEngine, owned by EditorApplication), NOT on the project's engine:
// the project's master fader and mute do not touch them, and stopAll() at the
// end of Play-in-Editor does not cut them. It opens its output device lazily —
// in pollBuild, the first frame a tone becomes POSSIBLE (uiSoundPossible:
// master, Sound, not muted), never inside the frame of the save that would
// play, because opening a device can take a noticeable moment and the
// feedback must never delay the action. Someone who never turns sound on never
// has a second device open; turning it off (or muting) closes it again. If
// the device cannot be opened, that is remembered until sound is switched off
// and on again, so a machine without output does not retry every frame.
// EditorSoundsMuted is the switch for exactly this engine: it silences every
// sound the editor makes itself and leaves each tone's own switch as it was.
//
// Progress display (step 3): in the same centred label while idle —
// "Ready · 3 builds today · 5 days in a row". drawFooterStatus composes it from
// its idleText, so EditorUI still passes plain "Ready" and the switches are
// read in this file only. NOT a new widget in the footer's right-anchored
// group — that chain is hand-maintained and every insertion edits every block
// to its left (see the comment there in EditorUI.cpp). While a moment's line is
// showing, the counters wait; the line fades back into the composed idle text.
// If the composed text would take more than a third of the footer's width
// (a narrow window), the label falls back to plain "Ready" rather than run
// into Undo/Redo or the right-hand group.
//
// ── The switches (EditorConfig, Preferences ▸ Feedback) ──────────────────────
//   bool  RewardsEnabled      = true;  master: off = no feedback, no sound, no
//                                      progress shown and nothing counted
//   The rest only act with the master on, and NEVER gate each other (topic 95):
//   sound without the line, the line without sound, either with or without
//   the counters.
//   bool  RewardsVisual       = true;  the moment's line and underline. Off:
//                                      the footer stays on its idle text (with
//                                      the counters, if those are on)
//   int   RewardsReducedMotion = 0;    0 = follow the system's reduce-motion
//                                      setting, 1 = off (full motion). There
//                                      is no "always reduce": the system
//                                      switch is where that lives. Reduced:
//                                      no shrinking underline. The system query
//                                      itself is step 4 — systemReducesMotion()
//                                      answers false until then.
//   bool  RewardsSound        = false; the tones at all
//   float RewardsVolume       = 0.5;   0..1, applied squared (gainFor)
//   bool  RewardsSoundSave        = true;  the tick
//   bool  RewardsSoundBuild       = true;  the chime
//   bool  RewardsSoundBuildFailed = true;  the failed-build tone
//   bool  RewardsSoundImport      = true;  the pop
//                                      Each tone's own switch, under
//                                      RewardsSound — which is the one that
//                                      starts off, so a fresh install still
//                                      hears nothing.
//   bool  EditorSoundsMuted       = false; the UI-sound engine. NOT under the
//                                      master: mutes whatever it plays
//   bool  RewardsShowProgress = true;  the footer counters. Off HIDES them;
//                                      counting goes on while the master is on,
//                                      so turning the display back on does not
//                                      find a streak that was broken by hiding
//                                      it.
// Wired in the six places every EditorConfig setting is, exactly like
// AutosaveEnabled: EditorConfig.h (field) · EditorApplication.cpp load
// (getCustomConfigBool) · EditorApplication.cpp save (setCustomConfigEntry)
// · EditorSettingsCatalog.cpp (boolRow, category "Feedback", id "rewards")
// · EditorSettingsPanel.cpp (row("rewards", "Feedback", …)) · its "Restore
// Defaults". The catalog row's id argument must equal the panel's row() id, or
// pinning the setting to Quick Settings breaks. Each label also has an entry in
// EditorHelp.cpp ("Preferences/Feedback/…").
//
// ── The counters are NOT settings ────────────────────────────────────────────
// "Builds today", "last active day" and "days in a row" are state, not
// preferences: they go into GlobalState custom config keys (per user, across
// projects), NOT into EditorConfig — anything in EditorConfig is in the settings
// catalog and therefore writable through MCP settings_set. The keys:
//   RewardsDay (YYYY-MM-DD, local time), RewardsBuildsToday, RewardsStreakDays.
// Loaded once, on first use; written through (writeConfig) only when a moment
// changed them — the first moment of a day and each build, a handful of writes
// a session. No globalState (tests): counted in memory, never written.
//
// Rules (recordUse / progressText, tested with string dates):
//   • A day counts as "used" on its first MOMENT, not on editor start, so
//     leaving the editor open overnight does not extend a streak. Any of the
//     three moments counts; they are all counted before fire()'s once-per-frame
//     fold, so a build that lands in the same frame as a save is not lost.
//   • "Builds" are successful builds — the BuildSucceeded moments, one per run.
//   • First moment of a new day: the day before the stored one → streak + 1;
//     any other gap → streak 1; builds today back to 0.
//   • The clock behind the stored day (set back by hand, a flight west): the
//     tally is left alone, nothing counted — a streak is not worth a guess.
//   • A stored day that does not parse (hand-edited file): treated as no
//     previous day.
//   • Display: builds today are 0 unless the stored day IS today. The streak
//     still shows when the stored day was yesterday — it is not broken until
//     today ends without a moment — and is gone after that. "N days in a row"
//     only from 2 on; builds today always, once anything was ever counted
//     ("0 builds today" is the honest morning state). Never counted: nothing
//     shown, the label is plain "Ready".
namespace HE::Ed::Rewards
{
	enum class Moment { Saved, BuildSucceeded, AssetsImported };

	// One tone per moment, and one for a failed build, which is not a moment.
	enum class Tone { SaveTick, BuildChime, BuildFailed, ImportPop };
	Tone toneFor(Moment m);

	// ── The editor side (UI thread only) ─────────────────────────────────────

	// A moment happened. The single gate on RewardsEnabled — a call site never
	// checks the switch itself. count: how many assets an import brought in.
	void fire(AppContext& ctx, Moment m, int count = 1);

	// Once per frame, with BuildProgressDialog::outcome(): fires BuildSucceeded
	// for each run serial that finished successfully and plays the failed-build
	// tone for one that did not. Separate from the dialog so this file does not
	// link against it (he_tests builds it without). appFocused: some editor
	// window has keyboard focus (the build tones only play without). Also the
	// UI-sound engine's housekeeping: opened when a tone becomes possible,
	// closed when none is.
	void pollBuild(AppContext& ctx, unsigned long long run, bool finished, bool success,
	               bool appFocused);

	// The footer's centred status label: idleText with the progress counters
	// after it, or the moment's line while one is showing. Positions itself
	// (SameLine to the window's centre).
	void drawFooterStatus(AppContext& ctx, const char* idleText);

	// The settings' "Preview": one tone once at the current volume, past the
	// Feed, its gaps and the tone's own switch — the user asked for exactly
	// this sound, now. Silent while the editor's sounds are muted.
	void preview(AppContext& ctx, Tone t);

	// RewardsReducedMotion resolved: follow the system, or off.
	bool reducedMotion(const AppContext& ctx);

	// The system's reduce-motion setting. false for now: the macOS/Windows
	// query is topic 95, step 4.
	bool systemReducesMotion();

	// ── The core, ImGui-free — the tests drive it with their own clock ───────

	inline constexpr double kHoldSec        = 0.9;    // the line at full strength
	inline constexpr double kFadeSec        = 0.7;    // …then back to the idle text
	inline constexpr double kToneGapSec     = 2.0;    // between any two tones
	inline constexpr double kSaveToneGapSec = 20.0;   // between two save tones

	// What the footer says for a moment. count only matters for imports.
	std::string lineFor(Moment m, int count);

	// 1 while holding, easing to 0 at kHoldSec + kFadeSec, 0 after; 1 before 0.
	float strengthAt(double age);

	// The volume slider (0..1) as a playback gain: squared, clamped to 0..1.
	float gainFor(float volume);

	// Build > Import > Save (rule 3 above).
	int rankOf(Moment m);

	// Rule 6: may this tone sound at all? Master, Sound, not muted, the tone's
	// own switch, volume > 0, not during Play, and for the two build tones no
	// editor window focused. The Feed's merging, rank and gaps come after.
	bool toneWanted(const EditorConfig& cfg, Tone t, bool playing, bool appFocused);

	// Whether any tone could play under these switches — what keeps the
	// UI-sound engine's device open. Volume is left out on purpose: dragging
	// the slider through zero must not close and reopen a device.
	bool uiSoundPossible(const EditorConfig& cfg);

	class Feed
	{
	public:
		struct Taken
		{
			bool taken = false;   // false: folded into a moment of the same frame
			bool shown = false;   // the footer line is (now) this moment's
			bool sound = false;   // play its tone now — the gaps are booked
		};

		// Take a moment at `now` (seconds) in frame `frame`. soundWanted: the
		// caller's switches allow a tone for it (rule 6); the Feed then applies
		// merging, rank and the gaps. See "Several moments close together".
		Taken take(Moment m, int count, double now, int frame, bool soundWanted);

		// take() without a tone. false = swallowed: a moment already arrived in
		// this frame (see "Once" above).
		bool push(Moment m, int count, double now, int frame)
		{
			return take(m, count, now, frame, false).taken;
		}

		// Rules 4 and 5 alone: may a tone play at `now`? true books the gaps.
		// take() uses it for a moment's tone, pollBuild for the failed build.
		bool takeTone(Tone t, double now);

		// The build edge detector: Succeeded or Failed exactly once per run
		// serial that finished, None otherwise (running, nothing built, or a run
		// already reported).
		enum class BuildEnd { None, Succeeded, Failed };
		BuildEnd buildEnded(unsigned long long run, bool finished, bool success);

		// buildEnded() == Succeeded.
		bool buildSucceeded(unsigned long long run, bool finished, bool success)
		{
			return buildEnded(run, finished, success) == BuildEnd::Succeeded;
		}

		struct Look
		{
			bool        active   = false;  // false = show the idle text
			std::string line;
			float       strength = 0.0f;   // 1 → 0: colour/alpha of the line
			float       bar      = 0.0f;   // 1 → 0: the shrinking underline
		};
		Look look(double now) const;

	private:
		bool               m_has       = false;
		Moment             m_moment    = Moment::Saved;
		int                m_count     = 0;
		double             m_at        = 0.0;
		int                m_lastFrame = -1;
		unsigned long long m_lastRun   = 0;
		bool               m_toned     = false;   // a tone has played at all
		double             m_toneAt    = 0.0;     // …and when the last one did
		bool               m_saveToned = false;
		double             m_saveToneAt = 0.0;
	};

	// ── Progress: the persistent counters (rules above) ──────────────────────

	struct Tally
	{
		std::string day;              // YYYY-MM-DD of the last counted moment, "" = never
		int         buildsToday = 0;  // successful builds on `day`
		int         streakDays  = 0;  // consecutive days ending with `day`
	};

	// "2026-03-01" → "2026-02-28"; "" if ymd is not a valid YYYY-MM-DD.
	std::string dayBefore(const std::string& ymd);

	// A moment on `today` (build = it was a BuildSucceeded). true = the tally
	// changed and wants writing.
	bool recordUse(Tally& t, const std::string& today, bool build);

	// "3 builds today · 5 days in a row", or "" when there is nothing to show.
	std::string progressText(const Tally& t, const std::string& today);

	// Today's local date as YYYY-MM-DD.
	std::string localDay();

	// ── The tones: mono int16 PCM, see "The tones" above ─────────────────────
	// Each is built once per rate and cached by the editor side. Empty for a
	// sampleRate <= 0.

	// Build: two short decaying sine notes a fifth apart, 0.42 s.
	std::vector<uint8_t> chimePcm16(int sampleRate);
	// Save: S1, the tick.
	std::vector<uint8_t> saveTickPcm16(int sampleRate);
	// Build failed: two notes a fourth down.
	std::vector<uint8_t> buildFailedPcm16(int sampleRate);
	// Import: I1, the pop.
	std::vector<uint8_t> importPopPcm16(int sampleRate);
	// The one for `t`.
	std::vector<uint8_t> tonePcm16(Tone t, int sampleRate);
}
