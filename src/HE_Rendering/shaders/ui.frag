#version 450
layout(push_constant) uniform UIPush {
    vec4 uRect;
    vec4 uColor;
    vec4 uUVRect;
    vec2 uViewport;
    vec2 uParams;    // x: 0 = solid color, 1 = font-atlas glyph
} pc;
// R8 font atlas (glyph coverage in .r) — solid quads ignore it, but the set must
// stay bound for every draw since the sampler is statically used by this shader.
layout(set = 0, binding = 0) uniform sampler2D uFontAtlas;
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 FragColor;
void main() {
    if (pc.uParams.x > 0.5) {
        float a = texture(uFontAtlas, vUV).r;
        FragColor = vec4(pc.uColor.rgb, pc.uColor.a * a);
    } else {
        FragColor = pc.uColor;
    }
}
