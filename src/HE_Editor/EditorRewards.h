#pragma once

// ── Reward moments: small, optional feedback for work that just went right ────
// Topic 75 ("Gamification"). THIS FILE IS THE PLAN, NOT THE IMPLEMENTATION: it
// declares nothing and nothing includes it yet. It records which editor events
// get a reward, where exactly each one is hooked, what the feedback looks like
// and where the switch lives, so the steps that build it do not have to rediscover
// any of it. Every hook site named below carries a one-line
// "Reward moment (EditorRewards.h)" marker comment pointing back here.
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
//      • doSaveActiveTab       — Ctrl/Cmd+S on an asset tab (saveAsset true,
//                                incl. the Skeletal Mesh viewer's clip). Only if
//                                the tab HAD unsaved edits (tabHasUnsavedEdits
//                                before the call): saveAsset answers true for a
//                                view-only tab that had nothing to write, and a
//                                reward for a no-op keystroke is noise.
//      • doSaveAll             — ONE moment for the whole batch, not one per
//                                asset; fires if something was written and
//                                nothing failed. It calls doSaveScene itself, so
//                                that inner call must not fire a second moment.
//      • the "save, then act" guard (Save button of the unsaved-changes prompt)
//        goes through doSaveScene / saveAsset and therefore counts too — but the
//        feedback must not delay runGuardedAction.
//    Prerequisite: AppContext::saveSceneToPath is std::function<void(...)>
//    (EditorApplication.h), so the UI cannot see whether the scene save worked.
//    Change it to return bool (EditorApplication::saveSceneToPath already
//    does) rather than guessing from ctx.sceneDirty afterwards.
//    NOT a moment: the solo autosave (SceneAutosave — a recovery copy, it
//    bypasses saveSceneToPath on purpose), MCP saves, hc.save of the level script.
//
// 2. BUILD SUCCEEDED — a build finished without errors.
//    Both kinds report into BuildProgressDialog, but from different threads:
//    Game Logic calls Build::finish from GameLogicBuildPanel::render (UI thread),
//    the export calls it from its worker (ExportDialogPanel.cpp). So the hook is
//    ONE edge detector on the UI thread over BuildProgressDialog::snapshot():
//    fire when `finished && success` becomes true for a run it has not fired
//    for yet (a run is identified by the running→finished transition). That
//    covers both Kind::GameLogic and Kind::Export without touching the worker.
//    It runs every frame, not only when the footer draws (the project hub
//    skips the footer); a moment seen there is kept until the footer shows it.
//    Decision: an MCP-started Game Logic build (McpToolsBuild → the same
//    GameLogicBuildPanel::start) counts as well — it is the user's project and
//    the user sees the Build window finish; there is no separate path to skip.
//    A failed build gets NO reward feedback; the existing Problem notification
//    stays exactly as it is.
//
// 3. ASSETS IMPORTED — one or more source files became assets.
//    Exactly two callers of Importer::importSource exist:
//      • EditorUI.cpp, PendingFileOp::ImportAsset — File ▸ Import Asset batch.
//        ONE moment per batch, carrying the count (`imported`), only if > 0.
//      • ContentBrowserPanel.cpp, context menu "Import" — one file; fire only
//        when importSource returned true (the call site ignores it today apart
//        from the error log).
//    NOT a moment: Reimport (it refreshes an existing asset), collab-received
//    assets. An OS file drop is not an import at all — it is handed to the
//    preview's widget manager (EditorApplication.cpp, SDL_EVENT_DROP_COMPLETE).
//
// ── What the feedback is ─────────────────────────────────────────────────────
// Visual (on by default): the footer's centred "Ready" status label, which is
// static text today, briefly becomes the moment's line — "Saved", "Build
// succeeded", "Imported 12 assets" — in the success colour, fading back to
// "Ready" over ~1.5 s. No window, no popup, no focus change, no layout shift.
// Words, not a check-mark glyph: the editor font (Roboto Condensed Bold) is not
// known to carry U+2713, and a missing glyph renders as "?". If a mark is wanted,
// draw it with ImDrawList lines.
//
// Sound (OFF by default — an open-plan office is the normal case): a short
// chime synthesised in code as PCM16 and played through
// AudioEngine::play(pcm, rate, channels) on the master bus (""), so no asset has
// to ship. Known edges, all harmless: AudioEngine::init can fail (play returns 0,
// nothing happens); ending a play session calls stopAll(), which may cut a chime
// short; the project's master volume/mute applies to it.
//
// Progress display (on by default): in the same centred label while idle,
// e.g. "Ready · 3 builds today · day 5 in a row". NOT a new widget in the
// footer's right-anchored group — that chain is hand-maintained and every
// insertion edits every block to its left (see the comment there in EditorUI.cpp).
//
// ── The switches (EditorConfig) ──────────────────────────────────────────────
//   bool RewardsEnabled      = true;   master: off = no feedback, no progress,
//                                      no counting (nothing written either)
//   bool RewardsSound        = false;  the chime (only with the master on)
//   bool RewardsShowProgress = true;   the idle counters in the footer
// A new EditorConfig setting is wired in six places, exactly like
// AutosaveEnabled:
//   EditorConfig.h (field) · EditorApplication.cpp load (getCustomConfigBool)
//   · EditorApplication.cpp save (setCustomConfigEntry)
//   · EditorSettingsCatalog.cpp (boolRow, section e.g. "Feedback")
//   · EditorSettingsPanel.cpp (row("rewards", …)) · its "Restore Defaults".
// The catalog row's id argument must equal the panel's row() id, or pinning the
// setting to Quick Settings breaks.
//
// ── The counters are NOT settings ────────────────────────────────────────────
// "Builds today", "last active day" and "days in a row" are state, not
// preferences: they go into GlobalState custom config keys (per user, across
// projects), NOT into EditorConfig — anything in EditorConfig is in the settings
// catalog and therefore writable through MCP settings_set. Suggested keys:
//   RewardsDay (YYYY-MM-DD, local time), RewardsBuildsToday, RewardsStreakDays.
// A day counts as "used" on the first moment of that day, not on editor start,
// so leaving the editor open overnight does not extend a streak.
//
// ── Planned shape (next steps) ───────────────────────────────────────────────
//   namespace HE::Ed::Rewards {
//     enum class Moment { Saved, BuildSucceeded, AssetsImported };
//     void fire(AppContext& ctx, Moment m, int count = 1);  // UI thread only;
//                                                           // a no-op when off
//     void drawFooterStatus(AppContext& ctx);  // replaces the "Ready" label
//   }
// fire() is the single gate on RewardsEnabled, so a call site never checks the
// switch itself.
