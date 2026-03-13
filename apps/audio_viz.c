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
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
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

/* ── Globals ────────────────────────────────────────────────────────── */

static volatile int running = 1;

/* Audio config */
static const char *alsa_device = DEFAULT_DEVICE;
static unsigned int sample_rate = DEFAULT_RATE;
static unsigned int channels = DEFAULT_CHANNELS;
static snd_pcm_uframes_t period_frames = DEFAULT_FRAMES;
static float mic_distance = DEFAULT_MIC_DIST;

/* Ring buffer: audio thread writes, render thread reads */
static float *ring_buf;           /* [RING_SLOTS][channels * period_frames] */
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
	snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_FLOAT_LE);
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
	printf("ALSA: %s, %u Hz, %u ch, period %lu frames\n",
	       alsa_device, sample_rate, channels, period_frames);

	int slot_size = channels * period_frames;
	float *tmp = malloc(slot_size * sizeof(float));

	while (running) {
		snd_pcm_sframes_t n = snd_pcm_readi(pcm, tmp, period_frames);
		if (n < 0) {
			n = snd_pcm_recover(pcm, (int)n, 0);
			if (n < 0) {
				fprintf(stderr, "ALSA read error: %s\n", snd_strerror((int)n));
				break;
			}
			continue;
		}

		pthread_mutex_lock(&ring_mtx);
		memcpy(ring_buf + ring_write * slot_size, tmp,
		       n * channels * sizeof(float));
		ring_write = (ring_write + 1) % RING_SLOTS;
		if (ring_write == ring_read)
			ring_read = (ring_read + 1) % RING_SLOTS; /* drop oldest */
		pthread_mutex_unlock(&ring_mtx);
	}

	free(tmp);
	snd_pcm_close(pcm);
	return NULL;
}

/* ── Drawing helpers ────────────────────────────────────────────────── */

static void draw_waveform(SDL_Renderer *r, const float *buf, int n,
			  int x0, int y0, int w, int h,
			  Uint8 cr, Uint8 cg, Uint8 cb)
{
	int mid = y0 + h / 2;

	/* Border */
	SDL_SetRenderDrawColor(r, 60, 60, 60, 255);
	SDL_Rect border = { x0, y0, w, h };
	SDL_RenderDrawRect(r, &border);

	/* Zero line */
	SDL_SetRenderDrawColor(r, 40, 40, 40, 255);
	SDL_RenderDrawLine(r, x0, mid, x0 + w, mid);

	/* Waveform */
	SDL_SetRenderDrawColor(r, cr, cg, cb, 255);
	for (int i = 1; i < w && i < n; i++) {
		int idx0 = (i - 1) * n / w;
		int idx1 = i * n / w;
		int py0 = mid - (int)(buf[idx0] * h / 2);
		int py1 = mid - (int)(buf[idx1] * h / 2);
		SDL_RenderDrawLine(r, x0 + i - 1, py0, x0 + i, py1);
	}
}

static void draw_spectrum(SDL_Renderer *r, const float *mag_db, int bins,
			  int x0, int y0, int w, int h,
			  float db_floor, float db_ceil)
{
	/* Border */
	SDL_SetRenderDrawColor(r, 60, 60, 60, 255);
	SDL_Rect border = { x0, y0, w, h };
	SDL_RenderDrawRect(r, &border);

	float db_range = db_ceil - db_floor;

	for (int i = 0; i < w && i < bins; i++) {
		int bin = i * bins / w;
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

static void draw_direction_indicator(SDL_Renderer *r, float angle_deg,
				     float confidence,
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

	/* Direction line */
	float rad = angle_deg * M_PI / 180.0f;
	int len = (int)(radius * confidence);
	int ex = cx + (int)(len * cosf(rad));
	int ey = cy - (int)(len * sinf(rad));
	SDL_SetRenderDrawColor(r, 255, 200, 0, 255);
	SDL_RenderDrawLine(r, cx, cy, ex, ey);

	/* Arrow tip */
	SDL_SetRenderDrawColor(r, 255, 200, 0, 255);
	for (int dx = -3; dx <= 3; dx++)
		for (int dy = -3; dy <= 3; dy++)
			SDL_RenderDrawPoint(r, ex + dx, ey + dy);
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
		"  -m DIST     Mic distance in metres for DOA (default: %.3f)\n"
		"  -h          Show this help\n",
		prog, DEFAULT_DEVICE, DEFAULT_RATE, DEFAULT_CHANNELS,
		(unsigned long)DEFAULT_FRAMES, DEFAULT_MIC_DIST);
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
	int opt;
	while ((opt = getopt(argc, argv, "d:r:c:n:m:h")) != -1) {
		switch (opt) {
		case 'd': alsa_device = optarg; break;
		case 'r': sample_rate = atoi(optarg); break;
		case 'c': channels = atoi(optarg); break;
		case 'n': period_frames = atoi(optarg); break;
		case 'm': mic_distance = atof(optarg); break;
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
						     FFTW_MEASURE);

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
						  FFTW_MEASURE);
		gcc_fwd2 = fftwf_plan_dft_r2c_1d(fft_size, gcc_in2, gcc_out2,
						  FFTW_MEASURE);
		gcc_inv = fftwf_plan_dft_c2r_1d(fft_size, gcc_cross, gcc_inv_out,
						FFTW_MEASURE);
	}

	float *mag_db = calloc(half_fft, sizeof(float));
	float *ch1_buf = calloc(period_frames, sizeof(float));
	float *ch2_buf = channels == 2 ? calloc(period_frames, sizeof(float)) : NULL;
	float *windowed = calloc(fft_size, sizeof(float));

	/* High-pass filter state */
	float hp_y1 = 0, hp_x1 = 0;
	float hp_y2 = 0, hp_x2 = 0;
	float hp_alpha = 0.995f; /* ~80 Hz cutoff at 48 kHz */

	/* SDL2 init */
	SDL_Init(SDL_INIT_VIDEO);
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

	/* Smoothed peak for meters */
	float peak_smooth = 0;

	float db_floor = -80.0f, db_ceil = 0.0f;
	float delay_samples = 0;
	float angle_deg = 90;
	float confidence = 0;

	/* Max lag for GCC-PHAT based on mic distance */
	int max_lag = (int)(mic_distance / SPEED_OF_SOUND * sample_rate) + 10;
	if (max_lag > fft_size / 2) max_lag = fft_size / 2;

	printf("FFT size: %d, bins: %d, max_lag: %d\n", fft_size, half_fft, max_lag);
	if (channels == 2)
		printf("Stereo mode: mic spacing %.1f cm, DOA enabled\n",
		       mic_distance * 100);

	/* ── Main loop ──────────────────────────────────────────────── */

	while (running) {
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			if (ev.type == SDL_QUIT)
				running = 0;
			if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_q)
				running = 0;
		}

		/* Read latest audio block from ring buffer */
		int have_data = 0;
		pthread_mutex_lock(&ring_mtx);
		if (ring_read != ring_write) {
			float *src = ring_buf + ring_read * slot_size;
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

			/* FFT of channel 1 */
			apply_hann_window(ch1_buf, windowed, fft_size);
			memcpy(fft_in, windowed, fft_size * sizeof(float));
			fftwf_execute(plan_fwd);
			compute_magnitude_db(fft_out, mag_db, fft_size);

			/* Spectrogram column */
			draw_spectrogram_col(spec_tex, mag_db, half_fft,
					     spec_col, spec_h, db_floor, db_ceil);
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
				delay_samples = delay_samples * 0.7f + lag * 0.3f;

				float tau = delay_samples / sample_rate;
				float sin_theta = SPEED_OF_SOUND * tau / mic_distance;
				if (sin_theta > 1) sin_theta = 1;
				if (sin_theta < -1) sin_theta = -1;
				angle_deg = 90.0f - asinf(sin_theta) * 180.0f / M_PI;

				/* Confidence from correlation peak */
				float pk = corr[lag >= 0 ? lag : lag + fft_size];
				confidence = pk > 0 ? (pk < 1 ? pk : 1) : 0;
			}
		}

		/* ── Render ─────────────────────────────────────────── */

		SDL_SetRenderDrawColor(ren, 15, 15, 20, 255);
		SDL_RenderClear(ren);

		int margin = 10;
		int panel_w = WIN_W - 2 * margin;

		/* Waveform: top area */
		int wave_h = channels == 2 ? 70 : 100;
		draw_waveform(ren, ch1_buf, period_frames,
			      margin, margin, panel_w, wave_h,
			      0, 200, 255);

		if (channels == 2)
			draw_waveform(ren, ch2_buf, period_frames,
				      margin, margin + wave_h + 5,
				      panel_w, wave_h,
				      255, 150, 0);

		int y_after_wave = margin + (channels == 2 ? 2 * wave_h + 10 : wave_h + 5);

		/* Level meter */
		float rms = compute_rms(ch1_buf, period_frames);
		draw_level_bar(ren, "RMS", rms, peak_smooth,
			       margin, y_after_wave, panel_w, 15);
		y_after_wave += 20;

		/* FFT spectrum */
		int spec_bar_h = 100;
		draw_spectrum(ren, mag_db, half_fft,
			      margin, y_after_wave, panel_w, spec_bar_h,
			      db_floor, db_ceil);
		y_after_wave += spec_bar_h + 5;

		/* Spectrogram — render scrolled so current column is rightmost */
		/* Left part: columns from spec_col to SPEC_HISTORY */
		int left_cols = SPEC_HISTORY - spec_col;
		if (left_cols > 0) {
			SDL_Rect src1 = { spec_col, 0, left_cols, spec_h };
			SDL_Rect dst1 = { margin, y_after_wave,
					  left_cols * panel_w / SPEC_HISTORY,
					  spec_h };
			SDL_RenderCopy(ren, spec_tex, &src1, &dst1);
		}
		/* Right part: columns 0 to spec_col */
		if (spec_col > 0) {
			SDL_Rect src2 = { 0, 0, spec_col, spec_h };
			SDL_Rect dst2 = {
				margin + left_cols * panel_w / SPEC_HISTORY,
				y_after_wave,
				spec_col * panel_w / SPEC_HISTORY,
				spec_h };
			SDL_RenderCopy(ren, spec_tex, &src2, &dst2);
		}
		y_after_wave += spec_h + 5;

		/* Direction indicator (stereo only) */
		if (channels == 2) {
			int dir_r = 50;
			draw_direction_indicator(ren, angle_deg, confidence,
						 WIN_W - margin - dir_r - 10,
						 margin + dir_r + 10, dir_r);
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
	free(ch1_buf);
	free(ch2_buf);
	free(windowed);
	free(ring_buf);

	SDL_DestroyTexture(spec_tex);
	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
	SDL_Quit();

	printf("Done.\n");
	return 0;
}
