CC ?= gcc
CFLAGS = -Wall -O2 $(shell sdl2-config --cflags)
LDFLAGS = $(shell sdl2-config --libs) -lm -lpthread -lasound -lfftw3f

all: audio_viz audio_viz_full

audio_viz: audio_viz.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

audio_viz_full: audio_viz_full.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f audio_viz audio_viz_full
