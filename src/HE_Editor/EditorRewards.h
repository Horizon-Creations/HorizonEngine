#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct AppContext;

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
//    A failed build gets NO reward feedback; the existing Problem notification
//    stays exactly as it is.
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
// ── What the feedback is ─────────────────────────────────────────────────────
// Visual (on with the master switch): the footer's centred "Ready" label
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
// Sound (OFF by default — an open-plan office is the normal case): a short
// two-note chime synthesised in code as PCM16 (chimePcm16) and played through
// AudioEngine::play(pcm, rate, channels) on the master bus (""), so no asset has
// to ship. Known edges, all harmless: AudioEngine::init can fail (play returns 0,
// nothing happens); ending a play session calls stopAll(), which may cut a chime
// short; the project's master volume/mute applies to it.
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
//   bool RewardsEnabled      = true;   master: off = no feedback, no sound, no
//                                      progress shown and nothing counted
//   bool RewardsSound        = false;  the chime (only with the master on)
//   bool RewardsShowProgress = true;   the footer counters (only with the
//                                      master on). Off HIDES them; counting goes
//                                      on while the master is on, so turning the
//                                      display back on does not find a streak
//                                      that was broken by hiding it.
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

	// ── The editor side (UI thread only) ─────────────────────────────────────

	// A moment happened. The single gate on RewardsEnabled — a call site never
	// checks the switch itself. count: how many assets an import brought in.
	void fire(AppContext& ctx, Moment m, int count = 1);

	// Once per frame, with BuildProgressDialog::outcome(): fires BuildSucceeded
	// for each run serial that finished successfully. Separate from the dialog
	// so this file does not link against it (he_tests builds it without).
	void pollBuild(AppContext& ctx, unsigned long long run, bool finished, bool success);

	// The footer's centred status label: idleText with the progress counters
	// after it, or the moment's line while one is showing. Positions itself
	// (SameLine to the window's centre).
	void drawFooterStatus(AppContext& ctx, const char* idleText);

	// ── The core, ImGui-free — the tests drive it with their own clock ───────

	inline constexpr double kHoldSec = 0.9;   // the line at full strength
	inline constexpr double kFadeSec = 0.7;   // …then back to the idle text

	// What the footer says for a moment. count only matters for imports.
	std::string lineFor(Moment m, int count);

	// 1 while holding, easing to 0 at kHoldSec + kFadeSec, 0 after; 1 before 0.
	float strengthAt(double age);

	class Feed
	{
	public:
		// Take a moment at `now` (seconds) in frame `frame`. false = swallowed:
		// a moment already arrived in this frame (see "Once" above).
		bool push(Moment m, int count, double now, int frame);

		// The build edge detector. true exactly once per run serial that ended
		// in success; any finished run is consumed, success or not.
		bool buildSucceeded(unsigned long long run, bool finished, bool success);

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

	// The chime: mono int16 PCM, two short decaying sine notes a fifth apart,
	// well under half a second. Built once and cached by fire().
	std::vector<uint8_t> chimePcm16(int sampleRate);
}
