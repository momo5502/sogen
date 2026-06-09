#version 450

// Geometry comes from a real vertex buffer (position + color per vertex), not baked into the shader.
// A push-constant angle rotates the quad in 2D so it spins.

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec3 inColor;

layout(push_constant) uniform PushConstants
{
    float angle;
} pc;

layout(location = 0) out vec3 fragColor;

void main()
{
    float c = cos(pc.angle);
    float s = sin(pc.angle);
    vec2 p = vec2(inPos.x * c - inPos.y * s, inPos.x * s + inPos.y * c);
    gl_Position = vec4(p, 0.0, 1.0);
    fragColor = inColor;
}
