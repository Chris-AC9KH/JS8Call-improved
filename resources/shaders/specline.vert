#version 440

// Positions arrive pre-transformed into clip space (see
// buildSpectrumGeometry() in CPlotter.cpp, which does the widget-pixel ->
// NDC -> clip-space conversion on the CPU once per frame, same as the
// rect corners for texquad.vert). Nothing left to do here but pass through.

layout(location = 0) in vec2 position;

void main()
{
    gl_Position = vec4(position, 0.0, 1.0);
}
