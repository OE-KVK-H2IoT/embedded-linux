/*
 * level_sdl2.c - SDL2 + KMSDRM level display for BMI160 IMU
 *
 * Reads accelerometer data from a BMI160 (via /dev/bmi160 character device
 * or fallback /dev/spidev0.0) and renders an artificial-horizon display
 * using SDL2.  A dedicated sensor thread decouples I/O from rendering.
 *
 * Build:
 *   gcc -Wall -O2 $(sdl2-config --cflags) -o level_sdl2 level_sdl2.c \
 *       $(sdl2-config --libs) -lm -lpthread
 *
 * Usage:
 *   ./level_sdl2 [-l logfile.csv] [-d /dev/bmi160] [-r 100]
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
#include <pthread.h>
#include <stdatomic.h>

#include <SDL2/SDL.h>

#include "jitter_logger.h"

/* ---------- configuration ---------- */

#define DEFAULT_DEVICE   "/dev/bmi160"
#define FALLBACK_DEVICE  "/dev/spidev0.0"
#define DEFAULT_RATE_HZ  100
#define WINDOW_W         640
#define WINDOW_H         480

#define ACC_SCALE  16384.0f   /* LSB/g for +/-2g */
#define GYR_SCALE    131.0f   /* LSB/(deg/s) for +/-250 deg/s */

/* Low-pass filter coefficient: filtered = (1-ALPHA)*prev + ALPHA*raw */
#define LP_ALPHA  0.1f

/* Colours (RGBA) */
#define SKY_R    50
#define SKY_G   150
#define SKY_B   200
#define GND_R   180
#define GND_G   100
#define GND_B    50
#define HOR_R   255
#define HOR_G   255
#define HOR_B   255

/* ---------- global state ---------- */

static volatile sig_atomic_t g_running = 1;

/* Atomics for inter-thread data (sensor -> render) */
static _Atomic float g_roll  = 0.0f;
static _Atomic float g_pitch = 0.0f;

/* Sensor timing shared with render thread for logging */
static _Atomic uint64_t g_sensor_ts   = 0;
static _Atomic uint64_t g_sensor_dt   = 0;

/* ---------- signal handler ---------- */

static void handle_signal(int sig)
{
	(void)sig;
	g_running = 0;
}

/* ---------- sensor I/O ---------- */

/*
 * Read from the /dev/bmi160 character device.
 * Expected format: "AX AY AZ GX GY GZ\n"
 */
static int read_chardev(int fd, int16_t *ax, int16_t *ay, int16_t *az,
			int16_t *gx, int16_t *gy, int16_t *gz)
{
	char buf[128];
	int n;

	/* Seek to beginning for re-reads */
	lseek(fd, 0, SEEK_SET);

	n = read(fd, buf, sizeof(buf) - 1);
	if (n <= 0)
		return -1;
	buf[n] = '\0';

	if (sscanf(buf, "%hd %hd %hd %hd %hd %hd",
		   ax, ay, az, gx, gy, gz) != 6)
		return -1;

	return 0;
}

/*
 * Read via raw SPI (fallback when chardev is not available).
 * Uses Linux's /dev/spidev ioctl interface.
 */
static int read_spidev(int fd, int16_t *ax, int16_t *ay, int16_t *az)
{
	/*
	 * Simplified: send register 0x92 (0x80|0x12) + 6 dummy bytes,
	 * receive 7 bytes total (first is dummy).
	 *
	 * For a real SPI userspace implementation we would use
	 * ioctl(SPI_IOC_MESSAGE).  Here we show the concept.
	 */
	uint8_t tx[7] = { 0x80 | 0x12, 0, 0, 0, 0, 0, 0 };
	uint8_t rx[7] = { 0 };

	lseek(fd, 0, SEEK_SET);
	/* This is a simplification; real code would use SPI_IOC_MESSAGE */
	if (write(fd, tx, sizeof(tx)) < 0)
		return -1;
	lseek(fd, 0, SEEK_SET);
	if (read(fd, rx, sizeof(rx)) < 0)
		return -1;

	*ax = (int16_t)((rx[2] << 8) | rx[1]);
	*ay = (int16_t)((rx[4] << 8) | rx[3]);
	*az = (int16_t)((rx[6] << 8) | rx[5]);
	return 0;
}

/* ---------- sensor thread ---------- */

struct sensor_cfg {
	const char *device;
	int         rate_hz;
	int         use_chardev;   /* 1 = /dev/bmi160, 0 = /dev/spidev */
};

static void *sensor_thread(void *arg)
{
	struct sensor_cfg *cfg = (struct sensor_cfg *)arg;
	int fd;
	uint64_t prev_ts, now;
	float roll_raw, pitch_raw;
	float roll_filt = 0.0f, pitch_filt = 0.0f;
	int first = 1;

	fd = open(cfg->device, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "Cannot open %s: %s\n",
			cfg->device, strerror(errno));
		g_running = 0;
		return NULL;
	}

	unsigned long period_us = 1000000UL / cfg->rate_hz;
	prev_ts = get_time_ns();

	while (g_running) {
		int16_t ax = 0, ay = 0, az = 0, gx = 0, gy = 0, gz = 0;
		int ret;

		if (cfg->use_chardev)
			ret = read_chardev(fd, &ax, &ay, &az, &gx, &gy, &gz);
		else
			ret = read_spidev(fd, &ax, &ay, &az);

		now = get_time_ns();

		if (ret == 0) {
			float fax = (float)ax / ACC_SCALE;
			float fay = (float)ay / ACC_SCALE;
			float faz = (float)az / ACC_SCALE;

			roll_raw = atan2f(fay, faz) * (180.0f / (float)M_PI);
			pitch_raw = atan2f(-fax, sqrtf(fay * fay + faz * faz))
				    * (180.0f / (float)M_PI);

			if (first) {
				roll_filt = roll_raw;
				pitch_filt = pitch_raw;
				first = 0;
			} else {
				roll_filt = (1.0f - LP_ALPHA) * roll_filt
					    + LP_ALPHA * roll_raw;
				pitch_filt = (1.0f - LP_ALPHA) * pitch_filt
					     + LP_ALPHA * pitch_raw;
			}

			atomic_store(&g_roll, roll_filt);
			atomic_store(&g_pitch, pitch_filt);
			atomic_store(&g_sensor_dt, now - prev_ts);
			atomic_store(&g_sensor_ts, now);
		}

		prev_ts = now;
		usleep(period_us);
	}

	close(fd);
	return NULL;
}

/* ---------- rendering helpers ---------- */

static void draw_filled_rect(SDL_Renderer *r, int x, int y, int w, int h,
			      uint8_t red, uint8_t green, uint8_t blue)
{
	SDL_Rect rect = { x, y, w, h };
	SDL_SetRenderDrawColor(r, red, green, blue, 255);
	SDL_RenderFillRect(r, &rect);
}

static void draw_line(SDL_Renderer *r, int x1, int y1, int x2, int y2,
		       uint8_t red, uint8_t green, uint8_t blue)
{
	SDL_SetRenderDrawColor(r, red, green, blue, 255);
	SDL_RenderDrawLine(r, x1, y1, x2, y2);
}

/* ---------- main ---------- */

static void print_usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [-l logfile.csv] [-d device] [-r rate_hz]\n"
		"\n"
		"  -l  CSV log file for jitter analysis\n"
		"  -d  Sensor device (default: %s, fallback: %s)\n"
		"  -r  Sensor read rate in Hz (default: %d)\n",
		prog, DEFAULT_DEVICE, FALLBACK_DEVICE, DEFAULT_RATE_HZ);
}

int main(int argc, char *argv[])
{
	const char *log_path = NULL;
	const char *device   = NULL;
	int rate_hz          = DEFAULT_RATE_HZ;
	int opt;

	while ((opt = getopt(argc, argv, "l:d:r:h")) != -1) {
		switch (opt) {
		case 'l': log_path = optarg; break;
		case 'd': device   = optarg; break;
		case 'r': rate_hz  = atoi(optarg); break;
		case 'h': /* fall through */
		default:
			print_usage(argv[0]);
			return (opt == 'h') ? 0 : 1;
		}
	}

	/* Determine sensor device */
	struct sensor_cfg scfg;
	scfg.rate_hz = rate_hz;

	if (device) {
		scfg.device = device;
		scfg.use_chardev = (strstr(device, "bmi160") != NULL);
	} else {
		/* Try chardev first, fall back to spidev */
		if (access(DEFAULT_DEVICE, R_OK) == 0) {
			scfg.device = DEFAULT_DEVICE;
			scfg.use_chardev = 1;
		} else {
			fprintf(stderr, "%s not found, falling back to %s\n",
				DEFAULT_DEVICE, FALLBACK_DEVICE);
			scfg.device = FALLBACK_DEVICE;
			scfg.use_chardev = 0;
		}
	}

	/* Install signal handlers */
	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	/* Open jitter log */
	FILE *jlog = NULL;
	if (log_path) {
		jlog = jitter_log_open(log_path);
		if (!jlog) {
			fprintf(stderr, "Warning: cannot open log %s\n",
				log_path);
		}
	}

	/* Initialise SDL2 */
	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
		return 1;
	}

	SDL_Window *win = SDL_CreateWindow(
		"IMU Level Display",
		SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		WINDOW_W, WINDOW_H,
		SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_SHOWN);
	if (!win) {
		fprintf(stderr, "SDL_CreateWindow failed: %s\n",
			SDL_GetError());
		SDL_Quit();
		return 1;
	}

	SDL_Renderer *ren = SDL_CreateRenderer(
		win, -1,
		SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (!ren) {
		fprintf(stderr, "SDL_CreateRenderer failed: %s\n",
			SDL_GetError());
		SDL_DestroyWindow(win);
		SDL_Quit();
		return 1;
	}

	/* Get actual window size (may differ from requested in fullscreen) */
	int win_w, win_h;
	SDL_GetWindowSize(win, &win_w, &win_h);

	/* Start sensor thread */
	pthread_t sensor_tid;
	if (pthread_create(&sensor_tid, NULL, sensor_thread, &scfg) != 0) {
		fprintf(stderr, "Failed to create sensor thread\n");
		SDL_DestroyRenderer(ren);
		SDL_DestroyWindow(win);
		SDL_Quit();
		return 1;
	}

	printf("Level display running (%dx%d). Press Ctrl+C or close window.\n",
	       win_w, win_h);

	/* ---------- render loop ---------- */

	uint64_t prev_frame_ts = get_time_ns();

	while (g_running) {
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			if (ev.type == SDL_QUIT ||
			    (ev.type == SDL_KEYDOWN &&
			     ev.key.keysym.sym == SDLK_ESCAPE)) {
				g_running = 0;
			}
		}

		float roll  = atomic_load(&g_roll);
		float pitch = atomic_load(&g_pitch);

		/* Horizon vertical position: pitch shifts it */
		int horizon_y = win_h / 2 + (int)(pitch * 3.0f);

		/* Clear and draw sky/ground */
		if (horizon_y > 0 && horizon_y < win_h) {
			draw_filled_rect(ren, 0, 0, win_w, horizon_y,
					 SKY_R, SKY_G, SKY_B);
			draw_filled_rect(ren, 0, horizon_y, win_w,
					 win_h - horizon_y,
					 GND_R, GND_G, GND_B);
		} else if (horizon_y <= 0) {
			draw_filled_rect(ren, 0, 0, win_w, win_h,
					 GND_R, GND_G, GND_B);
		} else {
			draw_filled_rect(ren, 0, 0, win_w, win_h,
					 SKY_R, SKY_G, SKY_B);
		}

		/* Draw rotated horizon line */
		float roll_rad = roll * (float)M_PI / 180.0f;
		int half_len = (int)(win_w * 0.7f);
		int cx = win_w / 2;
		int cy = horizon_y;
		int dx = (int)(half_len * cosf(roll_rad));
		int dy = (int)(half_len * sinf(roll_rad));

		draw_line(ren, cx - dx, cy + dy, cx + dx, cy - dy,
			  HOR_R, HOR_G, HOR_B);

		/* Center crosshair (fixed at screen center) */
		int scx = win_w / 2;
		int scy = win_h / 2;
		draw_line(ren, scx - 15, scy, scx + 15, scy,
			  HOR_R, HOR_G, HOR_B);
		draw_line(ren, scx, scy - 15, scx, scy + 15,
			  HOR_R, HOR_G, HOR_B);

		SDL_RenderPresent(ren);

		/* Jitter logging */
		uint64_t now = get_time_ns();
		uint64_t frame_dt = now - prev_frame_ts;
		prev_frame_ts = now;

		if (jlog) {
			struct jitter_sample s;
			s.timestamp_ns = now;
			s.sensor_dt_ns = atomic_load(&g_sensor_dt);
			s.frame_dt_ns  = frame_dt;
			/* latency = time since last sensor read */
			uint64_t sts = atomic_load(&g_sensor_ts);
			s.latency_ns = (now > sts) ? (now - sts) : 0;
			s.roll_deg   = roll;
			s.pitch_deg  = pitch;
			jitter_log_write(jlog, &s);
		}
	}

	/* ---------- cleanup ---------- */

	pthread_join(sensor_tid, NULL);

	if (jlog) {
		jitter_log_close(jlog);
		printf("Jitter log saved to %s\n", log_path);
	}

	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
	SDL_Quit();

	printf("Clean shutdown.\n");
	return 0;
}
