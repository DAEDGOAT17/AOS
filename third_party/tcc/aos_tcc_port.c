#include "aos_tcc_port.h"

#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int aos_tcc_compile_and_load(const char *source_code,
                            const char *symbol_name,
                            const char *extra_cflags,
                            driver_init_fn *out_fn) {
    char tmp_dir_template[] = "/tmp/aos_tcc_XXXXXX";
    char src_path[PATH_MAX];
    char so_path[PATH_MAX];
    char command[16384];
    char *tmp_dir;
    FILE *fp;
    void *handle;
    void *symbol;

    if (!source_code || !symbol_name || !out_fn) {
        return -1;
    }

    tmp_dir = mkdtemp(tmp_dir_template);
    if (!tmp_dir) {
        return -2;
    }

    snprintf(src_path, sizeof(src_path), "%s/driver.c", tmp_dir);
    snprintf(so_path, sizeof(so_path), "%s/driver.so", tmp_dir);

    fp = fopen(src_path, "w");
    if (!fp) {
        return -3;
    }
    fputs(source_code, fp);
    fclose(fp);

    snprintf(command, sizeof(command),
             "gcc -shared -fPIC -x c %s -o %s %s",
             src_path,
             so_path,
             extra_cflags ? extra_cflags : "");

    if (system(command) != 0) {
        return -4;
    }

    handle = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        return -5;
    }

    symbol = dlsym(handle, symbol_name);
    if (!symbol) {
        dlclose(handle);
        return -6;
    }

    *out_fn = (driver_init_fn)symbol;
    return 0;
}
