#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;
layout(set = 0, binding = 0) uniform DebugCB { mat4 uVP; };
layout(location = 0) out vec3 vColor;
void main() { vColor = aColor; gl_Position = uVP * vec4(aPos, 1.0); }
