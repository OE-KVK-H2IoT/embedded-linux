CC ?= gcc
CFLAGS = -Wall -O2

all: iio_capture

iio_capture: iio_capture.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f iio_capture
