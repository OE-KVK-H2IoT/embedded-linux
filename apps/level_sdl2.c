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
 *   ./level_sdl2 [-v] [-c] [-R preset] [-l logfile.csv] [-d /dev/bmi160] [-r 100]
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
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>

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

/* ---------- axis remap ---------- */

/*
 * Remaps raw (ax, ay, az) to body-frame axes before computing roll/pitch.
 * idx[] selects which raw axis (0=X, 1=Y, 2=Z) maps to each body axis;
 * sign[] flips the sign if needed.
 *
 * Parsed from a string like "XYZ" (default), "Y-XZ", "-X-Y-Z", etc.
 * Each body axis is specified as an optional '-' followed by X, Y, or Z.
 */
struct axis_remap {
	int idx[3];           /* source index for body X, Y, Z */
	int sign[3];          /* +1 or -1 */
};

static int parse_axis_remap(const char *str, struct axis_remap *m)
{
	const char *p = str;

	for (int i = 0; i < 3; i++) {
		int s = +1;
		if (*p == '-') {
			s = -1;
			p++;
		}
		switch (*p) {
		case 'X': case 'x': m->idx[i] = 0; break;
		case 'Y': case 'y': m->idx[i] = 1; break;
		case 'Z': case 'z': m->idx[i] = 2; break;
		default:
			fprintf(stderr,
				"Invalid axis '%c' in remap string \"%s\"\n"
				"Expected format: [−]X[−]Y[−]Z  "
				"e.g. XYZ, Y-XZ, X-Y-Z\n",
				*p ? *p : '?', str);
			return -1;
		}
		m->sign[i] = s;
		p++;
	}

	if (*p != '\0') {
		fprintf(stderr,
			"Remap string \"%s\" too long — expected 3 axes\n",
			str);
		return -1;
	}
	return 0;
}

/* ---------- global state ---------- */

static volatile sig_atomic_t g_running = 1;

/* Atomics for inter-thread data (sensor -> render) */
static _Atomic float g_roll  = 0.0f;
static _Atomic float g_pitch = 0.0f;
static _Atomic float g_yaw   = 0.0f;  /* integrated gyro heading */

/* Sensor timing shared with render thread for logging */
static _Atomic uint64_t g_sensor_ts   = 0;
static _Atomic uint64_t g_sensor_dt   = 0;

/* Calibration: render thread sets g_calibrate=1, sensor thread consumes it */
#define CAL_SAMPLES 50
static _Atomic int   g_calibrate = 0;
static _Atomic float g_cal_off_roll  = 0.0f;
static _Atomic float g_cal_off_pitch = 0.0f;

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
 * SPI helpers for BMI160 register access via /dev/spidev.
 * BMI160 SPI protocol: bit 7 of first byte = 1 for read, 0 for write.
 */

#define BMI160_SPI_SPEED  1000000   /* 1 MHz */
#define BMI160_CHIP_ID    0xD1

static int spi_read_reg(int fd, uint8_t reg, uint8_t *val)
{
	uint8_t tx[2] = { 0x80 | reg, 0 };
	uint8_t rx[2] = { 0 };

	struct spi_ioc_transfer xfer = {
		.tx_buf        = (unsigned long)tx,
		.rx_buf        = (unsigned long)rx,
		.len           = 2,
		.speed_hz      = BMI160_SPI_SPEED,
		.bits_per_word = 8,
	};

	if (ioctl(fd, SPI_IOC_MESSAGE(1), &xfer) < 0)
		return -1;
	*val = rx[1];
	return 0;
}

static int spi_write_reg(int fd, uint8_t reg, uint8_t val)
{
	uint8_t tx[2] = { reg & 0x7F, val };

	struct spi_ioc_transfer xfer = {
		.tx_buf        = (unsigned long)tx,
		.rx_buf        = 0,
		.len           = 2,
		.speed_hz      = BMI160_SPI_SPEED,
		.bits_per_word = 8,
	};

	return ioctl(fd, SPI_IOC_MESSAGE(1), &xfer) < 0 ? -1 : 0;
}

/*
 * Initialise the BMI160 over SPI: verify chip ID, then switch
 * accelerometer and gyroscope from suspend to normal mode.
 * Returns 0 on success, -1 on failure.
 */
static int init_spidev_bmi160(int fd)
{
	uint8_t id = 0;

	/* BMI160 requires a dummy read to activate SPI interface */
	spi_read_reg(fd, 0x00, &id);
	usleep(1000);

	if (spi_read_reg(fd, 0x00, &id) < 0) {
		fprintf(stderr, "BMI160: cannot read chip ID register\n");
		return -1;
	}
	if (id != BMI160_CHIP_ID) {
		fprintf(stderr,
			"BMI160: unexpected chip ID 0x%02X (expected 0x%02X)\n",
			id, BMI160_CHIP_ID);
		return -1;
	}

	/* CMD 0x11 = set accelerometer to normal mode */
	if (spi_write_reg(fd, 0x7E, 0x11) < 0)
		return -1;
	usleep(4000);   /* acc power-up time ~3.8 ms */

	/* CMD 0x15 = set gyroscope to normal mode */
	if (spi_write_reg(fd, 0x7E, 0x15) < 0)
		return -1;
	usleep(80000);  /* gyro power-up time ~80 ms */

	/* GYR_RANGE register 0x43: 0x03 = +/-250 deg/s (131 LSB/deg/s) */
	if (spi_write_reg(fd, 0x43, 0x03) < 0)
		return -1;

	return 0;
}

/*
 * Read gyro + accel data via SPI (fallback when chardev is not available).
 * BMI160 registers 0x0C..0x17: GX GY GZ AX AY AZ (12 bytes, little-endian).
 */
static int read_spidev(int fd, int16_t *ax, int16_t *ay, int16_t *az,
		       int16_t *gx, int16_t *gy, int16_t *gz)
{
	uint8_t tx[13] = { 0x80 | 0x0C };  /* read from 0x0C + 12 dummy */
	uint8_t rx[13] = { 0 };

	struct spi_ioc_transfer xfer = {
		.tx_buf        = (unsigned long)tx,
		.rx_buf        = (unsigned long)rx,
		.len           = sizeof(tx),
		.speed_hz      = BMI160_SPI_SPEED,
		.bits_per_word = 8,
	};

	if (ioctl(fd, SPI_IOC_MESSAGE(1), &xfer) < 0)
		return -1;

	/* rx[0] is dummy; then GX_L GX_H GY_L GY_H GZ_L GZ_H AX_L ... */
	*gx = (int16_t)((rx[2]  << 8) | rx[1]);
	*gy = (int16_t)((rx[4]  << 8) | rx[3]);
	*gz = (int16_t)((rx[6]  << 8) | rx[5]);
	*ax = (int16_t)((rx[8]  << 8) | rx[7]);
	*ay = (int16_t)((rx[10] << 8) | rx[9]);
	*az = (int16_t)((rx[12] << 8) | rx[11]);
	return 0;
}

/* ---------- sensor thread ---------- */

struct sensor_cfg {
	const char *device;
	int         rate_hz;
	int         use_chardev;   /* 1 = /dev/bmi160, 0 = /dev/spidev */
	int         verbose;       /* 1 = print IMU values to stderr */
	const struct axis_remap *remap;
};

static void *sensor_thread(void *arg)
{
	struct sensor_cfg *cfg = (struct sensor_cfg *)arg;
	int fd;
	uint64_t prev_ts, now;
	float roll_raw, pitch_raw;
	float roll_filt = 0.0f, pitch_filt = 0.0f;
	float yaw = 0.0f;
	int first = 1;

	fd = open(cfg->device, cfg->use_chardev ? O_RDONLY : O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Cannot open %s: %s\n",
			cfg->device, strerror(errno));
		fprintf(stderr,
			"Hint: enable SPI with 'dtparam=spi=on' in "
			"/boot/firmware/config.txt\n"
			"      for /dev/bmi160, also load the BMI160 overlay "
			"(see bmi160-spi-driver tutorial)\n");
		g_running = 0;
		return NULL;
	}

	/* For spidev: initialise BMI160 (wake from suspend mode) */
	if (!cfg->use_chardev) {
		if (init_spidev_bmi160(fd) < 0) {
			fprintf(stderr, "BMI160 init failed on %s\n",
				cfg->device);
			close(fd);
			g_running = 0;
			return NULL;
		}
		if (cfg->verbose)
			fprintf(stderr, "BMI160: chip ID OK, sensor active\n");
	}

	unsigned long period_us = 1000000UL / cfg->rate_hz;
	int verbose_counter = 0;
	int verbose_interval = cfg->rate_hz;  /* print once per second */

	/* Calibration state */
	int cal_active = 0, cal_count = 0;
	float cal_roll_sum = 0.0f, cal_pitch_sum = 0.0f;
	float off_roll = 0.0f, off_pitch = 0.0f;

	prev_ts = get_time_ns();

	while (g_running) {
		int16_t ax = 0, ay = 0, az = 0, gx = 0, gy = 0, gz = 0;
		int ret;

		if (cfg->use_chardev)
			ret = read_chardev(fd, &ax, &ay, &az, &gx, &gy, &gz);
		else
			ret = read_spidev(fd, &ax, &ay, &az,
					  &gx, &gy, &gz);

		now = get_time_ns();

		if (ret == 0) {
			/* Apply axis remap to accel and gyro */
			const struct axis_remap *m = cfg->remap;
			int16_t ra[3] = { ax, ay, az };
			ax = (int16_t)(m->sign[0] * ra[m->idx[0]]);
			ay = (int16_t)(m->sign[1] * ra[m->idx[1]]);
			az = (int16_t)(m->sign[2] * ra[m->idx[2]]);
			int16_t rg[3] = { gx, gy, gz };
			gx = (int16_t)(m->sign[0] * rg[m->idx[0]]);
			gy = (int16_t)(m->sign[1] * rg[m->idx[1]]);
			gz = (int16_t)(m->sign[2] * rg[m->idx[2]]);

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

			/* Integrate gyro Z for yaw (heading) */
			float dt = (float)(now - prev_ts) / 1e9f;
			if (dt > 0.0f && dt < 0.5f) {
				yaw += ((float)gz / GYR_SCALE) * dt;
				/* Wrap to 0..360 */
				yaw = fmodf(yaw, 360.0f);
				if (yaw < 0.0f)
					yaw += 360.0f;
			}

			/* Check for calibration request */
			if (atomic_exchange(&g_calibrate, 0)) {
				cal_active = 1;
				cal_count = 0;
				cal_roll_sum = 0.0f;
				cal_pitch_sum = 0.0f;
				if (cfg->verbose)
					fprintf(stderr,
						"Calibrating... hold still\n");
			}

			/* Accumulate calibration samples */
			if (cal_active) {
				cal_roll_sum  += roll_filt;
				cal_pitch_sum += pitch_filt;
				if (++cal_count >= CAL_SAMPLES) {
					off_roll  = cal_roll_sum  / CAL_SAMPLES;
					off_pitch = cal_pitch_sum / CAL_SAMPLES;
					atomic_store(&g_cal_off_roll, off_roll);
					atomic_store(&g_cal_off_pitch, off_pitch);
					cal_active = 0;
					fprintf(stderr,
						"Calibrated: offset roll=%+.1f° "
						"pitch=%+.1f°\n",
						off_roll, off_pitch);
				}
			}

			atomic_store(&g_roll, roll_filt - off_roll);
			atomic_store(&g_pitch, pitch_filt - off_pitch);
			atomic_store(&g_yaw, yaw);
			atomic_store(&g_sensor_dt, now - prev_ts);
			atomic_store(&g_sensor_ts, now);

			if (cfg->verbose && ++verbose_counter >= verbose_interval) {
				fprintf(stderr,
					"IMU: roll=%6.1f° pitch=%6.1f°"
					" yaw=%5.1f°"
					" (ax=%6d ay=%6d az=%6d)\n",
					roll_filt - off_roll,
					pitch_filt - off_pitch,
					yaw, ax, ay, az);
				verbose_counter = 0;
			}
		} else if (cfg->verbose && ++verbose_counter >= verbose_interval) {
			fprintf(stderr, "IMU: read failed (%s)\n",
				strerror(errno));
			verbose_counter = 0;
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
		"Usage: %s [-v] [-c] [-R axes] [-l logfile.csv] "
		"[-d device] [-r rate_hz]\n"
		"\n"
		"  -v  Verbose: print IMU values to stderr (~1 Hz)\n"
		"  -c  Calibrate on startup (set current orientation as zero)\n"
		"  -R  Axis remap for sensor mounting orientation (default: XYZ)\n"
		"      Format: three axes with optional '-' sign prefix\n"
		"      Examples: XYZ (default), Y-XZ (90 CW), -X-YZ (180),\n"
		"                X-Y-Z (chip down), X-ZY (chip forward)\n"
		"  -l  CSV log file for jitter analysis\n"
		"  -d  Sensor device (default: %s, fallback: %s)\n"
		"  -r  Sensor read rate in Hz (default: %d)\n"
		"\n"
		"  Press 'c' during runtime to recalibrate.\n",
		prog, DEFAULT_DEVICE, FALLBACK_DEVICE, DEFAULT_RATE_HZ);
}

int main(int argc, char *argv[])
{
	const char *log_path = NULL;
	const char *device   = NULL;
	int rate_hz          = DEFAULT_RATE_HZ;
	int opt;

	int verbose   = 0;
	int calibrate = 0;
	struct axis_remap remap = { {0,1,2}, {+1,+1,+1} };  /* XYZ */
	const char *remap_str = NULL;

	while ((opt = getopt(argc, argv, "vcR:l:d:r:h")) != -1) {
		switch (opt) {
		case 'v': verbose   = 1;      break;
		case 'c': calibrate = 1;      break;
		case 'R': remap_str = optarg; break;
		case 'l': log_path  = optarg; break;
		case 'd': device    = optarg; break;
		case 'r': rate_hz   = atoi(optarg); break;
		case 'h': /* fall through */
		default:
			print_usage(argv[0]);
			return (opt == 'h') ? 0 : 1;
		}
	}

	if (remap_str && parse_axis_remap(remap_str, &remap) < 0)
		return 1;

	/* Determine sensor device */
	struct sensor_cfg scfg;
	scfg.rate_hz = rate_hz;
	scfg.verbose = verbose;
	scfg.remap   = &remap;

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

	/*
	 * Create the "world" texture: sky on top, ground on bottom,
	 * horizon line in the middle.  Sized to the screen diagonal
	 * so rotation never exposes corners.
	 */
	int diag = (int)ceilf(sqrtf(
		(float)(win_w * win_w + win_h * win_h)));
	diag = (diag + 1) & ~1;  /* round up to even */

	SDL_Texture *world_tex = SDL_CreateTexture(
		ren, SDL_PIXELFORMAT_RGBA8888,
		SDL_TEXTUREACCESS_TARGET, diag, diag);
	if (!world_tex) {
		fprintf(stderr, "SDL_CreateTexture failed: %s\n",
			SDL_GetError());
		SDL_DestroyRenderer(ren);
		SDL_DestroyWindow(win);
		SDL_Quit();
		return 1;
	}

	SDL_SetRenderTarget(ren, world_tex);

	/* Sky (upper half) */
	draw_filled_rect(ren, 0, 0, diag, diag / 2,
			 SKY_R, SKY_G, SKY_B);
	/* Ground (lower half) */
	draw_filled_rect(ren, 0, diag / 2, diag, diag / 2,
			 GND_R, GND_G, GND_B);
	/* Horizon line (3 px thick for visibility) */
	for (int i = -1; i <= 1; i++)
		draw_line(ren, 0, diag / 2 + i, diag, diag / 2 + i,
			  HOR_R, HOR_G, HOR_B);
	/* Pitch reference lines every 10° */
	int pitch_line_half_w = diag / 10;
	for (int deg = -30; deg <= 30; deg += 10) {
		if (deg == 0)
			continue;
		int py = diag / 2 - (int)((float)deg * 3.0f);
		draw_line(ren, diag / 2 - pitch_line_half_w, py,
			  diag / 2 + pitch_line_half_w, py,
			  HOR_R, HOR_G, HOR_B);
	}

	SDL_SetRenderTarget(ren, NULL);

	/* Start sensor thread */
	pthread_t sensor_tid;
	if (pthread_create(&sensor_tid, NULL, sensor_thread, &scfg) != 0) {
		fprintf(stderr, "Failed to create sensor thread\n");
		SDL_DestroyRenderer(ren);
		SDL_DestroyWindow(win);
		SDL_Quit();
		return 1;
	}

	printf("Level display running (%dx%d). Press 'c' to calibrate, "
	       "Escape to quit.\n", win_w, win_h);

	/* Auto-calibrate: let filter settle (~1 s) then trigger */
	if (calibrate) {
		usleep(1000000);
		atomic_store(&g_calibrate, 1);
	}

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
			if (ev.type == SDL_KEYDOWN &&
			    ev.key.keysym.sym == SDLK_c) {
				atomic_store(&g_calibrate, 1);
			}
		}

		float roll  = atomic_load(&g_roll);
		float pitch = atomic_load(&g_pitch);

		/* Clear to black (visible if texture doesn't cover corners) */
		SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
		SDL_RenderClear(ren);

		/*
		 * Draw rotated sky/ground.  The world texture is
		 * centered on screen, shifted down by pitch, and
		 * rotated around the screen center by roll.
		 */
		int pitch_px = (int)(pitch * 3.0f);
		SDL_Rect dst = {
			(win_w - diag) / 2,
			(win_h - diag) / 2 + pitch_px,
			diag, diag
		};
		SDL_Point center = {
			diag / 2,
			diag / 2 - pitch_px
		};
		SDL_RenderCopyEx(ren, world_tex, NULL, &dst,
				 (double)roll, &center, SDL_FLIP_NONE);

		/* Fixed aircraft symbol at screen center */
		int scx = win_w / 2;
		int scy = win_h / 2;
		SDL_SetRenderDrawColor(ren, HOR_R, HOR_G, HOR_B, 255);
		/* Left wing */
		SDL_RenderDrawLine(ren, scx - 40, scy, scx - 10, scy);
		/* Right wing */
		SDL_RenderDrawLine(ren, scx + 10, scy, scx + 40, scy);
		/* Center dot */
		SDL_RenderDrawLine(ren, scx - 2, scy, scx + 2, scy);
		/* Wing drop ticks */
		SDL_RenderDrawLine(ren, scx - 10, scy, scx - 10, scy + 6);
		SDL_RenderDrawLine(ren, scx + 10, scy, scx + 10, scy + 6);

		/*
		 * Compass heading bar at the bottom.  Draws tick marks
		 * that scroll horizontally with yaw; longer ticks every
		 * 30° (N/E/S/W at 0/90/180/270).
		 */
		float yaw_deg = atomic_load(&g_yaw);
		int bar_y = win_h - 30;
		int bar_h = 25;
		int px_per_deg = 4;

		/* Dark background strip */
		SDL_SetRenderDrawColor(ren, 0, 0, 0, 200);
		{
			SDL_Rect bar = { 0, bar_y, win_w, bar_h };
			SDL_RenderFillRect(ren, &bar);
		}

		SDL_SetRenderDrawColor(ren, HOR_R, HOR_G, HOR_B, 255);

		/* Center pointer triangle */
		SDL_RenderDrawLine(ren, scx, bar_y, scx - 4, bar_y + 6);
		SDL_RenderDrawLine(ren, scx, bar_y, scx + 4, bar_y + 6);
		SDL_RenderDrawLine(ren, scx - 4, bar_y + 6,
				   scx + 4, bar_y + 6);

		/* Scrolling tick marks */
		for (int deg = -180; deg <= 540; deg += 10) {
			float rel = (float)deg - yaw_deg;
			int x = scx + (int)(rel * px_per_deg);
			if (x < 0 || x >= win_w)
				continue;

			int norm = ((deg % 360) + 360) % 360;
			int tick_h;

			if (norm % 90 == 0)
				tick_h = bar_h - 4; /* cardinal */
			else if (norm % 30 == 0)
				tick_h = bar_h / 2; /* major */
			else
				tick_h = bar_h / 4; /* minor */

			SDL_RenderDrawLine(ren, x, bar_y + bar_h,
					   x, bar_y + bar_h - tick_h);

			/* Cardinal labels: draw a small letter
			 * using lines (no SDL_ttf dependency) */
			if (norm % 90 == 0) {
				int ly = bar_y + 8;
				int lx = x;
				SDL_SetRenderDrawColor(ren,
					HOR_R, HOR_G, HOR_B, 255);
				switch (norm) {
				case 0:   /* N */
					SDL_RenderDrawLine(ren, lx-3,ly+7,
							   lx-3,ly);
					SDL_RenderDrawLine(ren, lx-3,ly,
							   lx+3,ly+7);
					SDL_RenderDrawLine(ren, lx+3,ly+7,
							   lx+3,ly);
					break;
				case 90:  /* E */
					SDL_RenderDrawLine(ren, lx-3,ly,
							   lx-3,ly+7);
					SDL_RenderDrawLine(ren, lx-3,ly,
							   lx+3,ly);
					SDL_RenderDrawLine(ren, lx-3,ly+3,
							   lx+2,ly+3);
					SDL_RenderDrawLine(ren, lx-3,ly+7,
							   lx+3,ly+7);
					break;
				case 180: /* S */
					SDL_RenderDrawLine(ren, lx+3,ly,
							   lx-3,ly);
					SDL_RenderDrawLine(ren, lx-3,ly,
							   lx-3,ly+3);
					SDL_RenderDrawLine(ren, lx-3,ly+3,
							   lx+3,ly+3);
					SDL_RenderDrawLine(ren, lx+3,ly+3,
							   lx+3,ly+7);
					SDL_RenderDrawLine(ren, lx+3,ly+7,
							   lx-3,ly+7);
					break;
				case 270: /* W */
					SDL_RenderDrawLine(ren, lx-4,ly,
							   lx-2,ly+7);
					SDL_RenderDrawLine(ren, lx-2,ly+7,
							   lx,ly+3);
					SDL_RenderDrawLine(ren, lx,ly+3,
							   lx+2,ly+7);
					SDL_RenderDrawLine(ren, lx+2,ly+7,
							   lx+4,ly);
					break;
				}
			}
		}

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

	SDL_DestroyTexture(world_tex);
	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
	SDL_Quit();

	printf("Clean shutdown.\n");
	return 0;
}
