#version 450
layout(location = 0) out vec3 fragColor;
layout(push_constant) uniform PushConstants { mat4 mvp; } pc;

// 6 faces * 2 triangles * 3 vertices, baked in (no vertex buffer). gl_VertexIndex selects the
// vertex; the host draws each face with a firstVertex of face*6, so gl_VertexIndex/6 is the face.
const vec3 positions[36] = vec3[](
    // +X
    vec3(0.5, -0.5, -0.5), vec3(0.5, 0.5, -0.5), vec3(0.5, 0.5, 0.5),
    vec3(0.5, -0.5, -0.5), vec3(0.5, 0.5, 0.5), vec3(0.5, -0.5, 0.5),
    // -X
    vec3(-0.5, -0.5, -0.5), vec3(-0.5, -0.5, 0.5), vec3(-0.5, 0.5, 0.5),
    vec3(-0.5, -0.5, -0.5), vec3(-0.5, 0.5, 0.5), vec3(-0.5, 0.5, -0.5),
    // +Y
    vec3(-0.5, 0.5, -0.5), vec3(-0.5, 0.5, 0.5), vec3(0.5, 0.5, 0.5),
    vec3(-0.5, 0.5, -0.5), vec3(0.5, 0.5, 0.5), vec3(0.5, 0.5, -0.5),
    // -Y
    vec3(-0.5, -0.5, -0.5), vec3(0.5, -0.5, -0.5), vec3(0.5, -0.5, 0.5),
    vec3(-0.5, -0.5, -0.5), vec3(0.5, -0.5, 0.5), vec3(-0.5, -0.5, 0.5),
    // +Z
    vec3(-0.5, -0.5, 0.5), vec3(0.5, -0.5, 0.5), vec3(0.5, 0.5, 0.5),
    vec3(-0.5, -0.5, 0.5), vec3(0.5, 0.5, 0.5), vec3(-0.5, 0.5, 0.5),
    // -Z
    vec3(-0.5, -0.5, -0.5), vec3(-0.5, 0.5, -0.5), vec3(0.5, 0.5, -0.5),
    vec3(-0.5, -0.5, -0.5), vec3(0.5, 0.5, -0.5), vec3(0.5, -0.5, -0.5)
);

const vec3 colors[6] = vec3[](
    vec3(0.90, 0.20, 0.20), // +X red
    vec3(0.20, 0.80, 0.30), // -X green
    vec3(0.25, 0.45, 1.00), // +Y blue
    vec3(0.95, 0.85, 0.20), // -Y yellow
    vec3(0.85, 0.30, 0.85), // +Z magenta
    vec3(0.25, 0.85, 0.90)  // -Z cyan
);

void main()
{
    gl_Position = pc.mvp * vec4(positions[gl_VertexIndex], 1.0);
    fragColor = colors[gl_VertexIndex / 6];
}
