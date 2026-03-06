#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <math.h>
#include <complex.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <inttypes.h>

#include "raylib.h"
#include "lib/circbuf.h"
#define GOERTZEL_IMPLEMENTATION
#include "lib/goertzel.h"

#define PROD_CONS_BUFF_SIZE    8092
#define PROD_UI_BUFF_SIZE    8092
#define CONS_UI_BUFF_SIZE    8092

#define FFT_SIZE     16          // must be power-of-2; covers ~0.5 s at 1000 Hz
#define SPECTRUM_H   300          // pixel height of spectrum panel

typedef struct {
    // --- Source identification ---
    enum { TELEM_PRODUCER, TELEM_CONSUMER } source;
    uint64_t timestamp_ns;          // clock_gettime monotonic, for ordering

    // --- Transmission state ---
    bool    is_transmitting;
    int     symbol_index;
    int     total_symbols;

    // --- PRODUCER fields ---
    float   prod_value;
    float   current_tone_hz;        // tone being sent right now
    float   current_angle;          // phase health [0, 2PI]
    double  cumulative_drift_ms;    // nanosleep drift, most important producer field
    int     prod_cons_overflows;
    uint64_t sample_time_ns;

    // --- CONSUMER fields ---
    float   cons_value;
    float   goertzel_energies[4];   // most important consumer field
    int     winning_margin;
    int     symbol_decoded;
    int     bits_in;
    int     bytes_decoded;
    uint8_t last_byte;
    int     fft_drops;

    // --- Shared buffer health ---
    int     prod_cons_fill;
    int     prod_ui_fill;
    int     cons_ui_fill;
} Packet;

CIRCBUF_DEF(float, prod_cons_buff, PROD_CONS_BUFF_SIZE);
CIRCBUF_DEF(Packet, prod_ui_buff, PROD_UI_BUFF_SIZE);
CIRCBUF_DEF(float, cons_ui_buff, CONS_UI_BUFF_SIZE);


atomic_bool is_transmitting = false;
const char* message = "Hello, MA STAR!";
float target_freq               = 0.1f;
const float consumer_sampling_frequency = 100000.0f;
const float producer_sampling_frequency = 100000.0f;
const float ui_sampling_frequency = 60.0f;
const int   screenWidth               = 1000;
const int   screenHeight              = 800;

void packet_print(const Packet* p) {
    printf("[%s | t=%" PRIu64 "ns | tx=%s]\n",
        p->source == TELEM_PRODUCER ? "PROD" : "CONS",
        p->timestamp_ns,
        p->is_transmitting ? "ON" : "OFF");

    if (p->source == TELEM_PRODUCER) {
        printf("  value:           %.2f Hz\n",   p->prod_value);
        printf("  overflows:       cons=%d\n",
            p->prod_cons_overflows);
    } else {
        printf("  goertzel:        [0]=%.1f [1]=%.1f [2]=%.1f [3]=%.1f\n",
            p->goertzel_energies[0], p->goertzel_energies[1],
            p->goertzel_energies[2], p->goertzel_energies[3]);
        printf("  decoded sym:     %d  (margin=%d)\n",
            p->symbol_decoded, p->winning_margin);
        printf("  bits_in:         %d/8\n",       p->bits_in);
        printf("  bytes decoded:   %d  last=0x%02X '%c'\n",
            p->bytes_decoded,
            p->last_byte,
            (p->last_byte >= 32 && p->last_byte < 127) ? p->last_byte : '.');
        printf("  fft drops:       %d\n",          p->fft_drops);
    }

    printf("  buf fill:        prod_cons=%d  prod_ui=%d  cons_ui=%d\n",
        p->prod_cons_fill, p->prod_ui_fill, p->cons_ui_fill);
}

// GLOBAL
void wait_nanosec(long delay_ns){
    struct timespec ts;
    ts.tv_sec = delay_ns / 1000000000;  // Extract whole seconds
    ts.tv_nsec = delay_ns % 1000000000; // Extract the remaining nanoseconds
    nanosleep(&ts, NULL);
}

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

// A tiny struct to hold our starting snapshot
typedef struct {
    uint64_t start;
} Stopwatch;
static inline Stopwatch timer_start(void) {
    Stopwatch sw = { .start = now_ns() };
    return sw;
}
static inline uint64_t timer_elapsed_ns(Stopwatch sw) {
    return now_ns() - sw.start;
}
static inline double timer_elapsed_ms(Stopwatch sw) {
    return (double)(now_ns() - sw.start) / 1000000.0;
}

// PRODUCER
void str_to_bits(const char *str, uint8_t *out) {
    int idx = 0;
    while (*str) {
        for (int i = 7; i >= 0; i--) {
            out[idx++] = (*str >> i) & 1;
        }
        str++;
    }
}

// PRODUCER
// Symbols mapping:
// 00 = symbols[0]
// 01 = symbols[1]
// 10 = symbols[2]
// 11 = symbols[3]
static const float symbols_lut[] = {
   36000.0f, 38000.0f, 40000.0f, 42000.0f
};

// PRODUCER
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
// PRODUCER
float sample_sin(float target_freq, float* current_angle){
    float delta = (2.0f * PI * target_freq) / producer_sampling_frequency;
    *current_angle += delta;
    if (*current_angle > 2.0f * PI) *current_angle -= 2.0f * PI;
    return (225 + (sinf(*current_angle) * 100));
}
// PRODUCER
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
void *producer_func(void *ptr)
{
    (void)ptr;
    float current_angle = 0.0f;
    // 1. Calculate the total delay in nanoseconds (1 billion ns = 1 second)
    long delay_ns = (long)(1000000000.0f / producer_sampling_frequency);

    uint64_t baseline_time_ns = now_ns();
    uint64_t total_samples = 0;
    while (1) {
        if(atomic_load(&is_transmitting)){

            size_t n_symbols = strlen(message) * 4;
            float symbols[n_symbols];
            fsk_symbols(message, symbols);

            // modulate
            for (size_t i = 0; i < n_symbols; i ++) {
                for (int s = 0; s < FFT_SIZE; s++) {
                    // Stopwatch compute_timer = timer_start();
                    float y_pos = sample_sin(symbols[i], &current_angle);
                    // uint64_t sample_time_ns = timer_elapsed_ns(compute_timer);

                    total_samples++;

                    uint64_t actual_time_ns = now_ns();
                    uint64_t ideal_time_ns = baseline_time_ns + (total_samples * delay_ns);

                    double drift_ms = (double)((long long)actual_time_ns - (long long)ideal_time_ns) / 1000000.0;

                    Packet t = {0};
                    t.source = TELEM_PRODUCER;
                    t.timestamp_ns = now_ns();
                    t.is_transmitting = true;
                    t.prod_value = y_pos;
                    t.current_tone_hz = symbols[i];
                    t.total_symbols=n_symbols;
                    t.symbol_index = i;
                    t.current_angle = current_angle;
                    t.prod_cons_fill = CIRCBUF_FS(prod_cons_buff);
                    t.prod_ui_fill = CIRCBUF_FS(prod_ui_buff);
                    t.cons_ui_fill = CIRCBUF_FS(cons_ui_buff);
                    t.cumulative_drift_ms = drift_ms;
                    // t.sample_time_ns = sample_time_ns;

                    if (CIRCBUF_PUSH(prod_cons_buff, &y_pos)) {
                        printf("Out of space in CB\n");
                        t.prod_cons_overflows++;
                    }
                    if (CIRCBUF_PUSH(prod_ui_buff, &t)) {
                        printf("Out of space in CB\n");
                    }
                    wait_nanosec(delay_ns);
                }
            }
            atomic_store(&is_transmitting, false);
        }else{
            // Stopwatch compute_timer = timer_start();
            float y_pos = sample_sin(target_freq, &current_angle);
            // uint64_t math_time_ns = timer_elapsed_ns(compute_timer);

            total_samples++;
            uint64_t actual_time_ns = now_ns();
            uint64_t ideal_time_ns = baseline_time_ns + (total_samples * delay_ns);
            double drift_ms = (double)((long long)actual_time_ns - (long long)ideal_time_ns) / 1000000.0;

            Packet t = {0};
            t.source = TELEM_PRODUCER;
            t.timestamp_ns = now_ns();
            t.is_transmitting = false;
            t.prod_value = y_pos;
            t.prod_cons_fill = CIRCBUF_FS(prod_cons_buff);
            t.prod_ui_fill = CIRCBUF_FS(prod_ui_buff);
            t.cons_ui_fill = CIRCBUF_FS(cons_ui_buff);
            t.cumulative_drift_ms = drift_ms;
            // t.sample_time_ns = math_time_ns;

            if (CIRCBUF_PUSH(prod_cons_buff, &y_pos)){
                printf("Out of space in CB\n");
            }
            if (CIRCBUF_PUSH(prod_ui_buff, &t)) {
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

// CONSUMER
static float sample_buff[FFT_SIZE];
int sample_head = 0;

// CONSUMER
void fft_push(float value){
    if(sample_head < FFT_SIZE){
        sample_buff[sample_head++] = value;
    }else{
        // TODO: deal with "dropped frames"
    }

}
// CONSUMER
int fft_ready(){
    if(sample_head == FFT_SIZE){
        return 1;
    }else{
        return 0;
    }
}
// CONSUMER
void fft_flush(){
    sample_head = 0;
}
// CONSUMER
int demod_goertzel(float* fft, int N) {
    int32_t best_mag = -1;
    int best_sym = 0;
    for (int i = 0; i < 4; i++) {
        int32_t mag = goertzel(symbols_lut[i], producer_sampling_frequency, fft, N);
        if (mag > best_mag) {
            best_mag = mag;
            best_sym = i;
        }
    }
    return best_sym;
}
// CONSUMER
typedef struct {
    uint8_t* bytes;      // decoded bytes so far
    size_t   byte_count;
    int      bit_buf;    // current byte being built
    int      bits_in;    // how many bits written into bit_buf (0-7)
    size_t   capacity;
} BitAccum;

// CONSUMER
void bitaccum_init(BitAccum* a, size_t initial_capacity) {
    a->bytes     = malloc(initial_capacity);
    a->capacity  = initial_capacity;
    a->byte_count = 0;
    a->bit_buf   = 0;
    a->bits_in   = 0;
}
// CONSUMER
void bitaccum_push(BitAccum* a, int bit) {
    a->bit_buf = (a->bit_buf << 1) | (bit & 1);
    a->bits_in++;
    if (a->bits_in == 8) {
        // grow if needed
        if (a->byte_count >= a->capacity) {
            a->capacity *= 2;
            uint8_t* temp = realloc(a->bytes, a->capacity);
            if (!temp) {
                printf("Out of memory!\n");
                // Handle error (e.g., free(a->bytes), exit)
            }
            a->bytes = temp;
        }
        a->bytes[a->byte_count++] = (uint8_t)a->bit_buf;
        a->bit_buf = 0;
        a->bits_in = 0;
    }
}

// CONSUMER
void bitaccum_reset(BitAccum* a) {
    a->byte_count = 0;
    a->bit_buf    = 0;
    a->bits_in    = 0;
}

// CONSUMER
void bitaccum_free(BitAccum* a) {
    free(a->bytes);
}

// CONSUMER
void *consumer_func(void *ptr){
    (void)ptr;

    float y = 0.0f;
    static bool prev_transmitting = false;
    static struct timespec tx_start;

    BitAccum accum;
    bitaccum_init(&accum, 64);

    // FFT Result - Only positive
    static float magnitude[FFT_SIZE / 2];
    memset(magnitude, 0, sizeof magnitude);

    long delay_ns = (long)(1000000000.0f / consumer_sampling_frequency);

    while(1){

        bool is_tx = atomic_load(&is_transmitting);

        if (is_tx && !prev_transmitting) {
            clock_gettime(CLOCK_MONOTONIC, &tx_start);
            fft_flush();
            bitaccum_reset(&accum);
        }

        bool falling_edge = !is_tx && prev_transmitting;


        /* ---- CONSUME BUFFER -------------------------------- */
        int popped = 0;
        while (CIRCBUF_POP(prod_cons_buff, &y) == 0) {
            popped++;
            float sample = (y - 225.0f) / 100.0f;
            fft_push(sample);

            /* ---- FFT COMPUTATION --------------- */
            if (fft_ready()) {
                static float complex fft_buf[FFT_SIZE];

                int start = sample_head;
                for (int k = 0; k < FFT_SIZE; k++) {
                    float hann = 0.5f * (1.0f - cosf(2.0f * PI * k / (FFT_SIZE - 1)));
                    // here we assume that we have completely overwritten the buffer with new informations
                    float s = sample_buff[(start + k) % FFT_SIZE];
                    fft_buf[k] = (float complex)(s * hann);
                }



                fft(fft_buf, FFT_SIZE);

                float ref = 1.0f / FFT_SIZE;
                for (int k = 0; k < FFT_SIZE / 2; k++) {
                    float mag = cabsf(fft_buf[k]) * ref * 2.0f; /* *2 for one-sided */
                    float db  = (mag > 1e-10f) ? 20.0f * log10f(mag) : -80.0f;

                    // magnitude[k] = magnitude[k] * 0.7f + db * 0.3f;
                    float res = magnitude[k] * 0.7f + db * 0.3f;
                    magnitude[k] = res;
                    CIRCBUF_PUSH(cons_ui_buff, &res);
                }

                /* ---- DEMODULATION ------------------------------------------------- */
                if (atomic_load(&is_transmitting)) {
                    int sym = demod_goertzel(sample_buff, FFT_SIZE);
                    int bit1 = (sym >> 1) & 1;
                    int bit0 =  sym & 1;
                    bitaccum_push(&accum, bit1);
                    bitaccum_push(&accum, bit0);
                }

                fft_flush();
            }
        }

        if (falling_edge) {
            struct timespec tx_end;
                clock_gettime(CLOCK_MONOTONIC, &tx_end);

                double elapsed_ms = (tx_end.tv_sec  - tx_start.tv_sec)  * 1000.0
                                + (tx_end.tv_nsec - tx_start.tv_nsec) / 1000000.0;

                printf("[CONS] decoded: %.*s\n", (int)accum.byte_count, accum.bytes);
                printf("[CONS] bit/s: %.2f\n", (accum.byte_count * 8) / (elapsed_ms / 1000.0));
                printf("[CONS] transmission time: %.2f ms\n", elapsed_ms);
                printf("[CONS] expected time: %.2f ms\n",
                    (strlen(message) * 4 * FFT_SIZE / producer_sampling_frequency) * 1000.0);
                bitaccum_reset(&accum);
        }

        prev_transmitting = is_tx;


        if (popped == 0) {
            wait_nanosec(delay_ns);
        }
    }

    return NULL;
}

// UI
int main(void)
{
    /* Waveform panel: top 500 px  |  Spectrum panel: bottom 300 px */
    const int waveH = screenHeight - SPECTRUM_H;

    InitWindow(screenWidth, screenHeight, "Waveform + FFT Spectrum");
    SetTargetFPS((int)ui_sampling_frequency);

    pthread_t producer;
    pthread_create(&producer, NULL, producer_func, NULL);
    pthread_t consumer;
    pthread_create(&consumer, NULL, consumer_func, NULL);

    int current_x = 0;

    // local buffer definition
    Packet prod_ui_buff_local[PROD_CONS_BUFF_SIZE] = {0};
    int prod_ui_buff_head = 0;

    float cons_ui_buff_local[FFT_SIZE] = {0};
    int cons_ui_buff_head = 0;

    while (!WindowShouldClose()) {
        if (IsKeyDown(KEY_UP)) target_freq += 10.0f;
        if (IsKeyDown(KEY_DOWN)) target_freq -= 10.0f;
        if (target_freq < 10.0f) target_freq = 10.0f;

        /* ---- TRANSMISSION TRIGGER -------------------------------- */
        if(IsKeyPressed(KEY_T)){
            bool current = atomic_load(&is_transmitting);
            atomic_store(&is_transmitting, !current);
        }


        // DRAINING BUFFERS
        Packet prod_packet;
        while (CIRCBUF_POP(prod_ui_buff, &prod_packet) == 0) {
            prod_ui_buff_local[prod_ui_buff_head++] = prod_packet;
            if (prod_ui_buff_head >= PROD_CONS_BUFF_SIZE) prod_ui_buff_head = 0; // Wrap around
        }

        float cons_val;
        while (CIRCBUF_POP(cons_ui_buff, &cons_val) == 0){
            cons_ui_buff_local[cons_ui_buff_head++] = cons_val;
            if (cons_ui_buff_head >= (FFT_SIZE / 2)) cons_ui_buff_head = 0; // Wrap around
        }

        /* ---- RENDERING ------------------------------------------------- */
        BeginDrawing();
        ClearBackground(BLACK);

        /* PRODUCER RENDERING */
        const int TX = screenWidth - 280;
        const int TY = 10;
        const int ROW = 16;
        const int TELEM_HISTORY = 16;

        DrawRectangle(TX - 5, TY - 5, 275, TELEM_HISTORY * ROW + 30, (Color){0,0,0,180});
        DrawText("PRODUCER TELEMETRY", TX, TY, 12, YELLOW);
        // Step backwards to get the 16 most recent packets
        for (int i = 0; i < TELEM_HISTORY; i++) {
            // Correct modulo math to safely step backwards in a circular buffer
            int idx = (prod_ui_buff_head - 1 - i + PROD_CONS_BUFF_SIZE) % PROD_CONS_BUFF_SIZE;
            Packet* p = &prod_ui_buff_local[idx];

            // Skip empty structs if the buffer hasn't filled up yet
            if (p->timestamp_ns == 0) continue;

            DrawText(
                TextFormat("symbol=%d drift=%.2f pc=%d ui=%d",
                    p->symbol_index,
                    p->cumulative_drift_ms,
                    p->prod_cons_fill,
                    p->prod_ui_fill),
                TX, TY + (i + 1) * ROW, 20,
                p->is_transmitting ? GREEN : GRAY);
        }

        /* PRODUCER RENDERING (Waveform) */
        DrawLine(0, waveH, screenWidth, waveH, DARKGRAY);
        DrawText(TextFormat("Frequency: %.1f Hz", target_freq), 20, 20, 20, WHITE);
        float x_step = (float)screenWidth / (FFT_SIZE - 1);
        for (int k = 1; k < FFT_SIZE; k++) {
            // 1. Modulo by the ACTUAL array size (PROD_CONS_BUFF_SIZE), not FFT_SIZE
            // 2. Safely step backwards from the head to get the newest data
            int idx_prev = (prod_ui_buff_head - FFT_SIZE + k - 1 + PROD_CONS_BUFF_SIZE) % PROD_CONS_BUFF_SIZE;
            int idx_curr = (prod_ui_buff_head - FFT_SIZE + k     + PROD_CONS_BUFF_SIZE) % PROD_CONS_BUFF_SIZE;

            // 3. Just use the raw value! It is already scaled by sample_sin()
            float val_prev = prod_ui_buff_local[idx_prev].prod_value;
            float val_curr = prod_ui_buff_local[idx_curr].prod_value;

            // Draw line connecting them, stretched across the screen
            DrawLineV(
                (Vector2){ (k - 1) * x_step, val_prev },
                (Vector2){ k * x_step,       val_curr },
                RED
            );
        }



        /* CONSUMER RENDERING */
        const int bins     = FFT_SIZE / 2;
        const float barW   = (float)screenWidth / bins;
        const float db_min = -80.0f;
        const float db_max =   0.0f;
        const float freq_res = producer_sampling_frequency / FFT_SIZE; /* Hz per bin */

        for (int k = 0; k < bins; k++) {
            float db     = cons_ui_buff_local[k];
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


        EndDrawing();
    }

    pthread_detach(producer);
    pthread_detach(consumer);
    CloseWindow();
    return 0;
}
