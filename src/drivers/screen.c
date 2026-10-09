#include "screen.h"
#include "io.h"
#include <stdint.h>
#include "font8x8.h"

uint32_t* fb_ptr = 0;
static uint32_t fb_width = 0;
uint32_t fb_height = 0;
uint32_t fb_pitch = 0;
static uint8_t fb_bpp = 0;

/* Current text attribute byte (VGA format: bg<<4 | fg) */
static uint8_t current_attr = 0x07; /* default: light grey on black */

void set_text_color(uint8_t attr) { current_attr = attr; }
void reset_text_color(void)      { current_attr = 0x07; }

/* 16-color CGA/VGA palette (ARGB order matching 32-bpp framebuffer) */
static const uint32_t vga_palette[16] = {
    0x000000, /* 0  Black        */
    0x0000AA, /* 1  Blue         */
    0x00AA00, /* 2  Green        */
    0x00AAAA, /* 3  Cyan         */
    0xAA0000, /* 4  Red          */
    0xAA00AA, /* 5  Magenta      */
    0xAA5500, /* 6  Brown        */
    0xAAAAAA, /* 7  Light Grey   */
    0x555555, /* 8  Dark Grey    */
    0x5555FF, /* 9  Light Blue   */
    0x55FF55, /* A  Light Green  */
    0x55FFFF, /* B  Light Cyan   */
    0xFF5555, /* C  Light Red    */
    0xFF55FF, /* D  Light Magenta*/
    0xFFFF55, /* E  Yellow       */
    0xFFFFFF, /* F  White        */
};

static inline uint32_t attr_fg(uint8_t attr) { return vga_palette[attr & 0x0F]; }
static inline uint32_t attr_bg(uint8_t attr) { return vga_palette[(attr >> 4) & 0x0F]; }

#define MAX_HISTORY 1000
/* Maximum possible grid dimensions (worst-case buffer allocation) */
#define MAX_ROWS 200
#define MAX_COLS 320
#define MARGIN_ROWS 2

/* Runtime grid dimensions — computed in screen_init_fb() from fb resolution */
static int SCREEN_ROWS = 25;
static int SCREEN_COLS = 80;
static int VISIBLE_ROWS = 23; /* SCREEN_ROWS - MARGIN_ROWS */

static int defer_refresh = 0;

static uint16_t history_buffer[MAX_HISTORY][MAX_COLS];
static int total_lines_stored = 0;
static int current_line_idx = 0;
static int current_col = 0;
static int scroll_offset = 0;

int cursor = 0;

static void clear_history_line(int idx) {
    for (int i = 0; i < SCREEN_COLS; i++) {
        history_buffer[idx][i] = (0x07 << 8) | ' ';
    }
}

static uint16_t fb_last_screen[MAX_ROWS][MAX_COLS] = {0};
static int fb_force_redraw = 1;

void screen_init_fb(uint64_t addr, uint32_t width, uint32_t height, uint32_t pitch, uint8_t bpp) {
    fb_ptr = (uint32_t*)addr;
    fb_width = width;
    fb_height = height;
    fb_pitch = pitch;
    fb_bpp = bpp;

    /* Compute grid to exactly fill the framebuffer at 1:1 pixel scale (8x8 font) */
    int cols = (int)(width / 8);
    int rows = (int)(height / 8);
    if (cols < 40) cols = 40;
    if (rows < 10) rows = 10;
    if (cols > MAX_COLS) cols = MAX_COLS;
    if (rows > MAX_ROWS) rows = MAX_ROWS;
    SCREEN_COLS = cols;
    SCREEN_ROWS = rows;
    VISIBLE_ROWS = SCREEN_ROWS - MARGIN_ROWS;
    fb_force_redraw = 1;
}



static void draw_fb_char(int r, int c, uint16_t vga_char, uint32_t fg_color, uint32_t bg_color) {
    if (!fb_ptr) return;

    /* 1:1 scale — each character cell is exactly 8x8 pixels, no centering gaps */
    int ch = vga_char & 0xFF;

    // Map extended characters to ASCII equivalents
    if (ch == 219) ch = '#';
    else if (ch == 179) ch = '|';
    else if (ch > 127) ch = '?';

    uint8_t *glyph = (uint8_t*)&font8x8_basic[ch];
    int bytes_per_pixel = fb_bpp / 8;

    int start_x = c * 8;
    int start_y = r * 8;

    // Guard against out-of-bounds writes
    if (start_x + 8 > (int)fb_width || start_y + 8 > (int)fb_height) return;

    for (int y = 0; y < 8; y++) {
        uint8_t *row_ptr = (uint8_t*)fb_ptr + (start_y + y) * fb_pitch;
        for (int x = 0; x < 8; x++) {
            int set = glyph[y] & (1 << x);
            uint32_t color = set ? fg_color : bg_color;
            uint8_t *pixel = row_ptr + (start_x + x) * bytes_per_pixel;

            if (bytes_per_pixel == 4) {
                *(uint32_t*)pixel = color;
            } else if (bytes_per_pixel == 3) {
                pixel[0] = color & 0xFF;         // B
                pixel[1] = (color >> 8) & 0xFF;  // G
                pixel[2] = (color >> 16) & 0xFF; // R
            }
        }
    }
}

static void refresh_fb_screen() {
    int end_history_total = total_lines_stored - scroll_offset;
    int start_history_total = end_history_total - (VISIBLE_ROWS - 1);
    if (start_history_total < 0) start_history_total = 0;

    for (int r = 0; r < VISIBLE_ROWS; r++) {
        int history_total_idx = start_history_total + r;
        if (history_total_idx <= total_lines_stored) {
            int circular_idx = history_total_idx % MAX_HISTORY;
            for (int c = 0; c < SCREEN_COLS; c++) {
                if (c == SCREEN_COLS - 1) continue;
                uint16_t val = history_buffer[circular_idx][c];
                if (fb_force_redraw || fb_last_screen[r][c] != val) {
                    uint8_t a = (val >> 8) & 0xFF;
                    draw_fb_char(r, c, val, attr_fg(a), attr_bg(a));
                    fb_last_screen[r][c] = val;
                }
            }
        } else {
            for (int c = 0; c < SCREEN_COLS - 1; c++) {
                uint16_t empty_val = (0x07 << 8) | ' ';
                if (fb_force_redraw || fb_last_screen[r][c] != empty_val) {
                    draw_fb_char(r, c, ' ', attr_fg(0x07), attr_bg(0x07));
                    fb_last_screen[r][c] = empty_val;
                }
            }
        }
    }

    for (int r = VISIBLE_ROWS; r < SCREEN_ROWS; r++) {
        for (int c = 0; c < SCREEN_COLS; c++) {
            uint16_t empty_val = (0x07 << 8) | ' ';
            if (fb_force_redraw || fb_last_screen[r][c] != empty_val) {
                draw_fb_char(r, c, ' ', attr_fg(0x07), attr_bg(0x07));
                fb_last_screen[r][c] = empty_val;
            }
        }
    }

    int max_scroll = total_lines_stored - (VISIBLE_ROWS - 1);
    if (max_scroll < 0) max_scroll = 0;

    for (int r = 0; r < VISIBLE_ROWS; r++) {
        int c = SCREEN_COLS - 1;
        uint16_t sb_val;
        uint32_t fg;
        if (max_scroll > 0) {
            int handle_r = (VISIBLE_ROWS - 1) - (scroll_offset * (VISIBLE_ROWS - 1) / max_scroll);
            if (r == handle_r) {
                sb_val = (0x0F << 8) | '#';
                fg = 0xFFFFFF;
            } else {
                sb_val = (0x08 << 8) | '|';
                fg = 0x444444;
            }
        } else {
            sb_val = (0x08 << 8) | '|';
            fg = 0x444444;
        }

        if (fb_force_redraw || fb_last_screen[r][c] != sb_val) {
            draw_fb_char(r, c, sb_val & 0xFF, fg, 0x111111);
            fb_last_screen[r][c] = sb_val;
        }
    }

    fb_force_redraw = 0;
}

void refresh_screen() {
    if (fb_ptr) {
        refresh_fb_screen();
        return;
    }
    
    uint16_t *vga = (uint16_t*)0xB8000;
    
    // The last line to show is total_lines_stored - scroll_offset
    // We want to show VISIBLE_ROWS lines ending at that line
    int end_history_total = total_lines_stored - scroll_offset;
    int start_history_total = end_history_total - (VISIBLE_ROWS - 1);
    if (start_history_total < 0) start_history_total = 0;

    // 1. Render visible history
    for (int r = 0; r < VISIBLE_ROWS; r++) {
        int history_total_idx = start_history_total + r;
        
        if (history_total_idx <= total_lines_stored) {
            // Map total index to circular buffer index
            int circular_idx = history_total_idx % MAX_HISTORY;
            
            for (int c = 0; c < SCREEN_COLS; c++) {
                int vga_idx = r * SCREEN_COLS + c;
                if (c == SCREEN_COLS - 1) continue; // Skip scrollbar col
                vga[vga_idx] = history_buffer[circular_idx][c];
            }
        } else {
            // Fill with empty if we haven't reached this total line yet
            for (int c = 0; c < SCREEN_COLS - 1; c++) {
                vga[r * SCREEN_COLS + c] = (0x07 << 8) | ' ';
            }
        }
    }

    // 2. Clear Margin Rows
    for (int r = VISIBLE_ROWS; r < SCREEN_ROWS; r++) {
        for (int c = 0; c < SCREEN_COLS; c++) {
            vga[r * SCREEN_COLS + c] = (0x07 << 8) | ' ';
        }
    }

    // 3. Render Scrollbar (Col 79)
    int max_scroll = total_lines_stored - (VISIBLE_ROWS - 1);
    if (max_scroll < 0) max_scroll = 0;

    for (int r = 0; r < VISIBLE_ROWS; r++) {
        int vga_idx = r * SCREEN_COLS + (SCREEN_COLS - 1);
        if (max_scroll > 0) {
            int handle_r = (VISIBLE_ROWS - 1) - (scroll_offset * (VISIBLE_ROWS - 1) / max_scroll);
            if (r == handle_r) {
                vga[vga_idx] = (0x0F << 8) | 219; // White block
            } else {
                vga[vga_idx] = (0x08 << 8) | 179; // Dark track
            }
        } else {
            vga[vga_idx] = (0x08 << 8) | 179;
        }
    }
}

void update_cursor() {
    if (fb_ptr) {
        // Optional logic: we can draw a pure white underscore character `_` at the cursor position
        // in refresh_fb_screen if we wanted, but avoiding VGA specific outb calls protects UEFI state
        return; 
    }

    if (scroll_offset > 0) {
        // Hide cursor via hardware registers
        outb(0x3D4, 0x0A);
        outb(0x3D5, 0x20); // Bit 5 hides cursor
        return;
    }

    // Show cursor (set scanline 14-15)
    outb(0x3D4, 0x0A); outb(0x3D5, 0x0E);
    outb(0x3D4, 0x0B); outb(0x3D5, 0x0F);

    // Position calculation
    int end_history_total = total_lines_stored;
    int start_history_total = end_history_total - (VISIBLE_ROWS - 1);
    if (start_history_total < 0) start_history_total = 0;

    int cursor_row = total_lines_stored - start_history_total;
    int pos = cursor_row * SCREEN_COLS + current_col;

    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    
    cursor = pos; // Sync global
}

void clear_screen() {
    for (int i = 0; i < MAX_HISTORY; i++) clear_history_line(i);
    total_lines_stored = 0;
    current_line_idx = 0;
    current_col = 0;
    scroll_offset = 0;
    fb_force_redraw = 1;
    refresh_screen();
    update_cursor();
}

void scroll_up() {
    int max_scroll = total_lines_stored - (VISIBLE_ROWS - 1);
    if (max_scroll < 0) max_scroll = 0;
    
    if (scroll_offset < max_scroll && scroll_offset < MAX_HISTORY - VISIBLE_ROWS) {
        scroll_offset++;
        refresh_screen();
        update_cursor();
    }
}

void scroll_down() {
    if (scroll_offset > 0) {
        scroll_offset--;
        refresh_screen();
        update_cursor();
    }
}

char agent_capture_buf[2048];
int agent_capture_idx = 0;
int agent_capture_active = 0;

void screen_start_capture() {
    agent_capture_idx = 0;
    agent_capture_active = 1;
    agent_capture_buf[0] = '\0';
}

void screen_stop_capture() {
    agent_capture_active = 0;
}

char* screen_get_capture() {
    return agent_capture_buf;
}

size_t screen_copy_current_view(char *buffer, size_t capacity) {
    size_t written = 0;
    int end_history_total = total_lines_stored - scroll_offset;
    int start_history_total = end_history_total - (VISIBLE_ROWS - 1);
    int first_line;
    int copied_lines = 0;

    if (!buffer || capacity == 0) return 0;
    if (start_history_total < 0) start_history_total = 0;
    first_line = end_history_total - 23;
    if (first_line < start_history_total) first_line = start_history_total;

    for (int line_index = first_line;
         line_index <= end_history_total && copied_lines < 24;
         line_index++, copied_lines++) {
        int circular_index = line_index % MAX_HISTORY;
        int line_length = SCREEN_COLS - 1;

        while (line_length > 0 &&
               (history_buffer[circular_index][line_length - 1] & 0xFF) == ' ') {
            line_length--;
        }

        for (int column = 0; column < line_length; column++) {
            unsigned char character = history_buffer[circular_index][column] & 0xFF;
            if (written + 1 >= capacity) goto finished;
            if (character < 32 || character > 126) character = '?';
            buffer[written++] = (char)character;
        }

        if (written + 1 >= capacity) goto finished;
        buffer[written++] = '\n';
    }

finished:
    buffer[written] = '\0';
    return written;
}

void print_char(char c) {
    if (agent_capture_active && agent_capture_idx < 2047) {
        agent_capture_buf[agent_capture_idx++] = c;
        agent_capture_buf[agent_capture_idx] = '\0';
    }
    outb(0x3F8, c);
    if (c == '\b') {
        if (current_col > 0) {
            current_col--;
            history_buffer[current_line_idx][current_col] = (current_attr << 8) | ' ';
        }
    } else if (c == '\n') {
        current_col = 0;
        total_lines_stored++;
        current_line_idx = total_lines_stored % MAX_HISTORY;
        clear_history_line(current_line_idx);
    } else {
        if (current_col < SCREEN_COLS - 1) {
            history_buffer[current_line_idx][current_col] = (current_attr << 8) | c;
            current_col++;
        } else {
            // Auto wrap
            print_char('\n');
            print_char(c);
            return;
        }
    }

    // Always follow output to the bottom if we aren't deferring layout renders
    if (defer_refresh == 0) {
        scroll_offset = 0;
        refresh_screen();
        update_cursor();
    }
}

void print_string(const char* str) {
    defer_refresh++;
    while (*str) print_char(*str++);
    defer_refresh--;
    if (defer_refresh == 0) {
        scroll_offset = 0;
        refresh_screen();
        update_cursor();
    }
}

void print_color_string(const char* str, uint8_t attr) {
    uint8_t saved = current_attr;
    current_attr = attr;
    print_string(str);
    current_attr = saved;
}

void kprint_hex(uint32_t n) {
    defer_refresh++;
    char* hex = "0123456789ABCDEF";
    print_string("0x");
    for (int i = 28; i >= 0; i -= 4) {
        print_char(hex[(n >> i) & 0xF]);
    }
    defer_refresh--;
    if (defer_refresh == 0) {
        scroll_offset = 0;
        refresh_screen();
        update_cursor();
    }
}

void kprint_dec(uint32_t n) {
    if (n == 0) {
        print_char('0');
        return;
    }
    defer_refresh++;
    char buf[11];
    int i = 0;
    while (n > 0) {
        buf[i++] = (n % 10) + '0';
        n /= 10;
    }
    while (--i >= 0) print_char(buf[i]);
    defer_refresh--;
    if (defer_refresh == 0) {
        scroll_offset = 0;
        refresh_screen();
        update_cursor();
    }
}
