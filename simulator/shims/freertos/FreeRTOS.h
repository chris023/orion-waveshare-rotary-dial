#pragma once
/*
 * Fake freertos/FreeRTOS.h for the host simulator build. ui_router.c is the
 * only dial_ui source that touches FreeRTOS, and only for three things: an
 * assert macro, and a spinlock it uses to guard a handful of instructions
 * (the knob detent accumulator) — never anything that blocks. The simulator
 * is single-threaded, so the "lock" is a no-op.
 */
#include <assert.h>

#define configASSERT(x) assert(x)

typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0

#define taskENTER_CRITICAL(mux) ((void)(mux))
#define taskEXIT_CRITICAL(mux)  ((void)(mux))
