#pragma once
#include "../HE_RENDERING_API.h"
#include "RenderPass.h"
#include "RenderTarget.h"
#include <vector>
#include <memory>
#include <functional>

// NOTE: members are exported individually rather than marking the whole class
// HE_RENDERING_API. A class-wide dllexport forces MSVC to emit the implicit
// copy-assignment operator, which is deleted here because of the move-only
// std::unique_ptr<RenderPass> vector member (C2280).
class RenderGraph {
public:
    // Called once per pass: the backend binds io.output (creating offscreen
    // targets on demand), then replays cmds into it. id 0 = the active
    // window/viewport target already bound by the backend.
    using PassSink = std::function<void(const RenderPass& pass,
                                        const RenderPassIO& io,
                                        const CommandBuffer& cmds)>;

    HE_RENDERING_API void addPass(std::unique_ptr<RenderPass> pass);

    // Per-pass execution: each pass records into a scratch buffer that is handed
    // to the sink with its declared target I/O. This is the form backends use.
    HE_RENDERING_API void execute(const RenderWorld&           world,
                 const std::vector<uint32_t>& sortedIndices,
                 const PassSink&              sink);

    // Convenience: run every pass into a single merged command buffer (no target
    // routing). Handy for tests and single-pass setups.
    HE_RENDERING_API void execute(const RenderWorld&           world,
                 const std::vector<uint32_t>& sortedIndices,
                 CommandBuffer&               outCmds);

    void clear();

    bool empty() const { return passes_.empty(); }

private:
    std::vector<std::unique_ptr<RenderPass>> passes_;
    CommandBuffer                            scratch_; // reused per pass in the sink form
};
