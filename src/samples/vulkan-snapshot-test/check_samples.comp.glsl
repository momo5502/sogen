#version 450
#extension GL_EXT_samplerless_texture_functions : require

layout(local_size_x = 64) in;

layout(binding = 0) uniform utexture2DMS integer_image;
layout(binding = 1) uniform texture2DMS depth_image;
layout(binding = 2) uniform utexture2DMS stencil_image;

layout(std430, binding = 3) buffer mismatches
{
    uint integer_mismatches;
    uint depth_mismatches;
    uint stencil_mismatches;
};

layout(push_constant) uniform expected
{
    uint integer_value;
    float depth_value;
    uint stencil_value;
    uint size;
    int samples;
};

void main()
{
    const uint index = gl_GlobalInvocationID.x;
    if (index >= size * size)
    {
        return;
    }

    const ivec2 position = ivec2(index % size, index / size);
    for (int i = 0; i < samples; ++i)
    {
        if (texelFetch(integer_image, position, i).r != integer_value)
        {
            atomicAdd(integer_mismatches, 1);
        }

        // D24 depth is quantized, so it is compared with a tolerance of one step.
        if (abs(texelFetch(depth_image, position, i).r - depth_value) > 1.0 / 16777216.0)
        {
            atomicAdd(depth_mismatches, 1);
        }

        if (texelFetch(stencil_image, position, i).r != stencil_value)
        {
            atomicAdd(stencil_mismatches, 1);
        }
    }
}
