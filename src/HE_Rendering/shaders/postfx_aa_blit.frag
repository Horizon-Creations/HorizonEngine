#version 450
// AA = Off (docs/anti-aliasing-plan.md). The final post pass is what writes the
// viewport image, so it still runs — this is that pass without any filtering.
// Same bindings/push constants as postfx_fxaa.frag so it shares the pipeline
// layout, the descriptor sets and the render pass.
layout(location=0) in vec2 vUV;
layout(location=0) out vec4 outColor;
layout(set=0, binding=0) uniform sampler2D uScene;
layout(set=0, binding=1) uniform sampler2D _dummy;
layout(push_constant) uniform PC { vec4 params; } pc;
void main(){ outColor = vec4(texture(uScene, vUV).rgb, 1.0); }
