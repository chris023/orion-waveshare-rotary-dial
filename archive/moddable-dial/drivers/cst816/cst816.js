/*
 * CST816 capacitive touch — native C reader (see modCST816.c).
 *
 * new CST816({sda:11, scl:12, rst:10}); id() = chip id (0xB6 = CST816D);
 * sample() returns null when untouched, else { x, y }.
 */

export default class CST816 @ "xs_cst816_destructor" {
	constructor(options) @ "xs_cst816";

	id() @ "xs_cst816_id";
	close() @ "xs_cst816_close";

	sampleRaw() @ "xs_cst816_sample";

	sample() {
		const v = this.sampleRaw();
		if (v < 0)
			return null;
		return { x: (v >> 12) & 0xfff, y: v & 0xfff };
	}
}
