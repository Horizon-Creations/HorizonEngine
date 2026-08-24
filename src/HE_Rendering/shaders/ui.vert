#version 450
layout(push_constant) uniform UIPush {
    vec4 uRect;      // xy=top-left px, zw=size px
    vec4 uColor;     // rgba (used in frag, but declared here for block match)
    vec4 uUVRect;    // {u0,v0,u1,v1} into the font atlas (glyph quads)
    vec2 uViewport;  // w, h in pixels
    vec2 uParams;    // x: 0 = solid color, 1 = font-atlas glyph
    vec4 uRotation;  // { angle(radians), pivotX, pivotY, unused }
} pc;
layout(location = 0) out vec2 vUV;
void main() {
    const vec2 c[4] = vec2[](vec2(0,0), vec2(1,0), vec2(0,1), vec2(1,1));
    vec2 uv = c[gl_VertexIndex];
    vec2 sp = pc.uRect.xy + uv * pc.uRect.zw;
    if (pc.uRotation.x != 0.0) {
        float sa = sin(pc.uRotation.x), ca = cos(pc.uRotation.x);
        vec2 d = sp - pc.uRotation.yz;
        sp = pc.uRotation.yz + vec2(d.x * ca - d.y * sa, d.x * sa + d.y * ca);
    }
    vUV = mix(pc.uUVRect.xy, pc.uUVRect.zw, uv);
    gl_Position = vec4(sp.x / pc.uViewport.x * 2.0 - 1.0,
                       1.0 - sp.y / pc.uViewport.y * 2.0,
                       0.0, 1.0);
}
