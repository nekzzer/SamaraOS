#ifndef SAMARA_MOUSE_H
#define SAMARA_MOUSE_H
#include "types.h"

void mouse_init(void);
void mouse_get(int* x, int* y, uint8_t* btn);
void mouse_draw_cursor(void);
void mouse_hide_cursor(void);
void mouse_set_text_cursor(bool enabled);
void mouse_set_range(int max_x, int max_y);
void mouse_get_range(int* max_x, int* max_y);
void mouse_set_pos(int x, int y);

#endif
