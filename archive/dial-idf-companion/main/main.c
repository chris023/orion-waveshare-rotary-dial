/*
 * Companion ESP32 firmware — knob → UART bridge for the Orion dial.
 *
 * The board's single physical knob is EC2, wired to THIS chip (companion
 * ESP32-U4WDH) on IO19 (phase A) / IO22 (phase B). The stock companion firmware
 * consumed EC2 locally as a BLE-HID volume control and never forwarded it, so
 * the S3 could not see the knob. This replacement reads EC2 with Espressif's
 * iot_knob decoder and streams each detent tick to the S3 over the inter-MCU
 * UART link (companion TX = IO23 → S3 RX = GPIO48).
 *
 * Protocol (ours; both MCUs run our firmware): one ASCII byte per tick —
 * 'R' = clockwise / right, 'L' = counter-clockwise / left. Simple, self-
 * resyncing, and trivial to extend later (e.g. a byte for a button press).
 *
 * Console logs (companion UART0 → USB when the cable is flipped) also print each
 * tick, so flashing just this chip verifies EC2 reads before wiring the S3 side.
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "bidi_switch_knob.h"

// Broad GPIO scan: find which companion pins actually toggle when the knob
// turns (IO19/22 read dead-constant, so the documented EC2 pins may be wrong).
// Configure every safe GPIO as input+pullup and log any that change.
// Safe, output-capable classic-ESP32 GPIOs with internal pull-ups (exclude
// flash 6-11, console 1/3, our TX 23, and input-only 34-39 which faulted).
#define SCAN_N 19
static const int scan_pins[SCAN_N] = {
    0, 2, 4, 5, 12, 13, 14, 15, 16, 17, 18, 19, 21, 22, 25, 26, 27, 32, 33
};
static void raw_probe_task(void *arg)
{
    int last[SCAN_N];
    for (int i = 0; i < SCAN_N; i++) {
        gpio_config_t c = { .mode = GPIO_MODE_INPUT, .pull_up_en = 1,
                            .pin_bit_mask = 1ULL << scan_pins[i] };
        gpio_config(&c);
        last[i] = -1;
    }
    ESP_LOGI("scan", "scanning %d companion GPIOs - TURN THE KNOB", SCAN_N);
    int hb = 0;
    for (;;) {
        for (int i = 0; i < SCAN_N; i++) {
            int v = gpio_get_level(scan_pins[i]);
            if (v != last[i]) {
                if (last[i] != -1) ESP_LOGI("scan", "GPIO%d: %d->%d", scan_pins[i], last[i], v);
                last[i] = v;
            }
        }
        if (++hb % 200 == 0) ESP_LOGI("scan", "alive");
        vTaskDelay(pdMS_TO_TICKS(3));
    }
}

#define KNOB_A_PIN   19          // EC2 phase A
#define KNOB_B_PIN   22          // EC2 phase B
#define LINK_UART     UART_NUM_1
#define LINK_TX_PIN   23         // → S3 GPIO48 (RX)
#define LINK_RX_PIN   18         // ← S3 GPIO38 (TX); unused for now
#define LINK_BAUD     115200

static const char *TAG = "companion-knob";

static void send_tick(char c)
{
    uart_write_bytes(LINK_UART, &c, 1);
}

static void knob_left_cb(void *arg, void *data)
{
    send_tick('L');
    ESP_LOGI(TAG, "LEFT");
}

static void knob_right_cb(void *arg, void *data)
{
    send_tick('R');
    ESP_LOGI(TAG, "RIGHT");
}

void app_main(void)
{
    // Inter-MCU UART to the S3.
    uart_config_t cfg = {
        .baud_rate = LINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(LINK_UART, 1024, 1024, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LINK_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(LINK_UART, LINK_TX_PIN, LINK_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    // Read EC2 (IO19/22) with the iot_knob quadrature decoder.
    knob_config_t kc = {
        .gpio_encoder_a = KNOB_A_PIN,
        .gpio_encoder_b = KNOB_B_PIN,
    };
    knob_handle_t knob = iot_knob_create(&kc);
    if (!knob) {
        ESP_LOGE(TAG, "knob create failed");
        return;
    }
    ESP_ERROR_CHECK(iot_knob_register_cb(knob, KNOB_LEFT, knob_left_cb, NULL));
    ESP_ERROR_CHECK(iot_knob_register_cb(knob, KNOB_RIGHT, knob_right_cb, NULL));

    ESP_LOGI(TAG, "companion knob->UART bridge up: EC2 IO19/22 -> UART1 TX GPIO23 @%d, 'R'/'L' per tick",
             LINK_BAUD);
    // (raw_probe_task disabled — blind GPIO scanning crashed on embedded-flash pins)
    for (;;)
        vTaskDelay(pdMS_TO_TICKS(5000));
}
