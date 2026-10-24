#version 450

// Writes one sample of a block of rows of a multisampled image from raw bits; the pipeline's sample mask selects the
// sample.
#if defined(UINT_FORMAT)
#define OUTPUT uvec4
#define VALUE(bits) (bits)
#elif defined(SINT_FORMAT)
#define OUTPUT ivec4
#define VALUE(bits) ivec4(bits)
#else
#define OUTPUT vec4
#define VALUE(bits) uintBitsToFloat(bits)
#endif

layout(set = 0, binding = 1, std430) readonly buffer samples_buffer
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
};

#if defined(DEPTH_FORMAT)
void main()
{
    const uvec2 coordinate = uvec2(gl_FragCoord.xy);
    gl_FragDepth = uintBitsToFloat(samples[((coordinate.y - first_row) * width + coordinate.x) * sample_count + sample_index].x);
}
#else
layout(location = 0) out OUTPUT color;

void main()
{
    const uvec2 coordinate = uvec2(gl_FragCoord.xy);
    color = VALUE(samples[((coordinate.y - first_row) * width + coordinate.x) * sample_count + sample_index]);
}
#endif
