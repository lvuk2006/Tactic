// ^
// Property of Luka Vukmirica - All Rights Reserved. (luka.vukmirica@gmail.com)
#include "ti_msp_dl_config.h"
#include <stdbool.h>
#include "pipeline.h"     // the model: reservoir -> tanh -> readout (no hardware)
#include "weights.h"      // W_CLASSES, W_THRESHOLD_RAW



// button poll (for negative logic sw2 of TI Launchpad MSPM0G3507)
static bool buttonpoll(void){
    return !DL_GPIO_readPins(GPIO_SWITCHES_S2_PORT, GPIO_SWITCHES_S2_PIN);
}

static volatile bool tick = false;  // Initialize tick to be false


int main(void){
    SYSCFG_DL_init(); // Initialize syscfg
    if (pipeline_init() != 0){ // Wire the model to its storage; fails only if weights.h sizes are bad
        DL_GPIO_setPins(GPIO_LEDS_PORT, GPIO_LEDS_USER_LED_1_PIN | GPIO_LEDS_USER_LED_2_PIN | GPIO_LEDS_USER_LED_3_PIN);
        while (1){ __WFI(); } // All three LEDs solid = setup failed, stop
    }
    
    NVIC_EnableIRQ(TIMER_TICK_INST_INT_IRQN); // Enable Interrups from timer
    DL_TimerG_startCounter(TIMER_TICK_INST); // Sets up timer G0
    int32_t READOUTARRAY[W_CLASSES]; // Setup for readout class array
    int32_t CT = 0; // Count for LED Refractory period

    while (1) {
        while (!tick) {
            __WFI(); // Sleep until interrupt arrives
        }
        tick = false; // Reset tick to false
        //DL_GPIO_togglePins(GPIO_LEDS_PORT, GPIO_LEDS_USER_LED_2_PIN);  // Toggle LED using GPIO for LED and Pin number as mask
        if(CT == 0){
            DL_GPIO_clearPins(GPIO_LEDS_PORT, GPIO_LEDS_USER_LED_1_PIN | GPIO_LEDS_USER_LED_2_PIN | GPIO_LEDS_USER_LED_3_PIN);
            
        }
        if (CT > 0){
            CT --; // Decrease count for refractory period
        }
        pipeline_step(buttonpoll(), READOUTARRAY); // One tick of the model: 3 class scores
        int32_t val = 0; // value for highest readout
        int32_t highest = 0; // value of that highest readout
        for (int32_t w = 0; w < W_CLASSES; w++){ // Max value search algorithm
            if (READOUTARRAY[w] > highest){ // Recent value must be greater than previous highest
                highest = READOUTARRAY[w]; // Update greatest if higheer
                val = w; // Recognize which class is winning
            }
        }
        if (val == 0 && READOUTARRAY[0] > W_THRESHOLD_RAW){ // Depending on val and whether value exceeded W_THRESHOLD_RAW value lights up certain led on board
            DL_GPIO_setPins(GPIO_LEDS_PORT, GPIO_LEDS_USER_LED_1_PIN);
            CT = 20; // Set Count for 2s over 100ms runtime 
        } else if (val == 1 && READOUTARRAY[1] > W_THRESHOLD_RAW){
            DL_GPIO_setPins(GPIO_LEDS_PORT, GPIO_LEDS_USER_LED_2_PIN);
            CT = 20; // Set Count for 2s over 100ms runtime 
        } else if (val == 2 && READOUTARRAY[2] > W_THRESHOLD_RAW) {
            DL_GPIO_setPins(GPIO_LEDS_PORT, GPIO_LEDS_USER_LED_3_PIN);
            CT = 20; // Set Count for 2s over 100ms runtime 
        }

    }

}


void TIMER_TICK_INST_IRQHandler(void){ // Timer zero interrupt function
    if (DL_TimerG_getPendingInterrupt(TIMER_TICK_INST) == DL_TIMER_IIDX_ZERO){ // If zero event hits, flag becomes true
        tick = true;
    }
}
