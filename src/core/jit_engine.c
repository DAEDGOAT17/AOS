#include "core/jit_engine.h"
#include "core/jit_runtime.h"
#include "core/ksym.h"
#include "screen.h"
#include "string.h"

static int jit_has_driver_entry(const char *source_code) {
    if (!source_code) {
        return 0;
    }
    return strstr(source_code, "driver_init") != NULL;
}

int jit_compile_and_load(const char *source_code) {
    jit_runtime_status_t status = {0};

    if (!source_code) {
        print_string("JIT: no source supplied\n");
        return -1;
    }

    if (!jit_has_driver_entry(source_code)) {
        print_string("JIT: rejected source: missing driver_init()\n");
        return -2;
    }

    return jit_runtime_exec_driver(source_code, "driver_init", &status);
}
