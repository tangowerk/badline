#version 450
layout(set = 0, binding = 0) uniform texture2D tex;
layout(set = 0, binding = 1) uniform sampler samp;
layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 col;
layout(location = 0) out vec4 color;

void main() {
    color = vec4(col.rgb, col.a * texture(sampler2D(tex, samp), uv).r);
}
