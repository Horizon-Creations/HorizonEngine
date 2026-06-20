#version 450

layout(location = 0) in vec3 vViewPos;
layout(location = 0) out vec4 FragColor;

void main()
{
    // Store view-space position in the RGB channels.
    // Alpha = 1.0 marks a valid (foreground) pixel.
    FragColor = vec4(vViewPos, 1.0);
}
