#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include "raylib.h"
#include <math.h>
#include <complex.h>   // C99 complex for FFT
#include <string.h>
#include "circbuf.h"
#include <unistd.h>
#include <time.h>
#include <stdatomic.h>
#include <stdlib.h>


#define BUFF_SIZE    4096
#define FFT_SIZE     128          // must be power-of-2; covers ~0.5 s at 1000 Hz
#define SPECTRUM_H   300          // pixel height of spectrum panel

CIRCBUF_DEF(float, buff, BUFF_SIZE);

atomic_bool is_transmitting = false;
const char* message = "Hello, World!";
float target_freq               = 0.1f;
const float consumer_sampling_frequency = 120.0f;
const float producer_sampling_frequency = 4000.0f;
const int   screenWidth               = 800;
const int   screenHeight              = 800;


void wait_nanosec(long delay_ns){
    struct timespec ts;
    ts.tv_sec = delay_ns / 1000000000;  // Extract whole seconds
    ts.tv_nsec = delay_ns % 1000000000; // Extract the remaining nanoseconds
    nanosleep(&ts, NULL);
}


/* ------------------------------------------------------------------ */
/*  Iterative Cooley-Tukey FFT (in-place, radix-2, DIT)               */
/* ------------------------------------------------------------------ */
static void fft(float complex *buf, int n){
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


// each char becomes 8 bits, so total = len * 8
void str_to_bits(const char *str, uint8_t *out) {
    int idx = 0;
    while (*str) {
        for (int i = 7; i >= 0; i--) {
            out[idx++] = (*str >> i) & 1;
        }
        str++;
    }
}

// Symbols mapping:
// 00 = symbols[0]
// 01 = symbols[1]
// 10 = symbols[2]
// 11 = symbols[3]
static const float symbols_lut[] = {
   100.0f, 300.0f, 500.0f, 700.0f
};

void fsk_symbols(const char* message, float* symbols) {
    size_t n_bits = strlen(message) * 8;

    uint8_t* bits = malloc(n_bits * sizeof(uint8_t));
    if (!bits) return;

    str_to_bits(message, bits);

    for (size_t i = 0; i < n_bits; i += 2) {
        uint8_t combined = (bits[i] << 1) | bits[i + 1];
        symbols[i / 2] = symbols_lut[combined];
    }

    free(bits);
}


float sample_sin(float target_freq, float* current_angle){
    float delta = (2.0f * PI * target_freq) / producer_sampling_frequency;
    *current_angle += delta;
    if (*current_angle > 2.0f * PI) *current_angle -= 2.0f * PI;
    return (225 + (sinf(*current_angle) * 100));
}

float sample_square(float target_freq, float* current_angle){
    float delta = (2.0f * PI * target_freq) / producer_sampling_frequency;
    *current_angle += delta;
    if (*current_angle > 2.0f * PI) *current_angle -= 2.0f * PI;
    float square = (*current_angle < PI) ? 1.0f : -1.0f;
    return (225 + (square * 100));
}

/* ------------------------------------------------------------------ */
/*  Producer thread                                                     */
/* ------------------------------------------------------------------ */
// #define SAMPLES_PER_SYMBOL 128
void *producer_func(void *ptr)
{
    (void)ptr;
    float current_angle = 0.0f;
    // 1. Calculate the total delay in nanoseconds (1 billion ns = 1 second)
    long delay_ns = (long)(1000000000.0f / producer_sampling_frequency);
    while (1) {
        if(atomic_load(&is_transmitting)){
            size_t n_symbols = strlen(message) * 4;
            float symbols[n_symbols];
            fsk_symbols(message, symbols);

            // modulate
            for (size_t i = 0; i < n_symbols; i ++) {
                for (int s = 0; s < FFT_SIZE; s++) {
                    float y_pos = sample_sin(symbols[i], &current_angle);
                    if (CIRCBUF_PUSH(buff, &y_pos)) {
                        printf("Out of space in CB\n");
                    }
                    wait_nanosec(delay_ns);
                }
            }
            atomic_store(&is_transmitting, false);
        }else{
            float y_pos = sample_sin(target_freq, &current_angle);
            if (CIRCBUF_PUSH(buff, &y_pos)){
                printf("Out of space in CB\n");
            }
            wait_nanosec(delay_ns);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* CONSUMER                                                     */
/* ------------------------------------------------------------------ */

int find_peak_bin(float* magnitude, int bins) {
    int peak = 1;
    for (int k = 2; k < bins; k++)
        if (magnitude[k] > magnitude[peak]) peak = k;
    return peak;
}

int bin_to_symbol(int bin) {
    float freq = bin * (producer_sampling_frequency / FFT_SIZE);
    float min_dist = fabsf(freq - symbols_lut[0]);
    int nearest = 0;
    for (int i = 1; i < 4; i++) {
        float dist = fabsf(freq - symbols_lut[i]);
        if (dist < min_dist) { min_dist = dist; nearest = i; }
    }
    return nearest;
}

float bin_to_freq(int bin) {
    return bin * (producer_sampling_frequency / FFT_SIZE);
}


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

        /* ---- TRANSMISSION TRIGGER -------------------------------- */
        if(IsKeyPressed(KEY_T)){
            bool current = atomic_load(&is_transmitting);
            atomic_store(&is_transmitting, !current);
        }

        float y = 0.0f;

        /* ---- CONSUME BUFFER -------------------------------- */
        while (CIRCBUF_POP(buff, &y) == 0) {
            float sample = (y - 225.0f) / 100.0f;

            fft_input[fft_head % FFT_SIZE] = sample;
            fft_head++;
            if (fft_samples_ready < FFT_SIZE) fft_samples_ready++;
        }

        /* ---- FFT COMPUTATION --------------- */
        if (fft_samples_ready >= FFT_SIZE) {
            static float complex fft_buf[FFT_SIZE];

            /* Build windowed frame (oldest → newest) with Hann window */
            int start = fft_head;
            for (int k = 0; k < FFT_SIZE; k++) {
                float hann = 0.5f * (1.0f - cosf(2.0f * PI * k / (FFT_SIZE - 1)));
                float s = fft_input[(start + k) % FFT_SIZE];
                fft_buf[k] = (float complex)(s * hann);
            }

            fft(fft_buf, FFT_SIZE);

            float ref = 1.0f / FFT_SIZE;
            for (int k = 0; k < FFT_SIZE / 2; k++) {
                float mag = cabsf(fft_buf[k]) * ref * 2.0f; /* *2 for one-sided */
                float db  = (mag > 1e-10f) ? 20.0f * log10f(mag) : -80.0f;
                magnitude[k] = magnitude[k] * 0.7f + db * 0.3f;
            }
        }

        /* ---- DEMODULATION ------------------------------------------------- */
        if (atomic_load(&is_transmitting) && fft_samples_ready >= FFT_SIZE) {
            int peak = find_peak_bin(magnitude, FFT_SIZE / 2);

            int symbol  = bin_to_symbol(peak);
            int freq = bin_to_freq(peak);

            printf("detected: freq-> %d; symbol-> %.2f; peak -> %d\n", freq, symbols_lut[symbol], peak);
        }
        else if(!atomic_load(&is_transmitting)){
            // reset parameters

        }

        /* ---- RENDERING ------------------------------------------------- */
        BeginDrawing();
        ClearBackground(BLACK);

        /* Waveform */
        DrawLine(0, waveH, screenWidth, waveH, DARKGRAY);
        DrawText(TextFormat("Frequency: %.1f Hz", target_freq), 20, 20, 20, WHITE);

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

        }

        EndDrawing();
    }

    pthread_detach(producer);
    CloseWindow();
    return 0;
}
