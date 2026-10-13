#ifndef WATR_TOOLCHAIN_H
#define WATR_TOOLCHAIN_H

#include <stdbool.h>
#include <stddef.h>

/* Assembles `asm_text` and links an executable at `out_path`. Without libc
 * that's GNU as + ld into a static binary with watrlang's own entry point;
 * with libc (or extra C inputs) the system C compiler links it, adding the
 * C runtime. Prints the commands first when `verbose`. */
bool toolchain_build_executable(const char *asm_text, size_t asm_len, const char *out_path, bool verbose,
                                bool libc, const char *const *extra, size_t nextra);

/* Writes `len` bytes to `path`. Prints an error on failure. */
bool write_file(const char *path, const char *data, size_t len);

#endif
