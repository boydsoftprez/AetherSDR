#version 440

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D waterfallTex;

layout(std140, binding = 0) uniform Params {
    float writeRow;     // normalized [0,1] — current write position in ring buffer
    float texHeight;    // texture height in pixels (for precise row offset)
    float pad0;
    float pad1;
};

void main()
{
    // Ring buffer scroll: offset V coordinate by writeRow
    float v = fract(v_uv.y + writeRow);
    fragColor = texture(waterfallTex, vec2(v_uv.x, v));
}
