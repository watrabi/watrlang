#ifndef WATR_DIAG_H
#define WATR_DIAG_H

#include <stdarg.h>
#include <stdbool.h>
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

/* A position in a source file. `len` is how many bytes to underline.
 * line == 0 means "the whole file" (no snippet is shown). */
typedef struct {
    const SrcFile *file;
    uint32_t offset;
    uint32_t line;
    uint32_t col;
    uint32_t len;
} Loc;

/* Reads a whole file into the arena. Prints an error and returns NULL on failure. */
SrcFile *src_load(Arena *a, const char *path);

/* Location of file->text[offset, offset+len), computing line and column. */
Loc loc_at(const SrcFile *file, uint32_t offset, uint32_t len);
/* Zero-width location just past the end of `loc` (e.g. where a missing `;` goes). */
Loc loc_after(Loc loc);

/*
 * Rust-style diagnostics:
 *
 *   error[E0206]: function `two` takes 2 arguments but 1 argument was supplied
 *    --> demo.watr:3:20
 *     |
 *   1 | function two(int a, int b): int { return a + b; }
 *     |          --- `two` is defined here
 *   2 | function main(): int {
 *   3 |     int x = two(1);
 *     |             ^^^^^^ expected 2 arguments
 *     |
 *     = note: ...
 *   help: ...
 *
 * Build one with diag_start, add labels/notes/fixes, then diag_end prints
 * it. Every function accepts NULL and does nothing, so callers that may
 * suppress a diagnostic (the parser in panic mode) don't need to check.
 */

typedef enum { DIAG_ERROR, DIAG_WARNING } DiagLevel;

typedef struct Diag Diag;

/* `code` is like "E0200" or "W0001" (NULL for none). `loc` is the primary span. */
Diag *diag_start(DiagLevel level, const char *code, Loc loc, const char *fmt, ...) WATR_PRINTF(4, 5);
Diag *diag_vstart(DiagLevel level, const char *code, Loc loc, const char *fmt, va_list ap);
/* Text printed next to the primary span's ^^^ marker. */
void diag_label(Diag *d, const char *fmt, ...) WATR_PRINTF(2, 3);
/* A secondary span (--- marker). fmt may be NULL for an unlabeled marker. */
void diag_span(Diag *d, Loc loc, const char *fmt, ...) WATR_PRINTF(3, 4);
/* "= note: ..." and "= help: ..." lines under the snippet. */
void diag_note(Diag *d, const char *fmt, ...) WATR_PRINTF(2, 3);
void diag_help(Diag *d, const char *fmt, ...) WATR_PRINTF(2, 3);
/* "help: ..." followed by the source line with `loc` replaced by
 * `replacement` (a zero-length loc means insertion). */
void diag_fix(Diag *d, Loc loc, const char *replacement, const char *fmt, ...) WATR_PRINTF(4, 5);
/* Prints and frees the diagnostic. */
void diag_end(Diag *d);

/* Shorthand for an error with no code, label or notes. */
void diag_error(Loc loc, const char *fmt, ...) WATR_PRINTF(2, 3);

int  diag_error_count(void);
int  diag_warning_count(void);
/* Prints "error: could not compile ..." plus the --explain hint. */
void diag_print_summary(const char *path);

/* Long-form explanation for `watrc --explain CODE`, or NULL. */
const char *diag_explain(const char *code);

/* Edit distance, for "did you mean ...?" suggestions. */
size_t diag_edit_distance(const char *a, const char *b);
/* Is `candidate` close enough to `name` to suggest? */
bool diag_similar(const char *name, const char *candidate);

/* Internal compiler failures and out-of-memory. Prints and exits. */
void  fatal(const char *fmt, ...) WATR_PRINTF(1, 2) WATR_NORETURN;
void *xmalloc(size_t size);

#endif
