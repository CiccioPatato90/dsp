#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <math.h>
#include <complex.h>
#include <string.h>
#include <sys/types.h>
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
#define SPECTRUM_H   200          // pixel height of spectrum panel

typedef struct {
    uint8_t version;
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
    enum { CONS_FFT, CONS_RISING, CONS_FALLING } cons_tag;
    float   cons_value;
    float   goertzel_energies[4];   // most important consumer field
    int     symbol_decoded;
    int     bits_in;
    int     cons_fft_bin;
    uint8_t*decoded_bytes;
    size_t  decoded_byte_count;
    uint8_t last_byte;
    float   effective_tx_time;
    float   expected_tx_time;
    float   bitrate;
    int    popped;
    uint64_t fft_time_ns;
    uint64_t goertzel_time_ns;

    // --- Shared buffer health ---
    int     prod_cons_fill;
    int     prod_ui_fill;
    int     cons_ui_fill;
} Packet;

CIRCBUF_DEF(float, prod_cons_buff, PROD_CONS_BUFF_SIZE);
CIRCBUF_DEF(Packet, prod_ui_buff, PROD_UI_BUFF_SIZE);
CIRCBUF_DEF(Packet, cons_ui_buff, CONS_UI_BUFF_SIZE);


atomic_bool is_transmitting = false;
const char* message = "Hello, MA STAR!";
float target_freq               = 0.1f;
const float consumer_sampling_frequency = 100000.0f;
const float producer_sampling_frequency = 100000.0f;
const float ui_sampling_frequency = 120.0f;
const int   screenWidth               = 1500;
const int   screenHeight              = 800;

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
                    Stopwatch compute_timer = timer_start();
                    float y_pos = sample_sin(symbols[i], &current_angle);
                    uint64_t sample_time_ns = timer_elapsed_ns(compute_timer);

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
                    t.sample_time_ns = sample_time_ns;

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
            Stopwatch compute_timer = timer_start();
            float y_pos = sample_sin(target_freq, &current_angle);
            uint64_t math_time_ns = timer_elapsed_ns(compute_timer);

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
            t.sample_time_ns = math_time_ns;

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
int demod_goertzel(float* fft, int N, Packet *telemetry) {
    int32_t best_mag = -1;
    int best_sym = 0;
    for (int i = 0; i < 4; i++) {
        int32_t mag = goertzel(symbols_lut[i], producer_sampling_frequency, fft, N);
        telemetry->goertzel_energies[i] = mag;
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

        Packet t = {0};
        t.source = TELEM_CONSUMER;
        t.timestamp_ns = now_ns();

        if (is_tx && !prev_transmitting) {
            clock_gettime(CLOCK_MONOTONIC, &tx_start);
            fft_flush();
            bitaccum_reset(&accum);
            t.is_transmitting = true;
        }

        bool falling_edge = !is_tx && prev_transmitting;

        /* ---- CONSUME BUFFER -------------------------------- */
        int popped = 0;
        while (CIRCBUF_POP(prod_cons_buff, &y) == 0) {
            popped++;
            float sample = (y - 225.0f) / 100.0f;
            fft_push(sample);


            t.prod_cons_fill = CIRCBUF_FS(prod_cons_buff);
            t.prod_ui_fill = CIRCBUF_FS(prod_ui_buff);
            t.cons_ui_fill = CIRCBUF_FS(cons_ui_buff);

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

                Stopwatch compute_timer = timer_start();
                fft(fft_buf, FFT_SIZE);
                uint64_t math_time_ns = timer_elapsed_ns(compute_timer);
                t.fft_time_ns = math_time_ns;

                float ref = 1.0f / FFT_SIZE;
                for (int k = 0; k < FFT_SIZE / 2; k++) {
                    float mag = cabsf(fft_buf[k]) * ref * 2.0f; /* *2 for one-sided */
                    float db  = (mag > 1e-10f) ? 20.0f * log10f(mag) : -80.0f;

                    // magnitude[k] = magnitude[k] * 0.7f + db * 0.3f;
                    float res = magnitude[k] * 0.7f + db * 0.3f;
                    magnitude[k] = res;
                    // first return point push
                    t.cons_value = res;
                    t.cons_fft_bin = k;
                    t.cons_tag = CONS_FFT;
                    CIRCBUF_PUSH(cons_ui_buff, &t);
                }

                /* ---- DEMODULATION ------------------------------------------------- */
                if (atomic_load(&is_transmitting)) {
                    compute_timer = timer_start();
                    int sym = demod_goertzel(sample_buff, FFT_SIZE, &t);
                    uint64_t math_time_ns = timer_elapsed_ns(compute_timer);
                    t.goertzel_time_ns = math_time_ns;
                    t.symbol_decoded = sym;
                    int bit1 = (sym >> 1) & 1;
                    int bit0 =  sym & 1;
                    bitaccum_push(&accum, bit1);
                    bitaccum_push(&accum, bit0);
                    t.cons_tag = CONS_RISING;
                    CIRCBUF_PUSH(cons_ui_buff, &t);
                }
                fft_flush();
            }
        }

        if (falling_edge) {
            struct timespec tx_end;
            clock_gettime(CLOCK_MONOTONIC, &tx_end);

            double elapsed_ms = (tx_end.tv_sec  - tx_start.tv_sec)  * 1000.0
                            + (tx_end.tv_nsec - tx_start.tv_nsec) / 1000000.0;

            // printf("[CONS] decoded: %.*s\n", (int)accum.byte_count, accum.bytes);
            // printf("[CONS] bit/s: %.2f\n", (accum.byte_count * 8) / (elapsed_ms / 1000.0));
            // printf("[CONS] transmission time: %.2f ms\n", elapsed_ms);
            t.decoded_bytes = accum.bytes;
            t.decoded_byte_count = accum.byte_count;
            t.bitrate = (accum.byte_count * 8) / (elapsed_ms / 1000.0);
            t.effective_tx_time = elapsed_ms;
            // printf("[CONS] expected time: %.2f ms\n",
            //     (strlen(message) * 4 * FFT_SIZE / producer_sampling_frequency) * 1000.0);
            t.expected_tx_time = (strlen(message) * 4 * FFT_SIZE / producer_sampling_frequency) * 1000.0;
            bitaccum_reset(&accum);
            t.popped = popped;
            t.cons_tag = CONS_FALLING;
            CIRCBUF_PUSH(cons_ui_buff, &t);
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

    Packet cons_ui_buff_local[FFT_SIZE] = {0};
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


        /* =========================================================
           DRAIN BUFFERS
           ========================================================= */
        static float spectrum[FFT_SIZE / 2] = {0};
        static Packet last_falling = {0};

        Packet prod_packet;
        while (CIRCBUF_POP(prod_ui_buff, &prod_packet) == 0) {
            prod_ui_buff_local[prod_ui_buff_head] = prod_packet;
            prod_ui_buff_head = (prod_ui_buff_head + 1) % PROD_CONS_BUFF_SIZE;
        }

        Packet cons_packet;
        while (CIRCBUF_POP(cons_ui_buff, &cons_packet) == 0) {
            cons_ui_buff_local[cons_ui_buff_head] = cons_packet;
            cons_ui_buff_head = (cons_ui_buff_head + 1) % CONS_UI_BUFF_SIZE;

            if (cons_packet.cons_tag == CONS_FFT)
                spectrum[cons_packet.cons_fft_bin] = cons_packet.cons_value;
            if (cons_packet.cons_tag == CONS_FALLING)
                last_falling = cons_packet;
        }

        /* =========================================================
           LAYOUT CONSTANTS
           ========================================================= */
        const int ROW          = 16;
        const int TELEM_HISTORY = 16;
        const int PANEL_W      = 280;
        const int PANEL_H      = TELEM_HISTORY * ROW + 30;
        const int MARGIN       = 10;
        const int TELEMETRY_FONT_SIZE = 17;

        // Anchor panels to top-right, side by side
        const int PROD_X = screenWidth - 2 * (PANEL_W + MARGIN);
        const int CONS_X = screenWidth - 1 * (PANEL_W + MARGIN);
        const int PANEL_Y = MARGIN;

        /* =========================================================
           DRAW
           ========================================================= */
        BeginDrawing();
        ClearBackground(BLACK);

        /* ---------------------------------------------------------
           WAVEFORM (top area)
           --------------------------------------------------------- */
        DrawLine(0, waveH, screenWidth, waveH, DARKGRAY);
        DrawText(TextFormat("Target: %.0f Hz", target_freq), 10, 10, 16, WHITE);

        float x_step = (float)screenWidth / (FFT_SIZE - 1);
        for (int k = 1; k < FFT_SIZE; k++) {
            int idx_prev = (prod_ui_buff_head - FFT_SIZE + k - 1 + PROD_CONS_BUFF_SIZE) % PROD_CONS_BUFF_SIZE;
            int idx_curr = (prod_ui_buff_head - FFT_SIZE + k     + PROD_CONS_BUFF_SIZE) % PROD_CONS_BUFF_SIZE;
            DrawLineV(
                (Vector2){ (k - 1) * x_step, prod_ui_buff_local[idx_prev].prod_value },
                (Vector2){ k       * x_step, prod_ui_buff_local[idx_curr].prod_value },
                RED
            );
        }

        /* ---------------------------------------------------------
           SPECTRUM (bottom area)
           --------------------------------------------------------- */
        const int    bins     = FFT_SIZE / 2;
        const float  barW     = (float)screenWidth / bins;
        const float  db_min   = -80.0f;
        const float  db_max   =   0.0f;
        const float  freq_res = producer_sampling_frequency / FFT_SIZE;

        for (int k = 0; k < bins; k++) {
            float norm = (spectrum[k] - db_min) / (db_max - db_min);
            norm = fmaxf(0.0f, fminf(1.0f, norm));
            int barH = (int)(norm * SPECTRUM_H);
            int x    = (int)(k * barW);
            int y0   = screenHeight - barH;
            Color col = ColorFromHSV(180.0f - norm * 60.0f, 1.0f, 0.9f);
            DrawRectangle(x, y0, (int)barW > 1 ? (int)barW - 1 : 1, barH, col);
        }

        // Frequency axis labels
        for (int hz = 0; hz <= (int)(producer_sampling_frequency / 2); hz += 1000) {
            int x = (int)((hz / freq_res) * barW);
            if (x >= screenWidth) break;
            DrawLine(x, waveH, x, screenHeight, DARKGRAY);
            DrawText(TextFormat("%dk", hz / 1000), x + 2, waveH + 4, 10, GRAY);
        }

        // Target frequency marker
        {
            int tx = (int)((target_freq / freq_res) * barW);
            DrawLine(tx, waveH, tx, screenHeight, YELLOW);
            DrawText(TextFormat("%.0fHz", target_freq), tx + 2, waveH + 16, 10, YELLOW);
        }

        /* ---------------------------------------------------------
           PRODUCER TELEMETRY PANEL
           --------------------------------------------------------- */
        DrawRectangle(PROD_X - 5, PANEL_Y - 5, PANEL_W, PANEL_H, (Color){0,0,0,200});
        DrawText("PRODUCER", PROD_X, PANEL_Y, TELEMETRY_FONT_SIZE + 3, YELLOW);

        for (int i = 0; i < TELEM_HISTORY; i++) {
            int idx = (prod_ui_buff_head - 1 - i + PROD_CONS_BUFF_SIZE) % PROD_CONS_BUFF_SIZE;
            Packet *p = &prod_ui_buff_local[idx];
            if (p->timestamp_ns == 0) continue;
            DrawText(
                TextFormat("sym=%d drift=%.2f pc=%d",
                    p->symbol_index,
                    p->cumulative_drift_ms,
                    p->prod_cons_fill),
                PROD_X, PANEL_Y + (i + 1) * ROW, TELEMETRY_FONT_SIZE,
                p->is_transmitting ? GREEN : GRAY);
        }

        /* ---------------------------------------------------------
           CONSUMER TELEMETRY PANEL
           --------------------------------------------------------- */
        DrawRectangle(CONS_X - 5, PANEL_Y - 5, PANEL_W, PANEL_H, (Color){0,0,0,200});
        DrawText("CONSUMER", CONS_X, PANEL_Y, TELEMETRY_FONT_SIZE + 3, YELLOW);

        for (int i = 0; i < TELEM_HISTORY; i++) {
            int idx = (cons_ui_buff_head - 1 - i + CONS_UI_BUFF_SIZE) % CONS_UI_BUFF_SIZE;
            Packet *p = &cons_ui_buff_local[idx];
            if (p->timestamp_ns == 0) continue;

            Color col = GRAY;
            const char *tag_str = "---";
            if (p->cons_tag == CONS_FFT)     { col = SKYBLUE; tag_str = "FFT"; }
            if (p->cons_tag == CONS_RISING)  { col = GREEN;   tag_str = "SYM"; }
            if (p->cons_tag == CONS_FALLING) { col = ORANGE;  tag_str = "END"; }

            DrawText(
                TextFormat("[%s] sym=%d fft=%lluus gz=%lluus",
                    tag_str,
                    p->symbol_decoded,
                    p->fft_time_ns   / 1000,
                    p->goertzel_time_ns / 1000),
                CONS_X, PANEL_Y + (i + 1) * ROW, TELEMETRY_FONT_SIZE, col);
        }

        /* ---------------------------------------------------------
           DECODED RESULT PANEL (persists from last transmission)
           --------------------------------------------------------- */
        if (last_falling.timestamp_ns != 0) {
            const int RX = PROD_X - 5;
            const int RY = PANEL_Y + PANEL_H + MARGIN;
            const int RW = 2 * PANEL_W + MARGIN;
            const int RH = 70;

            DrawRectangle(RX, RY, RW, RH, (Color){0,0,0,200});
            DrawRectangleLines(RX, RY, RW, RH, ORANGE);

            bool time_ok = fabsf(last_falling.effective_tx_time - last_falling.expected_tx_time) < 50.0f;

            DrawText(
                TextFormat("MSG:     %.*s",
                    (int)last_falling.decoded_byte_count,
                    last_falling.decoded_bytes),
                RX + 8, RY + 8, TELEMETRY_FONT_SIZE + 3, WHITE);
            DrawText(
                TextFormat("BITRATE: %.1f bit/s", last_falling.bitrate),
                RX + 8, RY + 28, TELEMETRY_FONT_SIZE + 3, SKYBLUE);
            DrawText(
                TextFormat("TIME:    %.1f ms  (expected %.1f ms)",
                    last_falling.effective_tx_time,
                    last_falling.expected_tx_time),
                RX + 8, RY + 48, TELEMETRY_FONT_SIZE + 3, time_ok ? GREEN : RED);
        }

        EndDrawing();
    }

    pthread_detach(producer);
    pthread_detach(consumer);
    CloseWindow();
    return 0;
}
