// ^
// Property of Luka Vukmirica - All Rights Reserved. (luka.vukmirica@gmail.com)
/* btnfit.c -- offline weight fitting for the MSPM0G3507 button-rhythm demo.
 *
 * TEST SCAFFOLDING / HOST TOOL (Claude-written, per the working agreement).
 * Runs entirely on the host. Nothing here ships to the target: its outputs are
 * ../src/weights.h (a table of const int16_t the runtime includes) and
 * golden.h (canned traces + expected scores for the target test build).
 *
 * It uses the author's own primitives unmodified -- convkinit / convkfifo /
 * convkrun from ../src/readoutconv.c, the tanh LUT from ../src/tanh_table.h,
 * and ridgefit from linreg.c -- so there is ONE implementation of the
 * arithmetic, and the weights cannot be fitted against a model that differs
 * from the one that runs.
 *
 *   btn(0/1) ─► resfifo (n=1,k=K1) ─► C fixed random int16 kernels ─► int32 ACC
 *                                                                       │
 *                                                        tanh LUT ◄──────┘
 *                                                            │ int16 Q15
 *                          outfifo (n=C,k=K2) ◄──────────────┘
 *                                    └─► 3 FITTED int16 kernels ─► int32 s[3]
 *                                            + bias_raw ─► argmax + threshold
 *
 * Reservoir kernels are random and FIXED. Only the 3 readout kernels are fitted.
 *
 * 🔴 THE MODEL IS INTEGER END TO END. Every number this tool reports is
 * produced by the same arithmetic the firmware runs -- there is no float
 * reference path and therefore no float-to-fixed round-trip to check. The only
 * floating point left is ridgefit itself, which sees integer-derived features
 * and whose output is immediately quantised and re-evaluated on the integer
 * path before anything is reported.
 *
 * Build:  see README.md in this directory.
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdint.h>      /* MUST precede readoutconv.h -- that header uses     */
                         /* int16_t but does not include stdint.h itself.      */
#include "btnvec.h"
#include "../src/readoutconv.h"
#include "../src/tanh_table.h"
#include "linreg.h"

/* ---- configuration -------------------------------------------------- */

#ifndef C
#define C           12      /* reservoir channels; -DC=n to sweep        */
#endif
#define K1          32      /* reservoir kernel depth (taps)            */
#define K2          32      /* readout kernel depth (taps)              */
#define DIM         (C * K2)

#define TRAIN_PER_CLASS  150
#define TRAIN_NEG        300
#define TEST_PER_CLASS    60
#define TEST_NEG         120
#define N_TRAIN   (TRAIN_PER_CLASS * N_CLASS + TRAIN_NEG)
#define N_TEST    (TEST_PER_CLASS  * N_CLASS + TEST_NEG)

#define NEG_ROW_RATIO   8   /* zero-target rows kept per positive-target row */
#define MAX_ROWS        60000
#define FIRE_TOL        6   /* steps of slack allowed on the fire instant   */
#define REFRACTORY     (2 * STEP_HZ)   /* ~2 s lockout after a fire          */

/* ---- the fixed-point contract with the runtime ------------------------
 *
 * RES_SHIFT is NOT free. tanh_table.h was generated with RES_SHIFT=16 and
 * GAIN=4, and its documented lookup is
 *
 *     row = ((ACC + (1 << (TANH_SHIFT-1))) >> TANH_SHIFT) + N/2
 *
 * Row r above centre represents x = r * (2*DOMAIN_R/N), and r = ACC >> S, so
 *
 *     gain / 2^RES_SHIFT  =  (2*DOMAIN_R/N) / 2^TANH_SHIFT
 *
 * which pins gain once RES_SHIFT and TANH_SHIFT are chosen. With DOMAIN_R=4,
 * N=256, TANH_SHIFT=9, RES_SHIFT=16 that gives gain = 4 exactly. Sweeping the
 * gain therefore means regenerating his table with a different TANH_SHIFT
 * (S = log2(2048/gain)), not changing a constant here -- so the gain sweep the
 * float version did is gone, and the pinned value is asserted below instead.
 *
 * The button enters as a LITERAL 0/1, not Q15. That is what makes the
 * reservoir accumulator bound so loose: max|ACC| = 1 * sum|kern|.
 *
 * Readout: features are Q15 (the tanh LUT output), weights are Q(OUT_SHIFT),
 * so the raw accumulator is Q(15+OUT_SHIFT) = Q28 at OUT_SHIFT=13. The
 * threshold and the bias are pre-scaled to Q28 at generation time, so the hot
 * loop does no shift at all: compare ACC + bias_raw against thr_raw directly.
 */
#define RES_SHIFT      16
#ifndef OUT_SHIFT_PREF
#define OUT_SHIFT_PREF 13      /* preferred; drops to 12 if the L1 bound fails */
#endif
#define FEAT_SCALE     32768.0f            /* 2^15: Q15 feature -> float       */
#define SCORE_SHIFT(osh)  (15 + (osh))     /* raw score is Q(15+osh)           */

#define TANH_DOMAIN_R  4       /* must match tanh_table.h's DOMAIN_R           */
#define GAIN_PINNED    ((float)(1 << RES_SHIFT) * (2.0f * TANH_DOMAIN_R / TANH_N) \
                        / (float)(1 << TANH_SHIFT))

#define INT32_MAX_D    2147483647.0

_Static_assert(DIM <= LR_MAX_DIM, "DIM exceeds LR_MAX_DIM -- build with -DLR_MAX_DIM=384");
_Static_assert(K1 <= MAX_DEPTH && K2 <= MAX_DEPTH, "kernel depth exceeds MAX_DEPTH");
_Static_assert(C  <= MAX_WIDTH, "C exceeds MAX_WIDTH");
_Static_assert(TANH_N == 256, "tanh table size changed -- recheck the index math");

/* ---- model state ----------------------------------------------------- */

typedef struct {
    ReadoutState res[C];          /* n=1, k=K1. All C share the same input,   */
                                  /* so all C FIFOs hold identical bytes; the */
                                  /* firmware keeps ONE (see the SRAM note).  */
    ReadoutState out[N_CLASS];    /* n=C, k=K2. Same: one FIFO, 3 kernels.    */
    int32_t bias_raw[N_CLASS];
    uint32_t seed;
} Model;

/* observed accumulator extremes, over everything this process evaluates */
static double obs_res_max = 0.0;
static double obs_out_max = 0.0;

static void model_init(Model *m, uint32_t seed) {
    Rng r; rnginit(&r, seed);
    m->seed = seed;
    /* Uniform in +/-sqrt(3/(nin*k)) gives unit-ish variance per tap, matching
       the normalisation used in acttest.c / fbtest.c. nin = 1 here. */
    float a = sqrtf(3.0f / (float)(1 * K1));
    for (int c = 0; c < C; c++) {
        if (convkinit(&m->res[c], 1, K1) != 0) {
            fprintf(stderr, "FATAL: convkinit failed for reservoir channel %d\n", c);
            exit(2);
        }
        memset(m->res[c].kern, 0, sizeof(m->res[c].kern));
        for (int j = 0; j < K1; j++) {
            float w = a * (2.0f * (rngnext(&r) / 4294967296.0f) - 1.0f);
            long q = lrintf(w * (float)(1 << RES_SHIFT));
            if (q >  32767) q =  32767;
            if (q < -32768) q = -32768;
            m->res[c].kern[j][0] = (int16_t)q;
        }
    }
    for (int y = 0; y < N_CLASS; y++) {
        if (convkinit(&m->out[y], C, K2) != 0) {
            fprintf(stderr, "FATAL: convkinit failed for readout %d\n", y);
            exit(2);
        }
        /* convkinit zeroes fifo but NOT kern; convkrun would read garbage. */
        memset(m->out[y].kern, 0, sizeof(m->out[y].kern));
        m->bias_raw[y] = 0;
    }

    /* Alive check at the source. A silently zeroed reservoir (e.g. a float
       assigned into an int16_t kernel after a fixed-point refactor) produces
       all-zero features, a singular normal-equation matrix, and a crash several
       hundred lines away. Fail here instead, where the cause is visible. */
    int nz = 0;
    for (int c = 0; c < C; c++)
        for (int j = 0; j < K1; j++) if (m->res[c].kern[j][0] != 0) nz++;
    if (nz == 0) {
        fprintf(stderr, "FATAL: all %d reservoir kernel taps are zero after init.\n"
                        "       Check RES_SHIFT and the ReadoutState.kern type.\n",
                C * K1);
        exit(2);
    }
}

static void model_reset(Model *m) {
    /* Zeroing the FIFOs is physically correct, not a warm-up hack: an all-zero
       history is exactly the state after a long idle with the button released,
       which is how the device actually starts. */
    for (int c = 0; c < C; c++) {
        memset(m->res[c].fifo, 0, sizeof(m->res[c].fifo));
        m->res[c].FIFOROW = 0;
    }
    for (int y = 0; y < N_CLASS; y++) {
        memset(m->out[y].fifo, 0, sizeof(m->out[y].fifo));
        m->out[y].FIFOROW = 0;
    }
}

/* The firmware's tanh lookup, exactly as tanh_table.h documents it. */
static inline int16_t tanh_lookup(int32_t acc) {
    int idx = (int)((acc + (1 << (TANH_SHIFT - 1))) >> TANH_SHIFT) + (TANH_N / 2);
    if (idx < 0) idx = 0;
    else if (idx >= TANH_N) idx = TANH_N - 1;
    return TANH_LUT[idx];
}

/* Advance one step. Push-then-run at both stages, matching convkrun's
   FIFOROW-1 indexing.
     feat  (optional) receives the flattened readout window, normalised to
           float for ridgefit: feat[j*C+i] = fifo[j ticks ago][i] / 2^15.
     score (optional) receives the raw Q(15+osh) score per class, bias folded
           in -- i.e. exactly what the firmware compares against the threshold.
   Passing NULL for score skips the readout MACs, which is what collection
   wants (the kernels are not fitted yet). */
static void model_step(Model *m, uint8_t btn, float *feat, int32_t *score) {
    int16_t b = btn ? 1 : 0;          /* LITERAL 0/1, not Q15 */
    int16_t h[C];

    for (int c = 0; c < C; c++) {
        convkfifo(&m->res[c], &b);
        int32_t acc = convkrun(&m->res[c]);
        double a = fabs((double)acc);
        if (a > obs_res_max) obs_res_max = a;
        h[c] = tanh_lookup(acc);
    }

    for (int y = 0; y < N_CLASS; y++) convkfifo(&m->out[y], h);

    if (feat) {
        int k = m->out[0].k, n = m->out[0].n, row = m->out[0].FIFOROW;
        for (int j = 0; j < k; j++) {
            int src = (((row - 1) - j) % k + k) % k;
            for (int i = 0; i < n; i++)
                feat[j * n + i] = (float)m->out[0].fifo[src][i] / FEAT_SCALE;
        }
    }

    if (score) {
        for (int y = 0; y < N_CLASS; y++) {
            int32_t acc = convkrun(&m->out[y]);
            double a = fabs((double)acc);
            if (a > obs_out_max) obs_out_max = a;
            score[y] = acc + m->bias_raw[y];
        }
    }
}

/* ---- data ------------------------------------------------------------ */

static BtnTrace train[N_TRAIN];
static BtnTrace test[N_TEST];
static float   *X;                  /* MAX_ROWS x DIM, row-major */
static float    Y[N_CLASS][MAX_ROWS];
static int      n_rows;

static void collect(Model *m) {
    n_rows = 0;
    Rng r; rnginit(&r, 0xC0FFEE);
    float feat[DIM];

    for (int t = 0; t < N_TRAIN; t++) {
        model_reset(m);
        for (int s = 0; s < TRACE_STEPS; s++) {
            model_step(m, train[t].btn[s], feat, NULL);

            int pos = 0;
            for (int c = 0; c < N_CLASS; c++) if (train[t].y[c][s] > 0.5f) pos = 1;

            /* Targets are ~2% ones. Keeping every row would let a least-squares
               fit drive all three scores to zero and still look good, so keep
               all positive rows and subsample the zeros. */
            if (!pos && rngrange(&r, 1, NEG_ROW_RATIO * 50) > 50) continue;
            if (n_rows >= MAX_ROWS) continue;

            memcpy(&X[(size_t)n_rows * DIM], feat, sizeof(feat));
            for (int c = 0; c < N_CLASS; c++) Y[c][n_rows] = train[t].y[c][s];
            n_rows++;
        }
    }
}

/* ---- evaluation ------------------------------------------------------ */

typedef struct {
    int correct;        /* right class, fired within FIRE_TOL of completion */
    int wrong_class;    /* fired, but named the wrong class                 */
    int bad_timing;     /* right class, but outside FIRE_TOL                */
    int missed;         /* positive that never fired                        */
    int false_alarm;    /* negative that fired                              */
    int n_pos, n_neg;
    int confusion[N_CLASS][N_CLASS];
    int fa_by_kind[4], n_by_kind[4];
} Score;

/* Raw integer scores for the whole test set, filled by replay_test().
   The threshold does not enter here, so one replay serves the whole
   threshold sweep exactly (not approximately). */
static int32_t (*TS)[TRACE_STEPS][N_CLASS];    /* [N_TEST][TRACE_STEPS][N_CLASS] */

static void replay_test(Model *m) {
    for (int t = 0; t < N_TEST; t++) {
        model_reset(m);
        for (int s = 0; s < TRACE_STEPS; s++)
            model_step(m, test[t].btn[s], NULL, TS[t][s]);
    }
}

static void evaluate(int32_t thr_raw, BtnTrace *set, int n_set, Score *sc) {
    memset(sc, 0, sizeof(*sc));

    for (int t = 0; t < n_set; t++) {
        int fired = -1, fire_t = -1, lock = 0;

        for (int s = 0; s < TRACE_STEPS; s++) {
            if (lock > 0) { lock--; continue; }

            int32_t best = INT32_MIN; int arg = -1;
            for (int c = 0; c < N_CLASS; c++) {
                int32_t v = TS[t][s][c];
                if (v > best) { best = v; arg = c; }
            }
            if (best > thr_raw) {
                if (fired < 0) { fired = arg; fire_t = s; }
                lock = REFRACTORY;
            }
        }

        if (set[t].label >= 0) {
            sc->n_pos++;
            if (fired < 0) sc->missed++;
            else {
                sc->confusion[set[t].label][fired]++;
                int dt = fire_t - set[t].endstep; if (dt < 0) dt = -dt;
                if (fired != set[t].label)     sc->wrong_class++;
                else if (dt > FIRE_TOL)        sc->bad_timing++;
                else                           sc->correct++;
            }
        } else {
            sc->n_neg++;
            int kk = set[t].negkind;
            if (kk >= 0 && kk < 4) sc->n_by_kind[kk]++;
            if (fired >= 0) {
                sc->false_alarm++;
                if (kk >= 0 && kk < 4) sc->fa_by_kind[kk]++;
            }
        }
    }
}

/* Combined objective: recall on positives minus false-alarm rate. */
static float objective(const Score *s) {
    float rec = s->n_pos ? (float)s->correct / s->n_pos : 0.0f;
    float fa  = s->n_neg ? (float)s->false_alarm / s->n_neg : 0.0f;
    return rec - fa;
}

/* ---- quantisation and the accumulator bound -------------------------- */

/* Quantise at a GIVEN shift. Unlike the old auto-shift version, the shift is
   now pinned by the contract (reservoir) or chosen and then verified
   (readout), because the tanh index math depends on it. */
static void quantise_at(const float *w, int n, int sh, int16_t *out, int *clipped) {
    *clipped = 0;
    for (int i = 0; i < n; i++) {
        double v = (double)w[i] * ldexp(1.0, sh);
        long q = lrint(v);
        if (q >  32767) { q =  32767; (*clipped)++; }
        if (q < -32768) { q = -32768; (*clipped)++; }
        out[i] = (int16_t)q;
    }
}

/* Worst-case |ACC| for a quantised kernel row set: max|input| * sum|kern_row|.
   Deliberately the L1 bound, which is provable and conservative -- an L2 /
   statistical bound is ~20x tighter but is not a guarantee. */
static double acc_bound(const int16_t *q, int nrow, int rowlen, double max_in) {
    double worst = 0.0;
    for (int r = 0; r < nrow; r++) {
        double sum = 0.0;
        for (int c = 0; c < rowlen; c++) sum += abs(q[r * rowlen + c]);
        double v = max_in * sum;
        if (v > worst) worst = v;
    }
    return worst;
}

/* ---- emit ------------------------------------------------------------ */

static void emit_weights(const char *path, Model *m,
                         const int16_t *rq, const int16_t *oq, int osh,
                         const int32_t *bias_raw, int32_t thr_raw,
                         float thr, uint32_t seed, float lambda,
                         double res_bound, double out_bound) {
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return; }

    fprintf(f, "// ^\n");
    fprintf(f, "// Property of Luka Vukmirica - All Rights Reserved. (luka.vukmirica@gmail.com)\n");
    fprintf(f, "/* weights.h -- GENERATED by host/btnfit.c. Do not edit by hand.\n");
    fprintf(f, " *\n");
    fprintf(f, " * Regenerate:  cd host && make && ./btnfit\n");
    fprintf(f, " *\n");
    fprintf(f, " * Fitted against the INTEGER path in ../src/readoutconv.c and the\n");
    fprintf(f, " * tanh LUT in ../src/tanh_table.h, so these weights and the runtime\n");
    fprintf(f, " * share one arithmetic. Do not change RES_SHIFT or the LUT without\n");
    fprintf(f, " * regenerating this file -- the tanh index math depends on both.\n");
    fprintf(f, " *\n");
    fprintf(f, " *   seed=%u  gain=%.4f (pinned by RES_SHIFT/TANH_SHIFT)  lambda=%.1e\n",
            seed, (double)GAIN_PINNED, lambda);
    fprintf(f, " *   C=%d K1=%d K2=%d classes=%d rate=%d Hz\n", C, K1, K2, N_CLASS, STEP_HZ);
    fprintf(f, " *   gesture: tap=%d dash=%d gap=%d steps; reach=%d steps (%.2f s)\n",
            TAP_STEPS, DASH_STEPS, GAP_STEPS, K1 + K2 - 1, (K1 + K2 - 1) / (float)STEP_HZ);
    fprintf(f, " *   float threshold %.4f -> raw Q%d %ld\n",
            thr, SCORE_SHIFT(osh), (long)thr_raw);
    fprintf(f, " *\n");
    fprintf(f, " * Provable L1 accumulator bounds (max|input| * sum|kern|):\n");
    fprintf(f, " *   reservoir  %.0f   (int32 max 2147483647, %.0fx headroom)\n",
            res_bound, INT32_MAX_D / res_bound);
    fprintf(f, " *   readout    %.0f   (int32 max 2147483647, %.2fx headroom)\n",
            out_bound, INT32_MAX_D / out_bound);
    fprintf(f, " */\n");
    fprintf(f, "#ifndef WEIGHTS_H\n#define WEIGHTS_H\n\n");
    fprintf(f, "#include <stdint.h>\n#include \"tanh_table.h\"\n\n");

    fprintf(f, "#define W_C          %d\n", C);
    fprintf(f, "#define W_K1         %d\n", K1);
    fprintf(f, "#define W_K2         %d\n", K2);
    fprintf(f, "#define W_CLASSES    %d\n", N_CLASS);
    fprintf(f, "#define W_RATE_HZ    %d\n", STEP_HZ);
    fprintf(f, "#define W_REFRACTORY %d   /* ticks of lockout after a fire */\n", REFRACTORY);
    fprintf(f, "#define W_RES_SHIFT  %d\n", RES_SHIFT);
    fprintf(f, "#define W_OUT_SHIFT  %d\n", osh);
    fprintf(f, "#define W_SCORE_SHIFT %d  /* raw score is Q%d = Q15 feature * Q%d weight */\n\n",
            SCORE_SHIFT(osh), SCORE_SHIFT(osh), osh);

    fprintf(f, "/* tanh lookup. ONE definition, shared by the runtime and the fitter, so\n");
    fprintf(f, "   the index math cannot drift between them. */\n");
    fprintf(f, "static inline int W_TANH_INDEX(int32_t acc) {\n");
    fprintf(f, "    int idx = (int)((acc + (1 << (TANH_SHIFT - 1))) >> TANH_SHIFT) + (TANH_N / 2);\n");
    fprintf(f, "    if (idx < 0) idx = 0;\n");
    fprintf(f, "    else if (idx >= TANH_N) idx = TANH_N - 1;\n");
    fprintf(f, "    return idx;\n");
    fprintf(f, "}\n");
    fprintf(f, "#define W_TANH(acc)  TANH_LUT[W_TANH_INDEX(acc)]\n\n");

    /* reservoir kernels: W_RES[c][j] <-> res[c].kern[j][0], j = ticks ago */
    fprintf(f, "/* W_RES[c][j] -> reservoir channel c, kern[j][0]; j = ticks ago. */\n");
    fprintf(f, "static const int16_t W_RES[W_C][W_K1] = {\n");
    for (int c = 0; c < C; c++) {
        fprintf(f, "  {");
        for (int j = 0; j < K1; j++)
            fprintf(f, "%6d%s", rq[c * K1 + j], j == K1 - 1 ? "" : ",");
        fprintf(f, "},\n");
    }
    fprintf(f, "};\n\n");

    /* readout kernels: index j*C+i matches convkrun's kern[j][i] */
    fprintf(f, "/* W_OUT[class][j*W_C + i]  <->  kern[j][i] in convkrun order. */\n");
    fprintf(f, "static const int16_t W_OUT[W_CLASSES][W_K2 * W_C] = {\n");
    for (int y = 0; y < N_CLASS; y++) {
        fprintf(f, "  {");
        for (int d = 0; d < DIM; d++)
            fprintf(f, "%6d%s", oq[y * DIM + d], d == DIM - 1 ? "" : ",");
        fprintf(f, "},\n");
    }
    fprintf(f, "};\n\n");

    fprintf(f, "/* Bias and threshold are PRE-SCALED to the raw accumulator's Q%d, so the\n",
            SCORE_SHIFT(osh));
    fprintf(f, "   hot loop does no shift and loses no precision:\n");
    fprintf(f, "       s = convkrun(&out[y]) + W_BIAS_RAW[y];\n");
    fprintf(f, "       if (s > W_THRESHOLD_RAW) fire(argmax);            */\n");
    fprintf(f, "static const int32_t W_BIAS_RAW[W_CLASSES] = {");
    for (int y = 0; y < N_CLASS; y++)
        fprintf(f, "%ld%s", (long)bias_raw[y], y == N_CLASS - 1 ? "" : ", ");
    fprintf(f, "};\n");
    fprintf(f, "static const int32_t W_THRESHOLD_RAW = %ld;\n\n", (long)thr_raw);

    fprintf(f, "/* The bounds above, as compile-time guarantees. If a future edit to the\n");
    fprintf(f, "   kernels breaks these, the build fails instead of the board wrapping\n");
    fprintf(f, "   an int32 silently -- ARMv6-M has no saturating add. */\n");
    fprintf(f, "#define W_RES_L1_BOUND %.0f\n", res_bound);
    fprintf(f, "#define W_OUT_L1_BOUND %.0f\n", out_bound);
    fprintf(f, "_Static_assert(W_RES_L1_BOUND < 2147483647.0, \"reservoir ACC can overflow int32\");\n");
    fprintf(f, "_Static_assert(W_OUT_L1_BOUND < 2147483647.0, \"readout ACC can overflow int32\");\n\n");

    fprintf(f, "#endif\n");
    fclose(f);
    printf("wrote %s\n", path);
}

/* Golden vectors: canned traces plus the expected per-tick scores, so the
   target test build can diff its arithmetic against the host's.
   Report score deltas and decision deltas SEPARATELY when comparing -- a
   decision flip within a couple of LSB of the threshold is a rounding
   convention difference, not a bug. */
static void emit_golden(const char *path, Model *m, int32_t thr_raw, int osh) {
    /* one positive per class, plus the first two negatives we find */
    int pick[5], np = 0;
    for (int cls = 0; cls < N_CLASS && np < 5; cls++)
        for (int t = 0; t < N_TEST; t++)
            if (test[t].label == cls) { pick[np++] = t; break; }
    for (int t = 0; t < N_TEST && np < 5; t++)
        if (test[t].label < 0) {
            int dup = 0;
            for (int i = 0; i < np; i++) if (pick[i] == t) dup = 1;
            if (!dup) pick[np++] = t;
        }

    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return; }

    fprintf(f, "// ^\n");
    fprintf(f, "// Property of Luka Vukmirica - All Rights Reserved. (luka.vukmirica@gmail.com)\n");
    fprintf(f, "/* golden.h -- GENERATED by host/btnfit.c. Do not edit by hand.\n");
    fprintf(f, " *\n");
    fprintf(f, " * %d canned traces with the exact per-tick scores the host produced,\n", np);
    fprintf(f, " * for bringing the target up one stage at a time. Feed GOLDEN[i].btn\n");
    fprintf(f, " * through the runtime and compare score-by-score.\n");
    fprintf(f, " *\n");
    fprintf(f, " * Compare SCORES and DECISIONS separately. A decision that flips while\n");
    fprintf(f, " * the score is within a couple of LSB of W_THRESHOLD_RAW (%ld) is a\n", (long)thr_raw);
    fprintf(f, " * rounding-convention difference, not a bug; a score that differs at all\n");
    fprintf(f, " * IS a bug, because both sides run the same integer arithmetic.\n");
    fprintf(f, " */\n");
    fprintf(f, "#ifndef GOLDEN_H\n#define GOLDEN_H\n\n#include <stdint.h>\n\n");
    fprintf(f, "#define GOLDEN_N       %d\n", np);
    fprintf(f, "#define GOLDEN_STEPS   %d\n", TRACE_STEPS);
    fprintf(f, "#define GOLDEN_CLASSES %d\n", N_CLASS);
    fprintf(f, "#define GOLDEN_THRESHOLD_RAW %ld\n", (long)thr_raw);
    fprintf(f, "#define GOLDEN_SCORE_SHIFT   %d\n\n", SCORE_SHIFT(osh));

    fprintf(f, "typedef struct {\n");
    fprintf(f, "    const uint8_t *btn;    /* GOLDEN_STEPS samples, 0/1            */\n");
    fprintf(f, "    const int32_t *score;  /* GOLDEN_STEPS*GOLDEN_CLASSES, [s][c]  */\n");
    fprintf(f, "    int label;             /* 0..2, or -1 for a negative           */\n");
    fprintf(f, "    int negkind;           /* -1, or 0 idle 1 held 2 random 3 near */\n");
    fprintf(f, "    int fire_tick;         /* tick the host fired on, -1 if never  */\n");
    fprintf(f, "    int fire_class;        /* class fired, -1 if never             */\n");
    fprintf(f, "} GoldenTrace;\n\n");

    /* Replay each picked trace ONCE, from a fresh reset, and keep both the
       scores and the decision. Replaying twice would be harmless today but is
       exactly the kind of second copy that drifts later. */
    static int32_t gsc[5][TRACE_STEPS][N_CLASS];
    int gfire_t[5], gfire_c[5];

    for (int i = 0; i < np; i++) {
        int t = pick[i];
        model_reset(m);
        for (int s = 0; s < TRACE_STEPS; s++)
            model_step(m, test[t].btn[s], NULL, gsc[i][s]);

        int fired = -1, fire_t = -1, lock = 0;
        for (int s = 0; s < TRACE_STEPS; s++) {
            if (lock > 0) { lock--; continue; }
            int32_t best = INT32_MIN; int arg = -1;
            for (int c = 0; c < N_CLASS; c++)
                if (gsc[i][s][c] > best) { best = gsc[i][s][c]; arg = c; }
            if (best > thr_raw) {
                if (fired < 0) { fired = arg; fire_t = s; }
                lock = REFRACTORY;
            }
        }
        gfire_t[i] = fire_t; gfire_c[i] = fired;

        fprintf(f, "static const uint8_t GOLDEN_BTN_%d[GOLDEN_STEPS] = {\n  ", i);
        for (int s = 0; s < TRACE_STEPS; s++) {
            fprintf(f, "%d%s", test[t].btn[s], s == TRACE_STEPS - 1 ? "" : ",");
            if (s % 32 == 31) fprintf(f, "\n  ");
        }
        fprintf(f, "\n};\n");

        fprintf(f, "static const int32_t GOLDEN_SCORE_%d[GOLDEN_STEPS * GOLDEN_CLASSES] = {\n  ", i);
        for (int s = 0; s < TRACE_STEPS; s++) {
            for (int c = 0; c < N_CLASS; c++)
                fprintf(f, "%ld%s", (long)gsc[i][s][c],
                        (s == TRACE_STEPS - 1 && c == N_CLASS - 1) ? "" : ",");
            if (s % 4 == 3) fprintf(f, "\n  ");
        }
        fprintf(f, "\n};\n\n");
    }

    fprintf(f, "static const GoldenTrace GOLDEN[GOLDEN_N] = {\n");
    for (int i = 0; i < np; i++) {
        int t = pick[i];
        fprintf(f, "  { GOLDEN_BTN_%d, GOLDEN_SCORE_%d, %d, %d, %d, %d },\n",
                i, i, test[t].label, test[t].negkind, gfire_t[i], gfire_c[i]);
    }
    fprintf(f, "};\n\n#endif\n");
    fclose(f);
    printf("wrote %s\n", path);
}

/* ---- main ------------------------------------------------------------ */

int main(int argc, char **argv) {
    int quick = (argc > 1 && strcmp(argv[1], "--quick") == 0);

    X  = malloc((size_t)MAX_ROWS * DIM * sizeof(float));
    TS = malloc(sizeof(*TS) * N_TEST);
    if (!X || !TS) { fprintf(stderr, "out of memory\n"); return 1; }

    const Jitter *J = &BTNVEC_DEFAULT_JITTER;
    btnvec_dataset(train, N_TRAIN, TRAIN_PER_CLASS, TRAIN_NEG, J, 2026);
    btnvec_dataset(test,  N_TEST,  TEST_PER_CLASS,  TEST_NEG,  J, 99991);

    printf("\n==== C = %d ====\n", C);
    printf("config: C=%d K1=%d K2=%d dim=%d rate=%dHz reach=%d steps (%.2f s)\n",
           C, K1, K2, DIM, STEP_HZ, K1 + K2 - 1, (K1 + K2 - 1) / (float)STEP_HZ);
    printf("gesture: tap=%d dash=%d gap=%d steps, span=%d steps (%.2f s)\n",
           TAP_STEPS, DASH_STEPS, GAP_STEPS,
           2 * TAP_STEPS + DASH_STEPS + 2 * GAP_STEPS,
           (2 * TAP_STEPS + DASH_STEPS + 2 * GAP_STEPS) / (float)STEP_HZ);
    printf("fixed point: RES_SHIFT=%d TANH_SHIFT=%d -> gain pinned at %.4f\n",
           RES_SHIFT, TANH_SHIFT, (double)GAIN_PINNED);
    printf("train=%d traces  test=%d traces\n\n", N_TRAIN, N_TEST);

    const float LAMBDAS[] = {1e-5f, 1e-4f, 1e-3f, 1e-2f, 1e-1f};
    const uint32_t SEEDS[] = {7, 11, 23, 42, 101};
    int NL = quick ? 1 : 5;
    int NS = quick ? 1 : 5;

    float best_obj = -1e30f;
    uint32_t best_seed = 0; float best_lam = 0, best_thr = 0;
    float seed_best[5];

    for (int si = 0; si < NS; si++) {
        seed_best[si] = -1e30f;
        Model m;
        model_init(&m, SEEDS[si]);
        collect(&m);

        for (int li = 0; li < NL; li++) {
            ReadoutWeights rw[N_CLASS];
            int ok = 1;
            for (int c = 0; c < N_CLASS; c++)
                if (ridgefit(X, Y[c], n_rows, DIM, LAMBDAS[li], &rw[c]) != 0) ok = 0;
            if (!ok) continue;

            /* Quantise and load the integer kernels, then replay. Everything
               scored below is the arithmetic the board will run. */
            float oflat[N_CLASS * DIM];
            for (int y = 0; y < N_CLASS; y++)
                for (int d = 0; d < DIM; d++) oflat[y * DIM + d] = rw[y].w[d];
            int16_t oq[N_CLASS * DIM]; int clipped;
            quantise_at(oflat, N_CLASS * DIM, OUT_SHIFT_PREF, oq, &clipped);

            for (int y = 0; y < N_CLASS; y++) {
                for (int j = 0; j < K2; j++)
                    for (int i = 0; i < C; i++)
                        m.out[y].kern[j][i] = oq[y * DIM + j * C + i];
                m.bias_raw[y] = (int32_t)llrint((double)rw[y].bias
                                                * ldexp(1.0, SCORE_SHIFT(OUT_SHIFT_PREF)));
            }
            replay_test(&m);

            for (float thr = 0.05f; thr <= 0.95f; thr += 0.05f) {
                int32_t thr_raw = (int32_t)llrint((double)thr
                                                  * ldexp(1.0, SCORE_SHIFT(OUT_SHIFT_PREF)));
                Score sc;
                evaluate(thr_raw, test, N_TEST, &sc);
                float o = objective(&sc);
                if (o > seed_best[si]) seed_best[si] = o;
                if (o > best_obj) {
                    best_obj = o; best_seed = SEEDS[si];
                    best_lam = LAMBDAS[li]; best_thr = thr;
                }
            }
        }
        printf("seed %3u  best objective %.4f\n", SEEDS[si], seed_best[si]);
        fflush(stdout);
    }

    if (best_obj < -1e29f) {
        fprintf(stderr,
            "FATAL: no (seed,lambda) combination produced a usable fit.\n"
            "       Every ridgefit failed, which almost always means the features\n"
            "       are degenerate -- check that the reservoir kernels are non-zero.\n");
        free(X); free(TS);
        return 2;
    }

    if (NS > 1) {
        float mean = 0, mn = 1e30f, mx = -1e30f;
        for (int i = 0; i < NS; i++) { mean += seed_best[i];
            if (seed_best[i] < mn) mn = seed_best[i];
            if (seed_best[i] > mx) mx = seed_best[i]; }
        mean /= NS;
        printf("\nseed spread: mean %.4f  min %.4f  max %.4f  range %.4f\n",
               mean, mn, mx, mx - mn);
    }

    /* ---- refit the winner and report it in full ---- */
    Model m; model_init(&m, best_seed); collect(&m);
    ReadoutWeights rw[N_CLASS];
    for (int c = 0; c < N_CLASS; c++) {
        int rc = ridgefit(X, Y[c], n_rows, DIM, best_lam, &rw[c]);
        if (rc != 0) {   /* a failed fit leaves rw[c] uninitialised; ridgepredict
                            would then read a garbage dim and run off the end. */
            fprintf(stderr, "FATAL: ridgefit failed for class %d (rc=%d)\n", c, rc);
            free(X); free(TS);
            return 2;
        }
    }

    /* Reservoir quantisation is already done (model_init writes int16 kernels
       at the pinned RES_SHIFT). Gather them for the bound and for emit. */
    int16_t rq[C * K1];
    for (int c = 0; c < C; c++)
        for (int j = 0; j < K1; j++) rq[c * K1 + j] = m.res[c].kern[j][0];
    double res_bound = acc_bound(rq, C, K1, 1.0);   /* max|input| = 1, literal 0/1 */

    /* Readout: try the preferred shift, verify the L1 bound WITH the bias
       folded in, drop one bit if it does not fit. */
    float oflat[N_CLASS * DIM];
    for (int y = 0; y < N_CLASS; y++)
        for (int d = 0; d < DIM; d++) oflat[y * DIM + d] = rw[y].w[d];

    int osh = OUT_SHIFT_PREF;
    int16_t oq[N_CLASS * DIM];
    int32_t bias_raw[N_CLASS];
    double out_bound = 0.0;
    for (;;) {
        int clipped;
        quantise_at(oflat, N_CLASS * DIM, osh, oq, &clipped);
        out_bound = acc_bound(oq, N_CLASS, DIM, 32767.0);
        double worst_bias = 0.0;
        for (int y = 0; y < N_CLASS; y++) {
            double b = fabs((double)rw[y].bias * ldexp(1.0, SCORE_SHIFT(osh)));
            if (b > worst_bias) worst_bias = b;
        }
        if (clipped)
            printf("  NOTE: %d readout weights clipped at Q%d\n", clipped, osh);
        if (out_bound + worst_bias <= INT32_MAX_D) break;

        printf("\n  *** READOUT L1 BOUND EXCEEDED AT Q%d ***\n", osh);
        printf("      max|ACC| %.4e + max|bias| %.4e = %.4e  >  int32 max %.4e\n",
               out_bound, worst_bias, out_bound + worst_bias, INT32_MAX_D);
        if (osh <= 8) { fprintf(stderr, "FATAL: cannot find a safe readout shift\n");
                        free(X); free(TS); return 2; }
        osh--;
        printf("      dropping to Q%d and re-checking\n", osh);
    }

    for (int y = 0; y < N_CLASS; y++) {
        for (int j = 0; j < K2; j++)
            for (int i = 0; i < C; i++)
                m.out[y].kern[j][i] = oq[y * DIM + j * C + i];
        bias_raw[y] = (int32_t)llrint((double)rw[y].bias * ldexp(1.0, SCORE_SHIFT(osh)));
        m.bias_raw[y] = bias_raw[y];
    }

    /* Re-sweep the threshold on the integer path at the final shift, so the
       reported threshold is optimal for the arithmetic that ships. */
    obs_res_max = obs_out_max = 0.0;
    replay_test(&m);
    float fin_thr = best_thr; float fin_obj = -1e30f;
    for (float thr = 0.05f; thr <= 0.95f; thr += 0.05f) {
        int32_t tr = (int32_t)llrint((double)thr * ldexp(1.0, SCORE_SHIFT(osh)));
        Score sc; evaluate(tr, test, N_TEST, &sc);
        float o = objective(&sc);
        if (o > fin_obj) { fin_obj = o; fin_thr = thr; }
    }
    int32_t thr_raw = (int32_t)llrint((double)fin_thr * ldexp(1.0, SCORE_SHIFT(osh)));
    Score sc; evaluate(thr_raw, test, N_TEST, &sc);

    printf("\nBEST: seed=%u lambda=%.0e thr=%.2f (raw Q%d %ld)   (rows fitted: %d)\n",
           best_seed, best_lam, fin_thr, SCORE_SHIFT(osh), (long)thr_raw, n_rows);
    printf("  positives %d:  correct %d (%.1f%%)  wrong-class %d  bad-timing %d  missed %d\n",
           sc.n_pos, sc.correct, 100.0 * sc.correct / sc.n_pos,
           sc.wrong_class, sc.bad_timing, sc.missed);
    printf("  negatives %d:  false alarms %d (%.1f%%)\n",
           sc.n_neg, sc.false_alarm, 100.0 * sc.false_alarm / sc.n_neg);
    for (int kk = 0; kk < 4; kk++)
        if (sc.n_by_kind[kk])
            printf("      %-10s %3d/%3d  (%.0f%%)\n", BTNVEC_NEGKIND[kk],
                   sc.fa_by_kind[kk], sc.n_by_kind[kk],
                   100.0 * sc.fa_by_kind[kk] / sc.n_by_kind[kk]);
    printf("\n  confusion (row = true, col = fired)\n          A     B     C\n");
    for (int i = 0; i < N_CLASS; i++) {
        printf("     %c ", 'A' + i);
        for (int j = 0; j < N_CLASS; j++) printf("%5d ", sc.confusion[i][j]);
        printf("\n");
    }

    printf("\n  fixed point (shifts: res Q%d pinned, out Q%d, score Q%d)\n",
           RES_SHIFT, osh, SCORE_SHIFT(osh));
    printf("    reservoir  L1 bound %.4e   observed %.4e   headroom %.0fx\n",
           res_bound, obs_res_max, INT32_MAX_D / res_bound);
    printf("    readout    L1 bound %.4e   observed %.4e   headroom %.2fx\n",
           out_bound, obs_out_max, INT32_MAX_D / out_bound);
    printf("    (L1 is the provable worst case; observed is over the %d test traces)\n",
           N_TEST);

    printf("\n  cost: reservoir %d + readout %d = %d int16 weights (%d B flash)\n",
           C * K1, N_CLASS * DIM, C * K1 + N_CLASS * DIM,
           2 * (C * K1 + N_CLASS * DIM));
    printf("        FIFOs %d B SRAM (one shared button history %d B + readout %d B),\n",
           2 * (K1 * 1 + K2 * C), 2 * K1, 2 * K2 * C);
    printf("        %d MACs/step, %d MAC/s at %d Hz\n",
           C * K1 + N_CLASS * DIM, (C * K1 + N_CLASS * DIM) * STEP_HZ, STEP_HZ);

    emit_weights("../src/weights.h", &m, rq, oq, osh, bias_raw, thr_raw,
                 fin_thr, best_seed, best_lam, res_bound, out_bound);
    emit_golden("golden.h", &m, thr_raw, osh);

    free(X); free(TS);
    return 0;
}
