// ^
// Property of Luka Vukmirica - All Rights Reserved. (luka.vukmirica@gmail.com)
#include "ti_msp_dl_config.h"
#include <stdbool.h>


// button poll (for negative logic sw2 of TI Launchpad MSPM0G3507)
static bool buttonpoll(void){
    return !DL_GPIO_readPins(GPIO_SWITCHES_S2_PORT, GPIO_SWITCHES_S2_PIN);
}

static volatile bool tick = false;  // Initialize tick to be false


int main(void){
    SYSCFG_DL_init(); // Initialize syscfg
    NVIC_EnableIRQ(TIMER_TICK_INST_INT_IRQN);
    DL_TimerG_startCounter(TIMER_TICK_INST);

    while (1) {
        while (!tick) {
            __WFI(); // Sleep until interrupt arrives
        }
        tick = false; // Reset tick to false
        DL_GPIO_togglePins(GPIO_LEDS_PORT, GPIO_LEDS_USER_LED_2_PIN);  // Toggle LED using GPIO for LED and Pin number as mask
    }
}

void TIMER_TICK_INST_IRQHandler(void){ // Timer zero interrupt function
    if (DL_TimerG_getPendingInterrupt(TIMER_TICK_INST) == DL_TIMER_IIDX_ZERO){ // If zero event hits, flag becomes true
        tick = true;
    }
}
