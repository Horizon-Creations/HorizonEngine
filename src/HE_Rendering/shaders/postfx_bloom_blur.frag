#version 450
layout(location=0) in vec2 vUV;
layout(location=0) out vec4 outColor;
layout(set=0, binding=0) uniform sampler2D uImage;
layout(set=0, binding=1) uniform sampler2D _dummy;
layout(push_constant) uniform PC { vec4 params; } pc;
void main(){
    const float w[5]=float[](0.227027,0.1945946,0.1216216,0.054054,0.016216);
    vec2 d=(pc.params.z>0.5)?vec2(pc.params.x,0.0):vec2(0.0,pc.params.y);
    vec3 r=texture(uImage,vUV).rgb*w[0];
    for(int i=1;i<5;++i){r+=texture(uImage,vUV+d*float(i)).rgb*w[i];r+=texture(uImage,vUV-d*float(i)).rgb*w[i];}
    outColor=vec4(r,1.0);
}
