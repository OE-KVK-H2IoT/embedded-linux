/*
 * sdl2_touch_paint - Touch-responsive drawing application
 *
 * Draws on a persistent canvas texture using SDL2's 2D renderer.
 * Supports both mouse and multi-touch input (SDL_FINGER* events).
 *
 * Controls:
 *   Esc       — quit
 *   c         — clear canvas
 *   n         — next colour
 *   [ / ]     — decrease / increase brush size
 *
 * Build:
 *   cmake -B build && cmake --build build
 *
 * Run:
 *   SDL_VIDEODRIVER=kmsdrm ./build/sdl2_touch_paint
 *
 * Copyright (C) 2025 Obuda University - Embedded Systems Lab
 * SPDX-License-Identifier: MIT
 */

#include <SDL2/SDL.h>
#include <math.h>
#include <stdio.h>

/* ── Brush drawing ────────────────────────────── */

static void draw_circle(SDL_Renderer *r, int cx, int cy, int radius)
{
    for (int dy = -radius; dy <= radius; dy++) {
        int dx = (int)sqrtf((float)(radius * radius - dy * dy));
        SDL_RenderDrawLine(r, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

static void stamp_line(SDL_Renderer *r, int x0, int y0,
                       int x1, int y1, int radius)
{
    float dx = (float)(x1 - x0), dy = (float)(y1 - y0);
    float dist = sqrtf(dx * dx + dy * dy);
    float step = (radius > 1) ? radius * 0.5f : 1.0f;
    int n = (dist < 0.001f) ? 1 : (int)ceilf(dist / step);

    for (int i = 0; i <= n; i++) {
        float t = (n == 0) ? 0.0f : (float)i / (float)n;
        int x = (int)lroundf((float)x0 + t * dx);
        int y = (int)lroundf((float)y0 + t * dy);
        draw_circle(r, x, y, radius);
    }
}

/* ── Main ─────────────────────────────────────── */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    int w = 800, h = 480;
    SDL_Window *win = SDL_CreateWindow("Touch Paint",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        w, h, SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!win) { fprintf(stderr, "Window: %s\n", SDL_GetError()); return 1; }

    SDL_GetWindowSize(win, &w, &h);

    SDL_Renderer *ren = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) { fprintf(stderr, "Renderer: %s\n", SDL_GetError()); return 1; }
    if (getenv("HIDE_CURSOR")) SDL_ShowCursor(SDL_DISABLE);

    /* Canvas texture — we draw strokes here and keep them */
    SDL_Texture *canvas = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA8888,
        SDL_TEXTUREACCESS_TARGET, w, h);
    SDL_SetRenderTarget(ren, canvas);
    SDL_SetRenderDrawColor(ren, 20, 20, 24, 255);
    SDL_RenderClear(ren);
    SDL_SetRenderTarget(ren, NULL);

    int running = 1, drawing = 0;
    int last_x = 0, last_y = 0;
    int brush = 6;

    /* Measurement state */
    Uint64 freq = SDL_GetPerformanceFrequency();
    Uint64 t_start = SDL_GetPerformanceCounter();
    Uint64 t_report = t_start;
    int frame_count = 0;
    int event_count = 0;         /* touch/mouse motion events per interval */
    unsigned long long points = 0;

    /* Colors: cycle with 'n' key */
    SDL_Color colors[] = {
        {240,240,240,255}, {255,80,80,255}, {80,255,80,255},
        {80,80,255,255}, {255,255,80,255}, {255,80,255,255},
    };
    int ci = 0;
    int num_colors = sizeof(colors) / sizeof(colors[0]);

    int swipe_exit = 0;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            /* Swipe up from bottom edge to exit */
            if (e.type == SDL_FINGERDOWN && e.tfinger.y > 0.9f)
                swipe_exit = 1;
            if (e.type == SDL_FINGERUP)
                swipe_exit = 0;
            if (e.type == SDL_FINGERMOTION && swipe_exit &&
                e.tfinger.y < 0.5f)
                running = 0;

            switch (e.type) {
            case SDL_QUIT:
                running = 0; break;
            case SDL_KEYDOWN:
                if (e.key.keysym.sym == SDLK_ESCAPE ||
                    e.key.keysym.sym == SDLK_q) running = 0;
                if (e.key.keysym.sym == SDLK_c) {
                    SDL_SetRenderTarget(ren, canvas);
                    SDL_SetRenderDrawColor(ren, 20, 20, 24, 255);
                    SDL_RenderClear(ren);
                    SDL_SetRenderTarget(ren, NULL);
                    points = 0;
                }
                if (e.key.keysym.sym == SDLK_LEFTBRACKET && brush > 1) brush--;
                if (e.key.keysym.sym == SDLK_RIGHTBRACKET && brush < 40) brush++;
                if (e.key.keysym.sym == SDLK_n) ci = (ci + 1) % num_colors;
                break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    w = e.window.data1; h = e.window.data2;
                    SDL_DestroyTexture(canvas);
                    canvas = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA8888,
                        SDL_TEXTUREACCESS_TARGET, w, h);
                    SDL_SetRenderTarget(ren, canvas);
                    SDL_SetRenderDrawColor(ren, 20, 20, 24, 255);
                    SDL_RenderClear(ren);
                    SDL_SetRenderTarget(ren, NULL);
                }
                break;

            /* Mouse input */
            case SDL_MOUSEBUTTONDOWN:
                if (e.button.button == SDL_BUTTON_LEFT) {
                    drawing = 1; last_x = e.button.x; last_y = e.button.y;
                } break;
            case SDL_MOUSEBUTTONUP:
                if (e.button.button == SDL_BUTTON_LEFT) drawing = 0;
                break;
            case SDL_MOUSEMOTION:
                if (drawing) {
                    SDL_SetRenderTarget(ren, canvas);
                    SDL_SetRenderDrawColor(ren,
                        colors[ci].r, colors[ci].g, colors[ci].b, 255);
                    stamp_line(ren, last_x, last_y, e.motion.x, e.motion.y, brush);
                    SDL_SetRenderTarget(ren, NULL);
                    last_x = e.motion.x; last_y = e.motion.y;
                    points++; event_count++;
                } break;

            /* Touch input (normalized 0..1 coordinates) */
            case SDL_FINGERDOWN:
                drawing = 1;
                last_x = (int)(e.tfinger.x * w);
                last_y = (int)(e.tfinger.y * h);
                break;
            case SDL_FINGERUP:
                drawing = 0; break;
            case SDL_FINGERMOTION:
                if (drawing) {
                    int tx = (int)(e.tfinger.x * w);
                    int ty = (int)(e.tfinger.y * h);
                    SDL_SetRenderTarget(ren, canvas);
                    SDL_SetRenderDrawColor(ren,
                        colors[ci].r, colors[ci].g, colors[ci].b, 255);
                    stamp_line(ren, last_x, last_y, tx, ty, brush);
                    SDL_SetRenderTarget(ren, NULL);
                    last_x = tx; last_y = ty;
                    points++; event_count++;
                } break;
            }
        }

        /* Present canvas + crosshair */
        SDL_SetRenderTarget(ren, NULL);
        SDL_RenderCopy(ren, canvas, NULL, NULL);
        SDL_SetRenderDrawColor(ren, 255, 80, 80, 200);
        SDL_RenderDrawLine(ren, last_x - 14, last_y, last_x + 14, last_y);
        SDL_RenderDrawLine(ren, last_x, last_y - 14, last_x, last_y + 14);
        SDL_RenderPresent(ren);

        frame_count++;
        Uint64 now = SDL_GetPerformanceCounter();
        double dt = (double)(now - t_report) / (double)freq;
        if (dt >= 1.0) {
            printf("FPS: %d | Events/sec: %d | Total points: %llu | "
                   "Brush: %dpx\n",
                   frame_count, event_count, points, brush);
            frame_count = 0; event_count = 0;
            t_report = now;
        }
    }

    SDL_DestroyTexture(canvas);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
