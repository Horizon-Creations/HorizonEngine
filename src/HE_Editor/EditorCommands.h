#pragma once

// ─── One way into the scene ──────────────────────────────────────────────────
// Until this file the editor had no single door a scene change came through.
// Three of them existed side by side and each was wired to undo and to
// collaboration on its own, or not at all:
//
//   • the UI's structural commands (duplicate / cut / paste / delete) took a
//     whole-world snapshot at every call site and relied on the entity-set diff
//     to tell the peers,
//   • the collab remote handlers applied a peer's edit straight to the registry
//     and deliberately recorded nothing,
//   • HE::api (the script/HorizonCode registry) wrote into components and knew
//     about neither.
//
// A fourth caller — the MCP bridge that lets an external client place and move
// objects — would have been a fourth wiring, and the one most likely to get it
// wrong, because nobody is watching the screen when it runs. So the wiring moves
// here instead: `execute(command, origin)` applies the change, records the undo
// entry that fits the situation, publishes what the session has not already
// seen, and tells the application what it has to react to.
//
// `Origin` is what makes one function serve all three callers. It is not a
// permission level; it answers "what already happened elsewhere":
//
//   User     — a human clicked. Undo yes, publish yes.
//   Remote   — a peer already did this and is telling us. Undo NO (undoing it
//              would revert someone else's work), publish NO (that is the echo
//              this editor must not send back).
//   External — an MCP client. Same as User, plus the checks a human gets from
//              the UI for free: not while playing, not a built-in.
//
// Deliberately free of ImGui, of SDL and of EditorApplication, so it lives in
// the test binary and the interesting question — "does undo put the world back
// exactly as it was" — is answerable without a window.
//
// Handles, not uuids: every caller in the editor already holds an entt handle,
// and the network subject is derived from the entity's uuid by the collab
// controller (`subjectFor`). An external caller addresses entities by uuid and
// resolves once, at the edge, through `entityByUuid` below.

#include <HorizonScene/HorizonWorld.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class EditorUndo;
class CollabUndo;

namespace HE::Ed
{

// ── Who is asking ────────────────────────────────────────────────────────────
enum class Origin : std::uint8_t
{
	User,       // a human, through the editor's own UI
	Remote,     // a peer in the session; already applied there
	External,   // an MCP client or another automated caller
};

// ── Why a command was refused ────────────────────────────────────────────────
// Every one of these is something a caller can act on, which is the point: an
// external client has to be able to tell "try again in a frame" (LockPending)
// from "never" (Builtin) without parsing a sentence.
enum class CmdError : std::uint8_t
{
	None = 0,
	NoWorld,          // no scene is open
	NotFound,         // the entity handle is not valid in this world
	Builtin,          // the environment sun/moon and friends are not editable
	PlayMode,         // refused while play-in-editor runs
	InvalidPayload,   // an empty or unreadable blob
	LockedByOther,    // someone else holds this subject in the session
	LockPending,      // our lock request is in flight; retry next frame
	Failed,           // the world refused the operation (cycle, parse failure…)
};

// The wire name of a refusal — what an external client reads. Deliberately NOT
// called toString: doctest stringifies an operand by calling an unqualified
// toString, so a free one in this namespace is found by ADL for every enum in
// it and hijacks the failure message with a const char* it then cannot add to.
const char* errorName(CmdError e);

// ── The commands ─────────────────────────────────────────────────────────────
// Five, and every one of them invertible from what `execute` records around it.
enum class CommandKind : std::uint8_t
{
	CreateSubtree,
	DestroySubtree,
	Reparent,
	SetTransform,
	SetComponents,
};

// One value type rather than a variant: the payload fields are few and small,
// the set is closed, and a plain struct is what the tests, the undo entries and
// the future MCP tool layer can all copy around without a visitor.
struct Command
{
	CommandKind kind = CommandKind::SetTransform;

	// The entity the command acts on. Unused by CreateSubtree.
	Entity target = entt::null;

	// CreateSubtree / Reparent: entt::null means the world root.
	Entity parent = entt::null;

	// CreateSubtree: the CBOR subtree from SceneSerializer::serializeSubtree.
	// SetComponents: the CBOR component state from serializeEntityComponents.
	std::vector<std::uint8_t> blob;

	// CreateSubtree only. Keeps the uuids stored in the blob instead of minting
	// fresh ones — right for a peer's create and for undoing a delete (the
	// entity must come back under the identity every later edit names), wrong
	// for duplicate and paste (two copies of one thing are two identities).
	bool preserveIds = false;

	// SetTransform: position(3) + rotation Euler degrees(3) + scale(3).
	float transform[9] {};

	static Command create(Entity parent, std::vector<std::uint8_t> blob, bool preserveIds);
	static Command destroy(Entity target);
	static Command reparent(Entity target, Entity newParent);
	static Command setTransform(Entity target, const float v9[9]);
	static Command setComponents(Entity target, std::vector<std::uint8_t> blob);
};

struct Result
{
	CmdError error = CmdError::None;
	// CreateSubtree: the new subtree root. entt::null for everything else.
	Entity   root  = entt::null;

	bool ok() const { return error == CmdError::None; }
	explicit operator bool() const { return ok(); }
};

// ── What one applied command recorded ────────────────────────────────────────
// Enough to invert it, expressed in network subjects rather than handles: a
// snapshot undo remaps every handle in the world, so an entry that named one
// would be pointing at a stranger by the time it is replayed.
struct Recorded
{
	CommandKind kind = CommandKind::SetTransform;

	std::uint64_t subject       = 0;   // the entity acted on
	std::uint64_t parentBefore  = 0;   // 0 = world root
	std::uint64_t parentAfter   = 0;

	// CreateSubtree / DestroySubtree: the blob that puts the subtree back.
	std::vector<std::uint8_t> subtree;

	float beforeTransform[9] {};
	float afterTransform[9]  {};

	std::vector<std::uint8_t> beforeComponents;
	std::vector<std::uint8_t> afterComponents;
};

// ── Where an undo entry goes ─────────────────────────────────────────────────
// Two stacks exist and they are mutually exclusive (EditorUndo.h, CollabUndo.h):
// whole-world snapshots outside a session, inverse operations inside one. The
// gateway is the place where that choice is made once instead of at every call
// site, and the interface is what lets a test count entries without either.
class IUndoSink
{
public:
	virtual ~IUndoSink() = default;

	// Before the world is touched. The snapshot stack captures here, because a
	// snapshot has to be taken of the state that is about to be replaced.
	virtual void beginCommand(const Command&, Origin) {}

	// After a successful apply. The inverse stack records here, because the
	// "after" side of an entry does not exist until then.
	virtual void recordCommand(const Recorded&, Origin) {}
};

// The snapshot stack (outside a session). Skips while play-in-editor runs, for
// the reason spelled out at EditorApplication::duplicateSelectedEntity: the play
// session has no undo system at all, and pushing an entry there left the scene
// falsely marked dirty for a change play-stop had already thrown away.
class SnapshotUndoSink final : public IUndoSink
{
public:
	SnapshotUndoSink(EditorUndo* undo, std::function<bool()> isPlaying)
		: m_undo(undo), m_isPlaying(std::move(isPlaying)) {}

	void beginCommand(const Command&, Origin) override;

private:
	EditorUndo*           m_undo = nullptr;
	std::function<bool()> m_isPlaying;
};

// The inverse-operation stack (inside a session).
class CollabUndoSink final : public IUndoSink
{
public:
	explicit CollabUndoSink(CollabUndo* undo) : m_undo(undo) {}

	void recordCommand(const Recorded&, Origin) override;

private:
	CollabUndo* m_undo = nullptr;
};

// ── The gateway ──────────────────────────────────────────────────────────────
class EditorCommands
{
public:
	// Everything the gateway needs from the rest of the editor, as functions
	// rather than as a CollabController pointer: the network half is a session
	// object with sockets in it, and the questions this class asks it are four
	// predicates and two sends. Passing those directly is what lets a test
	// assert "Origin::Remote published nothing" with a counter.
	struct Hooks
	{
		std::function<bool()>          isPlaying;   // play-in-editor runs
		std::function<bool()>          inSession;   // a collab session is live
		std::function<std::uint64_t()> nowMs;

		// Handle → wire subject (uuid-derived) and back.
		std::function<std::uint64_t(std::uint32_t)> subjectFor;

		// Session locks. `lockedByOther` answers from the replicated table and
		// is synchronous; `requestLock` starts a host round trip.
		std::function<bool(std::uint64_t)> ownsLock;
		std::function<bool(std::uint64_t)> lockedByOther;
		std::function<bool(std::uint64_t)> requestLock;

		// Live deltas. Structural changes are deliberately NOT published here:
		// EditorApplication::syncStructuralChanges diffs the entity set and is
		// complete by construction, so a second create message would arrive as
		// a duplicate on every peer.
		std::function<void(std::uint64_t, const float[3], const float[3],
		                   const float[3], std::uint64_t)> publishTransform;
		std::function<void(std::uint32_t, const std::vector<std::uint8_t>&)>
		                   publishComponents;

		// ── Observers ────────────────────────────────────────────────────────
		// beforeDestroy runs while the subtree still exists: the physics bodies
		// have to go first, because after destroyEntity the hierarchy that names
		// them is gone (see EditorApplication::deleteSelectedEntity).
		std::function<void(Entity, Origin)> beforeDestroy;
		std::function<void(Entity, Origin)> afterCreate;
		std::function<void(const Command&, Origin, const Result&)> onApplied;
	};

	void setWorld(HorizonWorld* world) { m_world = world; }
	HorizonWorld* world() const { return m_world; }

	void setHooks(Hooks h) { m_hooks = std::move(h); }
	Hooks& hooks() { return m_hooks; }

	// The two stacks. Which one is used per command follows inSession(); both
	// may be null, which is the correct state outside a project.
	void setUndoSinks(IUndoSink* outsideSession, IUndoSink* inSession)
	{
		m_snapshotSink = outsideSession;
		m_sessionSink  = inSession;
	}

	// Should `Origin::User` be refused when it does not hold the session lock?
	//
	// Off, and deliberately so: the editor's own structural commands never
	// checked locks, and turning the check on here would refuse a delete in the
	// one frame between clicking an entity and the host granting the lock —
	// a behaviour change dressed as a refactor. External callers are gated
	// regardless (they have no selection to claim a lock through). Switching
	// this on is a decision for the step that gives the UI something to say
	// when the answer is "not yet".
	void setLockUserCommands(bool on) { m_lockUserCommands = on; }

	Result execute(const Command& cmd, Origin origin);

private:
	Result applyCreate    (const Command&, Origin, Recorded&);
	Result applyDestroy   (const Command&, Origin, Recorded&);
	Result applyReparent  (const Command&, Origin, Recorded&);
	Result applyTransform (const Command&, Origin, Recorded&);
	Result applyComponents(const Command&, Origin, Recorded&);

	// The lock gate from step 2 of `execute`. Returns None when the command may
	// proceed.
	CmdError checkLock(std::uint64_t subject, Origin origin);

	std::uint64_t subjectOf(Entity e);

	HorizonWorld* m_world         = nullptr;
	IUndoSink*    m_snapshotSink  = nullptr;
	IUndoSink*    m_sessionSink   = nullptr;
	Hooks         m_hooks;
	bool          m_lockUserCommands = false;
};

// ── Addressing an entity from outside ────────────────────────────────────────
// A linear scan over EntityIdComponent. No index, on purpose: an index has to be
// kept honest across load, undo (which remaps every handle) and prefab
// instantiation, and the only caller is a tool handler running once per request
// on a world of a few thousand entities.
Entity entityByUuid(HorizonWorld& world, const std::string& uuid);
std::string uuidOf(HorizonWorld& world, Entity e);

} // namespace HE::Ed
