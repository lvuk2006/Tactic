// ^
// Property of Luka Vukmirica - All Rights Reserved. (luka.vukmirica@gmail.com)
#include <stdint.h>
#include "pipeline.h"
#include "readoutconv.h"
#include "tanh_lut.h"
#include "weights.h"      // W_RES, W_OUT, W_BIAS_RAW, sizes

static int16_t res_fifo[W_K1];          // shared button history, 32 x 1
static int16_t out_fifo[W_K2 * W_C];    // shared feature history, 32 x 12
static int     res_row, out_row;        // shared write positions
static ReadoutState res[W_C];           // 12 reservoir channels
static ReadoutState out[W_CLASSES];     // 3 readouts

int pipeline_init(void){
    for (int32_t c = 0; c < W_C; c++){
        res[c].fifo = res_fifo; // each conv knows the address of the button history that is shared
        res[c].FIFOROW = &res_row; // each conv knows the address of the shared write position
        res[c].kern = W_RES[c]; // each channel points at its own row of the weight table in flash
        if (convkinit(&res[c], 1, W_K1) != 0){ // initializes struct shape, history full of zeros
            return -1;
        }
    }
    for (int32_t y = 0; y < W_CLASSES; y++){
        out[y].fifo = out_fifo; // each readout knows the address of the feature history that is shared
        out[y].FIFOROW = &out_row; // each readout knows the address of the shared write position
        out[y].kern = W_OUT[y]; // each class points at its own row of the weight table in flash
        if (convkinit(&out[y], W_C, W_K2) != 0){
            return -1;
        }
    }
    return 0;
}

void pipeline_reset(void){
    for (int32_t i = 0; i < W_K1; i++){
        res_fifo[i] = 0;
    }
    for (int32_t i = 0; i < W_K2 * W_C; i++){
        out_fifo[i] = 0;
    }
    res_row = 0;
    out_row = 0;
}

void pipeline_step(int16_t button, int32_t score[]){
    int16_t RESARRAY[W_C]; // reservoir feature for each channel
    convkfifo(&res[0], &button); // one push into the shared button history
    for (int32_t c = 0; c < W_C; c++){ // Loops through all 12 channels running convolutions
        int32_t ACCOUT = convkrun(&res[c]);
        RESARRAY[c] = tanh_lut(ACCOUT); // Puts array of values through tanh LUT activation function
    }
    convkfifo(&out[0], RESARRAY); // one push of all W_C features into the shared feature history
    for (int32_t c = 0; c < W_CLASSES; c++){ // Runs convolution for 3 output classes
        score[c] = convkrun(&out[c]) + W_BIAS_RAW[c]; // bias folded in, same Q28 as the threshold
    }
}
