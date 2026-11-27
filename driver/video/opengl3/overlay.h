// Overlay UI renderer: one glyph atlas texture + a list of textured, colored quads.
// The frame is built by the program layer; this only uploads and draws it.

struct GLOverlay {
    GLuint program = 0;
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint tex = 0;
    unsigned texId = 0;
    GLint sizeLocation = -1;
    bool initialized = false;

    std::mutex mutex;
    std::shared_ptr<const OverlayFrame> frame;

    ~GLOverlay() { term(); }

    auto set(std::shared_ptr<const OverlayFrame> _frame) -> void {
        std::lock_guard<std::mutex> lock(mutex);
        frame = _frame;
    }

    auto visible() -> bool {
        std::lock_guard<std::mutex> lock(mutex);
        return frame && !frame->vertices.empty();
    }

    auto init() -> bool {
        std::string error;
        GLuint vertex = GLUtility::createShader(GL_VERTEX_SHADER, OpenGLOverlayVertexShader.c_str(), error);
        GLuint fragment = GLUtility::createShader(GL_FRAGMENT_SHADER, OpenGLOverlayFragmentShader.c_str(), error);

        if (vertex && fragment)
            program = GLUtility::createProgram(vertex, fragment, error, true);

        GLUtility::deleteShader(vertex);
        GLUtility::deleteShader(fragment);
        if (!program)
            return false;

        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);

        GLsizei stride = 8 * sizeof(float);
        GLint pos = glGetAttribLocation(program, "pos");
        GLint uv = glGetAttribLocation(program, "uv");
        GLint col = glGetAttribLocation(program, "col");
        glEnableVertexAttribArray(pos);
        glVertexAttribPointer(pos, 2, GL_FLOAT, GL_FALSE, stride, (void*)0);
        glEnableVertexAttribArray(uv);
        glVertexAttribPointer(uv, 2, GL_FLOAT, GL_FALSE, stride, (void*)(2 * sizeof(float)));
        glEnableVertexAttribArray(col);
        glVertexAttribPointer(col, 4, GL_FLOAT, GL_FALSE, stride, (void*)(4 * sizeof(float)));

        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindVertexArray(0);

        glUseProgram(program);
        glUniform1i(glGetUniformLocation(program, "tex"), 0);
        sizeLocation = glGetUniformLocation(program, "size");
        initialized = true;
        return true;
    }

    auto term() -> void {
        if (program) { glDeleteProgram(program); program = 0; }
        if (vbo) { glDeleteBuffers(1, &vbo); vbo = 0; }
        if (vao) { glDeleteVertexArrays(1, &vao); vao = 0; }
        if (tex) { glDeleteTextures(1, &tex); tex = 0; }
        texId = 0;
        initialized = false;
    }

    // draws over the whole window (render thread, context current)
    auto show(unsigned windowWidth, unsigned windowHeight) -> void {
        std::shared_ptr<const OverlayFrame> f;
        {
            std::lock_guard<std::mutex> lock(mutex);
            f = frame;
        }
        if (!initialized || !f || f->vertices.empty() || !f->atlas || !windowWidth || !windowHeight)
            return;

        glActiveTexture(GL_TEXTURE0);
        if (!tex || texId != f->atlasId) {
            if (!tex)
                glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            GLUtility::glParameters(GL_CLAMP_TO_EDGE, GL_NEAREST, GL_NEAREST);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, f->atlasWidth, f->atlasHeight, 0, GL_RED, GL_UNSIGNED_BYTE, f->atlas->data());
            texId = f->atlasId;
        } else
            glBindTexture(GL_TEXTURE_2D, tex);

        GLint prevViewport[4];
        glGetIntegerv(GL_VIEWPORT, prevViewport);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, windowWidth, windowHeight);

        glUseProgram(program);
        // vertices are in the pixel space of the frame; stretch if the window was resized meanwhile
        glUniform2f(sizeLocation, (float)f->width, (float)f->height);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, f->vertices.size() * sizeof(float), f->vertices.data(), GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(f->vertices.size() / 8));
        glDisable(GL_BLEND);

        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindVertexArray(0);
        glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    }
};
