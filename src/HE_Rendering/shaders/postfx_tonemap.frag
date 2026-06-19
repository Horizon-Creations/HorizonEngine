#version 450
layout(location=0) in vec2 vUV;
layout(location=0) out vec4 outColor;
layout(set=0, binding=0) uniform sampler2D uHDR;
layout(set=0, binding=1) uniform sampler2D uBloom;
layout(push_constant) uniform PC { vec4 params; } pc;
vec3 aces(vec3 x) { return clamp((x*(2.51*x+0.03))/(x*(2.43*x+0.59)+0.14),0.0,1.0); }
void main() {
    vec3 h = texture(uHDR, vUV).rgb + texture(uBloom, vUV).rgb * pc.params.y;
    h *= pc.params.x;
    outColor = vec4(pow(max(aces(h), vec3(1e-4)), vec3(1.0/2.2)), 1.0);
}
