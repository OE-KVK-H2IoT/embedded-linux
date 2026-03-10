CC ?= gcc
CFLAGS = -Wall -O2 $(shell sdl2-config --cflags)
LDFLAGS = $(shell sdl2-config --libs) -lm -lpthread

all: level_sdl2 iio_capture

level_sdl2: level_sdl2.c jitter_logger.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

atomic_sensor: atomic_sensor.c
	$(CC) -Wall -O2 -pthread -o $@ $<

pong_fb: pong_fb.c
	$(CC) -Wall -O2 -o $@ $<

iio_capture: iio_capture.c
	$(CC) -Wall -O2 -o $@ $<

clean:
	rm -f level_sdl2 atomic_sensor pong_fb iio_capture
