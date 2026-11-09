// fx_gen — writes the engine's built-in particle graphs (ParticleGraphAsset .hasset files, via
// ContentManager::saveAsset, byte-identical to what the Particle Graph Editor would save):
//
//     fx_gen <out-dir>          (EditorDeps/EngineContent/Particles)
//
// The fifth generator of the kind (mat_gen, matfn_gen, mesh_gen, widget_gen). The outputs are committed;
// this is not part of the normal build graph.
//
// What they are: the spray of a fall in the landscape water (Thema 232). A landscape's water names them
// (Field::fallCurtain/fallDrops/fallMist, defaulting to these ids); WaterSurface places one emitter each at
// every fall and tunes it with ParticleEmitterModifiers (rate, life, size, heading, width — by the fall's
// height, the current and the width of its edge). The LOOK is all here, in the graphs: an ordinary asset a
// project can open in the Particle Graph Editor, copy and replace. The values are the ones for strength 0,
// a fall of 1 s (the modifiers scale the life by the fall time), the water leaving along +X, one lip's
// width of edge (1.5 m) — the graph's frame is forward +X, up +Y, across +Z.
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <ContentManager/DefaultAssets.h>
#include <ParticleGraph/ParticleGraph.h>
#include <Types/UUID.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace HE;

namespace
{

struct Pg
{
    ParticleGraph g;
    int out = 0;
    Pg() { out = g.addNode(ParticleNodeType::EmitterOutput, 300.0f, 120.0f); }
    float row = 0.0f;
    void f(int pin, float v)
    {
        const int c = g.addNode(ParticleNodeType::ConstFloat, 0.0f, row);
        row += 60.0f;
        g.findNode(c)->p[0] = v;
        g.connect(c, 0, out, pin);
    }
    void v3(int pin, float x, float y, float z, bool color = false)
    {
        const int c = g.addNode(color ? ParticleNodeType::ConstColor : ParticleNodeType::ConstVec3, 0.0f, row);
        row += 60.0f;
        ParticleGraphNode* n = g.findNode(c);
        n->p[0] = x; n->p[1] = y; n->p[2] = z;
        g.connect(c, 0, out, pin);
    }
};

// The curtain: a sheet of streaks that leaves the lip along the current and falls for the fall's time, spreading as
// it drops; thin at the lip (it fades in), white below.
ParticleGraph curtain()
{
    Pg p;
    p.f (kParticleEmitRatePin, 160.0f);
    p.f (kParticleLifetimeMinPin, 0.95f);
    p.f (kParticleLifetimeMaxPin, 1.3f);
    p.f (kParticleStartSizePin, 0.28f);
    p.f (kParticleEndSizePin, 0.5f);
    p.v3(kParticleStartColorPin, 1.0f, 1.0f, 1.0f, true);
    p.v3(kParticleEndColorPin, 1.0f, 1.0f, 1.0f, true);
    p.f (kParticleStartAlphaPin, 0.75f);
    p.f (kParticleEndAlphaPin, 0.6f);
    p.v3(kParticleInitialVelocityPin, 1.0f, 0.1f, 0.0f);
    p.f (kParticleVelocitySpreadPin, 0.06f);
    p.v3(kParticleGravityPin, 0.0f, -9.8f, 0.0f);
    p.f (kParticleMaxParticlesPin, 240.0f);
    p.f (kParticleLoopingPin, 1.0f);
    p.f (kParticleBillboardPin, static_cast<float>(ParticleBillboard::Streak));
    p.f (kParticleStretchPin, 0.06f);
    p.f (kParticleFadeInPin, 0.06f);
    p.v3(kParticleSpawnHalfAPin, 0.0f, 0.0f, 0.75f);   // across the edge: half a lip's width
    p.v3(kParticleSpawnHalfBPin, 0.0f, 0.0f, 0.0f);    // on the line, not around it: an even top
    p.g.findNode(p.out)->materialAssetId = kEngineWaterSprayMaterialId;
    return p.g;
}

// The drops thrown at the foot: short streaks up and on, falling back.
ParticleGraph drops()
{
    Pg p;
    p.f (kParticleEmitRatePin, 60.0f);
    p.f (kParticleLifetimeMinPin, 0.4f);
    p.f (kParticleLifetimeMaxPin, 0.8f);
    p.f (kParticleStartSizePin, 0.07f);
    p.f (kParticleEndSizePin, 0.04f);
    p.v3(kParticleStartColorPin, 1.0f, 1.0f, 1.0f, true);
    p.v3(kParticleEndColorPin, 1.0f, 1.0f, 1.0f, true);
    p.f (kParticleStartAlphaPin, 0.75f);
    p.f (kParticleEndAlphaPin, 0.0f);
    p.v3(kParticleInitialVelocityPin, 0.4f, 1.5f, 0.0f);
    p.f (kParticleVelocitySpreadPin, 0.55f);
    p.v3(kParticleGravityPin, 0.0f, -9.8f, 0.0f);
    p.f (kParticleMaxParticlesPin, 80.0f);
    p.f (kParticleLoopingPin, 1.0f);
    p.f (kParticleBillboardPin, static_cast<float>(ParticleBillboard::Streak));
    p.f (kParticleStretchPin, 0.03f);
    p.f (kParticleFadeInPin, 0.0f);
    p.v3(kParticleSpawnHalfAPin, 0.0f, 0.0f, 0.7f);
    p.v3(kParticleSpawnHalfBPin, 0.35f, 0.0f, 0.0f);
    p.g.findNode(p.out)->materialAssetId = kEngineWaterSprayMaterialId;
    return p.g;
}

// The mist over the foot: big soft puffs that grow, drift on with the water and lift slowly.
ParticleGraph mist()
{
    Pg p;
    p.f (kParticleEmitRatePin, 10.0f);
    p.f (kParticleLifetimeMinPin, 1.6f);
    p.f (kParticleLifetimeMaxPin, 2.6f);
    p.f (kParticleStartSizePin, 0.7f);
    p.f (kParticleEndSizePin, 2.0f);
    p.v3(kParticleStartColorPin, 1.0f, 1.0f, 1.0f, true);
    p.v3(kParticleEndColorPin, 1.0f, 1.0f, 1.0f, true);
    p.f (kParticleStartAlphaPin, 0.06f);
    p.f (kParticleEndAlphaPin, 0.0f);
    p.v3(kParticleInitialVelocityPin, 0.3f, 0.5f, 0.0f);
    p.f (kParticleVelocitySpreadPin, 0.9f);
    p.v3(kParticleGravityPin, 0.0f, 0.15f, 0.0f);    // warm wet air: it lifts, slowly
    p.f (kParticleMaxParticlesPin, 60.0f);
    p.f (kParticleLoopingPin, 1.0f);
    p.f (kParticleBillboardPin, static_cast<float>(ParticleBillboard::Puff));
    p.f (kParticleStretchPin, 0.0f);
    p.f (kParticleFadeInPin, 0.0f);
    p.v3(kParticleSpawnHalfAPin, 0.0f, 0.0f, 0.9f);
    p.v3(kParticleSpawnHalfBPin, 0.5f, 0.0f, 0.0f);
    p.g.findNode(p.out)->materialAssetId = kEngineWaterSprayMaterialId;
    return p.g;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: fx_gen <output-dir>\n");
        return 2;
    }
    const std::string outDir = argv[1];
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    struct Entry { const char* name; UUID id; ParticleGraph g; };
    std::vector<Entry> entries;
    entries.push_back({ "WaterCurtain", kEngineWaterCurtainParticlesId, curtain() });
    entries.push_back({ "WaterDrops",   kEngineWaterDropsParticlesId,   drops() });
    entries.push_back({ "WaterMist",    kEngineWaterMistParticlesId,    mist() });

    int ok = 0;
    for (Entry& e : entries)
    {
        ParticleGraphAsset a;
        a.type          = AssetType::ParticleSystem;
        a.name          = e.name;
        a.path          = std::string(e.name) + ".hasset";
        a.id            = e.id;
        a.nodeGraphJson = particleGraphToJson(e.g);
        bool written = false;
        {
            ContentManager cm(outDir);
            written = cm.saveAsset(a);
        }
        if (written)
        {
            ContentManager cm(outDir);
            const UUID id = cm.loadAsset(a.path);
            written = id == e.id && cm.getParticleGraph(id) != nullptr;
        }
        std::printf("  %-12s %s\n", e.name, written ? "ok" : "FAILED");
        ok += written ? 1 : 0;
    }
    std::printf("fx_gen: wrote %d/%zu particle graphs to %s\n", ok, entries.size(), outDir.c_str());
    return ok == static_cast<int>(entries.size()) ? 0 : 1;
}
