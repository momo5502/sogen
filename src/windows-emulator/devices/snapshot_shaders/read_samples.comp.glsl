#version 450
#extension GL_EXT_samplerless_texture_functions : require

// Reads every sample of one layer of a multisampled image, a block of rows at a time, as raw bits.
#if defined(UINT_FORMAT)
#define SOURCE utexture2DMSArray
#define BITS(value) (value)
#elif defined(SINT_FORMAT)
#define SOURCE itexture2DMSArray
#define BITS(value) uvec4(value)
#else
#define SOURCE texture2DMSArray
#define BITS(value) floatBitsToUint(value)
#endif

layout(local_size_x = 64) in;

layout(set = 0, binding = 0) uniform SOURCE source_image;
layout(set = 0, binding = 1, std430) writeonly buffer samples_buffer
{
    uvec4 samples[];
};

layout(push_constant) uniform parameters
{
    uint width;
    uint rows;
    uint sample_count;
    uint layer;
    uint first_row;
    uint sample_index;
    uint stencil_bit;
};

void main()
{
    const uint index = gl_GlobalInvocationID.x;
    const uint texel = index / sample_count;
    if (texel >= width * rows)
    {
        return;
    }

    const ivec3 coordinate = ivec3(texel % width, first_row + texel / width, layer);
    samples[index] = BITS(texelFetch(source_image, coordinate, int(index % sample_count)));
}
