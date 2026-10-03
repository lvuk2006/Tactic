// ^
// Property of Luka Vukmirica - All Rights Reserved. (luka.vukmirica@gmail.com)
#include <stdint.h> 
#ifndef READOUTCONV_H
#define READOUTCONV_H

// constants:
#define MAX_WIDTH 12
#define MAX_DEPTH 32


typedef struct {
      int n, k;
      int16_t fifo[MAX_DEPTH][MAX_WIDTH];
      int FIFOROW;
      int16_t kern[MAX_DEPTH][MAX_WIDTH];
  } ReadoutState;

// Prototypes, so every caller shares ONE declaration of these.
int     convkinit(ReadoutState *rs, int32_t width, int32_t height);
void    convkfifo(ReadoutState *rs, int16_t input[]);
int32_t convkrun(ReadoutState *rs);

#endif



