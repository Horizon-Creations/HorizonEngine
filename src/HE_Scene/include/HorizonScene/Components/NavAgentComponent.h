#pragma once
#include <Math/Math.h>
#include <vector>

struct NavAgentComponent {
    glm::vec3 targetPos      = glm::vec3(0.0f);
    float     speed          = 3.5f;
    float     stoppingDist   = 0.1f;

    // Runtime path state (reset when targetPos changes)
    std::vector<glm::vec3> path;
    size_t                 pathIdx  = 0;
    bool                   hasPath  = false;
    bool                   moving   = false;
};
