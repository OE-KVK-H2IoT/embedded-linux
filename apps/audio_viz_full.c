/*
 * audio_viz_full.c — Full-featured I2S Audio Visualizer
 *
 * All challenge features from the Audio Visualizer tutorial built in:
 *   - Peak hold spectrum (white decay line)
 *   - Noise gate for direction estimation
 *   - Musical note tuner display
 *   - WAV file recording with on-screen touch button
 *   - Band-pass filter with on-screen toggle
 *   - Sub-sample TDOA (parabolic interpolation)
 *   - CPU budget timing display
 *   - On-screen touch button bar (REC, FILTER, GATE)
 *
 * Dependencies: ALSA, SDL2, FFTW3
 *
 * Build:  gcc -Wall -O2 $(sdl2-config --cflags) -o audio_viz_full audio_viz_full.c \
 *           $(sdl2-config --libs) -lasound -lfftw3f -lm -lpthread
 *
 * Run:    ./audio_viz_full -d hw:1,0 -c 2 -m 0.06 -f
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
#define DEFAULT_FRAMES    1024
#define LOWLAT_FRAMES     512       /* -l: low-latency period size (10.7ms at 48kHz) */
#define RING_SLOTS        8         /* capture ring — needs headroom for burst drain */

#define WIN_W             800
#define WIN_H             480
#define SPEC_HISTORY      256

#define SPEED_OF_SOUND    343.0f
#define DEFAULT_MIC_DIST  0.06f
#define DEFAULT_GAIN      4.0f      /* display gain (I2S mics are quiet) */
#define DEFAULT_WAVE_MS   50
#define MAX_WAVE_SAMPLES  (48000*2)

/* ── Globals ────────────────────────────────────────────────────────── */

static volatile int running = 1;

static const char *alsa_device = DEFAULT_DEVICE;
static unsigned int sample_rate = DEFAULT_RATE;
static unsigned int channels = DEFAULT_CHANNELS;
static snd_pcm_uframes_t period_frames = DEFAULT_FRAMES;
static float mic_distance = DEFAULT_MIC_DIST;
static float gain = DEFAULT_GAIN;
static int wave_ms = DEFAULT_WAVE_MS;
static int flip_dir = 0;
static int low_latency = 0;       /* -l: optimize for FX latency */

/* Noise gate */
static float gate_threshold_db = -40.0f;
static int gate_enabled = 1;

/* Band-pass filter */
static float lp_cutoff = 3000; /* default: 3 kHz low-pass (toggled off initially) */
static float hp_cutoff = 115;  /* default: mains hum rejection */

/* ALSA playback — 'default' works through PipeWire/PulseAudio which
 * handles buffering well.  Override with -o for direct hardware. */
static const char *playback_device = "default";
static int playback_active = 0;

/* ── 8-band graphic EQ ─────────────────────────────────────────────── */

#define EQ_BANDS   8
#define EQ_MAX_DB  12.0f

typedef struct {
	float b0, b1, b2, a1, a2; /* normalized (a0 = 1) */
	float x1, x2, y1, y2;     /* filter state */
} biquad_t;

typedef struct {
	float freq;       /* center frequency */
	float gain_db;    /* current gain (-12 to +12) */
	float q;          /* bandwidth */
	biquad_t filt[2]; /* per channel */
} eq_band_t;

static eq_band_t eq_bands[EQ_BANDS] = {
	{   60, 0, 1.4f, {{0},{0}} },
	{  150, 0, 1.4f, {{0},{0}} },
	{  400, 0, 1.4f, {{0},{0}} },
	{ 1000, 0, 1.4f, {{0},{0}} },
	{ 2500, 0, 1.4f, {{0},{0}} },
	{ 6000, 0, 1.4f, {{0},{0}} },
	{12000, 0, 1.4f, {{0},{0}} },
	{16000, 0, 1.4f, {{0},{0}} },
};

static int eq_enabled = 0;
static int eq_overlay = 0; /* show EQ overlay */

/* Delay / echo effect */
#define MAX_DELAY_SAMPLES (48000)  /* max 1 second */
static float *delay_buf_l = NULL;
static float *delay_buf_r = NULL;
static int delay_len = 0;         /* in samples */
static int delay_pos = 0;
static float delay_feedback = 0.2f;
static float delay_mix = 0.3f;
static int delay_enabled = 0;
static float delay_ms = 0.0f;     /* default: off (drag slider to enable) */

/* Voice effects for playback */
#define FX_NONE      0
#define FX_CHIPMUNK  1
#define FX_DEEP      2
#define FX_ROBOT     3
#define FX_COUNT     4
static int fx_mode = FX_NONE;
static const char *fx_names[FX_COUNT] = {
	"Normal", "Chipmunk", "Deep", "Robot"
};

/* Noise gate for playback */
static float playback_gate_db = -45.0f;
static int noise_reduce = 1; /* on by default */

/* Pipeline latency */
static float pipeline_latency_ms = 0;

/* Ring buffer */
static float *ring_buf;
static struct timespec ring_ts[RING_SLOTS];
static int ring_write = 0;
static int ring_read  = 0;
static pthread_mutex_t ring_mtx = PTHREAD_MUTEX_INITIALIZER;

/* ── WAV recording ──────────────────────────────────────────────────── */

static FILE *wav_file = NULL;
static uint32_t wav_data_bytes = 0;
static int recording = 0;

static void wav_start(int rate, int ch)
{
	/* Generate timestamped filename */
	char path[64];
	time_t now = time(NULL);
	struct tm *t = localtime(&now);
	snprintf(path, sizeof(path), "rec_%04d%02d%02d_%02d%02d%02d.wav",
		 t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
		 t->tm_hour, t->tm_min, t->tm_sec);

	wav_file = fopen(path, "wb");
	if (!wav_file) { perror("fopen wav"); return; }

	uint16_t bits = 16, fmt = 1;
	uint16_t nch = (uint16_t)ch;
	uint32_t byte_rate = rate * ch * bits / 8;
	uint16_t block_align = ch * bits / 8;
	uint32_t zero = 0, chunk = 16;
	uint32_t urate = (uint32_t)rate;

	fwrite("RIFF", 1, 4, wav_file);
	fwrite(&zero, 4, 1, wav_file);
	fwrite("WAVEfmt ", 1, 8, wav_file);
	fwrite(&chunk, 4, 1, wav_file);
	fwrite(&fmt, 2, 1, wav_file);
	fwrite(&nch, 2, 1, wav_file);
	fwrite(&urate, 4, 1, wav_file);
	fwrite(&byte_rate, 4, 1, wav_file);
	fwrite(&block_align, 2, 1, wav_file);
	fwrite(&bits, 2, 1, wav_file);
	fwrite("data", 1, 4, wav_file);
	fwrite(&zero, 4, 1, wav_file);

	wav_data_bytes = 0;
	recording = 1;
	printf("Recording to %s...\n", path);
}

static void wav_write_samples(const float *buf, int n)
{
	if (!wav_file) return;
	for (int i = 0; i < n; i++) {
		float s = buf[i];
		if (s > 1.0f) s = 1.0f;
		if (s < -1.0f) s = -1.0f;
		int16_t sample = (int16_t)(s * 32767);
		fwrite(&sample, 2, 1, wav_file);
	}
	wav_data_bytes += n * 2;
}

static void wav_stop(void)
{
	if (!wav_file) return;
	uint32_t file_size = wav_data_bytes + 36;
	fseek(wav_file, 4, SEEK_SET);
	fwrite(&file_size, 4, 1, wav_file);
	fseek(wav_file, 40, SEEK_SET);
	fwrite(&wav_data_bytes, 4, 1, wav_file);
	fclose(wav_file);
	wav_file = NULL;
	recording = 0;
	printf("Recording stopped (%u bytes)\n", wav_data_bytes);
}

/* ── DSP helpers ────────────────────────────────────────────────────── */

static void apply_hann_window(const float *in, float *out, int n)
{
	for (int i = 0; i < n; i++)
		out[i] = in[i] * 0.5f * (1.0f - cosf(2.0f * M_PI * i / (n - 1)));
}

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

static void lowpass_1pole(float *buf, int n, float *y_prev, float alpha)
{
	for (int i = 0; i < n; i++) {
		float y = alpha * buf[i] + (1.0f - alpha) * *y_prev;
		*y_prev = y;
		buf[i] = y;
	}
}

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

static float compute_rms(const float *buf, int n)
{
	float sum = 0;
	for (int i = 0; i < n; i++) sum += buf[i] * buf[i];
	return sqrtf(sum / n);
}

static float compute_peak(const float *buf, int n)
{
	float peak = 0;
	for (int i = 0; i < n; i++) {
		float a = fabsf(buf[i]);
		if (a > peak) peak = a;
	}
	return peak;
}

/* ── Biquad EQ (peaking filter — Audio EQ Cookbook) ─────────────────── */

static void biquad_compute_peaking(biquad_t *bq, float freq, float gain_db,
				   float q, float srate)
{
	if (fabsf(gain_db) < 0.1f) {
		/* Bypass: unity gain */
		bq->b0 = 1; bq->b1 = 0; bq->b2 = 0;
		bq->a1 = 0; bq->a2 = 0;
		return;
	}
	float A = powf(10.0f, gain_db / 40.0f);
	float w0 = 2.0f * M_PI * freq / srate;
	float alpha = sinf(w0) / (2.0f * q);

	float a0  =  1.0f + alpha / A;
	bq->b0 = (1.0f + alpha * A) / a0;
	bq->b1 = (-2.0f * cosf(w0)) / a0;
	bq->b2 = (1.0f - alpha * A) / a0;
	bq->a1 = (-2.0f * cosf(w0)) / a0;
	bq->a2 = (1.0f - alpha / A) / a0;
}

static void biquad_process(biquad_t *bq, float *buf, int n)
{
	for (int i = 0; i < n; i++) {
		float x = buf[i];
		float y = bq->b0 * x + bq->b1 * bq->x1 + bq->b2 * bq->x2
			- bq->a1 * bq->y1 - bq->a2 * bq->y2;
		bq->x2 = bq->x1; bq->x1 = x;
		bq->y2 = bq->y1; bq->y1 = y;
		buf[i] = y;
	}
}

static void eq_update_coeffs(void)
{
	for (int b = 0; b < EQ_BANDS; b++) {
		for (int ch = 0; ch < 2; ch++)
			biquad_compute_peaking(&eq_bands[b].filt[ch],
					       eq_bands[b].freq,
					       eq_bands[b].gain_db,
					       eq_bands[b].q,
					       (float)sample_rate);
	}
}

static void eq_apply(float *buf, int n, int ch)
{
	for (int b = 0; b < EQ_BANDS; b++)
		biquad_process(&eq_bands[b].filt[ch], buf, n);
}

/* ── Delay / echo effect ───────────────────────────────────────────── */

static void delay_process(float *buf, int n, float *dbuf)
{
	for (int i = 0; i < n; i++) {
		int rd = (delay_pos - delay_len + MAX_DELAY_SAMPLES)
			 % MAX_DELAY_SAMPLES;
		float delayed = dbuf[rd];
		float out = buf[i] + delayed * delay_mix;
		dbuf[delay_pos] = buf[i] + delayed * delay_feedback;
		delay_pos = (delay_pos + 1) % MAX_DELAY_SAMPLES;
		buf[i] = out;
	}
}

/* ── Voice effects (applied to playback copy only) ─────────────────── */

/*
 * Chipmunk: read through input FASTER → output has higher pitch.
 *   ratio > 1 means we consume more input per output sample.
 * Deep: read through input SLOWER → output has lower pitch.
 *   ratio < 1 means we consume less input per output sample.
 *
 * Both use a crossfade at the wrap point (last 64 samples blend
 * with the start) to eliminate the periodic pop/click.
 */

static float fx_phase_l = 0, fx_phase_r = 0;
static float fx_robot_phase = 0;  /* ring modulator oscillator */

#define XFADE_LEN 64

static void fx_resample(float *buf, int n, float ratio, float *phase)
{
	float tmp[4096];
	if (n > 4096) n = 4096;
	memcpy(tmp, buf, n * sizeof(float));

	for (int i = 0; i < n; i++) {
		float pos = fmodf(*phase, (float)n);
		int idx0 = (int)pos;
		int idx1 = (idx0 + 1) % n;
		float frac = pos - idx0;
		float sample = tmp[idx0] * (1.0f - frac) + tmp[idx1] * frac;

		/* Crossfade near the wrap point to avoid click */
		float dist_to_end = (float)n - pos;
		if (dist_to_end < XFADE_LEN) {
			float t = dist_to_end / XFADE_LEN; /* 1→0 */
			/* Blend with sample from the beginning */
			float wrap_pos = fmodf(pos - (float)n, (float)n);
			if (wrap_pos < 0) wrap_pos += n;
			int w0 = (int)wrap_pos;
			int w1 = (w0 + 1) % n;
			float wfrac = wrap_pos - w0;
			float wrap_sample = tmp[w0] * (1.0f - wfrac) +
					    tmp[w1] * wfrac;
			sample = sample * t + wrap_sample * (1.0f - t);
		}

		buf[i] = sample;
		*phase += ratio;
	}
	/* Keep phase bounded to prevent float precision loss */
	*phase = fmodf(*phase, (float)n);
}

static void fx_apply(float *buf, int n, int mode, float *phase)
{
	switch (mode) {
	case FX_CHIPMUNK:
		fx_resample(buf, n, 1.6f, phase); /* faster read → higher */
		break;
	case FX_DEEP:
		fx_resample(buf, n, 0.6f, phase); /* slower read → lower */
		break;
	case FX_ROBOT: {
		/* Bitcrusher + ring mod = classic "Dalek" robot voice.
		 * 1. Reduce sample rate (sample-and-hold every N samples)
		 * 2. Ring modulate at low frequency for metallic tone */
		int decimate = 6; /* hold every 6th sample → ~8 kHz staircase */
		float mod_freq = 80.0f;
		float step = 2.0f * M_PI * mod_freq / sample_rate;
		float held = 0;
		for (int i = 0; i < n; i++) {
			if (i % decimate == 0)
				held = buf[i];
			/* Ring mod: true multiply (no offset) → metallic */
			buf[i] = held * sinf(fx_robot_phase);
			fx_robot_phase += step;
		}
		if (fx_robot_phase > 2.0f * M_PI * 1000)
			fx_robot_phase -= 2.0f * M_PI * 1000;
		break;
	}
	default:
		break;
	}
}

/* ── Noise reduction (simple gate + smoothing) ─────────────────────── */

static void noise_reduce_process(float *buf, int n, float gate_db)
{
	float rms = 0;
	for (int i = 0; i < n; i++) rms += buf[i] * buf[i];
	rms = sqrtf(rms / n);
	float db = 20.0f * log10f(rms + 1e-10f);

	if (db < gate_db) {
		/* Below threshold: fade to silence (soft gate) */
		float atten = (db - gate_db) / 10.0f; /* -10dB fade range */
		if (atten < -1) atten = -1;
		float gain_factor = 1.0f + atten; /* 1 at threshold, 0 at -10dB below */
		if (gain_factor < 0) gain_factor = 0;
		for (int i = 0; i < n; i++)
			buf[i] *= gain_factor;
	}
}

/* ── Musical note detection ─────────────────────────────────────────── */

static const char *note_names[] = {
	"A", "A#", "B", "C", "C#", "D",
	"D#", "E", "F", "F#", "G", "G#"
};

static void freq_to_note(float freq, char *buf, int buflen)
{
	if (freq < 20.0f || freq > 20000.0f) {
		snprintf(buf, buflen, "---");
		return;
	}
	float semitones = 12.0f * log2f(freq / 440.0f);
	int nearest = (int)roundf(semitones);
	float cents = (semitones - nearest) * 100.0f;
	int note_idx = ((nearest % 12) + 12) % 12;
	int octave = 4 + (nearest + 9) / 12;
	if (nearest + 9 < 0)
		octave = 4 + (nearest + 9 - 11) / 12;
	snprintf(buf, buflen, "%s%d %+.0fc",
		 note_names[note_idx], octave, cents);
}

/* ── GCC-PHAT with sub-sample interpolation ─────────────────────────── */

static void gcc_phat(const float *ch1, const float *ch2, float *corr,
		     int n, fftwf_plan fwd1, fftwf_plan fwd2,
		     fftwf_plan inv, float *in1, float *in2,
		     fftwf_complex *out1, fftwf_complex *out2,
		     fftwf_complex *cross, float *inv_out)
{
	apply_hann_window(ch1, in1, n);
	apply_hann_window(ch2, in2, n);
	fftwf_execute(fwd1);
	fftwf_execute(fwd2);

	int half = n / 2 + 1;
	for (int i = 0; i < half; i++) {
		float re1 = out1[i][0], im1 = out1[i][1];
		float re2 = out2[i][0], im2 = out2[i][1];
		float cre = re1 * re2 + im1 * im2;
		float cim = im1 * re2 - re1 * im2;
		float mag = sqrtf(cre * cre + cim * cim) + 1e-10f;
		cross[i][0] = cre / mag;
		cross[i][1] = cim / mag;
	}

	fftwf_execute(inv);
	for (int i = 0; i < n; i++)
		corr[i] = inv_out[i] / n;
}

static float find_peak_lag_subsample(const float *corr, int n, int max_lag)
{
	float best = -1e30f;
	int best_i = 0;
	for (int i = 0; i < max_lag; i++) {
		if (corr[i] > best) { best = corr[i]; best_i = i; }
	}
	for (int i = n - max_lag; i < n; i++) {
		if (corr[i] > best) { best = corr[i]; best_i = i; }
	}

	/* Parabolic interpolation for sub-sample precision */
	int km1 = (best_i - 1 + n) % n;
	int kp1 = (best_i + 1) % n;
	float denom = corr[km1] - 2 * corr[best_i] + corr[kp1];
	float p = 0;
	if (fabsf(denom) > 1e-10f)
		p = 0.5f * (corr[km1] - corr[kp1]) / denom;

	float lag = (float)best_i + p;
	if (lag > n / 2.0f) lag -= n;
	return lag;
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

		for (int i = 0; i < (int)(n * channels); i++)
			tmp[i] = (float)raw_buf[i] / 2147483648.0f * gain;

		pthread_mutex_lock(&ring_mtx);
		clock_gettime(CLOCK_MONOTONIC, &ring_ts[ring_write]);
		memcpy(ring_buf + ring_write * slot_size, tmp,
		       n * channels * sizeof(float));
		ring_write = (ring_write + 1) % RING_SLOTS;
		if (ring_write == ring_read)
			ring_read = (ring_read + 1) % RING_SLOTS;
		pthread_mutex_unlock(&ring_mtx);
	}

	free(raw_buf);
	free(tmp);
	snd_pcm_close(pcm);
	return NULL;
}

/* ── ALSA playback ring buffer ──────────────────────────────────────── */

#define PLAY_RING_SLOTS  8  /* enough to absorb render frame jitter */
static float *play_ring;
static int play_ring_write = 0;
static int play_ring_read  = 0;
static pthread_mutex_t play_mtx  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  play_cond = PTHREAD_COND_INITIALIZER;
static float playback_latency_ms = 0;

static void *playback_thread(void *arg)
{
	(void)arg;
	snd_pcm_t *pcm = NULL;
	snd_pcm_hw_params_t *hw;
	int err;

	if ((err = snd_pcm_open(&pcm, playback_device,
				SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
		fprintf(stderr, "ALSA playback open '%s': %s\n",
			playback_device, snd_strerror(err));
		playback_device = NULL; /* signal: no output available */
		return NULL;
	}

	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_any(pcm, hw);
	snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
	snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
	unsigned int prate = sample_rate;
	snd_pcm_hw_params_set_rate_near(pcm, hw, &prate, NULL);
	snd_pcm_hw_params_set_channels(pcm, hw, channels);

	/* Request 4 periods for slack against scheduling jitter */
	snd_pcm_uframes_t pf = period_frames;
	snd_pcm_hw_params_set_period_size_near(pcm, hw, &pf, NULL);
	unsigned int nperiods = 4;
	snd_pcm_hw_params_set_periods_near(pcm, hw, &nperiods, NULL);

	if ((err = snd_pcm_hw_params(pcm, hw)) < 0) {
		fprintf(stderr, "ALSA playback hw_params: %s\n",
			snd_strerror(err));
		snd_pcm_close(pcm);
		return NULL;
	}

	snd_pcm_hw_params_get_period_size(hw, &pf, NULL);
	snd_pcm_uframes_t buf_size;
	snd_pcm_hw_params_get_buffer_size(hw, &buf_size);
	playback_latency_ms = (float)buf_size / prate * 1000.0f;

	/* SW params: start playback after filling half the buffer */
	snd_pcm_sw_params_t *sw;
	snd_pcm_sw_params_alloca(&sw);
	snd_pcm_sw_params_current(pcm, sw);
	snd_pcm_sw_params_set_start_threshold(pcm, sw, buf_size / 2);
	snd_pcm_sw_params(pcm, sw);

	int play_period = (int)pf;
	int play_channels = channels;
	int acc_frames_needed = play_period; /* frames to accumulate */
	int acc_size = play_channels * acc_frames_needed * 2; /* 2x for safety */
	float *acc_buf = calloc(acc_size, sizeof(float));
	int16_t *out_buf = malloc(play_channels * play_period * sizeof(int16_t));
	int acc_pos = 0; /* interleaved samples accumulated */
	int cap_slot = channels * period_frames;

	printf("ALSA playback: %s, %u Hz, %u ch, period %d, buffer %lu "
	       "(%.1f ms), %u periods\n",
	       playback_device, prate, channels, play_period,
	       buf_size, playback_latency_ms, nperiods);
	printf("Playback: cap=%d → play=%d frames "
	       "(accumulate %d capture blocks per write)\n",
	       (int)period_frames, play_period,
	       (play_period + (int)period_frames - 1) / (int)period_frames);

	/* Pre-fill playback buffer with silence to prevent initial underruns */
	{
		int16_t *silence = calloc(play_channels * play_period,
					  sizeof(int16_t));
		for (unsigned int p = 0; p < nperiods - 1; p++)
			snd_pcm_writei(pcm, silence, play_period);
		free(silence);
	}

	while (running) {
		/* Wait for data */
		pthread_mutex_lock(&play_mtx);
		while (play_ring_read == play_ring_write && running) {
			struct timespec ts;
			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_nsec += 5000000; /* 5ms timeout */
			if (ts.tv_nsec >= 1000000000) {
				ts.tv_sec++;
				ts.tv_nsec -= 1000000000;
			}
			pthread_cond_timedwait(&play_cond, &play_mtx, &ts);
		}
		if (!running) { pthread_mutex_unlock(&play_mtx); break; }

		/* Copy capture block into accumulation buffer */
		float *src = play_ring + play_ring_read * cap_slot;
		memcpy(acc_buf + acc_pos, src, cap_slot * sizeof(float));
		acc_pos += cap_slot;
		play_ring_read = (play_ring_read + 1) % PLAY_RING_SLOTS;
		pthread_mutex_unlock(&play_mtx);

		/* Write full playback periods, keep leftover */
		int out_slot = play_channels * play_period;
		while (acc_pos >= out_slot) {
			for (int i = 0; i < out_slot; i++) {
				float s = acc_buf[i];
				if (s > 1.0f) s = 1.0f;
				if (s < -1.0f) s = -1.0f;
				out_buf[i] = (int16_t)(s * 32767);
			}
			snd_pcm_sframes_t n = snd_pcm_writei(pcm, out_buf,
							      play_period);
			if (n < 0) {
				n = snd_pcm_recover(pcm, (int)n, 0);
				if (n < 0)
					fprintf(stderr, "ALSA write: %s\n",
						snd_strerror((int)n));
			}
			/* Shift leftover to start */
			int leftover = acc_pos - out_slot;
			if (leftover > 0)
				memmove(acc_buf, acc_buf + out_slot,
					leftover * sizeof(float));
			acc_pos = leftover;
		}
	}

	free(acc_buf);
	free(out_buf);
	snd_pcm_drain(pcm);
	snd_pcm_close(pcm);
	return NULL;
}

static void playback_feed(const float *ch1, const float *ch2, int n)
{
	if (!playback_active || !playback_device) return;
	int slot_size = channels * n;

	pthread_mutex_lock(&play_mtx);
	float *dst = play_ring + play_ring_write * slot_size;

	/* Interleave channels */
	if (channels == 2 && ch2) {
		for (int i = 0; i < n; i++) {
			dst[i * 2]     = ch1[i];
			dst[i * 2 + 1] = ch2[i];
		}
	} else {
		memcpy(dst, ch1, n * sizeof(float));
	}

	play_ring_write = (play_ring_write + 1) % PLAY_RING_SLOTS;
	if (play_ring_write == play_ring_read)
		play_ring_read = (play_ring_read + 1) % PLAY_RING_SLOTS;
	pthread_cond_signal(&play_cond);
	pthread_mutex_unlock(&play_mtx);
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

	SDL_SetRenderDrawColor(r, 60, 60, 60, 255);
	SDL_Rect border = { x0, y0, w, h };
	SDL_RenderDrawRect(r, &border);

	SDL_SetRenderDrawColor(r, 40, 40, 40, 255);
	SDL_RenderDrawLine(r, x0, mid, x0 + w, mid);

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

		int py_min = mid - (int)(vmax * h / 2);
		int py_max = mid - (int)(vmin * h / 2);
		if (py_min < ytop) py_min = ytop;
		if (py_max > ybot) py_max = ybot;
		if (py_min > ybot) py_min = ybot;
		if (py_max < ytop) py_max = ytop;

		if (i > 0) {
			if (prev_y < py_min)
				SDL_RenderDrawLine(r, x0 + i, prev_y, x0 + i, py_min);
			else if (prev_y > py_max)
				SDL_RenderDrawLine(r, x0 + i, py_max, x0 + i, prev_y);
		}

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
	SDL_SetRenderDrawColor(r, 60, 60, 60, 255);
	SDL_Rect border = { x0, y0, w, h };
	SDL_RenderDrawRect(r, &border);

	float db_range = db_ceil - db_floor;
	float nyquist = srate / 2.0f;
	/* Cap display at 20 kHz — content above is inaudible */
	float max_freq = nyquist < 20000.0f ? nyquist : 20000.0f;
	int max_bin = (int)(max_freq / nyquist * (bins - 1));
	if (max_bin >= bins) max_bin = bins - 1;

	for (int i = 0; i < w; i++) {
		int bin = i * max_bin / w;
		float norm = (mag_db[bin] - db_floor) / db_range;
		if (norm < 0) norm = 0;
		if (norm > 1) norm = 1;
		int bar_h = (int)(norm * h);

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

	float freq_ticks[] = {100, 500, 1000, 2000, 5000, 10000, 20000};
	int n_ticks = sizeof(freq_ticks) / sizeof(freq_ticks[0]);
	for (int t = 0; t < n_ticks; t++) {
		if (freq_ticks[t] > max_freq) break;
		int px = (int)(freq_ticks[t] / max_freq * w);
		if (px < 0 || px >= w) continue;

		SDL_SetRenderDrawColor(r, 80, 80, 80, 255);
		SDL_RenderDrawLine(r, x0 + px, y0, x0 + px, y0 + h);

		char lbl[16];
		if (freq_ticks[t] >= 1000)
			snprintf(lbl, sizeof(lbl), "%dk", (int)(freq_ticks[t] / 1000));
		else
			snprintf(lbl, sizeof(lbl), "%d", (int)freq_ticks[t]);

		SDL_SetRenderDrawColor(r, 120, 130, 140, 255);
		draw_text(r, lbl, x0 + px + 2, y0 + 2, 1);
	}
}

static void draw_peak_hold(SDL_Renderer *r, const float *peak_db, int bins,
			   int x0, int y0, int w, int h,
			   float db_floor, float db_ceil,
			   unsigned int srate)
{
	float nyquist = srate / 2.0f;
	float max_freq = nyquist < 20000.0f ? nyquist : 20000.0f;
	int max_bin = (int)(max_freq / nyquist * (bins - 1));
	if (max_bin >= bins) max_bin = bins - 1;
	float db_range = db_ceil - db_floor;
	SDL_SetRenderDrawColor(r, 255, 255, 255, 200);
	SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
	for (int i = 0; i < w; i++) {
		int bin = i * max_bin / w;
		float norm = (peak_db[bin] - db_floor) / db_range;
		if (norm < 0) norm = 0;
		if (norm > 1) norm = 1;
		int py = y0 + h - (int)(norm * h);
		SDL_RenderDrawPoint(r, x0 + i, py);
		SDL_RenderDrawPoint(r, x0 + i, py + 1);
	}
	SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}

static void draw_spectrogram_col(SDL_Texture *tex, const float *mag_db,
				 int bins, int col, int tex_h,
				 float db_floor, float db_ceil)
{
	float db_range = db_ceil - db_floor;
	Uint32 pixels[512];
	int draw_h = tex_h < 512 ? tex_h : 512;

	for (int row = 0; row < draw_h; row++) {
		int bin = (draw_h - 1 - row) * bins / draw_h;
		float norm = (mag_db[bin] - db_floor) / db_range;
		if (norm < 0) norm = 0;
		if (norm > 1) norm = 1;

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
	(void)label;
	SDL_SetRenderDrawColor(r, 60, 60, 60, 255);
	SDL_Rect border = { x0, y0, w, h };
	SDL_RenderDrawRect(r, &border);

	float rms_db = 20.0f * log10f(rms + 1e-10f);
	float norm = (rms_db + 60) / 60;
	if (norm < 0) norm = 0;
	if (norm > 1) norm = 1;
	int bar_w = (int)(norm * (w - 2));
	SDL_SetRenderDrawColor(r, 0, 180, 0, 255);
	SDL_Rect rms_bar = { x0 + 1, y0 + 1, bar_w, h - 2 };
	SDL_RenderFillRect(r, &rms_bar);

	float peak_db = 20.0f * log10f(peak + 1e-10f);
	float pnorm = (peak_db + 60) / 60;
	if (pnorm < 0) pnorm = 0;
	if (pnorm > 1) pnorm = 1;
	int peak_x = x0 + 1 + (int)(pnorm * (w - 2));
	SDL_SetRenderDrawColor(r, 255, 50, 50, 255);
	SDL_RenderDrawLine(r, peak_x, y0 + 1, peak_x, y0 + h - 1);
}

#define DIR_TRACE_LEN 32

typedef struct {
	float angle;
	float conf;
} dir_sample_t;

static void draw_direction_indicator(SDL_Renderer *r, float angle_deg,
				     float confidence,
				     const dir_sample_t *trace, int trace_len,
				     int trace_pos, int gate_open,
				     int cx, int cy, int radius)
{
	/* Dim circle when gate is closed */
	if (!gate_open)
		SDL_SetRenderDrawColor(r, 30, 30, 30, 255);
	else
		SDL_SetRenderDrawColor(r, 60, 60, 60, 255);
	for (int a = 0; a < 360; a++) {
		float rad = a * M_PI / 180.0f;
		int px = cx + (int)(radius * cosf(rad));
		int py = cy - (int)(radius * sinf(rad));
		SDL_RenderDrawPoint(r, px, py);
	}

	/* Fading trail */
	SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
	for (int i = 0; i < trace_len; i++) {
		int idx = (trace_pos + i) % trace_len;
		float a = trace[idx].angle;
		float c = trace[idx].conf;
		if (c < 0.05f) continue;

		float t = (float)(i + 1) / trace_len;
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

	for (int dx = -3; dx <= 3; dx++)
		for (int dy = -3; dy <= 3; dy++)
			SDL_RenderDrawPoint(r, ex + dx, ey + dy);
}

/* ── On-screen touch buttons ────────────────────────────────────────── */

#define BTN_COUNT 6
#define BTN_H     36

typedef struct {
	const char *label;
	int active;
	Uint8 r, g, b;      /* accent color */
	float x0, x1;        /* normalized touch coords */
} touch_btn_t;

static touch_btn_t buttons[BTN_COUNT] = {
	{ "REC",  0, 255, 60, 60,   0.0f, 0.0f },
	{ "FLT",  0, 60, 200, 255,  0.0f, 0.0f },
	{ "GATE", 1, 60, 255, 120,  0.0f, 0.0f },
	{ "EQ",   0, 255, 200, 50,  0.0f, 0.0f },
	{ "PLAY", 0, 50, 255, 50,   0.0f, 0.0f },
	{ "TDOA", 0, 255, 130, 255, 0.0f, 0.0f },
};

static int tdoa_overlay = 0;

static void layout_buttons(int win_w, int win_h)
{
	(void)win_h;
	int margin = 10;
	int spacing = 6;
	int total_w = win_w - 2 * margin;
	int btn_w = (total_w - (BTN_COUNT - 1) * spacing) / BTN_COUNT;

	for (int i = 0; i < BTN_COUNT; i++) {
		int bx = margin + i * (btn_w + spacing);
		buttons[i].x0 = (float)bx / win_w;
		buttons[i].x1 = (float)(bx + btn_w) / win_w;
	}
}

static void draw_buttons(SDL_Renderer *r, int win_w, int win_h)
{
	int margin = 10;
	int spacing = 6;
	int total_w = win_w - 2 * margin;
	int btn_w = (total_w - (BTN_COUNT - 1) * spacing) / BTN_COUNT;
	int by = win_h - BTN_H - 4;

	for (int i = 0; i < BTN_COUNT; i++) {
		int bx = margin + i * (btn_w + spacing);
		SDL_Rect rect = { bx, by, btn_w, BTN_H };

		if (buttons[i].active) {
			SDL_SetRenderDrawColor(r, buttons[i].r, buttons[i].g,
					       buttons[i].b, 200);
			SDL_RenderFillRect(r, &rect);
			SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
		} else {
			SDL_SetRenderDrawColor(r, 40, 40, 45, 255);
			SDL_RenderFillRect(r, &rect);
			SDL_SetRenderDrawColor(r, buttons[i].r, buttons[i].g,
					       buttons[i].b, 255);
			SDL_RenderDrawRect(r, &rect);
		}

		/* Center label — use scale 2, fall back to 1 if too wide */
		int scale = 2;
		int tw = strlen(buttons[i].label) * 6 * scale; /* 5px char + 1px gap */
		if (tw > btn_w - 4) scale = 1;
		tw = strlen(buttons[i].label) * 6 * scale;
		int tx = bx + (btn_w - tw) / 2;
		int ty = by + (BTN_H - 7 * scale) / 2;
		draw_text(r, buttons[i].label, tx, ty, scale);
	}
}

static int hit_test_button(float fx, float fy, int win_h)
{
	/* Button bar is at the bottom BTN_H+4 pixels */
	float btn_top = (float)(win_h - BTN_H - 4) / win_h;
	if (fy < btn_top) return -1;

	for (int i = 0; i < BTN_COUNT; i++) {
		if (fx >= buttons[i].x0 && fx <= buttons[i].x1)
			return i;
	}
	return -1;
}

/* ── EQ overlay ────────────────────────────────────────────────────── */

static int eq_dragging = -1; /* band index being dragged, -1 = none */

/* Close button: 32x32 X icon in top-right corner of overlay */
#define CLOSE_BTN_SIZE 32
#define CLOSE_BTN_MARGIN 6

static void draw_close_button(SDL_Renderer *r, int ox, int oy, int ow)
{
	int bx = ox + ow - CLOSE_BTN_SIZE - CLOSE_BTN_MARGIN;
	int by = oy + CLOSE_BTN_MARGIN;
	int bs = CLOSE_BTN_SIZE;

	/* Background circle */
	SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(r, 255, 60, 60, 180);
	for (int dy = -bs/2; dy <= bs/2; dy++) {
		int dx = (int)sqrtf((bs/2.0f)*(bs/2.0f) - dy*dy);
		SDL_RenderDrawLine(r, bx + bs/2 - dx, by + bs/2 + dy,
				   bx + bs/2 + dx, by + bs/2 + dy);
	}
	SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

	/* X lines */
	int p = 8; /* padding inside circle */
	SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
	for (int t = -1; t <= 1; t++) {
		SDL_RenderDrawLine(r, bx + p + t, by + p,
				   bx + bs - p + t, by + bs - p);
		SDL_RenderDrawLine(r, bx + bs - p + t, by + p,
				   bx + p + t, by + bs - p);
	}
}

static int close_button_hit(float fx, float fy, int win_w, int win_h)
{
	int margin = 30;
	int ox = margin, oy = margin;
	int ow = win_w - 2 * margin;
	int bx = ox + ow - CLOSE_BTN_SIZE - CLOSE_BTN_MARGIN;
	int by = oy + CLOSE_BTN_MARGIN;
	int px = (int)(fx * win_w);
	int py = (int)(fy * win_h);
	int cx = bx + CLOSE_BTN_SIZE / 2;
	int cy = by + CLOSE_BTN_SIZE / 2;
	int dx = px - cx, dy = py - cy;
	return (dx * dx + dy * dy) <= (CLOSE_BTN_SIZE / 2 + 8) *
				      (CLOSE_BTN_SIZE / 2 + 8);
}

static void draw_eq_overlay(SDL_Renderer *r, int win_w, int win_h)
{
	int margin = 30;
	int ox = margin, oy = margin;
	int ow = win_w - 2 * margin;
	int oh = win_h - 2 * margin;

	/* Semi-transparent background */
	SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(r, 10, 10, 15, 220);
	SDL_Rect bg = { ox, oy, ow, oh };
	SDL_RenderFillRect(r, &bg);
	SDL_SetRenderDrawColor(r, 80, 80, 100, 255);
	SDL_RenderDrawRect(r, &bg);
	SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

	/* Title + close button */
	SDL_SetRenderDrawColor(r, 200, 200, 220, 255);
	draw_text(r, "EQUALIZER", ox + 10, oy + 8, 2);
	draw_close_button(r, ox, oy, ow);

	/* Band sliders area */
	int slider_top = oy + 40;
	int slider_bot = oy + oh - 70;
	int slider_h = slider_bot - slider_top;
	int slider_mid = slider_top + slider_h / 2;
	int band_w = (ow - 40) / (EQ_BANDS + 2); /* extra space for delay */
	int bx0 = ox + 20;

	/* Zero line */
	SDL_SetRenderDrawColor(r, 50, 50, 60, 255);
	SDL_RenderDrawLine(r, ox + 10, slider_mid, ox + ow - 10, slider_mid);

	/* Grid lines at +6, -6, +12, -12 dB */
	SDL_SetRenderDrawColor(r, 35, 35, 45, 255);
	for (float db = -12; db <= 12; db += 6) {
		if (fabsf(db) < 0.1f) continue;
		int gy = slider_mid - (int)(db / EQ_MAX_DB * slider_h / 2);
		SDL_RenderDrawLine(r, ox + 10, gy, ox + ow - 10, gy);
	}

	/* dB labels */
	SDL_SetRenderDrawColor(r, 90, 90, 100, 255);
	char lbl[8];
	snprintf(lbl, sizeof(lbl), "+%d", (int)EQ_MAX_DB);
	draw_text(r, lbl, ox + 2, slider_top - 2, 1);
	snprintf(lbl, sizeof(lbl), "-%d", (int)EQ_MAX_DB);
	draw_text(r, lbl, ox + 2, slider_bot - 6, 1);
	draw_text(r, "0dB", ox + 2, slider_mid - 3, 1);

	/* EQ band sliders */
	for (int b = 0; b < EQ_BANDS; b++) {
		int cx = bx0 + b * band_w + band_w / 2;
		float norm = eq_bands[b].gain_db / EQ_MAX_DB; /* -1..+1 */
		int bar_top = slider_mid;
		int bar_bot = slider_mid - (int)(norm * slider_h / 2);
		if (bar_top > bar_bot) { int t = bar_top; bar_top = bar_bot; bar_bot = t; }

		/* Slider track */
		SDL_SetRenderDrawColor(r, 50, 50, 60, 255);
		SDL_RenderDrawLine(r, cx, slider_top, cx, slider_bot);

		/* Bar fill */
		Uint8 cr = norm > 0 ? 80 : 50;
		Uint8 cg = norm > 0 ? 200 : 120;
		Uint8 cb = 255;
		SDL_SetRenderDrawColor(r, cr, cg, cb, 200);
		SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
		int bw = band_w - 8;
		if (bw < 6) bw = 6;
		SDL_Rect bar = { cx - bw/2, bar_top, bw, bar_bot - bar_top };
		SDL_RenderFillRect(r, &bar);
		SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

		/* Thumb */
		int ty = slider_mid - (int)(norm * slider_h / 2);
		SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
		SDL_Rect thumb = { cx - bw/2, ty - 3, bw, 6 };
		SDL_RenderFillRect(r, &thumb);

		/* Frequency label */
		SDL_SetRenderDrawColor(r, 160, 170, 180, 255);
		char flbl[8];
		if (eq_bands[b].freq >= 1000)
			snprintf(flbl, sizeof(flbl), "%dk",
				 (int)(eq_bands[b].freq / 1000));
		else
			snprintf(flbl, sizeof(flbl), "%d",
				 (int)eq_bands[b].freq);
		int tw = strlen(flbl) * 6;
		draw_text(r, flbl, cx - tw / 2, slider_bot + 6, 1);

		/* Gain label */
		snprintf(flbl, sizeof(flbl), "%+.0f",
			 eq_bands[b].gain_db);
		tw = strlen(flbl) * 6;
		SDL_SetRenderDrawColor(r, 140, 160, 180, 255);
		draw_text(r, flbl, cx - tw / 2, slider_bot + 16, 1);
	}

	/* Gain slider */
	{
		int cx = bx0 + EQ_BANDS * band_w + band_w / 2;
		float norm = (gain - 1.0f) / 15.0f; /* 1..16 mapped to 0..1 */
		if (norm < 0) norm = 0;
		if (norm > 1) norm = 1;
		int bar_h = (int)(norm * slider_h);

		SDL_SetRenderDrawColor(r, 50, 50, 60, 255);
		SDL_RenderDrawLine(r, cx, slider_top, cx, slider_bot);

		SDL_SetRenderDrawColor(r, 50, 200, 50, 180);
		SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
		int bw = band_w - 8;
		if (bw < 6) bw = 6;
		SDL_Rect bar = { cx - bw/2, slider_bot - bar_h, bw, bar_h };
		SDL_RenderFillRect(r, &bar);
		SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

		int ty = slider_bot - bar_h;
		SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
		SDL_Rect thumb = { cx - bw/2, ty - 3, bw, 6 };
		SDL_RenderFillRect(r, &thumb);

		SDL_SetRenderDrawColor(r, 160, 170, 180, 255);
		draw_text(r, "GAIN", cx - 12, slider_bot + 6, 1);

		char glbl[8];
		snprintf(glbl, sizeof(glbl), "%.1f", gain);
		int tw = strlen(glbl) * 6;
		draw_text(r, glbl, cx - tw / 2, slider_bot + 16, 1);
	}

	/* Delay slider */
	{
		int cx = bx0 + (EQ_BANDS + 1) * band_w + band_w / 2;
		float norm = delay_ms / 1000.0f; /* 0..1 */
		int bar_h = (int)(norm * slider_h);

		SDL_SetRenderDrawColor(r, 50, 50, 60, 255);
		SDL_RenderDrawLine(r, cx, slider_top, cx, slider_bot);

		SDL_SetRenderDrawColor(r, 255, 180, 50, 180);
		SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
		int bw = band_w - 8;
		if (bw < 6) bw = 6;
		SDL_Rect bar = { cx - bw/2, slider_bot - bar_h, bw, bar_h };
		SDL_RenderFillRect(r, &bar);
		SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

		/* Thumb */
		int ty = slider_bot - bar_h;
		SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
		SDL_Rect thumb = { cx - bw/2, ty - 3, bw, 6 };
		SDL_RenderFillRect(r, &thumb);

		SDL_SetRenderDrawColor(r, 160, 170, 180, 255);
		draw_text(r, "DELAY", cx - 15, slider_bot + 6, 1);

		char dlbl[16];
		snprintf(dlbl, sizeof(dlbl), "%dms", (int)delay_ms);
		int tw = strlen(dlbl) * 6;
		draw_text(r, dlbl, cx - tw / 2, slider_bot + 16, 1);
	}

	/* Pitch preset buttons */
	{
		int py = oy + oh - 65;
		const char *presets[] = {
			"Normal", "Chipmunk", "Deep", "Robot"
		};
		int n_presets = FX_COUNT;
		int pbw = (ow - 40) / n_presets;

		for (int i = 0; i < n_presets; i++) {
			int px = ox + 20 + i * pbw;
			SDL_Rect pr = { px, py, pbw - 4, 22 };

			int is_active = (fx_mode == i);

			if (is_active) {
				SDL_SetRenderDrawColor(r, 200, 160, 50, 200);
				SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
				SDL_RenderFillRect(r, &pr);
				SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
				SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
			} else {
				SDL_SetRenderDrawColor(r, 50, 50, 60, 255);
				SDL_RenderFillRect(r, &pr);
				SDL_SetRenderDrawColor(r, 160, 160, 180, 255);
				SDL_RenderDrawRect(r, &pr);
			}
			int tw = strlen(presets[i]) * 6;
			draw_text(r, presets[i], px + (pbw - 4 - tw) / 2,
				  py + 5, 1);
		}

		/* Store rects for hit testing */
		SDL_SetRenderDrawColor(r, 100, 100, 120, 255);
		draw_text(r, "VOICE FX:", ox + 2, py + 5, 1);
	}

	/* Status line at bottom */
	int sy = oy + oh - 30;
	SDL_SetRenderDrawColor(r, 140, 160, 180, 255);
	char status[96];

	{
		float cap_ms = (float)period_frames / sample_rate * 1000.0f;
		if (playback_active) {
			snprintf(status, sizeof(status),
				 "Cap:%.0fms+Buf:%.0fms=%.0fms  FX:%s",
				 cap_ms, playback_latency_ms,
				 pipeline_latency_ms,
				 fx_names[fx_mode]);
		} else {
			snprintf(status, sizeof(status),
				 "Period:%d (%.0fms)  EQ:%s  Delay:%s",
				 (int)period_frames, cap_ms,
				 eq_enabled ? "ON" : "OFF",
				 delay_enabled ? "ON" : "OFF");
		}
	}
	draw_text(r, status, ox + 10, sy, 2);
}

/* Hit test returns:
 *   0..7            = EQ band slider
 *   EQ_BANDS (8)    = gain slider
 *   EQ_BANDS+1 (9)  = delay slider
 *   EQ_HIT_PITCH_BASE+0..3 = pitch presets
 *   -1              = nothing */
#define EQ_HIT_GAIN     EQ_BANDS
#define EQ_HIT_DELAY    (EQ_BANDS + 1)
#define EQ_HIT_PITCH_BASE (EQ_BANDS + 2)
/* FX presets are selected by index matching FX_NONE..FX_ROBOT */

static int eq_hit_test(float fx, float fy, int win_w, int win_h)
{
	int margin = 30;
	int ox = margin, oy = margin;
	int ow = win_w - 2 * margin;
	int oh = win_h - 2 * margin;
	int slider_top = oy + 40;
	int slider_bot = oy + oh - 70;
	int band_w = (ow - 40) / (EQ_BANDS + 2);
	int bx0 = ox + 20;

	int px = (int)(fx * win_w);
	int py = (int)(fy * win_h);

	/* Pitch preset buttons */
	int pitch_y = oy + oh - 65;
	if (py >= pitch_y && py <= pitch_y + 22) {
		int pbw = (ow - 40) / 4;
		for (int i = 0; i < 4; i++) {
			int bx = ox + 20 + i * pbw;
			if (px >= bx && px < bx + pbw - 4)
				return EQ_HIT_PITCH_BASE + i;
		}
	}

	/* Sliders area */
	if (py >= slider_top - 10 && py <= slider_bot + 10) {
		/* EQ bands */
		for (int b = 0; b < EQ_BANDS; b++) {
			int cx = bx0 + b * band_w + band_w / 2;
			if (abs(px - cx) < band_w / 2)
				return b;
		}
		/* Gain slider */
		int gcx = bx0 + EQ_BANDS * band_w + band_w / 2;
		if (abs(px - gcx) < band_w / 2)
			return EQ_HIT_GAIN;
		/* Delay slider */
		int dcx = bx0 + (EQ_BANDS + 1) * band_w + band_w / 2;
		if (abs(px - dcx) < band_w / 2)
			return EQ_HIT_DELAY;
	}

	return -1;
}

static void eq_drag_update(float fy, int win_h)
{
	int margin = 30;
	int oy = margin;
	int oh = win_h - 2 * margin;
	int slider_top = oy + 40;
	int slider_bot = oy + oh - 70;
	int slider_h = slider_bot - slider_top;
	int slider_mid = slider_top + slider_h / 2;

	int py = (int)(fy * win_h);

	if (eq_dragging >= 0 && eq_dragging < EQ_BANDS) {
		/* EQ band: map pixel to dB */
		float norm = (float)(slider_mid - py) / (slider_h / 2.0f);
		if (norm > 1) norm = 1;
		if (norm < -1) norm = -1;
		eq_bands[eq_dragging].gain_db = norm * EQ_MAX_DB;
		eq_update_coeffs();
		eq_enabled = 1;
	} else if (eq_dragging == EQ_HIT_GAIN) {
		/* Gain slider: map pixel to 1.0–16.0 */
		float norm = (float)(slider_bot - py) / slider_h;
		if (norm < 0) norm = 0;
		if (norm > 1) norm = 1;
		gain = 1.0f + norm * 15.0f;
	} else if (eq_dragging == EQ_HIT_DELAY) {
		/* Delay slider: map pixel to ms (0-1000) */
		float norm = (float)(slider_bot - py) / slider_h;
		if (norm < 0) norm = 0;
		if (norm > 1) norm = 1;
		delay_ms = norm * 1000.0f;
		delay_len = (int)(delay_ms / 1000.0f * sample_rate);
		if (delay_len >= MAX_DELAY_SAMPLES)
			delay_len = MAX_DELAY_SAMPLES - 1;
		delay_enabled = (delay_ms > 5.0f);
	}
}

/* ── TDOA visualization overlay ─────────────────────────────────────── */

static void draw_tdoa_overlay(SDL_Renderer *r, int win_w, int win_h,
			      const float *ch1, const float *ch2,
			      const float *corr_buf, int fft_n,
			      int max_lag_samples, float delay_samp,
			      float angle, float conf)
{
	int margin = 30;
	int ox = margin, oy = margin;
	int ow = win_w - 2 * margin;
	int oh = win_h - 2 * margin;

	/* Semi-transparent background */
	SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(r, 10, 10, 15, 220);
	SDL_Rect bg = { ox, oy, ow, oh };
	SDL_RenderFillRect(r, &bg);
	SDL_SetRenderDrawColor(r, 80, 80, 100, 255);
	SDL_RenderDrawRect(r, &bg);
	SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

	/* Title */
	SDL_SetRenderDrawColor(r, 200, 200, 220, 255);
	draw_text(r, "TDOA VISUALIZATION", ox + 10, oy + 8, 2);
	draw_close_button(r, ox, oy, ow);

	if (!ch1 || !ch2 || !corr_buf) {
		SDL_SetRenderDrawColor(r, 140, 140, 160, 255);
		draw_text(r, "Stereo mode required (-c 2)",
			  ox + ow / 2 - 100, oy + oh / 2, 2);
		return;
	}

	int pad = 15;
	int inner_w = ow - 2 * pad;

	/* ── Top panel: overlaid channel waveforms ──────────────── */

	int wave_oy = oy + 35;
	int wave_h = (oh - 100) / 3;
	int n = (int)period_frames;

	/* Border + zero line */
	SDL_SetRenderDrawColor(r, 50, 50, 60, 255);
	SDL_Rect wb = { ox + pad, wave_oy, inner_w, wave_h };
	SDL_RenderDrawRect(r, &wb);
	int wmid = wave_oy + wave_h / 2;
	SDL_SetRenderDrawColor(r, 35, 35, 45, 255);
	SDL_RenderDrawLine(r, ox + pad, wmid, ox + pad + inner_w, wmid);

	/* Channel 1 — cyan */
	SDL_SetRenderDrawColor(r, 0, 200, 255, 255);
	for (int i = 1; i < inner_w; i++) {
		int s0 = (i - 1) * n / inner_w;
		int s1 = i * n / inner_w;
		if (s0 >= n) s0 = n - 1;
		if (s1 >= n) s1 = n - 1;
		int y0 = wmid - (int)(ch1[s0] * wave_h / 2);
		int y1 = wmid - (int)(ch1[s1] * wave_h / 2);
		if (y0 < wave_oy) y0 = wave_oy;
		if (y0 > wave_oy + wave_h) y0 = wave_oy + wave_h;
		if (y1 < wave_oy) y1 = wave_oy;
		if (y1 > wave_oy + wave_h) y1 = wave_oy + wave_h;
		SDL_RenderDrawLine(r, ox + pad + i - 1, y0,
				   ox + pad + i, y1);
	}

	/* Channel 2 — orange */
	SDL_SetRenderDrawColor(r, 255, 150, 0, 255);
	for (int i = 1; i < inner_w; i++) {
		int s0 = (i - 1) * n / inner_w;
		int s1 = i * n / inner_w;
		if (s0 >= n) s0 = n - 1;
		if (s1 >= n) s1 = n - 1;
		int y0 = wmid - (int)(ch2[s0] * wave_h / 2);
		int y1 = wmid - (int)(ch2[s1] * wave_h / 2);
		if (y0 < wave_oy) y0 = wave_oy;
		if (y0 > wave_oy + wave_h) y0 = wave_oy + wave_h;
		if (y1 < wave_oy) y1 = wave_oy;
		if (y1 > wave_oy + wave_h) y1 = wave_oy + wave_h;
		SDL_RenderDrawLine(r, ox + pad + i - 1, y0,
				   ox + pad + i, y1);
	}

	/* Label */
	SDL_SetRenderDrawColor(r, 0, 200, 255, 255);
	draw_text(r, "CH1", ox + pad + 3, wave_oy + 3, 1);
	SDL_SetRenderDrawColor(r, 255, 150, 0, 255);
	draw_text(r, "CH2", ox + pad + 30, wave_oy + 3, 1);

	/* Lag annotation in samples/microseconds */
	{
		char lagstr[48];
		float lag_us = delay_samp / sample_rate * 1e6f;
		snprintf(lagstr, sizeof(lagstr),
			 "Delay: %.1f samples  %.1f us",
			 delay_samp, lag_us);
		SDL_SetRenderDrawColor(r, 180, 180, 200, 255);
		draw_text(r, lagstr, ox + pad + inner_w - 280,
			  wave_oy + 3, 1);
	}

	/* ── Middle panel: GCC-PHAT correlation ────────────────── */

	int corr_oy = wave_oy + wave_h + 15;
	int corr_h = wave_h;

	SDL_SetRenderDrawColor(r, 50, 50, 60, 255);
	SDL_Rect cb = { ox + pad, corr_oy, inner_w, corr_h };
	SDL_RenderDrawRect(r, &cb);
	int cmid = corr_oy + corr_h / 2;

	/* Zero line */
	SDL_SetRenderDrawColor(r, 35, 35, 45, 255);
	SDL_RenderDrawLine(r, ox + pad, cmid, ox + pad + inner_w, cmid);

	/* Zero-lag marker (center) */
	int center_x = ox + pad + inner_w / 2;
	SDL_SetRenderDrawColor(r, 50, 50, 70, 255);
	SDL_RenderDrawLine(r, center_x, corr_oy, center_x, corr_oy + corr_h);

	/* Plot correlation: show -max_lag to +max_lag region
	 * corr[0..max_lag-1] = positive lags
	 * corr[fft_n-max_lag..fft_n-1] = negative lags */
	int display_lags = max_lag_samples * 2;

	/* Find max for scaling */
	float corr_max = 0;
	for (int i = 0; i < max_lag_samples; i++) {
		float v = fabsf(corr_buf[i]);
		if (v > corr_max) corr_max = v;
	}
	for (int i = fft_n - max_lag_samples; i < fft_n; i++) {
		float v = fabsf(corr_buf[i]);
		if (v > corr_max) corr_max = v;
	}
	if (corr_max < 1e-10f) corr_max = 1.0f;

	/* Draw the correlation curve — green */
	SDL_SetRenderDrawColor(r, 50, 255, 100, 255);
	int prev_cy = cmid;
	for (int i = 0; i < inner_w; i++) {
		/* Map pixel to lag index (-max_lag .. +max_lag) */
		float lag_f = (float)i / inner_w * display_lags -
			      max_lag_samples;
		int lag_i = (int)roundf(lag_f);
		int idx;
		if (lag_i >= 0)
			idx = lag_i < fft_n ? lag_i : fft_n - 1;
		else
			idx = fft_n + lag_i;
		if (idx < 0) idx = 0;
		if (idx >= fft_n) idx = fft_n - 1;

		float val = corr_buf[idx] / corr_max;
		int cy = cmid - (int)(val * corr_h / 2 * 0.9f);
		if (cy < corr_oy) cy = corr_oy;
		if (cy > corr_oy + corr_h) cy = corr_oy + corr_h;

		if (i > 0)
			SDL_RenderDrawLine(r, ox + pad + i - 1, prev_cy,
					   ox + pad + i, cy);
		prev_cy = cy;
	}

	/* Peak marker — yellow vertical line at detected delay */
	{
		float peak_px_f = (delay_samp + max_lag_samples) /
				  display_lags * inner_w;
		int peak_px = ox + pad + (int)peak_px_f;
		if (peak_px > ox + pad && peak_px < ox + pad + inner_w) {
			SDL_SetRenderDrawColor(r, 255, 255, 0, 255);
			SDL_RenderDrawLine(r, peak_px, corr_oy + 2,
					   peak_px, corr_oy + corr_h - 2);
			/* Arrow head at top */
			SDL_RenderDrawLine(r, peak_px - 4, corr_oy + 8,
					   peak_px, corr_oy + 2);
			SDL_RenderDrawLine(r, peak_px + 4, corr_oy + 8,
					   peak_px, corr_oy + 2);

			/* Label above: delay value */
			char plbl[24];
			snprintf(plbl, sizeof(plbl), "%.1f", delay_samp);
			draw_text(r, plbl, peak_px + 6, corr_oy + 3, 1);
		}
	}

	/* Lag axis labels */
	SDL_SetRenderDrawColor(r, 120, 120, 140, 255);
	{
		char ll[16];
		snprintf(ll, sizeof(ll), "-%d", max_lag_samples);
		draw_text(r, ll, ox + pad + 2, corr_oy + corr_h - 10, 1);
		snprintf(ll, sizeof(ll), "+%d", max_lag_samples);
		draw_text(r, ll, ox + pad + inner_w - 24,
			  corr_oy + corr_h - 10, 1);
		draw_text(r, "0", center_x + 2,
			  corr_oy + corr_h - 10, 1);
		draw_text(r, "GCC-PHAT Cross-Correlation",
			  ox + pad + 3, corr_oy + 3, 1);
		draw_text(r, "lag (samples)", center_x - 36,
			  corr_oy + corr_h - 10, 1);
	}

	/* ── Bottom panel: info ────────────────────────────────── */

	int info_y = corr_oy + corr_h + 20;
	SDL_SetRenderDrawColor(r, 160, 170, 190, 255);

	char info1[80], info2[80], info3[80];
	float tau_us = delay_samp / sample_rate * 1e6f;
	float distance_cm = fabsf(delay_samp) / sample_rate *
			    SPEED_OF_SOUND * 100.0f;

	snprintf(info1, sizeof(info1),
		 "Delay: %+.2f samples  %+.1f us  "
		 "Path diff: %.2f cm",
		 delay_samp, tau_us, distance_cm);
	snprintf(info2, sizeof(info2),
		 "Angle: %.1f deg  Confidence: %.2f  "
		 "Mic spacing: %.1f cm",
		 angle, conf, mic_distance * 100);
	snprintf(info3, sizeof(info3),
		 "Max lag: %d samples (%.1f cm / %.0f m/s / %u Hz)",
		 max_lag_samples,
		 mic_distance * 100, SPEED_OF_SOUND, sample_rate);

	draw_text(r, info1, ox + pad, info_y, 2);
	draw_text(r, info2, ox + pad, info_y + 20, 2);
	draw_text(r, info3, ox + pad, info_y + 40, 1);
}

/* ── 5x7 rounded font ──────────────────────────────────────────────── */

#define FONT_W 5
#define FONT_H 7

static const uint8_t font5x7[128][FONT_H] = {
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
	['+'] = {0x00,0x04,0x04,0x1F,0x04,0x04,0x00},
	[' '] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00},
	['%'] = {0x18,0x19,0x02,0x04,0x08,0x13,0x03},
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
	['O'] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
	['P'] = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
	['R'] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
	['S'] = {0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E},
	['T'] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
	['U'] = {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
	['V'] = {0x11,0x11,0x11,0x11,0x0A,0x0A,0x04},
	['W'] = {0x11,0x11,0x11,0x15,0x15,0x1B,0x11},
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
	['#'] = {0x0A,0x1F,0x0A,0x0A,0x1F,0x0A,0x00},
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
		"  -T dB       Noise gate threshold (default: %.0f dB)\n"
		"  -L Hz       Low-pass cutoff, 0=off (default: %.0f)\n"
		"  -H Hz       High-pass cutoff (default: %.0f)\n"
		"  -o DEVICE   ALSA playback device (default: 'default')\n"
		"  -l          Low-latency mode (512-sample periods, may glitch)\n"
		"  -f          Flip direction 180 deg (mics facing you)\n"
		"  -h          Show this help\n",
		prog, DEFAULT_DEVICE, DEFAULT_RATE, DEFAULT_CHANNELS,
		(unsigned long)DEFAULT_FRAMES, DEFAULT_GAIN,
		DEFAULT_WAVE_MS, DEFAULT_MIC_DIST,
		gate_threshold_db, lp_cutoff, hp_cutoff);
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
	int opt;
	while ((opt = getopt(argc, argv, "d:r:c:n:g:w:m:T:L:H:o:lfh")) != -1) {
		switch (opt) {
		case 'd': alsa_device = optarg; break;
		case 'r': sample_rate = atoi(optarg); break;
		case 'c': channels = atoi(optarg); break;
		case 'n': period_frames = atoi(optarg); break;
		case 'g': gain = atof(optarg); break;
		case 'w': wave_ms = atoi(optarg); break;
		case 'm': mic_distance = atof(optarg); break;
		case 'T': gate_threshold_db = atof(optarg); break;
		case 'L': lp_cutoff = atof(optarg); break;
		case 'H': hp_cutoff = atof(optarg); break;
		case 'o': playback_device = optarg; break;
		case 'l': low_latency = 1; break;
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

	/* Initialize button state */
	buttons[0].active = 0;             /* REC — off */
	buttons[1].active = 0;             /* FILTER (low-pass) — off by default */
	buttons[2].active = gate_enabled;  /* GATE — on by default */
	buttons[3].active = 0;             /* EQ overlay — off */
	buttons[4].active = 0;             /* PLAY — off */
	buttons[5].active = 0;             /* TDOA viz — off */

	/* Low-latency: use smaller capture periods */
	if (low_latency && period_frames > LOWLAT_FRAMES)
		period_frames = LOWLAT_FRAMES;

	/* Allocate ring buffer */
	int slot_size = channels * period_frames;
	ring_buf = calloc(RING_SLOTS * slot_size, sizeof(float));

	/* Start audio thread */
	pthread_t audio_tid;
	pthread_create(&audio_tid, NULL, audio_thread, NULL);

	usleep(200000);
	if (!running) {
		fprintf(stderr, "Audio init failed\n");
		return 1;
	}

	int fft_size = (int)period_frames;
	int half_fft = fft_size / 2 + 1;

	/* FFTW plans */
	float *fft_in = fftwf_alloc_real(fft_size);
	fftwf_complex *fft_out = fftwf_alloc_complex(half_fft);
	fftwf_plan plan_fwd = fftwf_plan_dft_r2c_1d(fft_size, fft_in, fft_out,
						     FFTW_ESTIMATE);

	/* GCC-PHAT plans (stereo only) */
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
	float *peak_db = calloc(half_fft, sizeof(float));
	for (int i = 0; i < half_fft; i++) peak_db[i] = -80.0f;
	float *ch1_buf = calloc(period_frames, sizeof(float));
	float *ch2_buf = channels == 2 ? calloc(period_frames, sizeof(float)) : NULL;
	float *windowed = calloc(fft_size, sizeof(float));

	/* High-pass filter state */
	float hp_y1 = 0, hp_x1 = 0;
	float hp_y2 = 0, hp_x2 = 0;
	float hp_alpha = 1.0f;
	if (hp_cutoff > 0) {
		float rc = 1.0f / (2.0f * M_PI * hp_cutoff);
		float dt = 1.0f / sample_rate;
		hp_alpha = rc / (rc + dt);
	}
	int warmup = 5;

	/* Low-pass filter state */
	float lp_y1 = 0, lp_y2 = 0;
	float lp_alpha_val = 0;
	if (lp_cutoff > 0) {
		float dt = 1.0f / sample_rate;
		float rc = 1.0f / (2.0f * M_PI * lp_cutoff);
		lp_alpha_val = dt / (rc + dt);
	}

	/* Waveform history */
	int wave_samples = (int)((float)wave_ms / 1000.0f * sample_rate);
	if (wave_samples < (int)period_frames) wave_samples = (int)period_frames;
	if (wave_samples > MAX_WAVE_SAMPLES) wave_samples = MAX_WAVE_SAMPLES;
	float *wave_hist1 = calloc(wave_samples, sizeof(float));
	float *wave_hist2 = channels == 2 ? calloc(wave_samples, sizeof(float)) : NULL;
	int wave_pos = 0;
	int wave_filled = 0;

	/* SDL2 init */
	SDL_Init(SDL_INIT_VIDEO);
	if (getenv("HIDE_CURSOR"))
		SDL_ShowCursor(SDL_DISABLE);
	SDL_Window *win = SDL_CreateWindow("Audio Visualizer Full",
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		WIN_W, WIN_H, 0);
	SDL_Renderer *ren = SDL_CreateRenderer(win, -1,
		SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

	/* Spectrogram texture */
	int spec_h = 150;
	SDL_Texture *spec_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
		SDL_TEXTUREACCESS_STREAMING, SPEC_HISTORY, spec_h);
	{
		Uint32 *px = calloc(SPEC_HISTORY * spec_h, sizeof(Uint32));
		SDL_UpdateTexture(spec_tex, NULL, px, SPEC_HISTORY * sizeof(Uint32));
		free(px);
	}
	int spec_col = 0;

	float *spec_avg = channels == 2 ? calloc(half_fft, sizeof(float)) : NULL;
	float peak_smooth = 0;
	float db_floor = -80.0f, db_ceil = 0.0f;
	float delay_samples = 0;
	float angle_deg = 90;
	float confidence = 0;
	int gate_open = 1;

	dir_sample_t dir_trace[DIR_TRACE_LEN];
	memset(dir_trace, 0, sizeof(dir_trace));
	int dir_trace_pos = 0;
	int dir_trace_count = 0;

	float latency_avg = 0;
	float rms_db_avg = -60;
	float dom_freq_avg = 0;
	float note_freq_avg = 0;     /* slow EMA for note display */
	char note_display[16] = "---"; /* held note text */
	int note_hold = 0;            /* frames the current note has been stable */

	int max_lag = (int)(mic_distance / SPEED_OF_SOUND * sample_rate) + 10;
	if (max_lag > fft_size / 2) max_lag = fft_size / 2;

	/* System CPU from /proc/stat */
	float cpu_pct = 0;
	unsigned long prev_total = 0, prev_idle = 0;
	int cpu_poll_count = 0;

	/* Layout buttons */
	layout_buttons(WIN_W, WIN_H);

	/* EQ + delay + pitch init */
	eq_update_coeffs();
	delay_buf_l = calloc(MAX_DELAY_SAMPLES, sizeof(float));
	delay_buf_r = calloc(MAX_DELAY_SAMPLES, sizeof(float));
	delay_len = (int)(delay_ms / 1000.0f * sample_rate);
	/* (pitch buffers removed — effects work in-place now) */

	/* Playback ring buffer + thread */
	int play_slot_size = channels * period_frames;
	play_ring = calloc(PLAY_RING_SLOTS * play_slot_size, sizeof(float));
	pthread_t play_tid = 0;
	if (playback_device) {
		pthread_create(&play_tid, NULL, playback_thread, NULL);
		printf("Playback device: %s\n", playback_device);
	}

	printf("FFT size: %d, bins: %d, max_lag: %d%s\n",
	       fft_size, half_fft, max_lag,
	       low_latency ? " [LOW LATENCY]" : "");
	printf("Capture period: %.1f ms, theoretical min FX latency: %.1f ms\n",
	       (float)period_frames / sample_rate * 1000.0f,
	       (float)period_frames / sample_rate * 1000.0f * 2 +
	       playback_latency_ms);

	/* ── Main loop ──────────────────────────────────────────────── */

	int swipe_active = 0;
	float touch_start_y = 0;
	struct timespec capture_ts = {0, 0};

	while (running) {
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			if (ev.type == SDL_QUIT)
				running = 0;
			if (ev.type == SDL_KEYDOWN) {
				switch (ev.key.keysym.sym) {
				case SDLK_q:
				case SDLK_ESCAPE:
					running = 0;
					break;
				case SDLK_r:
					if (recording)
						wav_stop();
					else
						wav_start(sample_rate, channels);
					buttons[0].active = recording;
					break;
				case SDLK_f:
					buttons[1].active = !buttons[1].active;
					break;
				case SDLK_g:
					buttons[2].active = !buttons[2].active;
					gate_enabled = buttons[2].active;
					break;
				case SDLK_e:
					eq_overlay = !eq_overlay;
					buttons[3].active = eq_overlay;
					break;
				case SDLK_p:
					if (playback_device) {
						playback_active = !playback_active;
						buttons[4].active = playback_active;
					}
					break;
				case SDLK_t:
					tdoa_overlay = !tdoa_overlay;
					buttons[5].active = tdoa_overlay;
					break;
				}
			}
			/* Touch: track start position for tap vs swipe */
			if (ev.type == SDL_FINGERDOWN) {
				touch_start_y = ev.tfinger.y;
				/* Close button on active overlay */
				if ((eq_overlay || tdoa_overlay) &&
				    close_button_hit(ev.tfinger.x,
						     ev.tfinger.y,
						     WIN_W, WIN_H)) {
					if (eq_overlay) {
						eq_overlay = 0;
						buttons[3].active = 0;
					}
					if (tdoa_overlay) {
						tdoa_overlay = 0;
						buttons[5].active = 0;
					}
				} else if (eq_overlay) {
					int hit = eq_hit_test(
						ev.tfinger.x, ev.tfinger.y,
						WIN_W, WIN_H);
					if (hit >= EQ_HIT_PITCH_BASE) {
						/* FX preset button */
						int pi = hit - EQ_HIT_PITCH_BASE;
						if (pi >= 0 && pi < FX_COUNT)
							fx_mode = pi;
					} else if (hit >= 0) {
						eq_dragging = hit;
						eq_drag_update(ev.tfinger.y,
							       WIN_H);
					}
				}
				if (ev.tfinger.y > 0.85f && eq_dragging < 0)
					swipe_active = 1;
			}
			if (ev.type == SDL_FINGERMOTION) {
				if (eq_dragging >= 0)
					eq_drag_update(ev.tfinger.y, WIN_H);
				else if (swipe_active && ev.tfinger.y < 0.5f)
					running = 0;
			}
			if (ev.type == SDL_FINGERUP) {
				/* Tap = finger didn't travel far */
				float dy = fabsf(ev.tfinger.y - touch_start_y);
				if (dy < 0.05f) {
					int btn = hit_test_button(
						ev.tfinger.x, ev.tfinger.y,
						WIN_H);
					if (btn == 0) {
						if (recording)
							wav_stop();
						else
							wav_start(sample_rate,
								  channels);
						buttons[0].active = recording;
					} else if (btn == 1) {
						buttons[1].active =
							!buttons[1].active;
					} else if (btn == 2) {
						buttons[2].active =
							!buttons[2].active;
						gate_enabled =
							buttons[2].active;
					} else if (btn == 3) {
						eq_overlay = !eq_overlay;
						buttons[3].active = eq_overlay;
					} else if (btn == 4) {
						if (playback_device) {
							playback_active =
								!playback_active;
							buttons[4].active =
								playback_active;
						}
					} else if (btn == 5) {
						tdoa_overlay = !tdoa_overlay;
						buttons[5].active =
							tdoa_overlay;
					}
				}
				swipe_active = 0;
				eq_dragging = -1;
			}
		}

		/* Drain ALL pending audio blocks — feed each to playback,
		 * keep the last one for display processing.
		 * Without this, at 256-sample periods (187 blocks/s) the
		 * 60fps render loop would only process 60 blocks and
		 * starve the playback thread. */
		int have_data = 0;
		int blocks_processed = 0;

		for (;;) {
			pthread_mutex_lock(&ring_mtx);
			if (ring_read == ring_write) {
				pthread_mutex_unlock(&ring_mtx);
				break;
			}
			float *src = ring_buf + ring_read * slot_size;
			capture_ts = ring_ts[ring_read];
			for (int i = 0; i < (int)period_frames; i++) {
				ch1_buf[i] = src[i * channels];
				if (channels == 2)
					ch2_buf[i] = src[i * channels + 1];
			}
			ring_read = (ring_read + 1) % RING_SLOTS;
			pthread_mutex_unlock(&ring_mtx);

			have_data = 1;
			blocks_processed++;

			/* Filters run on every block (state must be continuous) */
			if (hp_cutoff > 0) {
				highpass_1pole(ch1_buf, period_frames,
					       &hp_y1, &hp_x1, hp_alpha);
				if (channels == 2)
					highpass_1pole(ch2_buf, period_frames,
						       &hp_y2, &hp_x2,
						       hp_alpha);
			}
			if (lp_cutoff > 0 && buttons[1].active) {
				lowpass_1pole(ch1_buf, period_frames,
					      &lp_y1, lp_alpha_val);
				if (channels == 2)
					lowpass_1pole(ch2_buf, period_frames,
						      &lp_y2, lp_alpha_val);
			}

			if (warmup > 0) { warmup--; have_data = 0; continue; }

			/* EQ on every block */
			if (eq_enabled) {
				eq_apply(ch1_buf, period_frames, 0);
				if (channels == 2)
					eq_apply(ch2_buf, period_frames, 1);
			}

			/* Delay on every block */
			if (delay_enabled && delay_len > 0) {
				delay_process(ch1_buf, period_frames,
					      delay_buf_l);
				if (channels == 2)
					delay_process(ch2_buf, period_frames,
						      delay_buf_r);
			}

			/* Feed EVERY block to playback */
			if (playback_active) {
				float pb1[4096], pb2[4096];
				int pn = period_frames < 4096 ?
					 (int)period_frames : 4096;
				memcpy(pb1, ch1_buf, pn * sizeof(float));
				if (channels == 2)
					memcpy(pb2, ch2_buf,
					       pn * sizeof(float));

				if (noise_reduce) {
					noise_reduce_process(pb1, pn,
							     playback_gate_db);
					if (channels == 2)
						noise_reduce_process(pb2, pn,
							playback_gate_db);
				}
				if (fx_mode != FX_NONE) {
					fx_apply(pb1, pn, fx_mode,
						 &fx_phase_l);
					if (channels == 2)
						fx_apply(pb2, pn, fx_mode,
							 &fx_phase_r);
				}
				for (int i = 0; i < pn; i++) {
					pb1[i] = tanhf(pb1[i]);
					if (channels == 2)
						pb2[i] = tanhf(pb2[i]);
				}

				playback_feed(pb1,
					      channels == 2 ? pb2 : NULL, pn);

				pipeline_latency_ms =
					(float)period_frames / sample_rate *
					1000.0f + playback_latency_ms;
			}

			/* WAV recording (every block) */
			if (recording) {
				wav_write_samples(ch1_buf, period_frames);
				if (channels == 2)
					wav_write_samples(ch2_buf, period_frames);
			}
		} /* end of capture drain loop */

		if (have_data) {
			/* Display processing runs once per render frame
			 * on the last captured block (ch1_buf/ch2_buf) */

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

				/* Average for display */
				for (int i = 0; i < half_fft; i++)
					spec_avg[i] = (mag_db[i] + mag_db2[i]) * 0.5f;
			}

			/* Peak hold update */
			const float *spec_src = (channels == 2) ? spec_avg : mag_db;
			for (int i = 0; i < half_fft; i++) {
				if (spec_src[i] > peak_db[i])
					peak_db[i] = spec_src[i];
				else
					peak_db[i] = peak_db[i] * 0.995f + spec_src[i] * 0.005f;
			}

			/* Spectrogram column */
			if (channels == 2) {
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

			/* Noise gate + GCC-PHAT */
			if (channels == 2) {
				float rms_level = compute_rms(ch1_buf, period_frames);
				float rms_level_db = 20.0f * log10f(rms_level + 1e-10f);
				gate_open = !gate_enabled ||
					    (rms_level_db > gate_threshold_db);

				if (gate_open) {
					gcc_phat(ch1_buf, ch2_buf, corr,
						 fft_size,
						 gcc_fwd1, gcc_fwd2, gcc_inv,
						 gcc_in1, gcc_in2,
						 gcc_out1, gcc_out2,
						 gcc_cross, gcc_inv_out);

					float lag = find_peak_lag_subsample(
						corr, fft_size, max_lag);
					delay_samples = delay_samples * 0.85f +
							lag * 0.15f;

					float tau = delay_samples / sample_rate;
					float sin_theta = SPEED_OF_SOUND * tau /
							  mic_distance;
					if (sin_theta > 1) sin_theta = 1;
					if (sin_theta < -1) sin_theta = -1;
					float raw_angle = 90.0f -
						asinf(sin_theta) * 180.0f / M_PI;
					if (flip_dir) raw_angle = -raw_angle;

					angle_deg = angle_deg * 0.92f +
						    raw_angle * 0.08f;

					float pk = corr[lag >= 0 ?
						(int)lag :
						(int)lag + fft_size];
					confidence = pk > 0 ?
						     (pk < 1 ? pk : 1) : 0;
				}

				if (++dir_trace_count >= 3) {
					dir_trace_count = 0;
					dir_trace[dir_trace_pos].angle = angle_deg;
					dir_trace[dir_trace_pos].conf =
						gate_open ? confidence : 0;
					dir_trace_pos = (dir_trace_pos + 1) %
							DIR_TRACE_LEN;
				}
			}

		}

		/* Poll system CPU usage (~every 30 frames = ~0.5s at 60fps) */
		if (++cpu_poll_count >= 30) {
			cpu_poll_count = 0;
			FILE *sf = fopen("/proc/stat", "r");
			if (sf) {
				unsigned long user, nice, sys, idle, iow, irq, sirq, steal;
				if (fscanf(sf, "cpu %lu %lu %lu %lu %lu %lu %lu %lu",
					   &user, &nice, &sys, &idle,
					   &iow, &irq, &sirq, &steal) == 8) {
					unsigned long total = user + nice + sys + idle +
							      iow + irq + sirq + steal;
					unsigned long dt = total - prev_total;
					unsigned long di = idle - prev_idle;
					prev_total = total;
					prev_idle = idle;
					if (dt > 0)
						cpu_pct = 100.0f * (1.0f - (float)di / dt);
				}
				fclose(sf);
			}
		}

		/* ── Render ─────────────────────────────────────────── */

		SDL_SetRenderDrawColor(ren, 15, 15, 20, 255);
		SDL_RenderClear(ren);

		int margin = 10;
		int panel_w = WIN_W - 2 * margin;
		int stereo = (channels == 2);

		int right_w = stereo ? 190 : 0;
		int left_w = panel_w - (stereo ? right_w + 10 : 0);
		int right_x = margin + left_w + 10;

		/* Waveform */
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

		/* Level meters */
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

		/* ── Left column: spectrum + spectrogram ──────────── */
		int y_left = y_cur;
		int spec_bar_h = 100;

		if (stereo) {
			draw_spectrum(ren, spec_avg, half_fft,
				      margin, y_left, left_w, spec_bar_h,
				      db_floor, db_ceil, sample_rate, fft_size);
		} else {
			draw_spectrum(ren, mag_db, half_fft,
				      margin, y_left, left_w, spec_bar_h,
				      db_floor, db_ceil, sample_rate, fft_size);
		}

		/* Peak hold overlay */
		draw_peak_hold(ren, peak_db, half_fft,
			       margin, y_left, left_w, spec_bar_h,
			       db_floor, db_ceil, sample_rate);

		y_left += spec_bar_h + 3;

		/* Spectrogram */
		int sgram_h = WIN_H - y_left - margin - BTN_H - 8;
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

		/* Latency */
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

		/* Dominant frequency with parabolic interpolation */
		int bin_min = (int)(120.0f / ((float)sample_rate / fft_size));
		int bin_max = half_fft * 95 / 100;
		if (bin_min < 1) bin_min = 1;
		int peak_bin = bin_min;
		float peak_mag = -200;
		const float *freq_src = (channels == 2) ? spec_avg : mag_db;
		for (int i = bin_min; i < bin_max; i++) {
			if (freq_src[i] > peak_mag) {
				peak_mag = freq_src[i];
				peak_bin = i;
			}
		}
		float dom_freq_now = (float)peak_bin * sample_rate / fft_size;

		/* Parabolic interpolation for sub-bin frequency */
		if (peak_bin > bin_min && peak_bin < bin_max - 1) {
			float alpha_p = freq_src[peak_bin - 1];
			float beta    = freq_src[peak_bin];
			float gamma   = freq_src[peak_bin + 1];
			float denom   = alpha_p - 2 * beta + gamma;
			if (fabsf(denom) > 1e-10f) {
				float p = 0.5f * (alpha_p - gamma) / denom;
				dom_freq_now = (peak_bin + p) * sample_rate / fft_size;
			}
		}
		dom_freq_avg = dom_freq_avg * 0.95f + dom_freq_now * 0.05f;

		/* Note display with hold: only change when a NEW note is
		 * stable for 10+ frames. Prevents flickering on boundaries. */
		note_freq_avg = note_freq_avg * 0.97f + dom_freq_now * 0.03f;
		{
			char note_candidate[16];
			freq_to_note(note_freq_avg, note_candidate,
				     sizeof(note_candidate));
			if (strcmp(note_candidate, note_display) != 0) {
				/* Different note — count how long it persists */
				note_hold++;
				if (note_hold >= 10 ||
				    strcmp(note_display, "---") == 0)
					snprintf(note_display,
						 sizeof(note_display),
						 "%s", note_candidate);
			} else {
				note_hold = 0;  /* same as displayed — reset */
			}
		}

		int txt_scale = 2;
		int line_h = (FONT_H + 2) * txt_scale;

		if (stereo) {
			int sy = y_cur;
			SDL_SetRenderDrawColor(ren, 140, 160, 180, 255);

			char stat[32];
			snprintf(stat, sizeof(stat), "Vis %5.1fms", latency_avg);
			draw_text(ren, stat, right_x, sy, txt_scale);
			sy += line_h;

			if (playback_active) {
				SDL_SetRenderDrawColor(ren, 100, 255, 100, 255);
				snprintf(stat, sizeof(stat), "FX  %5.1fms",
					 pipeline_latency_ms);
			} else {
				snprintf(stat, sizeof(stat), "FX   OFF");
			}
			draw_text(ren, stat, right_x, sy, txt_scale);
			SDL_SetRenderDrawColor(ren, 140, 160, 180, 255);
			sy += line_h;

			snprintf(stat, sizeof(stat), "RMS %5.1fdB", rms_db_avg);
			draw_text(ren, stat, right_x, sy, txt_scale);
			sy += line_h;

			snprintf(stat, sizeof(stat), "Frq %5dHz", (int)dom_freq_avg);
			draw_text(ren, stat, right_x, sy, txt_scale);
			sy += line_h;

			snprintf(stat, sizeof(stat), "Note %s", note_display);
			draw_text(ren, stat, right_x, sy, txt_scale);
			sy += line_h;

			snprintf(stat, sizeof(stat), "CPU  %4.1f%%", cpu_pct);
			draw_text(ren, stat, right_x, sy, txt_scale);
			sy += line_h + 5;

			/* Recording indicator */
			if (recording) {
				SDL_SetRenderDrawColor(ren, 255, 60, 60, 255);
				snprintf(stat, sizeof(stat), "REC %4.1fs",
					 wav_data_bytes / (float)(sample_rate *
					 channels * 2));
				draw_text(ren, stat, right_x, sy, txt_scale);
				sy += line_h + 5;
			}

			/* Direction indicator */
			int dir_r = 55;
			int dir_cx = right_x + right_w / 2;
			int dir_cy = sy + dir_r + 5;
			if (dir_cy + dir_r < WIN_H - margin - BTN_H - 8) {
				draw_direction_indicator(ren, angle_deg,
							 confidence,
							 dir_trace,
							 DIR_TRACE_LEN,
							 dir_trace_pos,
							 gate_open,
							 dir_cx, dir_cy,
							 dir_r);

				char ang[32];
				snprintf(ang, sizeof(ang), "%3.0f deg",
					 angle_deg);
				SDL_SetRenderDrawColor(ren, 140, 160, 180, 255);
				draw_text(ren, ang, dir_cx - 30,
					  dir_cy + dir_r + 5, txt_scale);
			}
		} else {
			/* Mono: stats below spectrogram */
			int sy = y_left + sgram_h + 5;
			char line1[80], line2[80];
			snprintf(line1, sizeof(line1),
				 "Vis %.0fms  FX %.0fms  CPU %4.1f%%",
				 latency_avg,
				 playback_active ? pipeline_latency_ms : 0.0f,
				 cpu_pct);
			snprintf(line2, sizeof(line2),
				 "Frq %5d Hz  Note %s  Gain %4.1fx",
				 (int)dom_freq_avg, note_display, gain);
			SDL_SetRenderDrawColor(ren, 140, 160, 180, 255);
			draw_text(ren, line1, margin, sy, txt_scale);
			draw_text(ren, line2, margin, sy + line_h, txt_scale);

			if (recording) {
				char rec_txt[32];
				SDL_SetRenderDrawColor(ren, 255, 60, 60, 255);
				snprintf(rec_txt, sizeof(rec_txt), "REC %4.1fs",
					 wav_data_bytes / (float)(sample_rate *
					 channels * 2));
				draw_text(ren, rec_txt, margin,
					  sy + 2 * line_h, txt_scale);
			}
		}

		/* On-screen buttons */
		draw_buttons(ren, WIN_W, WIN_H);

		/* EQ overlay (drawn on top of everything) */
		if (eq_overlay)
			draw_eq_overlay(ren, WIN_W, WIN_H);

		/* TDOA overlay */
		if (tdoa_overlay && stereo)
			draw_tdoa_overlay(ren, WIN_W, WIN_H,
					  ch1_buf, ch2_buf,
					  corr, fft_size, max_lag,
					  delay_samples, angle_deg,
					  confidence);

		SDL_RenderPresent(ren);
	}

	/* Cleanup */
	wav_stop();
	pthread_join(audio_tid, NULL);
	if (play_tid)
		pthread_join(play_tid, NULL);
	free(delay_buf_l);
	free(delay_buf_r);
	/* pitch buffers removed — effects work in-place */
	free(play_ring);

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
	free(peak_db);
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
