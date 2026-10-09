#include "core/jit_runtime.h"
#include "core/ksym.h"
#include "drivers/serial.h"
#include "screen.h"
#include "string.h"
#include "vmm.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

static int jit_contract_has_entry(const char *source_code, const char *entry_name) {
    if (!source_code || !entry_name) {
        return 0;
    }
    return strstr(source_code, entry_name) != NULL;
}

static int jit_is_ident_char(char ch) {
    return (ch == '_') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
}

static int jit_extract_assignment_value(const char *source_code, const char *name, int *out_value) {
    char token[64];
    const char *cursor;
    const char *eq;
    const char *semi;
    size_t len;
    long value;
    char *endptr;

    if (!source_code || !name || !out_value) {
        return 0;
    }

    cursor = source_code;
    len = strlen(name);
    while (*cursor) {
        if (strncmp(cursor, name, len) == 0) {
            if (cursor > source_code && jit_is_ident_char(cursor[-1])) {
                cursor++;
                continue;
            }

            cursor += len;
            while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r') {
                cursor++;
            }

            if (*cursor != '=') {
                continue;
            }

            eq = cursor + 1;
            while (*eq == ' ' || *eq == '\t' || *eq == '\n' || *eq == '\r') {
                eq++;
            }

            semi = eq;
            while (*semi && *semi != ';') {
                semi++;
            }
            if (*semi != ';') {
                return 0;
            }

            token[0] = '\0';
            for (len = 0; len < sizeof(token) - 1 && eq + len < semi; ++len) {
                token[len] = eq[len];
            }
            token[len] = '\0';

            value = strtol(token, &endptr, 10);
            if (endptr == token || *endptr != '\0') {
                return 0;
            }

            *out_value = (int)value;
            return 1;
        }
        cursor++;
    }

    return 0;
}

static int jit_resolve_symbol_refs(const char *source_code) {
    static const char *known_symbols[] = {
        "print_string",
        "serial_write_string",
        "kfree",
        "kmalloc",
        "vmm_map_page",
        "vmm_alloc_exec_pages",
        "aos_apply_network_setup",
        "shell_install_runtime_command",
        "inb",
        "outb"
    };
    size_t i;

    if (!source_code) {
        return 0;
    }

    for (i = 0; i < (sizeof(known_symbols) / sizeof(known_symbols[0])); ++i) {
        if (strstr(source_code, known_symbols[i]) != NULL && ksym_lookup(known_symbols[i]) == 0) {
            print_string("JIT: unresolved exported symbol: ");
            print_string((char *)known_symbols[i]);
            print_string("\n");
            return 0;
        }
    }

    return 1;
}

static int jit_parse_number(const char **cursor, int *out_value) {
    char *endptr = NULL;
    long value;

    if (!cursor || !*cursor || !out_value) {
        return 0;
    }

    while (**cursor == ' ' || **cursor == '\t' || **cursor == '\n' || **cursor == '\r') {
        (*cursor)++;
    }

    if (**cursor == '-') {
        value = strtol(*cursor, &endptr, 10);
        if (endptr == *cursor) {
            return 0;
        }
        *out_value = (int)value;
        *cursor = endptr;
        return 1;
    }

    if (**cursor < '0' || **cursor > '9') {
        return 0;
    }

    value = strtol(*cursor, &endptr, 10);
    if (endptr == *cursor) {
        return 0;
    }
    *out_value = (int)value;
    *cursor = endptr;
    return 1;
}

static int jit_parse_identifier_value(const char *source_code, const char *ident, int *out_value) {
    if (!source_code || !ident || !out_value) {
        return 0;
    }

    if (strcmp(ident, "driver_init") == 0) {
        return 0;
    }

    return jit_extract_assignment_value(source_code, ident, out_value);
}

static int jit_parse_expression(const char *source_code, const char **cursor, int *out_value) {
    int value = 0;
    int rhs = 0;
    char ident[64];
    size_t ident_len = 0;
    int negative = 0;

    if (!source_code || !cursor || !out_value) {
        return 0;
    }

    while (**cursor == ' ' || **cursor == '\t' || **cursor == '\n' || **cursor == '\r') {
        (*cursor)++;
    }

    if (**cursor == '-') {
        negative = 1;
        (*cursor)++;
    }

    if (**cursor >= '0' && **cursor <= '9') {
        if (!jit_parse_number(cursor, &value)) {
            return 0;
        }
    } else if ((**cursor >= 'a' && **cursor <= 'z') || (**cursor >= 'A' && **cursor <= 'Z') || **cursor == '_') {
        ident_len = 0;
        while (jit_is_ident_char(**cursor) && ident_len < sizeof(ident) - 1) {
            ident[ident_len++] = **cursor;
            (*cursor)++;
        }
        ident[ident_len] = '\0';

        if (!jit_parse_identifier_value(source_code, ident, &value)) {
            return 0;
        }
    } else {
        return 0;
    }

    if (negative) {
        value = -value;
    }

    while (**cursor == ' ' || **cursor == '\t' || **cursor == '\n' || **cursor == '\r') {
        (*cursor)++;
    }

    while (**cursor == '+' || **cursor == '-' || **cursor == '*' || **cursor == '/') {
        char op = **cursor;
        (*cursor)++;
        if (!jit_parse_expression(source_code, cursor, &rhs)) {
            return 0;
        }
        if (op == '+') value += rhs;
        else if (op == '-') value -= rhs;
        else if (op == '*') value *= rhs;
        else if (op == '/') {
            if (rhs == 0) {
                return 0;
            }
            value /= rhs;
        }
        while (**cursor == ' ' || **cursor == '\t' || **cursor == '\n' || **cursor == '\r') {
            (*cursor)++;
        }
    }

    *out_value = value;
    return 1;
}

static int jit_parse_return_value(const char *source_code, int *out_value) {
    const char *ret = NULL;
    const char *cursor;
    int value = 0;

    if (!source_code || !out_value) {
        return 0;
    }

    ret = strstr(source_code, "return");
    if (!ret) {
        return 0;
    }

    cursor = ret + 6;
    if (!jit_parse_expression(source_code, &cursor, &value)) {
        return 0;
    }
    while (*cursor && *cursor != ';') {
        cursor++;
    }
    if (*cursor != ';') {
        return 0;
    }

    *out_value = value;
    return 1;
}

static const char *jit_skip_whitespace(const char *cursor) {
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r') {
        cursor++;
    }
    return cursor;
}

static int jit_parse_string_call(const char *source_code,
                                 const char *entry_name,
                                 char *symbol_name,
                                 size_t symbol_name_size,
                                 char *literal,
                                 size_t literal_size) {
    static const char *output_symbols[] = {
        "serial_write_string",
        "print_string",
        "aos_apply_network_setup",
        "shell_install_runtime_command"
    };
    const char *entry;
    const char *body;
    const char *body_end;
    const char *call = NULL;
    const char *call_name = NULL;
    const char *cursor;
    size_t symbol_index;
    size_t literal_len = 0;

    if (!source_code || !entry_name || !symbol_name || symbol_name_size == 0 ||
        !literal || literal_size == 0) {
        return 0;
    }

    entry = strstr(source_code, entry_name);
    if (!entry) {
        return 0;
    }
    body = strchr(entry, '{');
    body_end = body ? strchr(body + 1, '}') : NULL;
    if (!body || !body_end) {
        return 0;
    }

    for (symbol_index = 0;
         symbol_index < sizeof(output_symbols) / sizeof(output_symbols[0]);
         ++symbol_index) {
        const char *candidate = strstr(body + 1, output_symbols[symbol_index]);
        const char *after_name;

        if (!candidate || candidate >= body_end ||
            (candidate > body + 1 && jit_is_ident_char(candidate[-1]))) {
            continue;
        }

        after_name = jit_skip_whitespace(candidate + strlen(output_symbols[symbol_index]));
        if (*after_name != '(' || (call && candidate >= call)) {
            continue;
        }

        call = candidate;
        call_name = output_symbols[symbol_index];
    }

    if (!call || !call_name || strlen(call_name) >= symbol_name_size) {
        return 0;
    }

    memcpy(symbol_name, call_name, strlen(call_name) + 1);
    cursor = jit_skip_whitespace(call + strlen(call_name));
    if (*cursor++ != '(') {
        return 0;
    }
    cursor = jit_skip_whitespace(cursor);
    if (*cursor++ != '"') {
        return 0;
    }

    while (*cursor && cursor < body_end && *cursor != '"') {
        char ch = *cursor++;
        if (ch == '\\') {
            ch = *cursor++;
            if (ch == 'n') ch = '\n';
            else if (ch == 'r') ch = '\r';
            else if (ch == 't') ch = '\t';
            else if (ch != '\\' && ch != '"') return 0;
        }
        if (literal_len + 1 >= literal_size) {
            return 0;
        }
        literal[literal_len++] = ch;
    }
    if (*cursor++ != '"') {
        return 0;
    }
    literal[literal_len] = '\0';

    cursor = jit_skip_whitespace(cursor);
    if (*cursor++ != ')') {
        return 0;
    }
    cursor = jit_skip_whitespace(cursor);
    return *cursor == ';';
}

static int jit_emit_return_stub(uint8_t *code_buf, size_t code_size, int return_value) {
    if (!code_buf || code_size < 6) {
        return 0;
    }

    code_buf[0] = 0xB8;
    code_buf[1] = (uint8_t)(return_value & 0xFF);
    code_buf[2] = (uint8_t)((return_value >> 8) & 0xFF);
    code_buf[3] = (uint8_t)((return_value >> 16) & 0xFF);
    code_buf[4] = (uint8_t)((return_value >> 24) & 0xFF);
    code_buf[5] = 0xC3;
    return 1;
}

static int jit_emit_call_stub(uint8_t *code_buf,
                              size_t code_size,
                              uintptr_t arg_addr,
                              int return_value,
                              int preserve_call_result,
                              size_t *target_patch_offset) {
    static const uint8_t call_prefix[] = {
        0x48, 0x83, 0xEC, 0x08,       /* sub rsp, 8: align stack before nested call */
        0x48, 0xBF                    /* movabs rdi, string pointer */
    };
    static const uint8_t call_suffix[] = {
        0x48, 0xB8                    /* movabs rax, symbol address */
    };
    size_t offset = 0;

    if (!code_buf || !target_patch_offset || code_size < 37) {
        return 0;
    }

    memcpy(code_buf + offset, call_prefix, sizeof(call_prefix));
    offset += sizeof(call_prefix);
    memcpy(code_buf + offset, &arg_addr, sizeof(arg_addr));
    offset += sizeof(arg_addr);
    memcpy(code_buf + offset, call_suffix, sizeof(call_suffix));
    offset += sizeof(call_suffix);
    *target_patch_offset = offset;
    memset(code_buf + offset, 0, sizeof(uintptr_t));
    offset += sizeof(uintptr_t);
    code_buf[offset++] = 0xFF;
    code_buf[offset++] = 0xD0;       /* call rax */
    code_buf[offset++] = 0x48;
    code_buf[offset++] = 0x83;
    code_buf[offset++] = 0xC4;
    code_buf[offset++] = 0x08;       /* add rsp, 8 */
    if (!preserve_call_result) {
        code_buf[offset++] = 0xB8;
        code_buf[offset++] = (uint8_t)(return_value & 0xFF);
        code_buf[offset++] = (uint8_t)((return_value >> 8) & 0xFF);
        code_buf[offset++] = (uint8_t)((return_value >> 16) & 0xFF);
        code_buf[offset++] = (uint8_t)((return_value >> 24) & 0xFF);
    }
    code_buf[offset++] = 0xC3;
    return 1;
}

static void jit_capture_fault_dump(jit_runtime_status_t *status) {
    uintptr_t cr2 = 0;
    uintptr_t rip = 0;
    uintptr_t rsp = 0;

    asm volatile("mov %%cr2, %0" : "=r"(cr2));
    asm volatile("mov %%rsp, %0" : "=r"(rsp));
    asm volatile("mov $1f, %0\n1:" : "=r"(rip));

    if (status) {
        status->fault_cr2 = cr2;
        status->fault_rip = rip;
        status->fault_rsp = rsp;
    }

    serial_write_string("JIT: fault captured: CR2=");
    serial_write_hex32((uint32_t)cr2);
    serial_write_string(" RIP=");
    serial_write_hex32((uint32_t)rip);
    serial_write_string(" RSP=");
    serial_write_hex32((uint32_t)rsp);
    serial_write_string("\n");
}

int jit_runtime_exec_driver(const char *source_code, const char *entry_name, jit_runtime_status_t *status) {
    uintptr_t print_symbol = 0;
    uintptr_t alloc_symbol = 0;
    uintptr_t serial_symbol = 0;
    void *rx_pages;
    int return_value = 0;
    char call_symbol[32];
    char call_literal[256];
    int has_call = 0;
    int (*driver_fn)(void) = NULL;

    if (!source_code || !entry_name) {
        print_string("JIT: invalid source or entry name\n");
        jit_capture_fault_dump(status);
        return -1;
    }

    if (!jit_contract_has_entry(source_code, entry_name)) {
        print_string("JIT: contract rejected: missing requested entry\n");
        jit_capture_fault_dump(status);
        return -2;
    }

    print_symbol = ksym_lookup("print_string");
    alloc_symbol = ksym_lookup("vmm_alloc_exec_pages");
    serial_symbol = ksym_lookup("serial_write_string");
    if (!print_symbol || !alloc_symbol || !serial_symbol) {
        print_string("JIT: kernel symbol binding failed\n");
        jit_capture_fault_dump(status);
        return -3;
    }

    if (!jit_resolve_symbol_refs(source_code)) {
        print_string("JIT: exported symbol validation failed\n");
        jit_capture_fault_dump(status);
        return -4;
    }

    has_call = jit_parse_string_call(source_code,
                                     entry_name,
                                     call_symbol,
                                     sizeof(call_symbol),
                                     call_literal,
                                     sizeof(call_literal));
    if (!jit_parse_return_value(source_code, &return_value) ||
        (!has_call && (strstr(source_code, "serial_write_string") != NULL ||
                       strstr(source_code, "print_string") != NULL ||
                       strstr(source_code, "aos_apply_network_setup") != NULL))) {
        print_string("JIT: unsupported subset: expected a return expression and optional output literal call\n");
        jit_capture_fault_dump(status);
        return -5;
    }

    rx_pages = vmm_alloc_exec_pages(1);
    if (!rx_pages) {
        print_string("JIT: executable page allocation failed\n");
        return -6;
    }

    memset(rx_pages, 0x90, 4096);

    {
        uintptr_t target_addr = 0;

        if (has_call) {
            char *literal_ptr;
            size_t literal_len;
            size_t target_patch_offset;

            target_addr = ksym_lookup(call_symbol);
            if (!target_addr) {
                print_string("JIT: missing symbol for generated call\n");
                return -7;
            }

            literal_ptr = (char *)rx_pages + 512;
            literal_len = strlen(call_literal);
            if (literal_len > 0) {
                memcpy(literal_ptr, call_literal, literal_len + 1);
            } else {
                literal_ptr[0] = '\0';
            }

            if (!jit_emit_call_stub((uint8_t *)rx_pages,
                                    4096,
                                    (uintptr_t)literal_ptr,
                                    return_value,
                                    strcmp(call_symbol, "aos_apply_network_setup") == 0 ||
                                        strcmp(call_symbol, "shell_install_runtime_command") == 0,
                                    &target_patch_offset)) {
                print_string("JIT: call stub generation failed\n");
                return -8;
            }
            if (target_patch_offset + sizeof(target_addr) > 4096) {
                print_string("JIT: call relocation is out of bounds\n");
                return -8;
            }
            memcpy((uint8_t *)rx_pages + target_patch_offset, &target_addr, sizeof(target_addr));
        } else {
            if (!jit_emit_return_stub((uint8_t *)rx_pages, 4096, return_value)) {
                print_string("JIT: stub generation failed\n");
                return -7;
            }
        }
    }

    if (!vmm_set_page_writable((uintptr_t)rx_pages, 0)) {
        print_string("JIT: unable to seal generated page read-only\n");
        return -9;
    }

    driver_fn = (int (*)(void))rx_pages;
    return_value = driver_fn();

    if (status) {
        status->entry_vaddr = (uintptr_t)rx_pages;
        status->error_code = (uintptr_t)return_value;
        status->status = 0;
    }

    serial_write_string("JIT: generated x64 stub executed in RX page: result=");
    serial_write_hex32((uint32_t)return_value);
    serial_write_string("\n");
    print_string("JIT: runtime contract accepted and executed\n");

    if (has_call && strcmp(call_symbol, "aos_apply_network_setup") == 0 && return_value != 0) {
        return -10;
    }
    if (has_call && strcmp(call_symbol, "shell_install_runtime_command") == 0 && return_value != 0) {
        return -11;
    }

    return 0;
}
