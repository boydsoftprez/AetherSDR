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
    // Simple passthrough — ring buffer split is handled by drawing two quads
    // with pre-computed UV ranges (no fract wrapping, no seam).
    fragColor = texture(waterfallTex, v_uv);
}
