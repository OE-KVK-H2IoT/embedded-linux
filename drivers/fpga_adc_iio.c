// SPDX-License-Identifier: GPL-2.0
/*
 * fpga_adc_iio.c — IIO driver for a 4-channel 12-bit ADC over SPI
 *
 * Compatible with MCP3008 (10-bit) and MCP3208 (12-bit) ADC chips.
 * Demonstrates the IIO driver API: channels, read_raw, triggered buffers.
 *
 * See: Custom IIO Driver tutorial
 */

#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/buffer.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>

#define FPGA_ADC_NUM_CHANNELS  4
#define FPGA_ADC_RESOLUTION    12   /* 12-bit ADC */
#define FPGA_ADC_VREF_MV       3300 /* 3.3V reference */

struct fpga_adc_state {
	struct spi_device *spi;
	/* DMA-safe buffers for SPI transfers (must not be on stack) */
	u8 tx_buf[3] __aligned(IIO_DMA_MINALIGN);
	u8 rx_buf[3] __aligned(IIO_DMA_MINALIGN);
};

/*
 * Read a single ADC channel over SPI.
 *
 * MCP3008/3208-compatible protocol:
 * TX: [0x01] [0x80 | (channel << 4)] [0x00]
 * RX: [xxxx] [0000 MSB3..0]          [LSB7..0]
 * Result: 12-bit unsigned value (0..4095)
 */
static int fpga_adc_read_channel(struct fpga_adc_state *st, int channel)
{
	struct spi_transfer t = {
		.tx_buf = st->tx_buf,
		.rx_buf = st->rx_buf,
		.len = 3,
	};
	int ret;

	st->tx_buf[0] = 0x01;                      /* start bit */
	st->tx_buf[1] = 0x80 | (channel << 4);     /* single-ended, channel select */
	st->tx_buf[2] = 0x00;

	ret = spi_sync_transfer(st->spi, &t, 1);
	if (ret)
		return ret;

	/* Extract 12-bit result from bytes 1-2 */
	return ((st->rx_buf[1] & 0x0F) << 8) | st->rx_buf[2];
}

/*
 * IIO read_raw callback — called for sysfs reads:
 *   cat in_voltage0_raw  → IIO_CHAN_INFO_RAW
 *   cat in_voltage_scale → IIO_CHAN_INFO_SCALE
 */
static int fpga_adc_read_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int *val, int *val2, long mask)
{
	struct fpga_adc_state *st = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
	{
		int ret;

		/* Prevent polled reads while buffered capture is active */
		ret = iio_device_claim_direct_mode(indio_dev);
		if (ret)
			return ret;

		ret = fpga_adc_read_channel(st, chan->channel);
		iio_device_release_direct_mode(indio_dev);

		if (ret < 0)
			return ret;

		*val = ret;
		return IIO_VAL_INT;
	}

	case IIO_CHAN_INFO_SCALE:
		/* Scale: Vref_mV / 2^resolution = 3300 / 4096 ≈ 0.806 mV/LSB */
		*val = FPGA_ADC_VREF_MV;
		*val2 = FPGA_ADC_RESOLUTION;
		return IIO_VAL_FRACTIONAL_LOG2;

	default:
		return -EINVAL;
	}
}

static const struct iio_info fpga_adc_info = {
	.read_raw = fpga_adc_read_raw,
};

/*
 * Channel definitions using a macro for consistency.
 * Each channel becomes in_voltageN_raw in sysfs.
 * All channels share in_voltage_scale.
 */
#define FPGA_ADC_CHANNEL(idx) {                             \
	.type = IIO_VOLTAGE,                                \
	.indexed = 1,                                       \
	.channel = (idx),                                   \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),        \
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),\
	.scan_index = (idx),                                \
	.scan_type = {                                      \
		.sign = 'u',                                \
		.realbits = 12,                             \
		.storagebits = 16,                          \
		.endianness = IIO_CPU,                      \
	},                                                  \
}

static const struct iio_chan_spec fpga_adc_channels[] = {
	FPGA_ADC_CHANNEL(0),
	FPGA_ADC_CHANNEL(1),
	FPGA_ADC_CHANNEL(2),
	FPGA_ADC_CHANNEL(3),
	IIO_CHAN_SOFT_TIMESTAMP(FPGA_ADC_NUM_CHANNELS),
};

/*
 * Triggered buffer handler — called on each trigger event.
 * Reads all enabled channels and pushes one sample set to the kfifo.
 */
static irqreturn_t fpga_adc_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct fpga_adc_state *st = iio_priv(indio_dev);
	struct {
		u16 channels[FPGA_ADC_NUM_CHANNELS];
		s64 timestamp;
	} __aligned(8) scan;
	int i, bit, ret;

	memset(&scan, 0, sizeof(scan));

	i = 0;
	for_each_set_bit(bit, indio_dev->active_scan_mask,
			 indio_dev->masklength) {
		ret = fpga_adc_read_channel(st, bit);
		if (ret < 0)
			goto done;
		scan.channels[i++] = ret;
	}

	iio_push_to_buffers_with_timestamp(indio_dev, &scan,
					   iio_get_time_ns(indio_dev));

done:
	iio_trigger_notify_done(indio_dev->trig);
	return IRQ_HANDLED;
}

static int fpga_adc_probe(struct spi_device *spi)
{
	struct iio_dev *indio_dev;
	struct fpga_adc_state *st;
	int ret;

	indio_dev = devm_iio_device_alloc(&spi->dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);
	st->spi = spi;
	spi_set_drvdata(spi, indio_dev);

	indio_dev->name = "fpga-adc";
	indio_dev->info = &fpga_adc_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = fpga_adc_channels;
	indio_dev->num_channels = ARRAY_SIZE(fpga_adc_channels);

	ret = iio_triggered_buffer_setup(indio_dev, NULL,
					 fpga_adc_trigger_handler, NULL);
	if (ret) {
		dev_err(&spi->dev, "Failed to setup triggered buffer\n");
		return ret;
	}

	ret = iio_device_register(indio_dev);
	if (ret) {
		dev_err(&spi->dev, "Failed to register IIO device\n");
		iio_triggered_buffer_cleanup(indio_dev);
		return ret;
	}

	dev_info(&spi->dev,
		 "FPGA ADC IIO driver probed (%d channels, %d-bit)\n",
		 FPGA_ADC_NUM_CHANNELS, FPGA_ADC_RESOLUTION);
	return 0;
}

static void fpga_adc_remove(struct spi_device *spi)
{
	struct iio_dev *indio_dev = spi_get_drvdata(spi);

	iio_device_unregister(indio_dev);
	iio_triggered_buffer_cleanup(indio_dev);
}

static const struct of_device_id fpga_adc_of_match[] = {
	{ .compatible = "custom,fpga-adc" },
	{ }
};
MODULE_DEVICE_TABLE(of, fpga_adc_of_match);

static const struct spi_device_id fpga_adc_id[] = {
	{ "fpga-adc", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, fpga_adc_id);

static struct spi_driver fpga_adc_driver = {
	.driver = {
		.name = "fpga-adc",
		.of_match_table = fpga_adc_of_match,
	},
	.probe = fpga_adc_probe,
	.remove = fpga_adc_remove,
	.id_table = fpga_adc_id,
};
module_spi_driver(fpga_adc_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Embedded Linux Course");
MODULE_DESCRIPTION("IIO driver for FPGA ADC (MCP3008-compatible SPI protocol)");
