#ifndef WATR_TOOLCHAIN_H
#define WATR_TOOLCHAIN_H

#include <stdbool.h>
#include <stddef.h>

/* Assembles `asm_text` and links it into a static executable at `out_path`
 * using GNU as and ld. Prints the commands first when `verbose`. */
bool toolchain_build_executable(const char *asm_text, size_t asm_len, const char *out_path, bool verbose);

/* Writes `len` bytes to `path`. Prints an error on failure. */
bool write_file(const char *path, const char *data, size_t len);

#endif
