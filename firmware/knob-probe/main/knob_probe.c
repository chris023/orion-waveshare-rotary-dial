/*
 * Rotary-encoder probe for the ESP32-S3 knob board.
 *
 * The knob has read dead on GPIO8/7 across many *polling* captures. Polling can
 * miss a detent transition that happens between samples, so this probe attaches
 * a GPIO interrupt (ANY edge) to each encoder pin: the ISR counts every edge no
 * matter how fast, so a single slow turn cannot be missed. It also scans a few
 * free neighbor GPIOs in case the wiring differs from the schematic.
 *
 * Expected outcomes:
 *   - edgesA/edgesB climb while turning        -> encoder IS on GPIO8/7 (earlier
 *                                                 polling tests had a timing bug)
 *   - a scan GPIO toggles instead              -> encoder is on a different pin
 *   - NOTHING moves across many slow turns     -> encoder is not electrically on
 *                                                 the S3 (companion-only / fault)
 */

#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "knobprobe";

// Schematic EC1 (S3 side): A=GPIO8, B=GPIO7. (GPIO19/20 are the native-USB
// D-/D+ and MUST NOT be touched, or we lose the serial console.)
#define PIN_A 8
#define PIN_B 7
static const int scan_pins[] = { 1, 2, 3, 4, 5, 6 };
#define NSCAN (sizeof(scan_pins) / sizeof(scan_pins[0]))

static volatile uint32_t edges_a, edges_b;

static void IRAM_ATTR isr_a(void *arg) { edges_a++; }
static void IRAM_ATTR isr_b(void *arg) { edges_b++; }

static void cfg_input(int pin, gpio_int_type_t intr)
{
    gpio_config_t c = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = intr,
    };
    gpio_config(&c);
}

void app_main(void)
{
    ESP_LOGW(TAG, "=== KNOB PROBE === turn the knob SLOWLY for ~60s (and press it)");
    cfg_input(PIN_A, GPIO_INTR_ANYEDGE);
    cfg_input(PIN_B, GPIO_INTR_ANYEDGE);
    for (size_t i = 0; i < NSCAN; i++) cfg_input(scan_pins[i], GPIO_INTR_DISABLE);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(PIN_A, isr_a, NULL);
    gpio_isr_handler_add(PIN_B, isr_b, NULL);

    int last_a = -1, last_b = -1;
    int last_scan[NSCAN];
    for (size_t i = 0; i < NSCAN; i++) last_scan[i] = -1;
    uint32_t la = 0, lb = 0;
    int hb = 0;

    for (;;) {
        int a = gpio_get_level(PIN_A), b = gpio_get_level(PIN_B);
        if (a != last_a || b != last_b || edges_a != la || edges_b != lb) {
            ESP_LOGI(TAG, "A(G8)=%d B(G7)=%d  edgesA=%" PRIu32 " edgesB=%" PRIu32,
                     a, b, edges_a, edges_b);
            last_a = a; last_b = b; la = edges_a; lb = edges_b;
        }
        for (size_t i = 0; i < NSCAN; i++) {
            int v = gpio_get_level(scan_pins[i]);
            if (v != last_scan[i]) {
                ESP_LOGI(TAG, "scan GPIO%d -> %d", scan_pins[i], v);
                last_scan[i] = v;
            }
        }
        if (++hb >= 100) {   // ~2s heartbeat (loop is 20ms)
            hb = 0;
            ESP_LOGW(TAG, "alive: A(G8)=%d B(G7)=%d edgesA=%" PRIu32 " edgesB=%" PRIu32,
                     a, b, edges_a, edges_b);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
