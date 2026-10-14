#include "target.h"

#include <string.h>

static const Target targets[] = {
    {"x86_64-linux", OS_LINUX, "linux", ""},
    {"x86_64-windows", OS_WINDOWS, "windows", ".exe"},
};

const Target *watr_target = &targets[0];

const Target *target_find(const char *name) {
    for (size_t i = 0; i < sizeof targets / sizeof targets[0]; i++) {
        const Target *t = &targets[i];
        if (strcmp(name, t->name) == 0 || strcmp(name, t->os_name) == 0) return t;
    }
    /* The GNU-style triples people already know. */
    if (strcmp(name, "x86_64-pc-windows-gnu") == 0 || strcmp(name, "x86_64-w64-mingw32") == 0) return &targets[1];
    if (strcmp(name, "x86_64-linux-gnu") == 0 || strcmp(name, "x86_64-unknown-linux-gnu") == 0) return &targets[0];
    return NULL;
}

const char *target_names(void) {
    return "x86_64-linux, x86_64-windows";
}
