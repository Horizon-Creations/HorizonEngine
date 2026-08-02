#version 450
// 3x3 box blur over the temporal history's alpha (the shadow scalar; rgb there
// is the world position for the next frame's disocclusion check, not colour).
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 0) uniform sampler2D uSrc;
void main()
{
    vec2 texel = 1.0 / vec2(textureSize(uSrc, 0));
    float sum = 0.0;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
            sum += texture(uSrc, vUV + vec2(float(x), float(y)) * texel).a;
    FragColor = vec4(sum / 9.0, 0.0, 0.0, 1.0);
}
