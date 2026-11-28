
static std::string OpenGLStockVertex = R"(
  #version 140

  in vec4 Position;
  in vec2 TexCoord;

  uniform mat4 MVP;

  out vec2 texCoordFrag;

  void main() {
    gl_Position = MVP * Position;
    texCoordFrag = TexCoord;
  }
)";

static std::string OpenGLStockFragment = R"(
  #version 140

  uniform sampler2D source;

  in vec2 texCoordFrag;

  out vec4 fragColor;

  void main() {
    fragColor = texture(source, texCoordFrag);
  }
)";

static std::string OpenGLOverlayVertexShader = R"(
  #version 140

  in vec2 pos;
  in vec2 uv;
  in vec4 col;
  uniform vec2 size;

  out vec2 texCoords;
  out vec4 color;

  void main() {
    texCoords = uv;
    color = col;
    gl_Position = vec4(pos.x / size.x * 2.0 - 1.0, 1.0 - pos.y / size.y * 2.0, 0.0, 1.0);
  }
)";

static std::string OpenGLOverlayFragmentShader = R"(
  #version 140

  in vec2 texCoords;
  in vec4 color;
  out vec4 glFragColor;

  uniform sampler2D tex;

  void main() {
    glFragColor = vec4(color.rgb, color.a * texture(tex, texCoords).r);
  }
)";
