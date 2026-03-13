CC ?= gcc
CFLAGS = -Wall -O2 $(shell sdl2-config --cflags)
LDFLAGS = $(shell sdl2-config --libs) -lm -lpthread

all: level_sdl2 sdl2_dashboard pong_fb iio_capture audio_viz

level_sdl2: level_sdl2.c jitter_logger.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

sdl2_dashboard: sdl2_dashboard.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -lSDL2_ttf

atomic_sensor: atomic_sensor.c
	$(CC) -Wall -O2 -pthread -o $@ $<

pong_fb: pong_fb.c
	$(CC) -Wall -O2 -o $@ $<

iio_capture: iio_capture.c
	$(CC) -Wall -O2 -o $@ $<

audio_viz: audio_viz.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -lasound -lfftw3f

clean:
	rm -f level_sdl2 sdl2_dashboard atomic_sensor pong_fb iio_capture audio_viz
