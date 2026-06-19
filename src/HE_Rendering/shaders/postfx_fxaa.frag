#version 450
layout(location=0) in vec2 vUV;
layout(location=0) out vec4 outColor;
layout(set=0, binding=0) uniform sampler2D uScene;
layout(set=0, binding=1) uniform sampler2D _dummy;
layout(push_constant) uniform PC { vec4 params; } pc;
float luma(vec3 c){return dot(c,vec3(0.299,0.587,0.114));}
void main(){
    const float EMIN=1.0/24.0,EMAX=1.0/8.0,SMAX=8.0;
    vec2 rcp=pc.params.xy;
    vec3 M=texture(uScene,vUV).rgb; float lM=luma(M);
    float lNW=luma(texture(uScene,vUV+vec2(-1,-1)*rcp).rgb);
    float lNE=luma(texture(uScene,vUV+vec2( 1,-1)*rcp).rgb);
    float lSW=luma(texture(uScene,vUV+vec2(-1, 1)*rcp).rgb);
    float lSE=luma(texture(uScene,vUV+vec2( 1, 1)*rcp).rgb);
    float lMin=min(lM,min(min(lNW,lNE),min(lSW,lSE)));
    float lMax=max(lM,max(max(lNW,lNE),max(lSW,lSE))); float rng=lMax-lMin;
    if(rng<max(EMIN,lMax*EMAX)){outColor=vec4(M,1);return;}
    vec2 dir; dir.x=-((lNW+lNE)-(lSW+lSE)); dir.y=(lNW+lSW)-(lNE+lSE);
    float dr=max((lNW+lNE+lSW+lSE)*0.25*(1.0/8.0),1.0/128.0);
    dir=clamp(dir/(min(abs(dir.x),abs(dir.y))+dr),-SMAX,SMAX)*rcp;
    vec3 A=0.5*(texture(uScene,vUV+dir*(1.0/3.0-0.5)).rgb+texture(uScene,vUV+dir*(2.0/3.0-0.5)).rgb);
    vec3 B=A*0.5+0.25*(texture(uScene,vUV+dir*(-0.5)).rgb+texture(uScene,vUV+dir*0.5).rgb);
    float lB=luma(B);
    outColor=(lB<lMin||lB>lMax)?vec4(A,1):vec4(B,1);
}
