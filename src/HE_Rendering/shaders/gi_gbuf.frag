#version 450
layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 0) out vec4 oPos;
layout(location = 1) out vec4 oNorm;
void main()
{
    oPos  = vec4(vWorldPos, 1.0); // a = 1 → valid geometry
    oNorm = vec4(normalize(vNormal), 0.0);
}
