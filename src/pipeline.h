// ^
// Property of Luka Vukmirica - All Rights Reserved. (luka.vukmirica@gmail.com)
#ifndef PIPELINE_H
#define PIPELINE_H

#include <stdint.h>

// The model, with no hardware in it: reservoir -> tanh -> readout.
// main.c runs it on the board; host/golden_check.c runs the SAME file on the
// computer against the host's golden scores, so the two cannot drift apart.

int  pipeline_init(void);    // point the structs at their storage, zero history; -1 on bad sizes
void pipeline_reset(void);   // back to power-on state: empty histories, write positions at 0
void pipeline_step(int16_t button, int32_t score[]);  // one tick: 0/1 in, W_CLASSES biased scores out

#endif
