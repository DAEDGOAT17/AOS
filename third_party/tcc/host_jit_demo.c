#include "aos_tcc_port.h"

#include <stdio.h>

int main(void) {
    const char *code =
        "int driver_init(void) { return 42; }\n";
    driver_init_fn fn = NULL;
    int rc = aos_tcc_compile_and_load(code, "driver_init", "-O2", &fn);

    if (rc != 0) {
        fprintf(stderr, "compile failed: rc=%d\n", rc);
        return 1;
    }

    if (!fn) {
        fprintf(stderr, "driver_init symbol not resolved\n");
        return 2;
    }

    printf("driver_init() -> %d\n", fn());
    return 0;
}
