#version 450
// screen text: quad corners in GL NDC (x, y, u, v), triangle strip
layout(push_constant) uniform Push { vec4 quad[4]; vec4 color; vec4 bgColor; } pc;
layout(location = 0) out vec2 uv;

void main() {
    // constant indices only: a dynamically indexed push constant array reads wrong values here (naga/RADV)
    vec4 q = pc.quad[3];
    if (gl_VertexIndex == 0) q = pc.quad[0];
    else if (gl_VertexIndex == 1) q = pc.quad[1];
    else if (gl_VertexIndex == 2) q = pc.quad[2];
    uv = q.zw;
    gl_Position = vec4(q.x, -q.y, 0.0, 1.0);
}
