#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

// Decodes BC1-BC5 blocks for devices without textureCompressionBC, following the Khronos Data Format Specification
// (S3TC and RGTC sections). Each invocation decodes one 4x4 block into whole words of the decoded buffer: one word per
// texel for BC1-BC3 (RGBA8), one per block row for BC4 (R8) and two per block row for BC5 (RG8), so no two
// invocations write the same word and no 8-bit storage access is needed.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer source_words
{
    uint words[];
};

layout(buffer_reference, std430, buffer_reference_align = 4) writeonly buffer decoded_words
{
    uint words[];
};

const uint KIND_BC1_RGB = 0;
const uint KIND_BC1_RGBA = 1;
const uint KIND_BC2 = 2;
const uint KIND_BC3 = 3;
const uint KIND_BC4_UNORM = 4;
const uint KIND_BC4_SNORM = 5;
const uint KIND_BC5_UNORM = 6;
const uint KIND_BC5_SNORM = 7;

layout(push_constant) uniform parameters
{
    uvec2 source;
    uvec2 decoded;
    uint blocks_x;
    uint blocks_y;
    uint slices;
    uint source_row_bytes;
    uint source_slice_bytes;
    uint decoded_row_words;
    uint decoded_slice_words;
    uint kind;
} p;

uint expand(uint value, uint bits)
{
    return (value << (8 - bits)) | (value >> (2 * bits - 8));
}

uvec3 rgb565(uint color)
{
    return uvec3(expand((color >> 11) & 0x1F, 5), expand((color >> 5) & 0x3F, 6), expand(color & 0x1F, 5));
}

uint pack_rgba(uvec4 c)
{
    return c.r | (c.g << 8) | (c.b << 16) | (c.a << 24);
}

// The four colors of a BC1 color block, with the alpha the BC1 variant gives them. BC2 and BC3 always use four colors.
void color_palette(uint endpoints, bool four_colors, bool punch_through, out uvec4 palette[4])
{
    uint c0 = endpoints & 0xFFFF;
    uint c1 = endpoints >> 16;
    uvec3 a = rgb565(c0);
    uvec3 b = rgb565(c1);
    palette[0] = uvec4(a, 255);
    palette[1] = uvec4(b, 255);
    if (four_colors || c0 > c1)
    {
        palette[2] = uvec4((2 * a + b + 1) / 3, 255);
        palette[3] = uvec4((a + 2 * b + 1) / 3, 255);
    }
    else
    {
        palette[2] = uvec4((a + b + 1) / 2, 255);
        palette[3] = uvec4(0, 0, 0, punch_through ? 0 : 255);
    }
}

// The eight values of a BC3 alpha or BC4/BC5 channel block, as bytes; signed blocks give two's complement bytes.
void channel_palette(uint endpoints, bool is_signed, out uint palette[8])
{
    int e0 = int(endpoints & 0xFF);
    int e1 = int((endpoints >> 8) & 0xFF);
    int low = 0;
    int high = 255;
    if (is_signed)
    {
        e0 = max((e0 ^ 0x80) - 0x80, -127);
        e1 = max((e1 ^ 0x80) - 0x80, -127);
        low = -127;
        high = 127;
    }

    int values[8];
    values[0] = e0;
    values[1] = e1;
    if (e0 > e1)
    {
        for (int i = 1; i < 7; ++i)
        {
            values[i + 1] = int(round(float((7 - i) * e0 + i * e1) / 7.0));
        }
    }
    else
    {
        for (int i = 1; i < 5; ++i)
        {
            values[i + 1] = int(round(float((5 - i) * e0 + i * e1) / 5.0));
        }
        values[6] = low;
        values[7] = high;
    }

    for (int i = 0; i < 8; ++i)
    {
        palette[i] = uint(values[i]) & 0xFF;
    }
}

// The 3-bit index of a texel in a channel block, whose 48 index bits follow the two endpoint bytes.
uint channel_index(uint low_word, uint high_word, uint texel)
{
    uint bit = 16 + 3 * texel;
    uint value = bit < 32 ? (low_word >> bit) : (high_word >> (bit - 32));
    if (bit < 32 && bit > 29)
    {
        value |= high_word << (32 - bit);
    }
    return value & 7;
}

void decode_channel(uint low_word, uint high_word, bool is_signed, out uint texels[16])
{
    uint palette[8];
    channel_palette(low_word, is_signed, palette);
    for (uint t = 0; t < 16; ++t)
    {
        texels[t] = palette[channel_index(low_word, high_word, t)];
    }
}

void main()
{
    uint block_x = gl_GlobalInvocationID.x;
    uint block_y = gl_GlobalInvocationID.y;
    uint slice = gl_GlobalInvocationID.z;
    if (block_x >= p.blocks_x || block_y >= p.blocks_y || slice >= p.slices)
    {
        return;
    }

    bool sixteen_bytes = p.kind == KIND_BC2 || p.kind == KIND_BC3 || p.kind == KIND_BC5_UNORM || p.kind == KIND_BC5_SNORM;
    uint source_offset = slice * p.source_slice_bytes + block_y * p.source_row_bytes + block_x * (sixteen_bytes ? 16 : 8);
    source_words source = source_words(p.source);
    uint first = source_offset / 4;
    uint w0 = source.words[first];
    uint w1 = source.words[first + 1];
    uint w2 = sixteen_bytes ? source.words[first + 2] : 0;
    uint w3 = sixteen_bytes ? source.words[first + 3] : 0;

    decoded_words decoded = decoded_words(p.decoded);
    uint row_base = slice * p.decoded_slice_words + block_y * 4 * p.decoded_row_words;

    if (p.kind <= KIND_BC3)
    {
        uint color_endpoints = p.kind <= KIND_BC1_RGBA ? w0 : w2;
        uint color_indices = p.kind <= KIND_BC1_RGBA ? w1 : w3;
        uvec4 palette[4];
        color_palette(color_endpoints, p.kind >= KIND_BC2, p.kind == KIND_BC1_RGBA, palette);

        uint alphas[16];
        if (p.kind == KIND_BC3)
        {
            decode_channel(w0, w1, false, alphas);
        }

        for (uint t = 0; t < 16; ++t)
        {
            uvec4 color = palette[(color_indices >> (2 * t)) & 3];
            if (p.kind == KIND_BC2)
            {
                uint nibble = ((t < 8 ? w0 : w1) >> (4 * (t % 8))) & 0xF;
                color.a = nibble * 17;
            }
            else if (p.kind == KIND_BC3)
            {
                color.a = alphas[t];
            }
            decoded.words[row_base + (t / 4) * p.decoded_row_words + block_x * 4 + t % 4] = pack_rgba(color);
        }
        return;
    }

    bool is_signed = p.kind == KIND_BC4_SNORM || p.kind == KIND_BC5_SNORM;
    uint red[16];
    decode_channel(w0, w1, is_signed, red);

    if (p.kind <= KIND_BC4_SNORM)
    {
        for (uint row = 0; row < 4; ++row)
        {
            uint t = row * 4;
            decoded.words[row_base + row * p.decoded_row_words + block_x] =
                red[t] | (red[t + 1] << 8) | (red[t + 2] << 16) | (red[t + 3] << 24);
        }
        return;
    }

    uint green[16];
    decode_channel(w2, w3, is_signed, green);
    for (uint row = 0; row < 4; ++row)
    {
        uint t = row * 4;
        uint base = row_base + row * p.decoded_row_words + block_x * 2;
        decoded.words[base] = red[t] | (green[t] << 8) | (red[t + 1] << 16) | (green[t + 1] << 24);
        decoded.words[base + 1] = red[t + 2] | (green[t + 2] << 8) | (red[t + 3] << 16) | (green[t + 3] << 24);
    }
}
