#pragma once
/*
 * Fake esp_log.h for the host simulator build. ui_router.c is the only
 * dial_ui source that logs; this just routes it to stdio so the sim's
 * console shows the same "router up, screen N" line the real firmware logs.
 */
#include <stdio.h>

#define ESP_LOGE(tag, fmt, ...) printf("E (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("W (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) printf("I (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) printf("D (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) printf("V (%s) " fmt "\n", tag, ##__VA_ARGS__)
