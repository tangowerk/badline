#version 450
// frame blending (M-Pacing3): latest picture (set 0) and its predecessor (set 1) mixed in
// linear light, `weight` = share of the latest picture
layout(set = 0, binding = 0) uniform texture2D tex;
layout(set = 0, binding = 1) uniform sampler samp;
layout(set = 1, binding = 0) uniform texture2D prevTex;
layout(set = 1, binding = 1) uniform sampler prevSamp;
layout(push_constant) uniform Push { mat4 mvp; float weight; } pc;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

vec3 toLinear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

vec3 toSrgb(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(max(c, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

void main() {
    vec4 a = texture(sampler2D(prevTex, prevSamp), uv);
    vec4 b = texture(sampler2D(tex, samp), uv);
    color = vec4(toSrgb(mix(toLinear(a.rgb), toLinear(b.rgb), pc.weight)), mix(a.a, b.a, pc.weight));
}
