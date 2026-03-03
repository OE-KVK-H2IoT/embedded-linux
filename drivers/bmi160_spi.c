// SPDX-License-Identifier: GPL-2.0
/*
 * bmi160_spi.c - SPI driver for Bosch BMI160 IMU
 *
 * This is a teaching driver for the Embedded Systems course.
 * It exposes accelerometer, gyroscope, and temperature data via
 * sysfs attributes and a character device (/dev/bmi160).
 *
 * Copyright (C) 2025 Obuda University - Embedded Systems Lab
 */

#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/delay.h>
#include <linux/mutex.h>

/* ---------- BMI160 register map ---------- */
#define BMI160_CHIP_ID          0x00
#define BMI160_CHIP_ID_VAL      0xD1

#define BMI160_DATA_GYR_X_L     0x0C
#define BMI160_DATA_GYR_X_H     0x0D
#define BMI160_DATA_GYR_Y_L     0x0E
#define BMI160_DATA_GYR_Y_H     0x0F
#define BMI160_DATA_GYR_Z_L     0x10
#define BMI160_DATA_GYR_Z_H     0x11

#define BMI160_DATA_ACC_X_L     0x12
#define BMI160_DATA_ACC_X_H     0x13
#define BMI160_DATA_ACC_Y_L     0x14
#define BMI160_DATA_ACC_Y_H     0x15
#define BMI160_DATA_ACC_Z_L     0x16
#define BMI160_DATA_ACC_Z_H     0x17

#define BMI160_TEMPERATURE_L    0x20
#define BMI160_TEMPERATURE_H    0x21

#define BMI160_ACC_CONF         0x40
#define BMI160_ACC_RANGE        0x41
#define BMI160_GYR_CONF         0x42
#define BMI160_GYR_RANGE        0x43

#define BMI160_CMD              0x7E

/* Command values */
#define BMI160_CMD_SOFT_RESET   0xB6
#define BMI160_CMD_ACC_NORMAL   0x11
#define BMI160_CMD_GYR_NORMAL   0x15

/* Scale factors */
#define BMI160_ACC_SCALE_2G     16384   /* LSB/g for +/-2g range */
#define BMI160_GYR_SCALE_250    131     /* LSB/(deg/s) for +/-250 deg/s */

/* ---------- driver private data ---------- */
struct bmi160_data {
	struct spi_device *spi;
	struct mutex lock;
	/* character device */
	dev_t devnum;
	struct cdev cdev;
	struct class *cls;
	struct device *chrdev;
};

/* ---------- SPI register helpers ---------- */

/*
 * BMI160 SPI protocol: to read a register, set bit 7 of the address byte.
 * The second byte is a dummy; the device returns data on the second byte.
 */
static int bmi160_spi_read_reg(struct spi_device *spi, u8 reg, u8 *val)
{
	u8 tx[2] = { reg | 0x80, 0x00 };
	u8 rx[2] = { 0 };
	struct spi_transfer xfer = {
		.tx_buf = tx,
		.rx_buf = rx,
		.len    = 2,
	};
	int ret;

	ret = spi_sync_transfer(spi, &xfer, 1);
	if (ret)
		return ret;

	*val = rx[1];
	return 0;
}

/*
 * SPI write: bit 7 clear, followed by the value byte.
 */
static int bmi160_spi_write_reg(struct spi_device *spi, u8 reg, u8 val)
{
	u8 tx[2] = { reg & 0x7F, val };

	return spi_write(spi, tx, 2);
}

/*
 * Bulk-read 12 bytes starting at DATA_GYR_X_L (0x0C).
 * Covers gyro XYZ (6 bytes) + accel XYZ (6 bytes).
 */
static int bmi160_read_motion_data(struct spi_device *spi,
				   s16 *gx, s16 *gy, s16 *gz,
				   s16 *ax, s16 *ay, s16 *az)
{
	u8 tx[13];
	u8 rx[13];
	struct spi_transfer xfer = {
		.tx_buf = tx,
		.rx_buf = rx,
		.len    = 13,
	};
	int ret;

	memset(tx, 0, sizeof(tx));
	tx[0] = BMI160_DATA_GYR_X_L | 0x80;   /* read, starting at 0x0C */

	ret = spi_sync_transfer(spi, &xfer, 1);
	if (ret)
		return ret;

	/* rx[0] is dummy; data starts at rx[1] */
	*gx = (s16)((rx[2] << 8) | rx[1]);
	*gy = (s16)((rx[4] << 8) | rx[3]);
	*gz = (s16)((rx[6] << 8) | rx[5]);
	*ax = (s16)((rx[8] << 8) | rx[7]);
	*ay = (s16)((rx[10] << 8) | rx[9]);
	*az = (s16)((rx[12] << 8) | rx[11]);

	return 0;
}

static int bmi160_read_temperature(struct spi_device *spi, s16 *temp_raw)
{
	u8 tx[3] = { BMI160_TEMPERATURE_L | 0x80, 0, 0 };
	u8 rx[3] = { 0 };
	struct spi_transfer xfer = {
		.tx_buf = tx,
		.rx_buf = rx,
		.len    = 3,
	};
	int ret;

	ret = spi_sync_transfer(spi, &xfer, 1);
	if (ret)
		return ret;

	*temp_raw = (s16)((rx[2] << 8) | rx[1]);
	return 0;
}

/* ---------- sysfs attributes ---------- */

static ssize_t accel_x_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct bmi160_data *data = dev_get_drvdata(dev);
	s16 gx, gy, gz, ax, ay, az;
	int ret;

	mutex_lock(&data->lock);
	ret = bmi160_read_motion_data(data->spi, &gx, &gy, &gz,
				      &ax, &ay, &az);
	mutex_unlock(&data->lock);
	if (ret)
		return ret;

	/* Return raw value; user-space divides by 16384 for g */
	return sysfs_emit(buf, "%d\n", ax);
}
static DEVICE_ATTR_RO(accel_x);

static ssize_t accel_y_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct bmi160_data *data = dev_get_drvdata(dev);
	s16 gx, gy, gz, ax, ay, az;
	int ret;

	mutex_lock(&data->lock);
	ret = bmi160_read_motion_data(data->spi, &gx, &gy, &gz,
				      &ax, &ay, &az);
	mutex_unlock(&data->lock);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%d\n", ay);
}
static DEVICE_ATTR_RO(accel_y);

static ssize_t accel_z_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct bmi160_data *data = dev_get_drvdata(dev);
	s16 gx, gy, gz, ax, ay, az;
	int ret;

	mutex_lock(&data->lock);
	ret = bmi160_read_motion_data(data->spi, &gx, &gy, &gz,
				      &ax, &ay, &az);
	mutex_unlock(&data->lock);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%d\n", az);
}
static DEVICE_ATTR_RO(accel_z);

static ssize_t gyro_x_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct bmi160_data *data = dev_get_drvdata(dev);
	s16 gx, gy, gz, ax, ay, az;
	int ret;

	mutex_lock(&data->lock);
	ret = bmi160_read_motion_data(data->spi, &gx, &gy, &gz,
				      &ax, &ay, &az);
	mutex_unlock(&data->lock);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%d\n", gx);
}
static DEVICE_ATTR_RO(gyro_x);

static ssize_t gyro_y_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct bmi160_data *data = dev_get_drvdata(dev);
	s16 gx, gy, gz, ax, ay, az;
	int ret;

	mutex_lock(&data->lock);
	ret = bmi160_read_motion_data(data->spi, &gx, &gy, &gz,
				      &ax, &ay, &az);
	mutex_unlock(&data->lock);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%d\n", gy);
}
static DEVICE_ATTR_RO(gyro_y);

static ssize_t gyro_z_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct bmi160_data *data = dev_get_drvdata(dev);
	s16 gx, gy, gz, ax, ay, az;
	int ret;

	mutex_lock(&data->lock);
	ret = bmi160_read_motion_data(data->spi, &gx, &gy, &gz,
				      &ax, &ay, &az);
	mutex_unlock(&data->lock);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%d\n", gz);
}
static DEVICE_ATTR_RO(gyro_z);

static ssize_t temperature_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct bmi160_data *data = dev_get_drvdata(dev);
	s16 temp_raw;
	int ret;
	int temp_mdeg;  /* temperature in milli-degrees C */

	mutex_lock(&data->lock);
	ret = bmi160_read_temperature(data->spi, &temp_raw);
	mutex_unlock(&data->lock);
	if (ret)
		return ret;

	/*
	 * BMI160 temperature formula:
	 *   temp_degC = 23.0 + temp_raw / 512.0
	 * We report milli-degrees to avoid floating point in kernel.
	 */
	temp_mdeg = 23000 + (temp_raw * 1000) / 512;
	return sysfs_emit(buf, "%d\n", temp_mdeg);
}
static DEVICE_ATTR_RO(temperature);

static struct attribute *bmi160_attrs[] = {
	&dev_attr_accel_x.attr,
	&dev_attr_accel_y.attr,
	&dev_attr_accel_z.attr,
	&dev_attr_gyro_x.attr,
	&dev_attr_gyro_y.attr,
	&dev_attr_gyro_z.attr,
	&dev_attr_temperature.attr,
	NULL,
};

static const struct attribute_group bmi160_attr_group = {
	.attrs = bmi160_attrs,
};

/* ---------- character device ---------- */

static int bmi160_cdev_open(struct inode *inode, struct file *filp)
{
	struct bmi160_data *data;

	data = container_of(inode->i_cdev, struct bmi160_data, cdev);
	filp->private_data = data;
	return 0;
}

static ssize_t bmi160_cdev_read(struct file *filp, char __user *ubuf,
				size_t count, loff_t *off)
{
	struct bmi160_data *data = filp->private_data;
	s16 gx, gy, gz, ax, ay, az;
	char buf[128];
	int len, ret;

	if (*off > 0)
		return 0;   /* simple one-shot read */

	mutex_lock(&data->lock);
	ret = bmi160_read_motion_data(data->spi, &gx, &gy, &gz,
				      &ax, &ay, &az);
	mutex_unlock(&data->lock);
	if (ret)
		return ret;

	/*
	 * Format: "AX AY AZ GX GY GZ\n"
	 * Values are raw signed 16-bit integers.
	 * User-space scales: accel / 16384.0 (g), gyro / 131.0 (deg/s).
	 */
	len = snprintf(buf, sizeof(buf), "%d %d %d %d %d %d\n",
		       ax, ay, az, gx, gy, gz);

	if (len > count)
		len = count;

	if (copy_to_user(ubuf, buf, len))
		return -EFAULT;

	*off += len;
	return len;
}

static const struct file_operations bmi160_cdev_fops = {
	.owner = THIS_MODULE,
	.open  = bmi160_cdev_open,
	.read  = bmi160_cdev_read,
};

/* ---------- init / probe ---------- */

static int bmi160_hw_init(struct spi_device *spi)
{
	u8 chip_id;
	int ret;

	/* Soft reset */
	ret = bmi160_spi_write_reg(spi, BMI160_CMD, BMI160_CMD_SOFT_RESET);
	if (ret)
		return ret;
	msleep(100);

	/* Verify chip ID */
	ret = bmi160_spi_read_reg(spi, BMI160_CHIP_ID, &chip_id);
	if (ret)
		return ret;

	if (chip_id != BMI160_CHIP_ID_VAL) {
		dev_err(&spi->dev,
			"unexpected chip ID 0x%02x (expected 0x%02x)\n",
			chip_id, BMI160_CHIP_ID_VAL);
		return -ENODEV;
	}

	dev_info(&spi->dev, "BMI160 detected (chip ID 0x%02x)\n", chip_id);

	/* Set accelerometer to normal mode */
	ret = bmi160_spi_write_reg(spi, BMI160_CMD, BMI160_CMD_ACC_NORMAL);
	if (ret)
		return ret;
	msleep(50);

	/* Set gyroscope to normal mode */
	ret = bmi160_spi_write_reg(spi, BMI160_CMD, BMI160_CMD_GYR_NORMAL);
	if (ret)
		return ret;
	msleep(80);

	/* Configure accelerometer: ODR 100 Hz, BWP normal */
	ret = bmi160_spi_write_reg(spi, BMI160_ACC_CONF, 0x28);
	if (ret)
		return ret;

	/* Accelerometer range: +/-2g */
	ret = bmi160_spi_write_reg(spi, BMI160_ACC_RANGE, 0x03);
	if (ret)
		return ret;

	/* Configure gyroscope: ODR 100 Hz, BWP normal */
	ret = bmi160_spi_write_reg(spi, BMI160_GYR_CONF, 0x28);
	if (ret)
		return ret;

	/* Gyroscope range: +/-250 deg/s */
	ret = bmi160_spi_write_reg(spi, BMI160_GYR_RANGE, 0x03);
	if (ret)
		return ret;

	return 0;
}

static int bmi160_spi_probe(struct spi_device *spi)
{
	struct bmi160_data *data;
	int ret;

	data = devm_kzalloc(&spi->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->spi = spi;
	mutex_init(&data->lock);
	spi_set_drvdata(spi, data);

	/* Hardware initialisation */
	ret = bmi160_hw_init(spi);
	if (ret) {
		dev_err(&spi->dev, "hardware init failed: %d\n", ret);
		return ret;
	}

	/* Register sysfs attributes */
	ret = sysfs_create_group(&spi->dev.kobj, &bmi160_attr_group);
	if (ret) {
		dev_err(&spi->dev, "sysfs creation failed: %d\n", ret);
		return ret;
	}

	/* Register character device */
	ret = alloc_chrdev_region(&data->devnum, 0, 1, "bmi160");
	if (ret) {
		dev_err(&spi->dev, "chrdev alloc failed: %d\n", ret);
		goto err_sysfs;
	}

	cdev_init(&data->cdev, &bmi160_cdev_fops);
	data->cdev.owner = THIS_MODULE;
	ret = cdev_add(&data->cdev, data->devnum, 1);
	if (ret) {
		dev_err(&spi->dev, "cdev_add failed: %d\n", ret);
		goto err_chrdev;
	}

	data->cls = class_create("bmi160");
	if (IS_ERR(data->cls)) {
		ret = PTR_ERR(data->cls);
		dev_err(&spi->dev, "class_create failed: %d\n", ret);
		goto err_cdev;
	}

	data->chrdev = device_create(data->cls, &spi->dev, data->devnum,
				     NULL, "bmi160");
	if (IS_ERR(data->chrdev)) {
		ret = PTR_ERR(data->chrdev);
		dev_err(&spi->dev, "device_create failed: %d\n", ret);
		goto err_class;
	}

	dev_info(&spi->dev, "BMI160 SPI driver ready (/dev/bmi160)\n");
	return 0;

err_class:
	class_destroy(data->cls);
err_cdev:
	cdev_del(&data->cdev);
err_chrdev:
	unregister_chrdev_region(data->devnum, 1);
err_sysfs:
	sysfs_remove_group(&spi->dev.kobj, &bmi160_attr_group);
	return ret;
}

static void bmi160_spi_remove(struct spi_device *spi)
{
	struct bmi160_data *data = spi_get_drvdata(spi);

	device_destroy(data->cls, data->devnum);
	class_destroy(data->cls);
	cdev_del(&data->cdev);
	unregister_chrdev_region(data->devnum, 1);
	sysfs_remove_group(&spi->dev.kobj, &bmi160_attr_group);

	dev_info(&spi->dev, "BMI160 SPI driver removed\n");
}

/* ---------- driver registration ---------- */

static const struct of_device_id bmi160_of_match[] = {
	{ .compatible = "bosch,bmi160" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, bmi160_of_match);

static const struct spi_device_id bmi160_spi_id[] = {
	{ "bmi160", 0 },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(spi, bmi160_spi_id);

static struct spi_driver bmi160_spi_driver = {
	.driver = {
		.name           = "bmi160_spi",
		.of_match_table = bmi160_of_match,
	},
	.probe    = bmi160_spi_probe,
	.remove   = bmi160_spi_remove,
	.id_table = bmi160_spi_id,
};
module_spi_driver(bmi160_spi_driver);

MODULE_AUTHOR("Obuda University Embedded Systems Lab");
MODULE_DESCRIPTION("SPI driver for Bosch BMI160 6-axis IMU (teaching)");
MODULE_LICENSE("GPL");
