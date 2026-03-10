/*
 * atomic_sensor.c — C11 atomics demo for inter-thread communication
 *
 * Simulates the sensor→display pattern from level_sdl2.c using only
 * terminal output.  A sensor thread reads CPU temperature and shares
 * it with a display thread via _Atomic variables — no mutex needed.
 *
 * Atomic patterns demonstrated:
 *   _Atomic float   g_temp       — atomic_store / atomic_load
 *   _Atomic int     g_readings   — atomic_fetch_add  (counter)
 *   _Atomic float   g_max_temp   — atomic_compare_exchange_weak (CAS)
 *   _Atomic int     g_calibrate  — atomic_exchange   (one-shot flag)
 *   volatile sig_atomic_t g_running — signal handler  (NOT _Atomic)
 *
 * Build:
 *   gcc -Wall -O2 -pthread -o atomic_sensor atomic_sensor.c -lm
 *
 * Usage:
 *   ./atomic_sensor          # runs until Ctrl-C
 *   Press 'c' + Enter during runtime to trigger a calibration cycle.
 *
 * Copyright (C) 2025 Obuda University — Embedded Systems Lab
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <stdatomic.h>

/* ── Atomic shared state ─────────────────────────────────────────── */

/* Sensor → Display: latest temperature (°C) */
static _Atomic float g_temp = 0.0f;

/* Sensor: total number of readings taken */
static _Atomic int g_readings = 0;

/* Sensor: highest temperature seen (updated via CAS loop) */
static _Atomic float g_max_temp = 0.0f;

/* Main → Sensor: one-shot calibration flag (consumed via exchange) */
static _Atomic int g_calibrate = 0;

/* Signal handler → all threads: shutdown flag.
 * Deliberately NOT _Atomic — signal handlers require
 * volatile sig_atomic_t, which is async-signal-safe. */
static volatile sig_atomic_t g_running = 1;

/* Calibration offset (set by sensor thread during calibration) */
static _Atomic float g_cal_offset = 0.0f;

/* ── Signal handler ──────────────────────────────────────────────── */

static void handle_signal(int sig)
{
	(void)sig;
	g_running = 0;   /* only sig_atomic_t is safe here */
}

/* ── Helper: read CPU temperature ────────────────────────────────── */

static float read_cpu_temp(void)
{
	FILE *f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
	if (!f)
		return -1.0f;

	int millideg;
	if (fscanf(f, "%d", &millideg) != 1) {
		fclose(f);
		return -1.0f;
	}
	fclose(f);
	return (float)millideg / 1000.0f;
}

/* ── Sensor thread ───────────────────────────────────────────────── */

static void *sensor_thread(void *arg)
{
	(void)arg;
	float cal_sum = 0.0f;
	int   cal_count = 0;
	int   cal_active = 0;

	while (g_running) {
		float temp = read_cpu_temp();
		if (temp < 0.0f) {
			usleep(100000);
			continue;
		}

		/* Apply calibration offset */
		float offset = atomic_load(&g_cal_offset);
		temp -= offset;

		/* Pattern 1: atomic_store — publish temperature */
		atomic_store(&g_temp, temp);

		/* Pattern 2: atomic_fetch_add — increment counter */
		atomic_fetch_add(&g_readings, 1);

		/* Pattern 3: atomic_compare_exchange_weak — update max
		 * This is a lock-free CAS loop: read current max, try to
		 * replace it if our value is higher.  If another thread
		 * changed it between read and write, the loop retries. */
		float cur_max = atomic_load(&g_max_temp);
		while (temp > cur_max) {
			if (atomic_compare_exchange_weak(&g_max_temp,
							&cur_max, temp))
				break;
			/* cur_max is updated by CAS on failure */
		}

		/* Pattern 4: atomic_exchange — consume calibration flag
		 * Exchange returns the old value and sets the new value
		 * in a single atomic step.  If old value was 1, we got
		 * the flag; if 0, there was no request. */
		if (atomic_exchange(&g_calibrate, 0)) {
			cal_active = 1;
			cal_count = 0;
			cal_sum = 0.0f;
			fprintf(stderr,
				"[sensor] Calibrating... hold conditions stable\n");
		}

		/* Accumulate calibration samples */
		if (cal_active) {
			cal_sum += temp + offset;  /* use raw temp */
			if (++cal_count >= 10) {
				float new_offset = cal_sum / 10.0f;
				atomic_store(&g_cal_offset, new_offset);
				cal_active = 0;
				fprintf(stderr,
					"[sensor] Calibrated: offset = %.2f°C\n",
					new_offset);
			}
		}

		usleep(100000);  /* 100 ms → ~10 Hz */
	}

	return NULL;
}

/* ── Display thread ──────────────────────────────────────────────── */

static void *display_thread(void *arg)
{
	(void)arg;

	while (g_running) {
		/* Read shared atomics (no lock needed) */
		float temp     = atomic_load(&g_temp);
		float max_temp = atomic_load(&g_max_temp);
		int   readings = atomic_load(&g_readings);

		/* Build a simple bar: each '#' = 1°C */
		int bar_len = (int)temp;
		if (bar_len < 0)  bar_len = 0;
		if (bar_len > 60) bar_len = 60;

		char bar[61];
		memset(bar, '#', bar_len);
		bar[bar_len] = '\0';

		printf("\r\033[K"   /* clear line */
		       "Temp: %5.1f°C  Max: %5.1f°C  "
		       "Readings: %d  [%s]",
		       temp, max_temp, readings, bar);
		fflush(stdout);

		usleep(500000);  /* 500 ms → 2 Hz display */
	}

	printf("\n");
	return NULL;
}

/* ── Main ────────────────────────────────────────────────────────── */

int main(void)
{
	/* Install signal handlers */
	struct sigaction sa = { .sa_handler = handle_signal };
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	printf("atomic_sensor — C11 atomics demo\n"
	       "Press 'c' + Enter to calibrate, Ctrl-C to quit.\n\n");

	/* Read initial temperature to seed max */
	float t0 = read_cpu_temp();
	if (t0 > 0.0f)
		atomic_store(&g_max_temp, t0);

	/* Launch threads */
	pthread_t sensor_tid, display_tid;
	pthread_create(&sensor_tid, NULL, sensor_thread, NULL);
	pthread_create(&display_tid, NULL, display_thread, NULL);

	/* Main thread: read stdin for 'c' key */
	while (g_running) {
		int ch = getchar();
		if (ch == 'c' || ch == 'C')
			atomic_store(&g_calibrate, 1);
		if (ch == EOF)
			break;
	}

	/* Shutdown */
	g_running = 0;
	pthread_join(sensor_tid, NULL);
	pthread_join(display_tid, NULL);

	printf("\nFinal stats: %d readings, max temperature: %.1f°C\n",
	       atomic_load(&g_readings), atomic_load(&g_max_temp));

	return 0;
}
