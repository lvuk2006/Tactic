// ^
// Property of Luka Vukmirica - All Rights Reserved. (luka.vukmirica@gmail.com)

# Runtime plan

`main.c` is not in this directory yet. This file says what goes in it, so that
what is missing is a known shape rather than a gap. Everything it depends on —
`readoutconv.c`, `tanh_lut.c`, `weights.h`, `golden.h` — is already here and
verified on the host.

## One tick, at 10 Hz

A hardware timer sets a flag; the loop does the work outside the ISR.

1. Sample the debounced button state, as a literal 0 or 1.
2. Push it into the reservoir FIFO, then run the 12 channels. Push first, then
   run — the convolution indexes backwards from `FIFOROW - 1`, so the order is
   not interchangeable.
3. `tanh_lut()` on each channel accumulator, giving 12 Q15 features.
4. Push those into the readout FIFO, then run one 32-tap convolution per class.
5. `argmax` over the three scores. If the winner is above `W_THRESHOLD_RAW`,
   fire and start the refractory countdown (`W_REFRACTORY` ticks).
6. Drive the LEDs: one per class on a fire, plus an idle indication so a viewer
   can tell the board is alive and listening rather than hung.

No segmentation, no end-of-pattern detection. Every tick is scored, which is
what makes the phase-independence result meaningful.

## Button input

Falling/rising edge interrupt plus a debounce counter, sampled at the tick rate.
The ISR sets state; it does not run the pipeline.

Two buttons on the LaunchPad: S1 is the pattern input, S2 re-arms after a fire.

## Bring-up order, one variable at a time

1. Replay `host/golden.h` traces through the pipeline on target, with the button
   ignored, and compare scores tick by tick against the host.
2. Compare **scores and decisions separately.** A decision that flips while the
   score sits within a couple of LSB of the threshold is a rounding-convention
   difference, not a bug. A score that differs at all is a bug, because both
   sides run the same integer arithmetic.
3. Only then switch the input source from the canned vectors to the button.

Same pipeline in both builds, with the input swapped — so the tested path and
the demo path cannot drift apart.

## Instrumentation to add while doing this

A TIMG channel counting at CPU clock, around one full tick, for cycles per
inference. The M0+ has no DWT cycle counter, so this is the only way to get a
real number rather than an estimate. Flash and SRAM come from
`arm-none-eabi-size` on the built ELF.
