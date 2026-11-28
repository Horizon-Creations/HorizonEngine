#version 450
// TAA / FSR velocity of a skinned mesh: taa_velocity.vert with skinned.vert's skin, this frame's palette (set 0, the
// skinned pass's bones-ring slot) and last frame's (set 1). The skin and the raster position are formed exactly as
// skinned.vert forms them, so the draw lands on the depth the skinned pass stored. Pairs with taa_velocity.frag.

layout(location = 0) in vec3  aPos;
layout(location = 3) in uvec4 aBoneIds;
layout(location = 4) in vec4  aBoneWgts;

layout(push_constant) uniform PushConstants {
    mat4 mvpJitter;
    mat4 mvpPrevJitter;
} pc;

layout(set = 0, binding = 0) uniform BonesCB     { mat4 uBoneMatrices[128]; };
layout(set = 1, binding = 0) uniform PrevBonesCB { mat4 uPrevBoneMatrices[128]; };

layout(location = 0) out vec4 vClipNow;
layout(location = 1) out vec4 vClipPrev;

void main()
{
    mat4 skin = aBoneWgts.x * uBoneMatrices[aBoneIds.x]
              + aBoneWgts.y * uBoneMatrices[aBoneIds.y]
              + aBoneWgts.z * uBoneMatrices[aBoneIds.z]
              + aBoneWgts.w * uBoneMatrices[aBoneIds.w];
    mat4 prev = aBoneWgts.x * uPrevBoneMatrices[aBoneIds.x]
              + aBoneWgts.y * uPrevBoneMatrices[aBoneIds.y]
              + aBoneWgts.z * uPrevBoneMatrices[aBoneIds.z]
              + aBoneWgts.w * uPrevBoneMatrices[aBoneIds.w];
    vec4 sp = skin * vec4(aPos, 1.0);
    gl_Position = pc.mvpJitter * sp;
    vClipNow    = gl_Position;
    vClipPrev   = pc.mvpPrevJitter * (prev * vec4(aPos, 1.0));
}
