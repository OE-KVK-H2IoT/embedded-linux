// SPDX-License-Identifier: GPL-2.0
/*
 * MCP9808 Temperature Sensor I2C Character Device Driver
 *
 * Reads the MCP9808 temperature register over I2C and exposes the value
 * as a human-readable string through /dev/mcp9808.
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/uaccess.h>

#define MCP9808_TEMP_REG	0x05
#define DRIVER_NAME		"mcp9808"

struct mcp9808_data {
	struct i2c_client *client;
	struct cdev cdev;
	dev_t devnum;
	struct class *class;
	struct device *device;
};

static struct mcp9808_data *mcp9808_dev;

static ssize_t mcp9808_read(struct file *file, char __user *buf,
			    size_t count, loff_t *offset)
{
	s32 raw;
	int temp;
	char temp_str[32];
	int len;

	if (*offset > 0)
		return 0;

	raw = i2c_smbus_read_word_swapped(mcp9808_dev->client,
					  MCP9808_TEMP_REG);
	if (raw < 0)
		return raw;

	/* Mask out alert flag bits (upper 3 bits) */
	raw &= 0x1FFF;

	/* Convert to millidegrees Celsius (0.0625 C per LSB = 625/10) */
	if (raw & 0x1000)
		temp = (raw - 0x2000) * 625 / 10;
	else
		temp = raw * 625 / 10;

	len = snprintf(temp_str, sizeof(temp_str), "%d.%04d\n",
		       temp / 10000, abs(temp % 10000));

	if (count < len)
		len = count;

	if (copy_to_user(buf, temp_str, len))
		return -EFAULT;

	*offset += len;
	return len;
}

static const struct file_operations mcp9808_fops = {
	.owner = THIS_MODULE,
	.read  = mcp9808_read,
};

static int mcp9808_probe(struct i2c_client *client)
{
	int ret;

	mcp9808_dev = devm_kzalloc(&client->dev, sizeof(*mcp9808_dev),
				   GFP_KERNEL);
	if (!mcp9808_dev)
		return -ENOMEM;

	mcp9808_dev->client = client;

	ret = alloc_chrdev_region(&mcp9808_dev->devnum, 0, 1, DRIVER_NAME);
	if (ret < 0)
		return ret;

	cdev_init(&mcp9808_dev->cdev, &mcp9808_fops);
	mcp9808_dev->cdev.owner = THIS_MODULE;

	ret = cdev_add(&mcp9808_dev->cdev, mcp9808_dev->devnum, 1);
	if (ret < 0)
		goto err_cdev;

	mcp9808_dev->class = class_create(DRIVER_NAME);
	if (IS_ERR(mcp9808_dev->class)) {
		ret = PTR_ERR(mcp9808_dev->class);
		goto err_class;
	}

	mcp9808_dev->device = device_create(mcp9808_dev->class, NULL,
					    mcp9808_dev->devnum, NULL,
					    DRIVER_NAME);
	if (IS_ERR(mcp9808_dev->device)) {
		ret = PTR_ERR(mcp9808_dev->device);
		goto err_device;
	}

	dev_info(&client->dev, "mcp9808 registered as /dev/mcp9808\n");
	return 0;

err_device:
	class_destroy(mcp9808_dev->class);
err_class:
	cdev_del(&mcp9808_dev->cdev);
err_cdev:
	unregister_chrdev_region(mcp9808_dev->devnum, 1);
	return ret;
}

static void mcp9808_remove(struct i2c_client *client)
{
	device_destroy(mcp9808_dev->class, mcp9808_dev->devnum);
	class_destroy(mcp9808_dev->class);
	cdev_del(&mcp9808_dev->cdev);
	unregister_chrdev_region(mcp9808_dev->devnum, 1);
	dev_info(&client->dev, "mcp9808: driver removed\n");
}

static const struct i2c_device_id mcp9808_id[] = {
	{ "mcp9808", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, mcp9808_id);

static const struct of_device_id mcp9808_of_match[] = {
	{ .compatible = "mcp9808" },
	{ }
};
MODULE_DEVICE_TABLE(of, mcp9808_of_match);

static struct i2c_driver mcp9808_i2c_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = mcp9808_of_match,
	},
	.probe    = mcp9808_probe,
	.remove   = mcp9808_remove,
	.id_table = mcp9808_id,
};

module_i2c_driver(mcp9808_i2c_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("MCP9808 Temperature Sensor Driver");
