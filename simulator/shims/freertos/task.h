#pragma once
/* Fake freertos/task.h for the host simulator build — ui_router.c includes
 * this alongside FreeRTOS.h but doesn't need anything beyond what that
 * already provides. */
#include "freertos/FreeRTOS.h"
