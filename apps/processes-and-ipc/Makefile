CC ?= gcc
CFLAGS = -Wall -O2 -pthread

all: atomic_sensor

atomic_sensor: atomic_sensor.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f atomic_sensor
