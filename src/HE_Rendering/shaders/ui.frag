#version 450
layout(push_constant) uniform UIPush {
    vec4 uRect;
    vec4 uColor;
    vec2 uViewport;
    vec2 _pad;
} pc;
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 FragColor;
void main() { FragColor = pc.uColor; }
