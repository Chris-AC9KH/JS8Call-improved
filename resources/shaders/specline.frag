#version 440

layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    vec4 color; // set per-frame from m_lineColor (green/cyan/yellow, matching
                // the original Spectrum::Current/Cumulative/LinearAvg colors)
} ubuf;

void main()
{
    fragColor = ubuf.color;
}
