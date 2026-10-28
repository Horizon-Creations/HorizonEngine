#pragma once
#include "../HE_RENDERING_API.h"
#include "RenderWorld.h"
#include "CommandBuffer.h"
#include "RenderTarget.h"
#include <vector>
#include <cstdint>

class HE_RENDERING_API RenderPass {
public:
    virtual ~RenderPass() = default;

    virtual void execute(const RenderWorld&           world,
                         const std::vector<uint32_t>& sortedIndices,
                         CommandBuffer&               outCmds) = 0;

    virtual const char* name() const = 0;

    // Declares the target this pass renders into and the offscreen targets it
    // samples. Default: render straight to the backbuffer, sampling nothing.
    virtual RenderPassIO describe() const { return {}; }
};

class HE_RENDERING_API GeometryPass : public RenderPass {
public:
    void execute(const RenderWorld&, const std::vector<uint32_t>&, CommandBuffer&) override;
    const char* name() const override { return "GeometryPass"; }
    // On: a batch carries DrawCall::instanceSpans (its instances read in place) instead
    // of a copied instanceTransforms list. For a backend that reads both forms (Metal).
    void setInstanceSpans(bool on) { m_instanceSpans = on; }
private:
    bool m_instanceSpans = false;
};

class HE_RENDERING_API ShadowPass : public RenderPass {
public:
    void execute(const RenderWorld&, const std::vector<uint32_t>&, CommandBuffer&) override;
    const char* name() const override { return "ShadowPass"; }
    RenderPassIO describe() const override;
    // Off: the pass still declares the shadow target (the backend's sink runs its depth branch
    // for it) but records no draws. For a backend that builds its depth views itself and never
    // reads the recorded set (D3D11, D3D12, OpenGL: FrameCull::prepareShadowViews): the walk over
    // every object with a draw call for each caster was a half of RenderGraph::record, unused.
    void setRecordDraws(bool on) { m_recordDraws = on; }
private:
    bool m_recordDraws = true;
};

class HE_RENDERING_API PostProcessPass : public RenderPass {
public:
    void execute(const RenderWorld&, const std::vector<uint32_t>&, CommandBuffer&) override;
    const char* name() const override { return "PostProcessPass"; }
    RenderPassIO describe() const override;
};
