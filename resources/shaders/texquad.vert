#version 440

// Shared by every texture blit: waterfall, overlay, scale, dial x2, filter x2.
// The vertex buffer is always the same unit quad (-1..1); per-quad placement
// comes from the uniform "rect", already expressed in clip space by the
// CPU (see toClipSpace() in CPlotter.cpp) so this shader does no matrix math.

layout(location = 0) in vec2 position;   // unit quad, -1..1
layout(location = 1) in vec2 texCoord0;  // 0..1

layout(location = 0) out vec2 v_texCoord;

layout(std140, binding = 0) uniform buf {
    vec4 rect;    // x, y = top-left in clip space; z, w = width, height
    vec4 params;  // x = opacity, y = rowOffset (waterfall only, 0..1), z/w unused
} ubuf;

void main()
{
    vec2 unit = position * 0.5 + 0.5;           // 0..1
    vec2 clipPos = ubuf.rect.xy + unit * ubuf.rect.zw;
    gl_Position = vec4(clipPos, 0.0, 1.0);
    v_texCoord = texCoord0;
}
