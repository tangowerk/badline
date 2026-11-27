#version 450
// emulator picture: unit quad (triangle strip), GL style MVP (rotation + y flip)
layout(push_constant) uniform Push { mat4 mvp; } pc;
layout(location = 0) out vec2 uv;

void main() {
    vec2 pos = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
    uv = pos;
    vec4 p = pc.mvp * vec4(pos, 0.0, 1.0);
    // GL clip space has y up, Vulkan y down
    gl_Position = vec4(p.x, -p.y, p.z, p.w);
}
