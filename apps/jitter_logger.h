/*
 * jitter_logger.h - Shared header for jitter/latency logging
 *
 * Records timestamped samples of sensor timing, frame timing, and
 * end-to-end latency to a CSV file for offline analysis.
 *
 * All functions are static inline so this header can be included
 * directly without a separate compilation unit.
 */

#ifndef JITTER_LOGGER_H
#define JITTER_LOGGER_H

#include <stdio.h>
#include <stdint.h>
#include <time.h>

struct jitter_sample {
	uint64_t timestamp_ns;   /* absolute timestamp (CLOCK_MONOTONIC_RAW) */
	uint64_t sensor_dt_ns;   /* time between sensor reads */
	uint64_t frame_dt_ns;    /* time between frame presentations */
	uint64_t latency_ns;     /* sensor-read to frame-present latency */
	float    roll_deg;       /* current roll angle */
	float    pitch_deg;      /* current pitch angle */
};

/**
 * get_time_ns() - Read CLOCK_MONOTONIC_RAW in nanoseconds.
 *
 * CLOCK_MONOTONIC_RAW is not affected by NTP adjustments, making it
 * ideal for measuring jitter and latency.
 */
static inline uint64_t get_time_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/**
 * jitter_log_open() - Open a CSV file and write the header row.
 * @path: File path for the CSV log.
 *
 * Returns a FILE pointer on success, NULL on failure.
 */
static inline FILE *jitter_log_open(const char *path)
{
	FILE *f = fopen(path, "w");
	if (!f) {
		perror("jitter_log_open");
		return NULL;
	}
	fprintf(f, "timestamp_ns,sensor_dt_ns,frame_dt_ns,latency_ns,"
		   "roll_deg,pitch_deg\n");
	return f;
}

/**
 * jitter_log_write() - Append one sample to the CSV log.
 * @f: FILE pointer from jitter_log_open().
 * @s: Pointer to the sample to write.
 */
static inline void jitter_log_write(FILE *f, const struct jitter_sample *s)
{
	if (!f)
		return;
	fprintf(f, "%lu,%lu,%lu,%lu,%.2f,%.2f\n",
		(unsigned long)s->timestamp_ns,
		(unsigned long)s->sensor_dt_ns,
		(unsigned long)s->frame_dt_ns,
		(unsigned long)s->latency_ns,
		s->roll_deg,
		s->pitch_deg);
}

/**
 * jitter_log_close() - Flush and close the CSV log.
 * @f: FILE pointer from jitter_log_open().
 */
static inline void jitter_log_close(FILE *f)
{
	if (f) {
		fflush(f);
		fclose(f);
	}
}

#endif /* JITTER_LOGGER_H */
