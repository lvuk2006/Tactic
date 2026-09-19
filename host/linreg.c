// ^
// Property of Luka Vukmirica - All Rights Reserved. (luka.vukmirica@gmail.com)
#include <math.h>
#include "linreg.h"

int ridgefit(float *X, float *y, int T, int dim, float lambda, ReadoutWeights *out) {
    if (dim > LR_MAX_DIM || dim <= 0 || T <= dim) return -1;

    double xmean[LR_MAX_DIM] = {0};
    double ymean = 0.0;
    for (int t = 0; t < T; t++) {
        for (int j = 0; j < dim; j++) xmean[j] += X[t*dim + j];
        ymean += y[t];
    }
    for (int j = 0; j < dim; j++) xmean[j] /= T;
    ymean /= T;

    static double A[LR_MAX_DIM][LR_MAX_DIM];
    double b[LR_MAX_DIM] = {0};
    for (int i = 0; i < dim; i++)
        for (int j = 0; j < dim; j++) A[i][j] = 0.0;

    double xc[LR_MAX_DIM];
    for (int t = 0; t < T; t++) {
        for (int j = 0; j < dim; j++) xc[j] = X[t*dim + j] - xmean[j];
        double yc = y[t] - ymean;
        for (int i = 0; i < dim; i++) {
            b[i] += xc[i] * yc;
            for (int j = 0; j < dim; j++) A[i][j] += xc[i] * xc[j];
        }
    }
    for (int i = 0; i < dim; i++) A[i][i] += lambda;

    /* Gaussian elimination with partial pivoting on [A|b]. */
    for (int col = 0; col < dim; col++) {
        int piv = col;
        double best = fabs(A[col][col]);
        for (int r = col + 1; r < dim; r++) {
            if (fabs(A[r][col]) > best) { best = fabs(A[r][col]); piv = r; }
        }
        if (best < 1e-12) return -2;
        if (piv != col) {
            for (int c = 0; c < dim; c++) {
                double tmp = A[col][c]; A[col][c] = A[piv][c]; A[piv][c] = tmp;
            }
            double tmp = b[col]; b[col] = b[piv]; b[piv] = tmp;
        }
        for (int r = col + 1; r < dim; r++) {
            double f = A[r][col] / A[col][col];
            for (int c = col; c < dim; c++) A[r][c] -= f * A[col][c];
            b[r] -= f * b[col];
        }
    }

    double w[LR_MAX_DIM];
    for (int r = dim - 1; r >= 0; r--) {
        double s = b[r];
        for (int c = r + 1; c < dim; c++) s -= A[r][c] * w[c];
        w[r] = s / A[r][r];
    }

    double bias = ymean;
    for (int j = 0; j < dim; j++) {
        out->w[j] = (float)w[j];
        bias -= w[j] * xmean[j];
    }
    out->dim = dim;
    out->bias = (float)bias;
    return 0;
}

float ridgepredict(ReadoutWeights *rw, float *x) {
    float acc = rw->bias;
    for (int j = 0; j < rw->dim; j++) acc += rw->w[j] * x[j];
    return acc;
}
