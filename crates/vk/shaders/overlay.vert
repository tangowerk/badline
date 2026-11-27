#version 450
// overlay quads: window pixels (origin top left), atlas uv, color
layout(push_constant) uniform Push { vec2 size; } pc;
layout(location = 0) in vec2 pos;
layout(location = 1) in vec2 uvIn;
layout(location = 2) in vec4 colIn;
layout(location = 0) out vec2 uv;
layout(location = 1) out vec4 col;

void main() {
    uv = uvIn;
    col = colIn;
    gl_Position = vec4(pos.x / pc.size.x * 2.0 - 1.0, pos.y / pc.size.y * 2.0 - 1.0, 0.0, 1.0);
}
