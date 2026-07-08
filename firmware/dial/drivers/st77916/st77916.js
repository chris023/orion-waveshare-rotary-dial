/*
 * ST77916 QSPI display driver — Moddable PixelsOut class.
 *
 * Backed by Espressif's esp_lcd_st77916 vendor component (see modST77916.c).
 * Transfers are synchronous (each band blocks until DMA completes), so
 * `async` is false.
 */

export default class ST77916 @ "xs_st77916_destructor" {
	constructor(options) @ "xs_st77916";

	begin(x, y, width, height) @ "xs_st77916_begin";
	send(pixels, offset, count) @ "xs_st77916_send";
	end() @ "xs_st77916_end";

	adaptInvalid() {}
	continue() @ "xs_st77916_continue";

	pixelsToBytes(count) @ "xs_st77916_pixelsToBytes";

	get pixelFormat() @ "xs_st77916_get_pixelFormat";
	get width() @ "xs_st77916_get_width";
	get height() @ "xs_st77916_get_height";
	get async() {return false;}
	get rotation() @ "xs_st77916_get_rotation";
	set rotation(value) @ "xs_st77916_set_rotation";

	get c_dispatch() @ "xs_st77916_get_c_dispatch";

	close() @ "xs_st77916_close";

	pixels(value = 0) {
		const pixels = this.width << 5;
		return (value > pixels) ? value : pixels;
	}
}
