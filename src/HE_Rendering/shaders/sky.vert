#version 450
layout(location = 0) out vec2 vNDC;
void main()
{
    // Fullscreen triangle at Vulkan far plane (z=1 in clip, Vulkan NDC z in [0,1])
    // Y is flipped in Vulkan: NDC y=+1 is bottom
    vec2 p = vec2(float((gl_VertexIndex & 1) << 2) - 1.0,
                  float((gl_VertexIndex & 2) << 1) - 1.0);
    vNDC = p;
    gl_Position = vec4(p.x, p.y, 1.0, 1.0);
}
