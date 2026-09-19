// ^
// Property of Luka Vukmirica - All Rights Reserved. (luka.vukmirica@gmail.com)
#ifndef LINREG_H
#define LINREG_H

/* Max design-matrix width a fit can handle. Raised from 64 to 96 (2026-09-10)
   because the button demo's readout is C=3 channels x k2=32 taps = 96 features.
   Overridable from the build so a host tool can go wider without editing this
   header. Host-side only -- ridgefit never runs on the target. */
#ifndef LR_MAX_DIM
#define LR_MAX_DIM 96
#endif

typedef struct {
    int dim;
    float w[LR_MAX_DIM];
    float bias;
} ReadoutWeights;

/* X: flat row-major T x dim design matrix (X[t*dim + j]); y: length-T target.
   Fits w, bias to minimize ||X w + bias - y||^2 + lambda*||w||^2 via
   mean-centered ridge regression (Gaussian elimination w/ partial pivot on
   the dim x dim normal equations). Returns 0 on success, <0 on failure
   (dim too big, T <= dim, or a singular/ill-conditioned system). */
int ridgefit(float *X, float *y, int T, int dim, float lambda, ReadoutWeights *out);
float ridgepredict(ReadoutWeights *rw, float *x);

#endif
