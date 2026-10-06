#include "hda_audio.h"
#include "pci.h"
#include "vmm.h"
#include "screen.h"
#include "string.h"

#define HDA_REG_GCTL 0x08
#define HDA_REG_STATESTS 0x0E
#define HDA_REG_IC 0x60
#define HDA_REG_IR 0x64
#define HDA_REG_ICS 0x68

#define HDA_STREAM_BASE 0x80
#define HDA_SD_CTL 0x00
#define HDA_SD_STS 0x03
#define HDA_SD_LPIB 0x04
#define HDA_SD_CBL 0x08
#define HDA_SD_LVI 0x0C
#define HDA_SD_FORMAT 0x12
#define HDA_SD_BDLPL 0x18
#define HDA_SD_BDLPU 0x1C

#define HDA_GCTL_RESET 0x01
#define HDA_ICS_BUSY 0x01
#define HDA_ICS_VALID 0x02
#define HDA_CODEC_ADDRESS 0
#define HDA_ADC_NODE 4
#define HDA_MIC_PIN_NODE 5
#define HDA_INPUT_STREAM_TAG 1

#define HDA_CAPTURE_RATE 16000
#define HDA_CAPTURE_BYTES (HDA_CAPTURE_RATE * 2)
#define HDA_SAMPLE_RING_SIZE 16384
#define HDA_MMIO_SIZE 0x4000

typedef struct __attribute__((packed)) {
    uint64_t address;
    uint32_t length;
    uint32_t flags;
} hda_bdl_entry_t;

static volatile uint8_t* hda_mmio;
static hda_bdl_entry_t capture_bdl[1] __attribute__((aligned(128)));
static int16_t capture_dma[HDA_CAPTURE_RATE] __attribute__((aligned(128)));
static int16_t sample_ring[HDA_SAMPLE_RING_SIZE];
static volatile uint32_t ring_write;
static volatile uint32_t ring_read;
static volatile uint32_t dropped_samples;
static uint32_t last_dma_position;
static int controller_ready;
static int capture_active;

static uint16_t hda_read16(uint32_t offset) {
    return *(volatile uint16_t*)(hda_mmio + offset);
}

static uint32_t hda_read32(uint32_t offset) {
    return *(volatile uint32_t*)(hda_mmio + offset);
}

static void hda_write8(uint32_t offset, uint8_t value) {
    *(volatile uint8_t*)(hda_mmio + offset) = value;
}

static void hda_write16(uint32_t offset, uint16_t value) {
    *(volatile uint16_t*)(hda_mmio + offset) = value;
}

static void hda_write32(uint32_t offset, uint32_t value) {
    *(volatile uint32_t*)(hda_mmio + offset) = value;
}

static int hda_wait_clear(uint32_t offset, uint32_t mask) {
    for (volatile uint32_t timeout = 0; timeout < 1000000; timeout++) {
        if ((hda_read32(offset) & mask) == 0) return 1;
    }
    return 0;
}

static int hda_codec_verb(uint8_t node, uint16_t verb, uint16_t payload, int short_verb) {
    if (!hda_wait_clear(HDA_REG_ICS, HDA_ICS_BUSY)) return -1;
    hda_write16(HDA_REG_ICS, HDA_ICS_VALID);

    uint32_t command = ((uint32_t)HDA_CODEC_ADDRESS << 28) | ((uint32_t)node << 20);
    if (short_verb) command |= ((uint32_t)((verb >> 8) & 0x0F) << 16) | payload;
    else command |= ((uint32_t)(verb & 0x0FFF) << 8) | (payload & 0xFF);

    hda_write32(HDA_REG_IC, command);
    hda_write16(HDA_REG_ICS, HDA_ICS_BUSY);
    for (volatile uint32_t timeout = 0; timeout < 1000000; timeout++) {
        uint16_t status = hda_read16(HDA_REG_ICS);
        if ((status & HDA_ICS_BUSY) == 0) {
            if (status & HDA_ICS_VALID) return (int)hda_read32(HDA_REG_IR);
            return -1;
        }
    }
    return -1;
}

static int hda_reset_stream(void) {
    uint32_t control = HDA_STREAM_BASE + HDA_SD_CTL;
    hda_write32(control, 0x01);
    for (volatile uint32_t timeout = 0; timeout < 1000000; timeout++) {
        if (hda_read32(control) & 0x01) break;
    }
    if (!(hda_read32(control) & 0x01)) return 0;
    hda_write32(control, 0);
    for (volatile uint32_t timeout = 0; timeout < 1000000; timeout++) {
        if ((hda_read32(control) & 0x01) == 0) return 1;
    }
    return 0;
}

void hda_audio_init(uint32_t bus, uint32_t device, uint32_t function) {
    uint32_t bar = pci_read_config_dword(bus, device, function, 0x10);
    if (bar == 0 || bar == 0xFFFFFFFF || (bar & 1)) {
        print_string("HDA ERROR: no memory-mapped BAR found.\n");
        return;
    }

    uint64_t base = (uint64_t)(bar & 0xFFFFFFF0);
    if (((bar >> 1) & 0x03) == 0x02) {
        base |= (uint64_t)pci_read_config_dword(bus, device, function, 0x14) << 32;
    }

    uint32_t command = pci_read_config_dword(bus, device, function, 0x04);
    pci_write_config_dword(bus, device, function, 0x04, command | 0x0006);
    for (uint64_t address = base; address < base + HDA_MMIO_SIZE; address += 4096)
        vmm_map_page(address, address);
    hda_mmio = (volatile uint8_t*)base;

    hda_write32(HDA_REG_GCTL, 0);
    for (volatile uint32_t timeout = 0; timeout < 1000000; timeout++) {
        if ((hda_read32(HDA_REG_GCTL) & HDA_GCTL_RESET) == 0) break;
    }
    hda_write32(HDA_REG_GCTL, HDA_GCTL_RESET);
    int reset_complete = 0;
    for (volatile uint32_t timeout = 0; timeout < 1000000; timeout++) {
        if (hda_read32(HDA_REG_GCTL) & HDA_GCTL_RESET) {
            reset_complete = 1;
            break;
        }
    }
    if (!reset_complete) {
        print_string("HDA ERROR: controller reset failed.\n");
        return;
    }

    uint32_t codec_mask = hda_read16(HDA_REG_STATESTS);
    if ((codec_mask & (1U << HDA_CODEC_ADDRESS)) == 0) {
        print_string("HDA ERROR: no codec detected.\n");
        return;
    }

    int vendor_id = hda_codec_verb(0, 0xF00, 0x00, 0);
    if (vendor_id < 0) {
        print_string("HDA ERROR: codec command interface failed.\n");
        return;
    }

    int pin_control = hda_codec_verb(HDA_MIC_PIN_NODE, 0x707, 0x20, 0);
    int format_write = hda_codec_verb(HDA_ADC_NODE, 0x200, 0x0210, 1);
    int stream_write = hda_codec_verb(HDA_ADC_NODE, 0x706, HDA_INPUT_STREAM_TAG << 4, 0);
    int gain_write = hda_codec_verb(HDA_ADC_NODE, 0x300, 0x7004, 1);
    int pin_readback = hda_codec_verb(HDA_MIC_PIN_NODE, 0xF07, 0, 0);
    int format_readback = hda_codec_verb(HDA_ADC_NODE, 0xA00, 0, 0);
    int stream_readback = hda_codec_verb(HDA_ADC_NODE, 0xF06, 0, 0);
    int gain_readback = hda_codec_verb(HDA_ADC_NODE, 0xB00, 0x2000, 1);
    if (pin_control < 0 || format_write < 0 || stream_write < 0 || gain_write < 0 ||
        pin_readback != 0x20 || format_readback != 0x0210 ||
        stream_readback != (HDA_INPUT_STREAM_TAG << 4) ||
        (gain_readback & 0x7F) != 0x04) {
        print_string("HDA ERROR: failed to configure microphone codec.\n");
        print_string("HDA codec readback pin/format/stream/gain: ");
        kprint_hex((uint32_t)pin_readback);
        print_char(' ');
        kprint_hex((uint32_t)format_readback);
        print_char(' ');
        kprint_hex((uint32_t)stream_readback);
        print_char(' ');
        kprint_hex((uint32_t)gain_readback);
        print_string("\n");
        return;
    }

    capture_bdl[0].address = (uint64_t)capture_dma;
    capture_bdl[0].length = HDA_CAPTURE_BYTES;
    capture_bdl[0].flags = 0;

    if (!hda_reset_stream()) {
        print_string("HDA ERROR: input stream reset failed.\n");
        return;
    }

    hda_write32(HDA_STREAM_BASE + HDA_SD_CBL, HDA_CAPTURE_BYTES);
    hda_write16(HDA_STREAM_BASE + HDA_SD_LVI, 0);
    hda_write16(HDA_STREAM_BASE + HDA_SD_FORMAT, 0x0210);
    hda_write32(HDA_STREAM_BASE + HDA_SD_BDLPL, (uint32_t)(uint64_t)capture_bdl);
    hda_write32(HDA_STREAM_BASE + HDA_SD_BDLPU, (uint32_t)((uint64_t)capture_bdl >> 32));
    controller_ready = 1;
    print_string("HDA: microphone codec ready (16 kHz mono PCM).\n");
}

int hda_mic_start(void) {
    if (!controller_ready) return 0;

    if (!hda_reset_stream()) return 0;
    hda_write32(HDA_STREAM_BASE + HDA_SD_CBL, HDA_CAPTURE_BYTES);
    hda_write16(HDA_STREAM_BASE + HDA_SD_LVI, 0);
    hda_write16(HDA_STREAM_BASE + HDA_SD_FORMAT, 0x0210);
    hda_write32(HDA_STREAM_BASE + HDA_SD_BDLPL, (uint32_t)(uint64_t)capture_bdl);
    hda_write32(HDA_STREAM_BASE + HDA_SD_BDLPU, (uint32_t)((uint64_t)capture_bdl >> 32));

    ring_write = 0;
    ring_read = 0;
    dropped_samples = 0;
    last_dma_position = 0;
    memset(capture_dma, 0, sizeof(capture_dma));
    hda_write8(HDA_STREAM_BASE + HDA_SD_STS, 0x1C);
    asm volatile("mfence" ::: "memory");
    hda_write32(HDA_STREAM_BASE + HDA_SD_CTL,
                (HDA_INPUT_STREAM_TAG << 20) | 0x02);
    capture_active = 1;
    return 1;
}

void hda_mic_stop(void) {
    if (!controller_ready) return;
    capture_active = 0;
    hda_write32(HDA_STREAM_BASE + HDA_SD_CTL, HDA_INPUT_STREAM_TAG << 20);
}

void hda_audio_poll(void) {
    if (!controller_ready || !capture_active) return;

    uint32_t position = hda_read32(HDA_STREAM_BASE + HDA_SD_LPIB) % HDA_CAPTURE_BYTES;
    position &= ~1U;
    while (last_dma_position != position) {
        uint32_t sample_index = last_dma_position / 2;
        uint32_t next_write = ring_write;
        if ((uint32_t)(next_write - ring_read) < HDA_SAMPLE_RING_SIZE) {
            sample_ring[next_write % HDA_SAMPLE_RING_SIZE] = capture_dma[sample_index];
            ring_write = next_write + 1;
        } else {
            dropped_samples++;
        }
        last_dma_position = (last_dma_position + 2) % HDA_CAPTURE_BYTES;
    }
}

uint32_t hda_mic_read(int16_t* samples, uint32_t capacity) {
    if (!samples || capacity == 0) return 0;

    uint32_t available = ring_write - ring_read;
    if (available > capacity) available = capacity;
    for (uint32_t index = 0; index < available; index++)
        samples[index] = sample_ring[(ring_read + index) % HDA_SAMPLE_RING_SIZE];
    ring_read += available;
    return available;
}

uint32_t hda_mic_dropped(void) {
    return dropped_samples;
}

int hda_mic_ready(void) {
    return controller_ready;
}