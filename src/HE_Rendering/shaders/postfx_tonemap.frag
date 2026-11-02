#version 450
layout(location=0) in vec2 vUV;
layout(location=0) out vec4 outColor;
layout(set=0, binding=0) uniform sampler2D uHDR;
layout(set=0, binding=1) uniform sampler2D uBloom;
layout(push_constant) uniform PC { vec4 params; } pc;
vec3 aces(vec3 x) { return clamp((x*(2.51*x+0.03))/(x*(2.43*x+0.59)+0.14),0.0,1.0); }
void main() {
    if (pc.params.x < 0.0) {   // raw capture (EditorCameraOverride::rawOutput): the material's colour, untouched
        vec3 c = clamp(texture(uHDR, vUV).rgb, 0.0, 1.0);
        if (pc.params.x < -1.5)   // rawSrgb: the exact sRGB curve, for a picture that becomes an sRGB texture
            c = mix(1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, c * 12.92, lessThanEqual(c, vec3(0.0031308)));
        outColor = vec4(c, 1.0);
        return;
    }
    vec3 h = texture(uHDR, vUV).rgb + texture(uBloom, vUV).rgb * pc.params.y;
    h *= pc.params.x;
    outColor = vec4(pow(max(aces(h), vec3(1e-4)), vec3(1.0/2.2)), 1.0);
}
