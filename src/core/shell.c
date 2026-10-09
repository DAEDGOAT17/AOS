#include "screen.h"
#include "string.h"
#include "io.h"
#include <stdbool.h>
#include "shell.h"
#include "core/lisp.h"
#include "kmalloc.h"
#include "task.h"
#include "timer.h"
#include "pci.h"
#include "math.h"
#include "fat32.h"
#include "ata.h"
#include "pmm.h"
#include "ahci.h"
#include "net_stack.h"
#include "lwip/netif.h"
#include "infer.h"
#include "voice.h"

char shell_buffer[256];
int buffer_idx = 0;
#define SHELL_HISTORY_SIZE 16
static char shell_history[SHELL_HISTORY_SIZE][256];
static int shell_history_count;
static int shell_history_position;
static char queued_shell_text[256];
static int queued_shell_text_ready;
static char queued_lisp_app[9];
static int queued_lisp_app_ready;
static char queued_lisp_expression[3001];
static int queued_lisp_expression_ready;
const char* commands[] = {
    "ls", "cd", "cat", "touch", "write", "rm", "mkdir", "rmdir", "clear", "echo", "pwd", "uptime", "diskinfo", "agentreport", "lisp", "apps", "runapp", "help", "ps", "mem", "reboot", "sysinfo", "cpuid", "arch", "pci", "ahci", "ifconfig", "netstat", "ai", "ai_mock", "pktdump", "ping", "voice", "infer", "agent_ctx_set", "agent_ctx_get", "agent_plan", "agent_selfcheck", "agent_task", "ask", NULL
};

typedef struct {
    const char* command;
    const char* source;
    const char* path;
} shell_runtime_program_t;

static const shell_runtime_program_t shell_runtime_programs[] = {
    { "pwd", "pwd:print_current_directory", "/agent/db/runtime_pwd.txt" },
    { "uptime", "uptime:show_uptime", "/agent/db/rtup.txt" },
    { "diskinfo", "diskinfo:show_disk_info", "/agent/db/rtdisk.txt" },
    { "agentreport", "agentreport:show_agent_report", "/agent/db/rtagent.txt" }
};

static const shell_runtime_program_t* shell_runtime_find_source(const char* source) {
    for (uint32_t index = 0; index < sizeof(shell_runtime_programs) / sizeof(shell_runtime_programs[0]); index++) {
        if (strcmp(source, shell_runtime_programs[index].source) == 0) return &shell_runtime_programs[index];
    }
    return NULL;
}

static const shell_runtime_program_t* shell_runtime_find_command(const char* command) {
    for (uint32_t index = 0; index < sizeof(shell_runtime_programs) / sizeof(shell_runtime_programs[0]); index++) {
        if (strcmp(command, shell_runtime_programs[index].command) == 0) return &shell_runtime_programs[index];
    }
    return NULL;
}

static int shell_runtime_program_installed(const char* command) {
    const shell_runtime_program_t* runtime_program = shell_runtime_find_command(command);
    char program[64];
    if (!runtime_program) return 0;
    int fd = fat32_open(runtime_program->path, 'r');
    if (fd < 0) return 0;
    int bytes = fat32_read(fd, program, sizeof(program) - 1);
    fat32_close(fd);
    if (bytes < 0) return 0;
    program[bytes] = '\0';
    return strcmp(program, runtime_program->source) == 0;
}

int shell_install_runtime_command(const char *program) {
    const shell_runtime_program_t* runtime_program;
    int fd;
    if (!program || !(runtime_program = shell_runtime_find_source(program)) || !fat32_is_mounted()) {
        print_string("Runtime command rejected: unsupported program.\n");
        return -1;
    }
    fd = fat32_open(runtime_program->path, 'w');
    if (fd < 0 || fat32_write(fd, runtime_program->source, strlen(runtime_program->source)) != (int)strlen(runtime_program->source)) {
        if (fd >= 0) fat32_close(fd);
        print_string("Runtime command install failed: FAT32 write error.\n");
        return -1;
    }
    fat32_close(fd);
    if (!ata_is_ramdisk() && ata_flush_cache() != 0) {
        print_string("Runtime command install failed: disk cache flush failed.\n");
        return -1;
    }
    if (ata_is_ramdisk()) {
        print_string("Runtime program installed: ");
        print_string(runtime_program->source);
        print_string(" (volatile RAM disk; not reboot-persistent).\n");
    } else {
        print_string("Runtime program installed: ");
        print_string(runtime_program->source);
        print_string(" (saved on persistent FAT32 storage).\n");
    }
    return 0;
}

static int shell_lisp_command_name_valid(const char* name) {
    uint32_t length = strlen(name);
    if (length == 0 || length > 8 || name[0] < 'a' || name[0] > 'z') return 0;
    for (uint32_t index = 0; index < length; index++) {
        char value = name[index];
        if (!((value >= 'a' && value <= 'z') || (value >= '0' && value <= '9'))) return 0;
    }
    for (uint32_t index = 0; commands[index]; index++) {
        if (strcmp(name, commands[index]) == 0) return 0;
    }
    return shell_runtime_find_command(name) == NULL;
}

static void shell_lisp_command_path(const char* name, char* path) {
    strcpy(path, "/agent/lisp/");
    strcat(path, name);
    strcat(path, ".lsp");
}

int shell_install_lisp_command(const char* definition) {
    char name[9];
    char path[32];
    const char* separator;
    uint32_t name_length;
    uint32_t source_length;
    int fd;

    if (!definition || !fat32_is_mounted() || strlen(definition) > 3010) {
        print_string("Lisp command rejected: missing definition or FAT32 mount.\n");
        return -1;
    }
    separator = strchr(definition, '\n');
    if (!separator) {
        print_string("Lisp command rejected: expected name followed by newline and expression.\n");
        return -1;
    }
    name_length = (uint32_t)(separator - definition);
    if (name_length == 0 || name_length >= sizeof(name)) {
        print_string("Lisp command rejected: command name must be 1-8 lowercase letters/digits.\n");
        return -1;
    }
    memcpy(name, definition, name_length);
    name[name_length] = '\0';
    if (!shell_lisp_command_name_valid(name) || !aos_lisp_validate(separator + 1)) {
        print_string("Lisp command rejected: name conflicts or expression is outside the Lisp subset.\n");
        return -1;
    }

    source_length = strlen(separator + 1);
    if (source_length == 0 || source_length > 3000) return -1;
    fat32_mkdir("/agent");
    fat32_mkdir("/agent/lisp");
    shell_lisp_command_path(name, path);
    fd = fat32_open(path, 'w');
    if (fd < 0 || fat32_write(fd, separator + 1, source_length) != (int)source_length) {
        if (fd >= 0) fat32_close(fd);
        print_string("Lisp command install failed: FAT32 write error.\n");
        return -1;
    }
    fat32_close(fd);
    if (!ata_is_ramdisk() && ata_flush_cache() != 0) {
        print_string("Lisp command install failed: storage flush failed.\n");
        return -1;
    }
    print_string("Lisp command installed: ");
    print_string(name);
    print_string(ata_is_ramdisk() ? " (volatile RAM disk).\n" : " (saved on persistent FAT32 storage).\n");
    return 0;
}

static int shell_execute_lisp_command(const char* command, const char* argument) {
    char path[32];
    char source[3001];
    if (!shell_lisp_command_name_valid(command)) return 0;
    shell_lisp_command_path(command, path);
    int fd = fat32_open(path, 'r');
    if (fd < 0) return 0;
    uint32_t size = fat32_get_size(fd);
    if (size == 0 || size >= sizeof(source)) {
        fat32_close(fd);
        print_string("Saved Lisp command rejected: invalid size.\n");
        return 1;
    }
    int bytes = fat32_read(fd, source, sizeof(source) - 1);
    fat32_close(fd);
    if (bytes <= 0) {
        print_string("Saved Lisp command could not be read.\n");
        return 1;
    }
    source[bytes] = '\0';
    if (!aos_lisp_validate(source)) {
        print_string("Saved Lisp command rejected: invalid expression.\n");
        return 1;
    }
    aos_lisp_execute(source, argument);
    return 1;
}

static uint32_t shell_saved_app_count;

static void shell_saved_app_list_callback(const char* name, uint8_t attributes,
                                          uint32_t size, uint32_t cluster) {
    uint32_t length = strlen(name);
    (void)cluster;
    if ((attributes & FAT_ATTR_DIRECTORY) || length < 5 ||
        strcmp(name + length - 4, ".LSP") != 0) return;
    print_string("  ");
    print_string(name);
    print_string("  ");
    kprint_dec(size);
    print_string(" bytes\n");
    shell_saved_app_count++;
}

static void shell_list_saved_apps(void) {
    uint32_t cluster;
    if (!fat32_is_mounted()) {
        print_string("AOS apps: FAT32 storage is unavailable.\n");
        return;
    }
    cluster = fat32_resolve_path("/agent/lisp");
    if (cluster == 0) {
        print_string("AOS apps: no saved Lisp apps yet.\n");
        return;
    }
    shell_saved_app_count = 0;
    print_string("AOS saved Lisp apps:\n");
    if (fat32_list_dir(cluster, shell_saved_app_list_callback) != 0) {
        print_string("  Could not read the app directory.\n");
        return;
    }
    if (shell_saved_app_count == 0) print_string("  (none)\n");
    print_string("Run one with: runapp <name> [argument]\n");
}

static void shell_run_saved_app(char* argument) {
    char name[9];
    char* app_argument;
    uint32_t length = 0;
    if (!argument) {
        print_string("Usage: runapp <name> [argument]\n");
        return;
    }
    while (argument[length] && argument[length] != ' ') length++;
    if (length == 0 || length >= sizeof(name)) {
        print_string("App name must be 1-8 lowercase letters/digits.\n");
        return;
    }
    memcpy(name, argument, length);
    name[length] = '\0';
    for (uint32_t index = 0; index < length; index++) {
        if (name[index] >= 'A' && name[index] <= 'Z') name[index] += 'a' - 'A';
    }
    app_argument = argument + length;
    while (*app_argument == ' ') app_argument++;
    if (!shell_lisp_command_name_valid(name)) {
        print_string("Saved app name is invalid or conflicts with a shell command.\n");
        return;
    }
    if (!shell_execute_lisp_command(name, app_argument))
        print_string("Saved app not found. Run 'apps' to see installed tools.\n");
}

int shell_queue_lisp_app(const char* name) {
    if (queued_lisp_app_ready || !name || !shell_lisp_command_name_valid(name)) return -1;
    strcpy(queued_lisp_app, name);
    queued_lisp_app_ready = 1;
    return 0;
}

int shell_queue_lisp_expression(const char* expression) {
    if (queued_lisp_expression_ready || !expression || strlen(expression) > 3000 ||
        !aos_lisp_validate(expression)) return -1;
    strcpy(queued_lisp_expression, expression);
    queued_lisp_expression_ready = 1;
    return 0;
}

static void shell_runtime_print_agent_file(const char* heading, const char* path) {
    char buffer[257];
    uint32_t total = 0;
    char last_character = '\0';
    int fd;
    print_string(heading);
    print_string(":\n");
    fd = fat32_open(path, 'r');
    if (fd < 0) {
        print_string("  (not saved)\n");
        return;
    }
    int bytes;
    while ((bytes = fat32_read(fd, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytes] = '\0';
        print_string(buffer);
        total += (uint32_t)bytes;
        last_character = buffer[bytes - 1];
    }
    fat32_close(fd);
    if (total == 0) print_string("  (empty)\n");
    else if (last_character != '\n') print_char('\n');
}

static void cmd_uptime(void) {
    uint32_t hours, minutes, seconds;
    timer_get_uptime(&hours, &minutes, &seconds);
    print_string("Uptime: ");
    kprint_dec(hours);
    print_string("h ");
    kprint_dec(minutes);
    print_string("m ");
    kprint_dec(seconds);
    print_string("s\n");
}

static void cmd_diskinfo(void) {
    char label[13];
    if (!fat32_is_mounted()) {
        print_string("Disk info unavailable: FAT32 is not mounted.\n");
        return;
    }
    fat32_get_label(label);
    print_string("Storage: ");
    if (ata_is_ramdisk()) print_string("RAM-backed\n");
    else if (ata_is_ahci()) print_string("AHCI\n");
    else print_string("ATA/IDE PIO\n");
    print_string("Sectors: ");
    kprint_dec(ata_get_sector_count());
    print_string("\nFAT32 volume label: ");
    print_string(label[0] ? label : "(none)");
    print_char('\n');
}

static void cmd_agentreport(void) {
    shell_runtime_print_agent_file("Saved task", "/agent/db/task.txt");
    shell_runtime_print_agent_file("Plan", "/agent/db/plan.txt");
    shell_runtime_print_agent_file("Last result", "/agent/db/result.txt");
    shell_runtime_print_agent_file("Observation", "/agent/db/observe.txt");
}

void shell_queue_text(const char* text) {
    if (!text || queued_shell_text_ready) return;
    uint32_t index = 0;
    while (text[index] && index < sizeof(queued_shell_text) - 1) {
        char value = text[index];
        queued_shell_text[index] =
            value == '\n' || value == '\r' || value == '\t' ? ' ' : value;
        index++;
    }
    queued_shell_text[index] = '\0';
    queued_shell_text_ready = index > 0;
}

static void shell_history_add(const char* command) {
    if (!command || !*command) return;
    if (shell_history_count > 0 &&
        strcmp(shell_history[shell_history_count - 1], command) == 0) {
        shell_history_position = shell_history_count;
        return;
    }

    if (shell_history_count == SHELL_HISTORY_SIZE) {
        for (int index = 1; index < SHELL_HISTORY_SIZE; index++) {
            strcpy(shell_history[index - 1], shell_history[index]);
        }
        shell_history_count--;
    }

    strcpy(shell_history[shell_history_count++], command);
    shell_history_position = shell_history_count;
}

static void shell_history_show(int position) {
    while (buffer_idx > 0) {
        print_char('\b');
        buffer_idx--;
    }
    shell_buffer[0] = '\0';

    if (position < shell_history_count) {
        strcpy(shell_buffer, shell_history[position]);
        buffer_idx = strlen(shell_buffer);
        for (int index = 0; index < buffer_idx; index++) {
            print_char(shell_buffer[index]);
        }
    }
}

// Static variables for filename completion state
static char tc_best_match[64];
static int tc_match_count = 0;
static char tc_prefix[64];
static int tc_prefix_len = 0;
static bool tc_is_dir = false;

// Network Stack Stubs
int stub_tcp_connect(uint32_t server_ip, uint16_t port);
int stub_tcp_send(int socket_id, const char* payload);

void tc_callback(const char* name, uint8_t attr, uint32_t size, uint32_t cluster) {
    (void)size;
    (void)cluster;
    if (strncasecmp(name, tc_prefix, tc_prefix_len) == 0) {
        if (tc_match_count == 0) {
            strcpy(tc_best_match, name);
            tc_is_dir = (attr & FAT_ATTR_DIRECTORY) != 0;
        } else {
            // Find common prefix (case insensitive check, but keep case from tc_best_match or input)
            // Simplified: we'll follow the case of the first match
            int j = 0;
            while (tc_best_match[j] && name[j]) {
                char c1 = tc_best_match[j];
                char c2 = name[j];
                if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
                if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
                if (c1 != c2) break;
                j++;
            }
            tc_best_match[j] = '\0';
            if (tc_is_dir && !(attr & FAT_ATTR_DIRECTORY)) tc_is_dir = false;
        }
        tc_match_count++;
    }
}

static void cmd_ifconfig() {
    set_text_color(MAKE_COLOR(COLOR_LIGHT_CYAN, COLOR_BLACK));
    print_string("Network Interfaces:\n");
    print_string("-------------------\n");
    reset_text_color();
    
    // lwIP keeps a global list of initialized hardware interfaces
    extern struct netif *netif_list;
    struct netif *n = netif_list;
    
    if (!n) {
        set_text_color(MAKE_COLOR(COLOR_LIGHT_RED, COLOR_BLACK));
        print_string("No network interfaces detected. Run hardware scan first.\n");
        reset_text_color();
        return;
    }
    
    while (n != NULL) {
        set_text_color(MAKE_COLOR(COLOR_WHITE, COLOR_BLACK));
        print_string("Interface: ");
        print_char(n->name[0]);
        print_char(n->name[1]);
        print_char('\n');
        
        print_string("  IP Address: ");
        uint32_t ip = n->ip_addr.addr;
        kprint_dec((ip >> 0) & 0xFF); print_char('.');
        kprint_dec((ip >> 8) & 0xFF); print_char('.');
        kprint_dec((ip >> 16) & 0xFF); print_char('.');
        kprint_dec((ip >> 24) & 0xFF); print_char('\n');
        
        print_string("  HW MAC:     ");
        char* hex = "0123456789ABCDEF";
        for (int i=0; i < n->hwaddr_len; i++) {
            print_char(hex[(n->hwaddr[i] >> 4) & 0xF]);
            print_char(hex[n->hwaddr[i] & 0xF]);
            if (i < n->hwaddr_len - 1) print_char(':');
        }
        
        print_string("\n  Link MTU:   ");
        kprint_dec(n->mtu);
        
        print_string("\n  State:      ");
        if (n->flags & NETIF_FLAG_LINK_UP) {
           set_text_color(MAKE_COLOR(COLOR_LIGHT_GREEN, COLOR_BLACK));
           print_string("ONLINE (UP)\n");
        } else {
           set_text_color(MAKE_COLOR(COLOR_RED, COLOR_BLACK));
           print_string("OFFLINE (DOWN)\n");
        }
        reset_text_color();
        print_char('\n');
        n = n->next;
    }
}

static void cmd_netstat() {
    extern uint32_t rtl8169_tx_packets;
    extern uint32_t rtl8169_rx_packets;
    extern uint32_t rtl8169_tx_bytes;
    extern uint32_t rtl8169_rx_bytes;

    extern uint32_t e1000_tx_packets;
    extern uint32_t e1000_rx_packets;
    extern uint32_t e1000_tx_bytes;
    extern uint32_t e1000_rx_bytes;

    set_text_color(MAKE_COLOR(COLOR_LIGHT_CYAN, COLOR_BLACK));
    print_string("Network Statistics (RTL8169):\n");
    print_string("-----------------------------\n");
    reset_text_color();

    print_string("TX Packets: ");
    kprint_dec(rtl8169_tx_packets);
    print_string(" (");
    kprint_dec(rtl8169_tx_bytes);
    print_string(" bytes)\n");

    print_string("RX Packets: ");
    kprint_dec(rtl8169_rx_packets);
    print_string(" (");
    kprint_dec(rtl8169_rx_bytes);
    print_string(" bytes)\n\n");

    set_text_color(MAKE_COLOR(COLOR_LIGHT_CYAN, COLOR_BLACK));
    print_string("Network Statistics (QEMU E1000 Mock):\n");
    print_string("-------------------------------------\n");
    reset_text_color();

    print_string("TX Packets: ");
    kprint_dec(e1000_tx_packets);
    print_string(" (");
    kprint_dec(e1000_tx_bytes);
    print_string(" bytes)\n");

    print_string("RX Packets: ");
    kprint_dec(e1000_rx_packets);
    print_string(" (");
    kprint_dec(e1000_rx_bytes);
    print_string(" bytes)\n");
}

extern uint8_t rtl8169_last_tx_packet[];
extern uint16_t rtl8169_last_tx_len;
extern uint8_t rtl8169_last_rx_packet[];
extern uint16_t rtl8169_last_rx_len;

void ping_request(const char* ip_str);

static void cmd_pktdump() {
    char* hex = "0123456789ABCDEF";
    extern void rtl8169_print_packet_parsed(uint8_t* data, int len);

    set_text_color(MAKE_COLOR(COLOR_LIGHT_CYAN, COLOR_BLACK));
    print_string("Last RTL8169 TX Packet (");
    kprint_dec(rtl8169_last_tx_len);
    print_string(" bytes):\n");
    print_string("------------------------------------------------\n");
    reset_text_color();

    if (rtl8169_last_tx_len > 0) {
        rtl8169_print_packet_parsed(rtl8169_last_tx_packet, rtl8169_last_tx_len);
    }

    for (int i = 0; i < rtl8169_last_tx_len; i++) {
        print_char(hex[(rtl8169_last_tx_packet[i] >> 4) & 0xF]);
        print_char(hex[rtl8169_last_tx_packet[i] & 0xF]);
        print_char(' ');
        if ((i + 1) % 16 == 0) print_char('\n');
    }
    if (rtl8169_last_tx_len == 0) print_string("None.\n");
    else if (rtl8169_last_tx_len % 16 != 0) print_char('\n');

    print_char('\n');

    set_text_color(MAKE_COLOR(COLOR_LIGHT_CYAN, COLOR_BLACK));
    print_string("Last RTL8169 RX Packet (");
    kprint_dec(rtl8169_last_rx_len);
    print_string(" bytes):\n");
    print_string("------------------------------------------------\n");
    reset_text_color();

    if (rtl8169_last_rx_len > 0) {
        rtl8169_print_packet_parsed(rtl8169_last_rx_packet, rtl8169_last_rx_len);
    }

    for (int i = 0; i < rtl8169_last_rx_len; i++) {
        print_char(hex[(rtl8169_last_rx_packet[i] >> 4) & 0xF]);
        print_char(hex[rtl8169_last_rx_packet[i] & 0xF]);
        print_char(' ');
        if ((i + 1) % 16 == 0) print_char('\n');
    }
    if (rtl8169_last_rx_len == 0) print_string("None.\n");
    else if (rtl8169_last_rx_len % 16 != 0) print_char('\n');
}

static void cmd_cpuid() {
    uint32_t eax, ebx, ecx, edx;

    // Leaf 0: Vendor string
    asm volatile("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(0));
    char vendor[13];
    vendor[0]  = ebx & 0xFF; vendor[1]  = (ebx >> 8) & 0xFF;
    vendor[2]  = (ebx >> 16) & 0xFF; vendor[3]  = (ebx >> 24) & 0xFF;
    vendor[4]  = edx & 0xFF; vendor[5]  = (edx >> 8) & 0xFF;
    vendor[6]  = (edx >> 16) & 0xFF; vendor[7]  = (edx >> 24) & 0xFF;
    vendor[8]  = ecx & 0xFF; vendor[9]  = (ecx >> 8) & 0xFF;
    vendor[10] = (ecx >> 16) & 0xFF; vendor[11] = (ecx >> 24) & 0xFF;
    vendor[12] = '\0';

    set_text_color(MAKE_COLOR(COLOR_LIGHT_CYAN, COLOR_BLACK));
    print_string("CPUID Information\n");
    print_string("-----------------\n");
    reset_text_color();
    print_string("CPU Vendor    : "); print_string(vendor); print_char('\n');

    // Leaf 1: Feature flags
    asm volatile("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(1));
    uint32_t family  = ((eax >> 8) & 0xF) + ((eax >> 20) & 0xFF);
    uint32_t model   = ((eax >> 4) & 0xF) | (((eax >> 16) & 0xF) << 4);
    uint32_t stepping = eax & 0xF;
    print_string("Family/Model  : "); kprint_dec(family);
    print_string("/"); kprint_dec(model);
    print_string("  Stepping: "); kprint_dec(stepping); print_char('\n');

    // Check key 64-bit feature bits
    int has_sse2   = (edx >> 26) & 1;
    int has_fpu    = (edx >>  0) & 1;
    int has_apic   = (edx >>  9) & 1;
    int has_avx    = (ecx >> 28) & 1;
    int has_sse4_2 = (ecx >> 20) & 1;

    print_string("Features      : ");
    if (has_fpu)    print_string("FPU ");
    if (has_apic)   print_string("APIC ");
    if (has_sse2)   print_string("SSE2 ");
    if (has_sse4_2) print_string("SSE4.2 ");
    if (has_avx)    print_string("AVX ");
    print_char('\n');

    // Leaf 0x80000001: Check Long Mode (bit 29 of EDX = LM)
    asm volatile("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(0x80000001));
    int has_lm = (edx >> 29) & 1;
    print_string("64-bit (LM)   : ");
    if (has_lm) { set_text_color(MAKE_COLOR(COLOR_LIGHT_GREEN, COLOR_BLACK)); print_string("YES - CPU supports x86_64"); }
    else        { set_text_color(MAKE_COLOR(COLOR_RED, COLOR_BLACK));         print_string("NO  - 32-bit only"); }
    reset_text_color();
    print_char('\n');

    // Leaf 0x80000002–0x80000004: Brand String
    char brand[49];
    uint32_t* b = (uint32_t*)brand;
    uint32_t dummy_b, dummy_c, dummy_d;
    asm volatile("cpuid" : "=a"(b[0]),  "=b"(b[1]),  "=c"(b[2]),  "=d"(b[3])  : "a"(0x80000002));
    asm volatile("cpuid" : "=a"(b[4]),  "=b"(b[5]),  "=c"(b[6]),  "=d"(b[7])  : "a"(0x80000003));
    asm volatile("cpuid" : "=a"(b[8]),  "=b"(b[9]),  "=c"(b[10]), "=d"(b[11]) : "a"(0x80000004));
    (void)dummy_b; (void)dummy_c; (void)dummy_d;
    brand[48] = '\0';
    // Trim leading spaces
    char* bp = brand;
    while (*bp == ' ') bp++;
    print_string("CPU Brand     : "); print_string(bp); print_char('\n');
}

static void cmd_arch() {
    uint32_t eax, ebx, ecx, edx;
    set_text_color(MAKE_COLOR(COLOR_LIGHT_GREEN, COLOR_BLACK));
    print_string("  ___  __  ___  __      _____  _  _   \n");
    print_string(" \\ \\ \\/ _ \\/ __\\/_ \\    / ___/ | || |  \n");
    print_string("  \\ \\/ // / (_ / __/   / /__  / _  |  \n");
    print_string("  /_/\\___/\\___/\\___/   \\___/ /_/ |_|  \n");
    reset_text_color();
    print_char('\n');
    print_string("Architecture : "); set_text_color(MAKE_COLOR(COLOR_LIGHT_CYAN, COLOR_BLACK));
    print_string("x86_64 (64-bit Long Mode)"); reset_text_color(); print_char('\n');
    print_string("Pointer Size : 64-bit (8 bytes)\n");
    print_string("Page Tables  : 4-Level (PML4 → PDPT → PD → PT)\n");
    print_string("Address Space: 48-bit Virtual (256 TB range)\n");
    print_string("Endianness   : Little-Endian\n");
    print_string("Registers    : RAX RBX RCX RDX RSI RDI R8-R15\n");
    print_string("SIMD         : SSE2+\n");

    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000001));
    print_string("Long Mode Bit: ");
    if ((edx >> 29) & 1) { set_text_color(MAKE_COLOR(COLOR_LIGHT_GREEN, COLOR_BLACK)); print_string("SET (Active)"); }
    else                 { set_text_color(MAKE_COLOR(COLOR_RED, COLOR_BLACK));          print_string("NOT SET (ERROR)"); }
    reset_text_color(); print_char('\n');
}

static void cmd_sysinfo() {
    set_text_color(MAKE_COLOR(COLOR_LIGHT_CYAN, COLOR_BLACK));
    print_string("  +--------------------------------------+\n");
    print_string("  |       AOS  --  System Report         |\n");
    print_string("  +--------------------------------------+\n");
    reset_text_color();

    // --- OS Info ---
    set_text_color(MAKE_COLOR(COLOR_LIGHT_BLUE, COLOR_BLACK));
    print_string("  =[ OS ]=================================\n");
    reset_text_color();
    print_string("  Name         : AOS\n");
    print_string("  Arch         : x86_64 (64-bit)\n");
    print_string("  Boot Method  : GRUB Multiboot2 / UEFI\n");
    print_string("  Paging       : 4-Level (PML4)\n");
    print_string("  Networking   : lwIP (Bare-Metal TCP/IP Stack)\n");
    print_string("  AI Shell     : Ollama API Autonomous Intercept\n");

    // --- CPU Info ---
    set_text_color(MAKE_COLOR(COLOR_LIGHT_BLUE, COLOR_BLACK));
    print_string("  =[ CPU ]================================\n");
    reset_text_color();
    uint32_t eax, ebx, ecx, edx;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    char vendor[13];
    vendor[0]=(ebx)&0xFF; vendor[1]=(ebx>>8)&0xFF; vendor[2]=(ebx>>16)&0xFF; vendor[3]=(ebx>>24)&0xFF;
    vendor[4]=(edx)&0xFF; vendor[5]=(edx>>8)&0xFF; vendor[6]=(edx>>16)&0xFF; vendor[7]=(edx>>24)&0xFF;
    vendor[8]=(ecx)&0xFF; vendor[9]=(ecx>>8)&0xFF; vendor[10]=(ecx>>16)&0xFF; vendor[11]=(ecx>>24)&0xFF;
    vendor[12]='\0';
    print_string("  Vendor       : "); print_string(vendor); print_char('\n');

    char brand[49];
    uint32_t* b = (uint32_t*)brand;
    asm volatile("cpuid" : "=a"(b[0]), "=b"(b[1]), "=c"(b[2]), "=d"(b[3])  : "a"(0x80000002));
    asm volatile("cpuid" : "=a"(b[4]), "=b"(b[5]), "=c"(b[6]), "=d"(b[7])  : "a"(0x80000003));
    asm volatile("cpuid" : "=a"(b[8]), "=b"(b[9]), "=c"(b[10]),"=d"(b[11]) : "a"(0x80000004));
    brand[48]='\0';
    char* bp = brand; while (*bp == ' ') bp++;
    print_string("  Model        : "); print_string(bp); print_char('\n');

    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    uint32_t family = ((eax>>8)&0xF) + ((eax>>20)&0xFF);
    uint32_t model_num = ((eax>>4)&0xF) | (((eax>>16)&0xF)<<4);
    print_string("  Family       : "); kprint_dec(family); print_string("  Model: "); kprint_dec(model_num); print_char('\n');
    int has_apic = (edx >> 9) & 1;
    print_string("  APIC         : ");
    if (has_apic) { set_text_color(MAKE_COLOR(COLOR_LIGHT_GREEN, COLOR_BLACK)); print_string("Present"); }
    else          { set_text_color(MAKE_COLOR(COLOR_RED, COLOR_BLACK));         print_string("Not Found"); }
    reset_text_color(); print_char('\n');

    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000001));
    int has_lm = (edx >> 29) & 1;
    print_string("  x86_64 LM    : ");
    if (has_lm) { set_text_color(MAKE_COLOR(COLOR_LIGHT_GREEN, COLOR_BLACK)); print_string("Active"); }
    else        { set_text_color(MAKE_COLOR(COLOR_RED, COLOR_BLACK));         print_string("INACTIVE"); }
    reset_text_color(); print_char('\n');

    // --- Memory ---
    set_text_color(MAKE_COLOR(COLOR_LIGHT_BLUE, COLOR_BLACK));
    print_string("  =[ MEMORY ]=============================\n");
    reset_text_color();
    uint32_t total_mb = pmm_get_total_memory_kb() / 1024;
    uint32_t used_kb  = pmm_get_used_blocks() * 4;
    uint32_t free_kb  = pmm_get_free_blocks() * 4;
    print_string("  Total RAM    : "); kprint_dec(total_mb); print_string(" MB\n");
    print_string("  Used         : "); kprint_dec(used_kb / 1024); print_string(" MB\n");
    print_string("  Free         : "); kprint_dec(free_kb / 1024); print_string(" MB\n");

    uint32_t uptime_h, uptime_m, uptime_s;
    timer_get_uptime(&uptime_h, &uptime_m, &uptime_s);
    set_text_color(MAKE_COLOR(COLOR_LIGHT_BLUE, COLOR_BLACK));
    print_string("  =[ UPTIME ]=============================\n");
    reset_text_color();
    print_string("  ");
    kprint_dec(uptime_h); print_string("h ");
    kprint_dec(uptime_m); print_string("m ");
    kprint_dec(uptime_s); print_string("s\n");
}

void shell_execute(char* cmd) {
    // Trim leading whitespace
    while (*cmd == ' ') cmd++;
    if (*cmd == '\0') return;

    // Trim trailing whitespace
    int len = strlen(cmd);
    while (len > 0 && cmd[len - 1] == ' ') {
        cmd[len - 1] = '\0';
        len--;
    }

    // Split command and argument
    char* arg = NULL;
    for (int i = 0; cmd[i] != '\0'; i++) {
        if (cmd[i] == ' ') {
            cmd[i] = '\0';
            arg = &cmd[i + 1];
            while (*arg == ' ') arg++;
            if (*arg == '\0') arg = NULL;
            break;
        }
    }

    // Remove trailing slashes from arg if it's a path (except root "/")
    if (arg) {
        int arg_len = strlen(arg);
        while (arg_len > 1 && arg[arg_len - 1] == '/') {
            arg[arg_len - 1] = '\0';
            arg_len--;
        }
    }

    // Execute commands
    if (strcmp(cmd, "ls") == 0) {
        if (!arg) {
            fat32_ls(0);
        } else {
            uint32_t cluster = fat32_resolve_path(arg);
            if (cluster != 0) fat32_ls(cluster);
            else print_string("Directory not found!\n");
        }
        return;
    }
    else if (strcmp(cmd, "cd") == 0) {
        if (!arg) { print_string("Usage: cd <path>\n"); return; }
        if (fat32_chdir(arg) != 0) {
            print_string("Directory not found!\n");
        }
        return;
    }
    else if (strcmp(cmd, "pwd") == 0) {
        if (arg) {
            print_string("Usage: pwd\n");
        } else if (!shell_runtime_program_installed("pwd")) {
            print_string("pwd is not installed. Install it through the AOS runtime JIT first.\n");
        } else {
            fat32_print_cwd();
            print_char('\n');
        }
        return;
    }
    else if (strcmp(cmd, "uptime") == 0) {
        if (arg) print_string("Usage: uptime\n");
        else if (!shell_runtime_program_installed("uptime")) {
            print_string("uptime is not installed. Install it through the AOS runtime JIT first.\n");
        } else {
            cmd_uptime();
        }
        return;
    }
    else if (strcmp(cmd, "diskinfo") == 0) {
        if (arg) print_string("Usage: diskinfo\n");
        else if (!shell_runtime_program_installed("diskinfo")) {
            print_string("diskinfo is not installed. Install it through the AOS runtime JIT first.\n");
        } else {
            cmd_diskinfo();
        }
        return;
    }
    else if (strcmp(cmd, "agentreport") == 0) {
        if (arg) print_string("Usage: agentreport\n");
        else if (!shell_runtime_program_installed("agentreport")) {
            print_string("agentreport is not installed. Install it through the AOS runtime JIT first.\n");
        } else {
            cmd_agentreport();
        }
        return;
    }
    else if (strcmp(cmd, "lisp") == 0) {
        if (!arg) {
            print_string("Usage: lisp <expression>\n");
        } else if (!aos_lisp_validate(arg)) {
            print_string("Lisp expression rejected: unsupported form or invalid syntax.\n");
        } else {
            aos_lisp_execute(arg, "");
        }
        return;
    }
    else if (strcmp(cmd, "apps") == 0) {
        if (arg) print_string("Usage: apps\n");
        else shell_list_saved_apps();
        return;
    }
    else if (strcmp(cmd, "runapp") == 0) {
        shell_run_saved_app(arg);
        return;
    }
    else if (strcmp(cmd, "cat") == 0) {
        if (!arg) { print_string("Usage: cat <file>\n"); return; }
        int fd = fat32_open(arg, 'r');
        if (fd >= 0) {
            char buf[513];
            int bytes;
            while ((bytes = fat32_read(fd, buf, 512)) > 0) {
                buf[bytes] = '\0';
                print_string(buf);
            }
            fat32_close(fd);
            print_char('\n');
        } else {
            print_string("File not found!\n");
        }
        return;
    }
    else if (strcmp(cmd, "touch") == 0) {
        if (!arg) { print_string("Usage: touch <file>\n"); return; }
        int fd = fat32_open(arg, 'w');
        if (fd >= 0) {
            fat32_close(fd);
            print_string("File created.\n");
        } else {
            print_string("Failed to create file.\n");
        }
        return;
    }
    else if (strcmp(cmd, "write") == 0) {
        if (!arg) { print_string("Usage: write <file> <text>\n"); return; }
        char* space = strstr(arg, " ");
        if (space) {
            *space = '\0';
            char* file = arg;
            char* text = space + 1;
            int fd = fat32_open(file, 'w');
            if (fd >= 0) {
                fat32_write(fd, text, strlen(text));
                fat32_close(fd);
                print_string("File written.\n");
            } else {
                print_string("Failed to write to file.\n");
            }
        } else {
            print_string("Usage: write <file> <text>\n");
        }
        return;
    }
    else if (strcmp(cmd, "rm") == 0) {
        if (!arg) { print_string("Usage: rm <file>\n"); return; }
        if (fat32_unlink(arg) == 0) {
            print_string("File removed.\n");
        } else {
            print_string("Could not remove file.\n");
        }
        return;
    }
    else if (strcmp(cmd, "mkdir") == 0) {
        if (!arg) { print_string("Usage: mkdir <dir>\n"); return; }
        if (fat32_mkdir(arg) == 0) {
            print_string("Directory created.\n");
        } else {
            print_string("Could not create directory.\n");
        }
        return;
    }
    else if (strcmp(cmd, "rmdir") == 0) {
        if (!arg) { print_string("Usage: rmdir <dir>\n"); return; }
        if (fat32_rmdir(arg) == 0) {
            print_string("Directory removed.\n");
        } else {
            print_string("Could not remove directory.\n");
        }
        return;
    }
    else if (strcmp(cmd, "clear") == 0) {
        clear_screen();
        return;
    }
    else if (strcmp(cmd, "echo") == 0) {
        if (arg) print_string(arg);
        print_char('\n');
        return;
    }
    else if (strcmp(cmd, "help") == 0) {
        print_string("AOS shell commands (arguments in <> are required; [] are optional):\n\n");
        print_string("FILES AND PROMPT\n");
        print_string("  ls [path]                  List files in the current directory or path\n");
        print_string("  cd <path>                  Change the current directory\n");
        print_string("  cat <file>                 Display a file\n");
        print_string("  touch <file>               Create an empty file\n");
        print_string("  write <file> <text>        Replace a file's contents with text\n");
        print_string("  rm <file>                  Remove a file\n");
        print_string("  mkdir <dir>                Create a directory\n");
        print_string("  rmdir <dir>                Remove a directory\n");
        print_string("  echo <text>                Print text; echo with no text prints a blank line\n");
        print_string("  pwd                        Show current path (enabled by runtime install)\n");
        print_string("  uptime                     Show uptime (enabled by runtime install)\n");
        print_string("  diskinfo                   Show storage type, sectors, and FAT32 label\n");
        print_string("  agentreport                Show saved agent task audit (runtime install)\n");
        print_string("  lisp <expression>          Evaluate a bounded Lisp expression\n");
        print_string("  apps                       List saved AOS Lisp apps and tools\n");
        print_string("  runapp <name> [argument]   Run a saved Lisp app directly\n");
        print_string("  Lisp memory: (memory-set \"key\" \"value\"), (memory-get \"key\")\n");
        print_string("  Lisp tools: (tool-save \"name\" \"(print 42)\"), tool-read, tool-run\n");
        print_string("  Saved Lisp tools use /agent/lisp/<name>.lsp and are validated before run\n");
        print_string("  (kernel-stub \"driver_init C subset\") runs one whitelisted JIT action\n");
        print_string("  clear                      Clear the screen\n");
        print_string("  help                       Show this command list\n\n");
        print_string("SYSTEM AND HARDWARE\n");
        print_string("  sysinfo                    Show system information\n");
        print_string("  arch                       Show CPU architecture status\n");
        print_string("  cpuid                      Show CPU identification details\n");
        print_string("  mem                        Show physical memory and heap usage\n");
        print_string("  ps                         List tasks\n");
        print_string("  pci <storage|network|audio> Scan PCI devices by class\n");
        print_string("  ahci                       Scan/init the AHCI controller\n");
        print_string("  reboot                     Restart AOS (destructive)\n\n");
        print_string("NETWORK\n");
        print_string("  ifconfig                   Show network interfaces and addresses\n");
        print_string("  netstat                    Show network statistics\n");
        print_string("  ping <ip>                  Send an ICMP echo request\n");
        print_string("  ai <ip> <prompt>           Send a prompt to an Ollama server\n");
        print_string("  pktdump [on|off]           Show last packet; optionally toggle live dump\n\n");
        print_string("AI, AGENT, AND VOICE\n");
        print_string("  infer <prompt>             Classify a prompt with the local inference engine\n");
        print_string("  ai_mock                    Test the AI command intercept\n");
        print_string("  ask <goal>                 Let the AOS agent use OS tools to complete a task\n");
        print_string("  agent_task <instruction>   Start a bounded OS agent task\n");
        print_string("                             Workspace writes are limited to /agent/work\n");
        print_string("  agent_plan <instruction>   Save a bounded self-improvement plan\n");
        print_string("  agent_selfcheck            Check saved agent task status\n");
        print_string("  agent_complete             Mark the current agent task complete\n");
        print_string("  agent_ctx_get <key>        Read an agent context value\n");
        print_string("  agent_ctx_set <key> <value> Set an agent context value\n");
        print_string("  voice                      Show microphone status\n");
        print_string("  voice status               Show microphone status\n");
        print_string("  voice type                 Dictate into the shell prompt\n");
        print_string("  voice meter                Capture microphone level for one second\n");
        print_string("  voice train <label>        Train a voice command label\n");
        print_string("  voice listen               Listen for a voice command\n");
        print_string("  voice infer <prompt>       Classify a spoken-command prompt\n");
        print_string("\nCaution: rm, rmdir, write, and reboot can change or remove data.\n\n");
        return;
    }
    else if (strcmp(cmd, "ps") == 0) {
        task_list();
        return;
    }
    else if (strcmp(cmd, "mem") == 0) {
        uint32_t total_pmem = pmm_get_total_memory_kb();
        print_string("Physical Memory: ");
        kprint_dec(total_pmem / 1024);
        print_string(" MB\n");
        
        uint32_t used_p = pmm_get_used_blocks();
        uint32_t total_p = pmm_get_used_blocks() + pmm_get_free_blocks();
        print_string("Physical Used:   ");
        kprint_dec((used_p * 4096) / 1024);
        print_string(" KB / ");
        kprint_dec((total_p * 4096) / 1024);
        print_string(" KB\n");

        heap_stats_t stats;
        kmalloc_get_stats(&stats);
        print_string("Kernel Heap:    ");
        kprint_dec(stats.used_size / 1024);
        print_string(" KB / ");
        kprint_dec(stats.total_size / 1024);
        print_string(" KB\n\n");
        return;
    }
    else if (strcmp(cmd, "reboot") == 0) {
        print_string("Rebooting...\n");
        sys_reboot();
        return;
    }
    else if (strcmp(cmd, "sysinfo") == 0) {
        cmd_sysinfo();
        return;
    }
    else if (strcmp(cmd, "cpuid") == 0) {
        cmd_cpuid();
        return;
    }
    else if (strcmp(cmd, "arch") == 0) {
        cmd_arch();
        return;
    }
    else if (strcmp(cmd, "pci") == 0) {
        if (!arg) {
            print_string("Usage: pci <storage|network|audio>\n");
        } else if (strcmp(arg, "storage") == 0) {
            pci_scan_storage();
        } else if (strcmp(arg, "network") == 0) {
            pci_scan_network();
        } else if (strcmp(arg, "audio") == 0) {
            pci_scan_multimedia();
        } else {
            print_string("Unknown PCI class. Valid: storage, network, audio\n");
        }
        return;
    }
    else if (strcmp(cmd, "ahci") == 0) {
        pci_init_ahci();
        return;
    }
    else if (strcmp(cmd, "ai") == 0) {
        if (!arg) {
            print_string("Usage: ai <ip> <prompt>\n");
        } else {
            char* space = strstr(arg, " ");
            if (space) {
                *space = '\0';
                char* ip_str = arg;
                char* prompt = space + 1;
                while (*prompt == ' ') prompt++;
                if (*prompt == '\0') {
                    print_string("Usage: ai <ip> <prompt>\n");
                    return;
                }
                print_string("Networking: Connecting to Cognitive Core (AI) at ");
                print_string(ip_str);
                print_string("...\n");
                ollama_request(ip_str, prompt);
                // The timer IRQ (100 Hz) now drives rtl8169_poll() and
                // sys_check_timeouts() every 10 ms — no spin-poll needed here.
                print_string("Network: Request queued. Waiting for Ollama response...\n");
            } else {
                print_string("Usage: ai <ip> <prompt>\n");
            }
        }
        return;
    }
    else if (strcmp(cmd, "ai_mock") == 0) {
        ollama_mock_intercept();
        return;
    }
    else if (strcmp(cmd, "ifconfig") == 0) {
        cmd_ifconfig();
        return;
    }
    else if (strcmp(cmd, "netstat") == 0) {
        cmd_netstat();
        return;
    }
    else if (strcmp(cmd, "ping") == 0) {
        if (!arg) {
            print_string("Usage: ping <ip>\n");
        } else {
            extern void ping_request(const char* ip_str);
            ping_request(arg);
        }
        return;
    }
    else if (strcmp(cmd, "voice") == 0) {
        extern void voice_train(const char* label);
        extern void voice_listen(void);
        extern int hda_mic_ready(void);
        extern int hda_mic_start(void);
        extern void hda_mic_stop(void);
        extern uint32_t hda_mic_read(int16_t* samples, uint32_t capacity);
        extern uint32_t hda_mic_dropped(void);

        if (arg && strncmp(arg, "train ", 6) == 0) {
            char* label = arg + 6;
            while (*label == ' ') label++;
            if (*label) {
                voice_train(label);
                return;
            }
        }
        if (arg && strcmp(arg, "listen") == 0) {
            voice_listen();
            return;
        }
        if (arg && strcmp(arg, "type") == 0) {
            voice_type();
            return;
        }
        if (arg && strncmp(arg, "infer ", 6) == 0) {
            char* prompt = arg + 6;
            while (*prompt == ' ') prompt++;
            if (*prompt) {
                infer_result_t decision;
                if (infer_execute_prompt(prompt, &decision) == 0) {
                    print_string("Voice infer: ");
                    print_string(prompt);
                    print_string(" => ");
                    print_string(infer_action_name(decision.action));
                    print_string("\n");
                    return;
                }
            }
            print_string("Usage: voice infer <prompt>\n");
            return;
        }
        if (arg && strcmp(arg, "infer") == 0) {
            print_string("Usage: voice infer <prompt>\n");
            return;
        }
        if (!arg || strcmp(arg, "status") == 0) {
            print_string(hda_mic_ready() ? "Voice: HDA microphone ready.\n" : "Voice: HDA microphone unavailable. Run with --audio.\n");
            return;
        }
        if (strcmp(arg, "meter") != 0) {
            print_string("Usage: voice [type|status|meter|train <label>|listen|infer <prompt>]\n");
            return;
        }
        if (!hda_mic_start()) {
            print_string("Voice ERROR: microphone is not ready.\n");
            return;
        }

        print_string("Voice: speak now; capturing for one second...\n");
        uint32_t start_tick = timer_get_ticks();
        uint32_t deadline = start_tick + 300;
        uint32_t sample_count = 0;
        uint32_t peak_level = 0;
        uint32_t clipped_samples = 0;
        uint64_t amplitude_sum = 0;
        int16_t samples[256];
        while (sample_count < 16000 && (int32_t)(timer_get_ticks() - deadline) < 0) {
            uint32_t request_count = 16000 - sample_count;
            if (request_count > 256) request_count = 256;
            uint32_t count = hda_mic_read(samples, request_count);
            for (uint32_t index = 0; index < count; index++) {
                int32_t sample = samples[index];
                uint32_t level = (uint32_t)(sample < 0 ? -sample : sample);
                if (level > peak_level) peak_level = level;
                if (level >= 32000) clipped_samples++;
                amplitude_sum += level;
            }
            sample_count += count;
            if (count == 0) asm volatile("hlt");
        }
        hda_mic_stop();
        print_string("Voice capture samples: ");
        kprint_dec(sample_count);
        print_string(" peak: ");
        kprint_dec(peak_level);
        print_string(" mean: ");
        kprint_dec(sample_count ? (uint32_t)(amplitude_sum / sample_count) : 0);
        print_string(" clipped: ");
        kprint_dec(clipped_samples);
        print_string(" ticks: ");
        kprint_dec(timer_get_ticks() - start_tick);
        print_string(" dropped: ");
        kprint_dec(hda_mic_dropped());
        print_string("\n");
        return;
    }
    else if (strcmp(cmd, "infer") == 0) {
        if (!arg) {
            print_string("Usage: infer <prompt>\n");
            return;
        }
        infer_result_t result;
        infer_decide(arg, &result);
        print_string("Inference: action=");
        print_string(infer_action_name(result.action));
        print_string(" confidence=");
        kprint_dec((uint32_t)(result.confidence * 100.0f));
        print_string("% safe=");
        print_string(result.safe ? "yes" : "no");
        print_string(" reason=");
        print_string(result.reason);
        print_string("\n");
        return;
    }
    else if (strcmp(cmd, "agent_ctx_set") == 0) {
        if (!arg) { print_string("Usage: agent_ctx_set <key> <value>\n"); return; }
        char* space = strstr(arg, " ");
        if (space) {
            *space = '\0';
            char* key = arg;
            char* value = space + 1;
            extern void agent_ctx_set(const char* k, const char* v);
            agent_ctx_set(key, value);
        } else {
            print_string("Usage: agent_ctx_set <key> <value>\n");
        }
        return;
    }
    else if (strcmp(cmd, "agent_ctx_get") == 0) {
        if (!arg) { print_string("Usage: agent_ctx_get <key>\n"); return; }
        extern void agent_ctx_get(const char* k);
        agent_ctx_get(arg);
        return;
    }
    else if (strcmp(cmd, "agent_plan") == 0) {
        if (!arg) { print_string("Usage: agent_plan <instruction>\n"); return; }
        extern void agent_plan(const char* inst);
        agent_plan(arg);
        return;
    }
    else if (strcmp(cmd, "agent_complete") == 0) {
        extern void agent_complete_task(void);
        agent_complete_task();
        return;
    }
    else if (strcmp(cmd, "agent_selfcheck") == 0) {
        extern void agent_selfcheck(void);
        agent_selfcheck();
        return;
    }
    else if (strcmp(cmd, "ask") == 0 || strcmp(cmd, "agent_task") == 0) {
        if (!arg) { print_string("Usage: ask <goal>\n"); return; }
        extern void agent_task(const char* inst);
        agent_task(arg);
        return;
    }
    else if (strcmp(cmd, "pktdump") == 0) {
        if (!arg) {
            cmd_pktdump();
        } else if (strcmp(arg, "on") == 0) {
            extern int rtl8169_live_pktdump;
            rtl8169_live_pktdump = 1;
            set_text_color(MAKE_COLOR(COLOR_LIGHT_GREEN, COLOR_BLACK));
            print_string("Live continuous packet dump ENABLED.\n");
            reset_text_color();
        } else if (strcmp(arg, "off") == 0) {
            extern int rtl8169_live_pktdump;
            rtl8169_live_pktdump = 0;
            set_text_color(MAKE_COLOR(COLOR_RED, COLOR_BLACK));
            print_string("Live continuous packet dump DISABLED.\n");
            reset_text_color();
        } else {
            print_string("Usage: pktdump [on|off]\n");
        }
        return;
    }
    else {
        if (!shell_execute_lisp_command(cmd, arg)) {
            print_string("Unknown command. Type 'help' for assistance.\n");
        }
    }
}

//shell input function
void shell_input(char c) {
    if (c == SHELL_KEY_UP) {
        if (shell_history_position > 0) {
            shell_history_position--;
            shell_history_show(shell_history_position);
        }
        return;
    }

    if (c == SHELL_KEY_DOWN) {
        if (shell_history_position < shell_history_count) {
            shell_history_position++;
            shell_history_show(shell_history_position);
        }
        return;
    }

    if (c == 0x03) { // Ctrl + C (ASCII ETX)
        set_text_color(MAKE_COLOR(COLOR_LIGHT_RED, COLOR_BLACK));
        print_string("^C\n");
        reset_text_color();
        buffer_idx = 0;
        shell_buffer[0] = '\0';
        
        // Reset any live streams
        extern int rtl8169_live_pktdump;
        rtl8169_live_pktdump = 0;
        shell_history_position = shell_history_count;

        set_text_color(MAKE_COLOR(COLOR_LIGHT_GREEN, COLOR_BLACK));
        print_string("AOS [");
        set_text_color(MAKE_COLOR(COLOR_LIGHT_BLUE, COLOR_BLACK));
        fat32_print_cwd();
        set_text_color(MAKE_COLOR(COLOR_LIGHT_GREEN, COLOR_BLACK));
        print_string("] $ ");
        reset_text_color();
        return;
    }

    if (c == '\n') {
        shell_buffer[buffer_idx] = '\0';
        shell_history_add(shell_buffer);
        print_char('\n');
        shell_execute(shell_buffer);
        set_text_color(MAKE_COLOR(COLOR_LIGHT_GREEN, COLOR_BLACK));
        print_string("AOS [");
        set_text_color(MAKE_COLOR(COLOR_LIGHT_BLUE, COLOR_BLACK));
        fat32_print_cwd();
        set_text_color(MAKE_COLOR(COLOR_LIGHT_GREEN, COLOR_BLACK));
        print_string("] $ ");
        reset_text_color();
        buffer_idx = 0;
        shell_buffer[0] = '\0';
        if (queued_shell_text_ready) {
            queued_shell_text_ready = 0;
            for (uint32_t index = 0; queued_shell_text[index] && buffer_idx < 255; index++)
                shell_input(queued_shell_text[index]);
            queued_shell_text[0] = '\0';
        }
        return;
    }

    if (c == '\b' && buffer_idx > 0) {
        buffer_idx--;
        print_char('\b');
        print_char(' ');
        print_char('\b');
        return;
    }

    if (c == '\t') {
        // Tab completion logic
        char* last_word = shell_buffer;
        int last_space_idx = -1;
        for (int i = 0; i < buffer_idx; i++) {
            if (shell_buffer[i] == ' ') {
                last_word = &shell_buffer[i + 1];
                last_space_idx = i;
            }
        }

        char prefix[64];
        int prefix_len = buffer_idx - (last_space_idx + 1);
        if (prefix_len > 63) prefix_len = 63;
        memcpy(prefix, last_word, prefix_len);
        prefix[prefix_len] = '\0';

        if (last_word == shell_buffer) {
            // Complete command
            int match_count = 0;
            char common_prefix[32];
            strcpy(common_prefix, "");

            for (int i = 0; commands[i] != NULL; i++) {
                if (strncmp(commands[i], prefix, prefix_len) == 0) {
                    if (match_count == 0) {
                        strcpy(common_prefix, commands[i]);
                    } else {
                        int j = 0;
                        while (common_prefix[j] && commands[i][j] && common_prefix[j] == commands[i][j]) j++;
                        common_prefix[j] = '\0';
                    }
                    match_count++;
                }
            }

            if (match_count > 0) {
                // One or more matches found
                for (int i = prefix_len; common_prefix[i] != '\0'; i++) {
                    shell_input(common_prefix[i]);
                }
                if (match_count == 1) {
                    shell_input(' ');
                }
            }
        } else {
            // Complete filename/directory
            // For now, only complete if the command is one that takes paths
            // Identify the command (first word)
            int first_space = -1;
            for (int i = 0; i < buffer_idx; i++) {
                if (shell_buffer[i] == ' ') { first_space = i; break; }
            }
            
            char cmd_name[32];
            int cmd_len = 0;
            if (first_space == -1) cmd_len = (buffer_idx < 31) ? buffer_idx : 31;
            else cmd_len = (first_space < 31) ? first_space : 31;
            
            memcpy(cmd_name, shell_buffer, cmd_len);
            cmd_name[cmd_len] = '\0';

            bool takes_path = false;
            if (strcmp(cmd_name, "ls") == 0 || strcmp(cmd_name, "cd") == 0 || 
                strcmp(cmd_name, "cat") == 0 || strcmp(cmd_name, "rm") == 0 ||
                strcmp(cmd_name, "mkdir") == 0 || strcmp(cmd_name, "rmdir") == 0 ||
                strcmp(cmd_name, "touch") == 0 || strcmp(cmd_name, "write") == 0) {
                takes_path = true;
            }

            if (takes_path) {
                tc_match_count = 0;
                tc_prefix_len = prefix_len;
                strcpy(tc_prefix, prefix);
                strcpy(tc_best_match, "");

                fat32_list_dir(fat32_cwd_cluster, tc_callback);

                if (tc_match_count > 0) {
                    for (int i = tc_prefix_len; tc_best_match[i] != '\0'; i++) {
                        shell_input(tc_best_match[i]);
                    }
                    if (tc_match_count == 1) {
                        shell_input(tc_is_dir ? '/' : ' ');
                    }
                }
            }
        }
        return;
    }

    if (buffer_idx < 255 && c >= ' ') {
        shell_buffer[buffer_idx++] = c;
        set_text_color(MAKE_COLOR(COLOR_WHITE, COLOR_BLACK));
        print_char(c);
        reset_text_color();
    }
}

extern char kbd_get(void);

void shell_task(void) {
    static bool welcomed = false;
    if (!welcomed) {
        set_text_color(MAKE_COLOR(COLOR_LIGHT_CYAN, COLOR_BLACK));
        print_string("\nAOS - AI-Assisted Operating Environment\n");
        reset_text_color();
        print_string("Type 'help' for a list of commands.\n\n");
        set_text_color(MAKE_COLOR(COLOR_LIGHT_GREEN, COLOR_BLACK));
        print_string("AOS [");
        set_text_color(MAKE_COLOR(COLOR_LIGHT_BLUE, COLOR_BLACK));
        fat32_print_cwd();
        set_text_color(MAKE_COLOR(COLOR_LIGHT_GREEN, COLOR_BLACK));
        print_string("] $ ");
        reset_text_color();
        welcomed = true;
    }

    if (queued_lisp_app_ready) {
        char app_name[sizeof(queued_lisp_app)];
        strcpy(app_name, queued_lisp_app);
        queued_lisp_app[0] = '\0';
        queued_lisp_app_ready = 0;
        shell_execute(app_name);
        print_string("\nAOS [");
        fat32_print_cwd();
        print_string("] $ ");
        return;
    }

    if (queued_lisp_expression_ready) {
        queued_lisp_expression_ready = 0;
        aos_lisp_execute(queued_lisp_expression, "");
        queued_lisp_expression[0] = '\0';
        print_string("\nAOS [");
        fat32_print_cwd();
        print_string("] $ ");
        return;
    }

    // Process background networking hardware loops are now handled by the
    // timer IRQ (timer_handler() at 100 Hz). No manual poll needed here.

    // Process input from buffer (prevents running commands in ISR context)
    char c = kbd_get();
    if (c) {
        shell_input(c);
    }
}
