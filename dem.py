import numpy as np

# Define FSK demodulation parameters
sampling_rate = 1000  # Sampling rate in Hz
frequency_1 = 10  # Frequency of the first FSK symbol in Hz
frequency_2 = 20  # Frequency of the second FSK symbol in Hz
symbol_duration = 1  # Duration of each FSK symbol in seconds
samples_per_symbol = symbol_duration * sampling_rate
# Generate a sample FSK signal (you would replace this with your actual input signal)
t = np.linspace(0, symbol_duration, samples_per_symbol, endpoint=False)
fsk_signal = np.concatenate(
    (np.sin(2 * np.pi * frequency_1 * t), np.sin(2 * np.pi * frequency_2 * t))
)
# Perform FSK demodulation
demodulated_signal = np.abs(np.fft.fft(fsk_signal))
# Thresholding to extract bit string
threshold = np.mean(demodulated_signal)
bit_string = "".join("1" if x > threshold else "0" for x in demodulated_signal)
# Print the extracted bit string
print("Extracted bit string:", bit_string)
