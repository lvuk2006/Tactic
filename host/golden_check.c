// ^
// Property of Luka Vukmirica - All Rights Reserved. (luka.vukmirica@gmail.com)
/* golden_check.c -- does the firmware's model reproduce the host's scores?
 *
 * Compiles the firmware's own ../src/pipeline.c (plus readoutconv.c and
 * tanh_lut.c) on the computer, replays every golden trace from golden.h
 * through it, and compares each of the 3 class scores at every step with the
 * scores btnfit recorded. Any difference at all is a bug: both sides run the
 * same integer arithmetic.
 *
 *   make check        build and run; exit status 0 means bit-identical
 *
 * Scores only. Decisions (argmax, threshold, LEDs) live in main.c with the
 * hardware and are not checked here.
 */
#include <stdio.h>
#include <stdint.h>
#include "pipeline.h"
#include "golden.h"

int main(void){
    if (pipeline_init() != 0){
        printf("FAIL: pipeline_init rejected the sizes in weights.h\n");
        return 1;
    }

    long total = 0, bad = 0;
    for (int t = 0; t < GOLDEN_N; t++){
        pipeline_reset();                       /* each trace starts from power-on */
        int trace_bad = 0;
        for (int s = 0; s < GOLDEN_STEPS; s++){
            int32_t score[GOLDEN_CLASSES];
            pipeline_step(GOLDEN[t].btn[s], score);
            for (int y = 0; y < GOLDEN_CLASSES; y++){
                int32_t want = GOLDEN[t].score[s * GOLDEN_CLASSES + y];
                total++;
                if (score[y] != want){
                    if (trace_bad == 0)         /* report the first one per trace */
                        printf("  trace %d: first mismatch at step %d class %d: got %ld, want %ld\n",
                               t, s, y, (long)score[y], (long)want);
                    trace_bad++;
                }
            }
        }
        bad += trace_bad;
        printf("trace %d: %3d/%d scores differ\n", t, trace_bad, GOLDEN_STEPS * GOLDEN_CLASSES);
    }

    if (bad == 0){
        printf("PASS: all %ld scores bit-identical to btnfit\n", total);
        return 0;
    }
    printf("FAIL: %ld of %ld scores differ\n", bad, total);
    return 1;
}
