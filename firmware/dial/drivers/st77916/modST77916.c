/*
 * ST77916 QSPI display driver for the Moddable SDK.
 *
 * Rather than adapt another controller's driver, this wraps Espressif's real
 * `esp_lcd_st77916` vendor component (vendored alongside as
 * esp_lcd_st77916_spi.c + headers). The vendor code owns everything
 * panel-specific: the QSPI opcode encoding (0x02<<24 cmd / 0x32<<24 color),
 * CASET/RASET windowing, COLMOD, MADCTL, and the init walk. This file just:
 *
 *   - builds the SPI bus + panel-IO + panel handle in the constructor, and
 *   - blits each Poco band with esp_lcd_panel_draw_bitmap() in send().
 *
 * Transfers are SYNCHRONOUS: on_color_trans_done gives a binary semaphore that
 * send() takes right after draw_bitmap(), so the caller's band buffer is never
 * reused while DMA is still reading it (the failure mode of the earlier async
 * adaptation: banding + fade-to-black on this write-once IPS panel).
 *
 * Board target supplies the pins/dims via manifest defines (MODDEF_ST77916_*).
 * Backlight (GPIO47) is handled in the board setup-target, not here.
 */

#include "xsmc.h"
#include "xsHost.h"

#include "commodettoBitmap.h"
#include "commodettoPocoBlit.h"
#include "commodettoPixelsOut.h"
#include "mc.xs.h"
#include "mc.defines.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_heap_caps.h"
#include "esp_lcd_types.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

#include "esp_lcd_st77916.h"
#include "esp_lcd_st77916_interface.h"		// esp_lcd_new_panel_st77916_spi (QSPI entry point)

#if !defined(MODDEF_ST77916_CS_PIN) || !defined(MODDEF_ST77916_SCK_PIN)
	#error ST77916 CS and SCK pins must be defined
#endif
#if !defined(MODDEF_ST77916_DATA0_PIN) || !defined(MODDEF_ST77916_DATA1_PIN) || !defined(MODDEF_ST77916_DATA2_PIN) || !defined(MODDEF_ST77916_DATA3_PIN)
	#error ST77916 QSPI data pins (DATA0-DATA3) must be defined
#endif

#ifndef MODDEF_ST77916_HZ
	#define MODDEF_ST77916_HZ (40000000)
#endif
#ifndef MODDEF_ST77916_RST_PIN
	#define MODDEF_ST77916_RST_PIN (-1)
#endif
#ifndef MODDEF_ST77916_SPI_PORT
	#define MODDEF_ST77916_SPI_PORT SPI2_HOST
#endif

/* Poco hands us at most (width << 5) pixels per band; size DMA for a few more. */
#define ST77916_MAX_TRANSFER (MODDEF_ST77916_WIDTH * 40 * (int)sizeof(uint16_t))

typedef struct {
	PixelsOutDispatch			dispatch;

	esp_lcd_panel_io_handle_t	io_handle;
	esp_lcd_panel_handle_t		panel;
	SemaphoreHandle_t			colorReady;		// given by on_color_trans_done

	uint8_t						*bounce;		// internal DMA-capable RAM staging buffer
	int							bounceBytes;

	int							updateX;
	int							updateWidth;
	int							curY;			// next row to write within the region
	uint8_t						rotation;		// 0/1/2/3 => 0/90/180/270 (stored only)
} st77916DisplayRecord, *st77916Display;

/* Bounce buffer holds up to this many scanlines (Poco hands us <= width<<5 px). */
#define ST77916_BOUNCE_LINES (40)

static void st77916Begin(void *refcon, CommodettoCoordinate x, CommodettoCoordinate y, CommodettoDimension w, CommodettoDimension h);
static void st77916End(void *refcon);
static void st77916Continue(void *refcon);
static void st77916Send(PocoPixel *pixels, int byteLength, void *refcon);
static void st77916AdaptInvalid(void *refcon, CommodettoRectangle r);

static const PixelsOutDispatchRecord gPixelsOutDispatch ICACHE_RODATA_ATTR = {
	st77916Begin,
	st77916Continue,
	st77916End,
	st77916Send,
	st77916AdaptInvalid
};

// Init sequence selection:
//   0 = Espressif's built-in vendor_specific_init_default (in esp_lcd_st77916_spi.c),
//       used by ESP32_Display_Panel and the board demos — selected by passing NULL.
//   1 = our st77916_init.h table (ported from modi12jin, a different ST77916 board).
#define ST77916_USE_LOCAL_INIT (1)

#if ST77916_USE_LOCAL_INIT
	// Vendor init table (st77916_lcd_init_cmd_t st77916_init_cmds[]); needs esp_lcd_st77916.h above.
	#include "st77916_init.h"
#endif

static bool st77916ColorDone(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
	st77916Display sd = (st77916Display)user_ctx;
	BaseType_t higherPriorityTaskWoken = pdFALSE;
	xSemaphoreGiveFromISR(sd->colorReady, &higherPriorityTaskWoken);
	return higherPriorityTaskWoken == pdTRUE;
}

void xs_st77916_destructor(void *data)
{
	st77916Display sd = data;
	if (!data) return;

	if (sd->panel)
		esp_lcd_panel_del(sd->panel);
	if (sd->io_handle)
		esp_lcd_panel_io_del(sd->io_handle);
	if (sd->colorReady)
		vSemaphoreDelete(sd->colorReady);
	if (sd->bounce)
		heap_caps_free(sd->bounce);

	c_free(data);
}

void xs_st77916(xsMachine *the)
{
	st77916Display sd;
	esp_err_t err;

	xsmcVars(1);
	if (xsmcGet(xsVar(0), xsArg(0), xsID_pixelFormat)) {
		if (kCommodettoBitmapFormat != xsmcToInteger(xsVar(0)))
			xsUnknownError("bad format");
	}

	sd = c_calloc(1, sizeof(st77916DisplayRecord));
	if (!sd)
		xsUnknownError("no memory");
	xsmcSetHostData(xsThis, sd);
	sd->dispatch = (PixelsOutDispatch)&gPixelsOutDispatch;

	sd->colorReady = xSemaphoreCreateBinary();
	if (!sd->colorReady)
		xsUnknownError("no semaphore");

	// DMA reads pixels from here (internal, DMA-capable) — never from PSRAM, whose
	// cache coherency with SPI DMA on the S3 otherwise corrupts bands of scanlines.
	sd->bounceBytes = MODDEF_ST77916_WIDTH * ST77916_BOUNCE_LINES * (int)sizeof(uint16_t);
	sd->bounce = heap_caps_malloc(sd->bounceBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
	if (!sd->bounce)
		xsUnknownError("no DMA buffer");

	spi_bus_config_t buscfg = ST77916_PANEL_BUS_QSPI_CONFIG(
		MODDEF_ST77916_SCK_PIN,
		MODDEF_ST77916_DATA0_PIN,
		MODDEF_ST77916_DATA1_PIN,
		MODDEF_ST77916_DATA2_PIN,
		MODDEF_ST77916_DATA3_PIN,
		ST77916_MAX_TRANSFER);
	err = spi_bus_initialize(MODDEF_ST77916_SPI_PORT, &buscfg, SPI_DMA_CH_AUTO);
	if (ESP_OK != err)
		xsUnknownError("spi_bus_initialize failed");

	esp_lcd_panel_io_spi_config_t io_config = ST77916_PANEL_IO_QSPI_CONFIG(
		MODDEF_ST77916_CS_PIN, st77916ColorDone, sd);
	io_config.pclk_hz = MODDEF_ST77916_HZ;
	err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)MODDEF_ST77916_SPI_PORT, &io_config, &sd->io_handle);
	if (ESP_OK != err)
		xsUnknownError("esp_lcd_new_panel_io_spi failed");

	st77916_vendor_config_t vendor_config = {
#if ST77916_USE_LOCAL_INIT
		.init_cmds = st77916_init_cmds,
		.init_cmds_size = sizeof(st77916_init_cmds) / sizeof(st77916_lcd_init_cmd_t),
#else
		.init_cmds = NULL,			// use Espressif's vendor_specific_init_default
		.init_cmds_size = 0,
#endif
		.flags = {
			.use_qspi_interface = 1,
		},
	};
	esp_lcd_panel_dev_config_t panel_config = {
		.reset_gpio_num = MODDEF_ST77916_RST_PIN,
		.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
		.bits_per_pixel = 16,
		.vendor_config = &vendor_config,
	};
	err = esp_lcd_new_panel_st77916_spi(sd->io_handle, &panel_config, &sd->panel);
	if (ESP_OK != err)
		xsUnknownError("esp_lcd_new_panel_st77916_spi failed");

	esp_lcd_panel_reset(sd->panel);
	esp_lcd_panel_init(sd->panel);
	esp_lcd_panel_disp_on_off(sd->panel, true);
}

static void st77916Begin(void *refcon, CommodettoCoordinate x, CommodettoCoordinate y, CommodettoDimension w, CommodettoDimension h)
{
	st77916Display sd = refcon;
	sd->updateX = x;
	sd->updateWidth = w;
	sd->curY = y;
}

static void st77916Continue(void *refcon)
{
}

static void st77916End(void *refcon)
{
}

static void st77916AdaptInvalid(void *refcon, CommodettoRectangle r)
{
}

static void st77916Send(PocoPixel *pixels, int byteLength, void *refcon)
{
	st77916Display sd = refcon;
	int w = sd->updateWidth;
	int bytesPerLine, maxLines, totalLines;
	uint8_t *src = (uint8_t *)pixels;

	if (byteLength < 0)
		byteLength = -byteLength;
	if (w <= 0)
		return;

	bytesPerLine = w << 1;				// RGB565 => 2 bytes/pixel
	totalLines = (byteLength >> 1) / w;
	maxLines = sd->bounceBytes / bytesPerLine;
	if ((totalLines <= 0) || (maxLines <= 0))
		return;

	// Stage each chunk into internal DMA RAM, then blit. x_end/y_end are exclusive.
	while (totalLines > 0) {
		int chunk = (totalLines < maxLines) ? totalLines : maxLines;
		int chunkBytes = chunk * bytesPerLine;

		c_memcpy(sd->bounce, src, chunkBytes);
		esp_lcd_panel_draw_bitmap(sd->panel, sd->updateX, sd->curY,
			sd->updateX + w, sd->curY + chunk, sd->bounce);
		// Block until this band's DMA completes before reusing the bounce buffer.
		xSemaphoreTake(sd->colorReady, portMAX_DELAY);

		sd->curY += chunk;
		src += chunkBytes;
		totalLines -= chunk;
	}
}

void xs_st77916_begin(xsMachine *the)
{
	st77916Display sd = xsmcGetHostData(xsThis);
	CommodettoCoordinate x = (CommodettoCoordinate)xsmcToInteger(xsArg(0));
	CommodettoCoordinate y = (CommodettoCoordinate)xsmcToInteger(xsArg(1));
	CommodettoDimension w = (CommodettoDimension)xsmcToInteger(xsArg(2));
	CommodettoDimension h = (CommodettoDimension)xsmcToInteger(xsArg(3));
	st77916Begin(sd, x, y, w, h);
}

void xs_st77916_send(xsMachine *the)
{
	st77916Display sd = xsmcGetHostData(xsThis);
	int argc = xsmcArgc;
	void *data;
	xsUnsignedValue count;
	int offset = 0;

	xsmcGetBufferReadable(xsArg(0), &data, &count);

	if (argc > 1) {
		offset = xsmcToInteger(xsArg(1));
		if ((offset < 0) || ((xsUnsignedValue)offset > count))
			xsUnknownError("bad offset");
		if (argc > 2) {
			int c = xsmcToInteger(xsArg(2));
			if ((c < 0) || ((xsUnsignedValue)(offset + c) > count))
				xsUnknownError("bad count");
			count = c;
		}
		else
			count -= offset;
	}

	st77916Send((PocoPixel *)(((uint8_t *)data) + offset), (int)count, sd);
}

void xs_st77916_end(xsMachine *the)
{
	st77916End(xsmcGetHostData(xsThis));
}

void xs_st77916_continue(xsMachine *the)
{
	st77916Continue(xsmcGetHostData(xsThis));
}

void xs_st77916_pixelsToBytes(xsMachine *the)
{
	int count = xsmcToInteger(xsArg(0));
	xsmcSetInteger(xsResult, ((count * kCommodettoPixelSize) + 7) >> 3);
}

void xs_st77916_get_pixelFormat(xsMachine *the)
{
	xsmcSetInteger(xsResult, kCommodettoBitmapFormat);
}

void xs_st77916_get_width(xsMachine *the)
{
	xsmcSetInteger(xsResult, MODDEF_ST77916_WIDTH);
}

void xs_st77916_get_height(xsMachine *the)
{
	xsmcSetInteger(xsResult, MODDEF_ST77916_HEIGHT);
}

void xs_st77916_get_rotation(xsMachine *the)
{
	st77916Display sd = xsmcGetHostData(xsThis);
	xsmcSetInteger(xsResult, sd->rotation * 90);
}

void xs_st77916_set_rotation(xsMachine *the)
{
	st77916Display sd = xsmcGetHostData(xsThis);
	int32_t rotation = xsmcToInteger(xsArg(0));

	if ((0 != rotation) && (90 != rotation) && (180 != rotation) && (270 != rotation))
		xsRangeError("invalid rotation");
	// Rotation is stored but not yet applied (round 360x360 panel starts at 0).
	sd->rotation = (uint8_t)(rotation / 90);
}

void xs_st77916_get_c_dispatch(xsMachine *the)
{
	xsResult = xsThis;
}

void xs_st77916_close(xsMachine *the)
{
	st77916Display sd = xsmcGetHostData(xsThis);
	if (sd) {
		xs_st77916_destructor(sd);
		xsmcSetHostData(xsThis, NULL);
	}
}
