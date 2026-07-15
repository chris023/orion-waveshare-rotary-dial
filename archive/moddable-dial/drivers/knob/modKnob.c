/*
 * Moddable XS native wrapper around Espressif's iot_knob quadrature decoder
 * (vendored here as bidi_switch_knob.c, from Waveshare's 04_Encoder_Test demo).
 *
 * Why native: the ESP32-S3 knob (EC1) is on GPIO8(A)/GPIO7(B), but Moddable's
 * JS-level GPIO reads (embedded:io/digital AND legacy pins/digital) read these
 * pins as dead-constant on this board, while the identical C gpio_get_level
 * works (proven by Waveshare's stock firmware). So we run the proven C decoder
 * — which polls GPIO8/7 every 3ms on an esp_timer and accumulates a signed
 * count — and expose that count to JS. Same "wrap the working C" approach that
 * brought up the ST77916 display.
 *
 * JS: `new Knob({signal: 8, control: 7})`; `read()` = cumulative signed count;
 * `write(0)` clears it to 0.
 */

#include "xsmc.h"
#include "xsHost.h"
#include "mc.xs.h"

#include "driver/gpio.h"
#include "bidi_switch_knob.h"

static int gKnobA = -1, gKnobB = -1;	// remembered for the raw-level diagnostic

#ifndef MODDEF_KNOB_ENCODER_A
	#define MODDEF_KNOB_ENCODER_A 8
#endif
#ifndef MODDEF_KNOB_ENCODER_B
	#define MODDEF_KNOB_ENCODER_B 7
#endif

void xs_knob_destructor(void *data)
{
	if (data)
		iot_knob_delete((knob_handle_t)data);
}

void xs_knob(xsMachine *the)
{
	int a = MODDEF_KNOB_ENCODER_A;
	int b = MODDEF_KNOB_ENCODER_B;

	if ((xsmcArgc > 0) && xsmcTest(xsArg(0))) {
		xsmcVars(1);
		if (xsmcHas(xsArg(0), xsID_signal)) {
			xsmcGet(xsVar(0), xsArg(0), xsID_signal);
			a = xsmcToInteger(xsVar(0));
		}
		if (xsmcHas(xsArg(0), xsID_control)) {
			xsmcGet(xsVar(0), xsArg(0), xsID_control);
			b = xsmcToInteger(xsVar(0));
		}
	}

	knob_config_t cfg = {
		.gpio_encoder_a = (uint8_t)a,
		.gpio_encoder_b = (uint8_t)b,
	};
	knob_handle_t knob = iot_knob_create(&cfg);
	if (!knob)
		xsUnknownError("knob create failed");

	gKnobA = a;
	gKnobB = b;
	xsmcSetHostData(xsThis, knob);
}

// Diagnostic: raw GPIO levels of A/B as (A<<1)|B, read straight via gpio_get_level.
void xs_knob_readRaw(xsMachine *the)
{
	int v = 0;
	if (gKnobA >= 0) v |= gpio_get_level((gpio_num_t)gKnobA) << 1;
	if (gKnobB >= 0) v |= gpio_get_level((gpio_num_t)gKnobB);
	xsmcSetInteger(xsResult, v);
}

void xs_knob_read(xsMachine *the)
{
	knob_handle_t knob = xsmcGetHostData(xsThis);
	xsmcSetInteger(xsResult, iot_knob_get_count_value(knob));
}

void xs_knob_write(xsMachine *the)
{
	knob_handle_t knob = xsmcGetHostData(xsThis);
	iot_knob_clear_count_value(knob);
}

void xs_knob_close(xsMachine *the)
{
	knob_handle_t knob = xsmcGetHostData(xsThis);
	if (knob) {
		iot_knob_delete(knob);
		xsmcSetHostData(xsThis, NULL);
	}
}
