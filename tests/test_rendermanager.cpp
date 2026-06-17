#include "doctest.h"
#include <HorizonRendering/GPUMemoryAllocator.h>
#include <HorizonRendering/RenderResourceManager.h>
#include <Types/UUID.h>

// ─── GPUMemoryAllocator ───────────────────────────────────────────────────────

TEST_CASE("GPUMemoryAllocator starts empty")
{
    GPUMemoryAllocator alloc(1024);
    CHECK(alloc.usedBytes()   == 0);
    CHECK(alloc.totalBudget() == 1024);
    CHECK(alloc.entryCount()  == 0);
}

TEST_CASE("GPUMemoryAllocator tracks allocations correctly")
{
    GPUMemoryAllocator alloc(1024);
    RenderHandle h1{1, 1};
    RenderHandle h2{2, 2};

    CHECK(alloc.requestAllocation(200, h1));
    CHECK(alloc.usedBytes()  == 200);
    CHECK(alloc.entryCount() == 1);

    CHECK(alloc.requestAllocation(300, h2));
    CHECK(alloc.usedBytes()  == 500);
    CHECK(alloc.entryCount() == 2);
}

TEST_CASE("GPUMemoryAllocator freeAllocation reduces usedBytes")
{
    GPUMemoryAllocator alloc(1024);
    RenderHandle h{1, 1};
    alloc.requestAllocation(400, h);
    CHECK(alloc.usedBytes() == 400);

    alloc.freeAllocation(h);
    CHECK(alloc.usedBytes()   == 0);
    CHECK(alloc.entryCount()  == 0);
}

TEST_CASE("GPUMemoryAllocator rejects allocation larger than budget")
{
    GPUMemoryAllocator alloc(100);
    RenderHandle h{1, 1};
    CHECK_FALSE(alloc.requestAllocation(200, h));
    CHECK(alloc.usedBytes() == 0);
}

TEST_CASE("GPUMemoryAllocator evictLRU removes least-recently-used entry")
{
    GPUMemoryAllocator alloc(1024);
    RenderHandle h1{1, 1};
    RenderHandle h2{2, 2};
    RenderHandle h3{3, 3};

    alloc.requestAllocation(100, h1);
    alloc.requestAllocation(100, h2);
    alloc.requestAllocation(100, h3);

    // h1 was added first → it's the LRU (back of the list).
    // Touch h1 to make it MRU so h2 becomes LRU.
    alloc.onHandleUsed(h1);

    RenderHandle evicted = RenderHandle::invalid();
    alloc.setEvictCallback([&](RenderHandle h) { evicted = h; });

    alloc.evictLRU(); // should evict h2 (LRU after h1 was touched)
    CHECK(evicted == h2);
    CHECK(alloc.usedBytes()  == 200);
    CHECK(alloc.entryCount() == 2);
}

TEST_CASE("GPUMemoryAllocator evictLRU on empty does nothing")
{
    GPUMemoryAllocator alloc(1024);
    alloc.evictLRU(); // must not crash
    CHECK(alloc.usedBytes() == 0);
}

TEST_CASE("GPUMemoryAllocator onHandleUsed promotes to MRU")
{
    GPUMemoryAllocator alloc(1024);
    RenderHandle h1{1, 1};
    RenderHandle h2{2, 2};

    alloc.requestAllocation(100, h1);
    alloc.requestAllocation(100, h2);

    // h1 is LRU; promote it. h2 should now be LRU.
    alloc.onHandleUsed(h1);

    RenderHandle evicted = RenderHandle::invalid();
    alloc.setEvictCallback([&](RenderHandle h) { evicted = h; });
    alloc.evictLRU();
    CHECK(evicted == h2);
}

// ─── RenderResourceManager ────────────────────────────────────────────────────

TEST_CASE("RenderResourceManager uploadMesh registers asset and returns valid handle")
{
    GPUMemoryAllocator     alloc(16 * 1024 * 1024); // 16 MB
    RenderResourceManager  mgr(alloc);

    HE::UUID id = HE::UUID::generate();
    MeshData mesh;
    mesh.vertexBytes = 4096;
    mesh.indexBytes  = 1024;

    RenderHandle h = mgr.uploadMesh(id, mesh);
    CHECK(h.isValid());
    CHECK(mgr.isLoaded(id));
    CHECK(mgr.findHandle(id) == h);
    CHECK(mgr.loadedCount()  == 1);
    CHECK(alloc.usedBytes()  == 5120);
}

TEST_CASE("RenderResourceManager uploadMesh is idempotent")
{
    GPUMemoryAllocator    alloc(16 * 1024 * 1024);
    RenderResourceManager mgr(alloc);

    HE::UUID id = HE::UUID::generate();
    MeshData mesh;
    mesh.vertexBytes = 1000;
    mesh.indexBytes  = 200;

    RenderHandle h1 = mgr.uploadMesh(id, mesh);
    RenderHandle h2 = mgr.uploadMesh(id, mesh); // second call must return same handle
    CHECK(h1 == h2);
    CHECK(mgr.loadedCount() == 1);
}

TEST_CASE("RenderResourceManager uploadTexture accounts for pixel footprint")
{
    GPUMemoryAllocator    alloc(64 * 1024 * 1024);
    RenderResourceManager mgr(alloc);

    HE::UUID id = HE::UUID::generate();
    TextureData tex;
    tex.width    = 1024;
    tex.height   = 1024;
    tex.channels = 4; // RGBA8

    RenderHandle h = mgr.uploadTexture(id, tex);
    CHECK(h.isValid());
    CHECK(alloc.usedBytes() == 1024u * 1024u * 4u);
}

TEST_CASE("RenderResourceManager createMaterial registers with default constant-buffer size")
{
    GPUMemoryAllocator    alloc(1024 * 1024);
    RenderResourceManager mgr(alloc);

    HE::UUID id = HE::UUID::generate();
    RenderHandle h = mgr.createMaterial(id, MaterialDesc{});
    CHECK(h.isValid());
    CHECK(alloc.usedBytes() == 256); // default constant-buffer size
}

TEST_CASE("RenderResourceManager release unregisters the asset")
{
    GPUMemoryAllocator    alloc(1024 * 1024);
    RenderResourceManager mgr(alloc);

    HE::UUID id = HE::UUID::generate();
    RenderHandle h = mgr.uploadMesh(id, {1024, 512});
    CHECK(mgr.isLoaded(id));

    mgr.release(h);
    CHECK(!mgr.isLoaded(id));
    CHECK(!mgr.findHandle(id).isValid());
    CHECK(alloc.usedBytes()  == 0);
    CHECK(mgr.loadedCount()  == 0);
}

TEST_CASE("RenderResourceManager findHandle returns invalid for unknown asset")
{
    GPUMemoryAllocator    alloc(1024 * 1024);
    RenderResourceManager mgr(alloc);

    HE::UUID id = HE::UUID::generate();
    CHECK(!mgr.isLoaded(id));
    CHECK(!mgr.findHandle(id).isValid());
}

TEST_CASE("RenderResourceManager multiple assets share the allocator budget")
{
    GPUMemoryAllocator    alloc(10000);
    RenderResourceManager mgr(alloc);

    HE::UUID id1 = HE::UUID::generate();
    HE::UUID id2 = HE::UUID::generate();

    mgr.uploadMesh   (id1, {4000, 0});
    mgr.uploadTexture(id2, TextureData{32, 32, 4}); // 32*32*4 = 4096 bytes

    CHECK(alloc.usedBytes()  == 4000 + 4096);
    CHECK(mgr.loadedCount()  == 2);
    CHECK(mgr.totalBudget()  == 10000);
}
