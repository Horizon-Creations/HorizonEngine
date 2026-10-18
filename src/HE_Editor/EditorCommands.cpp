#include "EditorCommands.h"

#include "EditorUndo.h"
#include "CollabUndo.h"
#include "StructuralSync.h"

#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/EntityIdComponent.h>

#include <cstdio>
#include <cstring>

namespace HE::Ed
{

const char* errorName(CmdError e)
{
	switch (e)
	{
	case CmdError::None:          return "ok";
	case CmdError::NoWorld:       return "no_world";
	case CmdError::NotFound:      return "not_found";
	case CmdError::Builtin:       return "builtin";
	case CmdError::PlayMode:      return "play_mode";
	case CmdError::InvalidPayload:return "invalid_payload";
	case CmdError::LockedByOther: return "locked_by_other";
	case CmdError::LockPending:   return "lock_pending";
	case CmdError::Failed:        return "failed";
	}
	return "failed";
}

// ── Constructors ─────────────────────────────────────────────────────────────

Command Command::create(Entity parent, std::vector<std::uint8_t> blob, bool preserveIds)
{
	Command c;
	c.kind        = CommandKind::CreateSubtree;
	c.parent      = parent;
	c.blob        = std::move(blob);
	c.preserveIds = preserveIds;
	return c;
}

Command Command::destroy(Entity target)
{
	Command c;
	c.kind   = CommandKind::DestroySubtree;
	c.target = target;
	return c;
}

Command Command::reparent(Entity target, Entity newParent)
{
	Command c;
	c.kind   = CommandKind::Reparent;
	c.target = target;
	c.parent = newParent;
	return c;
}

Command Command::setTransform(Entity target, const float v9[9])
{
	Command c;
	c.kind   = CommandKind::SetTransform;
	c.target = target;
	std::memcpy(c.transform, v9, sizeof(c.transform));
	return c;
}

Command Command::setComponents(Entity target, std::vector<std::uint8_t> blob)
{
	Command c;
	c.kind   = CommandKind::SetComponents;
	c.target = target;
	c.blob   = std::move(blob);
	return c;
}

// ── The two sinks ────────────────────────────────────────────────────────────

void SnapshotUndoSink::beginCommand(const Command& c, Origin)
{
	if (!m_undo) return;
	if (m_isPlaying && m_isPlaying()) return;
	// The kind IS the sentence for the history window: these are the five
	// entity gestures, and each says what it does.
	const char* label = "Edit Entity";
	switch (c.kind)
	{
	case CommandKind::CreateSubtree:  label = "Create Entity";  break;
	case CommandKind::DestroySubtree: label = "Delete Entity";  break;
	case CommandKind::Reparent:       label = "Reparent Entity"; break;
	case CommandKind::SetTransform:   label = "Set Transform";  break;
	case CommandKind::SetComponents:  label = "Edit Components"; break;
	}
	m_undo->snapshotNow(label);
}

void CollabUndoSink::recordCommand(const Recorded& r, Origin)
{
	if (!m_undo) return;
	switch (r.kind)
	{
	case CommandKind::CreateSubtree:
		m_undo->recordCreate(r.subject, r.subtree, r.parentAfter);
		break;
	case CommandKind::DestroySubtree:
		m_undo->recordDestroy(r.subject, r.subtree, r.parentBefore);
		break;
	case CommandKind::Reparent:
		m_undo->recordReparent(r.subject, r.parentBefore, r.parentAfter);
		break;
	case CommandKind::SetTransform:
		m_undo->recordTransform(r.subject, r.beforeTransform, r.afterTransform);
		break;
	case CommandKind::SetComponents:
		m_undo->recordComponents(r.subject, r.beforeComponents, r.afterComponents);
		break;
	}
}

// ── Helpers ──────────────────────────────────────────────────────────────────

std::uint64_t EditorCommands::subjectOf(Entity e)
{
	if (e == entt::null || !m_hooks.subjectFor) return 0;
	if (!m_world || !m_world->registry().valid(e)) return 0;
	return m_hooks.subjectFor(static_cast<std::uint32_t>(entt::to_integral(e)));
}

CmdError EditorCommands::checkLock(std::uint64_t subject, Origin origin)
{
	// A peer's edit was already agreed at the host — asking about the lock here
	// would refuse the very message that proves someone else holds it.
	if (origin == Origin::Remote) return CmdError::None;
	if (!m_hooks.inSession || !m_hooks.inSession()) return CmdError::None;
	if (origin == Origin::User && !m_lockUserCommands) return CmdError::None;
	if (subject == 0) return CmdError::None;

	if (m_hooks.ownsLock && m_hooks.ownsLock(subject)) return CmdError::None;
	if (m_hooks.lockedByOther && m_hooks.lockedByOther(subject))
		return CmdError::LockedByOther;

	// Nobody holds it as far as the replicated table knows, but the grant is a
	// host round trip. Applying optimistically is what the asset path does and
	// it is wrong here: an asset edit that loses the race can be reloaded from
	// disk, a destroyed entity cannot be un-destroyed by a banner.
	if (m_hooks.requestLock) m_hooks.requestLock(subject);
	return CmdError::LockPending;
}

// ── execute ──────────────────────────────────────────────────────────────────

Result EditorCommands::execute(const Command& cmd, Origin origin)
{
	Result res;

	if (!m_world) { res.error = CmdError::NoWorld; return res; }

	// Play-in-editor: the whole session runs without an undo system and the
	// world is restored from a snapshot on stop, so an external change would be
	// silently thrown away while leaving the scene marked dirty. A human is
	// allowed to do it anyway (Ctrl+D in PIE is a spawn, and the UI knows what
	// it is doing); a client that cannot see the screen is not.
	if (origin == Origin::External && m_hooks.isPlaying && m_hooks.isPlaying())
	{
		res.error = CmdError::PlayMode;
		return res;
	}

	auto& reg = m_world->registry();

	// Everything but a create names an entity that has to exist.
	if (cmd.kind != CommandKind::CreateSubtree)
	{
		if (cmd.target == entt::null || !reg.valid(cmd.target))
		{
			res.error = CmdError::NotFound;
			return res;
		}
		if (origin != Origin::Remote && m_world->isBuiltin(cmd.target))
		{
			res.error = CmdError::Builtin;
			return res;
		}
	}

	Recorded rec;
	rec.kind = cmd.kind;

	// The subject is read before the apply, because a destroy takes the entity
	// the mapping is derived from with it.
	const std::uint64_t subject =
		cmd.kind == CommandKind::CreateSubtree ? 0ull : subjectOf(cmd.target);
	rec.subject = subject;

	if (const CmdError lockErr = checkLock(subject, origin); lockErr != CmdError::None)
	{
		res.error = lockErr;
		return res;
	}

	// A destroy takes the whole subtree, and the host only checks the lock of the
	// entity the message names: a child another participant is editing would die
	// with an unheld parent here and stay alive everywhere else.
	if (cmd.kind == CommandKind::DestroySubtree && origin != Origin::Remote &&
	    m_hooks.inSession && m_hooks.inSession() && m_hooks.lockedByOther)
	{
		std::vector<Entity> stack;
		if (const auto* h = reg.try_get<HierarchyComponent>(cmd.target))
			stack.assign(h->children.begin(), h->children.end());
		while (!stack.empty())
		{
			const Entity e = stack.back();
			stack.pop_back();
			if (!reg.valid(e)) continue;
			if (const std::uint64_t s = subjectOf(e); s != 0 && m_hooks.lockedByOther(s))
			{
				res.error = CmdError::LockedByOther;
				return res;
			}
			if (const auto* h = reg.try_get<HierarchyComponent>(e))
				stack.insert(stack.end(), h->children.begin(), h->children.end());
		}
	}

	// Which stack this change belongs on. A peer's edit belongs on neither:
	// putting it on ours would let us "undo" their work.
	IUndoSink* sink = nullptr;
	if (origin != Origin::Remote)
	{
		const bool inSession = m_hooks.inSession && m_hooks.inSession();
		sink = inSession ? m_sessionSink : m_snapshotSink;
	}
	if (sink) sink->beginCommand(cmd, origin);

	switch (cmd.kind)
	{
	case CommandKind::CreateSubtree:  res = applyCreate    (cmd, origin, rec); break;
	case CommandKind::DestroySubtree: res = applyDestroy   (cmd, origin, rec); break;
	case CommandKind::Reparent:       res = applyReparent  (cmd, origin, rec); break;
	case CommandKind::SetTransform:   res = applyTransform (cmd, origin, rec); break;
	case CommandKind::SetComponents:  res = applyComponents(cmd, origin, rec); break;
	}

	if (!res.ok()) return res;

	if (sink) sink->recordCommand(rec, origin);
	if (m_hooks.onApplied) m_hooks.onApplied(cmd, origin, res);
	return res;
}

// ── The five operations ──────────────────────────────────────────────────────

Result EditorCommands::applyCreate(const Command& cmd, Origin origin, Recorded& rec)
{
	Result res;
	if (cmd.blob.empty()) { res.error = CmdError::InvalidPayload; return res; }

	SceneSerializer serializer;
	const Entity created =
		serializer.instantiatePrefab(*m_world, cmd.blob, cmd.parent, cmd.preserveIds);
	if (created == entt::null) { res.error = CmdError::Failed; return res; }

	res.root = created;

	// Recorded AFTER the apply, and re-serialised from what was actually made
	// rather than kept from the command. The command's blob is a template: with
	// preserveIds off — which is what duplicate and paste use, because two copies
	// of one thing are two identities — instantiatePrefab minted fresh uuids, so
	// the blob that went in names the SOURCE entity. Replaying it to redo would
	// have brought back a second copy of the original under the original's
	// identity and left the entity this command created gone for good.
	SceneSerializer capture;
	rec.subject      = subjectOf(created);
	rec.subtree      = capture.serializeSubtree(*m_world, created);
	rec.parentAfter  = subjectOf(cmd.parent);

	// Make whatever the new subtree references resident — see the hook's own
	// comment. Before afterCreate: a hook that reacts to the new entity (e.g.
	// a thumbnail refresh) should see a mesh that actually resolves.
	if (m_hooks.preloadAssetRefs) m_hooks.preloadAssetRefs();

	if (m_hooks.afterCreate) m_hooks.afterCreate(created, origin);
	return res;
}

Result EditorCommands::applyDestroy(const Command& cmd, Origin origin, Recorded& rec)
{
	Result res;

	// Capture before anything is touched: after destroyEntity the subtree cannot
	// be walked, and an entry without the blob is not an undo.
	SceneSerializer serializer;
	rec.subtree      = serializer.serializeSubtree(*m_world, cmd.target);
	rec.parentBefore = subjectOf(structParentOf(m_world->registry(), cmd.target));

	// While the hierarchy that names them still exists — see
	// EditorApplication::deleteSelectedEntity.
	if (m_hooks.beforeDestroy) m_hooks.beforeDestroy(cmd.target, origin);

	m_world->destroyEntity(cmd.target);
	return res;
}

Result EditorCommands::applyReparent(const Command& cmd, Origin, Recorded& rec)
{
	Result res;
	auto& reg = m_world->registry();

	const Entity newParent =
		cmd.parent == entt::null ? m_world->rootEntity() : cmd.parent;
	if (!reg.valid(newParent)) { res.error = CmdError::NotFound; return res; }

	rec.parentBefore = subjectOf(structParentOf(reg, cmd.target));

	if (!m_world->reparentEntity(cmd.target, newParent))
	{
		res.error = CmdError::Failed;
		return res;
	}

	rec.parentAfter = subjectOf(newParent);
	return res;
}

Result EditorCommands::applyTransform(const Command& cmd, Origin origin, Recorded& rec)
{
	Result res;
	auto& reg = m_world->registry();

	auto* tc = reg.try_get<TransformComponent>(cmd.target);
	if (!tc) { res.error = CmdError::NotFound; return res; }

	const float before[9] = {
		tc->position.x, tc->position.y, tc->position.z,
		tc->rotation.x, tc->rotation.y, tc->rotation.z,
		tc->scale.x,    tc->scale.y,    tc->scale.z,
	};
	std::memcpy(rec.beforeTransform, before,       sizeof(before));
	std::memcpy(rec.afterTransform,  cmd.transform, sizeof(rec.afterTransform));

	tc->position = glm::vec3(cmd.transform[0], cmd.transform[1], cmd.transform[2]);
	tc->rotation = glm::vec3(cmd.transform[3], cmd.transform[4], cmd.transform[5]);
	tc->scale    = glm::vec3(cmd.transform[6], cmd.transform[7], cmd.transform[8]);
	tc->dirty    = true;   // or the renderer keeps the stale matrix

	// A peer already has this value; sending it back is the echo.
	if (origin != Origin::Remote && m_hooks.publishTransform &&
	    m_hooks.inSession && m_hooks.inSession() && rec.subject != 0)
	{
		const float p[3] = { cmd.transform[0], cmd.transform[1], cmd.transform[2] };
		const float r[3] = { cmd.transform[3], cmd.transform[4], cmd.transform[5] };
		const float s[3] = { cmd.transform[6], cmd.transform[7], cmd.transform[8] };
		m_hooks.publishTransform(rec.subject, p, r, s,
		                         m_hooks.nowMs ? m_hooks.nowMs() : 0ull);
	}
	return res;
}

Result EditorCommands::applyComponents(const Command& cmd, Origin origin, Recorded& rec)
{
	Result res;
	if (cmd.blob.empty()) { res.error = CmdError::InvalidPayload; return res; }

	SceneSerializer serializer;
	// The inverse of a partial component write is the SAME keys as they were, so
	// the whole entity is captured: applyEntityComponents overwrites what is
	// present and leaves the rest alone, which makes the full state a valid
	// "before" for any subset.
	rec.beforeComponents = serializer.serializeEntityComponents(*m_world, cmd.target);

	if (!serializer.applyEntityComponents(*m_world, cmd.target, cmd.blob))
	{
		res.error = CmdError::InvalidPayload;
		return res;
	}
	// The whole state after, like the before: a session undo applies these as a
	// replacement (both directions), and a patch would strip everything it omits.
	rec.afterComponents = serializer.serializeEntityComponents(*m_world, cmd.target);

	// A patch can plant a fresh asset UUID the same way a create can — see the
	// hook's own comment.
	if (m_hooks.preloadAssetRefs) m_hooks.preloadAssetRefs();

	if (auto* tc = m_world->registry().try_get<TransformComponent>(cmd.target))
		tc->dirty = true;

	// The entity's whole state after the write, not the patch: peers apply what
	// arrives as a replacement (SceneSerializer::replaceEntityComponents), so a
	// client's one-key patch sent as-is would strip every other component there.
	if (origin != Origin::Remote && m_hooks.publishComponents &&
	    m_hooks.inSession && m_hooks.inSession())
	{
		m_hooks.publishComponents(
			static_cast<std::uint32_t>(entt::to_integral(cmd.target)),
			serializer.serializeEntityComponents(*m_world, cmd.target));
	}
	return res;
}

// ── Addressing from outside ──────────────────────────────────────────────────

std::string uuidOf(HorizonWorld& world, Entity e)
{
	auto& reg = world.registry();
	if (e == entt::null || !reg.valid(e)) return {};
	const auto* c = reg.try_get<EntityIdComponent>(e);
	if (!c) return {};
	char buf[33];
	std::snprintf(buf, sizeof(buf), "%016llx%016llx",
	              static_cast<unsigned long long>(c->id.hi),
	              static_cast<unsigned long long>(c->id.lo));
	return std::string(buf);
}

Entity entityByUuid(HorizonWorld& world, const std::string& uuid)
{
	// The inverse of uuidOf's "%016llx%016llx", lowercase only: an upper-case
	// digit never equalled uuidOf's text, so it does not resolve here either.
	if (uuid.size() != 32) return entt::null;
	std::uint64_t parts[2] = { 0, 0 };
	for (std::size_t i = 0; i < 32; ++i)
	{
		const char c = uuid[i];
		unsigned v;
		if (c >= '0' && c <= '9')      v = static_cast<unsigned>(c - '0');
		else if (c >= 'a' && c <= 'f') v = static_cast<unsigned>(c - 'a' + 10);
		else return entt::null;
		parts[i / 16] = (parts[i / 16] << 4) | v;
	}
	HE::UUID id;
	id.hi = parts[0];
	id.lo = parts[1];
	return entityByUuid(world, id);
}

Entity entityByUuid(HorizonWorld& world, const HE::UUID& id)
{
	return world.findByEntityId(id);
}

} // namespace HE::Ed
