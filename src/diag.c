#include "diag.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ---- colors ---------------------------------------------------------------- */

static int use_color = -1;

static int color_enabled(void) {
    if (use_color < 0) {
        const char *no_color = getenv("NO_COLOR");
        use_color = isatty(STDERR_FILENO) && !(no_color && no_color[0]);
    }
    return use_color;
}

#define C_RESET  (color_enabled() ? "\x1b[0m" : "")
#define C_BOLD   (color_enabled() ? "\x1b[1m" : "")
#define C_RED    (color_enabled() ? "\x1b[1;31m" : "")
#define C_YELLOW (color_enabled() ? "\x1b[1;33m" : "")
#define C_BLUE   (color_enabled() ? "\x1b[1;34m" : "")
#define C_CYAN   (color_enabled() ? "\x1b[1;36m" : "")
#define C_GREEN  (color_enabled() ? "\x1b[1;32m" : "")

/* ---- basics ---------------------------------------------------------------- */

static int error_count;
static int warning_count;

/* Codes that have been reported, for the --explain hint. */
#define MAX_SEEN_CODES 64
static const char *seen_codes[MAX_SEEN_CODES];
static int nseen_codes;

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

static char *vformat(const char *fmt, va_list ap) {
    if (!fmt) fmt = "";
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n < 0) n = 0;
    char *s = xmalloc((size_t)n + 1);
    vsnprintf(s, (size_t)n + 1, fmt, ap);
    return s;
}

static char *vformat_copy(const char *s) {
    size_t n = strlen(s) + 1;
    char *c = xmalloc(n);
    memcpy(c, s, n);
    return c;
}

SrcFile *src_load(Arena *a, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "%serror%s: cannot open `%s`: %s\n", C_RED, C_RESET, path, strerror(errno));
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
    if (failed || len > UINT32_MAX - 1) {
        fprintf(stderr, "%serror%s: cannot read `%s`%s\n", C_RED, C_RESET, path, failed ? "" : ": file is too large");
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

Loc loc_at(const SrcFile *file, uint32_t offset, uint32_t len) {
    Loc loc = {file, offset, 1, 1, len};
    for (uint32_t i = 0; i < offset && i < file->len; i++) {
        if (file->text[i] == '\n') {
            loc.line++;
            loc.col = 1;
        } else {
            loc.col++;
        }
    }
    return loc;
}

Loc loc_after(Loc loc) {
    if (!loc.file) return loc;
    return loc_at(loc.file, loc.offset + loc.len, 0);
}

/* ---- diagnostic objects ---------------------------------------------------- */

#define MAX_LABELS 8
#define MAX_NOTES 8
#define MAX_FIXES 3

typedef struct {
    Loc loc;
    char *text;      /* may be NULL */
    bool primary;
} Label;

typedef struct {
    bool is_help;
    char *text;
} Note;

typedef struct {
    Loc loc;
    char *replacement;
    char *text;
} Fix;

struct Diag {
    DiagLevel level;
    const char *code;
    char *message;
    Label labels[MAX_LABELS];
    int nlabels;
    Note notes[MAX_NOTES];
    int nnotes;
    Fix fixes[MAX_FIXES];
    int nfixes;
};

Diag *diag_vstart(DiagLevel level, const char *code, Loc loc, const char *fmt, va_list ap) {
    Diag *d = xmalloc(sizeof *d);
    memset(d, 0, sizeof *d);
    d->level = level;
    d->code = code;
    d->message = vformat(fmt, ap);
    d->labels[0].loc = loc;
    d->labels[0].primary = true;
    d->nlabels = 1;
    return d;
}

Diag *diag_start(DiagLevel level, const char *code, Loc loc, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    Diag *d = diag_vstart(level, code, loc, fmt, ap);
    va_end(ap);
    return d;
}

void diag_label(Diag *d, const char *fmt, ...) {
    if (!d) return;
    va_list ap;
    va_start(ap, fmt);
    free(d->labels[0].text);
    d->labels[0].text = vformat(fmt, ap);
    va_end(ap);
}

void diag_span(Diag *d, Loc loc, const char *fmt, ...) {
    if (!d || d->nlabels == MAX_LABELS || !loc.file) return;
    Label *l = &d->labels[d->nlabels++];
    l->loc = loc;
    l->primary = false;
    l->text = NULL;
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        l->text = vformat(fmt, ap);
        va_end(ap);
    }
}

static void add_note(Diag *d, bool is_help, const char *fmt, va_list ap) {
    if (!d || d->nnotes == MAX_NOTES) return;
    d->notes[d->nnotes].is_help = is_help;
    d->notes[d->nnotes].text = vformat(fmt, ap);
    d->nnotes++;
}

void diag_note(Diag *d, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    add_note(d, false, fmt, ap);
    va_end(ap);
}

/* Generic instantiations being checked, innermost last. */
#define MAX_CONTEXTS 16
static char *contexts[MAX_CONTEXTS];
static int ncontexts, context_depth;

void diag_push_context(Loc used_at, const char *fmt, ...) {
    if (context_depth++ >= MAX_CONTEXTS) return;
    va_list ap;
    va_start(ap, fmt);
    char *what = vformat(fmt, ap);
    va_end(ap);
    size_t n = strlen(what) + (used_at.file ? strlen(used_at.file->path) : 0) + 64;
    char *text = xmalloc(n);
    if (used_at.file) snprintf(text, n, "%s, used at %s:%u:%u", what, used_at.file->path, used_at.line, used_at.col);
    else snprintf(text, n, "%s", what);
    free(what);
    contexts[ncontexts++] = text;
}

void diag_pop_context(void) {
    if (context_depth == 0) return;
    if (context_depth-- > MAX_CONTEXTS) return;
    free(contexts[--ncontexts]);
}

void diag_help(Diag *d, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    add_note(d, true, fmt, ap);
    va_end(ap);
}

void diag_fix(Diag *d, Loc loc, const char *replacement, const char *fmt, ...) {
    if (!d || d->nfixes == MAX_FIXES || !loc.file || loc.line == 0) return;
    Fix *f = &d->fixes[d->nfixes++];
    f->loc = loc;
    size_t n = strlen(replacement);
    f->replacement = xmalloc(n + 1);
    memcpy(f->replacement, replacement, n + 1);
    va_list ap;
    va_start(ap, fmt);
    f->text = vformat(fmt, ap);
    va_end(ap);
}

/* ---- rendering ------------------------------------------------------------- */

typedef struct {
    const char *start;   /* first byte of the line */
    size_t len;          /* without the newline */
} LineText;

static LineText line_text(const SrcFile *f, uint32_t line) {
    LineText lt = {f->text, 0};
    uint32_t cur = 1;
    size_t i = 0;
    while (cur < line && i < f->len) {
        if (f->text[i++] == '\n') cur++;
    }
    lt.start = f->text + i;
    while (i + lt.len < f->len && f->text[i + lt.len] != '\n') lt.len++;
    if (lt.len && lt.start[lt.len - 1] == '\r') lt.len--;
    return lt;
}

/* Display width of bytes [0, n) of a line: tabs are 4 columns and UTF-8
 * continuation bytes take none. */
static size_t visual_width(const char *s, size_t n) {
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\t') w += 4;
        else if ((c & 0xC0) != 0x80) w++;
    }
    return w;
}

static void print_line_source(LineText lt) {
    for (size_t i = 0; i < lt.len; i++) {
        if (lt.start[i] == '\t') fputs("    ", stderr);
        else fputc(lt.start[i], stderr);
    }
}

static void spaces(size_t n) {
    for (size_t i = 0; i < n; i++) fputc(' ', stderr);
}

static int digits(uint32_t n) {
    int d = 1;
    while (n >= 10) {
        n /= 10;
        d++;
    }
    return d;
}

static void gutter(int width) {
    fprintf(stderr, "%s", C_BLUE);
    spaces((size_t)width + 1);
    fprintf(stderr, "|%s", C_RESET);
}

static void numbered_gutter(int width, uint32_t line) {
    fprintf(stderr, "%s%*u |%s ", C_BLUE, width, line, C_RESET);
}

/* Where a label sits on its line, in display columns. */
typedef struct {
    const Label *label;
    size_t col;      /* display column of the first marker */
    size_t width;    /* number of markers */
} Placed;

static void place(const Label *l, LineText lt, Placed *out) {
    size_t start = l->loc.col - 1;
    if (start > lt.len) start = lt.len;
    size_t end = start + l->loc.len;
    if (end > lt.len) end = lt.len;   /* multi-line spans are cut at the line end */
    out->label = l;
    out->col = visual_width(lt.start, start);
    out->width = end > start ? visual_width(lt.start + start, end - start) : 0;
    if (out->width == 0) out->width = 1;
}

static const char *label_color(const Diag *d, const Label *l) {
    if (!l->primary) return C_BLUE;
    return d->level == DIAG_ERROR ? C_RED : C_YELLOW;
}

/* Prints the marker rows for every label on one line, Rust-style:
 *
 *     return v() + nope();
 *            ---   ^^^^ not found
 *            |
 *            this returns nothing
 */
static void print_labels_for_line(const Diag *d, LineText lt, const Label **labels, int n_in, int width) {
    if (n_in <= 0) return;
    Placed all[MAX_LABELS];
    for (int i = 0; i < n_in; i++) place(labels[i], lt, &all[i]);

    /* Labels overlapping an earlier marker can't share its row; they get
     * a row of their own after the others. */
    Placed placed[MAX_LABELS], overlapping[MAX_LABELS];
    int n = 0, nover = 0;
    for (int i = 0; i < n_in; i++) {
        bool clash = false;
        for (int j = 0; j < n; j++)
            clash |= all[i].col < placed[j].col + placed[j].width && placed[j].col < all[i].col + all[i].width;
        if (clash) overlapping[nover++] = all[i];
        else placed[n++] = all[i];
    }
    /* sort by column */
    for (int i = 1; i < n; i++) {
        Placed key = placed[i];
        int j = i - 1;
        while (j >= 0 && placed[j].col > key.col) {
            placed[j + 1] = placed[j];
            j--;
        }
        placed[j + 1] = key;
    }

    /* Row 1: all markers, then the rightmost label's text. */
    gutter(width);
    fputc(' ', stderr);
    size_t at = 0;
    for (int i = 0; i < n; i++) {
        spaces(placed[i].col - at);
        fputs(label_color(d, placed[i].label), stderr);
        char mark = placed[i].label->primary ? '^' : '-';
        for (size_t k = 0; k < placed[i].width; k++) fputc(mark, stderr);
        fputs(C_RESET, stderr);
        at = placed[i].col + placed[i].width;
    }
    const Placed *last = &placed[n - 1];
    if (last->label->text)
        fprintf(stderr, " %s%s%s", label_color(d, last->label), last->label->text, C_RESET);
    fputc('\n', stderr);

    /* The other labels with text hang below, right to left, each joined to
     * its marker by a `|`. */
    for (int i = n - 2; i >= 0; i--) {
        if (!placed[i].label->text) continue;
        for (int row = 0; row < 2; row++) {
            gutter(width);
            fputc(' ', stderr);
            size_t pos = 0;
            for (int j = 0; j <= i; j++) {
                if (!placed[j].label->text || placed[j].col < pos) continue;
                spaces(placed[j].col - pos);
                fputs(label_color(d, placed[j].label), stderr);
                if (j == i && row == 1) {
                    fputs(placed[j].label->text, stderr);
                    pos = placed[j].col + strlen(placed[j].label->text);
                } else {
                    fputc('|', stderr);
                    pos = placed[j].col + 1;
                }
                fputs(C_RESET, stderr);
            }
            fputc('\n', stderr);
        }
    }

    for (int i = 0; i < nover; i++) {
        gutter(width);
        fputc(' ', stderr);
        spaces(overlapping[i].col);
        fputs(label_color(d, overlapping[i].label), stderr);
        char mark = overlapping[i].label->primary ? '^' : '-';
        for (size_t k = 0; k < overlapping[i].width; k++) fputc(mark, stderr);
        if (overlapping[i].label->text) fprintf(stderr, " %s", overlapping[i].label->text);
        fprintf(stderr, "%s\n", C_RESET);
    }
}

/* Prints the snippet for all labels in `file`. */
static void print_snippet(const Diag *d, const SrcFile *file, int width) {
    const Label *in_file[MAX_LABELS];
    int n = 0;
    for (int i = 0; i < d->nlabels; i++)
        if (d->labels[i].loc.file == file && d->labels[i].loc.line > 0) in_file[n++] = &d->labels[i];
    if (n == 0) return;

    /* Distinct lines, in order. */
    uint32_t lines[MAX_LABELS];
    int nlines = 0;
    for (int i = 0; i < n; i++) {
        uint32_t line = in_file[i]->loc.line;
        bool seen = false;
        for (int j = 0; j < nlines; j++) seen |= lines[j] == line;
        if (!seen) lines[nlines++] = line;
    }
    for (int i = 1; i < nlines; i++) {
        uint32_t key = lines[i];
        int j = i - 1;
        while (j >= 0 && lines[j] > key) {
            lines[j + 1] = lines[j];
            j--;
        }
        lines[j + 1] = key;
    }

    gutter(width);
    fputc('\n', stderr);
    for (int i = 0; i < nlines; i++) {
        if (i > 0) {
            uint32_t gap = lines[i] - lines[i - 1];
            if (gap == 2) {
                /* One line in between: show it rather than "...". */
                numbered_gutter(width, lines[i] - 1);
                print_line_source(line_text(file, lines[i] - 1));
                fputc('\n', stderr);
            } else if (gap > 2) {
                fprintf(stderr, "%s...%s\n", C_BLUE, C_RESET);
            }
        }
        LineText lt = line_text(file, lines[i]);
        numbered_gutter(width, lines[i]);
        print_line_source(lt);
        fputc('\n', stderr);

        const Label *on_line[MAX_LABELS];
        int k = 0;
        for (int j = 0; j < n; j++)
            if (in_file[j]->loc.line == lines[i]) on_line[k++] = in_file[j];
        print_labels_for_line(d, lt, on_line, k, width);
    }
}

static void print_fix(const Fix *f, int width) {
    fprintf(stderr, "%shelp%s%s: %s%s\n", C_CYAN, C_RESET, C_BOLD, f->text, C_RESET);
    LineText lt = line_text(f->loc.file, f->loc.line);
    size_t start = f->loc.col - 1;
    if (start > lt.len) start = lt.len;
    size_t end = start + f->loc.len;
    if (end > lt.len) end = lt.len;
    size_t rep_len = strlen(f->replacement);

    gutter(width);
    fputc('\n', stderr);
    numbered_gutter(width, f->loc.line);
    LineText before = {lt.start, start};
    LineText after = {lt.start + end, lt.len - end};
    print_line_source(before);
    if (rep_len) fprintf(stderr, "%s%s%s", C_GREEN, f->replacement, C_RESET);
    else {
        LineText removed = {lt.start + start, end - start};
        fputs(C_RED, stderr);
        print_line_source(removed);
        fputs(C_RESET, stderr);
    }
    print_line_source(after);
    fputc('\n', stderr);

    /* + for an insertion, ~ for a replacement, - for a deletion */
    gutter(width);
    fputc(' ', stderr);
    spaces(visual_width(lt.start, start));
    if (rep_len) {
        fputs(C_GREEN, stderr);
        char mark = end == start ? '+' : '~';
        for (size_t i = 0; i < visual_width(f->replacement, rep_len); i++) fputc(mark, stderr);
    } else {
        fputs(C_RED, stderr);
        for (size_t i = 0; i < visual_width(lt.start + start, end - start); i++) fputc('-', stderr);
    }
    fprintf(stderr, "%s\n", C_RESET);
}

static void remember_code(const char *code) {
    if (!code) return;
    for (int i = 0; i < nseen_codes; i++)
        if (strcmp(seen_codes[i], code) == 0) return;
    if (nseen_codes < MAX_SEEN_CODES) seen_codes[nseen_codes++] = code;
}

static void free_diag(Diag *d) {
    free(d->message);
    for (int i = 0; i < d->nlabels; i++) free(d->labels[i].text);
    for (int i = 0; i < d->nnotes; i++) free(d->notes[i].text);
    for (int i = 0; i < d->nfixes; i++) {
        free(d->fixes[i].replacement);
        free(d->fixes[i].text);
    }
    free(d);
}

/* A warning inside a generic instantiation is about the generic code, so
 * it's shown once, not once per instance. */
static bool seen_in_instance(const Diag *d) {
    static struct { const SrcFile *file; uint32_t offset; const char *code; } seen[256];
    static int nseen;
    Loc at = d->labels[0].loc;
    for (int i = 0; i < nseen; i++)
        if (seen[i].file == at.file && seen[i].offset == at.offset && seen[i].code == d->code) return true;
    if (nseen < 256) {
        seen[nseen].file = at.file;
        seen[nseen].offset = at.offset;
        seen[nseen].code = d->code;
        nseen++;
    }
    return false;
}

void diag_end(Diag *d) {
    if (!d) return;
    bool is_error = d->level == DIAG_ERROR;
    if (!is_error && ncontexts > 0 && d->nlabels > 0 && seen_in_instance(d)) {
        free_diag(d);
        return;
    }
    if (is_error) error_count++;
    else warning_count++;
    remember_code(d->code);
    /* The instantiations being checked: the innermost two and the
     * outermost one (where the user's code started it). */
    for (int i = ncontexts - 1; i >= 0 && d->nnotes < MAX_NOTES - 1; i--) {
        d->notes[d->nnotes].is_help = false;
        if (ncontexts > 4 && i == ncontexts - 3) {
            char buf[64];
            snprintf(buf, sizeof buf, "... and %d more instantiations, inside", ncontexts - 3);
            d->notes[d->nnotes++].text = vformat_copy(buf);
            i = 0;
        }
        d->notes[d->nnotes++].text = vformat_copy(contexts[i]);
    }

    /* Header */
    const char *color = is_error ? C_RED : C_YELLOW;
    fprintf(stderr, "%s%s", color, is_error ? "error" : "warning");
    if (d->code) fprintf(stderr, "[%s]", d->code);
    fprintf(stderr, "%s%s: %s%s\n", C_RESET, C_BOLD, d->message, C_RESET);

    /* Gutter wide enough for every line number we'll print. */
    uint32_t max_line = 1;
    for (int i = 0; i < d->nlabels; i++)
        if (d->labels[i].loc.line > max_line) max_line = d->labels[i].loc.line;
    for (int i = 0; i < d->nfixes; i++)
        if (d->fixes[i].loc.line > max_line) max_line = d->fixes[i].loc.line;
    int width = digits(max_line);

    /* Location and snippet of the primary file, then any other files. */
    const Loc *primary = &d->labels[0].loc;
    if (primary->file) {
        spaces((size_t)width);
        if (primary->line > 0)
            fprintf(stderr, "%s-->%s %s:%u:%u\n", C_BLUE, C_RESET, primary->file->path, primary->line, primary->col);
        else
            fprintf(stderr, "%s-->%s %s\n", C_BLUE, C_RESET, primary->file->path);
        print_snippet(d, primary->file, width);
    }
    for (int i = 1; i < d->nlabels; i++) {
        const SrcFile *f = d->labels[i].loc.file;
        bool shown = f == primary->file;
        for (int j = 1; j < i && !shown; j++) shown = d->labels[j].loc.file == f;
        if (shown) continue;
        spaces((size_t)width);
        fprintf(stderr, "%s:::%s %s:%u:%u\n", C_BLUE, C_RESET, f->path, d->labels[i].loc.line, d->labels[i].loc.col);
        print_snippet(d, f, width);
    }

    if (d->nnotes) {
        if (primary->file) {
            gutter(width);
            fputc('\n', stderr);
        }
        for (int i = 0; i < d->nnotes; i++) {
            spaces((size_t)width + 1);
            fprintf(stderr, "%s=%s %s%s:%s %s\n", C_BLUE, C_RESET, C_BOLD, d->notes[i].is_help ? "help" : "note",
                    C_RESET, d->notes[i].text);
        }
    }
    if (d->nfixes && !d->nnotes && primary->file && primary->line > 0) {
        gutter(width);
        fputc('\n', stderr);
    }
    for (int i = 0; i < d->nfixes; i++) print_fix(&d->fixes[i], width);
    fputc('\n', stderr);

    free_diag(d);
}

void diag_error(Loc loc, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    Diag *d = diag_vstart(DIAG_ERROR, NULL, loc, fmt, ap);
    va_end(ap);
    diag_end(d);
}

int diag_error_count(void) {
    return error_count;
}

int diag_warning_count(void) {
    return warning_count;
}

void diag_print_summary(const char *path) {
    if (error_count) {
        fprintf(stderr, "%serror%s%s: could not compile `%s` due to %d previous error%s", C_RED, C_RESET, C_BOLD, path,
                error_count, error_count == 1 ? "" : "s");
        if (warning_count)
            fprintf(stderr, "; %d warning%s emitted", warning_count, warning_count == 1 ? "" : "s");
        fprintf(stderr, "%s\n", C_RESET);
    } else if (warning_count) {
        fprintf(stderr, "%swarning%s%s: `%s` generated %d warning%s%s\n", C_YELLOW, C_RESET, C_BOLD, path,
                warning_count, warning_count == 1 ? "" : "s", C_RESET);
    }

    /* Point at --explain for the codes that have explanations. */
    int explained = 0;
    for (int i = 0; i < nseen_codes; i++) explained += diag_explain(seen_codes[i]) != NULL;
    if (explained == 0) return;
    const char *what = error_count ? "error" : "warning";
    if (explained == 1) fprintf(stderr, "%sThis %s has a detailed explanation: ", C_BOLD, what);
    else fprintf(stderr, "%sSome %ss have detailed explanations: ", C_BOLD, what);
    const char *first = NULL;
    int printed = 0;
    for (int i = 0; i < nseen_codes; i++) {
        if (!diag_explain(seen_codes[i])) continue;
        if (!first) first = seen_codes[i];
        fprintf(stderr, "%s%s", printed++ ? ", " : "", seen_codes[i]);
    }
    fprintf(stderr, ".%s\n", C_RESET);
    fprintf(stderr, "%sFor more information about %s %s, try `watrc --explain %s`.%s\n", C_BOLD,
            error_count ? "an" : "a", what, first, C_RESET);
}

/* ---- suggestions ----------------------------------------------------------- */

size_t diag_edit_distance(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la > 64 || lb > 64) return (size_t)-1;
    size_t row[65];
    for (size_t j = 0; j <= lb; j++) row[j] = j;
    for (size_t i = 1; i <= la; i++) {
        size_t diag = row[0];
        row[0] = i;
        for (size_t j = 1; j <= lb; j++) {
            size_t up = row[j];
            size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
            size_t best = diag + cost;
            if (up + 1 < best) best = up + 1;
            if (row[j - 1] + 1 < best) best = row[j - 1] + 1;
            row[j] = best;
            diag = up;
        }
    }
    return row[lb];
}

bool diag_similar(const char *name, const char *candidate) {
    if (strcmp(name, candidate) == 0) return false;
    size_t len = strlen(name);
    size_t limit = len < 3 ? 1 : len / 3 + 1;
    return diag_edit_distance(name, candidate) <= limit;
}
