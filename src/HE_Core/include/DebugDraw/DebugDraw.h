#pragma once
#include <glm/glm.hpp>
#include <vector>
#include <cmath>

struct DebugLine
{
    glm::vec3 start;
    glm::vec3 end;
    glm::vec3 color = glm::vec3(1.0f, 1.0f, 0.0f);
};

// CPU-side accumulator filled by the editor each frame and handed to the
// renderer via IRenderer::SetDebugLines(). Zero-cost when empty.
class DebugDrawBuffer
{
public:
    void line(const glm::vec3& a, const glm::vec3& b,
              const glm::vec3& color = glm::vec3(1.0f, 1.0f, 0.0f))
    {
        m_lines.push_back({ a, b, color });
    }

    void aabb(const glm::vec3& mn, const glm::vec3& mx,
              const glm::vec3& color = glm::vec3(1.0f, 1.0f, 0.0f))
    {
        // 12 edges of the box
        const glm::vec3 corners[8] = {
            { mn.x, mn.y, mn.z }, { mx.x, mn.y, mn.z },
            { mx.x, mx.y, mn.z }, { mn.x, mx.y, mn.z },
            { mn.x, mn.y, mx.z }, { mx.x, mn.y, mx.z },
            { mx.x, mx.y, mx.z }, { mn.x, mx.y, mx.z },
        };
        // Bottom face
        line(corners[0], corners[1], color); line(corners[1], corners[2], color);
        line(corners[2], corners[3], color); line(corners[3], corners[0], color);
        // Top face
        line(corners[4], corners[5], color); line(corners[5], corners[6], color);
        line(corners[6], corners[7], color); line(corners[7], corners[4], color);
        // Verticals
        line(corners[0], corners[4], color); line(corners[1], corners[5], color);
        line(corners[2], corners[6], color); line(corners[3], corners[7], color);
    }

    void sphere(const glm::vec3& center, float radius,
                const glm::vec3& color = glm::vec3(1.0f, 1.0f, 0.0f),
                int segments = 16)
    {
        const float step = 2.0f * 3.14159265f / float(segments);
        for (int i = 0; i < segments; ++i)
        {
            const float a0 = step * i, a1 = step * (i + 1);
            const float c0 = std::cos(a0), s0 = std::sin(a0);
            const float c1 = std::cos(a1), s1 = std::sin(a1);
            // XZ plane
            line(center + glm::vec3(c0 * radius, 0.0f, s0 * radius),
                 center + glm::vec3(c1 * radius, 0.0f, s1 * radius), color);
            // XY plane
            line(center + glm::vec3(c0 * radius, s0 * radius, 0.0f),
                 center + glm::vec3(c1 * radius, s1 * radius, 0.0f), color);
            // YZ plane
            line(center + glm::vec3(0.0f, c0 * radius, s0 * radius),
                 center + glm::vec3(0.0f, c1 * radius, s1 * radius), color);
        }
    }

    void clear()                                  { m_lines.clear(); }
    const std::vector<DebugLine>& lines() const   { return m_lines; }
    bool empty()                            const { return m_lines.empty(); }

private:
    std::vector<DebugLine> m_lines;
};
