CC ?= gcc
CFLAGS = -Wall -O2 $(shell sdl2-config --cflags)
LDFLAGS = $(shell sdl2-config --libs) -lm -lpthread

all: level_sdl2

level_sdl2: level_sdl2.c jitter_logger.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f level_sdl2
