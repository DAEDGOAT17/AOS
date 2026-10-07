#include "../include/math.h"
#include <stdint.h>

// Simple fixed-point matrix multiplication (row-major)
// C = A * B
void mat_mul_fixed(const int32_t *A, const int32_t *B, int32_t *C, uint32_t n, uint8_t frac_bits) {
    for (uint32_t i = 0; i < n; ++i) {
        for (uint32_t j = 0; j < n; ++j) {
            int64_t acc = 0;
            for (uint32_t k = 0; k < n; ++k) {
                int64_t a = A[i * n + k];
                int64_t b = B[k * n + j];
                acc += a * b; // product has 2*frac_bits fractional bits
            }
            // Downshift back to frac_bits with rounding
            int64_t rounding = (int64_t)1 << (frac_bits - 1);
            acc = (acc + rounding) >> frac_bits;
            C[i * n + j] = (int32_t)acc;
        }
    }
}

// Float implementations for freestanding environment

float sqrtf(float x) {
    if (x <= 0.0f) return 0.0f;
    float result = x < 1.0f ? 1.0f : x;
    for (int iteration = 0; iteration < 64; iteration++) {
        float next = 0.5f * (result + x / result);
        float difference = next > result ? next - result : result - next;
        result = next;
        if (difference <= result * 1.0e-6f) break;
    }
    return result;
}

float expf(float x) {
    if (x > 20.0f) x = 20.0f;
    if (x < -20.0f) x = -20.0f;

    const float ln2 = 0.6931471806f;
    int exponent = (int)(x / ln2);
    float reduced = x - (float)exponent * ln2;
    if (reduced < 0.0f) {
        reduced += ln2;
        exponent--;
    }

    float sum = 1.0f;
    float term = 1.0f;
    for (int i = 1; i <= 10; i++) {
        term = term * reduced / (float)i;
        sum += term;
    }

    while (exponent > 0) {
        sum *= 2.0f;
        exponent--;
    }
    while (exponent < 0) {
        sum *= 0.5f;
        exponent++;
    }
    return sum;
}

float tanhf(float x) {
    if (x > 10.0f) return 1.0f;
    if (x < -10.0f) return -1.0f;
    float e2x = expf(2.0f * x);
    return (e2x - 1.0f) / (e2x + 1.0f);
}
