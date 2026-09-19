// ^
// Property of Luka Vukmirica - All Rights Reserved. (luka.vukmirica@gmail.com)
//Firmware lookup: row = ((ACC + (1 << (TANH_SHIFT-1))) >> TANH_SHIFT) + N/2, clamped to [0, N-1]. */
#include "tanh_lut.h" // Prototype
int16_t tanh_lut(int32_t ACC){
    int32_t rowvalue = ((ACC + (1 << (TANH_SHIFT-1))) >> TANH_SHIFT) + TANH_N/2;
    if (rowvalue <= 0){
        rowvalue = 0;
    } else if (rowvalue >= TANH_N){
        rowvalue = TANH_N - 1;
    }
    return TANH_LUT[rowvalue];
}




    
