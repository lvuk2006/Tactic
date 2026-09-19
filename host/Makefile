# Host tools for the Tactic demo. Nothing here ships to the target.
#
#   make            build btnfit
#   ./btnfit        full sweep, writes ../src/weights.h and golden.h
#   ./btnfit --quick one seed, one lambda -- for checking a change compiles and runs
#
# LR_MAX_DIM must cover the readout width: C * K2 = 12 * 32 = 384.
# The reservoir/readout arithmetic comes from ../src/readoutconv.c, compiled in
# here, so the fitter and the firmware cannot drift apart.

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter
DEFS     = -DLR_MAX_DIM=384
SRC      = btnfit.c btnvec.c linreg.c ../src/readoutconv.c
LDLIBS   = -lm

btnfit: $(SRC) btnvec.h linreg.h ../src/readoutconv.h ../src/tanh_table.h
	$(CC) $(CFLAGS) $(DEFS) -o $@ $(SRC) $(LDLIBS)

# The tanh table generator is the author's. Regenerating the table invalidates
# weights.h, because the fitted weights depend on the LUT and its index shift.
tanh_lut_gen: tanh_lut_gen.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f btnfit tanh_lut_gen

.PHONY: clean
