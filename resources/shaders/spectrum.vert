#version 440

layout(location = 0) in vec2 position;  // pre-computed (x, y) in NDC

layout(location = 0) out vec4 v_color;

layout(std140, binding = 0) uniform Params {
    vec4  fillColor;     // RGBA fill/line color
};

void main()
{
    gl_Position = vec4(position, 0.0, 1.0);
    v_color = fillColor;
}
