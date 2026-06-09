#version 450

// Geometry (position + texcoord) comes from a vertex buffer; the position is rotated by a push-constant
// angle so the quad spins. The texcoord is passed through to the fragment shader, which samples a
// texture bound via a combined-image-sampler descriptor.

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;

layout(push_constant) uniform PushConstants
{
    float angle;
} pc;

layout(location = 0) out vec2 fragUV;

void main()
{
    float c = cos(pc.angle);
    float s = sin(pc.angle);
    vec2 p = vec2(inPos.x * c - inPos.y * s, inPos.x * s + inPos.y * c);
    gl_Position = vec4(p, 0.0, 1.0);
    fragUV = inUV;
}
