SUBDIRS = i2s-audio-viz level-display pong-fb sdl2-dashboard iio-buffered-capture processes-and-ipc

all:
	@for dir in $(SUBDIRS); do \
		echo "==> Building apps/$$dir"; \
		$(MAKE) -C $$dir || exit 1; \
	done

clean:
	@for dir in $(SUBDIRS); do \
		$(MAKE) -C $$dir clean; \
	done
