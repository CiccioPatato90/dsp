# Task: Dynamic symbols_lut

Make the FSK symbol count configurable by only changing `symbols_lut[]`. All other code adapts automatically.

## Rules
- `symbols_lut` length must always be a power of 2 (2, 4, 8, 16...)
- `SYMBOL_COUNT` and `BITS_PER_SYMBOL` are derived at compile time
- No other constants need changing

## Changes

### 1. Derive constants from LUT
```c
#define SYMBOL_COUNT    (sizeof(symbols_lut) / sizeof(symbols_lut[0]))
#define BITS_PER_SYMBOL ((int)(log2(SYMBOL_COUNT)))
```

### 2. `fsk_symbols()` — generalize bit grouping
```c
void fsk_symbols(const char* message, float* out_symbols, size_t* out_len) {
    size_t n_bits    = strlen(message) * 8;
    size_t n_symbols = n_bits / BITS_PER_SYMBOL;
    uint8_t* bits = malloc(n_bits);
    if (!bits) return;
    str_to_bits(message, bits);
    for (size_t i = 0; i < n_symbols; i++) {
        uint8_t combined = 0;
        for (int b = 0; b < BITS_PER_SYMBOL; b++)
            combined = (combined << 1) | bits[i * BITS_PER_SYMBOL + b];
        out_symbols[i] = symbols_lut[combined];
    }
    if (out_len) *out_len = n_symbols;
    free(bits);
}
```

### 3. `demod_goertzel()` — iterate all symbols
```c
int demod_goertzel(float* samples, int N) {
    int32_t best_mag = -1;
    int best_sym = 0;
    for (int i = 0; i < (int)SYMBOL_COUNT; i++) {
        int32_t mag = goertzel(symbols_lut[i], producer_sampling_frequency, samples, N);
        if (mag > best_mag) { best_mag = mag; best_sym = i; }
    }
    return best_sym;
}
```

### 4. Bit reconstruction — push BITS_PER_SYMBOL bits per symbol
```c
int sym = demod_goertzel(fft_input, FFT_SIZE);
for (int b = BITS_PER_SYMBOL - 1; b >= 0; b--)
    bitaccum_push(&accum, (sym >> b) & 1);
```

### 5. Producer — use derived symbol count
```c
size_t n_symbols = 0;
float symbols[strlen(message) * 8 / BITS_PER_SYMBOL];
fsk_symbols(message, symbols, &n_symbols);
```

## Example: switching from 4-FSK to 8-FSK
```c
// 4-FSK: 2 bits/symbol
static const float symbols_lut[] = { 120.0f, 230.0f, 340.0f, 450.0f };

// 8-FSK: 3 bits/symbol — just extend the array
static const float symbols_lut[] = { 120.0f, 185.0f, 250.0f, 315.0f,
                                      380.0f, 445.0f, 510.0f, 575.0f };
```
