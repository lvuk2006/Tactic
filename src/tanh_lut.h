// ^
// Property of Luka Vukmirica - All Rights Reserved. (luka.vukmirica@gmail.com)
#ifndef TANH_LUT_H
#define TANH_LUT_H

#include <stdint.h>
#include "tanh_table.h"   /* TANH_N, TANH_SHIFT, TANH_LUT */

int16_t tanh_lut(int32_t ACC);

#endif
