#include <pthread.h>
#include <stdio.h>
#include "raylib.h"
#include <math.h>
#include "circbuf.h"
#include <unistd.h>
#include <complex.h>   // C99 complex for FFT
#include <unistd.h>
#include <string.h>
#include <time.h>

#define BUFF_SIZE 800
#define FFT_SIZE 256

CIRCBUF_DEF(float, buff, BUFF_SIZE);

const float target_freq = 5.0f;
const float consumer_sampling_frequency = 60.0f;
const float producer_sampling_frequency = 1000.0f;

const int screenWidth = 800;
const int screenHeight = 800;

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

// create the function to be executed as a thread
void *producer_func(void *ptr)
{
    (void)ptr;

    float current_angle = 0.0f;
    float y_pos = (float) screenHeight/2;
    // useconds_t delay = (useconds_t)(1000000.0f / producer_sampling_frequency);

    // 1. Calculate the total delay in nanoseconds (1 billion ns = 1 second)
    long delay_ns = (long)(1000000000.0f / producer_sampling_frequency);

    // 2. Set up the timespec struct
    struct timespec ts;
    ts.tv_sec = delay_ns / 1000000000;  // Extract whole seconds (if any)
    ts.tv_nsec = delay_ns % 1000000000; // Extract the remaining nanoseconds



    while(1){
        // Formula: (2 * PI * freq) / sampling_rate
        float delta = (2.0f * PI * target_freq) / producer_sampling_frequency;
        current_angle += delta;
        if (current_angle > 2.0f * PI) current_angle -= 2.0f * PI;

        // Map sin value (-1 to 1) to screen coordinates
        // Center of screen is (400, 225). We multiply by 100 to make the movement visible.
        y_pos = 225 + (sinf(current_angle) * 100);

        if (CIRCBUF_PUSH(buff, &y_pos)) {
            printf("Out of space in CB\n");
            // do not error out, just skip the value
        }
        // usleep(delay);
        nanosleep(&ts, NULL);

    }

    return NULL;
}


// int main(int argc, char **argv)
// {
//     InitWindow(screenWidth, screenHeight, "Function Drawing");
//     SetTargetFPS(consumer_sampling_frequency);

//     // start producer thread
//     pthread_t producer;
//     pthread_create(&producer, NULL, *producer_func, (void *) NULL);

//     // Persist these across frames
//     float last_y = screenHeight / 2.0f;
//     int current_x = 0;

//     static float fft_input[FFT_SIZE];
//     int fft_head = 0;           // next write position
//     int fft_samples_ready = 0;  // how many valid samples we have so far

//     static float magnitude[FFT_SIZE / 2];
//     memset(magnitude, 0, sizeof magnitude);

//     while (!WindowShouldClose()) {
//         float y = 0.0f;
//         int debug = 0;
//         BeginDrawing();
//             DrawText(TextFormat("Frequency: %.1f Hz", target_freq), 20, 20, 20, WHITE);
//             DrawText("Use UP/DOWN arrows to change frequency", 20, 50, 15, WHITE);

//             while (CIRCBUF_POP(buff, &y) == 0) {
//                 float sample = (y - 225.0f) / 100.0f;
//                 fft_input[fft_head % FFT_SIZE] = sample;
//                 fft_head++;
//                 if (fft_samples_ready < FFT_SIZE) fft_samples_ready++;

//                 if(current_x > screenWidth){
//                     ClearBackground(BLACK);
//                     current_x=0;
//                 }
//                 int x = current_x % screenWidth;
//                 current_x++;
//                 debug++;
//                 DrawCircle(x, y, 3, RED);
//             }

//             if(fft_samples_ready >= FFT_SIZE){
//                 static float complex fft_buf[FFT_SIZE];

//                 /* Build windowed frame (oldest → newest) with Hann window */
//                 int start = fft_head; /* oldest sample index */
//                 for (int k = 0; k < FFT_SIZE; k++) {
//                     float hann = 0.5f * (1.0f - cosf(2.0f * PI * k / (FFT_SIZE - 1)));
//                     float s = fft_input[(start + k) % FFT_SIZE];
//                     fft_buf[k] = (float complex)(s * hann);
//                 }

//                 fft(fft_buf, FFT_SIZE);

//                 /* Magnitude in dB (floor at -80 dB) */
//                 float ref = 1.0f / FFT_SIZE;
//                 for (int k = 0; k < FFT_SIZE / 2; k++) {
//                     float mag = cabsf(fft_buf[k]) * ref * 2.0f; /* *2 for one-sided */
//                     float db  = (mag > 1e-10f) ? 20.0f * log10f(mag) : -80.0f;
//                     /* Smooth over frames so bars don't flicker too badly */
//                     magnitude[k] = magnitude[k] * 0.7f + db * 0.3f;
//                 }
//             }

            // int spectrum_h = 300;
            // int waveH = 500;

            // DrawLine(0, waveH, screenWidth, waveH, DARKGRAY);   /* separator */


            // /* Spectrum */
            // {
            //     const int bins     = FFT_SIZE / 2;
            //     const float barW   = (float)screenWidth / bins;
            //     const float db_min = -80.0f;
            //     const float db_max =   0.0f;
            //     const float freq_res = producer_sampling_frequency / FFT_SIZE; /* Hz per bin */

            //     for (int k = 0; k < bins; k++) {
            //         float db     = magnitude[k];
            //         float norm   = (db - db_min) / (db_max - db_min);   /* 0..1 */
            //         if (norm < 0.0f) norm = 0.0f;
            //         if (norm > 1.0f) norm = 1.0f;

            //         int barH = (int)(norm * spectrum_h);
            //         int x    = (int)(k * barW);
            //         int y0   = screenHeight - barH;

            //         /* Colour: cyan → yellow based on intensity */
            //         Color col = ColorFromHSV(180.0f - norm * 60.0f, 1.0f, 0.9f);
            //         DrawRectangle(x, y0, (int)barW > 1 ? (int)barW - 1 : 1, barH, col);
            //     }

            //     /* Frequency axis labels */
            //     for (int hz = 0; hz <= (int)(producer_sampling_frequency / 2); hz += 50) {
            //         int x = (int)((hz / freq_res) * barW);
            //         if (x >= screenWidth) break;
            //         DrawLine(x, waveH, x, screenHeight, DARKGRAY);
            //         DrawText(TextFormat("%d", hz), x + 2, waveH + 4, 10, GRAY);
            //     }

            //     DrawText("Spectrum (Hz)", 20, waveH + 4, 14, WHITE);
            // }
//         EndDrawing();
//         printf("consumed: %d values\n", debug);
//     }

//     // detach thread, OS will clean it up
//     pthread_detach(producer);

//     CloseWindow();
//     return 0;
// }

int main(int argc, char **argv)
{
    InitWindow(screenWidth, screenHeight, "FFT + Oscilloscope");
    SetTargetFPS(consumer_sampling_frequency);

    pthread_t producer;
    pthread_create(&producer, NULL, producer_func, NULL);

    float last_y = 225.0f;
    int current_x = 0;

    static float fft_input[FFT_SIZE] = { 0 };
    int fft_head = 0;
    int fft_samples_ready = 0;
    static float magnitude[FFT_SIZE / 2] = { 0 };

    // Clear once at the very start
    BeginDrawing(); ClearBackground(BLACK); EndDrawing();

    while (!WindowShouldClose()) {
        float y = 0.0f;

        BeginDrawing();
            // 1. UI AREA (Fixed at top)
            DrawRectangle(0, 0, screenWidth, 80, BLACK);
            DrawText(TextFormat("Frequency: %.1f Hz", target_freq), 20, 20, 20, WHITE);

            // 2. CONSUMER & OSCILLOSCOPE
            while (CIRCBUF_POP(buff, &y) == 0) {
                // Prepare FFT input
                float sample = (y - 225.0f) / 100.0f;
                fft_input[fft_head % FFT_SIZE] = sample;
                fft_head++;
                if (fft_samples_ready < FFT_SIZE) fft_samples_ready++;

                // Drawing logic
                if (current_x >= screenWidth) {
                    current_x = 0;
                    // Wipe the wave area only, or clear all
                    ClearBackground(BLACK);
                }

                // Draw line for smooth wave
                DrawLine(current_x - 1, (int)last_y, current_x, (int)y, RED);

                last_y = y;
                current_x++;
            }

            // 3. FFT CALCULATION (Once per frame)
            if (fft_samples_ready >= FFT_SIZE) {
                static float complex fft_buf[FFT_SIZE];
                int offset = fft_head % FFT_SIZE; // The "head" is the oldest data

                for (int k = 0; k < FFT_SIZE; k++) {
                    float hann = 0.5f * (1.0f - cosf(2.0f * PI * k / (FFT_SIZE - 1)));
                    // Read from oldest to newest
                    float s = fft_input[(offset + k) % FFT_SIZE];
                    fft_buf[k] = s * hann + 0 * I;
                }

                fft(fft_buf, FFT_SIZE);

                float ref = 1.0f / FFT_SIZE;
                for (int k = 0; k < FFT_SIZE / 2; k++) {
                    float mag = cabsf(fft_buf[k]) * ref * 2.0f;
                    float db  = (mag > 1e-10f) ? 20.0f * log10f(mag) : -80.0f;
                    magnitude[k] = magnitude[k] * 0.8f + db * 0.2f; // Smooth
                }
            }


            int spectrum_h = 300;
            int waveH = 500;
            DrawLine(0, waveH, screenWidth, waveH, DARKGRAY);
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

                    int barH = (int)(norm * spectrum_h);
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
}
