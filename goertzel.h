#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <complex.h>

#ifndef GOERTZEL_H_
#define GOERTZEL_H_

int32_t goertzel(double detect_hz, double sample_rate_hz, float* buffer, int N);

#endif // GOERTZEL_H_

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif


#ifdef GOERTZEL_IMPLEMENTATION

#define Qn_FROM_DOUBLE(value, n) (lrint((value) * (1 << (n))))
#define DOUBLE_FROM_Qn(value, n) ((double)(value) / (1 << (n)))

/* The number closest to +1.0 that can be represented. */
#define ONE_Qn(n) ((1<<(n)) - 1)
/* One half (0.5). */
#define HALF_Qn(n) (1<<((n) - 1))

/* Drop n bits from x (shift right) while rounding (add one half). */
#define ROUND_OFF_Qn(x, n)                      \
    ((n > 0) ? (((x) + HALF_Qn(n)) >> n) : (x))

/* Multiply two Qn numbers, rounding to the precision of the first.
   Make sure to cast one of the arguments to the size needed to avoid
   overflow in the multiplication before shifting. */
#define MUL_Qn_Qn(x, y, xn, yn)                 \
    ROUND_OFF_Qn((x) * (y), (yn))

/* Add two Qn numbers, using the precision of the first. */
#define ADD_Qn_Qn(x, y, xn, yn)                 \
    ((xn) > (yn) ? (x) + ((y) << ((xn)-(yn))) : \
     (x) + ROUND_OFF_Qn((y), ((xn)-(yn))))

typedef struct {
    int16_t real;
    int16_t imag;
} cint16_t;

int32_t cint16_abs(cint16_t z) {
    /* Cheat for now and use floating point sqrt(). */
    return lrint(sqrt((double)z.real*(double)z.real +
                      (double)z.imag*(double)z.imag));
}


int32_t goertzel(double detect_hz, double sample_rate_hz, float* buffer, int N) {
    /* Notation from p. 710 of Lyons. */

    /* Index of DFT frequency bin to calculate. */
    const double m = (N * detect_hz) / sample_rate_hz;

    /* This complex feedforward coefficient allows a single zero to
       cancel one of the complex poles.  It can be calculated in
       advance. */
    const double complex dbl_coeff_ff = -cexp(-I*2*M_PI*m/N);

    const int coeff_ff_Qn = 15;  /* Q1.15 */
    cint16_t coeff_ff;
    coeff_ff.real = Qn_FROM_DOUBLE(creal(dbl_coeff_ff), coeff_ff_Qn);
    coeff_ff.imag = Qn_FROM_DOUBLE(cimag(dbl_coeff_ff), coeff_ff_Qn);

    /* Feedback coefficient. */
    double dbl_coeff_fb = 2*cos(2*M_PI*m/N);
    const int coeff_fb_Qn = 14;  /* Q2.14 */
    int16_t coeff_fb = Qn_FROM_DOUBLE(dbl_coeff_fb, coeff_fb_Qn);

    const int w_Qn = 15;
    int32_t w[3] = {0};         /* Delay line. Q17.15. */

    int sample_index = 0;

    /* Read and process N+1 samples. */
    while (1) {
        const int x_Qn = 15;  /* Q1.15. */
        int16_t x = (int16_t)(buffer[sample_index] * ONE_Qn(x_Qn));;

        if (sample_index == N) {
            /* The final x input sample should be forced to zero. */
            x = 0;
        }

        /* Manually shift delay line and calculate next value. */
        w[2] = w[1];
        w[1] = w[0];

        /* w[0] = x + (coeff_fb * w[1]) - w[2] */
        w[0] = MUL_Qn_Qn((int64_t)w[1], (int64_t)coeff_fb, w_Qn, coeff_fb_Qn);
        w[0] = ADD_Qn_Qn(w[0], x, w_Qn, x_Qn);
        w[0] = ADD_Qn_Qn(w[0], -w[2], w_Qn, w_Qn);

        if (sample_index++ == N) {
            /* End of Goertzel alogorithm for this buffer. Apply the
             * feedforward coefficient to generate final output. */
            const int y_Qn = 5;
            cint16_t y; /* y = w[0] + coeff_ff * w[1];  complex multiply. */
            y.real = ROUND_OFF_Qn(w[0] +
                                  MUL_Qn_Qn((int64_t)coeff_ff.real, w[1],
                                            coeff_ff_Qn, w_Qn), w_Qn - y_Qn);
            y.imag = ROUND_OFF_Qn(
                MUL_Qn_Qn((int64_t)coeff_ff.imag, w[1],
                          coeff_ff_Qn, w_Qn), w_Qn - y_Qn);

            int32_t dft_mag = cint16_abs(y);

            return dft_mag;
        }
    }
}

#endif
