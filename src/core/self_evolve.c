#include "core/self_evolve.h"
#include "core/jit_engine.h"
#include "drivers/serial.h"
#include "screen.h"
#include "string.h"
#include "task.h"
#include "lwip/tcp.h"
#include "lwip/netif.h"

#define SELF_EVOLVE_BEGIN "---BEGIN_DRIVER---"
#define SELF_EVOLVE_END   "---END_DRIVER---"
#define SELF_EVOLVE_PORT  9000U

static char self_evolve_buffer[4096];
static char self_evolve_payload[4096];
static size_t self_evolve_len;
static struct tcp_pcb *self_evolve_listener = NULL;
static struct tcp_pcb *self_evolve_active_client = NULL;

static int self_evolve_extract_payload(const char *input, char *out, size_t out_size);

static void self_evolve_reset_buffer(void) {
    self_evolve_len = 0;
    self_evolve_buffer[0] = '\0';
    self_evolve_payload[0] = '\0';
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

    if (self_evolve_len + 2U >= sizeof(self_evolve_buffer)) {
        self_evolve_reset_buffer();
        self_evolve_report_result("---RESULT: PAYLOAD_TOO_LARGE---\n");
        return;
    }

    self_evolve_buffer[self_evolve_len++] = (char)ch;
    self_evolve_buffer[self_evolve_len] = '\0';

    if (strstr(self_evolve_buffer, SELF_EVOLVE_END) != NULL) {
        self_evolve_commit_payload();
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

void self_evolve_init(void) {
    serial_init();
    self_evolve_reset_buffer();
    self_evolve_emit_manifest();
    serial_write_string("SELF-EVOLVE: waiting for driver payload...\n");
    print_color_string("Hosted setup assistant ready; waiting for hardware guidance.\n",
                       MAKE_COLOR(COLOR_LIGHT_CYAN, COLOR_BLACK));

    self_evolve_network_listen();

    if (task_create("host_setup", self_evolve_poll, 2) < 0) {
        print_string("Hosted setup listener unavailable; continuing without it.\n");
    }
}
