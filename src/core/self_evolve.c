#include "core/self_evolve.h"
#include "core/jit_engine.h"
#include "shell.h"
#include "drivers/serial.h"
#include "screen.h"
#include "string.h"
#include "task.h"
#include "lwip/tcp.h"
#include "lwip/netif.h"

extern void shell_execute(char *cmd);
extern void screen_start_capture(void);
extern void screen_stop_capture(void);
extern char *screen_get_capture(void);

#define SELF_EVOLVE_BEGIN "---BEGIN_DRIVER---"
#define SELF_EVOLVE_END   "---END_DRIVER---"
#define SELF_EVOLVE_PORT  9000U
#define SELF_EVOLVE_CMD_PORT 9001U

static char self_evolve_buffer[8192];
static char self_evolve_payload[8192];
static char self_evolve_serial_buffer[8192];
static char self_evolve_serial_payload[8192];
static char self_evolve_screen_snapshot[8192];
static size_t self_evolve_len;
static size_t self_evolve_serial_len;
static struct tcp_pcb *self_evolve_listener = NULL;
static struct tcp_pcb *self_evolve_active_client = NULL;
static struct tcp_pcb *self_evolve_cmd_active_client = NULL;

static int self_evolve_extract_payload(const char *input, char *out, size_t out_size);

static void self_evolve_reset_buffer(void) {
    self_evolve_len = 0;
    self_evolve_buffer[0] = '\0';
    self_evolve_payload[0] = '\0';
}

static void self_evolve_reset_serial_buffer(void) {
    self_evolve_serial_len = 0;
    self_evolve_serial_buffer[0] = '\0';
    self_evolve_serial_payload[0] = '\0';
}

static void self_evolve_emit_manifest(void) {
    serial_write_string("---LAPTOP_MANIFEST_START---\n");
    serial_write_string("OS=AOS\n");
    serial_write_string("ARCH=x86_64\n");
    serial_write_string("BOOT=BIOS/UEFI\n");
    serial_write_string("DISCOVERY=PCI+ACPI+SMBIOS\n");
    serial_write_string("TARGET=SELF_EVOLVE_DRIVER\n");
    serial_write_string("---LAPTOP_MANIFEST_END---\n");
}

static void self_evolve_emit_network_banner(void) {
    print_string("HOST SETUP: listening on ");
    if (netif_default != NULL && netif_default->ip_addr.addr != 0U) {
        print_string(ip4addr_ntoa(&(netif_default->ip_addr)));
    } else {
        print_string("DHCP-pending");
    }
    print_string(":9000\n");
}

static void self_evolve_report_result(const char *result) {
    serial_write_string(result);
    if (self_evolve_active_client != NULL) {
        tcp_write(self_evolve_active_client, result, (u16_t)strlen(result), TCP_WRITE_FLAG_COPY);
        tcp_output(self_evolve_active_client);
    }
}

static void self_evolve_commit_payload(void) {
    if (self_evolve_extract_payload(self_evolve_buffer,
                                    self_evolve_payload,
                                    sizeof(self_evolve_payload))) {
        print_string("\nAOS setup patch received. Compiling...\n");
        if (jit_compile_and_load(self_evolve_payload) == 0) {
            self_evolve_report_result("---RESULT: SUCCESS---\n");
        } else {
            self_evolve_report_result("---RESULT: COMPILE_FAILED---\n");
        }
    } else {
        self_evolve_report_result("---RESULT: MALFORMED_PAYLOAD---\n");
    }

    self_evolve_reset_buffer();
}

static int self_evolve_consume_data(const char *chunk, size_t chunk_len) {
    size_t i;

    if (chunk == NULL || chunk_len == 0U) {
        return 0;
    }

    if (self_evolve_len + chunk_len + 1U >= sizeof(self_evolve_buffer)) {
        self_evolve_reset_buffer();
        self_evolve_report_result("---RESULT: PAYLOAD_TOO_LARGE---\n");
        return 0;
    }

    for (i = 0; i < chunk_len; ++i) {
        self_evolve_buffer[self_evolve_len++] = chunk[i];
    }
    self_evolve_buffer[self_evolve_len] = '\0';

    if (strstr(self_evolve_buffer, SELF_EVOLVE_END) != NULL) {
        self_evolve_commit_payload();
        return 1;
    }

    return 0;
}

static int self_evolve_extract_payload(const char *input, char *out, size_t out_size) {
    const char *begin = strstr(input, SELF_EVOLVE_BEGIN);
    const char *end = strstr(input, SELF_EVOLVE_END);
    const char *payload_start;
    size_t payload_len;

    if (!begin || !end || !out || out_size == 0) {
        return 0;
    }

    payload_start = begin + strlen(SELF_EVOLVE_BEGIN);
    while (*payload_start == '\r' || *payload_start == '\n') {
        payload_start++;
    }

    payload_len = (size_t)(end - payload_start);
    if (payload_len >= out_size) {
        payload_len = out_size - 1;
    }

    memcpy(out, payload_start, payload_len);
    out[payload_len] = '\0';
    return 1;
}

static void self_evolve_poll(void) {
    int ch;

    ch = serial_read_char();
    if (ch < 0) {
        return;
    }

    if (self_evolve_serial_len + 2U >= sizeof(self_evolve_serial_buffer)) {
        self_evolve_reset_serial_buffer();
        serial_write_string("---RESULT: PAYLOAD_TOO_LARGE---\n");
        return;
    }

    self_evolve_serial_buffer[self_evolve_serial_len++] = (char)ch;
    self_evolve_serial_buffer[self_evolve_serial_len] = '\0';

    if (strstr(self_evolve_serial_buffer, SELF_EVOLVE_END) != NULL) {
        if (self_evolve_extract_payload(self_evolve_serial_buffer,
                                        self_evolve_serial_payload,
                                        sizeof(self_evolve_serial_payload))) {
            print_string("\nAOS serial patch received. Compiling...\n");
            if (jit_compile_and_load(self_evolve_serial_payload) == 0) {
                serial_write_string("---RESULT: SUCCESS---\n");
            } else {
                serial_write_string("---RESULT: COMPILE_FAILED---\n");
            }
        } else {
            serial_write_string("---RESULT: MALFORMED_PAYLOAD---\n");
        }
        self_evolve_reset_serial_buffer();
    }
}

static err_t self_evolve_tcp_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    (void)arg;
    if (err != ERR_OK) {
        tcp_close(tpcb);
        if (self_evolve_active_client == tpcb) {
            self_evolve_active_client = NULL;
        }
        return ERR_OK;
    }

    if (p == NULL) {
        tcp_close(tpcb);
        if (self_evolve_active_client == tpcb) {
            self_evolve_active_client = NULL;
        }
        return ERR_OK;
    }

    for (struct pbuf *chunk = p; chunk != NULL; chunk = chunk->next) {
        if (chunk->len > 0U) {
            self_evolve_consume_data((const char *)chunk->payload, (size_t)chunk->len);
        }
    }

    pbuf_free(p);
    return ERR_OK;
}

static err_t self_evolve_tcp_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err) {
    const char *banner = "SELF-EVOLVE: waiting for driver payload...\n";
    (void)arg;

    if (err != ERR_OK || newpcb == NULL) {
        return ERR_VAL;
    }

    self_evolve_active_client = newpcb;
    tcp_recv(newpcb, self_evolve_tcp_recv_cb);
    tcp_err(newpcb, NULL);

    if (tcp_write(newpcb, banner, (u16_t)strlen(banner), TCP_WRITE_FLAG_COPY) != ERR_OK) {
        tcp_close(newpcb);
        self_evolve_active_client = NULL;
        return ERR_OK;
    }
    tcp_output(newpcb);
    return ERR_OK;
}

static void self_evolve_network_listen(void) {
    struct tcp_pcb *listener;

    listener = tcp_new();
    if (listener == NULL) {
        print_string("Hosted setup network listener unavailable; continuing without TCP.\n");
        return;
    }

    if (tcp_bind(listener, IP_ADDR_ANY, SELF_EVOLVE_PORT) != ERR_OK) {
        tcp_close(listener);
        print_string("Hosted setup network listener could not bind to port 9000.\n");
        return;
    }

    self_evolve_listener = tcp_listen(listener);
    if (self_evolve_listener == NULL) {
        tcp_close(listener);
        print_string("Hosted setup network listener failed to start on port 9000.\n");
        return;
    }

    tcp_accept(self_evolve_listener, self_evolve_tcp_accept_cb);
    self_evolve_emit_network_banner();
}

static void self_evolve_exec_remote_command(const char *cmd) {
    char command_copy[256];
    char *trimmed;
    char *output;

    if (cmd == NULL) {
        return;
    }

    strncpy(command_copy, cmd, sizeof(command_copy) - 1);
    command_copy[sizeof(command_copy) - 1] = '\0';

    trimmed = command_copy;
    while (*trimmed == ' ' || *trimmed == '\t' || *trimmed == '\r' || *trimmed == '\n') {
        trimmed++;
    }

    if (*trimmed == '\0') {
        return;
    }

    if (strcmp(trimmed, "screen_dump") == 0) {
        static const char begin[] = "---AOS_SCREEN_BEGIN---\n";
        static const char end[] = "---AOS_SCREEN_END---\n";
        size_t begin_len = sizeof(begin) - 1;
        size_t end_len = sizeof(end) - 1;
        size_t screen_len = screen_copy_current_view(
            self_evolve_screen_snapshot + begin_len,
            sizeof(self_evolve_screen_snapshot) - begin_len - end_len
        );
        size_t response_len = begin_len + screen_len + end_len;

        if (self_evolve_cmd_active_client == NULL) return;
        memcpy(self_evolve_screen_snapshot, begin, begin_len);
        memcpy(self_evolve_screen_snapshot + begin_len + screen_len, end, end_len);
        if (tcp_write(self_evolve_cmd_active_client,
                      self_evolve_screen_snapshot,
                      (u16_t)response_len,
                      TCP_WRITE_FLAG_COPY) == ERR_OK) {
            tcp_output(self_evolve_cmd_active_client);
        }
        return;
    }

    if (strstr(trimmed, "reboot") != NULL || strstr(trimmed, "shutdown") != NULL ||
        strstr(trimmed, "poweroff") != NULL || strstr(trimmed, "rm -rf") != NULL) {
        const char *reject = "REMOTE COMMAND REJECTED: destructive action blocked.\n";
        if (self_evolve_cmd_active_client != NULL) {
            tcp_write(self_evolve_cmd_active_client, reject, (u16_t)strlen(reject), TCP_WRITE_FLAG_COPY);
            tcp_output(self_evolve_cmd_active_client);
        }
        return;
    }

    if (strncmp(trimmed, "runapp ", 7) == 0) {
        const char *response = shell_queue_lisp_app(trimmed + 7) == 0 ?
            "Lisp application queued for the shell task.\n" :
            "Lisp application could not be queued.\n";
        if (self_evolve_cmd_active_client != NULL) {
            tcp_write(self_evolve_cmd_active_client, response, (u16_t)strlen(response), TCP_WRITE_FLAG_COPY);
            tcp_output(self_evolve_cmd_active_client);
        }
        return;
    }

    if (strncmp(trimmed, "lisp ", 5) == 0) {
        const char *response = shell_queue_lisp_expression(trimmed + 5) == 0 ?
            "Lisp expression queued for the shell task.\n" :
            "Lisp expression rejected or queue busy.\n";
        if (self_evolve_cmd_active_client != NULL) {
            tcp_write(self_evolve_cmd_active_client, response, (u16_t)strlen(response), TCP_WRITE_FLAG_COPY);
            tcp_output(self_evolve_cmd_active_client);
        }
        return;
    }

    screen_start_capture();
    shell_execute(trimmed);
    screen_stop_capture();
    output = screen_get_capture();

    if (self_evolve_cmd_active_client != NULL && output != NULL) {
        tcp_write(self_evolve_cmd_active_client, output, (u16_t)strlen(output), TCP_WRITE_FLAG_COPY);
        tcp_output(self_evolve_cmd_active_client);
    }
}

static err_t self_evolve_cmd_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    static char command_buffer[256];
    static size_t command_len = 0;
    char *line_end;
    char *line;

    (void)arg;
    if (err != ERR_OK) {
        if (self_evolve_cmd_active_client == tpcb) {
            self_evolve_cmd_active_client = NULL;
        }
        tcp_close(tpcb);
        return ERR_OK;
    }

    if (p == NULL) {
        if (self_evolve_cmd_active_client == tpcb) {
            self_evolve_cmd_active_client = NULL;
        }
        tcp_close(tpcb);
        command_len = 0;
        command_buffer[0] = '\0';
        return ERR_OK;
    }

    for (struct pbuf *chunk = p; chunk != NULL; chunk = chunk->next) {
        if (chunk->len == 0U) {
            continue;
        }
        size_t available = sizeof(command_buffer) - command_len - 1U;
        size_t copy_len = chunk->len;
        if (copy_len > available) {
            copy_len = available;
        }
        memcpy(command_buffer + command_len, chunk->payload, copy_len);
        command_len += copy_len;
        command_buffer[command_len] = '\0';
    }

    line_end = strchr(command_buffer, '\n');
    if (line_end != NULL) {
        *line_end = '\0';
        line = command_buffer;
        while (*line == ' ' || *line == '\t' || *line == '\r') {
            line++;
        }
        self_evolve_exec_remote_command(line);
        memmove(command_buffer, line_end + 1, strlen(line_end + 1) + 1U);
        command_len = strlen(command_buffer);
    }

    pbuf_free(p);
    return ERR_OK;
}

static err_t self_evolve_cmd_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err) {
    const char *banner = "AOS remote command listener ready. Send one command per line.\n";
    (void)arg;

    if (err != ERR_OK || newpcb == NULL) {
        return ERR_VAL;
    }

    self_evolve_cmd_active_client = newpcb;
    tcp_recv(newpcb, self_evolve_cmd_recv_cb);
    tcp_err(newpcb, NULL);

    if (tcp_write(newpcb, banner, (u16_t)strlen(banner), TCP_WRITE_FLAG_COPY) != ERR_OK) {
        tcp_close(newpcb);
        self_evolve_cmd_active_client = NULL;
        return ERR_OK;
    }
    tcp_output(newpcb);
    return ERR_OK;
}

static void self_evolve_command_network_listen(void) {
    struct tcp_pcb *listener;

    listener = tcp_new();
    if (listener == NULL) {
        print_string("Remote command listener unavailable; continuing without TCP command port.\n");
        return;
    }

    if (tcp_bind(listener, IP_ADDR_ANY, SELF_EVOLVE_CMD_PORT) != ERR_OK) {
        tcp_close(listener);
        print_string("Remote command listener could not bind to port 9001.\n");
        return;
    }

    listener = tcp_listen(listener);
    if (listener == NULL) {
        print_string("Remote command listener failed to start on port 9001.\n");
        return;
    }

    tcp_accept(listener, self_evolve_cmd_accept_cb);
    print_string("HOST SETUP: listening on ");
    if (netif_default != NULL && netif_default->ip_addr.addr != 0U) {
        print_string(ip4addr_ntoa(&(netif_default->ip_addr)));
    } else {
        print_string("DHCP-pending");
    }
    print_string(":9001\n");
}

void self_evolve_init(void) {
    serial_init();
    self_evolve_reset_buffer();
    self_evolve_reset_serial_buffer();
    self_evolve_emit_manifest();
    serial_write_string("SELF-EVOLVE: waiting for driver payload...\n");
    print_color_string("Hosted setup assistant ready; waiting for hardware guidance.\n",
                       MAKE_COLOR(COLOR_LIGHT_CYAN, COLOR_BLACK));

    self_evolve_network_listen();
    self_evolve_command_network_listen();

    if (task_create("host_setup", self_evolve_poll, 2) < 0) {
        print_string("Hosted setup listener unavailable; continuing without it.\n");
    }
}
