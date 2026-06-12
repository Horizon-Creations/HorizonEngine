#pragma once
#include "RenderPass.h"
#include <vector>
#include <memory>

class RenderGraph {
public:
    void addPass(std::unique_ptr<RenderPass> pass);
    void execute(const RenderWorld&           world,
                 const std::vector<uint32_t>& sortedIndices,
                 CommandBuffer&               outCmds);
    void clear();

private:
    std::vector<std::unique_ptr<RenderPass>> passes_;
};
