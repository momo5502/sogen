#version 450
layout(location = 0) out vec3 fragColor;

// Per-draw transforms: mvp positions the cube in clip space; model carries its rotation so face normals
// can be lit in world space. Two mat4s = 128 bytes, the guaranteed minimum maxPushConstantsSize.
layout(push_constant) uniform PushConstants
{
    mat4 mvp;
    mat4 model;
} pc;

// 6 faces * 2 triangles * 3 vertices, baked in (no vertex buffer). The host draws each face with a
// firstVertex of face*6, so gl_VertexIndex/6 is the face index.
const vec3 positions[36] = vec3[](
    vec3(0.5, -0.5, -0.5), vec3(0.5, 0.5, -0.5), vec3(0.5, 0.5, 0.5),
    vec3(0.5, -0.5, -0.5), vec3(0.5, 0.5, 0.5), vec3(0.5, -0.5, 0.5),
    vec3(-0.5, -0.5, -0.5), vec3(-0.5, -0.5, 0.5), vec3(-0.5, 0.5, 0.5),
    vec3(-0.5, -0.5, -0.5), vec3(-0.5, 0.5, 0.5), vec3(-0.5, 0.5, -0.5),
    vec3(-0.5, 0.5, -0.5), vec3(-0.5, 0.5, 0.5), vec3(0.5, 0.5, 0.5),
    vec3(-0.5, 0.5, -0.5), vec3(0.5, 0.5, 0.5), vec3(0.5, 0.5, -0.5),
    vec3(-0.5, -0.5, -0.5), vec3(0.5, -0.5, -0.5), vec3(0.5, -0.5, 0.5),
    vec3(-0.5, -0.5, -0.5), vec3(0.5, -0.5, 0.5), vec3(-0.5, -0.5, 0.5),
    vec3(-0.5, -0.5, 0.5), vec3(0.5, -0.5, 0.5), vec3(0.5, 0.5, 0.5),
    vec3(-0.5, -0.5, 0.5), vec3(0.5, 0.5, 0.5), vec3(-0.5, 0.5, 0.5),
    vec3(-0.5, -0.5, -0.5), vec3(-0.5, 0.5, -0.5), vec3(0.5, 0.5, -0.5),
    vec3(-0.5, -0.5, -0.5), vec3(0.5, 0.5, -0.5), vec3(0.5, -0.5, -0.5)
);

const vec3 normals[6] = vec3[](
    vec3(1.0, 0.0, 0.0), vec3(-1.0, 0.0, 0.0),
    vec3(0.0, 1.0, 0.0), vec3(0.0, -1.0, 0.0),
    vec3(0.0, 0.0, 1.0), vec3(0.0, 0.0, -1.0)
);

const vec3 colors[6] = vec3[](
    vec3(0.90, 0.25, 0.25), vec3(0.25, 0.80, 0.35),
    vec3(0.30, 0.50, 1.00), vec3(0.95, 0.85, 0.25),
    vec3(0.85, 0.35, 0.85), vec3(0.30, 0.85, 0.90)
);

void main()
{
    const int face = gl_VertexIndex / 6;
    gl_Position = pc.mvp * vec4(positions[gl_VertexIndex], 1.0);

    const vec3 world_normal = normalize(mat3(pc.model) * normals[face]);
    const vec3 light_dir = normalize(vec3(0.4, 0.8, 0.5));
    const float diffuse = max(dot(world_normal, light_dir), 0.0) * 0.8 + 0.2; // 0.2 ambient
    fragColor = colors[face] * diffuse;
}
