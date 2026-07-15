/*
 * Moddable XS native CST816 touch reader — uses ESP-IDF's NEW i2c_master API
 * (the legacy driver/i2c.h conflicts with Moddable's driver_ng and crashes).
 *
 * Reproduces the stock demo's register ops: i2c @configurable Hz on SDA=11/
 * SCL=12, optional RST toggle, write reg 0x00 = 0x00 ("normal mode"), then read
 * 7 bytes from reg 0x00 -> [2]=count, [3]=evt|Xhi, [4]=Xlo, [5]=Yhi, [6]=Ylo.
 *
 * JS: new CST816({sda,scl,rst,address,hz}); id() -> chip id; sample() -> -1 when
 * no touch else ((x & 0xFFF) << 12) | (y & 0xFFF).
 */

#include "xsmc.h"
#include "xsHost.h"
#include "mc.xs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"

#ifndef MODDEF_CST816_SDA
	#define MODDEF_CST816_SDA 11
#endif
#ifndef MODDEF_CST816_SCL
	#define MODDEF_CST816_SCL 12
#endif
#ifndef MODDEF_CST816_ADDRESS
	#define MODDEF_CST816_ADDRESS 0x15
#endif
#ifndef MODDEF_CST816_HZ
	#define MODDEF_CST816_HZ 300000
#endif

typedef struct {
	i2c_master_bus_handle_t		bus;
	i2c_master_dev_handle_t		dev;
} cst816Record, *cst816;

void xs_cst816_destructor(void *data)
{
	cst816 t = data;
	if (!t) return;
	if (t->dev)
		i2c_master_bus_rm_device(t->dev);
	if (t->bus)
		i2c_del_master_bus(t->bus);
	c_free(t);
}

void xs_cst816(xsMachine *the)
{
	int sda = MODDEF_CST816_SDA, scl = MODDEF_CST816_SCL, rst = -1;
	int address = MODDEF_CST816_ADDRESS, hz = MODDEF_CST816_HZ;
	int writeInit = 1;

	if ((xsmcArgc > 0) && xsmcTest(xsArg(0))) {
		xsmcVars(1);
		if (xsmcHas(xsArg(0), xsID_sda))     { xsmcGet(xsVar(0), xsArg(0), xsID_sda);     sda = xsmcToInteger(xsVar(0)); }
		if (xsmcHas(xsArg(0), xsID_scl))     { xsmcGet(xsVar(0), xsArg(0), xsID_scl);     scl = xsmcToInteger(xsVar(0)); }
		if (xsmcHas(xsArg(0), xsID_rst))     { xsmcGet(xsVar(0), xsArg(0), xsID_rst);     rst = xsmcToInteger(xsVar(0)); }
		if (xsmcHas(xsArg(0), xsID_address)) { xsmcGet(xsVar(0), xsArg(0), xsID_address); address = xsmcToInteger(xsVar(0)); }
		if (xsmcHas(xsArg(0), xsID_hz))      { xsmcGet(xsVar(0), xsArg(0), xsID_hz);      hz = xsmcToInteger(xsVar(0)); }
	}

	if (rst >= 0) {
		// Reset dance (Tasmota / CST816D datasheet): HIGH 10ms, LOW 10ms, HIGH,
		// then >=100ms before init. A reset pulse also EXITS deep-sleep (0xA5),
		// clearing any lingering sleep from prior experiments.
		gpio_config_t rc = { .mode = GPIO_MODE_OUTPUT, .pin_bit_mask = 1ULL << rst };
		gpio_config(&rc);
		gpio_set_level((gpio_num_t)rst, 1);
		vTaskDelay(pdMS_TO_TICKS(10));
		gpio_set_level((gpio_num_t)rst, 0);
		vTaskDelay(pdMS_TO_TICKS(10));
		gpio_set_level((gpio_num_t)rst, 1);
		vTaskDelay(pdMS_TO_TICKS(120));
	}

	cst816 t = c_calloc(1, sizeof(cst816Record));
	if (!t)
		xsUnknownError("no memory");
	xsmcSetHostData(xsThis, t);

	i2c_master_bus_config_t buscfg = {
		.i2c_port = -1,
		.sda_io_num = sda,
		.scl_io_num = scl,
		.clk_source = I2C_CLK_SRC_DEFAULT,
		.glitch_ignore_cnt = 7,
		.flags = { .enable_internal_pullup = 1 },
	};
	if (ESP_OK != i2c_new_master_bus(&buscfg, &t->bus))
		xsUnknownError("i2c bus create failed");

	i2c_device_config_t devcfg = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = (uint16_t)address,
		.scl_speed_hz = (uint32_t)hz,
	};
	if (ESP_OK != i2c_master_bus_add_device(t->bus, &devcfg, &t->dev))
		xsUnknownError("i2c add device failed");

	if (writeInit) {
		// Enable the touch-report engine (the step every prior attempt missed).
		// From Moddable's Waveshare cst816s driver + the Tasmota CST816 recipe.
		static const uint8_t seq[][2] = {
			{ 0xFE, 0xFF },		// DisAutoSleep: never enter standby (keeps scanning for polling)
			{ 0xFA, 0x41 },		// IrqCtl: EnTouch | OnceWLP (enable touch IRQ/report)
			{ 0xEC, 0x00 },		// MotionMask: plain point reporting
			{ 0xFB, 0x00 },		// AutoReset: off
			{ 0x00, 0x00 },		// work mode: normal
		};
		for (int i = 0; i < (int)(sizeof(seq) / sizeof(seq[0])); i++)
			i2c_master_transmit(t->dev, (uint8_t *)seq[i], 2, 1000);
	}
}

void xs_cst816_id(xsMachine *the)
{
	cst816 t = xsmcGetHostData(xsThis);
	uint8_t reg = 0xA7, id = 0;
	if (ESP_OK != i2c_master_transmit_receive(t->dev, &reg, 1, &id, 1, 1000))
		id = 0xFF;
	xsmcSetInteger(xsResult, id);
}

void xs_cst816_sample(xsMachine *the)
{
	cst816 t = xsmcGetHostData(xsThis);
	uint8_t reg = 0x00, buf[7] = {0};

	if (ESP_OK != i2c_master_transmit_receive(t->dev, &reg, 1, buf, sizeof(buf), 1000)) {
		xsmcSetInteger(xsResult, -1);
		return;
	}
	int count = buf[2] & 0x0F;
	if (0 == count) {
		xsmcSetInteger(xsResult, -1);
		return;
	}
	int x = ((buf[3] & 0x0F) << 8) | buf[4];
	int y = ((buf[5] & 0x0F) << 8) | buf[6];
	xsmcSetInteger(xsResult, ((x & 0xFFF) << 12) | (y & 0xFFF));
}

void xs_cst816_close(xsMachine *the)
{
	cst816 t = xsmcGetHostData(xsThis);
	if (t) {
		xs_cst816_destructor(t);
		xsmcSetHostData(xsThis, NULL);
	}
}
