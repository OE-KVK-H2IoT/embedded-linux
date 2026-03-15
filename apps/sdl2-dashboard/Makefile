CC ?= gcc
CFLAGS = -Wall -O2 $(shell sdl2-config --cflags)
LDFLAGS = $(shell sdl2-config --libs) -lSDL2_ttf -lm

all: sdl2_dashboard

sdl2_dashboard: sdl2_dashboard.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f sdl2_dashboard
