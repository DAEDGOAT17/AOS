#ifndef SCREEN_H
#define SCREEN_H

#include <stddef.h>
#include <stdint.h>

/* VGA color attribute nibbles (fg | bg<<4) */
#define COLOR_BLACK         0x0
#define COLOR_BLUE          0x1
#define COLOR_GREEN         0x2
#define COLOR_CYAN          0x3
#define COLOR_RED           0x4
#define COLOR_MAGENTA       0x5
#define COLOR_BROWN         0x6
#define COLOR_LIGHT_GREY    0x7
#define COLOR_DARK_GREY     0x8
#define COLOR_LIGHT_BLUE    0x9
#define COLOR_LIGHT_GREEN   0xA
#define COLOR_LIGHT_CYAN    0xB
#define COLOR_LIGHT_RED     0xC
#define COLOR_LIGHT_MAGENTA 0xD
#define COLOR_YELLOW        0xE
#define COLOR_WHITE         0xF

/* Pack fg+bg into VGA attribute byte */
#define MAKE_COLOR(fg, bg)  (((bg) << 4) | (fg))

void clear_screen();
void screen_show_text(const char* text);
void print_char(char c);
void print_string(const char* str);
void print_color_string(const char* str, uint8_t attr);
void set_text_color(uint8_t attr);
void reset_text_color(void);
void update_cursor();
void kprint_hex(uint32_t n);
void kprint_dec(uint32_t num);
void screen_init_fb(uint64_t addr, uint32_t width, uint32_t height, uint32_t pitch, uint8_t bpp);
extern int cursor;
void scroll_up();
void scroll_down();
size_t screen_copy_current_view(char *buffer, size_t capacity);

#endif