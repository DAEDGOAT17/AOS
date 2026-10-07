#ifndef AOS_TCC_PORT_H
#define AOS_TCC_PORT_H

#include <stddef.h>

typedef int (*driver_init_fn)(void);

int aos_tcc_compile_and_load(const char *source_code,
                            const char *symbol_name,
                            const char *extra_cflags,
                            driver_init_fn *out_fn);

#endif
