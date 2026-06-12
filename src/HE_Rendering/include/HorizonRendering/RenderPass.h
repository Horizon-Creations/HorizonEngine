#pragma once
#include "RenderWorld.h"
#include "CommandBuffer.h"
#include <vector>
#include <cstdint>

class RenderPass {
public:
    virtual ~RenderPass() = default;

    virtual void execute(const RenderWorld&           world,
                         const std::vector<uint32_t>& sortedIndices,
                         CommandBuffer&               outCmds) = 0;

    virtual const char* name() const = 0;
};

class GeometryPass : public RenderPass {
public:
    void execute(const RenderWorld&, const std::vector<uint32_t>&, CommandBuffer&) override;
    const char* name() const override { return "GeometryPass"; }
};

class ShadowPass : public RenderPass {
public:
    void execute(const RenderWorld&, const std::vector<uint32_t>&, CommandBuffer&) override;
    const char* name() const override { return "ShadowPass"; }
};

class PostProcessPass : public RenderPass {
public:
    void execute(const RenderWorld&, const std::vector<uint32_t>&, CommandBuffer&) override;
    const char* name() const override { return "PostProcessPass"; }
};
