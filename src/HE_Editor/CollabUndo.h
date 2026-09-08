#pragma once

// ─── Per-user undo/redo for a shared session ─────────────────────────────────
// EditorUndo is snapshot-based: every entry is a full-world blob, and undo
// *restores the whole world*. In a session that would silently revert everyone
// else's concurrent work, and no amount of adjusting when it fires can fix it —
// the data model is wrong for the job.
//
// So while a session is running the editor uses this stack instead. An entry is
// an INVERSE OPERATION rather than a snapshot:
//
//     (subject, before, after)
//
// Undo re-applies `before` and publishes it like any ordinary edit; redo does
// the same with `after`. Nothing is "restored" — an undo is just another change,
// which is precisely what makes it compose with other people's work. Figma and
// collaborative text editors solve it the same way.
//
// **Locks are what make it sound.** You may only edit what you hold, so while an
// entry sits on your stack nobody else can have touched that subject, and
// inverting it cannot clobber anyone. The rule that follows: an entry is only
// valid while you still hold the lock on its subject. Release it (deselect, or
// someone takes it after you) and the entry is dropped, because the recorded
// `before` is no longer a truthful inverse.
//
// Each participant has their own stack, so everyone undoes *their own* changes —
// which is the behaviour users actually expect from a shared editor.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class CollabUndo
{
public:
	enum class Kind : std::uint8_t
	{
		Transform,   // entity transform: position / rotation(Euler) / scale
		Asset,       // an authored asset file's bytes
		// ── Structural, added with the command gateway ───────────────────────
		// Without these, a create, a delete or a re-parent made during a session
		// simply could not be taken back — the stack knew how to invert a move
		// and a file, and nothing else. That was survivable while only a human
		// with an outliner in front of them made those changes; it stops being
		// survivable when an external client makes them, because the person at
		// the keyboard did not do it and has no other way to undo it.
		Create,      // an entity subtree came into existence
		Destroy,     // an entity subtree went away
		Reparent,    // an entity moved in the hierarchy
		Components,  // an entity's component state was written
	};

	struct Entry
	{
		Kind          kind    = Kind::Transform;
		std::uint64_t subject = 0;

		// Transform payloads: 9 floats each (pos3 + rotEuler3 + scale3).
		float beforeTransform[9] {};
		float afterTransform[9]  {};

		// Asset payloads.
		std::string               path;
		std::vector<std::uint8_t> beforeBytes;
		std::vector<std::uint8_t> afterBytes;

		// Create / Destroy: the CBOR subtree blob that puts it back. Replayed
		// with preserved uuids, so the entity returns under the identity every
		// other entry and every peer names it by.
		std::vector<std::uint8_t> subtree;

		// Create / Destroy / Reparent: the parent's subject, 0 = world root.
		std::uint64_t parentBefore = 0;
		std::uint64_t parentAfter  = 0;
	};

	// How the stack reaches the world. Set once by the editor; applying is not
	// done here, so this class stays free of both the ECS and the network.
	using ApplyTransform = std::function<void(std::uint64_t, const float[9])>;
	using ApplyAsset     = std::function<void(const std::string&,
	                                          const std::vector<std::uint8_t>&)>;
	// Answers "do we still hold this subject?" — the validity rule above.
	using OwnsSubject    = std::function<bool(std::uint64_t)>;

	// The structural half. Separate from setHandlers because it arrived later and
	// because the two groups have different owners in the editor: the pair above
	// is a component write and a file write, these four go through the command
	// gateway (EditorCommands.h) so that undoing a delete is the same operation,
	// with the same publishing, as making one.
	//   ApplyCreate(subject, subtree, parentSubject) — instantiate with preserved
	//   uuids; ApplyDestroy(subject); ApplyReparent(subject, parentSubject, 0 =
	//   world root); ApplyComponents(subject, cbor).
	using ApplyCreate     = std::function<void(std::uint64_t,
	                                           const std::vector<std::uint8_t>&,
	                                           std::uint64_t)>;
	using ApplyDestroy    = std::function<void(std::uint64_t)>;
	using ApplyReparent   = std::function<void(std::uint64_t, std::uint64_t)>;
	using ApplyComponents = std::function<void(std::uint64_t,
	                                           const std::vector<std::uint8_t>&)>;

	void setHandlers(ApplyTransform t, ApplyAsset a, OwnsSubject owns)
	{
		m_applyTransform = std::move(t);
		m_applyAsset     = std::move(a);
		m_owns           = std::move(owns);
	}

	void setStructuralHandlers(ApplyCreate c, ApplyDestroy d,
	                           ApplyReparent r, ApplyComponents k)
	{
		m_applyCreate     = std::move(c);
		m_applyDestroy    = std::move(d);
		m_applyReparent   = std::move(r);
		m_applyComponents = std::move(k);
	}

	// Record a local change. Recording clears the redo stack, as undo stacks do.
	void recordTransform(std::uint64_t subject, const float before[9], const float after[9]);
	void recordAsset(std::uint64_t subject, const std::string& path,
	                 std::vector<std::uint8_t> before, std::vector<std::uint8_t> after);
	void recordCreate(std::uint64_t subject, std::vector<std::uint8_t> subtree,
	                  std::uint64_t parentSubject);
	void recordDestroy(std::uint64_t subject, std::vector<std::uint8_t> subtree,
	                   std::uint64_t parentSubject);
	void recordReparent(std::uint64_t subject, std::uint64_t parentBefore,
	                    std::uint64_t parentAfter);
	void recordComponents(std::uint64_t subject, std::vector<std::uint8_t> before,
	                      std::vector<std::uint8_t> after);

	bool canUndo() const { return !m_undo.empty(); }
	bool canRedo() const { return !m_redo.empty(); }

	// Human-readable label for the next undo/redo, so the menu can say what it
	// will actually do rather than a bare "Undo".
	std::string undoLabel() const;
	std::string redoLabel() const;

	bool undo();
	bool redo();

	void clear();

	// Drop every entry for subjects we no longer hold. Called when locks change.
	//
	// Create and Destroy entries are exempt, and they have to be: on one side of
	// such an entry the entity does not exist, so nobody holds a lock on it and
	// nobody can — the ownership test answers "no" for a reason that has nothing
	// to do with someone else having taken it. Applying the rule anyway made
	// structural undo stillborn: a delete released the lock on what it deleted,
	// so the entry it had just pushed was thrown away on the very next call, and
	// undoing a delete was impossible by construction. The soundness argument
	// the rule exists for holds trivially in that state — a subtree that is not
	// in the world cannot have been changed by anyone.
	void dropUnowned();

	std::size_t undoDepth() const { return m_undo.size(); }

private:
	bool applyEntry(const Entry& e, bool useBefore);
	void push(Entry&& e);
	static std::string labelFor(const Entry& e, bool isUndo);

	std::vector<Entry> m_undo;
	std::vector<Entry> m_redo;

	ApplyTransform  m_applyTransform;
	ApplyAsset      m_applyAsset;
	OwnsSubject     m_owns;
	ApplyCreate     m_applyCreate;
	ApplyDestroy    m_applyDestroy;
	ApplyReparent   m_applyReparent;
	ApplyComponents m_applyComponents;

	// A generous cap: asset entries carry two copies of the file, so an
	// unbounded stack would grow with every save.
	static constexpr std::size_t kMaxEntries = 64;
};
