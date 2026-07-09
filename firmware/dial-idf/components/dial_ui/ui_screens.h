#pragma once
#include "ui_router.h"

/*
 * M1 screens — functional ports of the original ad-hoc UI into the router.
 * The M2 visual-language pass restyles these; behavior lands first.
 * Call once before ui_router_start().
 */
void ui_screens_register_all(void);
