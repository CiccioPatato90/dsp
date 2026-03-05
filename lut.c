#include <pthread.h>
#include <stdbool.h>
#include <complex.h>   // C99 complex for FFT
#include <unistd.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

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
   38.0f, 39.0f, 40.0f, 41.0f
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
    printf("\n");

    free(bits);
}

int main(void){
    char* message = "Hello, World!";
    size_t n_symbols = strlen(message) * 4;
    float symbols[n_symbols];

    fsk_symbols(message, symbols);

    for (size_t i = 0; i < n_symbols; i ++) {
        printf("-%.2f-", symbols[i]);
    }
}
