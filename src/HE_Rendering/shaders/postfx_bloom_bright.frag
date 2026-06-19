#version 450
layout(location=0) in vec2 vUV;
layout(location=0) out vec4 outColor;
layout(set=0, binding=0) uniform sampler2D uHDR;
layout(set=0, binding=1) uniform sampler2D _dummy;
layout(push_constant) uniform PC { vec4 params; } pc;
void main(){
    vec3 c=texture(uHDR,vUV).rgb; float br=max(c.r,max(c.g,c.b));
    float s=clamp(br-pc.params.x+pc.params.y,0.0,2.0*pc.params.y);
    s=(s*s)/(4.0*pc.params.y+1e-4); float contrib=max(s,br-pc.params.x)/max(br,1e-4);
    outColor=vec4(c*contrib,1.0);
}
