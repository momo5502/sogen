#version 450

// A real 3D cube: per-vertex position + color come from a vertex buffer, transformed by a 64-byte mat4
// model-view-projection push constant computed on the CPU. Correct occlusion comes from the depth
// buffer (no CPU face sorting needed), so the faces can be drawn in any order in one indexed draw.

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inColor;

layout(push_constant) uniform PushConstants
{
    mat4 mvp;
} pc;

layout(location = 0) out vec3 fragColor;

void main()
{
    gl_Position = pc.mvp * vec4(inPos, 1.0);
    fragColor = inColor;
}
