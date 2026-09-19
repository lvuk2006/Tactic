// ^
// Property of Luka Vukmirica - All Rights Reserved. (luka.vukmirica@gmail.com)
/* btnvec.h -- button-trace vector generator for the MSPM0G3507 demo.
 *
 * TEST SCAFFOLDING (Claude-written, per the working agreement). This file
 * generates synthetic button-press traces and their regression targets. It
 * does NOT define any part of the runtime architecture -- it only produces
 * the data that btnfit.c fits weights against and that the golden-vector
 * comparison replays.
 *
 * Task: one button, sampled at STEP_HZ. Three target rhythms, which are the
 * three distinct permutations of the hold-duration multiset {tap,tap,dash}:
 *     A = tap  tap  dash
 *     B = dash tap  tap
 *     C = tap  dash tap
 * All three have identical total press time and identical press counts, so
 * any order-blind feature (sum/count/mean/energy) is at chance by
 * construction. Only order separates them.
 */
#ifndef BTNVEC_H
#define BTNVEC_H

#include <stdint.h>

#define STEP_HZ      10    /* 100 ms per step */
#define N_CLASS      3

/* Gesture timing in steps, at STEP_HZ. 3:1 dash/tap ratio (Morse-like) so the
   two stay separable under human timing slop. At 10 Hz: tap 0.5 s, dash 1.5 s,
   gap 0.5 s, whole gesture 3.5 s nominal / 4.5 s worst case with jitter. */
#define TAP_STEPS    5
#define DASH_STEPS   15
#define GAP_STEPS    5
#define N_PRESS      3     /* presses per pattern */
#define TRACE_STEPS  96    /* 9.6 s per trace */
#define TARGET_BOX   3     /* target held high for this many steps at completion */

/* Deterministic RNG so host and target replay identically and results are
   reproducible from a seed alone. Not cryptographic, does not need to be. */
typedef struct { uint32_t s; } Rng;

void  rnginit(Rng *r, uint32_t seed);
uint32_t rngnext(Rng *r);
int   rngrange(Rng *r, int lo, int hi);   /* inclusive both ends */

/* One generated example.
     btn[t]    = 1 while the button is held at step t, else 0
     y[c][t]   = 1 during the TARGET_BOX window at the completion of class c
     label     = 0..N_CLASS-1 for a target pattern, -1 for a negative
     endstep   = step at which the pattern completed, -1 for a negative */
typedef struct {
    uint8_t btn[TRACE_STEPS];
    float   y[N_CLASS][TRACE_STEPS];
    int     label;
    int     endstep;
    int     negkind;   /* negatives only: 0 idle, 1 held, 2 random, 3 near-miss */
} BtnTrace;

/* Names for BtnTrace.negkind, for diagnostics. */
extern const char *BTNVEC_NEGKIND[4];

/* Jitter model. Defaults are the ones described in the design: a human finger
   varies a tap by about one 50 ms step and a gap by about two. bounce_pct is
   the percent chance per held step of a single-step spurious release; leave it
   at 0 if the firmware debounces in hardware, raise it to train through a
   noisy switch. */
typedef struct {
    int hold_jit;     /* +/- steps applied to each hold  */
    int gap_jit;      /* +/- steps applied to each gap   */
    int bounce_pct;   /* 0..100, chance of a 1-step dropout inside a hold */
} Jitter;

extern const Jitter BTNVEC_DEFAULT_JITTER;

/* Generate one positive example of class `cls` (0..N_CLASS-1). */
void btnvec_positive(BtnTrace *out, int cls, const Jitter *j, Rng *r);

/* Generate one negative example. Draws uniformly from: idle (no press),
   held button, random press train, and near-miss patterns whose hold
   durations are NOT a permutation of {2,2,6}. */
void btnvec_negative(BtnTrace *out, const Jitter *j, Rng *r);

/* Fill a dataset: `per_class` positives of each class plus `n_neg` negatives,
   interleaved in a fixed order determined by the seed. Returns the number of
   traces written, or -1 if cap was too small. */
int btnvec_dataset(BtnTrace *out, int cap, int per_class, int n_neg,
                   const Jitter *j, uint32_t seed);

#endif
