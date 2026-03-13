/*
 * Step 1: Static Rainbow Triangle
 *
 * Minimal OpenGL ES 2.0 program — SDL2 window, vertex/fragment shaders,
 * one VBO, glDrawArrays.  No matrices, no index buffer.
 *
 * Build:  cmake --build build --target step1_triangle
 * Run:    SDL_VIDEODRIVER=kmsdrm ./build/step1_triangle
 */

#include <SDL2/SDL.h>
#include <GLES2/gl2.h>
#include <stdio.h>

/* ── Shader helpers ─────────────────────────────────── */

static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "Shader error: %s\n", log);
        return 0;
    }
    return s;
}

static GLuint link_program(GLuint vs, GLuint fs)
{
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        fprintf(stderr, "Link error: %s\n", log);
        return 0;
    }
    return p;
}

/* ── Main ───────────────────────────────────────────── */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    /* Request OpenGL ES 2.0 context */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    int w = 800, h = 480;
    SDL_Window *win = SDL_CreateWindow("Step 1: Triangle",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        w, h, SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!win) {
        fprintf(stderr, "Window: %s\n", SDL_GetError());
        SDL_Quit(); return 1;
    }

    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) {
        fprintf(stderr, "GL context: %s\n", SDL_GetError());
        SDL_DestroyWindow(win); SDL_Quit(); return 1;
    }
    SDL_GL_SetSwapInterval(1);  /* VSync ON */
    if (getenv("HIDE_CURSOR")) SDL_ShowCursor(SDL_DISABLE);

    printf("GL Renderer: %s\n", glGetString(GL_RENDERER));
    printf("GL Version:  %s\n", glGetString(GL_VERSION));

    /* ── Shaders (no matrix — clip-space coordinates) ── */
    const char *vs_src =
        "attribute vec3 aPos;\n"
        "attribute vec3 aCol;\n"
        "varying vec3 vCol;\n"
        "void main() {\n"
        "    vCol = aCol;\n"
        "    gl_Position = vec4(aPos, 1.0);\n"
        "}\n";

    const char *fs_src =
        "precision mediump float;\n"
        "varying vec3 vCol;\n"
        "void main() {\n"
        "    gl_FragColor = vec4(vCol, 1.0);\n"
        "}\n";

    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    GLuint prog = link_program(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!prog) return 1;

    GLint loc_pos = glGetAttribLocation(prog, "aPos");
    GLint loc_col = glGetAttribLocation(prog, "aCol");

    /* ── Triangle geometry: 3 vertices × (position + color) ── */
    const float verts[] = {
        /* x     y     z       r    g    b  */
         0.0f,  0.5f, 0.0f,  1.0f, 0.0f, 0.0f,   /* top    — red   */
        -0.5f, -0.5f, 0.0f,  0.0f, 1.0f, 0.0f,   /* left   — green */
         0.5f, -0.5f, 0.0f,  0.0f, 0.0f, 1.0f,   /* right  — blue  */
    };

    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

    /* ── Render loop ── */
    int running = 1, swipe_exit = 0;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
            if (e.type == SDL_KEYDOWN &&
                (e.key.keysym.sym == SDLK_ESCAPE ||
                 e.key.keysym.sym == SDLK_q))
                running = 0;
            if (e.type == SDL_FINGERDOWN && e.tfinger.y > 0.9f) swipe_exit = 1;
            if (e.type == SDL_FINGERUP) swipe_exit = 0;
            if (e.type == SDL_FINGERMOTION && swipe_exit && e.tfinger.y < 0.5f)
                running = 0;
        }

        glViewport(0, 0, w, h);
        glClearColor(0.12f, 0.12f, 0.14f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(prog);

        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glEnableVertexAttribArray((GLuint)loc_pos);
        glVertexAttribPointer((GLuint)loc_pos, 3, GL_FLOAT, GL_FALSE,
                              6 * sizeof(float), (void *)0);
        glEnableVertexAttribArray((GLuint)loc_col);
        glVertexAttribPointer((GLuint)loc_col, 3, GL_FLOAT, GL_FALSE,
                              6 * sizeof(float), (void *)(3 * sizeof(float)));

        glDrawArrays(GL_TRIANGLES, 0, 3);

        SDL_GL_SwapWindow(win);
    }

    glDeleteProgram(prog);
    glDeleteBuffers(1, &vbo);
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
