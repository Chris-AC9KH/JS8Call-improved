#version 440

layout(location = 0) in vec2 v_texCoord;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    vec4 rect;
    vec4 params;  // x = opacity, y = rowOffset
} ubuf;

layout(binding = 1) uniform sampler2D tex;

void main()
{
    vec2 uv = v_texCoord;

    // 1. If rowOffset is exactly 0 and opacity is 1.0, it's a static overlay layer
    // we invert uv.y natively here to flip the text right-side up!
    if (ubuf.params.y == 0.0) {
        uv.y = 1.0 - uv.y;
    } else {
        // Keeps your working waterfall circular ring buffer scrolling correctly
        uv.y = fract(uv.y + ubuf.params.y);
    }

    fragColor = texture(tex, uv);
    fragColor.a *= ubuf.params.x; // opacity
}
