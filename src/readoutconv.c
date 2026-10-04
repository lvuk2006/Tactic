// ^
// Property of Luka Vukmirica - All Rights Reserved. (luka.vukmirica@gmail.com)
#include <stdio.h> // For input/output
#include <stdint.h> // For integer values
#include "readoutconv.h" // Include header file too.




int convkinit(ReadoutState *rs, int32_t width, int32_t height){
    if ((width > MAX_WIDTH) | (width <= 0) | (height > MAX_DEPTH) | (height <= 0)) { // Makes sure kernel values are not out of bounds
        return -1;
    }

    rs->n = width; // Plugs kernel dimensions
    rs->k = height;

    for (int j = 0; j < rs->k ; j++){ // Sets new dimensions space of kernel to zero
            for (int i = 0; i < rs->n; i++){
                rs->fifo[rs->n * j + i] = 0; // For all references of fifo and kern since
                // they are pointers now, the system must calculate
                // offset instead of using "[j][i]" due to pointer carrying no information
                // about the array it is referencing.
            }
    }
    *rs->FIFOROW = 0; // Initialize FIFO row index to zero

    return 0; // Initialization success
    }

void convkfifo(ReadoutState *rs, int16_t input[]){
    for (int i = 0; i < rs->n; i++){ // Plug in new input from N-taps into FIFO
        rs->fifo[*rs->FIFOROW * rs->n + i] = input[i];
    }

    *rs->FIFOROW = (1 + *rs->FIFOROW) % rs->k; // Loopback or increment FIFOROW
}

int32_t convkrun(ReadoutState *rs){
    int32_t ACC = 0;
    for(int j = 0; j < rs->k; j++){ // Do convolution with kernel and FIFO
        for(int i = 0; i < rs->n; i++){
            ACC = ACC + rs->fifo[rs->n*((((*rs->FIFOROW - 1)-j) % rs->k + rs->k) % rs->k) + i]*rs->kern[rs->n*j + i];
        }
    }

    return ACC;
}

// Build struct for rs
// static ReadoutState state = { .n = 2, .k = 4,.FIFOROW = 0, .kern = {
//           {1, 1},   // row j=0
//           {2, 2},   // row j=1
//           {3, 3},
//           {4, 4}    // row j=2
//       } };

// static int16_t inputfeed[6][2] = {
//         {1, 1},
//         {2, 2},
//         {3, 3},
//         {4, 4},
//         {5, 5},
//         {6, 6}};

// int main(){
     
//     // Initialize convolutional readout
//     convkinit(&state, state.n, state.k);

//     // Feed convolution six times
//     for (int i = 0; i < 6; i++){
//         convkfifo(&state, inputfeed[i]); // Feeds input feed row multiple times
//         int32_t out = convkrun(&state);
//         printf("Convolution sum is %2d\n", out); // prints out convolution sum
//     }

//     return 0;
// }
