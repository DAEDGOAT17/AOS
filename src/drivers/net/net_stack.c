// net_stack.c — Ollama AI client + ICMP ping for Jarvis OS
// Fixes applied vs original:
//   1. Comprehensive system prompt that lists every OS command.
//   2. Uses Ollama's dedicated "system" field (separate from user prompt).
//   3. json_body 3072 B, request 4096 B, intercept_cmd 256 B — no overflow.
//   4. json_escape() sanitises the user prompt (handles embedded " and \).
//   5. TCP accumulation buffer: waits for the full response before parsing
//      so no EXEC_CMD tag can ever span a TCP-segment boundary.
//   6. Explicit bounds check on intercept_cmd: silently discards instead of
//      overflowing when the AI sends an unusually long command string.

#include "net_stack.h"
#include "agent.h"
#include "screen.h"
#include "string.h"
#include "lwip/tcp.h"
#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"

// shell_execute() is defined in shell.c and runs any built-in command.
extern void shell_execute(char* cmd);

// ------------------------------------------------------------------
// Change to match the exact model name shown by 'ollama list'
// ------------------------------------------------------------------
#define OLLAMA_MODEL "gemma:12b"

static char global_prompt[8192];
static char global_ip_str[32];
static int agent_loop_active = 0;
static int agent_action_count = 0;
#define AGENT_MAX_ACTIONS 4

// Keep model-proposed commands inside a narrow, read-only tool boundary.
#define SYSTEM_PROMPT                                                           \
    "You are JARVIS for Jarvis OS. For a prompt marked [AGENT TASK], follow the goal using the tool protocol below. Otherwise answer normally without tools.\\n" \
    "Agent tools are read-only: ls [path], cat <file>, sysinfo, ps, mem, cpuid, arch.\\n" \
    "Use at most four successful tools, one per response, formatted exactly as <EXEC_CMD:tool>. Never use shell chaining, writes, deletes, networking, reboot, or other commands.\\n" \
    "Inspect each actual tool output before deciding the next action. When the goal is satisfied, output <EXEC_CMD:stop>. If it cannot be safely completed, output <EXEC_CMD:stop>; the OS will request review if there is no successful evidence.\\n" \
    "Do not claim an action succeeded unless its output confirms it."

static int agent_readonly_command(const char* command) {
    const char* arg = command;
    while (*arg && *arg != ' ') arg++;
    int verb_len = (int)(arg - command);
    char verb[16];
    if (verb_len <= 0 || verb_len >= (int)sizeof(verb)) return 0;
    memcpy(verb, command, (size_t)verb_len);
    verb[verb_len] = '\0';

    if (*arg == ' ') {
        arg++;
        if (*arg == '\0' || *arg == ' ') return 0;
    }

    int takes_path = strcmp(verb, "ls") == 0 || strcmp(verb, "cat") == 0;
    if (takes_path) {
        if (strcmp(verb, "cat") == 0 && *arg == '\0') return 0;
        while (*arg) {
            char c = *arg++;
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '/' || c == '.' ||
                  c == '_' || c == '-')) return 0;
        }
        return 1;
    }

    if (*arg != '\0') return 0;
    return strcmp(verb, "sysinfo") == 0 || strcmp(verb, "ps") == 0 ||
           strcmp(verb, "mem") == 0 || strcmp(verb, "cpuid") == 0 ||
           strcmp(verb, "arch") == 0;
}

static int agent_tool_failed(const char* output) {
    return !output || output[0] == '\0' ||
           strstr(output, "not found") != NULL ||
           strstr(output, "Unknown command") != NULL ||
           strstr(output, "Usage:") != NULL ||
           strstr(output, "Failed") != NULL;
}

static void ollama_handle_agent_command(const char* command) {
    if (strcmp(command, "stop") == 0 || strcmp(command, "done") == 0) {
        if (agent_loop_active && agent_action_count > 0) {
            char summary[] = "Completed after successful read-only actions; output stored in agent DB.";
            agent_task_result(summary);
            print_string("\n[JARVIS Agent completed with observed action evidence]\n");
        } else if (agent_loop_active) {
            agent_task_failed("Agent stopped without a successful read-only action.");
            print_string("\n[JARVIS Agent stopped; task needs review]\n");
        } else {
            print_string("\n[JARVIS] Tool execution rejected outside agent_task.\n");
        }
        agent_loop_active = 0;
        return;
    }

    if (!agent_loop_active || !agent_readonly_command(command)) {
        print_string("\n[JARVIS] Rejected unsafe or out-of-task tool command.\n");
        if (agent_loop_active) {
            agent_task_failed("Agent requested a command outside the read-only tool allowlist.");
            agent_loop_active = 0;
        }
        return;
    }

    if (agent_action_count >= AGENT_MAX_ACTIONS) {
        agent_task_failed("Agent exceeded the four-action limit.");
        agent_loop_active = 0;
        print_string("\n[JARVIS] Action limit reached; task needs review.\n");
        return;
    }

    set_text_color(MAKE_COLOR(COLOR_LIGHT_CYAN, COLOR_BLACK));
    print_string("\n[JARVIS Agent >> ");
    print_string(command);
    print_string("]\n");
    reset_text_color();

    extern void screen_start_capture(void);
    extern void screen_stop_capture(void);
    extern char* screen_get_capture(void);

    screen_start_capture();
    char command_buffer[256];
    strcpy(command_buffer, command);
    shell_execute(command_buffer);
    screen_stop_capture();

    char* output = screen_get_capture();
    if (agent_tool_failed(output)) {
        agent_task_failed("A read-only tool did not produce successful output.");
        agent_loop_active = 0;
        print_string("\n[JARVIS] Tool failed; task needs review.\n");
        return;
    }

    agent_task_observation(command, output);
    agent_action_count++;

    static char feedback_prompt[2048];
    strcpy(feedback_prompt, "[AGENT EXEC RESULT] Tool: ");
    strcat(feedback_prompt, command);
    strcat(feedback_prompt, "\\nObserved output:\\n");
    int prompt_len = strlen(feedback_prompt);
    int output_len = strlen(output);
    if (output_len > (int)sizeof(feedback_prompt) - prompt_len - 96)
        output_len = (int)sizeof(feedback_prompt) - prompt_len - 96;
    memcpy(feedback_prompt + prompt_len, output, (size_t)output_len);
    feedback_prompt[prompt_len + output_len] = '\0';
    if (agent_action_count >= AGENT_MAX_ACTIONS)
        strcat(feedback_prompt, "\\nAction budget exhausted. Use <EXEC_CMD:stop> now.");
    else
        strcat(feedback_prompt, "\\nChoose one next read-only action, or use <EXEC_CMD:stop> if done.");

    print_string("\n[Network: Sending observed result to agent... ]\n");
    ollama_feedback_request(global_ip_str, feedback_prompt);
}

// ------------------------------------------------------------------
// itoa — int to ASCII, no libc
// ------------------------------------------------------------------
static void itoa(int n, char s[]) {
    int i = 0, j, sign;
    if ((sign = n) < 0) n = -n;
    do { s[i++] = n % 10 + '0'; } while ((n /= 10) > 0);
    if (sign < 0) s[i++] = '-';
    s[i] = '\0';
    for (j = i - 1, i = 0; i < j; i++, j--) {
        char c = s[i]; s[i] = s[j]; s[j] = c;
    }
}

// ------------------------------------------------------------------
// json_escape - copies src into dst, escaping embedded quotes and backslashes.
// Writes at most (dst_max - 1) bytes then null-terminates.
// Prevents a stray quote in the user prompt from breaking the JSON body.
// ------------------------------------------------------------------
static void json_escape(const char* src, char* dst, int dst_max) {
    int out = 0;
    while (*src && out < dst_max - 2) {
        if (*src == '"' || *src == '\\') {
            dst[out++] = '\\';
            dst[out++] = *src++;
        } else if (*src == '\n') {
            dst[out++] = '\\';
            dst[out++] = 'n';
            src++;
        } else if (*src == '\r') {
            dst[out++] = '\\';
            dst[out++] = 'r';
            src++;
        } else if (*src == '\t') {
            dst[out++] = '\\';
            dst[out++] = 't';
            src++;
        } else {
            dst[out++] = *src++;
        }
    }
    dst[out] = '\0';
}

// ------------------------------------------------------------------
// decode_unicode_escape — converts \uXXXX → single ASCII char
// p points to the 4 hex digits that follow \u in the JSON stream.
// ------------------------------------------------------------------
static char decode_unicode_escape(const char* p) {
    unsigned int val = 0;
    for (int i = 0; i < 4; i++) {
        val <<= 4;
        char c = p[i];
        if      (c >= '0' && c <= '9') val |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') val |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') val |= (unsigned)(c - 'A' + 10);
    }
    return (char)(val & 0x7F); // ASCII only
}

// ------------------------------------------------------------------
// ollama_parse_json — walks the "response" JSON field, prints text,
//   and executes anything inside <EXEC_CMD:...> tags.
//
// Handles all the escape forms Ollama models produce in practice:
//   \uXXXX  — e.g. \u003c = '<', \u003e = '>'  (most common)
//   Literal  < EXEC_CMD: ... >                    (fallback)
//   \n       — newline inside the JSON string
//   \t       — tab (printed as space)
//   \"       — escaped quote inside the response text
//
// intercept_cmd is 256 bytes with an explicit bounds check — the
// parser silently discards extra bytes rather than overflowing.
// ------------------------------------------------------------------
#define ICMD_MAX 255

void ollama_parse_json(char* payload) {
    // Skip past HTTP response headers if present (real Ollama response).
    // Mock payloads have no \r\n\r\n so body == payload in that case.
    char* body = payload;
    char* hdr_end = strstr(payload, "\r\n\r\n");
    if (hdr_end) body = hdr_end + 4;

    char* resp = strstr(body, "\"response\":\"");
    if (!resp) {
        if (agent_loop_active) {
            agent_task_failed("AI response did not contain a valid response field.");
            agent_loop_active = 0;
        }
        return;
    }
    resp += 12;        // Skip past  "response":"

    set_text_color(MAKE_COLOR(COLOR_LIGHT_MAGENTA, COLOR_BLACK));
    print_string("\nAI: ");

    char intercept_cmd[ICMD_MAX + 1];
    int  intercept_idx = 0;
    int  catching      = 0;

    while (*resp && *resp != '"') {

        // ── \uXXXX Unicode escape ────────────────────────────────────────
        if (*resp == '\\' && *(resp+1) == 'u' &&
            *(resp+2) && *(resp+3) && *(resp+4) && *(resp+5)) {

            char decoded = decode_unicode_escape(resp + 2);
            resp += 6; // consume \uXXXX

            if (decoded == '<') {
                // Possible start of <EXEC_CMD: tag — skip optional spaces after '<'
                const char *scan = resp;
                while (*scan == ' ') scan++;
                if (scan[0]=='E' && scan[1]=='X' && scan[2]=='E' &&
                    scan[3]=='C' && scan[4]=='_' && scan[5]=='C' &&
                    scan[6]=='M' && scan[7]=='D' && scan[8]==':') {
                    catching      = 1;
                    intercept_idx = 0;
                    resp = (char *)(scan + 9); // skip [spaces]EXEC_CMD:
                    while (*resp == ' ') resp++; // skip spaces after colon
                } else {
                    if (catching && intercept_idx < ICMD_MAX)
                        intercept_cmd[intercept_idx++] = decoded;
                    else if (!catching)
                        print_char(decoded);
                }
            } else if (decoded == '>' && catching) {
                catching = 0;
                while (intercept_idx > 0 && intercept_cmd[intercept_idx-1] == ' ')
                    intercept_idx--;
                intercept_cmd[intercept_idx] = '\0';
                ollama_handle_agent_command(intercept_cmd);
                reset_text_color();
                return;
            } else {
                if (catching && intercept_idx < ICMD_MAX)
                    intercept_cmd[intercept_idx++] = decoded;
                else if (!catching)
                    print_char(decoded);
            }
            continue;
        }

        // ── \n escape (JSON newline) ─────────────────────────────────────
        if (*resp == '\\' && *(resp+1) == 'n') {
            if (!catching) print_char('\n');
            resp += 2;
            continue;
        }

        // ── \t escape (JSON tab — print as space) ────────────────────────
        if (*resp == '\\' && *(resp+1) == 't') {
            if (!catching) print_char(' ');
            resp += 2;
            continue;
        }

        // ── \" escape (literal quote inside response text) ───────────────
        if (*resp == '\\' && *(resp+1) == '"') {
            if (catching && intercept_idx < ICMD_MAX)
                intercept_cmd[intercept_idx++] = '"';
            else if (!catching)
                print_char('"');
            resp += 2;
            continue;
        }

        // ── \\ escape (literal backslash) ───────────────────────────────
        if (*resp == '\\' && *(resp+1) == '\\') {
            if (catching && intercept_idx < ICMD_MAX)
                intercept_cmd[intercept_idx++] = '\\';
            else if (!catching)
                print_char('\\');
            resp += 2;
            continue;
        }

        // ── Literal <EXEC_CMD: — skip optional spaces after '<' ────────
        if (!catching && *resp == '<') {
            const char *scan = resp + 1;
            while (*scan == ' ') scan++; // tolerate "< EXEC_CMD:"
            if (scan[0]=='E' && scan[1]=='X' && scan[2]=='E' &&
                scan[3]=='C' && scan[4]=='_' && scan[5]=='C' &&
                scan[6]=='M' && scan[7]=='D' && scan[8]==':') {
                catching      = 1;
                intercept_idx = 0;
                resp = (char *)(scan + 9); // skip <[spaces]EXEC_CMD:
                while (*resp == ' ') resp++; // skip spaces after colon
                continue;
            }
        }

        // ── Close tag while catching ─────────────────────────────────────
        if (catching && *resp == '>') {
            catching = 0;
            while (intercept_idx > 0 && intercept_cmd[intercept_idx-1] == ' ')
                intercept_idx--;
            intercept_cmd[intercept_idx] = '\0';
            ollama_handle_agent_command(intercept_cmd);
            reset_text_color();
            return;
        }

        // ── Normal character ─────────────────────────────────────────────
        if (catching) {
            // Bounds-checked: silently discard if at limit (no overflow)
            if (intercept_idx < ICMD_MAX)
                intercept_cmd[intercept_idx++] = *resp;
        } else {
            print_char(*resp);
        }
        resp++;
    }

    if (agent_loop_active) {
        agent_task_failed("AI response did not provide a read-only action or stop command.");
        agent_loop_active = 0;
    }
    reset_text_color();
}

// ------------------------------------------------------------------
// TCP accumulation buffer
//
// Problem with the old per-segment approach:
//   Ollama sends the JSON response across multiple TCP segments.
//   If "response":" or an <EXEC_CMD:...> tag straddles a segment
//   boundary, the old parser would silently miss it.
//
// Fix: accumulate all segments here and parse once the connection
// closes (p == NULL callback).  8 KB handles responses up to ~6000
// tokens of llama output which is more than enough for phi3.
// ------------------------------------------------------------------
static char rx_accum[8192];
static int  rx_accum_len = 0;

static err_t ollama_recv_cb(void *arg, struct tcp_pcb *tpcb,
                             struct pbuf *p, err_t err) {
    (void)arg; (void)err;

    if (!p) {
        // Connection closed — the full HTTP response is accumulated.
        tcp_close(tpcb);
        if (rx_accum_len > 0) {
            rx_accum[rx_accum_len] = '\0';
            ollama_parse_json(rx_accum);
        } else if (agent_loop_active) {
            agent_task_failed("AI connection closed without a response.");
            agent_loop_active = 0;
        }
        reset_text_color();
        set_text_color(MAKE_COLOR(COLOR_LIGHT_GREEN, COLOR_BLACK));
        print_string("\nJARVIS [/] $ ");
        reset_text_color();
        rx_accum_len = 0;
        return ERR_OK;
    }

    // Append this segment to the accumulation buffer.
    int space    = (int)sizeof(rx_accum) - rx_accum_len - 1;
    int copy_len = (int)p->tot_len;
    if (copy_len > space) copy_len = space;
    if (copy_len > 0) {
        pbuf_copy_partial(p, rx_accum + rx_accum_len, copy_len, 0);
        rx_accum_len += copy_len;
    }

    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

// ------------------------------------------------------------------
// ai_mock — offline test without a real Ollama server
// ------------------------------------------------------------------
void ollama_mock_intercept() {
    print_string("Simulating AI Agent response (offline)...\n");
    // Exercises: plain text, a \n escape, two EXEC_CMD tags.
    char* test_payload =
        "{\"model\":\"" OLLAMA_MODEL "\","
        "\"response\":\"Scanning your system now!\\n"
        "<EXEC_CMD:sysinfo>"
        " Also listing storage devices: "
        "<EXEC_CMD:pci storage>"
        "\"}";
    ollama_parse_json(test_payload);
}

// ------------------------------------------------------------------
// ollama_connected_cb — builds and sends the HTTP POST to Ollama
// ------------------------------------------------------------------
static err_t ollama_connected_cb(void *arg, struct tcp_pcb *tpcb, err_t err) {
    (void)arg;
    if (err != ERR_OK) {
        if (agent_loop_active) {
            agent_task_failed("Could not connect to the AI endpoint.");
            agent_loop_active = 0;
        }
        print_string("Network: Connection to Ollama FAILED (err=");
        kprint_dec((int)err);
        print_string(").\nJARVIS [/] $ ");
        return err;
    }
    print_string("Network: Connected! Model: " OLLAMA_MODEL "\n");

    // JSON-escape the user prompt so stray " or \ can't break the body.
    static char escaped_prompt[8192];
    json_escape(global_prompt, escaped_prompt, (int)sizeof(escaped_prompt));

    // Build JSON body.
    // Layout: {"model":"...","system":"<SYSTEM_PROMPT>","prompt":"<user>","stream":false}
    static char json_body[16384];
    strcpy(json_body,
        "{\"model\":\"" OLLAMA_MODEL "\","
        "\"system\":\"" SYSTEM_PROMPT "\","
        "\"prompt\":\"");

    // Append escaped user prompt only if it fits.
    int used = strlen(json_body);
    int esc_len = strlen(escaped_prompt);
    if (used + esc_len < (int)sizeof(json_body) - 24) {
        strcat(json_body, escaped_prompt);
    } else {
        print_string("Network: Prompt truncated (too long).\n");
    }
    strcat(json_body, "\",\"stream\":false}");

    // Build HTTP request.
    // Worst-case: ~200 (headers) + ~2100 (body) = ~2300 → 4096 is safe.
    char len_str[16];
    itoa(strlen(json_body), len_str);

    static char request[16384];
    strcpy(request, "POST /api/generate HTTP/1.1\r\n");
    strcat(request, "Host: ");
    strcat(request, global_ip_str);
    strcat(request, ":11434\r\n");
    strcat(request, "Content-Type: application/json\r\n");
    strcat(request, "Connection: close\r\n");  // server flushes immediately
    strcat(request, "Content-Length: ");
    strcat(request, len_str);
    strcat(request, "\r\n\r\n");
    strcat(request, json_body);

    int req_len = strlen(request);
    if (req_len >= (int)sizeof(request)) {
        print_string("Network: Request overflowed buffer! Aborting.\n");
        tcp_close(tpcb);
        return ERR_MEM;
    }

    err_t write_err = tcp_write(tpcb, request, req_len, TCP_WRITE_FLAG_COPY);
    if (write_err != ERR_OK) {
        print_string("Network: tcp_write() FAILED (err=");
        kprint_dec((int)write_err);
        print_string(") — request too large for TCP_SND_BUF!\n");
        tcp_close(tpcb);
        return write_err;
    }
    err_t out_err = tcp_output(tpcb);
    if (out_err != ERR_OK) {
        print_string("Network: tcp_output() FAILED (err=");
        kprint_dec((int)out_err);
        print_string(")\n");
    }
    print_string("Network: HTTP POST sent (");
    kprint_dec(req_len);
    print_string(" bytes). Waiting for Ollama...\n");
    return ERR_OK;
}

// ------------------------------------------------------------------
// Error callback
// ------------------------------------------------------------------
static void ollama_err_cb(void *arg, err_t err) {
    (void)arg;
    if (agent_loop_active) {
        agent_task_failed("AI connection failed during the agent loop.");
        agent_loop_active = 0;
    }
    print_string("\nNetwork: TCP error (err=");
    kprint_dec((int)err);
    print_string(") — Ollama unreachable or reset.\nJARVIS [/] $ ");
    rx_accum_len = 0;
}

void ollama_feedback_request(const char* ip_str, const char* feedback) {
    rx_accum_len = 0;

    int g_len = strlen(global_prompt);
    int f_len = strlen(feedback);
    if (g_len + f_len + 5 < 8192) {
        strcat(global_prompt, "\n\n");
        strcat(global_prompt, feedback);
    } else {
        print_string("Network: Memory limit reached for autonomous chain. Halting.\n");
        if (agent_loop_active) {
            agent_task_failed("Agent context limit reached.");
            agent_loop_active = 0;
        }
        return;
    }

    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) {
        if (agent_loop_active) {
            agent_task_failed("Could not allocate a TCP connection for agent feedback.");
            agent_loop_active = 0;
        }
        return;
    }
    ip4_addr_t server;
    if (!ip4addr_aton(ip_str, &server)) {
        tcp_close(pcb);
        if (agent_loop_active) {
            agent_task_failed("Invalid AI endpoint address.");
            agent_loop_active = 0;
        }
        return;
    }
    tcp_recv(pcb, ollama_recv_cb);
    tcp_err(pcb, ollama_err_cb);
    tcp_connect(pcb, &server, 11434, ollama_connected_cb);
}

// ------------------------------------------------------------------
// ollama_request — public API called by 'ai <ip> <prompt>'
// ------------------------------------------------------------------
void ollama_request(const char* ip_str, const char* prompt) {
    // Reset accumulation buffer before every new request.
    rx_accum_len = 0;

    extern char fat32_cwd_path[];
    static char context_prompt[1024];
    strcpy(context_prompt, "[JARVIS OS STATE] CWD=");
    strcat(context_prompt, fat32_cwd_path);
    strcat(context_prompt, " | Task: ");

    int ctx_len  = strlen(context_prompt);
    int user_len = strlen(prompt);
    int max_user = (int)sizeof(context_prompt) - ctx_len - 96;
    if (max_user < 0) max_user = 0;
    if (user_len > max_user) user_len = max_user;
    
    int cur_len = strlen(context_prompt);
    strncpy(context_prompt + cur_len, prompt, user_len);
    context_prompt[cur_len + user_len] = '\0';

    if (agent_loop_active)
        strcat(context_prompt, " [AGENT TASK] Use bounded read-only tools and report observations.");
    else
        strcat(context_prompt, " Answer normally without executing tools.");

    int len = strlen(context_prompt);
    if (len > 1023) len = 1023;
    strncpy(global_prompt, context_prompt, len);
    global_prompt[len] = '\0';

    strncpy(global_ip_str, ip_str, 31);
    global_ip_str[31] = '\0';

    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) {
        print_string("Network: Out of memory for TCP socket.\n");
        if (agent_loop_active) {
            agent_task_failed("Could not allocate an AI connection.");
            agent_loop_active = 0;
        }
        return;
    }

    ip4_addr_t server;
    if (!ip4addr_aton(ip_str, &server)) {
        print_string("Network: Invalid IP address format!\n");
        tcp_close(pcb);
        if (agent_loop_active) {
            agent_task_failed("Invalid AI endpoint address.");
            agent_loop_active = 0;
        }
        return;
    }

    // ARP pre-warm: resolve the server MAC before tcp_connect() fires
    // the SYN, so the SYN goes out immediately without an ARP round-trip.
    extern struct netif *netif_default;
    if (netif_default) {
        ip_addr_t srv;
        ip_addr_copy_from_ip4(srv, server);
        etharp_query(netif_default, ip_2_ip4(&srv), NULL);
        print_string("Network: ARP pre-warm sent for ");
        print_string(ip_str);
        print_string("\n");
    }

    tcp_recv(pcb, ollama_recv_cb);
    tcp_err(pcb, ollama_err_cb);
    err_t e = tcp_connect(pcb, &server, 11434, ollama_connected_cb);
    if (e != ERR_OK) {
        print_string("Network: tcp_connect() failed.\n");
        tcp_close(pcb);
        if (agent_loop_active) {
            agent_task_failed("Could not start the AI connection.");
            agent_loop_active = 0;
        }
    }
}

void ollama_agent_request(const char* ip_str, const char* prompt) {
    if (agent_loop_active) {
        print_string("Agent ERROR: another task is already running.\n");
        return;
    }
    agent_loop_active = 1;
    agent_action_count = 0;
    ollama_request(ip_str, prompt);
}

// ======================================================================
// ICMP Ping (unchanged from previous version)
// ======================================================================
#include "lwip/raw.h"
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
#include "timer.h"

static struct raw_pcb *ping_pcb       = NULL;
static uint32_t        ping_start_ms  = 0;
static ip4_addr_t      ping_target;

static u8_t ping_recv_cb(void *arg, struct raw_pcb *pcb,
                          struct pbuf *p, const ip_addr_t *addr) {
    (void)arg; (void)pcb;
    if (p->tot_len >= sizeof(struct icmp_echo_hdr)) {
        struct icmp_echo_hdr *iecho = (struct icmp_echo_hdr *)p->payload;
        if (iecho->type == ICMP_ER) {
            uint32_t rtt = (timer_get_ticks() - ping_start_ms) * 10;
            print_string("64 bytes from ");
            print_string(ip4addr_ntoa(ip_2_ip4(addr)));
            print_string(": icmp_seq=");
            kprint_dec(lwip_ntohs(iecho->seqno));
            print_string(" time=");
            kprint_dec(rtt);
            print_string("ms\n");
            pbuf_free(p);
            return 1; // consumed
        }
    }
    return 0;
}

void ping_request(const char* ip_str) {
    if (!ip4addr_aton(ip_str, &ping_target)) {
        print_string("Ping: Invalid IP address.\n");
        return;
    }
    if (!ping_pcb) {
        ping_pcb = raw_new(IP_PROTO_ICMP);
        if (!ping_pcb) { print_string("Ping: No RAW PCB.\n"); return; }
        raw_recv(ping_pcb, ping_recv_cb, NULL);
        raw_bind(ping_pcb, IP4_ADDR_ANY);
    }
    struct pbuf *p = pbuf_alloc(PBUF_IP, sizeof(struct icmp_echo_hdr), PBUF_RAM);
    if (!p) { print_string("Ping: Out of memory.\n"); return; }

    struct icmp_echo_hdr *iecho = (struct icmp_echo_hdr *)p->payload;
    ICMPH_TYPE_SET(iecho, ICMP_ECHO);
    ICMPH_CODE_SET(iecho, 0);
    iecho->chksum = 0;
    iecho->id     = lwip_htons(0xBEEF);
    iecho->seqno  = lwip_htons(1);
    iecho->chksum = inet_chksum(iecho, p->len);

    print_string("PING "); print_string(ip_str);
    print_string(" ("); print_string(ip_str);
    print_string("): 56 data bytes\n");

    ping_start_ms = timer_get_ticks();
    raw_sendto(ping_pcb, p, (ip_addr_t *)&ping_target);
    pbuf_free(p);
}
