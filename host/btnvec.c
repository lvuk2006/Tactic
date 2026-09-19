// ^
// Property of Luka Vukmirica - All Rights Reserved. (luka.vukmirica@gmail.com)
/* btnvec.c -- see btnvec.h. TEST SCAFFOLDING, not runtime code. */
#include <string.h>
#include "btnvec.h"

/* ---- deterministic RNG (xorshift32) --------------------------------- */

void rnginit(Rng *r, uint32_t seed) {
    r->s = seed ? seed : 0x9E3779B9u;   /* xorshift32 must never be seeded 0 */
}

uint32_t rngnext(Rng *r) {
    uint32_t x = r->s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->s = x;
    return x;
}

int rngrange(Rng *r, int lo, int hi) {
    if (hi <= lo) return lo;
    return lo + (int)(rngnext(r) % (uint32_t)(hi - lo + 1));
}

/* ---- patterns -------------------------------------------------------- */

/* The three classes are the three distinct permutations of {tap,tap,dash}. The gap
   between presses is the same for all three, so hold ORDER is the only thing
   that distinguishes them. */
#define T TAP_STEPS
#define D DASH_STEPS
static const int HOLD[N_CLASS][N_PRESS] = {
    {T, T, D},   /* A: tap  tap  dash */
    {D, T, T},   /* B: dash tap  tap  */
    {T, D, T},   /* C: tap  dash tap  */
};
#define GAP_BASE GAP_STEPS

/* Near-miss hold sets: deliberately NOT permutations of {tap,tap,dash}, because the
   three permutations that exist are exactly the three target classes. */
#define M ((TAP_STEPS + DASH_STEPS) / 2)   /* deliberately ambiguous middle duration */
static const int NEARMISS[][N_PRESS] = {
    {T, T, T}, {D, D, D}, {D, D, T}, {T, D, D},
    {D, T, D}, {M, M, M}, {T, T, M}, {M, T, D},
};
#define N_NEARMISS ((int)(sizeof(NEARMISS) / sizeof(NEARMISS[0])))

const char *BTNVEC_NEGKIND[4] = {"idle", "held", "random", "near-miss"};

const Jitter BTNVEC_DEFAULT_JITTER = { .hold_jit = 2, .gap_jit = 2, .bounce_pct = 0 };

/* Offset range for the start of the pattern within the trace. Lower bound is
   >0 so a pattern is never jammed against t=0; upper bound leaves room for the
   longest jittered pattern (45 steps at the default jitter) plus the target box. */
#define OFF_LO  4
#define OFF_HI  30

static int jit(int base, int amount, Rng *r) {
    int v = base + rngrange(r, -amount, amount);
    return v < 1 ? 1 : v;
}

/* Write a held run of `len` steps starting at `t`. Returns the step after the
   run. Applies bounce (a single-step spurious release) to interior steps only,
   since a bounce on the leading or trailing edge is indistinguishable from the
   hold simply being one step shorter. */
static int put_hold(uint8_t *btn, int t, int len, const Jitter *j, Rng *r) {
    for (int i = 0; i < len; i++) {
        if (t + i >= TRACE_STEPS) return TRACE_STEPS;
        int bounced = (j->bounce_pct > 0) && (i > 0) && (i < len - 1) &&
                      (rngrange(r, 1, 100) <= j->bounce_pct);
        btn[t + i] = bounced ? 0 : 1;
    }
    return t + len;
}

/* Render `n` presses with the given hold/gap durations. Returns the step index
   immediately after the final release, i.e. the completion instant. */
static int render(uint8_t *btn, int t, const int *holds, int n,
                  int gap_base, const Jitter *j, Rng *r) {
    for (int p = 0; p < n; p++) {
        t = put_hold(btn, t, jit(holds[p], j->hold_jit, r), j, r);
        if (p < n - 1) t += jit(gap_base, j->gap_jit, r);   /* gap: btn stays 0 */
        if (t >= TRACE_STEPS) return TRACE_STEPS;
    }
    return t;
}

static void clear(BtnTrace *o) {
    memset(o, 0, sizeof(*o));
    o->label = -1;
    o->endstep = -1;
    o->negkind = -1;
}

/* Parse a rendered trace back into press/gap runs. `gaps[i]` is the idle run
   following press i, or -1 if nothing follows it. Returns the press count. */
static int parse_runs(const uint8_t *b, int *holds, int *gaps, int max) {
    int n = 0, t = 0;
    while (t < TRACE_STEPS && n < max) {
        while (t < TRACE_STEPS && !b[t]) t++;
        if (t >= TRACE_STEPS) break;
        int h = 0;
        while (t < TRACE_STEPS && b[t]) { h++; t++; }
        holds[n] = h;
        int g = 0, u = t;
        while (u < TRACE_STEPS && !b[u]) { g++; u++; }
        gaps[n] = (u < TRACE_STEPS) ? g : -1;
        n++;
    }
    return n;
}

/* True if a trace is indistinguishable from some target class under the given
   jitter, i.e. three presses whose holds land in the tap/tap/dash bands
   with both internal gaps inside the positive gap range.

   This exists because near-miss hold durations that sit BETWEEN the tap and
   dash bands (the M entries above) can render as a genuine positive. Rather
   than hand-pruning the near-miss table -- which silently breaks again if the
   jitter is ever retuned -- negatives are filtered against this predicate, so
   the "a negative is never secretly a positive" invariant holds for every
   generator path and survives edits to the tables above. */
static int looks_positive(const uint8_t *btn, const Jitter *j) {
    int h[8], g[8];
    if (parse_runs(btn, h, g, 8) != N_PRESS) return 0;

    int shortlo = TAP_STEPS  - j->hold_jit, shorthi = TAP_STEPS  + j->hold_jit;
    int longlo  = DASH_STEPS - j->hold_jit, longhi  = DASH_STEPS + j->hold_jit;
    if (shortlo < 1) shortlo = 1;

    int nshort = 0, nlong = 0;
    for (int i = 0; i < N_PRESS; i++) {
        if (h[i] >= shortlo && h[i] <= shorthi) nshort++;
        else if (h[i] >= longlo && h[i] <= longhi) nlong++;
    }
    if (nshort != 2 || nlong != 1) return 0;

    int gaphi = GAP_BASE + j->gap_jit;
    for (int i = 0; i < N_PRESS - 1; i++)
        if (g[i] < 1 || g[i] > gaphi) return 0;

    return 1;
}

/* ---- public generators ---------------------------------------------- */

void btnvec_positive(BtnTrace *out, int cls, const Jitter *j, Rng *r) {
    clear(out);
    if (cls < 0 || cls >= N_CLASS) return;

    int end = render(out->btn, rngrange(r, OFF_LO, OFF_HI),
                     HOLD[cls], N_PRESS, GAP_BASE, j, r);

    out->label = cls;
    out->endstep = end;

    /* Target: a short box at the completion instant. A box rather than a single
       impulse so the fit is not asking for exact-step timing it cannot resolve
       through jitter. */
    for (int i = 0; i < TARGET_BOX; i++) {
        if (end + i < TRACE_STEPS) out->y[cls][end + i] = 1.0f;
    }
}

static void gen_negative(BtnTrace *out, const Jitter *j, Rng *r) {
    clear(out);

    int kind = rngrange(r, 0, 3);
    out->negkind = kind;
    switch (kind) {

    case 0:   /* idle -- button never pressed */
        break;

    case 1: { /* held down for a long time */
        int t = rngrange(r, OFF_LO, OFF_HI);
        put_hold(out->btn, t, rngrange(r, 3*DASH_STEPS, 6*DASH_STEPS), j, r);
        break;
    }

    case 2: { /* random press train: wrong counts and wrong durations */
        int t = rngrange(r, OFF_LO, 12);
        int n = rngrange(r, 1, 6);
        for (int p = 0; p < n && t < TRACE_STEPS; p++) {
            t = put_hold(out->btn, t, rngrange(r, 2, DASH_STEPS + 5), j, r);
            t += rngrange(r, 2, 2 * GAP_BASE + 4);
        }
        break;
    }

    default: { /* near miss: right shape, wrong rhythm */
        int t = rngrange(r, OFF_LO, OFF_HI);
        if (rngrange(r, 0, 3) == 0) {
            /* Hard negative: the CORRECT hold durations of a random class, but
               played far too slowly. Tests that the readout is matching the
               rhythm and not merely the press sequence.

               The slow gap must stay clear of a positive's jittered gap range,
               or the same trace can appear under both labels. A positive gap
               is GAP_BASE +/- gap_jit, so the slowest positive gap is
               GAP_BASE + gap_jit; the fastest gap here is 3*GAP_BASE -
               gap_jit. With the defaults that is 7 versus 13. */
            render(out->btn, t, HOLD[rngrange(r, 0, N_CLASS - 1)], N_PRESS,
                   GAP_BASE + rngrange(r, 2*GAP_BASE, 3*GAP_BASE), j, r);
        } else {
            render(out->btn, t, NEARMISS[rngrange(r, 0, N_NEARMISS - 1)],
                   N_PRESS, GAP_BASE, j, r);
        }
        break;
    }
    }
    /* label stays -1 and every y[c][t] stays 0: a negative asks all three
       readouts for silence across the whole trace. */
}

void btnvec_negative(BtnTrace *out, const Jitter *j, Rng *r) {
    /* Redraw until the result is not secretly a positive. The random-press and
       near-miss paths can both land on a valid pattern by chance; without this
       filter roughly 2% of negatives are mislabelled, which blurs exactly the
       timing boundary the demo is meant to demonstrate. Bounded so a
       pathological jitter setting cannot hang the generator -- if every draw
       collides we fall back to idle, which is unambiguously negative. */
    for (int attempt = 0; attempt < 32; attempt++) {
        gen_negative(out, j, r);
        if (!looks_positive(out->btn, j)) return;
    }
    clear(out);
}

int btnvec_dataset(BtnTrace *out, int cap, int per_class, int n_neg,
                   const Jitter *j, uint32_t seed) {
    int total = per_class * N_CLASS + n_neg;
    if (total > cap) return -1;

    Rng r;
    rnginit(&r, seed);

    int i = 0;
    for (int c = 0; c < N_CLASS; c++)
        for (int k = 0; k < per_class; k++) btnvec_positive(&out[i++], c, j, &r);
    for (int k = 0; k < n_neg; k++) btnvec_negative(&out[i++], j, &r);

    /* Fisher-Yates, so positives and negatives are not blocked together. Order
       does not affect a ridge fit, but it keeps any per-trace diagnostic the
       fitter prints from looking artificially clean early and dirty late. */
    for (int k = total - 1; k > 0; k--) {
        int m = rngrange(&r, 0, k);
        BtnTrace tmp = out[k]; out[k] = out[m]; out[m] = tmp;
    }
    return total;
}
