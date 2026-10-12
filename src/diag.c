#include "diag.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int error_count;
static int use_color = -1;

static int color_enabled(void) {
    if (use_color < 0) {
        const char *no_color = getenv("NO_COLOR");
        use_color = isatty(STDERR_FILENO) && !(no_color && no_color[0]);
    }
    return use_color;
}

#define C_RESET (color_enabled() ? "\x1b[0m" : "")
#define C_BOLD  (color_enabled() ? "\x1b[1m" : "")
#define C_RED   (color_enabled() ? "\x1b[1;31m" : "")
#define C_CYAN  (color_enabled() ? "\x1b[1;36m" : "")
#define C_GREEN (color_enabled() ? "\x1b[1;32m" : "")

void *xmalloc(size_t size) {
    void *p = malloc(size ? size : 1);
    if (!p) fatal("out of memory (wanted %zu bytes)", size);
    return p;
}

void fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%swatrc: fatal:%s ", C_RED, C_RESET);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(2);
}

SrcFile *src_load(Arena *a, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "%swatrc: error:%s cannot open '%s': %s\n", C_RED, C_RESET, path, strerror(errno));
        return NULL;
    }

    size_t cap = 4096, len = 0;
    char *buf = xmalloc(cap);
    for (;;) {
        if (len + 1 >= cap) {
            cap *= 2;
            char *grown = realloc(buf, cap);
            if (!grown) fatal("out of memory reading '%s'", path);
            buf = grown;
        }
        size_t n = fread(buf + len, 1, cap - len - 1, f);
        len += n;
        if (n == 0) break;
    }
    int failed = ferror(f);
    fclose(f);
    if (failed) {
        fprintf(stderr, "%swatrc: error:%s cannot read '%s'\n", C_RED, C_RESET, path);
        free(buf);
        return NULL;
    }
    if (len > UINT32_MAX - 1) {
        fprintf(stderr, "%swatrc: error:%s '%s' is too large\n", C_RED, C_RESET, path);
        free(buf);
        return NULL;
    }

    SrcFile *sf = ARENA_NEW(a, SrcFile);
    sf->path = arena_strndup(a, path, strlen(path));
    sf->text = arena_strndup(a, buf, len);
    sf->len = len;
    free(buf);
    return sf;
}

/* Prints the source line containing `loc` with a caret/underline below it. */
static void print_snippet(Loc loc, const char *color) {
    const SrcFile *f = loc.file;
    if (!f || loc.offset > f->len) return;

    size_t start = loc.offset;
    while (start > 0 && f->text[start - 1] != '\n') start--;
    size_t end = loc.offset;
    while (end < f->len && f->text[end] != '\n') end++;

    char gutter[16];
    int gw = snprintf(gutter, sizeof gutter, "%u", loc.line);

    fprintf(stderr, " %s%s |%s ", C_CYAN, gutter, C_RESET);
    fwrite(f->text + start, 1, end - start, stderr);
    fputc('\n', stderr);

    fprintf(stderr, " %*s %s|%s ", gw, "", C_CYAN, C_RESET);
    /* Copy tabs from the source so the caret lines up however tabs render. */
    for (size_t i = start; i < loc.offset; i++)
        fputc(f->text[i] == '\t' ? '\t' : ' ', stderr);

    size_t underline = loc.len ? loc.len : 1;
    if (loc.offset + underline > end) underline = end > loc.offset ? end - loc.offset : 1;
    fputs(color, stderr);
    fputc('^', stderr);
    for (size_t i = 1; i < underline; i++) fputc('~', stderr);
    fprintf(stderr, "%s\n", C_RESET);
}

static void report(Loc loc, const char *kind, const char *color, const char *fmt, va_list ap) {
    if (loc.file)
        fprintf(stderr, "%s%s:%u:%u:%s ", C_BOLD, loc.file->path, loc.line, loc.col, C_RESET);
    fprintf(stderr, "%s%s:%s %s", color, kind, C_RESET, C_BOLD);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "%s\n", C_RESET);
    print_snippet(loc, color);
}

void diag_verror(Loc loc, const char *fmt, va_list ap) {
    report(loc, "error", C_RED, fmt, ap);
    error_count++;
}

void diag_error(Loc loc, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    diag_verror(loc, fmt, ap);
    va_end(ap);
}

void diag_note(Loc loc, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    report(loc, "note", C_GREEN, fmt, ap);
    va_end(ap);
}

int diag_error_count(void) {
    return error_count;
}
