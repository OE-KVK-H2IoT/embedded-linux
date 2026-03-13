/*
 * Step 3: Rotating Square
 *
 * Adds matrix math (identity + Y-rotation) and a uniform to animate
 * the square.  The vertex shader now multiplies by uMVP.
 *
 * Build:  cmake --build build --target step3_rotating_square
 * Run:    SDL_VIDEODRIVER=kmsdrm ./build/step3_rotating_square
 */

#include <SDL2/SDL.h>
#include <GLES2/gl2.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

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

/* ── Matrix math (column-major, OpenGL convention) ──── */

static void mat4_identity(float m[16])
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat4_rotate_y(float m[16], float a)
{
    mat4_identity(m);
    m[0] = cosf(a);  m[8]  = sinf(a);
    m[2] = -sinf(a); m[10] = cosf(a);
}

/* ── Main ───────────────────────────────────────────── */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    int w = 800, h = 480;
    SDL_Window *win = SDL_CreateWindow("Step 3: Rotating Square",
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
    SDL_GL_SetSwapInterval(1);
    if (getenv("HIDE_CURSOR")) SDL_ShowCursor(SDL_DISABLE);

    printf("GL Renderer: %s\n", glGetString(GL_RENDERER));
    printf("GL Version:  %s\n", glGetString(GL_VERSION));

    /* ── Shaders (NOW with uMVP matrix) ── */
    const char *vs_src =
        "attribute vec3 aPos;\n"
        "attribute vec3 aCol;\n"
        "uniform mat4 uMVP;\n"
        "varying vec3 vCol;\n"
        "void main() {\n"
        "    vCol = aCol;\n"
        "    gl_Position = uMVP * vec4(aPos, 1.0);\n"
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
    GLint loc_mvp = glGetUniformLocation(prog, "uMVP");

    /* ── Square geometry (same as Step 2) ── */
    const float verts[] = {
        -0.5f,  0.5f, 0.0f,  1.0f, 0.0f, 0.0f,
         0.5f,  0.5f, 0.0f,  0.0f, 1.0f, 0.0f,
         0.5f, -0.5f, 0.0f,  0.0f, 0.0f, 1.0f,
        -0.5f, -0.5f, 0.0f,  1.0f, 1.0f, 0.0f,
    };

    const GLushort indices[] = {
        0, 1, 2,
        2, 3, 0,
    };

    GLuint vbo, ibo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

    glGenBuffers(1, &ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    /* ── Render loop ── */
    Uint64 t_start = SDL_GetPerformanceCounter();
    double freq = (double)SDL_GetPerformanceFrequency();

    int running = 1;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)
                running = 0;
        }

        double sec = (double)(SDL_GetPerformanceCounter() - t_start) / freq;
        float angle = (float)sec;

        glViewport(0, 0, w, h);
        glClearColor(0.12f, 0.12f, 0.14f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        /* Build rotation matrix */
        float MVP[16];
        mat4_rotate_y(MVP, angle);

        glUseProgram(prog);
        glUniformMatrix4fv(loc_mvp, 1, GL_FALSE, MVP);

        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
        glEnableVertexAttribArray((GLuint)loc_pos);
        glVertexAttribPointer((GLuint)loc_pos, 3, GL_FLOAT, GL_FALSE,
                              6 * sizeof(float), (void *)0);
        glEnableVertexAttribArray((GLuint)loc_col);
        glVertexAttribPointer((GLuint)loc_col, 3, GL_FLOAT, GL_FALSE,
                              6 * sizeof(float), (void *)(3 * sizeof(float)));

        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (void *)0);

        SDL_GL_SwapWindow(win);
    }

    glDeleteProgram(prog);
    glDeleteBuffers(1, &vbo);
    glDeleteBuffers(1, &ibo);
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
