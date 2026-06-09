#version 450

// Geometry comes from a vertex buffer (position + color). The rotation angle and a color tint come
// from a uniform buffer bound through a descriptor set (params.x = angle, params.yzw = tint), updated
// by the CPU every frame. params is a single vec4 to avoid std140 padding subtleties.

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec3 inColor;

layout(set = 0, binding = 0) uniform Transform
{
    vec4 params;
} ubo;

layout(location = 0) out vec3 fragColor;

void main()
{
    float angle = ubo.params.x;
    vec3 tint = ubo.params.yzw;
    float c = cos(angle);
    float s = sin(angle);
    vec2 p = vec2(inPos.x * c - inPos.y * s, inPos.x * s + inPos.y * c);
    gl_Position = vec4(p, 0.0, 1.0);
    fragColor = inColor * tint;
}
