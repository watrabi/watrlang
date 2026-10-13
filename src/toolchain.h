#ifndef WATR_TOOLCHAIN_H
#define WATR_TOOLCHAIN_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    BUILD_STATIC,        /* GNU as + ld: a static binary with watrlang's own entry point */
    BUILD_LIBC,          /* the system C compiler links it, adding the C runtime */
    BUILD_FREESTANDING,  /* ld with no libraries at all, optionally with a linker script */
    BUILD_OBJECT,        /* just assemble: an object file */
} BuildMode;

typedef struct {
    BuildMode mode;
    bool verbose;                /* print the commands first */
    const char *linker_script;   /* -T, for BUILD_FREESTANDING */
    const char *entry;           /* -e, for BUILD_FREESTANDING */
    const char *const *extra;    /* more inputs: .c, .s, .o, .a, .so */
    size_t nextra;
} BuildOptions;

/* Assembles `asm_text` and produces `out_path` as `opts` says. */
bool toolchain_build(const char *asm_text, size_t asm_len, const char *out_path, const BuildOptions *opts);

/* Writes `len` bytes to `path`. Prints an error on failure. */
bool write_file(const char *path, const char *data, size_t len);

#endif
