#include <pthread.h>
#include <stdio.h>
#include "raylib.h"
#include <math.h>
#include <complex.h>   // C99 complex for FFT
#include <string.h>
#include "circbuf.h"
#include <unistd.h>

#define BUFF_SIZE    4096
#define FFT_SIZE     256          // must be power-of-2; covers ~0.5 s at 1000 Hz
#define SPECTRUM_H   300          // pixel height of spectrum panel

CIRCBUF_DEF(float, buff, BUFF_SIZE);

float target_freq               = 10.0f;
const float consumer_sampling_frequency = 120.0f;
const float producer_sampling_frequency = 3000.0f;
const int   screenWidth               = 800;
const int   screenHeight              = 800;

/* ------------------------------------------------------------------ */
/*  Iterative Cooley-Tukey FFT (in-place, radix-2, DIT)               */
/* ------------------------------------------------------------------ */
static void fft(float complex *buf, int n)
{
    /* Bit-reversal permutation */
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float complex tmp = buf[i]; buf[i] = buf[j]; buf[j] = tmp; }
    }
    /* Butterfly stages */
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * PI / (float)len;
        float complex wlen = cosf(ang) + sinf(ang) * I;
        for (int i = 0; i < n; i += len) {
            float complex w = 1.0f;
            for (int k = 0; k < len / 2; k++) {
                float complex u = buf[i + k];
                float complex v = buf[i + k + len/2] * w;
                buf[i + k]           = u + v;
                buf[i + k + len/2]   = u - v;
                w *= wlen;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Producer thread                                                     */
/* ------------------------------------------------------------------ */
void *producer_func(void *ptr)
{
    (void)ptr;
    float current_angle = 0.0f;
    useconds_t delay = (useconds_t)(1000000.0f / producer_sampling_frequency);
    while (1) {
        float delta = (2.0f * PI * target_freq) / producer_sampling_frequency;
        current_angle += delta;
        if (current_angle > 2.0f * PI) current_angle -= 2.0f * PI;

        float y_pos = 225 + (sinf(current_angle) * 100);
        if (CIRCBUF_PUSH(buff, &y_pos))
            printf("Out of space in CB\n");

        usleep(delay);
    }
    // while (1) {
    //     float delta = (2.0f * PI * target_freq) / producer_sampling_frequency;
    //     current_angle += delta;
    //     if (current_angle > 2.0f * PI) current_angle -= 2.0f * PI;

    //     // No trig needed — just compare phase to PI
    //     float square = (current_angle < PI) ? 1.0f : -1.0f;
    //     float y_pos = 225 + (square * 100);

    //     if (CIRCBUF_PUSH(buff, &y_pos))
    //         printf("Out of space in CB\n");
    //     usleep(delay);
    // }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  main / consumer                                                     */
/* ------------------------------------------------------------------ */
int main(void)
{
    /* Waveform panel: top 500 px  |  Spectrum panel: bottom 300 px */
    const int waveH = screenHeight - SPECTRUM_H;

    InitWindow(screenWidth, screenHeight, "Waveform + FFT Spectrum");
    SetTargetFPS((int)consumer_sampling_frequency);

    pthread_t producer;
    pthread_create(&producer, NULL, producer_func, NULL);

    /* Ring buffer for FFT accumulation */
    static float fft_input[FFT_SIZE];
    int fft_head = 0;           // next write position
    int fft_samples_ready = 0;  // how many valid samples we have so far

    /* Magnitude spectrum (only positive frequencies: FFT_SIZE/2 bins) */
    static float magnitude[FFT_SIZE / 2];
    memset(magnitude, 0, sizeof magnitude);

    int current_x = 0;

    while (!WindowShouldClose()) {
        if (IsKeyDown(KEY_UP)) target_freq += 0.1f;
        if (IsKeyDown(KEY_DOWN)) target_freq -= 0.1f;
        if (target_freq < 0.1f) target_freq = 0.1f;

        float y = 0.0f;

        /* ---- drain circular buffer -------------------------------- */
        while (CIRCBUF_POP(buff, &y) == 0) {
            /* Normalise the pixel value back to [-1, 1] for the FFT.
               y was produced as:  225 + sin()*100  → sin = (y-225)/100  */
            float sample = (y - 225.0f) / 100.0f;

            fft_input[fft_head % FFT_SIZE] = sample;
            fft_head++;
            if (fft_samples_ready < FFT_SIZE) fft_samples_ready++;
        }

        /* ---- compute FFT once we have a full window --------------- */
        if (fft_samples_ready >= FFT_SIZE) {
            static float complex fft_buf[FFT_SIZE];

            /* Build windowed frame (oldest → newest) with Hann window */
            int start = fft_head; /* oldest sample index */
            for (int k = 0; k < FFT_SIZE; k++) {
                float hann = 0.5f * (1.0f - cosf(2.0f * PI * k / (FFT_SIZE - 1)));
                float s = fft_input[(start + k) % FFT_SIZE];
                fft_buf[k] = (float complex)(s * hann);
            }

            fft(fft_buf, FFT_SIZE);

            /* Magnitude in dB (floor at -80 dB) */
            float ref = 1.0f / FFT_SIZE;
            for (int k = 0; k < FFT_SIZE / 2; k++) {
                float mag = cabsf(fft_buf[k]) * ref * 2.0f; /* *2 for one-sided */
                float db  = (mag > 1e-10f) ? 20.0f * log10f(mag) : -80.0f;
                /* Smooth over frames so bars don't flicker too badly */
                magnitude[k] = magnitude[k] * 0.7f + db * 0.3f;
            }
        }

        /* ---- draw ------------------------------------------------- */
        BeginDrawing();
        ClearBackground(BLACK);

        /* Waveform */
        DrawLine(0, waveH, screenWidth, waveH, DARKGRAY);   /* separator */
        DrawText(TextFormat("Frequency: %.1f Hz", target_freq), 20, 20, 20, WHITE);

        // if (CIRCBUF_POP(buff, &y) != 0) y = (float)waveH / 2;  /* fallback */
        // /* Re-drain again for drawing (already drained above; this is
        //    just for the waveform dots – swap draw order if you prefer) */
        // {
        //     /* Quick second pass: re-pop any new samples that arrived
        //        between our drain and BeginDrawing (tiny window, usually 0) */
        // }
        // /* We draw the last known positions from what we popped earlier.
        //    For clarity we just re-read the ring buffer in-order for display. */
        // for (int k = 0; k < FFT_SIZE && k < screenWidth; k++) {
        //     int idx = (fft_head - FFT_SIZE + k + FFT_SIZE * 2) % FFT_SIZE;
        //     float sv = fft_input[idx] * 100.0f + (float)waveH / 2;
        //     DrawPixel(k, (int)sv, RED);
        // }

        float x_step = (float)screenWidth / (FFT_SIZE - 1);

                for (int k = 1; k < FFT_SIZE; k++) {
                    // Get previous point
                    int idx_prev = (fft_head + k - 1) % FFT_SIZE;
                    float val_prev = fft_input[idx_prev] * 100.0f + (float)waveH / 2;

                    // Get current point
                    int idx_curr = (fft_head + k) % FFT_SIZE;
                    float val_curr = fft_input[idx_curr] * 100.0f + (float)waveH / 2;

                    // Draw line connecting them, stretched across the screen
                    DrawLineV(
                        (Vector2){ (k - 1) * x_step, val_prev },
                        (Vector2){ k * x_step,       val_curr },
                        RED
                    );
                }

        /* Spectrum */
        {
            const int bins     = FFT_SIZE / 2;
            const float barW   = (float)screenWidth / bins;
            const float db_min = -80.0f;
            const float db_max =   0.0f;
            const float freq_res = producer_sampling_frequency / FFT_SIZE; /* Hz per bin */

            for (int k = 0; k < bins; k++) {
                float db     = magnitude[k];
                float norm   = (db - db_min) / (db_max - db_min);   /* 0..1 */
                if (norm < 0.0f) norm = 0.0f;
                if (norm > 1.0f) norm = 1.0f;

                int barH = (int)(norm * SPECTRUM_H);
                int x    = (int)(k * barW);
                int y0   = screenHeight - barH;

                /* Colour: cyan → yellow based on intensity */
                Color col = ColorFromHSV(180.0f - norm * 60.0f, 1.0f, 0.9f);
                DrawRectangle(x, y0, (int)barW > 1 ? (int)barW - 1 : 1, barH, col);
            }

            /* Frequency axis labels */
            for (int hz = 0; hz <= (int)(producer_sampling_frequency / 2); hz += 50) {
                int x = (int)((hz / freq_res) * barW);
                if (x >= screenWidth) break;
                DrawLine(x, waveH, x, screenHeight, DARKGRAY);
                DrawText(TextFormat("%d", hz), x + 2, waveH + 4, 10, GRAY);
            }

            DrawText("Spectrum (Hz)", 20, waveH + 4, 14, WHITE);
        }

        EndDrawing();
    }

    pthread_detach(producer);
    CloseWindow();
    return 0;
}
