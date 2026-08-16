#include "HorizonScene/AnimatorHost.h"
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HcCompiledLoader.h>   // HorizonCode::compiledClasses()
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <HorizonScene/Components/AnimatorStateMachineComponent.h>
#include <Diagnostics/Logger.h>
#include <string>
#include <utility>

namespace
{
// The animator asset behind an entity, or null. Quiet on the way out: most
// entities have no state machine, and most state machines have no sync graph.
const AnimatorStateMachineAsset* assetOf(ContentManager& cm, HorizonWorld& world, Entity e,
                                         HE::UUID& outId)
{
    auto& reg = world.registry();
    if (!reg.valid(e)) return nullptr;
    const auto* sm = reg.try_get<AnimatorStateMachineComponent>(e);
    if (!sm || sm->stateMachineAssetId == HE::UUID{}) return nullptr;
    outId = sm->stateMachineAssetId;
    return cm.getAnimatorStateMachine(outId);
}
} // namespace

void AnimatorHost::begin(HorizonCode::Runtime& runtime, HorizonWorld& world, ContentManager& cm)
{
    end();
    m_runtime = &runtime;
    m_world   = &world;
    m_content = &cm;

    for (auto [e, sm] : world.registry().view<AnimatorStateMachineComponent>().each())
        bind(e);

    if (!m_byEntity.empty())
        HE_LOG_INFO(HorizonCode, "AnimatorHost: %zu sync graph(s) running", m_byEntity.size());
}

HorizonCode::InstanceId AnimatorHost::bind(Entity entity)
{
    if (!m_runtime || !m_world || !m_content) return 0;

    HE::UUID assetId{};
    const AnimatorStateMachineAsset* a = assetOf(*m_content, *m_world, entity, assetId);
    const uint32_t raw = static_cast<uint32_t>(entity);

    // The asset changed under an existing binding — rebind, or the entity would
    // keep running the previous state machine's sync logic.
    if (auto it = m_assetOf.find(raw); it != m_assetOf.end() && it->second != assetId)
        unbind(entity);

    if (!a || a->syncGraphJson.empty()) return 0;
    if (auto it = m_byEntity.find(raw); it != m_byEntity.end()) return it->second;

    HorizonCode::InstanceId inst = 0;
    // Compiled first, interpreted fallback — the same duality every other graph
    // runs under, keyed by the asset's own path so the export can find it.
    if (auto compiled = HorizonCode::compiledClasses().create(a->path))
        inst = m_runtime->addCompiled(std::move(compiled), {}, { a->path, {}, {} });
    else
    {
        HorizonCode::Graph g;
        if (!HorizonCode::fromJson(a->syncGraphJson, g))
        {
            HE_LOG_ERROR(HorizonCode,
                "Animator '%s': its sync graph did not parse — the state machine's "
                "parameters will keep their defaults", a->path.c_str());
            return 0;
        }
        inst = m_runtime->add(std::move(g), {}, { a->path, {}, {} });
    }
    if (!inst) return 0;

    // The graph's owner is the animated entity: "Get Owning Entity" has to
    // answer with the character this state machine is posing, not with the
    // player — the same asset may be driving an NPC.
    m_runtime->setOwnedEntity(inst, raw);
    m_byEntity[raw] = inst;
    m_assetOf[raw]  = assetId;
    m_runtime->fireConstruct(inst);
    return inst;
}

void AnimatorHost::unbind(Entity entity)
{
    const uint32_t raw = static_cast<uint32_t>(entity);
    auto it = m_byEntity.find(raw);
    if (it == m_byEntity.end()) { m_assetOf.erase(raw); return; }
    if (m_runtime) m_runtime->destroy(it->second);
    m_byEntity.erase(it);
    m_assetOf.erase(raw);
}

void AnimatorHost::fireUpdate(Entity entity, float dt)
{
    if (!m_runtime) return;
    auto it = m_byEntity.find(static_cast<uint32_t>(entity));
    if (it == m_byEntity.end()) return;
    m_runtime->fireEvent(it->second, "Update", 0, HorizonCode::Value::ofFloat(dt));
}

void AnimatorHost::end()
{
    if (m_runtime)
        for (auto& [raw, inst] : m_byEntity) m_runtime->destroy(inst);
    m_byEntity.clear();
    m_assetOf.clear();
    m_runtime = nullptr;
    m_world   = nullptr;
    m_content = nullptr;
}
