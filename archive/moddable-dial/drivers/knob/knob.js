/*
 * Knob — rotary encoder (EC1, GPIO8/7) for the ESP32-S3 knob board.
 *
 * Backed by Espressif's iot_knob C decoder (see modKnob.c). read() returns a
 * cumulative signed count (right = +, left = -); write(0) resets it. Poll and
 * diff for per-tick deltas.
 */

export default class Knob @ "xs_knob_destructor" {
	constructor(options) @ "xs_knob";

	read() @ "xs_knob_read";
	readRaw() @ "xs_knob_readRaw";
	write(count) @ "xs_knob_write";
	close() @ "xs_knob_close";
}
