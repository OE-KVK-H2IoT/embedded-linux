/*
 * sdl2_dashboard.c - Multi-gauge SDL2 dashboard (no compositor)
 *
 * Reads MCP9808 temperature (I2C via sysfs), BMI160 IMU angles
 * (character device), and CPU usage (/proc/stat), rendering a
 * fullscreen dashboard with three panels:
 *
 *   ┌──────────┐ ┌──────────┐ ┌─────┐
 *   │  TEMP    │ │ HORIZON  │ │ CPU │
 *   │  gauge   │ │  strip   │ │ bar │
 *   │  (arc)   │ │          │ │     │
 *   │  23.5°C  │ │  ═══╲═══ │ │ ▓▓░ │
 *   └──────────┘ └──────────┘ └─────┘
 *
 * Keyboard fallback: when sensors are absent, arrow keys adjust
 * simulated temperature (Up/Down) and roll (Left/Right).
 *
 * Build:
 *   gcc -Wall -O2 $(sdl2-config --cflags) \
 *       -o sdl2_dashboard sdl2_dashboard.c \
 *       $(sdl2-config --libs) -lSDL2_ttf -lm
 *
 * Usage:
 *   SDL_VIDEODRIVER=kmsdrm ./sdl2_dashboard
 *
 * Copyright (C) 2025 Obuda University - Embedded Systems Lab
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

/* ---------- configuration ---------- */

#define WINDOW_W  800
#define WINDOW_H  480
#define POLL_HZ   10        /* sensor polling rate */

/* Sensor paths */
#define MCP9808_GLOB  "/sys/bus/i2c/devices/1-0018/hwmon/hwmon*/temp1_input"
#define BMI160_DEV    "/dev/bmi160"

/* Font — searched in order (Debian/Ubuntu, Arch, Fedora) */
static const char *font_search_paths[] = {
	"/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",  /* Debian/Ubuntu */
	"/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",              /* Arch */
	"/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf", /* Fedora */
	NULL
};
#define FONT_SIZE     24
#define FONT_SIZE_LG  36

/* Temperature gauge range */
#define TEMP_MIN      15.0f
#define TEMP_MAX      45.0f

/* Arc gauge parameters */
#define ARC_START_DEG  135.0f   /* start angle (degrees, CCW from 3 o'clock) */
#define ARC_END_DEG    405.0f   /* end angle */
#define ARC_SEGMENTS   60       /* line segments for smooth arc */

/* Colours */
#define BG_R   25
#define BG_G   25
#define BG_B   30

#define ARC_R  0
#define ARC_G  200
#define ARC_B  120

#define ARC_BG_R  60
#define ARC_BG_G  60
#define ARC_BG_B  65

#define SKY_R    50
#define SKY_G   130
#define SKY_B   200

#define GND_R   140
#define GND_G    90
#define GND_B    40

#define HOR_R   255
#define HOR_G   255
#define HOR_B   255

#define BAR_R    80
#define BAR_G   180
#define BAR_B   255

#define BAR_BG_R  50
#define BAR_BG_G  50
#define BAR_BG_B  55

#define TEXT_R   220
#define TEXT_G   220
#define TEXT_B   220

/* ---------- global state ---------- */

static volatile sig_atomic_t g_running = 1;

/* ---------- signal handler ---------- */

static void handle_signal(int sig)
{
	(void)sig;
	g_running = 0;
}

/* ---------- sensor reading ---------- */

/*
 * Read MCP9808 temperature from sysfs (milli-degrees Celsius).
 * Returns temperature in °C, or NAN on failure.
 */
static float read_temperature(void)
{
	static char path[256] = {0};
	FILE *f;
	int millideg;

	/* Resolve glob pattern on first call */
	if (path[0] == '\0') {
		glob_t g;
		if (glob(MCP9808_GLOB, 0, NULL, &g) != 0 || g.gl_pathc == 0) {
			globfree(&g);
			return NAN;
		}
		strncpy(path, g.gl_pathv[0], sizeof(path) - 1);
		globfree(&g);
	}

	f = fopen(path, "r");
	if (!f)
		return NAN;

	if (fscanf(f, "%d", &millideg) != 1) {
		fclose(f);
		return NAN;
	}
	fclose(f);

	return millideg / 1000.0f;
}

/*
 * Read BMI160 IMU angles from character device.
 * Expected format: "AX AY AZ GX GY GZ\n"
 * Returns 0 on success, -1 on failure.
 */
static int read_imu(float *roll, float *pitch)
{
	int fd;
	char buf[128];
	int n;
	int16_t ax, ay, az, gx, gy, gz;

	fd = open(BMI160_DEV, O_RDONLY);
	if (fd < 0)
		return -1;

	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);

	if (n <= 0)
		return -1;
	buf[n] = '\0';

	if (sscanf(buf, "%hd %hd %hd %hd %hd %hd",
		   &ax, &ay, &az, &gx, &gy, &gz) != 6)
		return -1;

	float fay = (float)ay / 16384.0f;
	float faz = (float)az / 16384.0f;
	float fax = (float)ax / 16384.0f;

	*roll  = atan2f(fay, faz) * (180.0f / (float)M_PI);
	*pitch = atan2f(-fax, sqrtf(fay * fay + faz * faz))
		 * (180.0f / (float)M_PI);
	return 0;
}

/*
 * Read CPU usage from /proc/stat.
 * Returns percentage (0-100) or -1.0 on failure.
 */
static float read_cpu_usage(void)
{
	static unsigned long prev_total = 0, prev_idle = 0;
	FILE *f;
	char line[256];
	unsigned long user, nice, system, idle, iowait, irq, softirq, steal;
	unsigned long total, diff_total, diff_idle;

	f = fopen("/proc/stat", "r");
	if (!f)
		return -1.0f;

	if (!fgets(line, sizeof(line), f)) {
		fclose(f);
		return -1.0f;
	}
	fclose(f);

	if (sscanf(line, "cpu %lu %lu %lu %lu %lu %lu %lu %lu",
		   &user, &nice, &system, &idle, &iowait,
		   &irq, &softirq, &steal) < 4)
		return -1.0f;

	total = user + nice + system + idle + iowait + irq + softirq + steal;
	diff_total = total - prev_total;
	diff_idle  = idle - prev_idle;

	prev_total = total;
	prev_idle  = idle;

	if (diff_total == 0)
		return 0.0f;

	return 100.0f * (1.0f - (float)diff_idle / (float)diff_total);
}

/* ---------- rendering helpers ---------- */

static void render_text(SDL_Renderer *ren, TTF_Font *font,
			const char *text, int x, int y,
			uint8_t r, uint8_t g, uint8_t b)
{
	SDL_Color col = { r, g, b, 255 };
	SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, col);
	if (!surf)
		return;
	SDL_Texture *tex = SDL_CreateTextureFromSurface(ren, surf);
	if (!tex) {
		SDL_FreeSurface(surf);
		return;
	}
	SDL_Rect dst = { x, y, surf->w, surf->h };
	SDL_RenderCopy(ren, tex, NULL, &dst);
	SDL_DestroyTexture(tex);
	SDL_FreeSurface(surf);
}

static void render_text_centered(SDL_Renderer *ren, TTF_Font *font,
				 const char *text, int cx, int cy,
				 uint8_t r, uint8_t g, uint8_t b)
{
	int tw, th;
	TTF_SizeUTF8(font, text, &tw, &th);
	render_text(ren, font, text, cx - tw / 2, cy - th / 2, r, g, b);
}

/*
 * Draw an arc (series of line segments) from start_deg to end_deg.
 */
static void draw_arc(SDL_Renderer *ren, int cx, int cy, int radius,
		     float start_deg, float end_deg, int segments,
		     uint8_t r, uint8_t g, uint8_t b)
{
	SDL_SetRenderDrawColor(ren, r, g, b, 255);
	float step = (end_deg - start_deg) / (float)segments;

	for (int i = 0; i < segments; i++) {
		float a1 = (start_deg + step * i) * (float)M_PI / 180.0f;
		float a2 = (start_deg + step * (i + 1)) * (float)M_PI / 180.0f;
		int x1 = cx + (int)(radius * cosf(a1));
		int y1 = cy - (int)(radius * sinf(a1));
		int x2 = cx + (int)(radius * cosf(a2));
		int y2 = cy - (int)(radius * sinf(a2));
		SDL_RenderDrawLine(ren, x1, y1, x2, y2);
	}
}

/*
 * Draw the temperature arc gauge.
 */
static void render_temp_gauge(SDL_Renderer *ren, TTF_Font *font,
			      TTF_Font *font_lg,
			      int cx, int cy, int radius,
			      float temperature)
{
	/* Background arc */
	draw_arc(ren, cx, cy, radius,
		 ARC_START_DEG, ARC_END_DEG, ARC_SEGMENTS,
		 ARC_BG_R, ARC_BG_G, ARC_BG_B);
	draw_arc(ren, cx, cy, radius - 1,
		 ARC_START_DEG, ARC_END_DEG, ARC_SEGMENTS,
		 ARC_BG_R, ARC_BG_G, ARC_BG_B);

	/* Value arc */
	float t = (temperature - TEMP_MIN) / (TEMP_MAX - TEMP_MIN);
	if (t < 0.0f) t = 0.0f;
	if (t > 1.0f) t = 1.0f;
	float value_end = ARC_START_DEG + t * (ARC_END_DEG - ARC_START_DEG);

	draw_arc(ren, cx, cy, radius,
		 ARC_START_DEG, value_end, (int)(ARC_SEGMENTS * t) + 1,
		 ARC_R, ARC_G, ARC_B);
	draw_arc(ren, cx, cy, radius - 1,
		 ARC_START_DEG, value_end, (int)(ARC_SEGMENTS * t) + 1,
		 ARC_R, ARC_G, ARC_B);
	draw_arc(ren, cx, cy, radius - 2,
		 ARC_START_DEG, value_end, (int)(ARC_SEGMENTS * t) + 1,
		 ARC_R, ARC_G, ARC_B);

	/* Temperature text */
	char buf[32];
	if (isnan(temperature))
		snprintf(buf, sizeof(buf), "-- °C");
	else
		snprintf(buf, sizeof(buf), "%.1f °C", temperature);
	render_text_centered(ren, font_lg, buf, cx, cy,
			     TEXT_R, TEXT_G, TEXT_B);

	/* Label */
	render_text_centered(ren, font, "TEMPERATURE", cx, cy + radius / 2,
			     ARC_BG_R + 40, ARC_BG_G + 40, ARC_BG_B + 40);
}

/*
 * Draw the horizon indicator panel.
 */
static void render_horizon(SDL_Renderer *ren, TTF_Font *font,
			   int x, int y, int w, int h,
			   float roll, float pitch)
{
	/* Clip region (just use a filled background) */
	int horizon_y = h / 2 + (int)(pitch * 2.0f);

	/* Sky */
	SDL_Rect sky = { x, y, w, horizon_y };
	if (sky.h > h) sky.h = h;
	if (sky.h > 0) {
		SDL_SetRenderDrawColor(ren, SKY_R, SKY_G, SKY_B, 255);
		SDL_RenderFillRect(ren, &sky);
	}

	/* Ground */
	if (horizon_y < h) {
		SDL_Rect gnd = { x, y + horizon_y, w, h - horizon_y };
		SDL_SetRenderDrawColor(ren, GND_R, GND_G, GND_B, 255);
		SDL_RenderFillRect(ren, &gnd);
	}

	/* Horizon line (rotated by roll) */
	float roll_rad = roll * (float)M_PI / 180.0f;
	int cx = x + w / 2;
	int cy = y + horizon_y;
	int half = (int)(w * 0.45f);
	int dx = (int)(half * cosf(roll_rad));
	int dy = (int)(half * sinf(roll_rad));

	SDL_SetRenderDrawColor(ren, HOR_R, HOR_G, HOR_B, 255);
	SDL_RenderDrawLine(ren, cx - dx, cy + dy, cx + dx, cy - dy);

	/* Center crosshair */
	int scx = x + w / 2;
	int scy = y + h / 2;
	SDL_RenderDrawLine(ren, scx - 12, scy, scx + 12, scy);
	SDL_RenderDrawLine(ren, scx, scy - 12, scx, scy + 12);

	/* Roll/Pitch text */
	char buf[64];
	snprintf(buf, sizeof(buf), "R:%.0f° P:%.0f°", roll, pitch);
	render_text(ren, font, buf, x + 8, y + h - 28,
		    TEXT_R, TEXT_G, TEXT_B);

	/* Label */
	render_text_centered(ren, font, "HORIZON", x + w / 2, y + 16,
			     HOR_R, HOR_G, HOR_B);
}

/*
 * Draw the CPU usage bar.
 */
static void render_cpu_bar(SDL_Renderer *ren, TTF_Font *font,
			   int x, int y, int w, int h,
			   float cpu_percent)
{
	/* Background bar */
	SDL_Rect bg = { x, y, w, h };
	SDL_SetRenderDrawColor(ren, BAR_BG_R, BAR_BG_G, BAR_BG_B, 255);
	SDL_RenderFillRect(ren, &bg);

	/* Fill bar (bottom-up) */
	if (cpu_percent < 0.0f) cpu_percent = 0.0f;
	if (cpu_percent > 100.0f) cpu_percent = 100.0f;
	int fill_h = (int)(h * cpu_percent / 100.0f);
	SDL_Rect bar = { x, y + h - fill_h, w, fill_h };
	SDL_SetRenderDrawColor(ren, BAR_R, BAR_G, BAR_B, 255);
	SDL_RenderFillRect(ren, &bar);

	/* Percentage text */
	char buf[16];
	snprintf(buf, sizeof(buf), "%.0f%%", cpu_percent);
	render_text_centered(ren, font, buf, x + w / 2, y + h / 2,
			     TEXT_R, TEXT_G, TEXT_B);

	/* Label */
	render_text_centered(ren, font, "CPU", x + w / 2, y - 20,
			     TEXT_R, TEXT_G, TEXT_B);
}

/* ---------- main ---------- */

int main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

	/* Install signal handlers */
	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	/* Probe sensors */
	int has_temp = !isnan(read_temperature());
	float dummy_r, dummy_p;
	int has_imu  = (read_imu(&dummy_r, &dummy_p) == 0);

	if (!has_temp)
		fprintf(stderr, "MCP9808 not found — using keyboard fallback "
				"(Up/Down)\n");
	if (!has_imu)
		fprintf(stderr, "BMI160 not found — using keyboard fallback "
				"(Left/Right)\n");

	/* Simulated values for keyboard fallback */
	float sim_temp  = 22.0f;
	float sim_roll  = 0.0f;
	float sim_pitch = 0.0f;

	/* Initialise SDL2 + TTF */
	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	if (TTF_Init() < 0) {
		fprintf(stderr, "TTF_Init: %s\n", TTF_GetError());
		SDL_Quit();
		return 1;
	}

	/* Find font: try each known path */
	const char *font_path = NULL;
	for (const char **p = font_search_paths; *p; p++) {
		if (access(*p, R_OK) == 0) {
			font_path = *p;
			break;
		}
	}
	if (!font_path) {
		fprintf(stderr, "DejaVuSans-Bold.ttf not found.\n"
			"Install: sudo apt install fonts-dejavu-core  "
			"(Debian/Ubuntu)\n"
			"     or: sudo pacman -S ttf-dejavu  (Arch)\n"
			"     or: sudo dnf install dejavu-sans-fonts  "
			"(Fedora)\n");
		TTF_Quit();
		SDL_Quit();
		return 1;
	}

	SDL_Window *win = SDL_CreateWindow(
		"Dashboard",
		SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		WINDOW_W, WINDOW_H,
		SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_SHOWN);
	if (!win) {
		fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
		TTF_Quit();
		SDL_Quit();
		return 1;
	}

	SDL_Renderer *ren = SDL_CreateRenderer(
		win, -1,
		SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (!ren) {
		fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
		SDL_DestroyWindow(win);
		TTF_Quit();
		SDL_Quit();
		return 1;
	}

	/* Use renderer output size, not window size — correct on HiDPI */
	int win_w, win_h;
	SDL_GetRendererOutputSize(ren, &win_w, &win_h);

	/* Scale font sizes to output resolution (baseline: 480p) */
	int font_size    = FONT_SIZE * win_h / 480;
	int font_size_lg = FONT_SIZE_LG * win_h / 480;

	TTF_Font *font = TTF_OpenFont(font_path, font_size);
	TTF_Font *font_lg = TTF_OpenFont(font_path, font_size_lg);
	if (!font || !font_lg) {
		fprintf(stderr, "TTF_OpenFont(%s): %s\n",
			font_path, TTF_GetError());
		SDL_DestroyRenderer(ren);
		SDL_DestroyWindow(win);
		TTF_Quit();
		SDL_Quit();
		return 1;
	}

	/* Prime CPU usage (first read is always 0) */
	read_cpu_usage();
	SDL_Delay(100);

	printf("Dashboard running (%dx%d). Press Escape or Ctrl+C to quit.\n",
	       win_w, win_h);

	/* ---------- render loop ---------- */

	Uint32 last_poll = 0;
	float temperature = has_temp ? read_temperature() : sim_temp;
	float roll = 0.0f, pitch = 0.0f;
	float cpu_pct = 0.0f;

	while (g_running) {
		/* --- events --- */
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			if (ev.type == SDL_QUIT ||
			    (ev.type == SDL_KEYDOWN &&
			     ev.key.keysym.sym == SDLK_ESCAPE)) {
				g_running = 0;
			}
			/* Keyboard fallback for missing sensors */
			if (ev.type == SDL_KEYDOWN) {
				switch (ev.key.keysym.sym) {
				case SDLK_UP:
					if (!has_temp) sim_temp += 0.5f;
					break;
				case SDLK_DOWN:
					if (!has_temp) sim_temp -= 0.5f;
					break;
				case SDLK_LEFT:
					if (!has_imu) sim_roll -= 2.0f;
					break;
				case SDLK_RIGHT:
					if (!has_imu) sim_roll += 2.0f;
					break;
				default:
					break;
				}
			}
		}

		/* --- poll sensors at POLL_HZ --- */
		Uint32 now_ms = SDL_GetTicks();
		if (now_ms - last_poll >= 1000 / POLL_HZ) {
			last_poll = now_ms;

			if (has_temp) {
				float t = read_temperature();
				if (!isnan(t))
					temperature = t;
			} else {
				temperature = sim_temp;
			}

			if (has_imu) {
				float r, p;
				if (read_imu(&r, &p) == 0) {
					roll = r;
					pitch = p;
				}
			} else {
				roll = sim_roll;
				pitch = sim_pitch;
			}

			cpu_pct = read_cpu_usage();
		}

		/* --- render --- */

		/* Background */
		SDL_SetRenderDrawColor(ren, BG_R, BG_G, BG_B, 255);
		SDL_RenderClear(ren);

		/* Layout: three panels with margins */
		int margin = 20;
		int panel_gap = 15;

		/* Temperature gauge (left, square) */
		int temp_size = win_h - 2 * margin;
		int temp_cx = margin + temp_size / 2;
		int temp_cy = margin + temp_size / 2;
		int temp_radius = temp_size / 2 - 30;
		render_temp_gauge(ren, font, font_lg,
				  temp_cx, temp_cy, temp_radius,
				  temperature);

		/* Horizon (center) */
		int hor_x = margin + temp_size + panel_gap;
		int hor_w = win_w - hor_x - margin - 80 - panel_gap;
		int hor_y = margin;
		int hor_h = win_h - 2 * margin;
		render_horizon(ren, font, hor_x, hor_y, hor_w, hor_h,
			       roll, pitch);

		/* CPU bar (right) */
		int cpu_x = win_w - margin - 60;
		int cpu_y = margin + 30;
		int cpu_w = 50;
		int cpu_h = win_h - 2 * margin - 40;
		render_cpu_bar(ren, font, cpu_x, cpu_y, cpu_w, cpu_h,
			       cpu_pct);

		SDL_RenderPresent(ren);   /* blocks until VSync */
	}

	/* ---------- cleanup ---------- */

	TTF_CloseFont(font);
	TTF_CloseFont(font_lg);
	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
	TTF_Quit();
	SDL_Quit();

	printf("Clean shutdown.\n");
	return 0;
}
