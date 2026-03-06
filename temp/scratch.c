// CONSUMER
int find_peak_bin(float* magnitude, int bins) {
    int peak = 1;
    for (int k = 2; k < bins; k++)
        if (magnitude[k] > magnitude[peak]) peak = k;
    return peak;
}

// CONSUMER
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

// CONSUMER
float bin_to_freq(int bin) {
    return bin * (producer_sampling_frequency / FFT_SIZE);
}
