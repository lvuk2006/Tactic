# Tactic

*The layer that executes.*

Temporal pattern recognition on a TI MSPM0G3507 (Cortex-M0+, 80 MHz, 32 KB SRAM).
One button in, three LEDs out, no host connection once it is flashed.

In DSP terms: a bank of fixed random FIR filters, a tanh activation lookup table, a second FIR stage, and a
trained linear readout. In ML terms: a time-delay convolutional reservoir with a fixed
random substrate, where only the readout is fitted. None of those pieces are new — see
[Where this sits](#where-this-sits) — the point here is the composition, the size, and
the fact that the memory horizon is a number you can state exactly instead of a property
you hope for. The filters are FIR, so the network cannot diverge due to a lack of a spectral radius, has no state to wind
up, and looks back exactly K1 + K2 - 1 = 63 samples. At 10 Hz that is 6.3 seconds, and
it is true by construction rather than by measurement.

## Status

Everything below the entry point is done and verified. The entry point is the last piece,
and it is what I am writing now.

- Model, fitting pipeline, fixed-point kernels, tanh LUT, generated weights and golden
  vectors: **done**, and the numbers below are measured on them
- CMake + arm-none-eabi toolchain and DSLite flashing: **done** — the CMake build reproduces
  TI's own makefile output byte for byte, 688 B, which is how I know the toolchain is honest
- Runtime loop, button ISR and LED output: **in progress.** `src/main.c` is deliberately not
  in the repo until it is complete — I would rather ship no entry point than TI's blink example
  wearing my name.

Practical consequence: the **host tools build and run today**, and the firmware target is
skipped by CMake until `src/main.c` lands. That is the only thing missing, and
[`src/RUNTIME_PLAN.md`](src/RUNTIME_PLAN.md) says exactly what goes in it and in what order
I am bringing it up.

## The task, and why it is not trivial

One button, sampled at 10 Hz. A gesture is three presses — two short (5 ticks), one long
(15 ticks), 5-tick gaps — and the three classes are the three orders those can come in.

Every class therefore has **the same press count and the same total press time.** Anything
that ignores order — a counter, a sum, a mean, total energy — is provably at chance here.
That was the whole reason for choosing this task: it cannot be solved by the cheap thing,
so it is an honest test of whether temporal structure is being captured.

While a hand-written state machine can also solve it, the interesting comparison is not when you feed the system clean input but jittered input. This is where an FSM
has to commit at each threshold and cannot revise. Measuring that is on the list below.

## Numbers

Computed from the source, provable at build time:

| | |
|---|---|
| weights | 1536 int16 = 3072 B flash |
| tanh table | 256 int16 = 512 B flash |
| state | 832 B SRAM (64 B button history + 768 B readout FIFOs) |
| arithmetic | 1536 MACs per tick, 15,360 MAC/s at 10 Hz |
| memory horizon | 63 ticks = 6.3 s exactly |
| accumulator bounds | reservoir 5374x headroom, readout 1.07x, both `_Static_assert`ed |

Measured on 300 held-out traces the fit never saw:

| | |
|---|---|
| correct | 172/180 (95.6%), zero wrong-class |
| false alarms | 26/120 (21.7%) |

The false alarm number deserves its breakdown, because the headline is misleading: idle
0/32, held-down 0/29, random presses 4/22, deliberate near-misses 22/37. The near-miss
negatives use a hold duration of 10 ticks, exactly halfway between a short press and a long
one, so they are ambiguous on purpose. It never fires on an untouched or simply held button.

Not filled in yet, because they need the board: cycles per inference, CPU duty, end-to-end
latency, total code size.

## How it runs, per tick

    button (0 or 1)
      -> 12 fixed random FIR channels, 32 taps        int32 accumulator, Q16
      -> tanh via 256-entry LUT                       Q15
      -> per class: 32-tap FIR over the 12 channels   int32 accumulator, Q28
      -> argmax, compare against a pre-scaled threshold, ~2 s refractory

Three shifts hold the whole thing together, and each is derived rather than chosen:

- `W_OUT_SHIFT = 13` — the largest Q-format whose provable worst case, `max|feature| *
  sum|weights|`, still fits in int32. The fitted weights sum to about 7.48, the budget is
  8.0, so Q13 fits and Q14 does not.
- `TANH_SHIFT = 9` — converting a Q16 accumulator into a table row is a multiply by 32 and
  a divide by 65536, and folding the gain of 4.0 in leaves a single shift. No multiply, no
  divide, which matters on a core that has neither a divider nor a 64-bit multiply.
- `W_SCORE_SHIFT = 28` — Q15 features times Q13 weights. Never executed: the threshold is
  scaled to Q28 when the weights are generated, so the hot loop is one compare.

Everything is integer. There is no float anywhere in `src/`.

## Layout

| | | ships to the MCU? |
|---|---|---|
| `src/` | runtime: conv kernels, tanh LUT, generated `weights.h` | yes |
| `vendor/` | unmodified TI startup, linker script, SysConfig output | yes |
| `host/` | trace generator, ridge fit, golden vectors | no |
| `cmake/`, `tools/` | toolchain file, flash script, ccxml | — |

The split is deliberate. `arm-none-eabi-size` then measures only what actually runs on the
chip, and a reader can see at a glance that none of the fitting tooling ships.

`src/readoutconv.c` is compiled into the host fitter as well, and the host calls the same
`tanh_lut()` the firmware does. The weights are therefore fitted against the exact integer
arithmetic the target executes, not against a float model of it. That is the main reason I
trust the numbers above: there is no quantisation gap to discover later, because there is
no separate float path to diverge from.

## Building

Host tools and a refit, which rewrites `src/weights.h` and `host/golden.h`:

    cd host && make && ./btnfit

It is deterministic — same initialization seed, same output, byte for byte. `weights.h` is checked into
the repo on purpose, so the firmware builds without running the fitter first.

Firmware (once `src/main.c` is in — see Status):

    cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
    cmake --build build
    tools/flash.sh build/tactic.elf

Needs `arm-none-eabi-gcc` (tested with 15.3.Rel1), the MSPM0 SDK 2.10.00.04, and DSLite
from CCS for flashing. No OpenOCD. Override the SDK path with `-DMSPM0_SDK=...` if yours
lives somewhere else.

One trap worth mentioning oncemore, since it cost me a build: with Make or Ninja, `CMAKE_BUILD_TYPE`
defaults to empty, which means no `-O` flag at all. The binary came out at 1176 B against
TI's 688 B. `CMakeLists.txt` now defaults to Release.

## What is verified, and what is not

Verified: the fit reproduces byte-identically; host and firmware share one arithmetic path;
the FIFO indexing is phase-independent (all 32 start phases produce identical output, so
there is no incomplete-window case); both accumulator bounds are asserted at compile time.

Not verified: anything involving the board, because the runtime loop is not written yet.

Next, in order: the runtime loop and button ISR, then the golden vectors replayed on target,
then cycle counts from a TIMG channel. After that, two measurements I want — accuracy and
score margin against input jitter, and the same for a hand-written state machine on the same
traces.

## Where this sits

Reservoir computing is Jaeger's echo state networks (2001) and Maass's liquid state machines
(2002): a fixed nonlinear map over input history, with only a linear readout trained. The
feedforward version is Gauthier et al., *Next generation reservoir computing* (2021), which
drops recurrence entirely for time-delay taps and a linear readout.

The choice to keep the substrate linear and put the nonlinearity at the boundary has a nice
experimental precedent in Vandoorne et al., *Experimental demonstration of reservoir computing
on a silicon photonics chip* (Nature Communications, 2014): a passive optical mesh where the
only nonlinearity is the photodetector at the readout. Their Figure 5 shows a purely linear
readout failing XOR-with-memory and the detector nonlinearity fixing it, and their Figure 6
shows node nonlinearity buying little over a passive network. Different physics, same
structural argument.

## License

Source-available, not open source. Read it, build it, run it on your own hardware, modify it
for yourself. Commercial use, redistribution and incorporation into other software are
reserved — see [LICENSE](LICENSE). Files under `vendor/` belong to Texas Instruments and are
redistributed unmodified under BSD-3-Clause.
