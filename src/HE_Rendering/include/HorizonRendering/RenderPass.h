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
};

class HE_RENDERING_API ShadowPass : public RenderPass {
public:
    void execute(const RenderWorld&, const std::vector<uint32_t>&, CommandBuffer&) override;
    const char* name() const override { return "ShadowPass"; }
    RenderPassIO describe() const override;
};

class HE_RENDERING_API PostProcessPass : public RenderPass {
public:
    void execute(const RenderWorld&, const std::vector<uint32_t>&, CommandBuffer&) override;
    const char* name() const override { return "PostProcessPass"; }
    RenderPassIO describe() const override;
};
