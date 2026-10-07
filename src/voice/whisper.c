#include "whisper.h"
#include "pmm.h"
#include "screen.h"
#include "timer.h"
#if defined(__SSE2__)
#include <stddef.h>
extern void* malloc(size_t size);
extern void free(void* pointer);
#include <emmintrin.h>
#endif

#define WHISPER_MAX_MELS 128
#define WHISPER_MAX_FILTER_SIZE 1024
#define WHISPER_MAX_TENSORS 256
#define WHISPER_FFT_SIZE 400
#define WHISPER_FFT_BINS 201
#define WHISPER_HOP_SIZE 160
#define WHISPER_TINY_STATE 384
#define WHISPER_TINY_FF_STATE 1536
#define WHISPER_TINY_AUDIO_CTX 1500
#define WHISPER_TINY_TEXT_CTX 448
#define WHISPER_MAX_OUTPUT_TOKENS 24

extern float sqrtf(float value);
extern float tanhf(float value);

typedef struct {
    const uint8_t* bytes;
    uint32_t size;
    uint32_t offset;
} whisper_reader_t;

static whisper_model_info_t model_info;
static uint32_t token_offsets[WHISPER_MAX_VOCAB];
static uint32_t token_lengths[WHISPER_MAX_VOCAB];
static uint32_t tensor_dimensions[WHISPER_MAX_TENSORS][4];
static whisper_tensor_t model_tensors[WHISPER_MAX_TENSORS];
static int model_is_ready;
static float fft_sin[WHISPER_FFT_SIZE];
static float fft_cos[WHISPER_FFT_SIZE];
static float hann_window[WHISPER_FFT_SIZE];
static float fft_power[WHISPER_FFT_BINS];
static float fft_input[WHISPER_FFT_SIZE];
static float attention_scores[WHISPER_MEL_FRAMES];
static float encoder_conv1[WHISPER_MEL_FRAMES * WHISPER_TINY_STATE];
static float encoder_conv2[WHISPER_TINY_AUDIO_CTX * WHISPER_TINY_STATE];
static float encoder_state_a[WHISPER_TINY_AUDIO_CTX * WHISPER_TINY_STATE];
static float encoder_state_b[WHISPER_TINY_AUDIO_CTX * WHISPER_TINY_STATE];
static float encoder_norm[WHISPER_TINY_AUDIO_CTX * WHISPER_TINY_STATE];
static float encoder_query[WHISPER_TINY_AUDIO_CTX * WHISPER_TINY_STATE];
static float encoder_key[WHISPER_TINY_AUDIO_CTX * WHISPER_TINY_STATE];
static float encoder_value[WHISPER_TINY_AUDIO_CTX * WHISPER_TINY_STATE];
static float encoder_attention[WHISPER_TINY_AUDIO_CTX * WHISPER_TINY_STATE];
static float encoder_hidden[WHISPER_TINY_FF_STATE];
static float encoder_vector[WHISPER_TINY_STATE];
static uint32_t encoded_frame_count;
static int encoder_error;
static float decoder_cross_key[4 * WHISPER_TINY_AUDIO_CTX * WHISPER_TINY_STATE];
static float decoder_cross_value[4 * WHISPER_TINY_AUDIO_CTX * WHISPER_TINY_STATE];
static float decoder_self_key[4 * WHISPER_TINY_TEXT_CTX * WHISPER_TINY_STATE];
static float decoder_self_value[4 * WHISPER_TINY_TEXT_CTX * WHISPER_TINY_STATE];
static float decoder_logits[60000];
static float transcription_mel[80 * WHISPER_MEL_FRAMES];
static uint32_t decoder_tokens[WHISPER_TINY_TEXT_CTX];

typedef struct {
    const whisper_tensor_t* attn_norm_weight;
    const whisper_tensor_t* attn_norm_bias;
    const whisper_tensor_t* attn_query_weight;
    const whisper_tensor_t* attn_query_bias;
    const whisper_tensor_t* attn_key_weight;
    const whisper_tensor_t* attn_value_weight;
    const whisper_tensor_t* attn_value_bias;
    const whisper_tensor_t* attn_out_weight;
    const whisper_tensor_t* attn_out_bias;
    const whisper_tensor_t* cross_norm_weight;
    const whisper_tensor_t* cross_norm_bias;
    const whisper_tensor_t* cross_query_weight;
    const whisper_tensor_t* cross_query_bias;
    const whisper_tensor_t* cross_key_weight;
    const whisper_tensor_t* cross_value_weight;
    const whisper_tensor_t* cross_value_bias;
    const whisper_tensor_t* cross_out_weight;
    const whisper_tensor_t* cross_out_bias;
    const whisper_tensor_t* mlp_norm_weight;
    const whisper_tensor_t* mlp_norm_bias;
    const whisper_tensor_t* mlp_in_weight;
    const whisper_tensor_t* mlp_in_bias;
    const whisper_tensor_t* mlp_out_weight;
    const whisper_tensor_t* mlp_out_bias;
} whisper_decoder_layer_t;

static whisper_decoder_layer_t decoder_layers[4];

static float whisper_log10(float value) {
    if (value < 1.0e-10f) value = 1.0e-10f;

    int exponent = 0;
    while (value >= 2.0f) {
        value *= 0.5f;
        exponent++;
    }
    while (value < 1.0f) {
        value *= 2.0f;
        exponent--;
    }

    float ratio = (value - 1.0f) / (value + 1.0f);
    float squared = ratio * ratio;
    float term = ratio;
    float logarithm = term;
    for (int divisor = 3; divisor <= 21; divisor += 2) {
        term *= squared;
        logarithm += term / (float)divisor;
    }
    logarithm = 2.0f * logarithm + (float)exponent * 0.6931471806f;
    return logarithm * 0.4342944819f;
}

static float whisper_exp_negative(float value) {
    if (value >= 0.0f) return 1.0f;
    if (value < -80.0f) return 0.0f;

    float scale = 1.0f;
    while (value < -0.6931471806f) {
        value += 0.6931471806f;
        scale *= 0.5f;
    }

    float sum = 1.0f;
    float term = 1.0f;
    for (int divisor = 1; divisor <= 10; divisor++) {
        term *= value / (float)divisor;
        sum += term;
    }
    return sum * scale;
}

static void whisper_mark_progress(const char* stage, uint32_t started_at) {
    if (!stage) return;
    uint32_t elapsed = timer_get_ticks() - started_at;
    print_string("Whisper: ");
    print_string(stage);
    print_string(" done in ");
    kprint_dec(elapsed);
    print_string(" ticks.\n");
}

static void whisper_init_audio_tables(void) {
    const float step_cos = 0.9998766325f;
    const float step_sin = 0.0157073173f;
    float cosine = 1.0f;
    float sine = 0.0f;

    for (uint32_t index = 0; index < WHISPER_FFT_SIZE; index++) {
        fft_cos[index] = cosine;
        fft_sin[index] = sine;
        hann_window[index] = 0.5f * (1.0f - cosine);

        float next_cosine = cosine * step_cos - sine * step_sin;
        sine = sine * step_cos + cosine * step_sin;
        cosine = next_cosine;
        if ((index & 31U) == 31U) {
            float magnitude = sqrtf(cosine * cosine + sine * sine);
            cosine /= magnitude;
            sine /= magnitude;
        }
    }
}

static float whisper_pcm_sample(const int16_t* pcm, uint32_t sample_count, int64_t index) {
    if (index < 0) {
        uint64_t reflected = (uint64_t)(-index);
        if (reflected >= sample_count) return 0.0f;
        return (float)pcm[reflected] / 32768.0f;
    }
    if ((uint64_t)index < sample_count)
        return (float)pcm[index] / 32768.0f;
    if ((uint64_t)index < (uint64_t)sample_count + WHISPER_FFT_SIZE / 2 && sample_count > 1) {
        uint64_t reflected = (uint64_t)sample_count - 2 -
            ((uint64_t)index - sample_count);
        return (float)pcm[reflected] / 32768.0f;
    }
    return 0.0f;
}

int whisper_pcm_to_mel(const int16_t* pcm, uint32_t sample_count,
    float* mel, size_t mel_capacity, uint32_t* frame_count) {
    if (!model_is_ready || !mel || !frame_count || sample_count > WHISPER_MAX_PCM_SAMPLES ||
        (sample_count > 0 && !pcm) || model_info.n_filters != 80 ||
        model_info.filter_size != WHISPER_FFT_BINS ||
        mel_capacity < (size_t)model_info.n_filters * WHISPER_MEL_FRAMES) return 0;

    whisper_init_audio_tables();
    const uint32_t n_mels = model_info.n_filters;
    const uint32_t n_frames = WHISPER_MEL_FRAMES;
    const float silence_log_energy = -10.0f;
    for (size_t index = 0; index < (size_t)n_mels * n_frames; index++)
        mel[index] = silence_log_energy;

    uint32_t active_frames = sample_count == 0 ? 1 :
        (sample_count + WHISPER_FFT_SIZE + WHISPER_HOP_SIZE - 1) / WHISPER_HOP_SIZE;
    if (active_frames > n_frames) active_frames = n_frames;

    for (uint32_t frame = 0; frame < active_frames; frame++) {
        int64_t offset = (int64_t)frame * WHISPER_HOP_SIZE - WHISPER_FFT_SIZE / 2;
        int has_signal = 0;
        for (uint32_t sample = 0; sample < WHISPER_FFT_SIZE; sample++) {
            float value = whisper_pcm_sample(pcm, sample_count, offset + sample);
            fft_input[sample] = value * hann_window[sample];
            if (fft_input[sample] != 0.0f) has_signal = 1;
        }
        if (!has_signal) continue;

        for (uint32_t bin = 0; bin < WHISPER_FFT_BINS; bin++) {
            float real = 0.0f;
            float imaginary = 0.0f;
            for (uint32_t sample = 0; sample < WHISPER_FFT_SIZE; sample++) {
                uint32_t phase = (bin * sample) % WHISPER_FFT_SIZE;
                real += fft_input[sample] * fft_cos[phase];
                imaginary -= fft_input[sample] * fft_sin[phase];
            }
            fft_power[bin] = real * real + imaginary * imaginary;
        }

        for (uint32_t band = 0; band < n_mels; band++) {
            float energy = 0.0f;
            const float* filter = model_info.mel_filters +
                (size_t)band * model_info.filter_size;
            for (uint32_t bin = 0; bin < WHISPER_FFT_BINS; bin++)
                energy += fft_power[bin] * filter[bin];
            mel[(size_t)band * n_frames + frame] = whisper_log10(energy);
        }
    }

    float maximum = -1.0e20f;
    for (size_t index = 0; index < (size_t)n_mels * n_frames; index++)
        if (mel[index] > maximum) maximum = mel[index];
    maximum -= 8.0f;

    for (size_t index = 0; index < (size_t)n_mels * n_frames; index++) {
        if (mel[index] < maximum) mel[index] = maximum;
        mel[index] = (mel[index] + 4.0f) * 0.25f;
    }

    for (uint32_t band = 0; band < n_mels; band++) {
        for (uint32_t frame = 0; frame < active_frames; frame++)
            mel[(size_t)band * active_frames + frame] =
                mel[(size_t)band * n_frames + frame];
    }

    *frame_count = active_frames;
    return 1;
}

static int whisper_read_u32(whisper_reader_t* reader, uint32_t* value) {
    if (!reader || !value || reader->offset > reader->size ||
        reader->size - reader->offset < 4) return 0;

    const uint8_t* source = reader->bytes + reader->offset;
    *value = (uint32_t)source[0] | ((uint32_t)source[1] << 8) |
        ((uint32_t)source[2] << 16) | ((uint32_t)source[3] << 24);
    reader->offset += 4;
    return 1;
}

static int whisper_skip(whisper_reader_t* reader, uint64_t count) {
    if (!reader || reader->offset > reader->size ||
        count > (uint64_t)(reader->size - reader->offset)) return 0;
    reader->offset += (uint32_t)count;
    return 1;
}

static int whisper_model_clear(void) {
    uint8_t* bytes = (uint8_t*)&model_info;
    for (uint32_t index = 0; index < sizeof(model_info); index++) bytes[index] = 0;
    for (uint32_t index = 0; index < WHISPER_MAX_VOCAB; index++) {
        token_offsets[index] = 0;
        token_lengths[index] = 0;
    }
    for (uint32_t index = 0; index < WHISPER_MAX_TENSORS; index++) {
        uint8_t* tensor = (uint8_t*)&model_tensors[index];
        for (uint32_t byte = 0; byte < sizeof(model_tensors[index]); byte++) tensor[byte] = 0;
        for (uint32_t dimension = 0; dimension < 4; dimension++)
            tensor_dimensions[index][dimension] = 0;
    }
    model_is_ready = 0;
    return 0;
}

static int whisper_read_hparams(whisper_reader_t* reader, whisper_hparams_t* params) {
    uint32_t* fields = (uint32_t*)params;
    for (uint32_t index = 0; index < 11; index++) {
        if (!whisper_read_u32(reader, &fields[index])) return 0;
    }

    return params->n_vocab > 0 && params->n_vocab <= WHISPER_MAX_VOCAB &&
        params->n_audio_ctx > 0 && params->n_audio_state > 0 &&
        params->n_audio_head > 0 && params->n_audio_layer > 0 &&
        params->n_text_ctx > 0 && params->n_text_state > 0 &&
        params->n_text_head > 0 && params->n_text_layer > 0 &&
        params->n_mels > 0 && params->n_mels <= WHISPER_MAX_MELS;
}

static int whisper_read_filters(whisper_reader_t* reader) {
    uint32_t n_mels;
    uint32_t filter_size;
    if (!whisper_read_u32(reader, &n_mels) || !whisper_read_u32(reader, &filter_size))
        return 0;
    if (n_mels != model_info.hparams.n_mels || filter_size == 0 ||
        filter_size > WHISPER_MAX_FILTER_SIZE) return 0;

    uint64_t filter_bytes = (uint64_t)n_mels * filter_size * sizeof(float);
    if (reader->offset > reader->size || filter_bytes > reader->size - reader->offset)
        return 0;

    model_info.n_filters = n_mels;
    model_info.filter_size = filter_size;
    model_info.mel_filters = (const float*)(reader->bytes + reader->offset);
    return whisper_skip(reader, filter_bytes);
}

static int whisper_read_vocab(whisper_reader_t* reader) {
    uint32_t count;
    if (!whisper_read_u32(reader, &count) || count == 0 ||
        count > model_info.hparams.n_vocab ||
        count > WHISPER_MAX_VOCAB) return 0;

    for (uint32_t token = 0; token < count; token++) {
        uint32_t length;
        if (!whisper_read_u32(reader, &length) || reader->offset > reader->size ||
            length > reader->size - reader->offset) return 0;
        token_offsets[token] = reader->offset;
        token_lengths[token] = length;
        if (!whisper_skip(reader, length)) return 0;
    }

    model_info.vocab_count = count;
    model_info.vocab_offsets = token_offsets;
    model_info.vocab_lengths = token_lengths;
    return 1;
}

static int whisper_tensor_data_size(uint32_t type, uint64_t elements, uint64_t* size) {
    if (!size || elements == 0) return 0;
    if (type == 0) {
        if (elements > UINT64_MAX / sizeof(float)) return 0;
        *size = elements * sizeof(float);
        return 1;
    }
    if (type == 1) {
        if (elements > UINT64_MAX / sizeof(uint16_t)) return 0;
        *size = elements * sizeof(uint16_t);
        return 1;
    }
    return 0;
}

static int whisper_read_tensors(whisper_reader_t* reader) {
    while (reader->offset < reader->size) {
        if (model_info.tensor_count >= WHISPER_MAX_TENSORS) return 0;

        uint32_t n_dims;
        uint32_t name_length;
        uint32_t type;
        if (!whisper_read_u32(reader, &n_dims) || !whisper_read_u32(reader, &name_length) ||
            !whisper_read_u32(reader, &type)) return 0;
        if (n_dims == 0 || n_dims > 4 || name_length == 0 ||
            name_length >= WHISPER_TENSOR_NAME_MAX) return 0;

        uint32_t tensor_index = model_info.tensor_count;
        uint64_t elements = 1;
        for (uint32_t dimension = 0; dimension < n_dims; dimension++) {
            uint32_t size;
            if (!whisper_read_u32(reader, &size) || size == 0 ||
                elements > UINT64_MAX / size) return 0;
            tensor_dimensions[tensor_index][dimension] = size;
            elements *= size;
        }

        if (reader->offset > reader->size || name_length > reader->size - reader->offset)
            return 0;
        whisper_tensor_t* tensor = &model_tensors[tensor_index];
        for (uint32_t index = 0; index < name_length; index++)
            tensor->name[index] = (char)reader->bytes[reader->offset + index];
        tensor->name[name_length] = '\0';
        if (!whisper_skip(reader, name_length)) return 0;

        uint64_t data_size;
        if (!whisper_tensor_data_size(type, elements, &data_size) ||
            reader->offset > reader->size || data_size > reader->size - reader->offset)
            return 0;

        tensor->n_dims = n_dims;
        tensor->type = type;
        tensor->elements = elements;
        tensor->data_size = data_size;
        tensor->dimensions = tensor_dimensions[tensor_index];
        tensor->data = reader->bytes + reader->offset;
        if (!whisper_skip(reader, data_size)) return 0;
        model_info.tensor_count++;
    }

    return model_info.tensor_count > 0;
}

int whisper_model_parse(const void* bytes, size_t size) {
    whisper_model_clear();
    if (!bytes || size < 4 || size > UINT32_MAX) return 0;

    whisper_reader_t reader = {(const uint8_t*)bytes, (uint32_t)size, 0};
    uint32_t magic;
    if (!whisper_read_u32(&reader, &magic) || magic != WHISPER_MODEL_MAGIC ||
        !whisper_read_hparams(&reader, &model_info.hparams) ||
        model_info.hparams.n_audio_state != model_info.hparams.n_text_state ||
        !whisper_read_filters(&reader) || !whisper_read_vocab(&reader) ||
        !whisper_read_tensors(&reader)) {
        whisper_model_clear();
        return 0;
    }

    model_info.bytes = (const uint8_t*)bytes;
    model_info.size = (uint32_t)size;
    model_info.tensors = model_tensors;
    model_is_ready = 1;
    return 1;
}

int whisper_model_init(void) {
    if (!whisper_model_loaded || whisper_model_end <= whisper_model_start ||
        whisper_model_end - whisper_model_start > UINT32_MAX) return 0;
    return whisper_model_parse((const void*)whisper_model_start,
        (size_t)(whisper_model_end - whisper_model_start));
}

int whisper_model_ready(void) {
    return model_is_ready;
}

const whisper_model_info_t* whisper_model_info(void) {
    return model_is_ready ? &model_info : 0;
}

const whisper_tensor_t* whisper_model_find_tensor(const char* name) {
    if (!model_is_ready || !name) return 0;
    for (uint32_t index = 0; index < model_info.tensor_count; index++) {
        const char* left = model_tensors[index].name;
        const char* right = name;
        while (*left && *right && *left == *right) {
            left++;
            right++;
        }
        if (!*left && !*right) return &model_tensors[index];
    }
    return 0;
}

const char* whisper_model_token(uint32_t token, uint32_t* length) {
    if (length) *length = 0;
    if (!model_is_ready || token >= model_info.vocab_count) return 0;
    if (length) *length = token_lengths[token];
    return (const char*)(model_info.bytes + token_offsets[token]);
}

static float whisper_f16_to_f32(uint16_t half) {
    uint32_t sign = (uint32_t)(half & 0x8000U) << 16;
    uint32_t exponent = (half >> 10) & 0x1FU;
    uint32_t mantissa = half & 0x03FFU;
    uint32_t bits;

    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            int32_t shift = 0;
            while ((mantissa & 0x0400U) == 0) {
                mantissa <<= 1;
                shift++;
            }
            mantissa &= 0x03FFU;
            bits = sign | ((uint32_t)(127 - 14 - shift) << 23) | (mantissa << 13);
        }
    } else if (exponent == 0x1FU) {
        bits = sign | 0x7F800000U | (mantissa << 13);
    } else {
        bits = sign | ((exponent + 112U) << 23) | (mantissa << 13);
    }

    union {
        uint32_t bits;
        float value;
    } converted = {bits};
    return converted.value;
}

int whisper_tensor_value(const whisper_tensor_t* tensor, uint64_t index, float* value) {
    if (!tensor || !value || !tensor->data || index >= tensor->elements) return 0;
    if (tensor->type == 0) {
        const uint8_t* source = tensor->data + index * sizeof(float);
        uint32_t bits = (uint32_t)source[0] | ((uint32_t)source[1] << 8) |
            ((uint32_t)source[2] << 16) | ((uint32_t)source[3] << 24);
        union {
            uint32_t bits;
            float value;
        } converted = {bits};
        *value = converted.value;
        return 1;
    }
    if (tensor->type == 1) {
        const uint8_t* source = tensor->data + index * sizeof(uint16_t);
        uint16_t half = (uint16_t)source[0] | ((uint16_t)source[1] << 8);
        *value = whisper_f16_to_f32(half);
        return 1;
    }
    return 0;
}

int whisper_tensor_matvec(const whisper_tensor_t* tensor, const float* input,
    uint32_t input_count, float* output, uint32_t output_count) {
    if (!tensor || !input || !output || !tensor->dimensions || tensor->n_dims != 2 ||
        tensor->dimensions[0] != input_count || tensor->dimensions[1] != output_count ||
        tensor->elements != (uint64_t)input_count * output_count) return 0;

    for (uint32_t row = 0; row < output_count; row++) {
        float sum = 0.0f;
        uint64_t offset = (uint64_t)row * input_count;
#if defined(__SSE2__)
        __m128 accumulator = _mm_setzero_ps();
        uint32_t column = 0;
        if (tensor->type == 0) {
            const float* weights = (const float*)tensor->data + offset;
            for (; column + 4 <= input_count; column += 4) {
                __m128 weight_values = _mm_loadu_ps(weights + column);
                __m128 input_values = _mm_loadu_ps(input + column);
                accumulator = _mm_add_ps(accumulator,
                    _mm_mul_ps(weight_values, input_values));
            }
        } else {
            for (; column + 4 <= input_count; column += 4) {
                float weight_values[4];
                for (uint32_t lane = 0; lane < 4; lane++) {
                    if (!whisper_tensor_value(tensor, offset + column + lane,
                            &weight_values[lane])) return 0;
                }
                __m128 weights = _mm_loadu_ps(weight_values);
                __m128 inputs = _mm_loadu_ps(input + column);
                accumulator = _mm_add_ps(accumulator, _mm_mul_ps(weights, inputs));
            }
        }
        float partial_sums[4];
        _mm_storeu_ps(partial_sums, accumulator);
        sum = partial_sums[0] + partial_sums[1] + partial_sums[2] + partial_sums[3];
        for (; column < input_count; column++) {
            float weight;
            if (!whisper_tensor_value(tensor, offset + column, &weight)) return 0;
            sum += weight * input[column];
        }
#else
        for (uint32_t column = 0; column < input_count; column++) {
            float weight;
            if (!whisper_tensor_value(tensor, offset + column, &weight)) return 0;
            sum += weight * input[column];
        }
#endif
        output[row] = sum;
    }
    return 1;
}

int whisper_tensor_linear_sequence(const whisper_tensor_t* tensor,
    const whisper_tensor_t* bias, const float* input, uint32_t frame_count,
    uint32_t input_width, float* output, uint32_t output_width) {
    if (!tensor || !input || !output || tensor->n_dims != 2 ||
        tensor->dimensions[0] != input_width || tensor->dimensions[1] != output_width ||
        (bias && bias->elements != output_width)) return 0;

    for (uint32_t frame = 0; frame < frame_count; frame++) {
        const float* frame_input = input + (size_t)frame * input_width;
        float* frame_output = output + (size_t)frame * output_width;
        if (!whisper_tensor_matvec(tensor, frame_input, input_width,
                frame_output, output_width)) return 0;
        if (bias) {
            for (uint32_t index = 0; index < output_width; index++) {
                float offset;
                if (!whisper_tensor_value(bias, index, &offset)) return 0;
                frame_output[index] += offset;
            }
        }
    }
    return 1;
}

int whisper_tensor_conv1d(const whisper_tensor_t* weights,
    const whisper_tensor_t* bias, const float* input, uint32_t input_frames,
    uint32_t input_channels, uint32_t stride, uint32_t padding,
    float* output, uint32_t output_capacity, uint32_t* output_frames) {
    if (!weights || !input || !output || !output_frames || !weights->dimensions ||
        weights->n_dims != 3 || weights->dimensions[1] != input_channels ||
        stride == 0 || (bias && bias->elements != weights->dimensions[2])) return 0;

    uint32_t kernel = weights->dimensions[0];
    uint32_t output_channels = weights->dimensions[2];
    uint64_t padded_frames = (uint64_t)input_frames + 2ULL * padding;
    if (padded_frames < kernel) return 0;
    uint32_t frames = (uint32_t)((padded_frames - kernel) / stride + 1);
    if ((uint64_t)frames * output_channels > output_capacity) return 0;

    for (uint32_t frame = 0; frame < frames; frame++) {
        for (uint32_t out_channel = 0; out_channel < output_channels; out_channel++) {
            float sum = 0.0f;
            if (bias && !whisper_tensor_value(bias, out_channel, &sum)) return 0;
            for (uint32_t in_channel = 0; in_channel < input_channels; in_channel++) {
                for (uint32_t tap = 0; tap < kernel; tap++) {
                    int64_t input_frame = (int64_t)frame * stride + tap - padding;
                    if (input_frame < 0 || (uint64_t)input_frame >= input_frames) continue;
                    uint64_t weight_index = ((uint64_t)out_channel * input_channels +
                        in_channel) * kernel + tap;
                    float weight;
                    if (!whisper_tensor_value(weights, weight_index, &weight)) return 0;
                    sum += weight * input[(size_t)in_channel * input_frames + input_frame];
                }
            }
            output[(size_t)out_channel * frames + frame] = sum;
        }
    }
    *output_frames = frames;
    return 1;
}

int whisper_layer_norm_f32(const float* input, float* output,
    const whisper_tensor_t* weight, const whisper_tensor_t* bias,
    uint32_t count, float epsilon) {
    if (!input || !output || !weight || !bias || count == 0 || epsilon <= 0.0f ||
        weight->elements != count || bias->elements != count) return 0;

    float mean = 0.0f;
    for (uint32_t index = 0; index < count; index++) mean += input[index];
    mean /= (float)count;

    float variance = 0.0f;
    for (uint32_t index = 0; index < count; index++) {
        float difference = input[index] - mean;
        variance += difference * difference;
    }
    variance /= (float)count;
    float inverse_stddev = 1.0f / sqrtf(variance + epsilon);

    for (uint32_t index = 0; index < count; index++) {
        float scale;
        float offset;
        if (!whisper_tensor_value(weight, index, &scale) ||
            !whisper_tensor_value(bias, index, &offset)) return 0;
        output[index] = (input[index] - mean) * inverse_stddev * scale + offset;
    }
    return 1;
}

int whisper_layer_norm_sequence_f32(const float* input, float* output,
    const whisper_tensor_t* weight, const whisper_tensor_t* bias,
    uint32_t frame_count, uint32_t width, float epsilon) {
    if (!input || !output) return 0;
    for (uint32_t frame = 0; frame < frame_count; frame++) {
        if (!whisper_layer_norm_f32(input + (size_t)frame * width,
                output + (size_t)frame * width, weight, bias, width, epsilon)) return 0;
    }
    return 1;
}

int whisper_attention_f32(const float* query, const float* key, const float* value,
    uint32_t query_frames, uint32_t key_frames, uint32_t width,
    uint32_t head_count, int causal, float* output) {
    if (!query || !key || !value || !output || query_frames == 0 || key_frames == 0 ||
        width == 0 || head_count == 0 || width % head_count != 0 ||
        key_frames > WHISPER_MEL_FRAMES) return 0;

    uint32_t head_width = width / head_count;
    float scale = 1.0f / sqrtf((float)head_width);
    for (uint32_t head = 0; head < head_count; head++) {
        uint32_t head_offset = head * head_width;
        for (uint32_t query_frame = 0; query_frame < query_frames; query_frame++) {
            uint32_t allowed_keys = causal && query_frames == key_frames
                ? query_frame + 1 : key_frames;
            float maximum = -1.0e30f;
            for (uint32_t key_frame = 0; key_frame < allowed_keys; key_frame++) {
                float score = 0.0f;
                const float* query_row = query + (size_t)query_frame * width + head_offset;
                const float* key_row = key + (size_t)key_frame * width + head_offset;
                for (uint32_t dim = 0; dim < head_width; dim++)
                    score += query_row[dim] * key_row[dim];
                score *= scale;
                attention_scores[key_frame] = score;
                if (score > maximum) maximum = score;
            }

            float denominator = 0.0f;
            for (uint32_t key_frame = 0; key_frame < allowed_keys; key_frame++) {
                attention_scores[key_frame] = whisper_exp_negative(
                    attention_scores[key_frame] - maximum);
                denominator += attention_scores[key_frame];
            }
            if (denominator <= 0.0f) return 0;

            float* output_row = output + (size_t)query_frame * width + head_offset;
            for (uint32_t dim = 0; dim < head_width; dim++) {
                float sum = 0.0f;
                for (uint32_t key_frame = 0; key_frame < allowed_keys; key_frame++) {
                    sum += (attention_scores[key_frame] / denominator) *
                        value[(size_t)key_frame * width + head_offset + dim];
                }
                output_row[dim] = sum;
            }
        }
    }
    return 1;
}

static void whisper_make_layer_tensor_name(char* output, uint32_t capacity,
    uint32_t layer, const char* suffix) {
    const char* prefix = "encoder.blocks.";
    uint32_t length = 0;
    while (*prefix && length + 1 < capacity) output[length++] = *prefix++;

    char digits[10];
    uint32_t digit_count = 0;
    do {
        digits[digit_count++] = (char)('0' + layer % 10);
        layer /= 10;
    } while (layer && digit_count < sizeof(digits));
    while (digit_count && length + 1 < capacity) output[length++] = digits[--digit_count];

    while (*suffix && length + 1 < capacity) output[length++] = *suffix++;
    output[length] = '\0';
}

static const whisper_tensor_t* whisper_encoder_layer_tensor(uint32_t layer,
    const char* suffix) {
    char name[WHISPER_TENSOR_NAME_MAX];
    whisper_make_layer_tensor_name(name, sizeof(name), layer, suffix);
    return whisper_model_find_tensor(name);
}

static const whisper_tensor_t* whisper_decoder_layer_tensor(uint32_t layer,
    const char* suffix) {
    char name[WHISPER_TENSOR_NAME_MAX];
    const char* prefix = "decoder.blocks.";
    uint32_t length = 0;
    while (*prefix && length + 1 < sizeof(name)) name[length++] = *prefix++;
    char digits[10];
    uint32_t digit_count = 0;
    do {
        digits[digit_count++] = (char)('0' + layer % 10);
        layer /= 10;
    } while (layer && digit_count < sizeof(digits));
    while (digit_count && length + 1 < sizeof(name)) name[length++] = digits[--digit_count];
    while (*suffix && length + 1 < sizeof(name)) name[length++] = *suffix++;
    name[length] = '\0';
    return whisper_model_find_tensor(name);
}

static int whisper_init_decoder_layers(void) {
    if (model_info.hparams.n_text_layer != 4 ||
        model_info.hparams.n_text_state != WHISPER_TINY_STATE ||
        model_info.hparams.n_text_head != 6) return 0;

    for (uint32_t layer = 0; layer < 4; layer++) {
        whisper_decoder_layer_t* weights = &decoder_layers[layer];
        weights->attn_norm_weight = whisper_decoder_layer_tensor(layer, ".attn_ln.weight");
        weights->attn_norm_bias = whisper_decoder_layer_tensor(layer, ".attn_ln.bias");
        weights->attn_query_weight = whisper_decoder_layer_tensor(layer, ".attn.query.weight");
        weights->attn_query_bias = whisper_decoder_layer_tensor(layer, ".attn.query.bias");
        weights->attn_key_weight = whisper_decoder_layer_tensor(layer, ".attn.key.weight");
        weights->attn_value_weight = whisper_decoder_layer_tensor(layer, ".attn.value.weight");
        weights->attn_value_bias = whisper_decoder_layer_tensor(layer, ".attn.value.bias");
        weights->attn_out_weight = whisper_decoder_layer_tensor(layer, ".attn.out.weight");
        weights->attn_out_bias = whisper_decoder_layer_tensor(layer, ".attn.out.bias");
        weights->cross_norm_weight = whisper_decoder_layer_tensor(layer, ".cross_attn_ln.weight");
        weights->cross_norm_bias = whisper_decoder_layer_tensor(layer, ".cross_attn_ln.bias");
        weights->cross_query_weight = whisper_decoder_layer_tensor(layer, ".cross_attn.query.weight");
        weights->cross_query_bias = whisper_decoder_layer_tensor(layer, ".cross_attn.query.bias");
        weights->cross_key_weight = whisper_decoder_layer_tensor(layer, ".cross_attn.key.weight");
        weights->cross_value_weight = whisper_decoder_layer_tensor(layer, ".cross_attn.value.weight");
        weights->cross_value_bias = whisper_decoder_layer_tensor(layer, ".cross_attn.value.bias");
        weights->cross_out_weight = whisper_decoder_layer_tensor(layer, ".cross_attn.out.weight");
        weights->cross_out_bias = whisper_decoder_layer_tensor(layer, ".cross_attn.out.bias");
        weights->mlp_norm_weight = whisper_decoder_layer_tensor(layer, ".mlp_ln.weight");
        weights->mlp_norm_bias = whisper_decoder_layer_tensor(layer, ".mlp_ln.bias");
        weights->mlp_in_weight = whisper_decoder_layer_tensor(layer, ".mlp.0.weight");
        weights->mlp_in_bias = whisper_decoder_layer_tensor(layer, ".mlp.0.bias");
        weights->mlp_out_weight = whisper_decoder_layer_tensor(layer, ".mlp.2.weight");
        weights->mlp_out_bias = whisper_decoder_layer_tensor(layer, ".mlp.2.bias");

        const whisper_tensor_t* const* fields = (const whisper_tensor_t* const*)weights;
        for (uint32_t field = 0; field < sizeof(*weights) / sizeof(fields[0]); field++)
            if (!fields[field]) return 0;
    }
    return 1;
}

static int whisper_decoder_add_bias(float* values, const whisper_tensor_t* bias,
    uint32_t count) {
    if (!values || !bias || bias->elements != count) return 0;
    for (uint32_t index = 0; index < count; index++) {
        float value;
        if (!whisper_tensor_value(bias, index, &value)) return 0;
        values[index] += value;
    }
    return 1;
}

static int whisper_prepare_cross_attention(const float* encoded,
    uint32_t encoded_frames) {
    if (!whisper_init_decoder_layers()) return 0;
    for (uint32_t layer = 0; layer < 4; layer++) {
        whisper_decoder_layer_t* weights = &decoder_layers[layer];
        float* key = decoder_cross_key + (size_t)layer *
            WHISPER_TINY_AUDIO_CTX * WHISPER_TINY_STATE;
        float* value = decoder_cross_value + (size_t)layer *
            WHISPER_TINY_AUDIO_CTX * WHISPER_TINY_STATE;
        if (!whisper_tensor_linear_sequence(weights->cross_key_weight, 0,
                encoded, encoded_frames, WHISPER_TINY_STATE, key, WHISPER_TINY_STATE) ||
            !whisper_tensor_linear_sequence(weights->cross_value_weight,
                weights->cross_value_bias, encoded, encoded_frames,
                WHISPER_TINY_STATE, value, WHISPER_TINY_STATE)) return 0;
    }
    return 1;
}

static int whisper_decoder_step(uint32_t token, uint32_t position,
    uint32_t encoded_frames, float* logits) {
    const whisper_tensor_t* token_embedding =
        whisper_model_find_tensor("decoder.token_embedding.weight");
    const whisper_tensor_t* position_embedding =
        whisper_model_find_tensor("decoder.positional_embedding");
    if (!token_embedding || !position_embedding || token >= model_info.hparams.n_vocab ||
        position >= model_info.hparams.n_text_ctx || position >= WHISPER_TINY_TEXT_CTX)
        return 0;

    float* current = encoder_state_b;
    for (uint32_t channel = 0; channel < WHISPER_TINY_STATE; channel++) {
        float token_value;
        float position_value;
        if (!whisper_tensor_value(token_embedding,
                (uint64_t)token * WHISPER_TINY_STATE + channel, &token_value) ||
            !whisper_tensor_value(position_embedding,
                (uint64_t)position * WHISPER_TINY_STATE + channel, &position_value)) return 0;
        current[channel] = token_value + position_value;
    }

    for (uint32_t layer = 0; layer < 4; layer++) {
        whisper_decoder_layer_t* weights = &decoder_layers[layer];
        if (!whisper_layer_norm_f32(current, encoder_norm, weights->attn_norm_weight,
                weights->attn_norm_bias, WHISPER_TINY_STATE, 1.0e-5f) ||
            !whisper_tensor_matvec(weights->attn_query_weight, encoder_norm,
                WHISPER_TINY_STATE, encoder_query, WHISPER_TINY_STATE) ||
            !whisper_decoder_add_bias(encoder_query, weights->attn_query_bias,
                WHISPER_TINY_STATE) ||
            !whisper_tensor_matvec(weights->attn_key_weight, encoder_norm,
                WHISPER_TINY_STATE, encoder_key, WHISPER_TINY_STATE) ||
            !whisper_tensor_matvec(weights->attn_value_weight, encoder_norm,
                WHISPER_TINY_STATE, encoder_value, WHISPER_TINY_STATE) ||
            !whisper_decoder_add_bias(encoder_value, weights->attn_value_bias,
                WHISPER_TINY_STATE)) return 0;

        float* self_key = decoder_self_key + ((size_t)layer *
            WHISPER_TINY_TEXT_CTX + position) * WHISPER_TINY_STATE;
        float* self_value = decoder_self_value + ((size_t)layer *
            WHISPER_TINY_TEXT_CTX + position) * WHISPER_TINY_STATE;
        for (uint32_t channel = 0; channel < WHISPER_TINY_STATE; channel++) {
            self_key[channel] = encoder_key[channel];
            self_value[channel] = encoder_value[channel];
        }

        const float* self_keys = decoder_self_key +
            (size_t)layer * WHISPER_TINY_TEXT_CTX * WHISPER_TINY_STATE;
        const float* self_values = decoder_self_value +
            (size_t)layer * WHISPER_TINY_TEXT_CTX * WHISPER_TINY_STATE;
        if (!whisper_attention_f32(encoder_query, self_keys, self_values, 1,
                position + 1, WHISPER_TINY_STATE, model_info.hparams.n_text_head,
                0, encoder_attention) ||
            !whisper_tensor_matvec(weights->attn_out_weight, encoder_attention,
                WHISPER_TINY_STATE, encoder_vector, WHISPER_TINY_STATE) ||
            !whisper_decoder_add_bias(encoder_vector, weights->attn_out_bias,
                WHISPER_TINY_STATE)) return 0;
        for (uint32_t channel = 0; channel < WHISPER_TINY_STATE; channel++)
            current[channel] += encoder_vector[channel];

        if (!whisper_layer_norm_f32(current, encoder_norm, weights->cross_norm_weight,
                weights->cross_norm_bias, WHISPER_TINY_STATE, 1.0e-5f) ||
            !whisper_tensor_matvec(weights->cross_query_weight, encoder_norm,
                WHISPER_TINY_STATE, encoder_query, WHISPER_TINY_STATE) ||
            !whisper_decoder_add_bias(encoder_query, weights->cross_query_bias,
                WHISPER_TINY_STATE)) return 0;

        const float* cross_keys = decoder_cross_key + (size_t)layer *
            WHISPER_TINY_AUDIO_CTX * WHISPER_TINY_STATE;
        const float* cross_values = decoder_cross_value + (size_t)layer *
            WHISPER_TINY_AUDIO_CTX * WHISPER_TINY_STATE;
        if (!whisper_attention_f32(encoder_query, cross_keys, cross_values, 1,
                encoded_frames, WHISPER_TINY_STATE, model_info.hparams.n_text_head,
                0, encoder_attention) ||
            !whisper_tensor_matvec(weights->cross_out_weight, encoder_attention,
                WHISPER_TINY_STATE, encoder_vector, WHISPER_TINY_STATE) ||
            !whisper_decoder_add_bias(encoder_vector, weights->cross_out_bias,
                WHISPER_TINY_STATE)) return 0;
        for (uint32_t channel = 0; channel < WHISPER_TINY_STATE; channel++)
            current[channel] += encoder_vector[channel];

        if (!whisper_layer_norm_f32(current, encoder_norm, weights->mlp_norm_weight,
                weights->mlp_norm_bias, WHISPER_TINY_STATE, 1.0e-5f) ||
            !whisper_tensor_matvec(weights->mlp_in_weight, encoder_norm,
                WHISPER_TINY_STATE, encoder_hidden, WHISPER_TINY_FF_STATE) ||
            !whisper_decoder_add_bias(encoder_hidden, weights->mlp_in_bias,
                WHISPER_TINY_FF_STATE)) return 0;
        whisper_gelu_f32(encoder_hidden, WHISPER_TINY_FF_STATE);
        if (!whisper_tensor_matvec(weights->mlp_out_weight, encoder_hidden,
                WHISPER_TINY_FF_STATE, encoder_vector, WHISPER_TINY_STATE) ||
            !whisper_decoder_add_bias(encoder_vector, weights->mlp_out_bias,
                WHISPER_TINY_STATE)) return 0;
        for (uint32_t channel = 0; channel < WHISPER_TINY_STATE; channel++)
            current[channel] += encoder_vector[channel];
    }

    const whisper_tensor_t* final_weight = whisper_model_find_tensor("decoder.ln.weight");
    const whisper_tensor_t* final_bias = whisper_model_find_tensor("decoder.ln.bias");
    if (!whisper_layer_norm_f32(current, encoder_norm, final_weight, final_bias,
            WHISPER_TINY_STATE, 1.0e-5f) ||
        !whisper_tensor_matvec(token_embedding, encoder_norm, WHISPER_TINY_STATE,
            logits, model_info.hparams.n_vocab)) return 0;
    return 1;
}

static int whisper_append_token(uint32_t token, char* output, size_t capacity,
    size_t* output_length) {
    uint32_t length;
    const char* token_text = whisper_model_token(token, &length);
    if (!token_text || !output || !output_length) return 0;
    if (*output_length + length >= capacity) return 0;
    for (uint32_t index = 0; index < length; index++)
        output[(*output_length)++] = token_text[index];
    return 1;
}

static uint32_t whisper_repeated_ngram_length(const uint32_t* tokens,
    uint32_t generated_count, uint32_t candidate) {
    for (uint32_t length = 3; length <= 4; length++) {
        if (generated_count + 1 < 3 * length) continue;
        uint32_t current_start = generated_count + 1 - length;
        uint32_t middle_start = current_start - length;
        uint32_t previous_start = middle_start - length;
        uint32_t matched = 1;
        for (uint32_t offset = 0; offset < length; offset++) {
            uint32_t current = current_start + offset == generated_count
                ? candidate : tokens[3 + current_start + offset];
            uint32_t middle = tokens[3 + middle_start + offset];
            uint32_t previous = tokens[3 + previous_start + offset];
            if (current != middle || middle != previous) {
                matched = 0;
                break;
            }
        }
        if (matched) return length;
    }
    return 0;
}

int whisper_decode_audio(const float* encoded, uint32_t encoded_frames,
    char* output, size_t output_capacity) {
    uint32_t start_tick = timer_get_ticks();
    print_string("Whisper: decoder starting...\n");
    if (!model_is_ready || !encoded || encoded_frames == 0 ||
        encoded_frames > WHISPER_TINY_AUDIO_CTX || !output || output_capacity == 0 ||
        model_info.hparams.n_vocab > 60000 || model_info.vocab_count <= WHISPER_TOKEN_EOT ||
        !whisper_prepare_cross_attention(encoded, encoded_frames)) return 0;

    const uint32_t eot_token = WHISPER_TOKEN_EOT;
    output[0] = '\0';

    for (uint32_t index = 0; index < 4 * WHISPER_TINY_TEXT_CTX * WHISPER_TINY_STATE; index++) {
        decoder_self_key[index] = 0.0f;
        decoder_self_value[index] = 0.0f;
    }

    decoder_tokens[0] = WHISPER_TOKEN_SOT;
    decoder_tokens[1] = WHISPER_TOKEN_TRANSCRIBE;
    decoder_tokens[2] = WHISPER_TOKEN_NO_TIMESTAMPS;
    for (uint32_t position = 0; position < 3; position++) {
        if (!whisper_decoder_step(decoder_tokens[position], position,
                encoded_frames, decoder_logits)) return 0;
    }

    size_t output_length = 0;
    size_t output_offsets[WHISPER_MAX_OUTPUT_TOKENS + 1];
    output_offsets[0] = 0;
    uint32_t token_count = 3;
    int completed = 0;
    for (uint32_t generated = 0; generated < WHISPER_MAX_OUTPUT_TOKENS &&
            token_count < WHISPER_TINY_TEXT_CTX; generated++) {
        uint32_t best_token = 0;
        float best_logit = generated == 0 ? -1.0e30f : decoder_logits[eot_token];
        if (generated > 0) best_token = eot_token;
        for (uint32_t token = 0; token < eot_token; token++) {
            if (decoder_logits[token] > best_logit) {
                best_logit = decoder_logits[token];
                best_token = token;
            }
        }
        if (best_token == eot_token) {
            completed = 1;
            break;
        }
        uint32_t repeated_length = whisper_repeated_ngram_length(
            decoder_tokens, generated, best_token);
        if (repeated_length > 0) {
            output_length = output_offsets[generated + 1 - repeated_length];
            break;
        }
        if (best_token >= eot_token ||
            !whisper_append_token(best_token, output, output_capacity, &output_length))
            return 0;
        output_offsets[generated + 1] = output_length;

        decoder_tokens[token_count] = best_token;
        token_count++;
        if (generated + 1 >= WHISPER_MAX_OUTPUT_TOKENS ||
            token_count >= WHISPER_TINY_TEXT_CTX) break;
        if (!whisper_decoder_step(best_token, token_count,
                encoded_frames, decoder_logits)) return 0;
    }

    while (output_length > 0 && output[output_length - 1] == ' ')
        output_length--;
    size_t first = 0;
    while (first < output_length && output[first] == ' ') first++;
    if (first > 0) {
        for (size_t index = first; index < output_length; index++)
            output[index - first] = output[index];
        output_length -= first;
    }
    output[output_length] = '\0';
    whisper_mark_progress("decoder", start_tick);
    return completed || token_count > 3;
}

int whisper_transcribe_pcm(const int16_t* pcm, uint32_t sample_count,
    char* output, size_t output_capacity) {
    if (sample_count > WHISPER_MAX_LIVE_PCM_SAMPLES) {
        print_string("Whisper: clamping audio to a short live-dictation window for runtime safety.\n");
        sample_count = WHISPER_MAX_LIVE_PCM_SAMPLES;
    }
    uint32_t start_tick = timer_get_ticks();
    print_string("Whisper: PCM capture accepted; converting to log-mel...\n");
    uint32_t mel_frames = 0;
    uint32_t encoded_frames = 0;
    if (!whisper_pcm_to_mel(pcm, sample_count, transcription_mel,
            sizeof(transcription_mel) / sizeof(transcription_mel[0]), &mel_frames) ||
        mel_frames == 0) return 0;
    whisper_mark_progress("pcm->mel", start_tick);

    print_string("Whisper: running encoder...\n");
    uint32_t enc_start = timer_get_ticks();
    if (!whisper_encode_audio(transcription_mel, mel_frames, 80,
            &encoded_frames)) return 0;
    whisper_mark_progress("encoder", enc_start);

    return whisper_decode_audio(whisper_encoded_audio(), encoded_frames,
        output, output_capacity);
}

static int whisper_encoder_add_residual(float* destination, const float* residual,
    uint32_t elements) {
    for (uint32_t index = 0; index < elements; index++)
        destination[index] += residual[index];
    return 1;
}

int whisper_encode_audio(const float* mel, uint32_t mel_frames,
    uint32_t mel_channels, uint32_t* encoded_frames) {
    encoder_error = 0;
    uint32_t encoder_start = timer_get_ticks();
    if (!model_is_ready || !mel || !encoded_frames || mel_channels != 80 ||
        mel_frames == 0 || mel_frames > WHISPER_MAX_LIVE_MEL_FRAMES ||
        model_info.hparams.n_audio_state != WHISPER_TINY_STATE ||
        model_info.hparams.n_audio_head != 6 || model_info.hparams.n_audio_layer != 4 ||
        model_info.hparams.n_audio_ctx > WHISPER_TINY_AUDIO_CTX ||
        model_info.hparams.n_mels != mel_channels) {
        encoder_error = 1;
        return 0;
    }

    const whisper_tensor_t* conv1_weights = whisper_model_find_tensor("encoder.conv1.weight");
    const whisper_tensor_t* conv1_bias = whisper_model_find_tensor("encoder.conv1.bias");
    const whisper_tensor_t* conv2_weights = whisper_model_find_tensor("encoder.conv2.weight");
    const whisper_tensor_t* conv2_bias = whisper_model_find_tensor("encoder.conv2.bias");
    const whisper_tensor_t* position = whisper_model_find_tensor("encoder.positional_embedding");
    uint32_t conv1_frames = 0;
    uint32_t conv2_frames = 0;
    if (!conv1_weights || !conv1_bias || !conv2_weights || !conv2_bias || !position) {
        encoder_error = 2;
        return 0;
    }
    if (!whisper_tensor_conv1d(conv1_weights, conv1_bias, mel, mel_frames, mel_channels,
            1, 1, encoder_conv1, WHISPER_MEL_FRAMES * WHISPER_TINY_STATE, &conv1_frames)) {
        encoder_error = 3;
        return 0;
    }
    print_string("Whisper: encoder conv1 complete.\n");

    whisper_gelu_f32(encoder_conv1,
        conv1_frames * WHISPER_TINY_STATE);
    if (!whisper_tensor_conv1d(conv2_weights, conv2_bias, encoder_conv1, conv1_frames,
            WHISPER_TINY_STATE, 2, 1, encoder_conv2,
            WHISPER_TINY_AUDIO_CTX * WHISPER_TINY_STATE, &conv2_frames) ||
        conv2_frames > model_info.hparams.n_audio_ctx) {
        encoder_error = 4;
        return 0;
    }
    print_string("Whisper: encoder conv2 complete.\n");
    whisper_gelu_f32(encoder_conv2, conv2_frames * WHISPER_TINY_STATE);

    for (uint32_t frame = 0; frame < conv2_frames; frame++) {
        for (uint32_t channel = 0; channel < WHISPER_TINY_STATE; channel++) {
            float positional_value;
            uint64_t position_index = (uint64_t)frame * WHISPER_TINY_STATE + channel;
            if (!whisper_tensor_value(position, position_index, &positional_value)) {
                encoder_error = 5;
                return 0;
            }
            encoder_state_a[(size_t)frame * WHISPER_TINY_STATE + channel] =
                encoder_conv2[(size_t)channel * conv2_frames + frame] + positional_value;
        }
    }

    for (uint32_t layer = 0; layer < model_info.hparams.n_audio_layer; layer++) {
        if ((uint32_t)(timer_get_ticks() - encoder_start) > 5000U) {
            print_string("Whisper: encoder watchdog triggered; audio is too long for this kernel path.\n");
            encoder_error = 997;
            return 0;
        }
        print_string("Whisper: encoder block ");
        kprint_dec(layer + 1);
        print_string(" started.\n");
        const whisper_tensor_t* attn_norm_weight = whisper_encoder_layer_tensor(layer, ".attn_ln.weight");
        const whisper_tensor_t* attn_norm_bias = whisper_encoder_layer_tensor(layer, ".attn_ln.bias");
        const whisper_tensor_t* query_weight = whisper_encoder_layer_tensor(layer, ".attn.query.weight");
        const whisper_tensor_t* query_bias = whisper_encoder_layer_tensor(layer, ".attn.query.bias");
        const whisper_tensor_t* key_weight = whisper_encoder_layer_tensor(layer, ".attn.key.weight");
        const whisper_tensor_t* value_weight = whisper_encoder_layer_tensor(layer, ".attn.value.weight");
        const whisper_tensor_t* value_bias = whisper_encoder_layer_tensor(layer, ".attn.value.bias");
        const whisper_tensor_t* attn_out_weight = whisper_encoder_layer_tensor(layer, ".attn.out.weight");
        const whisper_tensor_t* attn_out_bias = whisper_encoder_layer_tensor(layer, ".attn.out.bias");
        const whisper_tensor_t* mlp_norm_weight = whisper_encoder_layer_tensor(layer, ".mlp_ln.weight");
        const whisper_tensor_t* mlp_norm_bias = whisper_encoder_layer_tensor(layer, ".mlp_ln.bias");
        const whisper_tensor_t* mlp_in_weight = whisper_encoder_layer_tensor(layer, ".mlp.0.weight");
        const whisper_tensor_t* mlp_in_bias = whisper_encoder_layer_tensor(layer, ".mlp.0.bias");
        const whisper_tensor_t* mlp_out_weight = whisper_encoder_layer_tensor(layer, ".mlp.2.weight");
        const whisper_tensor_t* mlp_out_bias = whisper_encoder_layer_tensor(layer, ".mlp.2.bias");

        if (!attn_norm_weight || !attn_norm_bias || !query_weight || !query_bias ||
            !key_weight || !value_weight || !value_bias || !attn_out_weight ||
            !attn_out_bias || !mlp_norm_weight || !mlp_norm_bias || !mlp_in_weight ||
            !mlp_in_bias || !mlp_out_weight || !mlp_out_bias) {
            encoder_error = 100 + (int)layer * 10;
            return 0;
        }

        if (!whisper_layer_norm_sequence_f32(encoder_state_a, encoder_norm,
                attn_norm_weight, attn_norm_bias, conv2_frames, WHISPER_TINY_STATE, 1.0e-5f)) {
            encoder_error = 101 + (int)layer * 10;
            return 0;
        }
        if (!whisper_tensor_linear_sequence(query_weight, query_bias, encoder_norm,
                conv2_frames, WHISPER_TINY_STATE, encoder_query, WHISPER_TINY_STATE)) {
            encoder_error = 102 + (int)layer * 10;
            return 0;
        }
        if (!whisper_tensor_linear_sequence(key_weight, 0, encoder_norm,
                conv2_frames, WHISPER_TINY_STATE, encoder_key, WHISPER_TINY_STATE)) {
            encoder_error = 103 + (int)layer * 10;
            return 0;
        }
        if (!whisper_tensor_linear_sequence(value_weight, value_bias, encoder_norm,
                conv2_frames, WHISPER_TINY_STATE, encoder_value, WHISPER_TINY_STATE)) {
            encoder_error = 104 + (int)layer * 10;
            return 0;
        }
        if (!whisper_attention_f32(encoder_query, encoder_key, encoder_value,
                conv2_frames, conv2_frames, WHISPER_TINY_STATE,
                model_info.hparams.n_audio_head, 0, encoder_attention)) {
            encoder_error = 105 + (int)layer * 10;
            return 0;
        }
        if (!whisper_tensor_linear_sequence(attn_out_weight, attn_out_bias,
                encoder_attention, conv2_frames, WHISPER_TINY_STATE,
                encoder_state_b, WHISPER_TINY_STATE)) {
            encoder_error = 106 + (int)layer * 10;
            return 0;
        }

        whisper_encoder_add_residual(encoder_state_b, encoder_state_a,
            conv2_frames * WHISPER_TINY_STATE);

        for (uint32_t frame = 0; frame < conv2_frames; frame++) {
            const float* frame_input = encoder_state_b + (size_t)frame * WHISPER_TINY_STATE;
            if (!whisper_layer_norm_f32(frame_input, encoder_norm +
                    (size_t)frame * WHISPER_TINY_STATE, mlp_norm_weight,
                    mlp_norm_bias, WHISPER_TINY_STATE, 1.0e-5f) ||
                !whisper_tensor_matvec(mlp_in_weight,
                    encoder_norm + (size_t)frame * WHISPER_TINY_STATE,
                    WHISPER_TINY_STATE, encoder_hidden, WHISPER_TINY_FF_STATE)) {
                encoder_error = 107 + (int)layer * 10;
                return 0;
            }

            for (uint32_t index = 0; index < WHISPER_TINY_FF_STATE; index++) {
                float offset;
                if (!whisper_tensor_value(mlp_in_bias, index, &offset)) {
                    encoder_error = 108 + (int)layer * 10;
                    return 0;
                }
                encoder_hidden[index] += offset;
            }
            whisper_gelu_f32(encoder_hidden, WHISPER_TINY_FF_STATE);
            if (!whisper_tensor_matvec(mlp_out_weight, encoder_hidden,
                    WHISPER_TINY_FF_STATE, encoder_vector, WHISPER_TINY_STATE)) {
                encoder_error = 109 + (int)layer * 10;
                return 0;
            }
            for (uint32_t index = 0; index < WHISPER_TINY_STATE; index++) {
                float offset;
                if (!whisper_tensor_value(mlp_out_bias, index, &offset)) {
                    encoder_error = 110 + (int)layer * 10;
                    return 0;
                }
                encoder_state_a[(size_t)frame * WHISPER_TINY_STATE + index] =
                    frame_input[index] + encoder_vector[index] + offset;
            }
        }
        print_string("Whisper: encoder block ");
        kprint_dec(layer + 1);
        print_string(" complete.\n");
    }

    const whisper_tensor_t* final_weight = whisper_model_find_tensor("encoder.ln_post.weight");
    const whisper_tensor_t* final_bias = whisper_model_find_tensor("encoder.ln_post.bias");
    if (!final_weight || !final_bias ||
        !whisper_layer_norm_sequence_f32(encoder_state_a, encoder_norm,
            final_weight, final_bias, conv2_frames, WHISPER_TINY_STATE, 1.0e-5f)) {
        encoder_error = 900;
        return 0;
    }

    for (size_t index = 0; index < (size_t)conv2_frames * WHISPER_TINY_STATE; index++)
        encoder_state_a[index] = encoder_norm[index];
    *encoded_frames = conv2_frames;
    encoded_frame_count = conv2_frames;
    return 1;
}

const float* whisper_encoded_audio(void) {
    return encoded_frame_count > 0 ? encoder_state_a : 0;
}

int whisper_encode_error(void) {
    return encoder_error;
}

void whisper_gelu_f32(float* values, uint32_t count) {
    if (!values) return;
    const float scale = 0.7978845608f;
    for (uint32_t index = 0; index < count; index++) {
        float value = values[index];
        float cubic = value * value * value;
        values[index] = 0.5f * value * (1.0f + tanhf(scale * (value + 0.044715f * cubic)));
    }
}