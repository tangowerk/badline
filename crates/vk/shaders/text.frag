#version 450
layout(push_constant) uniform Push { vec4 quad[4]; vec4 color; vec4 bgColor; } pc;
layout(set = 0, binding = 0) uniform texture2D tex;
layout(set = 0, binding = 1) uniform sampler samp;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

void main() {
    float a = texture(sampler2D(tex, samp), uv).r;
    if (pc.bgColor.a == 0.0)
        color = vec4(1.0, 1.0, 1.0, a) * pc.color;
    else
        color = vec4(mix(pc.bgColor.rgb, pc.color.rgb, a), pc.bgColor.a);
}
