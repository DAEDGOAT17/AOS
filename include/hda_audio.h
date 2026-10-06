#ifndef HDA_AUDIO_H
#define HDA_AUDIO_H

#include <stdint.h>

void hda_audio_init(uint32_t bus, uint32_t device, uint32_t function);
void hda_audio_poll(void);
int hda_mic_start(void);
void hda_mic_stop(void);
uint32_t hda_mic_read(int16_t* samples, uint32_t capacity);
uint32_t hda_mic_dropped(void);
int hda_mic_ready(void);

#endif