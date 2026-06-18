#include "doctest.h"
#include <HorizonScene/Components/SkeletalMeshComponent.h>
#include <HorizonRendering/CommandBuffer.h>
#include <HorizonRendering/RenderWorld.h>
#include <HorizonRendering/RenderPass.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonRendering/RenderExtractor.h>
#include <glm/gtc/matrix_transform.hpp>

// ─────────────────────────────────────────────────────────────────────────────
//  SkeletalMeshComponent defaults
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("SkeletalMeshComponent default bone matrix is identity")
{
    SkeletalMeshComponent smc;
    REQUIRE(smc.boneMatrices.size() == 1);
    CHECK(smc.boneMatrices[0] == glm::mat4(1.0f));
}

TEST_CASE("SkeletalMeshComponent default flags")
{
    SkeletalMeshComponent smc;
    CHECK(smc.castsShadow    == true);
    CHECK(smc.receivesShadow == true);
    CHECK(smc.dirty          == true);
    CHECK(smc.meshAssetId    == HE::UUID{});
}

TEST_CASE("SkeletalMeshComponent bone matrices can be set")
{
    SkeletalMeshComponent smc;
    glm::mat4 t = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 2.0f, 3.0f));
    smc.boneMatrices = { glm::mat4(1.0f), t };
    REQUIRE(smc.boneMatrices.size() == 2);
    CHECK(smc.boneMatrices[1] == t);
}

// ─────────────────────────────────────────────────────────────────────────────
//  SkinnedDrawCall storage in CommandBuffer
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("CommandBuffer records skinned draw calls")
{
    CommandBuffer cb;
    cb.reset();

    SkinnedDrawCall dc;
    dc.boneMatrices = { glm::mat4(1.0f) };
    dc.transform    = glm::translate(glm::mat4(1.0f), glm::vec3(5.0f, 0.0f, 0.0f));
    cb.recordSkinnedDraw(dc);

    REQUIRE(cb.skinnedDrawCalls().size() == 1);
    CHECK(cb.skinnedDrawCalls()[0].transform[3][0] == doctest::Approx(5.0f));
    CHECK(cb.skinnedDrawCalls()[0].boneMatrices.size() == 1);
    CHECK(cb.skinnedDrawCalls()[0].boneMatrices[0] == glm::mat4(1.0f));
}

TEST_CASE("CommandBuffer reset clears skinned draw calls")
{
    CommandBuffer cb;
    SkinnedDrawCall dc;
    dc.boneMatrices = { glm::mat4(1.0f) };
    cb.recordSkinnedDraw(dc);
    cb.reset();
    CHECK(cb.skinnedDrawCalls().empty());
    CHECK(cb.empty());
}

TEST_CASE("CommandBuffer static and skinned draws are independent")
{
    CommandBuffer cb;
    DrawCall dc;
    cb.recordDraw(dc);
    SkinnedDrawCall sdc;
    cb.recordSkinnedDraw(sdc);

    CHECK(cb.drawCalls().size()        == 1);
    CHECK(cb.skinnedDrawCalls().size() == 1);
    CHECK_FALSE(cb.empty());

    cb.reset();
    CHECK(cb.drawCalls().empty());
    CHECK(cb.skinnedDrawCalls().empty());
}

// ─────────────────────────────────────────────────────────────────────────────
//  RenderExtractor populates skinnedObjects from SkeletalMeshComponent
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("RenderExtractor collects SkeletalMeshComponent into skinnedObjects")
{
    HorizonWorld world;
    entt::registry& reg = world.registry();

    HE::UUID meshId = HE::UUID::generate();
    entt::entity e = world.createEntity();
    world.addComponent(e, TransformComponent{
        .position = glm::vec3(1.0f, 2.0f, 3.0f),
        .rotation = glm::vec3(0.0f),
        .scale    = glm::vec3(1.0f) });
    SkeletalMeshComponent smc;
    smc.meshAssetId  = meshId;
    smc.boneMatrices = { glm::mat4(1.0f), glm::mat4(2.0f) };
    world.addComponent(e, smc);

    RenderWorld rw;
    RenderExtractor extractor;
    extractor.extract(world, rw, 16.0f / 9.0f);

    REQUIRE(rw.skinnedObjects.size() == 1);
    CHECK(rw.skinnedObjects[0].meshAssetId   == meshId);
    CHECK(rw.skinnedObjects[0].boneMatrices.size() == 2);
    CHECK(rw.skinnedObjects[0].boneMatrices[0] == glm::mat4(1.0f));
    // The entity must NOT also appear in the static objects list
    bool foundInStatic = false;
    for (const auto& obj : rw.objects)
        if (obj.meshAssetId == meshId) { foundInStatic = true; break; }
    CHECK_FALSE(foundInStatic);
}

TEST_CASE("RenderExtractor uses identity bone when boneMatrices is empty")
{
    HorizonWorld world;

    HE::UUID meshId = HE::UUID::generate();
    entt::entity e = world.createEntity();
    world.addComponent(e, TransformComponent{
        .position = {}, .rotation = {}, .scale = glm::vec3(1.0f) });
    SkeletalMeshComponent smc;
    smc.meshAssetId  = meshId;
    smc.boneMatrices = {};   // deliberately empty
    world.addComponent(e, smc);

    RenderWorld rw;
    RenderExtractor extractor;
    extractor.extract(world, rw, 1.0f);

    REQUIRE(rw.skinnedObjects.size() == 1);
    REQUIRE(rw.skinnedObjects[0].boneMatrices.size() == 1);
    CHECK(rw.skinnedObjects[0].boneMatrices[0] == glm::mat4(1.0f));
}

// ─────────────────────────────────────────────────────────────────────────────
//  GeometryPass emits SkinnedDrawCalls for skinnedObjects
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("GeometryPass emits skinned draw call for skinned objects")
{
    RenderWorld rw;
    SkinnedRenderObject sobj;
    sobj.meshAssetId  = HE::UUID::generate();
    sobj.boneMatrices = { glm::mat4(1.0f) };
    rw.skinnedObjects.push_back(sobj);

    CommandBuffer cb;
    GeometryPass pass;
    pass.execute(rw, {}, cb);

    CHECK(cb.drawCalls().empty());                 // no static objects
    REQUIRE(cb.skinnedDrawCalls().size() == 1);
    CHECK(cb.skinnedDrawCalls()[0].meshAssetId == sobj.meshAssetId);
    CHECK(cb.skinnedDrawCalls()[0].boneMatrices.size() == 1);
}
