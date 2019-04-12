/*
 * Copyright (c) 2019 Michel Heily <michel.heily@gmail.com>
 * Based-Of SSD1673 by PHYTEC Messtechnik GmbH.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_LEVEL CONFIG_DISPLAY_LOG_LEVEL
#include <logging/log.h>
LOG_MODULE_REGISTER(il0373);

#include <string.h>
#include <device.h>
#include <display.h>
#include <init.h>
#include <gpio.h>
#include <spi.h>
#include <misc/byteorder.h>

#include "display_il0373.h"
#include <display/cfb.h>


#define EPD_PANEL_WIDTH					DT_GD_IL0373_0_WIDTH
#define EPD_PANEL_HEIGHT				DT_GD_IL0373_0_HEIGHT
#define EPD_PANEL_NUMOF_COLUMS			EPD_PANEL_WIDTH
#define EPD_PANEL_NUMOF_ROWS_PER_PAGE	8
#define EPD_PANEL_NUMOF_PAGES			(EPD_PANEL_HEIGHT / EPD_PANEL_NUMOF_ROWS_PER_PAGE)

struct il0373_data {
	struct device *reset;
	struct device *dc;
	struct device *busy;
	struct device *spi_dev;
	struct spi_config spi_config;
#if defined(DT_GD_IL0373_0_CS_GPIO_CONTROLLER)
	struct spi_cs_control cs_ctrl;
#endif
	u8_t scan_mode;
};

#define SENDING_COMMAND(driver) gpio_pin_write((driver)->dc, DT_GD_IL0373_0_DC_GPIOS_PIN, 0)
#define SENDING_DATA(driver) 	gpio_pin_write((driver)->dc, DT_GD_IL0373_0_DC_GPIOS_PIN, 1)

static inline int il0373_write_cmd(struct il0373_data *driver,
				    u8_t cmd, u8_t *data, size_t len)
{
	int err;
	struct spi_buf buf = {.buf = &cmd, .len = sizeof(cmd)};
	struct spi_buf_set buf_set = {.buffers = &buf, .count = 1};

	SENDING_COMMAND(driver);
	err = spi_write(driver->spi_dev, &driver->spi_config, &buf_set);
	if (err < 0) {
		return err;
	}

	if (data != NULL) {
		buf.buf = data;
		buf.len = len;
		SENDING_DATA(driver);
		err = spi_write(driver->spi_dev, &driver->spi_config, &buf_set);
		if (err < 0) {
			return err;
		}
	}

	return 0;
}


static inline void il0373_busy_wait(struct il0373_data *driver)
{
	u32_t val = 0U;

	gpio_pin_read(driver->busy, DT_GD_IL0373_0_BUSY_GPIOS_PIN, &val);
	while (val) {
		k_sleep(1);
		gpio_pin_read(driver->busy, DT_GD_IL0373_0_BUSY_GPIOS_PIN, &val);
	}
}

int il0373_resume(const struct device *dev)
{
	struct il0373_data *driver = dev->driver_data;

	return il0373_write_cmd(driver, IL0373_CMD_POWER_ON,
				 			NULL, 0);
}

static int il0373_suspend(const struct device *dev)
{
	struct il0373_data *driver = dev->driver_data;

	return il0373_write_cmd(driver, IL0373_CMD_POWER_OFF,
				 			NULL, 0);
}

static int il0373_update_display(const struct device *dev)
{
	struct il0373_data *driver = dev->driver_data;
	int err;

	err = il0373_write_cmd(driver, IL0373_CMD_DISPLAY_REFRESH, NULL, 0);
	if (err < 0) {
		return err;
	}

	k_sleep(50);

	il0373_busy_wait(driver);

	return 0;
}

static int il0373_write(const struct device *dev, const u16_t x,
			 const u16_t y,
			 const struct display_buffer_descriptor *desc,
			 const void *buf)
{
	struct il0373_data *driver = dev->driver_data;
	int err;

	if (desc->pitch < desc->width) {
		LOG_ERR("Pitch is smaller than width");
		return -EINVAL;
	}

	if (buf == NULL || desc->buf_size == 0) {
		LOG_ERR("Display buffer is not available");
		return -EINVAL;
	}

	if (desc->pitch > desc->width) {
		LOG_ERR("Unsupported mode");
		return -ENOTSUP;
	}

	if ((y + desc->height) > EPD_PANEL_HEIGHT) {
		LOG_ERR("Buffer out of bounds (height)");
		return -EINVAL;
	}

	if ((x + desc->width) > EPD_PANEL_WIDTH) {
		LOG_ERR("Buffer out of bounds (width)");
		return -EINVAL;
	}

	if ((desc->height % EPD_PANEL_NUMOF_ROWS_PER_PAGE) != 0) {
		LOG_ERR("Buffer height not multiple of %d",
				EPD_PANEL_NUMOF_ROWS_PER_PAGE);
		return -EINVAL;
	}

	if ((y % EPD_PANEL_NUMOF_ROWS_PER_PAGE) != 0) {
		LOG_ERR("Y coordinate not multiple of %d",
				EPD_PANEL_NUMOF_ROWS_PER_PAGE);
		return -EINVAL;
	}


	LOG_INF("write request, x=%d, y=%d, buf_size=%d", x, y, desc->buf_size);

	u8_t transposed_buf[EPD_PANEL_WIDTH * EPD_PANEL_HEIGHT / 8] = { 0 };
	for (size_t y = 0; y < EPD_PANEL_HEIGHT; y++) {
		for (size_t x = 0; x < EPD_PANEL_WIDTH; x++) {
		        size_t bitmap_idx = (y / 8) * EPD_PANEL_WIDTH + x;
		        u8_t bit = 7 - (y % 8);
			if (((u8_t*)buf)[bitmap_idx] & (1 << bit)) {
				transposed_buf[((EPD_PANEL_WIDTH - 1 - x) * EPD_PANEL_HEIGHT + y) / 8] |= 1 << (7 - (y % 8));
			}
		}
	}

	err = il0373_write_cmd(driver, IL0373_CMD_DTM2, transposed_buf, desc->buf_size);
	if (err < 0) {
		return err;
	}

	return il0373_update_display(dev);
}

static int il0373_read(const struct device *dev, const u16_t x,
			const u16_t y,
			const struct display_buffer_descriptor *desc,
			void *buf)
{
	LOG_ERR("not supported");
	return -ENOTSUP;
}

static void *il0373_get_framebuffer(const struct device *dev)
{
	LOG_ERR("not supported");
	return NULL;
}

static int il0373_set_brightness(const struct device *dev,
				  const u8_t brightness)
{
	LOG_WRN("not supported");
	return -ENOTSUP;
}

static int il0373_set_contrast(const struct device *dev, u8_t contrast)
{
	LOG_WRN("not supported");
	return -ENOTSUP;
}

static void il0373_get_capabilities(const struct device *dev,
				     struct display_capabilities *caps)
{
	memset(caps, 0, sizeof(struct display_capabilities));
	caps->x_resolution = EPD_PANEL_WIDTH;
	caps->y_resolution = EPD_PANEL_HEIGHT;
	caps->supported_pixel_formats = PIXEL_FORMAT_MONO10;
	caps->current_pixel_format = PIXEL_FORMAT_MONO10;
	caps->screen_info = SCREEN_INFO_MONO_VTILED |
			    SCREEN_INFO_MONO_MSB_FIRST |
			    SCREEN_INFO_EPD |
			    SCREEN_INFO_DOUBLE_BUFFER;
}

static int il0373_set_orientation(const struct device *dev,
				   const enum display_orientation
				   orientation)
{
	LOG_ERR("Unsupported");
	return -ENOTSUP;
}

static int il0373_set_pixel_format(const struct device *dev,
				    const enum display_pixel_format pf)
{
	if (pf == PIXEL_FORMAT_MONO10) {
		return 0;
	}

	LOG_ERR("not supported");
	return -ENOTSUP;
}

static int il0373_clear_and_write_buffer(struct device *dev)
{
	int err;
	static u8_t clear_page[EPD_PANEL_WIDTH];
	u8_t page;
	struct spi_buf sbuf;
	struct spi_buf_set buf_set = {.buffers = &sbuf, .count = 1};
	struct il0373_data *driver = dev->driver_data;

	err = il0373_write_cmd(driver, IL0373_CMD_DTM1, NULL, 0);
	if (err < 0) {
		return err;
	}

	memset(clear_page, 0xff, sizeof(clear_page));
	sbuf.buf = clear_page;
	sbuf.len = sizeof(clear_page);
	for (page = 0; page <= EPD_PANEL_NUMOF_PAGES; ++page) {
		SENDING_DATA(driver);
		err = spi_write(driver->spi_dev, &driver->spi_config, &buf_set);
		if (err < 0) {
			return err;
		}
	}

	err = il0373_write_cmd(driver, IL0373_CMD_DTM2, NULL, 0);
	if (err < 0) {
		return err;
	}


	memset(clear_page, 0xff, sizeof(clear_page));
	sbuf.buf = clear_page;
	sbuf.len = sizeof(clear_page);
	for (page = 0; page <= EPD_PANEL_NUMOF_PAGES; ++page) {
		SENDING_DATA(driver);
		err = spi_write(driver->spi_dev, &driver->spi_config, &buf_set);
		if (err < 0) {
			return err;
		}
	}

	il0373_update_display(dev);

	return 0;
}

static int il0373_send_lut(struct il0373_data *driver, u8_t lut_id, u8_t b0, u8_t b6, u8_t b18)
{
	struct spi_buf sbuf;
	struct spi_buf_set buf_set = {.buffers = &sbuf, .count = 1};
	int err;
	int i;

	u8_t padding = (lut_id == 0x20) ? 20 : 18;

	u8_t lut_data[] = {
		b0, 0x08, 0x00, 0x00, 0x00, 0x02, b6, 0x28, 0x28, 0x00, 0x00, 0x01,
		b0, 0x14, 0x00, 0x00, 0x00, 0x01, b18, 0x12, 0x12, 0x00, 0x00, 0x01
  	};

	err = il0373_write_cmd(driver, lut_id, NULL, 0);
	if (err < 0) {
		return err;
	}

	sbuf.buf = lut_data;
	sbuf.len = sizeof(lut_data);
	SENDING_DATA(driver);
	err = spi_write(driver->spi_dev, &driver->spi_config, &buf_set);
	if (err < 0) {
		return err;
	}

	u8_t zero = 0;
	sbuf.buf = &zero;
	sbuf.len = 1;

	SENDING_DATA(driver);
	for (i = 0; i < padding; ++i) {
		err = spi_write(driver->spi_dev, &driver->spi_config, &buf_set);
		if (err < 0) {
			return err;
		}
	}

	return 0;
}

static int il0373_controller_init(struct device *dev)
{
	int err;
	u8_t tmp[6];
	struct il0373_data *driver = dev->driver_data;

	LOG_INF("EPD width=%d height=%d", EPD_PANEL_WIDTH, EPD_PANEL_HEIGHT);

	LOG_INF("EPD Reset");

	gpio_pin_write(driver->reset, DT_GD_IL0373_0_RESET_GPIOS_PIN, 0);
	k_sleep(20);
	gpio_pin_write(driver->reset, DT_GD_IL0373_0_RESET_GPIOS_PIN, 1);
	k_sleep(20);

	LOG_INF("EPD Power settings");

	tmp[0] = 0x03;
	tmp[1] = 0x00;
	tmp[2] = 0x2b;
	tmp[3] = 0x2b;
	tmp[4] = 0x03;
	err = il0373_write_cmd(driver, IL0373_CMD_POWER_SETTING, tmp, 5);
	if (err < 0) {
		return err;
	}


	LOG_INF("EPD Booster start");
	tmp[0] = 0x17;
	tmp[1] = 0x17;
	tmp[2] = 0x17;
	err = il0373_write_cmd(driver, IL0373_CMD_BOOSTER_SOFT_START, tmp, 3);
	if (err < 0) {
		return err;
	}


	LOG_INF("EPD Panel settings");
	tmp[0] = 0xbf;
	tmp[1] = 0x0d; // VCOM to 0V fast
	err = il0373_write_cmd(driver, IL0373_CMD_PANEL_SETTING, tmp, 2);
	if (err < 0) {
		return err;
	}

	LOG_INF("EPD PLL");
	tmp[0] = 0x3a; // 100HZ
	err = il0373_write_cmd(driver, IL0373_CMD_PLL, tmp, 1);
	if (err < 0) {
		return err;
	}


	LOG_INF("EPD Resolution");
	tmp[0] = EPD_PANEL_HEIGHT;
	tmp[1] = EPD_PANEL_WIDTH >> 8;
	tmp[2] = EPD_PANEL_WIDTH & 0xff;
	err = il0373_write_cmd(driver, IL0373_CMD_RESOLUTION, tmp, 3);
	if (err < 0) {
		return err;
	}

	tmp[0] = 0x08;
	err = il0373_write_cmd(driver, IL0373_CMD_VCM_DC_SETTING, tmp, 1);
	if (err < 0) {
		return err;
	}

	tmp[0] = 0x97;
	err = il0373_write_cmd(driver, IL0373_CMD_CDI, tmp, 1);
	if (err < 0) {
		return err;
	}


	// il0373_set_orientation_internall(driver);

	/* send partial lut */
	LOG_INF("EPD Sending lut");
	err = il0373_send_lut(driver, 0x20, 0x00, 0x60, 0x00);
	if (err < 0) {
		return err;
	}

	err = il0373_send_lut(driver, 0x21, 0x40, 0x90, 0xa0);
	if (err < 0) {
		return err;
	}

	err = il0373_send_lut(driver, 0x22, 0x40, 0x90, 0xa0);
	if (err < 0) {
		return err;
	}

	err = il0373_send_lut(driver, 0x23, 0x80, 0x90, 0x50);
	if (err < 0) {
		return err;
	}

	err = il0373_send_lut(driver, 0x24, 0x80, 0x90, 0x50);
	if (err < 0) {
		return err;
	}

	LOG_INF("EPD PowerOn");

	err = il0373_write_cmd(driver, IL0373_CMD_POWER_ON, NULL, 0);
	if (err < 0) {
		return err;
	}

	il0373_busy_wait(driver);

	return il0373_clear_and_write_buffer(dev);
}

static int il0373_init(struct device *dev)
{
	struct il0373_data *driver = dev->driver_data;

	driver->spi_dev = device_get_binding(DT_GD_IL0373_0_BUS_NAME);
	if (driver->spi_dev == NULL) {
		LOG_ERR("Could not get SPI device for IL0373");
		return -EIO;
	}

	driver->spi_config.frequency = DT_GD_IL0373_0_SPI_MAX_FREQUENCY;
	driver->spi_config.operation = SPI_OP_MODE_MASTER | SPI_WORD_SET(8);
	driver->spi_config.slave = DT_GD_IL0373_0_BASE_ADDRESS;
	driver->spi_config.cs = NULL;

	driver->reset = device_get_binding(DT_GD_IL0373_0_RESET_GPIOS_CONTROLLER);
	if (driver->reset == NULL) {
		LOG_ERR("Could not get GPIO port for IL0373 reset");
		return -EIO;
	}

	gpio_pin_configure(driver->reset, DT_GD_IL0373_0_RESET_GPIOS_PIN,
			   GPIO_DIR_OUT);

	driver->dc = device_get_binding(DT_GD_IL0373_0_DC_GPIOS_CONTROLLER);
	if (driver->dc == NULL) {
		LOG_ERR("Could not get GPIO port for IL0373 DC signal");
		return -EIO;
	}

	gpio_pin_configure(driver->dc, DT_GD_IL0373_0_DC_GPIOS_PIN,
			   GPIO_DIR_OUT);

	driver->busy = device_get_binding(DT_GD_IL0373_0_BUSY_GPIOS_CONTROLLER);
	if (driver->busy == NULL) {
		LOG_ERR("Could not get GPIO port for IL0373 busy signal");
		return -EIO;
	}

	gpio_pin_configure(driver->busy, DT_GD_IL0373_0_BUSY_GPIOS_PIN,
			   GPIO_DIR_IN);

#if defined(DT_GD_IL0373_0_CS_GPIO_CONTROLLER)
	driver->cs_ctrl.gpio_dev = device_get_binding(
		DT_GD_IL0373_0_CS_GPIO_CONTROLLER);
	if (!driver->cs_ctrl.gpio_dev) {
		LOG_ERR("Unable to get SPI GPIO CS device");
		return -EIO;
	}

	driver->cs_ctrl.gpio_pin = DT_GD_IL0373_0_CS_GPIO_PIN;
	driver->cs_ctrl.delay = 0;
	driver->spi_config.cs = &driver->cs_ctrl;
#endif

	return il0373_controller_init(dev);
}

static struct il0373_data il0373_driver;

static struct display_driver_api il0373_driver_api = {
	.blanking_on = il0373_resume,
	.blanking_off = il0373_suspend,
	.write = il0373_write,
	.read = il0373_read,
	.get_framebuffer = il0373_get_framebuffer,
	.set_brightness = il0373_set_brightness,
	.set_contrast = il0373_set_contrast,
	.get_capabilities = il0373_get_capabilities,
	.set_pixel_format = il0373_set_pixel_format,
	.set_orientation = il0373_set_orientation,
};


DEVICE_AND_API_INIT(il0373, DT_GD_IL0373_0_LABEL, il0373_init,
		    &il0373_driver, NULL,
		    POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY,
		    &il0373_driver_api);
