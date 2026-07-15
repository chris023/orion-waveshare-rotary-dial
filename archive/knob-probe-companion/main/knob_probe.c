/*
 * WIDE rotary-encoder scan for the COMPANION MCU (classic ESP32), poll-based.
 *
 * The knob works in stock firmware, so the encoder is wired to some pins. This
 * polls a conservative set of pull-up-capable GPIOs at 2 ms and counts level
 * transitions per pin. Turn the knob; whichever pins rack up correlated
 * transitions ARE the encoder A/B (and maybe its push-switch).
 *
 * Excluded (either reserved or observed to fault this ESP32-U4WDH at config):
 *   1/3 UART0 console, 6-11 SPI flash, 34-39 input-only (float),
 *   0/2/12/15 strapping, 16/17 flash/cache-adjacent on embedded-flash parts,
 *   32/33 possible 32 kHz XTAL.
 */

#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "knobscan";

static const int PINS[] = { 4, 5, 13, 14, 18, 19, 21, 22, 23, 25, 26, 27 };
#define NPINS (sizeof(PINS) / sizeof(PINS[0]))

void app_main(void)
{
    ESP_LOGW(TAG, "=== WIDE KNOB SCAN v4 (PULL-DOWN) === turn the knob SLOWLY & continuously ~35s");
    int last[NPINS];
    uint32_t changes[NPINS] = {0};
    for (size_t i = 0; i < NPINS; i++) {
        int pin = PINS[i];
        gpio_config_t c = {
            .pin_bit_mask = 1ULL << pin,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,   // test: encoder common at Vcc?
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&c);
        last[i] = gpio_get_level(pin);
    }
    ESP_LOGW(TAG, "configured %d pins OK, entering poll loop", (int)NPINS);

    int hb = 0;
    for (;;) {
        for (size_t i = 0; i < NPINS; i++) {
            int v = gpio_get_level(PINS[i]);
            if (v != last[i]) {
                changes[i]++;
                last[i] = v;
                ESP_LOGI(TAG, "GPIO%d -> %d  (changes=%" PRIu32 ")", PINS[i], v, changes[i]);
            }
        }
        if (++hb >= 1000) {   // ~2s heartbeat (loop 2ms)
            hb = 0;
            char buf[256]; size_t off = 0;
            for (size_t i = 0; i < NPINS && off < sizeof(buf) - 1; i++)
                if (changes[i])
                    off += snprintf(buf + off, sizeof(buf) - off, " G%d=%" PRIu32, PINS[i], changes[i]);
            ESP_LOGW(TAG, "movers:%s", off ? buf : " (none yet)");
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}
