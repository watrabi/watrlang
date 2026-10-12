#ifndef WATR_DIAG_H
#define WATR_DIAG_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "arena.h"

#if defined(__GNUC__) || defined(__clang__)
#define WATR_PRINTF(fmt, args) __attribute__((format(printf, fmt, args)))
#define WATR_NORETURN __attribute__((noreturn))
#else
#define WATR_PRINTF(fmt, args)
#define WATR_NORETURN
#endif

/* A loaded source file. `text` is NUL-terminated; `len` excludes the NUL. */
typedef struct {
    const char *path;
    const char *text;
    size_t len;
} SrcFile;

/* A position in a source file. `len` is how many bytes to underline (0 = just a caret). */
typedef struct {
    const SrcFile *file;
    uint32_t offset;
    uint32_t line;
    uint32_t col;
    uint32_t len;
} Loc;

/* Reads a whole file into the arena. Prints an error and returns NULL on failure. */
SrcFile *src_load(Arena *a, const char *path);

void diag_error(Loc loc, const char *fmt, ...) WATR_PRINTF(2, 3);
void diag_verror(Loc loc, const char *fmt, va_list ap);
void diag_note(Loc loc, const char *fmt, ...) WATR_PRINTF(2, 3);
int  diag_error_count(void);

/* Internal compiler failures and out-of-memory. Prints and exits. */
void  fatal(const char *fmt, ...) WATR_PRINTF(1, 2) WATR_NORETURN;
void *xmalloc(size_t size);

#endif
