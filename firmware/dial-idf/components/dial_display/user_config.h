#ifndef USER_CONFIG_H
#define USER_CONFIG_H

//spi & i2c handle
#define LCD_HOST SPI2_HOST
#define TOUCH_HOST I2C_NUM_0

// I2C
#define ESP32_SCL_NUM (GPIO_NUM_12)
#define ESP32_SDA_NUM (GPIO_NUM_11)


//  DISP
// The pixel number in horizontal and vertical
#define EXAMPLE_LCD_H_RES              360 
#define EXAMPLE_LCD_V_RES              360
#define EXAMPLE_LVGL_BUF_HEIGHT        (EXAMPLE_LCD_V_RES / 10) 

#define EXAMPLE_PIN_NUM_LCD_CS      (gpio_num_t)14
#define EXAMPLE_PIN_NUM_LCD_PCLK    (gpio_num_t)13
#define EXAMPLE_PIN_NUM_LCD_DATA0   (gpio_num_t)15
#define EXAMPLE_PIN_NUM_LCD_DATA1   (gpio_num_t)16
#define EXAMPLE_PIN_NUM_LCD_DATA2   (gpio_num_t)17
#define EXAMPLE_PIN_NUM_LCD_DATA3   (gpio_num_t)18
#define EXAMPLE_PIN_NUM_LCD_RST     (gpio_num_t)21
#define EXAMPLE_PIN_NUM_BK_LIGHT    (gpio_num_t)47

#define EXAMPLE_TOUCH_ADDR                0x15
#define EXAMPLE_PIN_NUM_TOUCH_RST         (gpio_num_t)10
#define EXAMPLE_PIN_NUM_TOUCH_INT         (gpio_num_t)9


#define EXAMPLE_LVGL_TICK_PERIOD_MS    2
#define EXAMPLE_LVGL_TASK_MAX_DELAY_MS 500
#define EXAMPLE_LVGL_TASK_MIN_DELAY_MS 5
#define EXAMPLE_LVGL_TASK_STACK_SIZE   (8 * 1024)
/*
 * The UI outranks everything it competes with, and it gets its own core.
 *
 * This was 2 — BELOW the network worker's 4 — which meant that every time the
 * worker ran a TLS handshake (and it runs one per MCP call, since each call
 * opens a fresh HTTPS connection), FreeRTOS preempted the LVGL task for the
 * whole of that CPU-bound crypto. The screen stopped repainting, and since
 * LVGL is also what polls the touch panel, a tap that began and ended inside
 * that window was never seen at all. That is what made the dial feel like it
 * was freezing and dropping presses.
 *
 * Rule now: nothing the user is waiting on may be starved by work they are not.
 */
#define EXAMPLE_LVGL_TASK_PRIORITY     5
#define EXAMPLE_LVGL_TASK_CORE         1   // APP core; Wi-Fi/lwIP live on core 0

#define EXAMPLE_USE_TOUCH  1 //Without tp ---- Touch off

// Rotary encoder (EC1) — GPIO8 = phase A, GPIO7 = phase B (from schematic).
#define EXAMPLE_ENCODER_ECA_PIN  8
#define EXAMPLE_ENCODER_ECB_PIN  7

//#define Backlight_Testing
//#define EXAMPLE_Rotate_90


//bit

#define SET_BIT(reg,bit) (reg |= ((uint32_t)0x01<<bit))
#define CLEAR_BIT(reg,bit) (reg &= (~((uint32_t)0x01<<bit)))
#define READ_BIT(reg,bit) (((uint32_t)reg>>bit) & 0x01)
#define BIT_EVEN_ALL (0x00ffffff)

#endif