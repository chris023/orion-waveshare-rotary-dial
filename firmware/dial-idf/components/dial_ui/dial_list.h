#pragma once
#include "lvgl.h"

/*
 * Rotor list — the shared round-screen treatment for every vertical list
 * screen (menu face, settings, Wi-Fi, About). A full-screen scroll container
 * that keeps exactly one row "focused" in the vertical center (scroll-snap)
 * and zooms/fades rows toward the top/bottom edges, so list content reads as
 * sitting on a curved surface and never pokes past the round panel's chord.
 *
 * Usage, all from the LVGL task (same contract as every screen hook):
 *   s_list = dial_list_create(scr, ROW_H);
 *   ...append rows (any lv_obj children, uniform ROW_H tall)...
 *   dial_list_settle(s_list);        // initial layout + transform pass
 * and from on_knob:
 *   dial_list_knob(s_list, detents)  // one row of focus per detent
 */

// Full-screen rotor container: flex column, snap-center, scrollbar off,
// top/bottom padding sized so the first/last row can rest centered. row_h
// must match the uniform height of the rows appended afterwards (it drives
// both the padding and the knob's row math).
lv_obj_t *dial_list_create(lv_obj_t *parent, lv_coord_t row_h);

// Run layout and apply the initial zoom/fade pass. Call once after the rows
// are appended (creation happens before LVGL lays the screen out, so the
// transform pass can't run off real coordinates until forced here).
void dial_list_settle(lv_obj_t *list);

// Move the focused row by `detents` (+down/-up), animated onto center.
// Returns  1 = focus moved (voice the tick haptic),
//         -1 = already at the end stop (voice the range-stop haptic),
//          0 = swallowed: a finger drag owns the list right now — no
//              feedback, the detent simply doesn't apply mid-drag.
int dial_list_knob(lv_obj_t *list, int detents);
