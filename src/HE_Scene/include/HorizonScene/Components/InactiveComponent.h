#pragma once

// ── Switched off ─────────────────────────────────────────────────────────────
// A tag, not data: an entity carrying it is one the user has switched off as a
// whole — the "Active" box at the top of the Details panel, unticked. Off
// means off for everything the entity does, not just for what it draws: the
// extractor skips its renderables, its script is not started, no physics body
// is built for it, its audio source does not play. That is the difference to
// the per-component `visible` flags (MeshComponent::visible and friends),
// which only ever meant "do not draw" and left a hidden entity's script
// running and its collider standing.
//
// Inherited down the hierarchy: an entity under an inactive parent is
// inactive too, the way a Unity GameObject's activeInHierarchy works — a
// switched-off room takes its furniture with it. HE::isEntityActive() in
// EntityActive.h is the one place that spells the rule out; systems ask it
// rather than testing for the tag themselves.
//
// Gameplay state, unlike EditorLockComponent: a prop that is off in the editor
// is off in the packaged game. So it lives in the components block of the
// scene format (key "inactive", an empty object), which makes it part of what
// prefabs diff and sync, what the clipboard carries and what collaboration
// replicates — an instance switched off while its template is on shows up as
// a whole-component override, exactly like any other component only one side
// has. Written only when set, so a scene where everything is on is byte-for-
// byte what it was.
//
// Known limit: the systems honour the tag when they START (play begins, a
// zone streams in). Flipping it while the game runs does not start a script
// late or tear a physics body down; that is the runtime half, not built yet.
struct InactiveComponent
{
};
