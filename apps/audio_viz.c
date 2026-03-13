/*
 * audio_viz.c — I2S Microphone Audio Visualizer for Raspberry Pi
 *
 * Captures audio from ALSA, displays waveform + FFT spectrum + scrolling
 * spectrogram using SDL2.  Optionally captures 2 channels and shows
 * cross-correlation / GCC-PHAT delay estimate for direction detection.
 *
 * Dependencies: ALSA, SDL2, FFTW3 (or compile with -DUSE_KISSFFT)
 *
 * Build:  gcc -Wall -O2 $(sdl2-config --cflags) -o audio_viz audio_viz.c \
 *           $(sdl2-config --libs) -lasound -lfftw3f -lm -lpthread
 *
 * Run:    ./audio_viz                 # default: hw:0, mono, 48 kHz
 *         ./audio_viz -d hw:1 -c 2    # device hw:1, stereo
 *         ./audio_viz -r 16000        # 16 kHz sample rate
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <getopt.h>

#include <alsa/asoundlib.h>
#include <SDL2/SDL.h>
#include <fftw3.h>

/* ── Defaults ───────────────────────────────────────────────────────── */

#define DEFAULT_DEVICE    "hw:0"
#define DEFAULT_RATE      48000
#define DEFAULT_CHANNELS  1
#define DEFAULT_FRAMES    1024      /* ALSA period size = FFT size */
#define RING_SLOTS        4         /* ring buffer depth (periods) */

#define WIN_W             800
#define WIN_H             480
#define SPEC_HISTORY      256       /* spectrogram scroll columns */

#define SPEED_OF_SOUND    343.0f    /* m/s */
#define DEFAULT_MIC_DIST  0.06f     /* 6 cm default mic spacing */
#define DEFAULT_GAIN      4.0f      /* software gain (I2S mics are often quiet) */
#define DEFAULT_WAVE_MS   50        /* waveform display length in ms */
#define MAX_WAVE_SAMPLES  (48000*2) /* max 2 seconds of waveform history */

/* ── Globals ────────────────────────────────────────────────────────── */

static volatile int running = 1;

/* Audio config */
static const char *alsa_device = DEFAULT_DEVICE;
static unsigned int sample_rate = DEFAULT_RATE;
static unsigned int channels = DEFAULT_CHANNELS;
static snd_pcm_uframes_t period_frames = DEFAULT_FRAMES;
static float mic_distance = DEFAULT_MIC_DIST;
static float gain = DEFAULT_GAIN;
static int wave_ms = DEFAULT_WAVE_MS;
static int flip_dir = 0;          /* -f: flip direction 180° */

/* Ring buffer: audio thread writes, render thread reads */
static float *ring_buf;           /* [RING_SLOTS][channels * period_frames] */
static struct timespec ring_ts[RING_SLOTS]; /* capture timestamp per slot */
static int ring_write = 0;
static int ring_read  = 0;
static pthread_mutex_t ring_mtx = PTHREAD_MUTEX_INITIALIZER;

/* ── DSP helpers ────────────────────────────────────────────────────── */

static void apply_hann_window(const float *in, float *out, int n)
{
	for (int i = 0; i < n; i++)
		out[i] = in[i] * 0.5f * (1.0f - cosf(2.0f * M_PI * i / (n - 1)));
}

/* Simple 1-pole high-pass: y[n] = alpha * (y[n-1] + x[n] - x[n-1]) */
static void highpass_1pole(float *buf, int n, float *y_prev, float *x_prev,
			   float alpha)
{
	for (int i = 0; i < n; i++) {
		float x = buf[i];
		float y = alpha * (*y_prev + x - *x_prev);
		*x_prev = x;
		*y_prev = y;
		buf[i] = y;
	}
}

/* Compute magnitude spectrum (half-spectrum, dB scale) */
static void compute_magnitude_db(fftwf_complex *fft_out, float *mag_db,
				 int fft_size)
{
	int half = fft_size / 2 + 1;
	for (int i = 0; i < half; i++) {
		float re = fft_out[i][0];
		float im = fft_out[i][1];
		float mag = sqrtf(re * re + im * im) / fft_size;
		mag_db[i] = 20.0f * log10f(mag + 1e-10f);
	}
}

/* Compute RMS level */
static float compute_rms(const float *buf, int n)
{
	float sum = 0;
	for (int i = 0; i < n; i++) sum += buf[i] * buf[i];
	return sqrtf(sum / n);
}

/* Compute peak level */
static float compute_peak(const float *buf, int n)
{
	float peak = 0;
	for (int i = 0; i < n; i++) {
		float a = fabsf(buf[i]);
		if (a > peak) peak = a;
	}
	return peak;
}

/* ── GCC-PHAT for 2-channel delay estimation ────────────────────────── */

static void gcc_phat(const float *ch1, const float *ch2, float *corr,
		     int n, fftwf_plan fwd1, fftwf_plan fwd2,
		     fftwf_plan inv, float *in1, float *in2,
		     fftwf_complex *out1, fftwf_complex *out2,
		     fftwf_complex *cross, float *inv_out)
{
	/* Window and copy */
	apply_hann_window(ch1, in1, n);
	apply_hann_window(ch2, in2, n);

	/* Forward FFT */
	fftwf_execute(fwd1);
	fftwf_execute(fwd2);

	/* Cross-power spectrum with phase transform */
	int half = n / 2 + 1;
	for (int i = 0; i < half; i++) {
		float re1 = out1[i][0], im1 = out1[i][1];
		float re2 = out2[i][0], im2 = out2[i][1];

		/* G12 = X1 * conj(X2) */
		float cre = re1 * re2 + im1 * im2;
		float cim = im1 * re2 - re1 * im2;

		/* PHAT weighting: normalize by magnitude */
		float mag = sqrtf(cre * cre + cim * cim) + 1e-10f;
		cross[i][0] = cre / mag;
		cross[i][1] = cim / mag;
	}

	/* Inverse FFT */
	fftwf_execute(inv);

	/* Normalize and copy to output */
	for (int i = 0; i < n; i++)
		corr[i] = inv_out[i] / n;
}

static int find_peak_lag(const float *corr, int n, int max_lag)
{
	float best = -1e30f;
	int best_i = 0;
	for (int i = 0; i < max_lag; i++) {
		if (corr[i] > best) { best = corr[i]; best_i = i; }
	}
	for (int i = n - max_lag; i < n; i++) {
		if (corr[i] > best) { best = corr[i]; best_i = i; }
	}
	/* Convert to signed lag */
	if (best_i > n / 2) best_i -= n;
	return best_i;
}

/* ── Audio capture thread ───────────────────────────────────────────── */

static void *audio_thread(void *arg)
{
	(void)arg;
	snd_pcm_t *pcm = NULL;
	snd_pcm_hw_params_t *hw;
	int err;

	if ((err = snd_pcm_open(&pcm, alsa_device, SND_PCM_STREAM_CAPTURE, 0)) < 0) {
		fprintf(stderr, "ALSA open '%s': %s\n", alsa_device, snd_strerror(err));
		running = 0;
		return NULL;
	}

	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_any(pcm, hw);
	snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
	snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S32_LE);
	snd_pcm_hw_params_set_channels(pcm, hw, channels);
	snd_pcm_hw_params_set_rate_near(pcm, hw, &sample_rate, NULL);
	snd_pcm_hw_params_set_period_size_near(pcm, hw, &period_frames, NULL);

	if ((err = snd_pcm_hw_params(pcm, hw)) < 0) {
		fprintf(stderr, "ALSA hw_params: %s\n", snd_strerror(err));
		snd_pcm_close(pcm);
		running = 0;
		return NULL;
	}

	/* Query actual values after negotiation */
	snd_pcm_hw_params_get_rate(hw, &sample_rate, NULL);
	snd_pcm_hw_params_get_period_size(hw, &period_frames, NULL);
	printf("ALSA: %s, %u Hz, %u ch, period %lu frames (S32_LE)\n",
	       alsa_device, sample_rate, channels, period_frames);

	int slot_size = channels * period_frames;
	int32_t *raw_buf = malloc(slot_size * sizeof(int32_t));
	float *tmp = malloc(slot_size * sizeof(float));

	while (running) {
		snd_pcm_sframes_t n = snd_pcm_readi(pcm, raw_buf, period_frames);
		if (n < 0) {
			n = snd_pcm_recover(pcm, (int)n, 0);
			if (n < 0) {
				fprintf(stderr, "ALSA read error: %s\n", snd_strerror((int)n));
				break;
			}
			continue;
		}

		/* Convert S32_LE to float and apply gain.
		 * I2S mics output 24-bit data in upper bits of 32-bit word,
		 * so the raw signal is often quiet — gain compensates. */
		for (int i = 0; i < (int)(n * channels); i++)
			tmp[i] = (float)raw_buf[i] / 2147483648.0f * gain;

		pthread_mutex_lock(&ring_mtx);
		clock_gettime(CLOCK_MONOTONIC, &ring_ts[ring_write]);
		memcpy(ring_buf + ring_write * slot_size, tmp,
		       n * channels * sizeof(float));
		ring_write = (ring_write + 1) % RING_SLOTS;
		if (ring_write == ring_read)
			ring_read = (ring_read + 1) % RING_SLOTS; /* drop oldest */
		pthread_mutex_unlock(&ring_mtx);
	}

	free(raw_buf);
	free(tmp);
	snd_pcm_close(pcm);
	return NULL;
}

/* ── Forward declarations ───────────────────────────────────────────── */

static void draw_text(SDL_Renderer *r, const char *str,
		      int x0, int y0, int scale);

/* ── Drawing helpers ────────────────────────────────────────────────── */

static void draw_waveform(SDL_Renderer *r, const float *buf, int n,
			  int x0, int y0, int w, int h,
			  Uint8 cr, Uint8 cg, Uint8 cb)
{
	int mid = y0 + h / 2;
	int ytop = y0 + 1;
	int ybot = y0 + h - 1;

	/* Border */
	SDL_SetRenderDrawColor(r, 60, 60, 60, 255);
	SDL_Rect border = { x0, y0, w, h };
	SDL_RenderDrawRect(r, &border);

	/* Zero line */
	SDL_SetRenderDrawColor(r, 40, 40, 40, 255);
	SDL_RenderDrawLine(r, x0, mid, x0 + w, mid);

	/* Waveform — for each pixel column, find min/max sample in that
	 * range and draw a vertical line between them. This produces a
	 * filled waveform that doesn't miss peaks even when zoomed out. */
	SDL_SetRenderDrawColor(r, cr, cg, cb, 255);

	int prev_y = mid;
	for (int i = 0; i < w; i++) {
		int s0 = i * n / w;
		int s1 = (i + 1) * n / w;
		if (s1 <= s0) s1 = s0 + 1;
		if (s1 > n) s1 = n;

		float vmin = buf[s0], vmax = buf[s0];
		for (int s = s0 + 1; s < s1; s++) {
			if (buf[s] < vmin) vmin = buf[s];
			if (buf[s] > vmax) vmax = buf[s];
		}

		/* Clamp to drawing area */
		int py_min = mid - (int)(vmax * h / 2);
		int py_max = mid - (int)(vmin * h / 2);
		if (py_min < ytop) py_min = ytop;
		if (py_max > ybot) py_max = ybot;
		if (py_min > ybot) py_min = ybot;
		if (py_max < ytop) py_max = ytop;

		/* Connect to previous column for continuity */
		if (i > 0) {
			if (prev_y < py_min)
				SDL_RenderDrawLine(r, x0 + i, prev_y, x0 + i, py_min);
			else if (prev_y > py_max)
				SDL_RenderDrawLine(r, x0 + i, py_max, x0 + i, prev_y);
		}

		/* Draw min-max span */
		SDL_RenderDrawLine(r, x0 + i, py_min, x0 + i, py_max);
		prev_y = mid - (int)(buf[s1 - 1] * h / 2);
		if (prev_y < ytop) prev_y = ytop;
		if (prev_y > ybot) prev_y = ybot;
	}
}

static void draw_spectrum(SDL_Renderer *r, const float *mag_db, int bins,
			  int x0, int y0, int w, int h,
			  float db_floor, float db_ceil,
			  unsigned int srate, int fft_n)
{
	/* Border */
	SDL_SetRenderDrawColor(r, 60, 60, 60, 255);
	SDL_Rect border = { x0, y0, w, h };
	SDL_RenderDrawRect(r, &border);

	float db_range = db_ceil - db_floor;
	float nyquist = srate / 2.0f;

	for (int i = 0; i < w; i++) {
		int bin = i * (bins - 1) / w;
		float norm = (mag_db[bin] - db_floor) / db_range;
		if (norm < 0) norm = 0;
		if (norm > 1) norm = 1;
		int bar_h = (int)(norm * h);

		/* Color: blue → green → yellow → red */
		Uint8 cr, cg, cb;
		if (norm < 0.5f) {
			cr = 0;
			cg = (Uint8)(norm * 2 * 255);
			cb = (Uint8)((1 - norm * 2) * 255);
		} else {
			cr = (Uint8)((norm - 0.5f) * 2 * 255);
			cg = (Uint8)((1 - (norm - 0.5f) * 2) * 255);
			cb = 0;
		}

		SDL_SetRenderDrawColor(r, cr, cg, cb, 255);
		SDL_RenderDrawLine(r, x0 + i, y0 + h - bar_h, x0 + i, y0 + h);
	}

	/* Frequency axis labels */
	float freq_ticks[] = {100, 500, 1000, 2000, 5000, 10000, 20000};
	int n_ticks = sizeof(freq_ticks) / sizeof(freq_ticks[0]);
	for (int t = 0; t < n_ticks; t++) {
		if (freq_ticks[t] > nyquist) break;
		int px = (int)(freq_ticks[t] / nyquist * w);
		if (px < 0 || px >= w) continue;

		/* Tick mark */
		SDL_SetRenderDrawColor(r, 80, 80, 80, 255);
		SDL_RenderDrawLine(r, x0 + px, y0, x0 + px, y0 + h);

		/* Label */
		char lbl[16];
		if (freq_ticks[t] >= 1000)
			snprintf(lbl, sizeof(lbl), "%dk", (int)(freq_ticks[t] / 1000));
		else
			snprintf(lbl, sizeof(lbl), "%d", (int)freq_ticks[t]);

		SDL_SetRenderDrawColor(r, 120, 130, 140, 255);
		draw_text(r, lbl, x0 + px + 2, y0 + 2, 1);
	}
}

static void draw_spectrogram_col(SDL_Texture *tex, const float *mag_db,
				 int bins, int col, int tex_h,
				 float db_floor, float db_ceil)
{
	float db_range = db_ceil - db_floor;
	Uint32 pixels[512]; /* max tex_h */
	int draw_h = tex_h < 512 ? tex_h : 512;

	for (int row = 0; row < draw_h; row++) {
		int bin = (draw_h - 1 - row) * bins / draw_h;
		float norm = (mag_db[bin] - db_floor) / db_range;
		if (norm < 0) norm = 0;
		if (norm > 1) norm = 1;

		/* Colormap: black → blue → cyan → yellow → white */
		Uint8 r, g, b;
		if (norm < 0.25f) {
			r = 0; g = 0; b = (Uint8)(norm * 4 * 255);
		} else if (norm < 0.5f) {
			r = 0; g = (Uint8)((norm - 0.25f) * 4 * 255); b = 255;
		} else if (norm < 0.75f) {
			r = (Uint8)((norm - 0.5f) * 4 * 255);
			g = 255; b = (Uint8)((1 - (norm - 0.5f) * 4) * 255);
		} else {
			r = 255; g = 255; b = (Uint8)((norm - 0.75f) * 4 * 255);
		}
		pixels[row] = (255u << 24) | (r << 16) | (g << 8) | b;
	}

	SDL_Rect dst = { col, 0, 1, draw_h };
	SDL_UpdateTexture(tex, &dst, pixels, sizeof(Uint32));
}

static void draw_level_bar(SDL_Renderer *r, const char *label,
			   float rms, float peak,
			   int x0, int y0, int w, int h)
{
	SDL_SetRenderDrawColor(r, 60, 60, 60, 255);
	SDL_Rect border = { x0, y0, w, h };
	SDL_RenderDrawRect(r, &border);

	/* RMS bar (green) */
	float rms_db = 20.0f * log10f(rms + 1e-10f);
	float norm = (rms_db + 60) / 60;
	if (norm < 0) norm = 0;
	if (norm > 1) norm = 1;
	int bar_w = (int)(norm * (w - 2));
	SDL_SetRenderDrawColor(r, 0, 180, 0, 255);
	SDL_Rect rms_bar = { x0 + 1, y0 + 1, bar_w, h - 2 };
	SDL_RenderFillRect(r, &rms_bar);

	/* Peak marker (red line) */
	float peak_db = 20.0f * log10f(peak + 1e-10f);
	float pnorm = (peak_db + 60) / 60;
	if (pnorm < 0) pnorm = 0;
	if (pnorm > 1) pnorm = 1;
	int peak_x = x0 + 1 + (int)(pnorm * (w - 2));
	SDL_SetRenderDrawColor(r, 255, 50, 50, 255);
	SDL_RenderDrawLine(r, peak_x, y0 + 1, peak_x, y0 + h - 1);
}

#define DIR_TRACE_LEN 32  /* number of trail dots */

typedef struct {
	float angle;
	float conf;
} dir_sample_t;

static void draw_direction_indicator(SDL_Renderer *r, float angle_deg,
				     float confidence,
				     const dir_sample_t *trace, int trace_len,
				     int trace_pos,
				     int cx, int cy, int radius)
{
	/* Circle outline */
	SDL_SetRenderDrawColor(r, 60, 60, 60, 255);
	for (int a = 0; a < 360; a++) {
		float rad = a * M_PI / 180.0f;
		int px = cx + (int)(radius * cosf(rad));
		int py = cy - (int)(radius * sinf(rad));
		SDL_RenderDrawPoint(r, px, py);
	}

	/* Fading trail — oldest to newest */
	SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
	for (int i = 0; i < trace_len; i++) {
		/* Oldest first: trace_pos is next write slot, so
		 * trace_pos+0 = oldest, trace_pos-1 = newest */
		int idx = (trace_pos + i) % trace_len;
		float a = trace[idx].angle;
		float c = trace[idx].conf;
		if (c < 0.05f) continue;

		float t = (float)(i + 1) / trace_len; /* 0→1, newest=1 */
		int alpha = (int)(t * 120);
		float trad = a * M_PI / 180.0f;
		int tlen = (int)(radius * 0.85f * c);
		int tx = cx + (int)(tlen * cosf(trad));
		int ty = cy - (int)(tlen * sinf(trad));

		SDL_SetRenderDrawColor(r, 255, 180, 50, alpha);
		for (int dx = -2; dx <= 2; dx++)
			for (int dy = -2; dy <= 2; dy++)
				SDL_RenderDrawPoint(r, tx + dx, ty + dy);
	}
	SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

	/* Current direction line */
	float rad = angle_deg * M_PI / 180.0f;
	int len = (int)(radius * confidence);
	int ex = cx + (int)(len * cosf(rad));
	int ey = cy - (int)(len * sinf(rad));
	SDL_SetRenderDrawColor(r, 255, 200, 0, 255);
	SDL_RenderDrawLine(r, cx, cy, ex, ey);

	/* Arrow tip */
	for (int dx = -3; dx <= 3; dx++)
		for (int dy = -3; dy <= 3; dy++)
			SDL_RenderDrawPoint(r, ex + dx, ey + dy);
}

/* ── 5x7 rounded font for on-screen stats ───────────────────────────── */

#define FONT_W 5
#define FONT_H 7

static const uint8_t font5x7[128][FONT_H] = {
	/* Each row is 5 bits wide, MSB=leftmost pixel (bit4..bit0) */
	['0'] = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
	['1'] = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
	['2'] = {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F},
	['3'] = {0x0E,0x11,0x01,0x06,0x01,0x11,0x0E},
	['4'] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
	['5'] = {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
	['6'] = {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},
	['7'] = {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
	['8'] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
	['9'] = {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
	['.'] = {0x00,0x00,0x00,0x00,0x00,0x06,0x06},
	[','] = {0x00,0x00,0x00,0x00,0x06,0x04,0x08},
	[':'] = {0x00,0x06,0x06,0x00,0x06,0x06,0x00},
	['-'] = {0x00,0x00,0x00,0x0E,0x00,0x00,0x00},
	[' '] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00},
	['A'] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},
	['B'] = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
	['C'] = {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
	['D'] = {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
	['E'] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
	['F'] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
	['G'] = {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},
	['H'] = {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
	['I'] = {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
	['K'] = {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
	['L'] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
	['M'] = {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},
	['N'] = {0x11,0x19,0x15,0x13,0x11,0x11,0x11},
	['P'] = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
	['R'] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
	['S'] = {0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E},
	['T'] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
	['V'] = {0x11,0x11,0x11,0x11,0x0A,0x0A,0x04},
	['X'] = {0x11,0x0A,0x04,0x04,0x04,0x0A,0x11},
	['Z'] = {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
	['a'] = {0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F},
	['b'] = {0x10,0x10,0x1E,0x11,0x11,0x11,0x1E},
	['c'] = {0x00,0x00,0x0E,0x11,0x10,0x11,0x0E},
	['d'] = {0x01,0x01,0x0F,0x11,0x11,0x11,0x0F},
	['e'] = {0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E},
	['f'] = {0x06,0x08,0x08,0x1E,0x08,0x08,0x08},
	['g'] = {0x00,0x00,0x0F,0x11,0x0F,0x01,0x0E},
	['h'] = {0x10,0x10,0x1E,0x11,0x11,0x11,0x11},
	['i'] = {0x04,0x00,0x0C,0x04,0x04,0x04,0x0E},
	['k'] = {0x10,0x10,0x12,0x14,0x18,0x14,0x12},
	['l'] = {0x0C,0x04,0x04,0x04,0x04,0x04,0x0E},
	['m'] = {0x00,0x00,0x1A,0x15,0x15,0x11,0x11},
	['n'] = {0x00,0x00,0x1E,0x11,0x11,0x11,0x11},
	['o'] = {0x00,0x00,0x0E,0x11,0x11,0x11,0x0E},
	['p'] = {0x00,0x00,0x1E,0x11,0x1E,0x10,0x10},
	['r'] = {0x00,0x00,0x16,0x19,0x10,0x10,0x10},
	['s'] = {0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E},
	['t'] = {0x08,0x08,0x1E,0x08,0x08,0x09,0x06},
	['u'] = {0x00,0x00,0x11,0x11,0x11,0x13,0x0D},
	['v'] = {0x00,0x00,0x11,0x11,0x0A,0x0A,0x04},
	['x'] = {0x00,0x00,0x11,0x0A,0x04,0x0A,0x11},
	['y'] = {0x00,0x00,0x11,0x11,0x0F,0x01,0x0E},
	['z'] = {0x00,0x00,0x1F,0x02,0x04,0x08,0x1F},
};

static void draw_text(SDL_Renderer *r, const char *str,
		      int x0, int y0, int scale)
{
	int cx = x0;
	for (const char *p = str; *p; p++) {
		int ch = (unsigned char)*p;
		if (ch >= 128) ch = ' ';
		for (int row = 0; row < FONT_H; row++) {
			uint8_t bits = font5x7[ch][row];
			for (int col = 0; col < FONT_W; col++) {
				if (bits & (0x10 >> col)) {
					SDL_Rect px = { cx + col * scale,
							y0 + row * scale,
							scale, scale };
					SDL_RenderFillRect(r, &px);
				}
			}
		}
		cx += (FONT_W + 1) * scale;
	}
}

/* ── Signal handler ─────────────────────────────────────────────────── */

static void handle_signal(int sig) { (void)sig; running = 0; }

/* ── Usage ──────────────────────────────────────────────────────────── */

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  -d DEVICE   ALSA device (default: %s)\n"
		"  -r RATE     Sample rate (default: %u)\n"
		"  -c CHANS    Channels: 1 or 2 (default: %u)\n"
		"  -n FRAMES   Period/FFT size (default: %lu)\n"
		"  -g GAIN     Software gain multiplier (default: %.1f)\n"
		"  -w MS       Waveform display length in ms (default: %d)\n"
		"  -m DIST     Mic distance in metres for DOA (default: %.3f)\n"
		"  -f          Flip direction 180 deg (mics facing you)\n"
		"  -h          Show this help\n",
		prog, DEFAULT_DEVICE, DEFAULT_RATE, DEFAULT_CHANNELS,
		(unsigned long)DEFAULT_FRAMES, DEFAULT_GAIN,
		DEFAULT_WAVE_MS, DEFAULT_MIC_DIST);
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
	int opt;
	while ((opt = getopt(argc, argv, "d:r:c:n:g:w:m:fh")) != -1) {
		switch (opt) {
		case 'd': alsa_device = optarg; break;
		case 'r': sample_rate = atoi(optarg); break;
		case 'c': channels = atoi(optarg); break;
		case 'n': period_frames = atoi(optarg); break;
		case 'g': gain = atof(optarg); break;
		case 'w': wave_ms = atoi(optarg); break;
		case 'm': mic_distance = atof(optarg); break;
		case 'f': flip_dir = 1; break;
		default: usage(argv[0]); return opt == 'h' ? 0 : 1;
		}
	}

	if (channels < 1 || channels > 2) {
		fprintf(stderr, "Only 1 or 2 channels supported\n");
		return 1;
	}

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	/* Allocate ring buffer */
	int slot_size = channels * period_frames;
	ring_buf = calloc(RING_SLOTS * slot_size, sizeof(float));

	/* Start audio thread */
	pthread_t audio_tid;
	pthread_create(&audio_tid, NULL, audio_thread, NULL);

	/* Wait for audio thread to negotiate parameters */
	usleep(200000);
	if (!running) {
		fprintf(stderr, "Audio init failed\n");
		return 1;
	}

	int fft_size = (int)period_frames;
	int half_fft = fft_size / 2 + 1;

	/* FFTW plans — channel 1 spectrum */
	float *fft_in = fftwf_alloc_real(fft_size);
	fftwf_complex *fft_out = fftwf_alloc_complex(half_fft);
	fftwf_plan plan_fwd = fftwf_plan_dft_r2c_1d(fft_size, fft_in, fft_out,
						     FFTW_ESTIMATE);

	/* GCC-PHAT plans (only if stereo) */
	float *gcc_in1 = NULL, *gcc_in2 = NULL, *gcc_inv_out = NULL;
	fftwf_complex *gcc_out1 = NULL, *gcc_out2 = NULL, *gcc_cross = NULL;
	fftwf_plan gcc_fwd1 = NULL, gcc_fwd2 = NULL, gcc_inv = NULL;
	float *corr = NULL;

	if (channels == 2) {
		gcc_in1 = fftwf_alloc_real(fft_size);
		gcc_in2 = fftwf_alloc_real(fft_size);
		gcc_inv_out = fftwf_alloc_real(fft_size);
		gcc_out1 = fftwf_alloc_complex(half_fft);
		gcc_out2 = fftwf_alloc_complex(half_fft);
		gcc_cross = fftwf_alloc_complex(half_fft);
		corr = calloc(fft_size, sizeof(float));

		gcc_fwd1 = fftwf_plan_dft_r2c_1d(fft_size, gcc_in1, gcc_out1,
						  FFTW_ESTIMATE);
		gcc_fwd2 = fftwf_plan_dft_r2c_1d(fft_size, gcc_in2, gcc_out2,
						  FFTW_ESTIMATE);
		gcc_inv = fftwf_plan_dft_c2r_1d(fft_size, gcc_cross, gcc_inv_out,
						FFTW_ESTIMATE);
	}

	float *mag_db = calloc(half_fft, sizeof(float));
	float *mag_db2 = channels == 2 ? calloc(half_fft, sizeof(float)) : NULL;
	float *ch1_buf = calloc(period_frames, sizeof(float));
	float *ch2_buf = channels == 2 ? calloc(period_frames, sizeof(float)) : NULL;
	float *windowed = calloc(fft_size, sizeof(float));

	/* High-pass filter state */
	float hp_y1 = 0, hp_x1 = 0;
	float hp_y2 = 0, hp_x2 = 0;
	float hp_alpha = 0.985f; /* ~115 Hz cutoff at 48 kHz — kills mains hum */
	int warmup = 5; /* discard first N blocks to let HP filter settle */

	/* Waveform history — circular buffer holding wave_ms of audio */
	int wave_samples = (int)((float)wave_ms / 1000.0f * sample_rate);
	if (wave_samples < (int)period_frames) wave_samples = (int)period_frames;
	if (wave_samples > MAX_WAVE_SAMPLES) wave_samples = MAX_WAVE_SAMPLES;
	float *wave_hist1 = calloc(wave_samples, sizeof(float));
	float *wave_hist2 = channels == 2 ? calloc(wave_samples, sizeof(float)) : NULL;
	int wave_pos = 0;   /* write cursor in circular buffer */
	int wave_filled = 0; /* how many samples have been written total */

	printf("Waveform: %d ms = %d samples\n", wave_ms, wave_samples);

	/* SDL2 init */
	SDL_Init(SDL_INIT_VIDEO);
	if (getenv("HIDE_CURSOR"))
		SDL_ShowCursor(SDL_DISABLE);
	SDL_Window *win = SDL_CreateWindow("Audio Visualizer",
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		WIN_W, WIN_H, 0);
	SDL_Renderer *ren = SDL_CreateRenderer(win, -1,
		SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

	/* Spectrogram texture */
	int spec_h = 150;
	SDL_Texture *spec_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
		SDL_TEXTUREACCESS_STREAMING, SPEC_HISTORY, spec_h);
	/* Clear texture to black */
	{
		Uint32 *px = calloc(SPEC_HISTORY * spec_h, sizeof(Uint32));
		SDL_UpdateTexture(spec_tex, NULL, px, SPEC_HISTORY * sizeof(Uint32));
		free(px);
	}
	int spec_col = 0;

	/* Averaged magnitude buffer for stereo spectrogram */
	float *spec_avg = channels == 2 ? calloc(half_fft, sizeof(float)) : NULL;

	/* Smoothed peak for meters */
	float peak_smooth = 0;

	float db_floor = -80.0f, db_ceil = 0.0f;
	float delay_samples = 0;
	float angle_deg = 90;
	float confidence = 0;

	/* Direction trace ring buffer */
	dir_sample_t dir_trace[DIR_TRACE_LEN];
	memset(dir_trace, 0, sizeof(dir_trace));
	int dir_trace_pos = 0;
	int dir_trace_count = 0; /* frames between trace samples */

	/* Smoothed stats for display */
	float latency_avg = 0;
	float rms_db_avg = -60;
	float dom_freq_avg = 0;

	/* Max lag for GCC-PHAT based on mic distance */
	int max_lag = (int)(mic_distance / SPEED_OF_SOUND * sample_rate) + 10;
	if (max_lag > fft_size / 2) max_lag = fft_size / 2;

	printf("FFT size: %d, bins: %d, max_lag: %d\n", fft_size, half_fft, max_lag);
	if (channels == 2)
		printf("Stereo mode: mic spacing %.1f cm, DOA enabled\n",
		       mic_distance * 100);

	/* ── Main loop ──────────────────────────────────────────────── */

	/* Exit gesture: swipe up from bottom edge (touch displays) */
	int swipe_active = 0;

	while (running) {
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			if (ev.type == SDL_QUIT)
				running = 0;
			if (ev.type == SDL_KEYDOWN &&
			    (ev.key.keysym.sym == SDLK_q ||
			     ev.key.keysym.sym == SDLK_ESCAPE))
				running = 0;
			/* Swipe up from bottom edge to exit */
			if (ev.type == SDL_FINGERDOWN && ev.tfinger.y > 0.9f)
				swipe_active = 1;
			if (ev.type == SDL_FINGERUP)
				swipe_active = 0;
			if (ev.type == SDL_FINGERMOTION && swipe_active &&
			    ev.tfinger.y < 0.5f)
				running = 0;
		}

		/* Read latest audio block from ring buffer */
		int have_data = 0;
		struct timespec capture_ts = {0, 0};
		pthread_mutex_lock(&ring_mtx);
		if (ring_read != ring_write) {
			float *src = ring_buf + ring_read * slot_size;
			capture_ts = ring_ts[ring_read];
			/* De-interleave */
			for (int i = 0; i < (int)period_frames; i++) {
				ch1_buf[i] = src[i * channels];
				if (channels == 2)
					ch2_buf[i] = src[i * channels + 1];
			}
			ring_read = (ring_read + 1) % RING_SLOTS;
			have_data = 1;
		}
		pthread_mutex_unlock(&ring_mtx);

		if (have_data) {
			/* High-pass filter */
			highpass_1pole(ch1_buf, period_frames, &hp_y1, &hp_x1,
				       hp_alpha);
			if (channels == 2)
				highpass_1pole(ch2_buf, period_frames, &hp_y2,
					       &hp_x2, hp_alpha);

			/* Discard first blocks while HP filter settles
			 * (avoids startup transient spike in waveform) */
			if (warmup > 0) { warmup--; have_data = 0; continue; }

			/* Append to waveform history */
			for (int i = 0; i < (int)period_frames; i++) {
				wave_hist1[wave_pos] = ch1_buf[i];
				if (channels == 2)
					wave_hist2[wave_pos] = ch2_buf[i];
				wave_pos = (wave_pos + 1) % wave_samples;
			}
			wave_filled += (int)period_frames;
			if (wave_filled > wave_samples) wave_filled = wave_samples;

			/* FFT of channel 1 */
			apply_hann_window(ch1_buf, windowed, fft_size);
			memcpy(fft_in, windowed, fft_size * sizeof(float));
			fftwf_execute(plan_fwd);
			compute_magnitude_db(fft_out, mag_db, fft_size);

			/* FFT of channel 2 */
			if (channels == 2) {
				apply_hann_window(ch2_buf, windowed, fft_size);
				memcpy(fft_in, windowed, fft_size * sizeof(float));
				fftwf_execute(plan_fwd);
				compute_magnitude_db(fft_out, mag_db2, fft_size);
			}

			/* Spectrogram column — average L+R for stereo */
			if (channels == 2) {
				for (int i = 0; i < half_fft; i++)
					spec_avg[i] = (mag_db[i] + mag_db2[i]) * 0.5f;
				draw_spectrogram_col(spec_tex, spec_avg, half_fft,
						     spec_col, spec_h,
						     db_floor, db_ceil);
			} else {
				draw_spectrogram_col(spec_tex, mag_db, half_fft,
						     spec_col, spec_h,
						     db_floor, db_ceil);
			}
			spec_col = (spec_col + 1) % SPEC_HISTORY;

			/* Level meters */
			float peak = compute_peak(ch1_buf, period_frames);
			peak_smooth = peak > peak_smooth ? peak :
				      peak_smooth * 0.95f;

			/* GCC-PHAT for stereo */
			if (channels == 2) {
				gcc_phat(ch1_buf, ch2_buf, corr, fft_size,
					 gcc_fwd1, gcc_fwd2, gcc_inv,
					 gcc_in1, gcc_in2,
					 gcc_out1, gcc_out2, gcc_cross,
					 gcc_inv_out);

				int lag = find_peak_lag(corr, fft_size, max_lag);
				delay_samples = delay_samples * 0.85f + lag * 0.15f;

				float tau = delay_samples / sample_rate;
				float sin_theta = SPEED_OF_SOUND * tau / mic_distance;
				if (sin_theta > 1) sin_theta = 1;
				if (sin_theta < -1) sin_theta = -1;
				float raw_angle = 90.0f - asinf(sin_theta) * 180.0f / M_PI;
				if (flip_dir) raw_angle = -raw_angle;

				/* Heavier smoothing on displayed angle */
				angle_deg = angle_deg * 0.92f + raw_angle * 0.08f;

				/* Confidence from correlation peak */
				float pk = corr[lag >= 0 ? lag : lag + fft_size];
				confidence = pk > 0 ? (pk < 1 ? pk : 1) : 0;

				/* Record trace every 3rd frame (~15 Hz) */
				if (++dir_trace_count >= 3) {
					dir_trace_count = 0;
					dir_trace[dir_trace_pos].angle = angle_deg;
					dir_trace[dir_trace_pos].conf = confidence;
					dir_trace_pos = (dir_trace_pos + 1) % DIR_TRACE_LEN;
				}
			}
		}

		/* ── Render ─────────────────────────────────────────── */

		SDL_SetRenderDrawColor(ren, 15, 15, 20, 255);
		SDL_RenderClear(ren);

		int margin = 10;
		int panel_w = WIN_W - 2 * margin;
		int stereo = (channels == 2);

		/* In stereo, reserve right panel for stats + direction */
		int right_w = stereo ? 190 : 0;
		int left_w = panel_w - (stereo ? right_w + 10 : 0);
		int right_x = margin + left_w + 10;

		/* Waveform: top area — linearize circular history buffer.
		 * Only draw the filled portion to avoid zero-gap spikes. */
		int wave_h = stereo ? 55 : 100;
		int draw_n = wave_filled < wave_samples ? wave_filled : wave_samples;
		if (draw_n > 0) {
			int start = (wave_pos - draw_n + wave_samples) % wave_samples;
			float *lin1 = malloc(draw_n * sizeof(float));
			for (int i = 0; i < draw_n; i++)
				lin1[i] = wave_hist1[(start + i) % wave_samples];
			draw_waveform(ren, lin1, draw_n,
				      margin, margin, panel_w, wave_h,
				      0, 200, 255);
			free(lin1);

			if (stereo) {
				float *lin2 = malloc(draw_n * sizeof(float));
				for (int i = 0; i < draw_n; i++)
					lin2[i] = wave_hist2[(start + i) % wave_samples];
				draw_waveform(ren, lin2, draw_n,
					      margin, margin + wave_h + 3,
					      panel_w, wave_h,
					      255, 150, 0);
				free(lin2);
			}
		}

		int y_cur = margin + (stereo ? 2 * wave_h + 6 : wave_h + 5);

		/* Level meters (full width) */
		float rms = compute_rms(ch1_buf, period_frames);
		draw_level_bar(ren, "L", rms, peak_smooth,
			       margin, y_cur, panel_w, 12);
		y_cur += 15;

		if (stereo) {
			float rms2 = compute_rms(ch2_buf, period_frames);
			float peak2 = compute_peak(ch2_buf, period_frames);
			draw_level_bar(ren, "R", rms2, peak2,
				       margin, y_cur, panel_w, 12);
			y_cur += 15;
		}

		/* ── Left column: spectra + spectrogram ──────────── */
		int y_left = y_cur;

		if (stereo) {
			/* Single averaged spectrum (L+R pick up same sound) */
			int sh = 100;
			draw_spectrum(ren, spec_avg, half_fft,
				      margin, y_left, left_w, sh,
				      db_floor, db_ceil, sample_rate, fft_size);
			y_left += sh + 3;
		} else {
			/* Mono: single spectrum, full width */
			int sh = 100;
			draw_spectrum(ren, mag_db, half_fft,
				      margin, y_left, left_w, sh,
				      db_floor, db_ceil, sample_rate, fft_size);
			y_left += sh + 5;
		}

		/* Spectrogram — render scrolled so current column is rightmost */
		int sgram_h = WIN_H - y_left - margin;
		if (sgram_h < 40) sgram_h = 40;
		int sgram_w = left_w;

		int left_cols = SPEC_HISTORY - spec_col;
		if (left_cols > 0) {
			SDL_Rect src1 = { spec_col, 0, left_cols, spec_h };
			SDL_Rect dst1 = { margin, y_left,
					  left_cols * sgram_w / SPEC_HISTORY,
					  sgram_h };
			SDL_RenderCopy(ren, spec_tex, &src1, &dst1);
		}
		if (spec_col > 0) {
			SDL_Rect src2 = { 0, 0, spec_col, spec_h };
			SDL_Rect dst2 = {
				margin + left_cols * sgram_w / SPEC_HISTORY,
				y_left,
				spec_col * sgram_w / SPEC_HISTORY,
				sgram_h };
			SDL_RenderCopy(ren, spec_tex, &src2, &dst2);
		}

		/* ── Right column (stereo) or bottom (mono): stats ── */

		/* Measure capture-to-display latency (moving average) */
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (capture_ts.tv_sec > 0) {
			float lat_raw = (now.tv_sec - capture_ts.tv_sec) * 1000.0f +
					(now.tv_nsec - capture_ts.tv_nsec) / 1e6f;
			latency_avg = latency_avg * 0.9f + lat_raw * 0.1f;
		}

		/* Smoothed RMS */
		float rms_now = compute_rms(ch1_buf, period_frames);
		float rms_db_now = 20.0f * log10f(rms_now + 1e-10f);
		rms_db_avg = rms_db_avg * 0.85f + rms_db_now * 0.15f;

		/* Find dominant frequency — skip DC area and top 5% (noise) */
		int bin_min = (int)(120.0f / ((float)sample_rate / fft_size));
		int bin_max = half_fft * 95 / 100;
		if (bin_min < 1) bin_min = 1;
		int peak_bin = bin_min;
		float peak_mag = -200;
		for (int i = bin_min; i < bin_max; i++) {
			if (mag_db[i] > peak_mag) {
				peak_mag = mag_db[i];
				peak_bin = i;
			}
		}
		float dom_freq_now = (float)peak_bin * sample_rate / fft_size;
		dom_freq_avg = dom_freq_avg * 0.95f + dom_freq_now * 0.05f;

		int txt_scale = 2;
		int line_h = (FONT_H + 2) * txt_scale;

		if (stereo) {
			/* Stats in right panel */
			int sy = y_cur;
			SDL_SetRenderDrawColor(ren, 140, 160, 180, 255);

			char stat[32];
			snprintf(stat, sizeof(stat), "Lat %5.1f ms", latency_avg);
			draw_text(ren, stat, right_x, sy, txt_scale);
			sy += line_h;

			snprintf(stat, sizeof(stat), "RMS %5.1f dB", rms_db_avg);
			draw_text(ren, stat, right_x, sy, txt_scale);
			sy += line_h;

			snprintf(stat, sizeof(stat), "Frq %5d Hz", (int)dom_freq_avg);
			draw_text(ren, stat, right_x, sy, txt_scale);
			sy += line_h;

			snprintf(stat, sizeof(stat), "Gain %4.1fx", gain);
			draw_text(ren, stat, right_x, sy, txt_scale);
			sy += line_h + 10;

			/* Direction indicator */
			int dir_r = 60;
			int dir_cx = right_x + right_w / 2;
			int dir_cy = sy + dir_r + 5;
			if (dir_cy + dir_r < WIN_H - margin) {
				draw_direction_indicator(ren, angle_deg,
							 confidence,
							 dir_trace, DIR_TRACE_LEN,
							 dir_trace_pos,
							 dir_cx, dir_cy, dir_r);

				/* Angle label below indicator */
				char ang[32];
				snprintf(ang, sizeof(ang), "%3.0f deg", angle_deg);
				SDL_SetRenderDrawColor(ren, 140, 160, 180, 255);
				draw_text(ren, ang, dir_cx - 30,
					  dir_cy + dir_r + 5, txt_scale);
			}
		} else {
			/* Mono: stats below spectrogram */
			int sy = y_left + sgram_h + 5;
			char line1[64], line2[64];
			snprintf(line1, sizeof(line1),
				 "Latency %6.1f ms   RMS %6.1f dB",
				 latency_avg, rms_db_avg);
			snprintf(line2, sizeof(line2),
				 "Freq  %6d Hz   Gain  %5.1fx",
				 (int)dom_freq_avg, gain);
			SDL_SetRenderDrawColor(ren, 140, 160, 180, 255);
			draw_text(ren, line1, margin, sy, txt_scale);
			draw_text(ren, line2, margin, sy + line_h, txt_scale);
		}

		SDL_RenderPresent(ren);
	}

	/* Cleanup */
	pthread_join(audio_tid, NULL);

	fftwf_destroy_plan(plan_fwd);
	fftwf_free(fft_in);
	fftwf_free(fft_out);

	if (channels == 2) {
		fftwf_destroy_plan(gcc_fwd1);
		fftwf_destroy_plan(gcc_fwd2);
		fftwf_destroy_plan(gcc_inv);
		fftwf_free(gcc_in1);
		fftwf_free(gcc_in2);
		fftwf_free(gcc_inv_out);
		fftwf_free(gcc_out1);
		fftwf_free(gcc_out2);
		fftwf_free(gcc_cross);
		free(corr);
	}

	free(mag_db);
	free(mag_db2);
	free(spec_avg);
	free(ch1_buf);
	free(ch2_buf);
	free(windowed);
	free(ring_buf);
	free(wave_hist1);
	free(wave_hist2);

	SDL_DestroyTexture(spec_tex);
	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
	SDL_Quit();

	printf("Done.\n");
	return 0;
}
