#pragma once
#include <Types/Enums.h>
#include <cstdint>
#include <string>
#include <filesystem>
#include <vector>
#include <entt/entt.hpp>
#include "HorizonScene/Components/PrefabInstanceComponent.h"

class HorizonWorld;
class ContentManager;
using SerializeFormat = HE::SerializeFormat;
using Entity = entt::entity;

class SceneSerializer {
public:
    bool save(const HorizonWorld& world,
              const std::filesystem::path& path,
              SerializeFormat format);

    bool load(HorizonWorld& world,
              const std::filesystem::path& path,
              SerializeFormat format);

    // Additive load: merges the entities from a .hescene file into the existing
    // world without clearing it first. The loaded scene's root entity becomes
    // a new child of the current world root, preserving all existing entities.
    // outCreated (optional) receives every entity the merge created — the
    // runtime's zone system uses this to unload an additively-loaded scene again.
    bool loadAdditive(HorizonWorld& world,
                      const std::filesystem::path& path,
                      SerializeFormat format,
                      std::vector<Entity>* outCreated = nullptr);

    // Additive merge from an in-memory CBOR snapshot (a packed scene entry).
    bool loadAdditiveFromMemory(HorizonWorld& world,
                                const std::vector<uint8_t>& data,
                                std::vector<Entity>* outCreated = nullptr);

    // In-memory snapshot (CBOR, same structure as the binary file format).
    // Used by play-in-editor and the undo system. load does not clear the
    // world first — call HorizonWorld::clear() when replacing the content.
    bool saveToMemory(const HorizonWorld& world, std::vector<uint8_t>& out);
    bool loadFromMemory(HorizonWorld& world, const std::vector<uint8_t>& data);

    // Prefab serialization: capture an entity subtree (root + all descendants)
    // as a self-contained CBOR blob. Entities are remapped to contiguous IDs so
    // the prefab is independent of the source world's handle space.
    std::vector<uint8_t> serializeSubtree(const HorizonWorld& world, Entity root);

    // ── Single-entity component state (CBOR) ─────────────────────────────────
    // Capture or restore just the components of ONE existing entity, without
    // creating or destroying anything. This is what live collaboration
    // replicates for non-transform edits: a prefab blob would mint a new entity,
    // which is the wrong operation when the entity already exists on the peer
    // and is merely being edited.
    //
    // applyEntityComponents overwrites the components present in `data` and
    // leaves the rest alone, so it is an update rather than a replacement.
    std::vector<uint8_t> serializeEntityComponents(const HorizonWorld& world, Entity entity);
    bool applyEntityComponents(HorizonWorld& world, Entity entity,
                               const std::vector<uint8_t>& data);

    // Is this a component key the loader restores? Anything else in a scene is
    // data being dropped, and loading warns about it.
    //
    // Public because the list is hand-maintained and therefore drifts: adding a
    // component to the save AND load paths still leaves it looking unknown, and
    // the result is a warning that says a component was dropped when it was
    // not — noise that trains people to ignore a message meant to catch real
    // loss. A test walks a world holding every component and asserts this
    // answers true for every key the save path writes.
    static bool isKnownComponentKey(const std::string& key);

    // Base64 (RFC 4648) over raw bytes — the encoding the scene format uses for
    // the arrays that would otherwise be N JSON nodes (terrain sculptHeights and
    // layerWeights, navmesh geometry).
    //
    // Public for the one caller that has to speak the scene format from outside
    // it: a tool that edits a terrain's heights hands the result back as a
    // component patch, and that patch has to carry `sculptHeightsB64` in exactly
    // the spelling the loader reads. A second encoder written next to it would be
    // a second definition of the scene format, and the one that quietly stops
    // matching. Not a general-purpose base64 API — it exists so that there is
    // only ever one.
    static std::string          encodeBase64(const uint8_t* data, size_t len);
    static std::vector<uint8_t> decodeBase64(const std::string& s);

    // Instantiate a prefab blob into the world. Creates fresh entities for
    // every entry in the prefab and re-wires their hierarchy. The new subtree
    // root is reparented to `parent` (world root if entt::null). Returns the
    // new root entity, or entt::null on parse failure.
    //
    // preserveIds keeps the entity UUIDs stored in the blob instead of minting
    // fresh ones. Only for collaboration's structural replication, where both
    // peers must know the new subtree under the SAME identities; a prefab drop
    // must keep the default, or two drops would claim one identity twice.
    //
    // outBindings, when given, receives one entry per record — the record's
    // uuid on the template side, the created entity's uuid on the instance
    // side — for a caller that is placing a prefab ASSET and is about to stamp
    // a PrefabInstanceComponent on the root. Nothing is stamped here: this
    // function also serves paste, duplicate and a peer's create, none of which
    // is a placement. A blob whose records already carry the component (a
    // duplicate of an instance, a redo) has its bindings re-pointed at the
    // entities this call created, so no caller needs the parameter for that.
    Entity instantiatePrefab(HorizonWorld& world,
                             const std::vector<uint8_t>& data,
                             Entity parent = entt::null,
                             bool preserveIds = false,
                             std::vector<PrefabInstanceComponent::Binding>* outBindings = nullptr);

    // ── Prefab propagation ───────────────────────────────────────────────────
    // Bring a placed instance up to date with its asset: every template record
    // that is bound to a living entity of the placement has its components
    // re-applied from the blob, EXCEPT what the instance's override list marks
    // as authored here (a whole component, or single top-level properties of
    // one — the instance's current values win for those and the rest of the
    // block comes from the template). A component the template dropped is
    // removed unless overridden; one it gained is added. A template record
    // without a binding is new in the asset and is created, bound and parented
    // under the placement's counterpart of its parent record. What is never
    // touched: the root's transform (the placement itself), the "prefab" block
    // of any record (the instance's own link, or a nested instance's), a bound
    // entity that no longer exists (deleted here, which is an authored change),
    // and a bound entity whose record the asset no longer has (left standing
    // and counted as unbound — see the component header for why that is not a
    // deletion).
    //
    // An instance without any bindings predates the table (every placement
    // made before bindings existed). Its root is bound by structure — the
    // parentless record is the root, unambiguously — and every root component
    // that differs from the template is recorded as a whole-component override
    // rather than overwritten, so the first sync after an upgrade changes
    // nothing a human can see and the Inspector can revert those later, one by
    // one. Its children stay unbound: there is nothing that says which is which.
    //
    // Entity references inside component blocks (a joint's target, a rig's
    // follow entity) are copied verbatim, as instantiatePrefab copies them; the
    // template's ids do not resolve in the instance and nothing here pretends
    // they do.
    struct PrefabSyncReport {
        size_t instances          = 0; // placements visited
        size_t componentsApplied  = 0; // component blocks written from the template
        size_t componentsRemoved  = 0; // components the template no longer has
        size_t entitiesCreated    = 0; // records new in the asset, created here
        size_t overridesKept      = 0; // blocks/properties the override list protected
        size_t overridesAdopted   = 0; // whole-component overrides seeded on a legacy instance
        size_t unboundEntities    = 0; // bound entities whose record the asset lost
        size_t unresolvedAssets   = 0; // instances whose asset could not be read
    };
    // One placement (`root` carries the PrefabInstanceComponent) against one
    // asset blob. False when the blob does not parse or `root` is no instance.
    bool syncPrefabInstance(HorizonWorld& world, Entity root,
                            const std::vector<uint8_t>& assetBlob,
                            PrefabSyncReport* report = nullptr);
    // Every placement in the world against the content manager's copy of its
    // asset (made resident on demand; a placement whose asset is missing is
    // skipped and counted). Returns the number of placements synced. The
    // editor runs this when a scene is opened and before it is saved.
    size_t syncPrefabInstances(HorizonWorld& world, ContentManager& content,
                               PrefabSyncReport* report = nullptr);

    // ── Override recording ───────────────────────────────────────────────────
    // The other half of the sync above: the sync keeps what the override list
    // names, and this is what puts names on it. Every way the bound entity
    // differs from its template record RIGHT NOW is written into the instance's
    // list — a top-level property whose value differs (or that only one side
    // has) as a property override, a component only one side has as a
    // whole-component override, a different display name as "__name". Nothing
    // is compared that the sync would not write (the root's transform, any
    // "prefab" block), and an entry that is already there is not made twice.
    // Returns how many entries were added.
    //
    // The editor calls it after a human edited something: the diff against the
    // template is then, by definition, what was authored here — provided the
    // instance was synced against the asset first, which is why the editor
    // syncs on open and after every push. A property whose value merely equals
    // the template's gets no entry, so touching a value and putting it back
    // leaves nothing behind. An entity the table does not bind (a child added
    // here) is not something the sync touches, so there is nothing to protect
    // and nothing is recorded.
    size_t recordPrefabOverrides(HorizonWorld& world, Entity root, Entity entity,
                                 const std::vector<uint8_t>& assetBlob);

    // The placements whose binding table names this entity — usually one, two
    // for the root of a nested instance (it sits in its own table and in the
    // outer one). By uuid, not by hierarchy: the sync resolves bindings the
    // same way, so a child dragged out of the subtree is still the record's
    // counterpart. Empty for an entity no instance binds.
    static std::vector<Entity> prefabInstancesBinding(HorizonWorld& world, Entity entity);

    // Take an override back: the entry is dropped from the list and the
    // placement synced against the asset, so the property (or the whole
    // component, for an entry without a property) is what the template says
    // again — a component added here goes, one removed here comes back.
    // False when the instance has no such entry or the blob does not parse.
    bool revertPrefabOverride(HorizonWorld& world, Entity root,
                              const std::vector<uint8_t>& assetBlob,
                              const PrefabInstanceComponent::Override& entry);

    // ── Push to prefab ───────────────────────────────────────────────────────
    // The placement as a new template: the subtree serialised the way "Save as
    // Prefab" does it, with every entity id translated back to the template id
    // its binding names — the component header spells out why: the records
    // must keep the uuids every other placement's bindings hang on, or the
    // first push breaks every binding in the project. A child without a
    // binding (added here) keeps its own uuid, which is what the record is
    // called from now on. The root record's transform is the CURRENT asset's,
    // not the placement's (a placement's position is where it stands, never
    // what the prefab is), and the root's own "prefab" block is dropped. A
    // nested instance's bindings are translated like the records.
    //
    // The instance is rewritten to match: bindings become the identity over
    // the records just written (an entry for a deleted child goes, its record
    // no longer exists), the override list is emptied — the asset now IS this
    // placement. The caller writes `outBlob` to the asset and syncs the other
    // placements. False when `root` is no instance.
    bool pushPrefabInstance(HorizonWorld& world, Entity root,
                            const std::vector<uint8_t>& currentAssetBlob,
                            std::vector<uint8_t>& outBlob);

    // Remove the component a scene-format key names ("light", "rigidbody"), the
    // inverse of the one block applyComponents restores for it. False when the
    // key is unknown or the entity does not carry the component. "__name" is
    // not a component and is refused.
    static bool removeComponentByKey(HorizonWorld& world, Entity entity, const std::string& key);

private:
    bool saveJSON  (const HorizonWorld& world, const std::filesystem::path& path);
    bool saveBinary(const HorizonWorld& world, const std::filesystem::path& path);
    bool loadJSON  (HorizonWorld& world, const std::filesystem::path& path);
    bool loadBinary(HorizonWorld& world, const std::filesystem::path& path);
};
