#pragma once
/*
 * Fake esp_app_desc.h for the host simulator build. scr_about.c reads only
 * .version and .idf_ver off this struct; the real definition (many more
 * fields, all irrelevant here) lives in ESP-IDF's app_update component.
 */

typedef struct {
    char version[32];
    char idf_ver[32];
} esp_app_desc_t;

const esp_app_desc_t *esp_app_get_description(void);
