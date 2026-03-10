/*
 * pong_fb.c — Pong game for Linux framebuffer (/dev/fbN)
 *
 * Works on any monochrome 1bpp framebuffer: SSD1306 OLED (128x64, 128x32)
 * or BUSE LED matrix (128x19). Auto-detects resolution via ioctl.
 *
 * Build:  gcc -o pong_fb pong_fb.c
 * Run:    ./pong_fb [/dev/fb0]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <termios.h>
#include <time.h>
#include <signal.h>

/* Game constants */
#define PADDLE_HEIGHT	6
#define PADDLE_WIDTH	2
#define BALL_SIZE	2
#define PADDLE_MARGIN	4
#define FPS		30

static int fb_fd = -1;
static unsigned char *fb_mem = NULL;
static size_t fb_size;
static int width, height, line_length;
static struct termios orig_termios;
static volatile int running = 1;

/* --- Pixel helpers for 1bpp packed framebuffer --- */

static void set_pixel(int x, int y, int on)
{
	int byte_idx, bit_idx;

	if (x < 0 || x >= width || y < 0 || y >= height)
		return;

	byte_idx = y * line_length + x / 8;
	bit_idx = 7 - (x % 8);  /* MSB = leftmost pixel */

	if (on)
		fb_mem[byte_idx] |= (1 << bit_idx);
	else
		fb_mem[byte_idx] &= ~(1 << bit_idx);
}

static void clear_screen(void)
{
	memset(fb_mem, 0, fb_size);
}

static void draw_rect(int x0, int y0, int w, int h)
{
	for (int y = y0; y < y0 + h; y++)
		for (int x = x0; x < x0 + w; x++)
			set_pixel(x, y, 1);
}

/* --- Terminal raw mode for keyboard input --- */

static void restore_terminal(void)
{
	tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

static void setup_terminal(void)
{
	struct termios raw;

	tcgetattr(STDIN_FILENO, &orig_termios);
	raw = orig_termios;
	raw.c_lflag &= ~(ECHO | ICANON);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 0;
	tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

/* Non-blocking key read. Returns: 'u'=up, 'd'=down, 'q'=quit, 0=none */
static char read_key(void)
{
	char buf[3];
	int n = read(STDIN_FILENO, buf, sizeof(buf));

	if (n == 1) {
		if (buf[0] == 'q' || buf[0] == 'Q')
			return 'q';
		if (buf[0] == 'w' || buf[0] == 'W')
			return 'u';
		if (buf[0] == 's' || buf[0] == 'S')
			return 'd';
	}
	/* Arrow keys: ESC [ A/B */
	if (n == 3 && buf[0] == 27 && buf[1] == '[') {
		if (buf[2] == 'A')
			return 'u';
		if (buf[2] == 'B')
			return 'd';
	}
	return 0;
}

/* --- Signal handler --- */

static void handle_signal(int sig)
{
	(void)sig;
	running = 0;
}

/* --- Main --- */

int main(int argc, char *argv[])
{
	const char *fb_path = argc > 1 ? argv[1] : "/dev/fb0";
	struct fb_var_screeninfo vinfo;
	struct fb_fix_screeninfo finfo;

	/* Open framebuffer */
	fb_fd = open(fb_path, O_RDWR);
	if (fb_fd < 0) {
		perror("open framebuffer");
		return 1;
	}

	/* Query resolution */
	if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
		perror("FBIOGET_VSCREENINFO");
		close(fb_fd);
		return 1;
	}
	if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
		perror("FBIOGET_FSCREENINFO");
		close(fb_fd);
		return 1;
	}

	width = vinfo.xres;
	height = vinfo.yres;
	line_length = finfo.line_length;
	fb_size = line_length * height;

	printf("Framebuffer: %s (%dx%d, %d bpp, line_length=%d)\n",
	       fb_path, width, height, vinfo.bits_per_pixel, line_length);

	if (vinfo.bits_per_pixel != 1) {
		fprintf(stderr, "Only 1bpp monochrome framebuffers supported\n");
		close(fb_fd);
		return 1;
	}

	/* mmap framebuffer */
	fb_mem = mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED,
		      fb_fd, 0);
	if (fb_mem == MAP_FAILED) {
		perror("mmap");
		close(fb_fd);
		return 1;
	}

	/* Setup terminal and signals */
	setup_terminal();
	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	/* Adapt paddle height to display size */
	int paddle_h = PADDLE_HEIGHT;
	if (height < 32)
		paddle_h = height / 4;
	if (paddle_h < 2)
		paddle_h = 2;

	/* Game state */
	int paddle_y = (height - paddle_h) / 2;
	int ai_y = (height - paddle_h) / 2;

	float ball_x = width / 2.0f;
	float ball_y = height / 2.0f;
	float ball_vx = 1.5f;
	float ball_vy = 1.0f;

	int score_left = 0, score_right = 0;

	struct timespec ts;
	ts.tv_sec = 0;
	ts.tv_nsec = 1000000000L / FPS;

	/* Game loop */
	while (running) {
		/* Input */
		char key = read_key();
		if (key == 'q') break;
		if (key == 'u' && paddle_y > 0) paddle_y -= 2;
		if (key == 'd' && paddle_y < height - paddle_h) paddle_y += 2;

		/* AI opponent — follows ball with some delay */
		int ai_center = ai_y + paddle_h / 2;
		if (ai_center < (int)ball_y - 1 && ai_y < height - paddle_h)
			ai_y++;
		else if (ai_center > (int)ball_y + 1 && ai_y > 0)
			ai_y--;

		/* Update ball */
		ball_x += ball_vx;
		ball_y += ball_vy;

		/* Bounce off top/bottom */
		if (ball_y <= 0) { ball_y = 0; ball_vy = -ball_vy; }
		if (ball_y >= height - BALL_SIZE) {
			ball_y = height - BALL_SIZE;
			ball_vy = -ball_vy;
		}

		/* Left paddle collision (player) */
		if (ball_x <= PADDLE_MARGIN + PADDLE_WIDTH &&
		    ball_y + BALL_SIZE > paddle_y &&
		    ball_y < paddle_y + paddle_h) {
			ball_x = PADDLE_MARGIN + PADDLE_WIDTH;
			ball_vx = -ball_vx;
		}

		/* Right paddle collision (AI) */
		if (ball_x >= width - PADDLE_MARGIN - PADDLE_WIDTH - BALL_SIZE &&
		    ball_y + BALL_SIZE > ai_y &&
		    ball_y < ai_y + paddle_h) {
			ball_x = width - PADDLE_MARGIN - PADDLE_WIDTH - BALL_SIZE;
			ball_vx = -ball_vx;
		}

		/* Score */
		if (ball_x < 0) {
			score_right++;
			ball_x = width / 2.0f;
			ball_y = height / 2.0f;
			ball_vx = 1.5f;
		}
		if (ball_x > width) {
			score_left++;
			ball_x = width / 2.0f;
			ball_y = height / 2.0f;
			ball_vx = -1.5f;
		}

		/* Draw */
		clear_screen();

		/* Center line (dotted) */
		for (int y = 0; y < height; y += 4)
			set_pixel(width / 2, y, 1);

		/* Paddles */
		draw_rect(PADDLE_MARGIN, paddle_y, PADDLE_WIDTH, paddle_h);
		draw_rect(width - PADDLE_MARGIN - PADDLE_WIDTH, ai_y,
			  PADDLE_WIDTH, paddle_h);

		/* Ball */
		draw_rect((int)ball_x, (int)ball_y, BALL_SIZE, BALL_SIZE);

		/* Flush (for mmap'd framebuffers with deferred I/O) */
		msync(fb_mem, fb_size, MS_SYNC);

		nanosleep(&ts, NULL);
	}

	printf("\nFinal score: %d - %d\n", score_left, score_right);

	/* Cleanup */
	clear_screen();
	msync(fb_mem, fb_size, MS_SYNC);
	munmap(fb_mem, fb_size);
	close(fb_fd);
	restore_terminal();

	return 0;
}
