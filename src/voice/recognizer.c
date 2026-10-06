#include "voice.h"
#include "hda_audio.h"
#include "fat32.h"
#include "timer.h"
#include "screen.h"
#include "string.h"

#define VOICE_SAMPLE_COUNT 16000
#define VOICE_FRAME_SIZE 256
#define VOICE_FRAME_HOP 160
#define VOICE_MAX_FRAMES 100
#define VOICE_FEATURES 9
#define VOICE_LABEL_COUNT 4
#define VOICE_MAGIC 0x31454356
#define VOICE_ACCEPT_SCORE 260
#define VOICE_MIN_SCORE_MARGIN 24

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t frame_count;
    uint8_t features[VOICE_MAX_FRAMES][VOICE_FEATURES];
} voice_template_t;

typedef struct {
    const char* label;
    const char* path;
    const char* shell_command;
} voice_command_t;

static const voice_command_t voice_commands[VOICE_LABEL_COUNT] = {
    { "help", "/agent/db/VHELP.VOC", "help" },
    { "list", "/agent/db/VLIST.VOC", "ls /" },
    { "status", "/agent/db/VSTAT.VOC", "sysinfo" },
    { "stop", "/agent/db/VSTOP.VOC", NULL }
};

static const int32_t goertzel_coefficients[8] = {
    32610, 32413, 32138, 31581, 30572, 28899, 25330, 18205
};

static int16_t voice_pcm[VOICE_SAMPLE_COUNT];
static uint32_t dtw_previous[VOICE_MAX_FRAMES + 1];
static uint32_t dtw_current[VOICE_MAX_FRAMES + 1];

static int voice_capture(int16_t* samples, uint32_t target_count) {
    if (!samples || !hda_mic_start()) return 0;

    uint32_t captured = 0;
    uint32_t deadline = timer_get_ticks() + 300;
    while (captured < target_count && (int32_t)(timer_get_ticks() - deadline) < 0) {
        uint32_t request_count = target_count - captured;
        if (request_count > 256) request_count = 256;
        uint32_t count = hda_mic_read(samples + captured, request_count);
        captured += count;
        if (count == 0) asm volatile("hlt");
    }

    hda_mic_stop();
    return captured == target_count;
}

static uint16_t voice_extract_features(const int16_t* samples,
                                       uint8_t features[VOICE_MAX_FRAMES][VOICE_FEATURES]) {
    uint16_t frame_count = 0;
    for (uint32_t offset = 0; offset + VOICE_FRAME_SIZE <= VOICE_SAMPLE_COUNT;
         offset += VOICE_FRAME_HOP) {
        if (frame_count >= VOICE_MAX_FRAMES) break;

        uint64_t powers[8];
        uint64_t total_power = 0;
        uint64_t absolute_sum = 0;
        for (uint32_t index = 0; index < VOICE_FRAME_SIZE; index++) {
            int32_t sample = samples[offset + index];
            absolute_sum += (uint32_t)(sample < 0 ? -sample : sample);
        }

        for (int band = 0; band < 8; band++) {
            int64_t previous = 0;
            int64_t previous2 = 0;
            int32_t coefficient = goertzel_coefficients[band];
            for (uint32_t index = 0; index < VOICE_FRAME_SIZE; index++) {
                int32_t sample = samples[offset + index];
                int64_t current = (sample >> 3) +
                    (((int64_t)coefficient * previous) >> 14) - previous2;
                previous2 = previous;
                previous = current;
            }

            int64_t power = previous * previous + previous2 * previous2 -
                (((int64_t)coefficient * previous * previous2) >> 14);
            powers[band] = power > 0 ? (uint64_t)power : 0;
            total_power += powers[band];
        }

        for (int band = 0; band < 8; band++) {
            features[frame_count][band] = total_power
                ? (uint8_t)((powers[band] * 255) / total_power)
                : 0;
        }
        uint64_t mean_absolute = absolute_sum / VOICE_FRAME_SIZE;
        features[frame_count][8] = mean_absolute > 32767
            ? 255
            : (uint8_t)(mean_absolute >> 7);
        frame_count++;
    }
    return frame_count;
}

static const voice_command_t* voice_find_command(const char* label) {
    for (int index = 0; index < VOICE_LABEL_COUNT; index++) {
        if (strcmp(label, voice_commands[index].label) == 0)
            return &voice_commands[index];
    }
    return NULL;
}

static int voice_store_template(const voice_command_t* command,
                                const voice_template_t* model) {
    int fd = fat32_open(command->path, 'w');
    if (fd < 0) return 0;
    int bytes = fat32_write(fd, model, sizeof(*model));
    fat32_close(fd);
    return bytes == (int)sizeof(*model);
}

static int voice_load_template(const voice_command_t* command,
                               voice_template_t* model) {
    int fd = fat32_open(command->path, 'r');
    if (fd < 0) return 0;
    int bytes = fat32_read(fd, model, sizeof(*model));
    fat32_close(fd);
    return bytes == (int)sizeof(*model) && model->magic == VOICE_MAGIC &&
        model->frame_count > 0 && model->frame_count <= VOICE_MAX_FRAMES;
}

void voice_train(const char* label) {
    const voice_command_t* command = voice_find_command(label);
    if (!command) {
        print_string("Voice usage: voice train <help|list|status|stop>\n");
        return;
    }

    print_string("Voice training: say the ");
    print_string(label);
    print_string(" phrase after the prompt.\n");
    if (!voice_capture(voice_pcm, VOICE_SAMPLE_COUNT)) {
        print_string("Voice ERROR: microphone capture timed out.\n");
        return;
    }

    voice_template_t model;
    memset(&model, 0, sizeof(model));
    model.magic = VOICE_MAGIC;
    model.frame_count = voice_extract_features(voice_pcm, model.features);
    if (!model.frame_count || !voice_store_template(command, &model)) {
        print_string("Voice ERROR: could not store the enrolled template.\n");
        return;
    }

    print_string("Voice: enrolled '");
    print_string(label);
    print_string("' locally.\n");
}

static uint32_t voice_frame_distance(const uint8_t* left, const uint8_t* right) {
    uint32_t distance = 0;
    for (int feature = 0; feature < VOICE_FEATURES; feature++) {
        int difference = (int)left[feature] - (int)right[feature];
        distance += (uint32_t)(difference < 0 ? -difference : difference);
    }
    return distance;
}

static uint32_t voice_dtw_score(const voice_template_t* model,
                               const uint8_t features[VOICE_MAX_FRAMES][VOICE_FEATURES],
                               uint16_t frame_count) {
    const uint32_t infinity = 0x3FFFFFFF;
    for (uint16_t column = 0; column <= model->frame_count; column++)
        dtw_previous[column] = column == 0 ? 0 : infinity;

    for (uint16_t row = 1; row <= frame_count; row++) {
        dtw_current[0] = infinity;
        for (uint16_t column = 1; column <= model->frame_count; column++) {
            uint32_t best = dtw_previous[column - 1];
            if (dtw_previous[column] < best) best = dtw_previous[column];
            if (dtw_current[column - 1] < best) best = dtw_current[column - 1];
            dtw_current[column] = best + voice_frame_distance(
                features[row - 1], model->features[column - 1]);
        }
        for (uint16_t column = 0; column <= model->frame_count; column++)
            dtw_previous[column] = dtw_current[column];
    }
    return dtw_previous[model->frame_count] / (frame_count + model->frame_count);
}

void voice_listen(void) {
    print_string("Voice: say one enrolled command after the prompt.\n");
    if (!voice_capture(voice_pcm, VOICE_SAMPLE_COUNT)) {
        print_string("Voice ERROR: microphone capture timed out.\n");
        return;
    }

    uint8_t input_features[VOICE_MAX_FRAMES][VOICE_FEATURES];
    uint16_t frame_count = voice_extract_features(voice_pcm, input_features);
    const voice_command_t* best_command = NULL;
    uint32_t best_score = 0xFFFFFFFF;
    uint32_t second_score = 0xFFFFFFFF;
    voice_template_t model;

    for (int index = 0; index < VOICE_LABEL_COUNT; index++) {
        if (!voice_load_template(&voice_commands[index], &model)) continue;
        uint32_t score = voice_dtw_score(&model, input_features, frame_count);
        if (score < best_score) {
            second_score = best_score;
            best_score = score;
            best_command = &voice_commands[index];
        } else if (score < second_score) {
            second_score = score;
        }
    }

    if (!best_command || best_score > VOICE_ACCEPT_SCORE ||
        (second_score != 0xFFFFFFFF && second_score - best_score < VOICE_MIN_SCORE_MARGIN)) {
        print_string("Voice: no confident command match. Best score: ");
        kprint_dec(best_score);
        print_string(" next: ");
        kprint_dec(second_score);
        print_string("\n");
        return;
    }

    print_string("Voice recognized: ");
    print_string(best_command->label);
    print_string(" score: ");
    kprint_dec(best_score);
    print_string("\n");

    if (best_command->shell_command) {
        char command[16];
        strcpy(command, best_command->shell_command);
        extern void shell_execute(char* cmd);
        shell_execute(command);
    }
}