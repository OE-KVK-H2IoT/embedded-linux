// SPDX-License-Identifier: GPL-2.0
/*
 * SSD1306 I2C OLED Framebuffer Driver
 *
 * Provides /dev/fbN for a 128x64 (or 128x32) monochrome SSD1306 OLED
 * display connected over I2C. Uses fbdev deferred I/O to periodically
 * push the framebuffer contents to the display.
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/fb.h>
#include <linux/delay.h>
#include <linux/slab.h>

#define DRIVER_NAME		"ssd1306fb"
#define SSD1306_WIDTH		128
#define SSD1306_HEIGHT		64
#define SSD1306_PAGES		(SSD1306_HEIGHT / 8)
#define SSD1306_BUFSIZE		(SSD1306_WIDTH * SSD1306_PAGES)

/* I2C control bytes */
#define SSD1306_CMD		0x00
#define SSD1306_DATA		0x40

struct ssd1306_par {
	struct i2c_client *client;
	u32 width;
	u32 height;
	u8 *buffer;	/* hardware-format buffer (page-oriented) */
};

/* --- Low-level I2C helpers --- */

static int ssd1306_write_cmd(struct i2c_client *client, u8 cmd)
{
	return i2c_smbus_write_byte_data(client, SSD1306_CMD, cmd);
}

static int ssd1306_write_data(struct i2c_client *client, const u8 *data,
			      size_t len)
{
	u8 *buf;
	int ret;

	/* I2C message: control byte (0x40) + data bytes */
	buf = kmalloc(len + 1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	buf[0] = SSD1306_DATA;
	memcpy(buf + 1, data, len);

	ret = i2c_master_send(client, buf, len + 1);
	kfree(buf);

	return ret < 0 ? ret : 0;
}

/* --- Display initialisation --- */

static const u8 ssd1306_init_cmds[] = {
	0xAE,		/* Display OFF */
	0xD5, 0x80,	/* Set display clock divide ratio/oscillator frequency */
	0xA8, 0x3F,	/* Set multiplex ratio (63 for 64 rows) */
	0xD3, 0x00,	/* Set display offset: none */
	0x40,		/* Set display start line to 0 */
	0x8D, 0x14,	/* Enable charge pump */
	0x20, 0x00,	/* Set memory addressing mode: horizontal */
	0xA1,		/* Set segment re-map: column 127 mapped to SEG0 */
	0xC8,		/* Set COM output scan direction: remapped */
	0xDA, 0x12,	/* Set COM pins hardware configuration */
	0x81, 0xCF,	/* Set contrast control */
	0xD9, 0xF1,	/* Set pre-charge period */
	0xDB, 0x40,	/* Set VCOMH deselect level */
	0xA4,		/* Set entire display ON (resume from GDDRAM) */
	0xA6,		/* Set normal display (not inverted) */
	0xAF,		/* Display ON */
};

static int ssd1306_init_display(struct ssd1306_par *par)
{
	struct i2c_client *client = par->client;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(ssd1306_init_cmds); i++) {
		ret = ssd1306_write_cmd(client, ssd1306_init_cmds[i]);
		if (ret < 0)
			return ret;
	}

	/* Adjust multiplex ratio for 32-row displays */
	if (par->height == 32) {
		ssd1306_write_cmd(client, 0xA8);
		ssd1306_write_cmd(client, 0x1F);  /* 31 */
		ssd1306_write_cmd(client, 0xDA);
		ssd1306_write_cmd(client, 0x02);  /* Sequential COM, no remap */
	}

	return 0;
}

/* --- Framebuffer update --- */

static void ssd1306_update_display(struct ssd1306_par *par)
{
	struct fb_info *info = dev_get_drvdata(&par->client->dev);
	u8 *vmem = info->screen_base;
	int x, y, page;
	int pages = par->height / 8;
	int buf_size = par->width * pages;

	memset(par->buffer, 0, buf_size);

	/*
	 * Convert from linear 1bpp framebuffer (row-major, LSB=leftmost pixel,
	 * matching mainline ssd1307fb convention) to SSD1306 page format
	 * (each byte = 8 vertical pixels in a column).
	 */
	for (y = 0; y < par->height; y++) {
		page = y / 8;
		for (x = 0; x < par->width; x++) {
			int src_byte = (y * par->width + x) / 8;
			int src_bit = x % 8;

			if (vmem[src_byte] & (1 << src_bit))
				par->buffer[page * par->width + x] |=
					(1 << (y % 8));
		}
	}

	/* Set column and page address range */
	ssd1306_write_cmd(par->client, 0x21);  /* Column address */
	ssd1306_write_cmd(par->client, 0);
	ssd1306_write_cmd(par->client, par->width - 1);
	ssd1306_write_cmd(par->client, 0x22);  /* Page address */
	ssd1306_write_cmd(par->client, 0);
	ssd1306_write_cmd(par->client, pages - 1);

	/* Send pixel data */
	ssd1306_write_data(par->client, par->buffer, buf_size);
}

/* --- Deferred I/O callback --- */

static void ssd1306_deferred_io(struct fb_info *info, struct list_head *pagelist)
{
	struct ssd1306_par *par = info->par;

	ssd1306_update_display(par);
}

static struct fb_deferred_io ssd1306_defio = {
	.delay		= HZ / 20,	/* 50 ms refresh interval (20 fps) */
	.deferred_io	= ssd1306_deferred_io,
};

/* --- fb_ops --- */

static const struct fb_ops ssd1306_fbops = {
	.owner		= THIS_MODULE,
	.fb_read	= fb_sys_read,
	.fb_write	= fb_sys_write,
	.fb_fillrect	= sys_fillrect,
	.fb_copyarea	= sys_copyarea,
	.fb_imageblit	= sys_imageblit,
};

/* --- Probe / Remove --- */

static int ssd1306_probe(struct i2c_client *client)
{
	struct fb_info *info;
	struct ssd1306_par *par;
	u32 width = SSD1306_WIDTH;
	u32 height = SSD1306_HEIGHT;
	int vmem_size;
	u8 *vmem;
	int ret;

	/* Read dimensions from Device Tree if available */
	of_property_read_u32(client->dev.of_node, "width", &width);
	of_property_read_u32(client->dev.of_node, "height", &height);

	vmem_size = width * height / 8;

	info = framebuffer_alloc(sizeof(struct ssd1306_par), &client->dev);
	if (!info)
		return -ENOMEM;

	par = info->par;
	par->client = client;
	par->width = width;
	par->height = height;

	par->buffer = devm_kzalloc(&client->dev, width * (height / 8),
				   GFP_KERNEL);
	if (!par->buffer) {
		ret = -ENOMEM;
		goto err_alloc;
	}

	vmem = devm_kzalloc(&client->dev, vmem_size, GFP_KERNEL);
	if (!vmem) {
		ret = -ENOMEM;
		goto err_alloc;
	}

	info->fbops = &ssd1306_fbops;
	info->screen_base = vmem;
	info->screen_size = vmem_size;
	info->fbdefio = &ssd1306_defio;

	info->fix.type		= FB_TYPE_PACKED_PIXELS;
	info->fix.visual	= FB_VISUAL_MONO10;
	info->fix.line_length	= width / 8;
	info->fix.smem_len	= vmem_size;
	strscpy(info->fix.id, DRIVER_NAME, sizeof(info->fix.id));

	info->var.xres		= width;
	info->var.yres		= height;
	info->var.xres_virtual	= width;
	info->var.yres_virtual	= height;
	info->var.bits_per_pixel = 1;
	info->var.red.length	= 1;
	info->var.green.length	= 1;
	info->var.blue.length	= 1;

	fb_deferred_io_init(info);

	ret = ssd1306_init_display(par);
	if (ret < 0) {
		dev_err(&client->dev, "Failed to initialise display\n");
		goto err_init;
	}

	ret = register_framebuffer(info);
	if (ret < 0) {
		dev_err(&client->dev, "Failed to register framebuffer\n");
		goto err_init;
	}

	dev_set_drvdata(&client->dev, info);
	dev_info(&client->dev, "ssd1306fb: %ux%u OLED registered as /dev/fb%d\n",
		 width, height, info->node);

	return 0;

err_init:
	fb_deferred_io_cleanup(info);
err_alloc:
	framebuffer_release(info);
	return ret;
}

static void ssd1306_remove(struct i2c_client *client)
{
	struct fb_info *info = dev_get_drvdata(&client->dev);

	unregister_framebuffer(info);
	fb_deferred_io_cleanup(info);

	/* Turn display off */
	ssd1306_write_cmd(client, 0xAE);

	framebuffer_release(info);
	dev_info(&client->dev, "ssd1306fb: driver removed\n");
}

/* --- I2C matching --- */

static const struct i2c_device_id ssd1306_id[] = {
	{ "ssd1306fb", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ssd1306_id);

static const struct of_device_id ssd1306_of_match[] = {
	{ .compatible = "ssd1306fb" },
	{ }
};
MODULE_DEVICE_TABLE(of, ssd1306_of_match);

static struct i2c_driver ssd1306_i2c_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = ssd1306_of_match,
	},
	.probe    = ssd1306_probe,
	.remove   = ssd1306_remove,
	.id_table = ssd1306_id,
};

module_i2c_driver(ssd1306_i2c_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("SSD1306 I2C OLED Framebuffer Driver");
