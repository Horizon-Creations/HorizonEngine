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
// ── The six moments ──────────────────────────────────────────────────────────
// All six fire on the UI thread, once per USER action, and only on success.
// 1–3 are topic 75's; 4–6 came with topic 140 (docs/editor-feinschliff-plan.md).
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
// 4. COMPILED CLEAN — the Compile button of a HorizonCode graph found nothing
//    that would keep the class from shipping compiled. Line "Compiles clean".
//      • LevelScriptPanel.cpp, runCompileCheck's clean branch (level script,
//        Game Instance, HC classes). That code has no AppContext, so it uses
//        post() — fire(), one frame later (see post below).
//      • UIEditorPanel.cpp, the widget graph's Compile button — fire() direct.
//    A compile with a fallback is NOT a moment: the graph already jumps to the
//    offending node. NO focus rule: the check runs synchronously on the click,
//    so the editor always has focus when it ends — a focus gate would mute it
//    for good. Counted as "used" (the streak), NOT as a build.
//
// 5. COMMITTED — a commit and/or push the user started in the Source Control
//    panel went through. Line "Committed", "Pushed" or "Committed and pushed";
//    count carries which as flags (kSyncCommit | kSyncPush), and a merge (rule
//    2) ORs them, so Commit then Push within the hold reads "Committed and
//    pushed". Detected by SyncWatch inside GitController (ImGui-free, tested):
//    armed by requestCommitAll / requestPush — only when the service really
//    queued the request (busy() right after it) — and judged on the frame the
//    service was idle BEFORE its pump (the m_cloneBusy trap in GitController.h:
//    idle after the pump can still have the result waiting), with no
//    lastError and a lastInfo. Its kind comes from the REQUEST, not from
//    parsing lastInfo: a commit with Auto Push and a remote is "committed and
//    pushed". EditorApplication takes it after m_git.update (takeSyncMoment)
//    and post()s it — there is no AppContext at that point. Pull and fetch are
//    not moments; a commit whose auto-push failed is an error and no moment.
//    Same rank as a build; counted as "used" and, for the commit part, in the
//    day's commits (RewardsCommitsToday, the tooltip only — not the footer).
//
// 6. TOUR FINISHED — the interactive tutorial's last step is done. Fires in
//    TutorialPanel.cpp where the cursor moves from the last step to finished:
//    the Finish button and the auto-advance after a done step, the two places
//    that call advance. Once per run through the tour: Back and Start Over do
//    not fire, and a finished tour has no step left to advance from. Not per
//    step — a step has its own "Done." and auto-advance. Counted as "used".
//
// Each of 4–6 has a switch of its own (RewardsMoment*, below) that turns off
// only the moment — line and tone; it is still counted while the master is on.
//
// post(): fire() deferred to the next pollBuild, i.e. the start of the next
// frame, for a hook without an AppContext (moments 4 in LevelScriptPanel and
// 5). Not a second gate — fire() still
// decides everything. A side effect worth knowing: the deferred moment takes
// the NEXT frame's once-per-frame slot (rule 1), so a Cmd+S in exactly that
// frame is counted but not shown. Compile outranks Save, so the line would be
// Compile's anyway.
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
//   3. RANK Build = Commit = Tour > Import > Compile > Save: a lower moment
//      does not replace a higher line that still shows — it is counted (the
//      tally ran before the Feed), not shown, never heard, and the hold is NOT
//      restarted. A higher one replaces a lower line and may sound under 4 and
//      5. EQUAL rank, other kind (a commit landing on a build's line): the
//      newer replaces it, like a higher one. Once a line has faded out,
//      anything replaces it.
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
// carry U+2713, and a missing glyph renders as "?" — the check beside the
// line (V1, below) is drawn, not typed. The editor draws every
// frame (only the game sets Application::setEventDriven), so the fade needs no
// redraw request.
//
// ── The visual cues (topic 95, step 4) ───────────────────────────────────────
// Each has its own switch; each is drawn with ImDrawList — no glyph, no icon
// font (the editor ImGui loads none), no layout change, no window.
//   V1  CHECK MARK (RewardsCheckMark, part of the line): a tick drawn left of
//       the moment's line, "written" as a two-stroke polyline over
//       kCheckDrawSec, then fading with the line. A second, colour-free sign
//       for "this worked" next to the green.
//   V2b LIGHT EDGE (RewardsLightEdge, part of the line): one 1-px line along
//       the footer's top edge that spreads from the middle outwards and fades
//       over kEdgeSec. One pulse per line, never a blink.
//       V1 and V2b run on the line's own clock (Look::lineAge): a MERGED
//       moment (rule 2) restarts the hold but does not re-write the check or
//       pulse the edge again — the second Cmd+S in a row is quiet to the eye too.
//   V3  COUNTER TICK (RewardsCounterTick, part of the counters): when "3 builds
//       today" becomes 4, or the streak grows, only that number lights up in
//       the green for kTickSec and the old number rolls up out of the way over
//       kRollSec. It waits until the moment's line is gone (the counters are
//       hidden behind it) and shows the OLD number until then, so it never
//       rolls from a value that was already on screen. Deliberately NOT a
//       progress bar: a bar needs a goal ("5/10 builds"), and a goal is the
//       first step to levels and daily targets.
//   V4  TAB CHECK (RewardsTabCheck): a tab's unsaved marker " *" turns into a
//       small drawn check for kTabCheckSec when that tab went from unsaved to
//       saved within kSaveMatchSec of a Saved moment (SaveMarks). Where the
//       user is looking anyway. Undo back to clean is not a save: no check.
//       Edited again while the check shows: the marker is back at once.
//   V5  FRESH IMPORTS (RewardsImportHighlight): assets an import just wrote get
//       a frame in the Content Browser that holds and fades over
//       kImportHoldSec + kImportFadeSec, counted from the first frame the tile
//       is actually on screen (FreshImports), for up to kImportWindowSec after
//       the import. "Just wrote" = new or rewritten in the import's target
//       folder, from a listing before and after (DirSnapshot) — importSource
//       does not say what it wrote, and a file clock would be a guess.
//   RECENT DAYS (RewardsStreakTooltip, part of the counters): hovering the
//       counters — and only hovering, never on its own — shows the last
//       kRecentDays days: which had a moment, each day's builds and, where
//       there were any, its commits. Kept in
//       GlobalState key RewardsRecent next to the tally. Words stay neutral:
//       no "keep your streak", nothing that asks for tomorrow.
//
// ── Reduced motion ───────────────────────────────────────────────────────────
// RewardsReducedMotion: follow the system (macOS "Reduce motion", Windows
// "Show animations in Windows" off) or off (always the full motion). There
// is no "always reduce" — the system switch is where that lives. Reduced:
// the check marks (V1, V4) appear whole at once, no light edge (V2b), no
// rolling digits (V3 still lights up — a colour change is not motion), no
// shrinking underline. Fades stay: they do not move anything. The system is
// asked through a hook the editor sets (setSystemMotionQuery — the Cocoa and
// Win32 calls live in EditorSystemMotion.*, outside he_tests), at most once a
// second.
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
//   Compiled clean, Committed, Tour finished: no tone YET (hasTone false) —
//   their tones are topic 140's next step. Until then such a moment is shown
//   and counted but never heard, and it must not borrow another tone's switch:
//   fire() asks hasTone before toneWanted. Giving one a tone = a Tone value,
//   its PCM, its switch, and hasTone/toneFor answering for it.
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
// EditorSoundsMuted is the switch for exactly this engine: it silences what it
// plays (today: these tones) and leaves each tone's own switch as it was. Clip
// auditions (Audio Editor) use the project's engine and are NOT affected.
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
//   bool  RewardsCheckMark    = true;  V1, under Visual (it sits beside the line)
//   bool  RewardsLightEdge    = true;  V2b, under Visual (it pulses for the line)
//   bool  RewardsMomentCompile  = true;  moment 4 — line and (later) tone;
//   bool  RewardsMomentCommit   = true;  moment 5   counted either way while
//   bool  RewardsMomentTutorial = true;  moment 6   the master is on
//   bool  RewardsTabCheck     = true;  V4 — siblings of Visual: other places,
//   bool  RewardsImportHighlight = true; V5  not the footer line
//   int   RewardsReducedMotion = 0;    0 = follow the system's reduce-motion
//                                      setting, 1 = off (full motion). See
//                                      "Reduced motion" above.
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
//                                      master: mutes whatever it plays (not
//                                      clip auditions — project engine)
//   bool  RewardsShowProgress = true;  the footer counters. Off HIDES them;
//                                      counting goes on while the master is on,
//                                      so turning the display back on does not
//                                      find a streak that was broken by hiding
//                                      it.
//   bool  RewardsCounterTick  = true;  V3, under Show Progress
//   bool  RewardsStreakTooltip = true; the recent-days tooltip, under Show
//                                      Progress (no counters, nothing to hover)
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
//   RewardsDay (YYYY-MM-DD, local time), RewardsBuildsToday, RewardsStreakDays,
//   RewardsCommitsToday, RewardsRecent ("YYYY-MM-DD:builds[:commits],…", the
//   last kRecentDays days that had a moment, oldest first — for the tooltip).
//   The commits field is written only for a day that had one, so a file a
//   day without commits leaves behind is still read by an editor from before
//   it (which drops an entry it cannot parse rather than guess). A tally from
//   before RewardsRecent existed is seeded from its streak (seedRecent): the
//   streak's days were used, their builds unknown except today's.
// Loaded once, on first use; written through (writeConfig) only when a moment
// changed them — the first moment of a day and each build, a handful of writes
// a session. No globalState (tests): counted in memory, never written.
//
// Rules (recordUse / progressText, tested with string dates):
//   • A day counts as "used" on its first MOMENT, not on editor start, so
//     leaving the editor open overnight does not extend a streak. Any of the
//     six moments counts, its own switch on or off; they are all counted
//     before fire()'s once-per-frame fold, so a build that lands in the same
//     frame as a save is not lost.
//   • "Builds" are successful builds — the BuildSucceeded moments, one per run.
//     A clean compile is not a build.
//   • "Commits" are Committed moments with the commit flag; a push alone
//     counts the day as used and nothing else. Shown only in the tooltip.
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
	enum class Moment { Saved, BuildSucceeded, AssetsImported,
	                    CompiledClean, Committed, TourFinished };

	// Committed's count: which of the two went through (OR-ed on a merge).
	inline constexpr int kSyncCommit = 1;
	inline constexpr int kSyncPush   = 2;

	// One tone per topic-75 moment, and one for a failed build, which is not a
	// moment. Moments 4–6 have none yet (see "The tones").
	enum class Tone { SaveTick, BuildChime, BuildFailed, ImportPop };
	bool hasTone(Moment m);
	Tone toneFor(Moment m);   // only meaningful where hasTone(m)

	// ── The editor side (UI thread only) ─────────────────────────────────────

	// A moment happened. The single gate on RewardsEnabled — a call site never
	// checks the switch itself. count: how many assets an import brought in;
	// for Committed, kSyncCommit | kSyncPush.
	void fire(AppContext& ctx, Moment m, int count = 1);

	// fire(), deferred to the next pollBuild — for a hook that has no
	// AppContext (see "post()" above). A handful at most; queued in order.
	void post(Moment m, int count = 1);

	// Once per frame, with BuildProgressDialog::outcome(): fires BuildSucceeded
	// for each run serial that finished successfully and plays the failed-build
	// tone for one that did not. Separate from the dialog so this file does not
	// link against it (he_tests builds it without). appFocused: some editor
	// window has keyboard focus (the build tones only play without). Also the
	// UI-sound engine's housekeeping: opened when a tone becomes possible,
	// closed when none is. And where post()'s queue is fired.
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

	// The system's reduce-motion setting, through the query the editor set
	// (setSystemMotionQuery), asked at most once a second. false without one.
	bool systemReducesMotion();
	// nullptr = no system to ask. Setting it forgets the cached answer.
	void setSystemMotionQuery(bool (*query)());

	// V4, once per tab per frame from the tab bar: key = the tab's asset path
	// ("" = the scene tab), dirty = what its marker says this frame.
	struct TabMark
	{
		bool  check  = false;  // draw the check instead of the " *"
		float stroke = 0.0f;   // 0..1 of the check written
		float alpha  = 0.0f;
	};
	TabMark tabMark(AppContext& ctx, const std::string& key, bool dirty);

	// A check mark in the "done" green into the current window's draw list:
	// its box at (x, y), size × size. What V1 and V4 draw.
	void drawCheckMark(float x, float y, float size, float stroke, float alpha);

	// V5. Before an import into `dir`: a listing to compare against (empty —
	// and free — while the highlight is off). After it: the files that are new
	// or rewritten since get their frame. strength: 0..1 for a Content Browser
	// tile, 0 for anything that was not just imported.
	struct DirSnapshot;
	DirSnapshot importSnapshot(const AppContext& ctx, const std::string& dir);
	void markImported(AppContext& ctx, const DirSnapshot& before);
	float importHighlight(AppContext& ctx, const std::string& fullPath);

	// ── The core, ImGui-free — the tests drive it with their own clock ───────

	inline constexpr double kHoldSec        = 0.9;    // the line at full strength
	inline constexpr double kFadeSec        = 0.7;    // …then back to the idle text
	inline constexpr double kToneGapSec     = 2.0;    // between any two tones
	inline constexpr double kSaveToneGapSec = 20.0;   // between two save tones
	inline constexpr double kCheckDrawSec   = 0.15;   // V1/V4: the check is written
	inline constexpr double kEdgeSec        = 0.6;    // V2b: spread + fade of the edge
	inline constexpr double kTickSec        = 0.6;    // V3: a number lit up
	inline constexpr double kRollSec        = 0.25;   // V3: …the old one rolling away
	inline constexpr double kTabCheckSec    = 0.6;    // V4: the tab's check shows
	inline constexpr double kSaveMatchSec   = 0.5;    // V4: dirty → clean this close to a save
	inline constexpr double kImportHoldSec  = 1.2;    // V5: the tile's frame, full
	inline constexpr double kImportFadeSec  = 0.8;    // V5: …then gone
	inline constexpr double kImportWindowSec = 30.0;  // V5: to come on screen at all
	inline constexpr int    kRecentDays     = 7;      // the tooltip's days

	// What the footer says for a moment. count only matters for imports and
	// for Committed (its flags).
	std::string lineFor(Moment m, int count);

	// The moment's own switch (RewardsMoment*; true for moments 1–3, which
	// have none): may it show and sound? Counting does not ask.
	bool momentWanted(const EditorConfig& cfg, Moment m);

	// 1 while holding, easing to 0 at kHoldSec + kFadeSec, 0 after; 1 before 0.
	float strengthAt(double age);

	// The volume slider (0..1) as a playback gain: squared, clamped to 0..1.
	float gainFor(float volume);

	// Build = Commit = Tour > Import > Compile > Save (rule 3 above).
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
			double      lineAge  = 0.0;    // since this line began; a merge
			                               // (rule 2) does not restart it — V1/V2b
		};
		Look look(double now) const;

	private:
		bool               m_has       = false;
		Moment             m_moment    = Moment::Saved;
		int                m_count     = 0;
		double             m_at        = 0.0;
		double             m_since     = 0.0;     // the line began (not merges)
		int                m_lastFrame = -1;
		unsigned long long m_lastRun   = 0;
		bool               m_toned     = false;   // a tone has played at all
		double             m_toneAt    = 0.0;     // …and when the last one did
		bool               m_saveToned = false;
		double             m_saveToneAt = 0.0;
	};

	// Moment 5's edge, git-free: GitController owns one (see "COMMITTED").
	class SyncWatch
	{
	public:
		// The user asked for a commit and/or push (kSyncCommit | kSyncPush) and
		// the service queued it. A second request before the first is judged
		// adds its flags: both are in the same queue and judged together.
		void requested(int flags);

		// Once per frame. idleBeforePump: the service was idle BEFORE this
		// frame's pump, so the pump got every result; lastError/lastInfo as
		// they are after it. Returns the flags to fire Committed with, once,
		// or 0 (nothing armed, still busy, or it failed).
		int poll(bool idleBeforePump, const std::string& lastError, const std::string& lastInfo);

		bool armed() const { return m_flags != 0; }

	private:
		int m_flags = 0;
	};

	// ── Visual cues, the parts with a clock (see "The visual cues") ──────────

	// V1/V4: how much of the check is written `age` s after it began — 0 → 1
	// over kCheckDrawSec, 1 at once when reduced.
	float checkStroke(double age, bool reduced);

	// The check as a polyline in its size × size box at (x, y), y down: the
	// short stroke down, then the long one up. stroke 0..1 by length; fewer
	// than two points = nothing to draw yet.
	struct Pt { float x = 0.0f, y = 0.0f; };
	std::vector<Pt> checkPolyline(float stroke, float x, float y, float size);

	// V2b at `age` s into the line: spread 0 → 1 (half-widths of the footer
	// from its middle), alpha one soft pulse to 0 at kEdgeSec. Reduced: none.
	struct Edge { float spread = 0.0f; float alpha = 0.0f; };
	Edge edgeAt(double age, bool reduced);

	// V3: one counter. observe() every frame with the counter's value; a rise
	// becomes a PENDING tick that start() — the frame the counters are on
	// screen again — sets going. A fall (a new day, a streak run out) and the
	// first value ever seen are not ticks.
	class CounterTick
	{
	public:
		void observe(int value);
		void start(double now);

		struct Look
		{
			int   shown = 0;      // the number to draw (the old one while pending)
			int   old   = 0;      // …rolling away while roll < 1
			float glow  = 0.0f;   // 1 → 0 over kTickSec: the green on the number
			float roll  = 1.0f;   // 0 → 1 over kRollSec; 1 at once when reduced
		};
		Look look(double now, bool reduced) const;

	private:
		bool   m_known   = false;
		bool   m_pending = false;
		int    m_from    = 0;
		int    m_to      = 0;
		double m_at      = -1.0;
	};

	// V4: the tabs' dirty → clean edges. update() once per tab per frame;
	// lastSaveAt = when the last Saved moment fired (< 0: never). Returns how
	// long the tab's check has shown, or -1 while it shows none.
	class SaveMarks
	{
	public:
		double update(const std::string& key, bool dirty, double now, double lastSaveAt);

	private:
		struct Tab { bool dirty = false; double checkAt = -1.0; };
		std::vector<std::pair<std::string, Tab>> m_tabs;   // a handful: a vector
	};

	// V5: a folder's files and their write times, before an import. `dir` is
	// empty for "not taken" (the highlight was off) — changedSince is then {}.
	struct DirSnapshot
	{
		std::string dir;
		std::vector<std::pair<std::string, long long>> files;   // name, write time
	};
	DirSnapshot snapshotDir(const std::string& dir);
	// Full paths (normalPath) of the files in before.dir that are new or were
	// written since `before` was taken.
	std::vector<std::string> changedSince(const DirSnapshot& before);
	// lexically_normal + generic separators: one spelling per file to compare.
	std::string normalPath(const std::string& p);

	class FreshImports
	{
	public:
		// These (normalPath) were just imported.
		void mark(const std::vector<std::string>& paths, double now);
		// The tile's frame, 0..1. The first call for a marked path starts its
		// clock — a tile scrolled into view after 5 s still gets its 2 s — as
		// long as that is within kImportWindowSec of the import.
		float strength(const std::string& path, double now);
		// Forget what never came on screen within kImportWindowSec — so the
		// Content Browser stops asking once nothing can light up any more.
		void  prune(double now);
		bool  empty() const { return m_marks.empty(); }

	private:
		struct Mark { std::string path; double markedAt = 0.0; double seenAt = -1.0; };
		std::vector<Mark> m_marks;
	};

	// ── Progress: the persistent counters (rules above) ──────────────────────

	struct DayUse
	{
		std::string day;       // YYYY-MM-DD with at least one moment
		int         builds  = 0;
		int         commits = 0;
	};

	struct Tally
	{
		std::string day;              // YYYY-MM-DD of the last counted moment, "" = never
		int         buildsToday  = 0; // successful builds on `day`
		int         streakDays   = 0; // consecutive days ending with `day`
		int         commitsToday = 0; // commits on `day` (the tooltip only)
		std::vector<DayUse> recent;   // the last kRecentDays days WITH a moment,
		                              // oldest first — the tooltip's history
	};

	// RewardsRecent's text form, and back. Entries that do not parse are dropped.
	std::string formatRecent(const std::vector<DayUse>& recent);
	std::vector<DayUse> parseRecent(const std::string& text);
	// A tally from before RewardsRecent: its streak's days (at most
	// kRecentDays), today's builds and commits on its own day, 0 on the others.
	void seedRecent(Tally& t);

	// 0 = Monday … 6 = Sunday; -1 if ymd is not a valid YYYY-MM-DD.
	int weekdayOf(const std::string& ymd);

	// The tooltip's row: the n days ending with `today`, oldest first.
	struct DayCell
	{
		std::string day;
		int         weekday = -1;
		bool        used    = false;   // a moment on that day
		int         builds  = 0;
		int         commits = 0;
	};
	std::vector<DayCell> recentDays(const Tally& t, const std::string& today, int n = kRecentDays);

	// What the footer shows, as numbers: any = something was ever counted;
	// builds = today's; streak = the days in a row, 0 while it is not shown
	// (below 2, or run out).
	struct Progress { bool any = false; int builds = 0; int streak = 0; };
	Progress progressOf(const Tally& t, const std::string& today);
	std::string buildsPhrase(int builds);   // "1 build today", "3 builds today"
	std::string streakPhrase(int days);     // "5 days in a row"

	// "2026-03-01" → "2026-02-28"; "" if ymd is not a valid YYYY-MM-DD.
	std::string dayBefore(const std::string& ymd);

	// A moment on `today`: m with its count (Committed's flags decide whether
	// it was a commit). true = the tally changed and wants writing.
	bool recordMoment(Tally& t, const std::string& today, Moment m, int count = 1);
	// The same for "a moment" (build = it was a BuildSucceeded) — the form the
	// topic-75 rules are written and tested in.
	bool recordUse(Tally& t, const std::string& today, bool build);

	// "1 commit today", "3 commits today" — the tooltip's line; "" for 0.
	std::string commitsPhrase(int commits);

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
