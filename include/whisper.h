#ifndef WHISPER_H
#define WHISPER_H

#include <stddef.h>
#include <stdint.h>

#define WHISPER_MODEL_MAGIC 0x67676d6cU
#define WHISPER_TOKEN_EOT 50256U
#define WHISPER_TOKEN_SOT 50257U
#define WHISPER_TOKEN_TRANSCRIBE 50358U
#define WHISPER_TOKEN_NO_TIMESTAMPS 50362U
#define WHISPER_MAX_VOCAB 60000
#define WHISPER_MAX_TENSORS 256
#define WHISPER_TENSOR_NAME_MAX 128
#define WHISPER_MAX_LIVE_PCM_SAMPLES 24000U
#define WHISPER_MAX_LIVE_MEL_FRAMES 160U
#define WHISPER_MEL_FRAMES 3000
#define WHISPER_MAX_PCM_SAMPLES 480000

typedef struct {
    uint32_t n_vocab;
    uint32_t n_audio_ctx;
    uint32_t n_audio_state;
    uint32_t n_audio_head;
    uint32_t n_audio_layer;
    uint32_t n_text_ctx;
    uint32_t n_text_state;
    uint32_t n_text_head;
    uint32_t n_text_layer;
    uint32_t n_mels;
    uint32_t ftype;
} whisper_hparams_t;

typedef struct {
    char name[WHISPER_TENSOR_NAME_MAX];
    uint32_t n_dims;
    uint32_t type;
    uint64_t elements;
    uint64_t data_size;
    const uint32_t* dimensions;
    const uint8_t* data;
} whisper_tensor_t;

typedef struct {
    const uint8_t* bytes;
    uint32_t size;
    whisper_hparams_t hparams;
    uint32_t n_filters;
    uint32_t filter_size;
    const float* mel_filters;
    uint32_t vocab_count;
    const uint32_t* vocab_offsets;
    const uint32_t* vocab_lengths;
    uint32_t tensor_count;
    const whisper_tensor_t* tensors;
} whisper_model_info_t;

int whisper_model_parse(const void* bytes, size_t size);
int whisper_model_init(void);
int whisper_model_ready(void);
const whisper_model_info_t* whisper_model_info(void);
const whisper_tensor_t* whisper_model_find_tensor(const char* name);
const char* whisper_model_token(uint32_t token, uint32_t* length);
int whisper_tensor_value(const whisper_tensor_t* tensor, uint64_t index, float* value);
int whisper_tensor_matvec(const whisper_tensor_t* tensor, const float* input,
    uint32_t input_count, float* output, uint32_t output_count);
int whisper_tensor_linear_sequence(const whisper_tensor_t* tensor,
    const whisper_tensor_t* bias, const float* input, uint32_t frame_count,
    uint32_t input_width, float* output, uint32_t output_width);
int whisper_tensor_conv1d(const whisper_tensor_t* weights,
    const whisper_tensor_t* bias, const float* input, uint32_t input_frames,
    uint32_t input_channels, uint32_t stride, uint32_t padding,
    float* output, uint32_t output_capacity, uint32_t* output_frames);
int whisper_layer_norm_f32(const float* input, float* output,
    const whisper_tensor_t* weight, const whisper_tensor_t* bias,
    uint32_t count, float epsilon);
int whisper_layer_norm_sequence_f32(const float* input, float* output,
    const whisper_tensor_t* weight, const whisper_tensor_t* bias,
    uint32_t frame_count, uint32_t width, float epsilon);
int whisper_attention_f32(const float* query, const float* key, const float* value,
    uint32_t query_frames, uint32_t key_frames, uint32_t width,
    uint32_t head_count, int causal, float* output);
void whisper_gelu_f32(float* values, uint32_t count);
int whisper_pcm_to_mel(const int16_t* pcm, uint32_t sample_count,
    float* mel, size_t mel_capacity, uint32_t* frame_count);
int whisper_encode_audio(const float* mel, uint32_t mel_frames,
    uint32_t mel_channels, uint32_t* encoded_frames);
const float* whisper_encoded_audio(void);
int whisper_encode_error(void);
int whisper_decode_audio(const float* encoded, uint32_t encoded_frames,
    char* output, size_t output_capacity);
int whisper_transcribe_pcm(const int16_t* pcm, uint32_t sample_count,
    char* output, size_t output_capacity);

#endif