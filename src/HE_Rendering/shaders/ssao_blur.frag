#version 450

// 4x4 box blur to remove the noise-rotation pattern.  Single channel (R).
// Vertex shader: postfx.vert (attribute-less fullscreen triangle).
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 FragColor;

layout(set = 0, binding = 0) uniform sampler2D uAOInput;

void main()
{
    vec2  texel = 1.0 / vec2(textureSize(uAOInput, 0));
    float sum   = 0.0;
    for (int x = -2; x < 2; ++x)
        for (int y = -2; y < 2; ++y)
            sum += texture(uAOInput, vUV + vec2(float(x), float(y)) * texel).r;
    float ao = sum / 16.0;
    FragColor = vec4(ao, ao, ao, 1.0);
}
