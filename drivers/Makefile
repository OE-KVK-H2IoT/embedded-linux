obj-m += bmi160_spi.o
obj-m += mcp9808.o
obj-m += ssd1306_fb.o
obj-m += fpga_adc_iio.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean
