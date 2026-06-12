#pragma once
#include <Types/Handle.h>
#include <Math/Math.h>
#include <vector>

struct DrawCall {
    RenderHandle mesh;
    RenderHandle material;
    glm::mat4    transform;
    uint32_t     instanceCount = 1;
};

class CommandBuffer {
public:
    void reset();
    void recordDraw(const DrawCall& call);

    const std::vector<DrawCall>& drawCalls() const;

private:
    std::vector<DrawCall> drawCalls_;
};
