#include "lexer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const kind_names[TOK_COUNT] = {
#define X(name, spelling) #name,
    WATR_TOKENS(X)
#undef X
#define X(name, spelling) "KW_" #name,
    WATR_KEYWORDS(X)
#undef X
};

static const char *const kind_spellings[TOK_COUNT] = {
#define X(name, spelling) spelling,
    WATR_TOKENS(X)
    WATR_KEYWORDS(X)
#undef X
};

const char *token_kind_name(TokenKind k) {
    return k < TOK_COUNT ? kind_names[k] : "?";
}

const char *token_kind_spelling(TokenKind k) {
    return k < TOK_COUNT ? kind_spellings[k] : "?";
}

/* Longest spellings first so "<<=" wins over "<<" wins over "<". */
static const struct {
    const char *text;
    TokenKind kind;
} puncts[] = {
    {"<<=", TOK_SHL_ASSIGN}, {">>=", TOK_SHR_ASSIGN}, {"...", TOK_ELLIPSIS},

    {"->", TOK_ARROW}, {"=>", TOK_FATARROW}, {"::", TOK_COLONCOLON},
    {"++", TOK_PLUSPLUS}, {"--", TOK_MINUSMINUS},
    {"+=", TOK_PLUS_ASSIGN}, {"-=", TOK_MINUS_ASSIGN}, {"*=", TOK_STAR_ASSIGN},
    {"/=", TOK_SLASH_ASSIGN}, {"%=", TOK_PERCENT_ASSIGN}, {".=", TOK_DOT_ASSIGN},
    {"&=", TOK_AMP_ASSIGN}, {"|=", TOK_PIPE_ASSIGN}, {"^=", TOK_CARET_ASSIGN},
    {"==", TOK_EQ}, {"!=", TOK_NE}, {"<=", TOK_LE}, {">=", TOK_GE},
    {"&&", TOK_ANDAND}, {"||", TOK_OROR}, {"<<", TOK_SHL}, {">>", TOK_SHR},

    {"(", TOK_LPAREN}, {")", TOK_RPAREN}, {"{", TOK_LBRACE}, {"}", TOK_RBRACE},
    {"[", TOK_LBRACKET}, {"]", TOK_RBRACKET}, {",", TOK_COMMA}, {";", TOK_SEMI},
    {":", TOK_COLON}, {"?", TOK_QUESTION}, {"+", TOK_PLUS}, {"-", TOK_MINUS},
    {"*", TOK_STAR}, {"/", TOK_SLASH}, {"%", TOK_PERCENT}, {".", TOK_DOT},
    {"&", TOK_AMP}, {"|", TOK_PIPE}, {"^", TOK_CARET}, {"~", TOK_TILDE},
    {"!", TOK_BANG}, {"<", TOK_LT}, {">", TOK_GT}, {"=", TOK_ASSIGN},
};

static bool is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool is_ident_char(char c) {
    return is_ident_start(c) || is_digit(c);
}

/* Value of `c` as a digit in `base`, or -1. */
static int digit_value(char c, int base) {
    int d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
    else return -1;
    return d < base ? d : -1;
}

void lexer_init(Lexer *l, const SrcFile *file) {
    l->file = file;
    l->src = file->text;
    l->len = file->len;
    l->pos = 0;
    l->line = 1;
    l->col = 1;
    /* Skip a UTF-8 byte order mark. */
    if (l->len >= 3 && memcmp(l->src, "\xEF\xBB\xBF", 3) == 0) l->pos = 3;
}

void lexer_init_range(Lexer *l, const SrcFile *file, size_t start, size_t end, uint32_t line, uint32_t col) {
    l->file = file;
    l->src = file->text;
    l->len = end;
    l->pos = start;
    l->line = line;
    l->col = col;
}

static char peek(const Lexer *l, size_t ahead) {
    return l->pos + ahead < l->len ? l->src[l->pos + ahead] : '\0';
}

static bool at_end(const Lexer *l) {
    return l->pos >= l->len;
}

static char advance(Lexer *l) {
    char c = l->src[l->pos++];
    if (c == '\n') {
        l->line++;
        l->col = 1;
    } else {
        l->col++;
    }
    return c;
}

static Loc here(const Lexer *l) {
    Loc loc = {l->file, (uint32_t)l->pos, l->line, l->col, 0};
    return loc;
}

static Loc span_from(const Lexer *l, Loc start) {
    start.len = (uint32_t)(l->pos - start.offset);
    return start;
}

static void skip_trivia(Lexer *l) {
    for (;;) {
        char c = peek(l, 0);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance(l);
        } else if (c == '/' && peek(l, 1) == '/') {
            while (!at_end(l) && peek(l, 0) != '\n') advance(l);
        } else if (c == '/' && peek(l, 1) == '*') {
            Loc start = here(l);
            advance(l);
            advance(l);
            for (;;) {
                if (at_end(l)) {
                    start.len = 2;
                    Diag *d = diag_start(DIAG_ERROR, "E0001", start, "unterminated block comment");
                    diag_label(d, "this comment is never closed");
                    diag_help(d, "add `*/` where the comment should end");
                    diag_end(d);
                    return;
                }
                if (peek(l, 0) == '*' && peek(l, 1) == '/') {
                    advance(l);
                    advance(l);
                    break;
                }
                advance(l);
            }
        } else {
            return;
        }
    }
}

static void report_non_ascii(Loc loc) {
    Diag *d = diag_start(DIAG_ERROR, "E0015", loc, "identifiers must be ASCII");
    diag_label(d, "contains non-ASCII characters");
    diag_note(d, "names may use ASCII letters, digits and `_`; any UTF-8 is fine in strings and comments");
    diag_end(d);
}

static TokenKind keyword_or_ident(const char *s, size_t n) {
    static const struct {
        const char *text;
        TokenKind kind;
    } keywords[] = {
#define X(name, spelling) {spelling, TOK_KW_##name},
        WATR_KEYWORDS(X)
#undef X
    };
    for (size_t i = 0; i < sizeof keywords / sizeof keywords[0]; i++) {
        if (strlen(keywords[i].text) == n && memcmp(keywords[i].text, s, n) == 0)
            return keywords[i].kind;
    }
    return TOK_IDENT;
}

/* Lexes digits in `base`, allowing `_` separators between digits.
 * Accumulates into *val and sets *overflow if it doesn't fit in u64.
 * Returns the number of digits consumed (not counting underscores). */
static size_t lex_digits(Lexer *l, int base, uint64_t *val, bool *overflow, bool *bad_underscore) {
    size_t ndigits = 0;
    bool last_underscore = false;
    for (;;) {
        char c = peek(l, 0);
        if (c == '_') {
            if (ndigits == 0 || last_underscore) *bad_underscore = true;
            last_underscore = true;
            advance(l);
            continue;
        }
        int d = digit_value(c, base);
        if (d < 0) break;
        if (*val > (UINT64_MAX - (uint64_t)d) / (uint64_t)base) *overflow = true;
        *val = *val * (uint64_t)base + (uint64_t)d;
        ndigits++;
        last_underscore = false;
        advance(l);
    }
    if (last_underscore) *bad_underscore = true;
    return ndigits;
}

static Token lex_number(Lexer *l) {
    Loc start = here(l);
    Token t = {TOK_INT, 0, start, l->src + l->pos, {0}};
    uint64_t val = 0;
    bool overflow = false, bad_underscore = false, is_float = false;

    int base = 10;
    if (peek(l, 0) == '0') {
        char p = peek(l, 1);
        if (p == 'x' || p == 'X') base = 16;
        else if (p == 'b' || p == 'B') base = 2;
        else if (p == 'o' || p == 'O') base = 8;
    }

    if (base != 10) {
        advance(l);
        advance(l);
        if (lex_digits(l, base, &val, &overflow, &bad_underscore) == 0) {
            /* Report the bad digit itself if there is one, e.g. 0xZZ. */
            while (is_ident_char(peek(l, 0))) advance(l);
            const char *kind = base == 16 ? "hex" : base == 2 ? "binary" : "octal";
            Diag *d = diag_start(DIAG_ERROR, "E0003", span_from(l, start), "missing %s digits after `%.2s`", kind, t.start);
            diag_label(d, "expected %s digits", kind);
            diag_note(d, "%s digits are %s", kind, base == 16 ? "0-9 and a-f" : base == 2 ? "0 and 1" : "0-7");
            diag_end(d);
            t.kind = TOK_ERROR;
            t.loc = span_from(l, start);
            return t;
        }
    } else {
        lex_digits(l, 10, &val, &overflow, &bad_underscore);
        size_t int_len = l->pos - start.offset;

        /* `1.5` is a float; `1.` followed by a non-digit is `1` then `.` (concat). */
        if (peek(l, 0) == '.' && is_digit(peek(l, 1))) {
            is_float = true;
            advance(l);
            uint64_t ignored = 0;
            bool ignored_overflow = false;
            lex_digits(l, 10, &ignored, &ignored_overflow, &bad_underscore);
        }
        if (peek(l, 0) == 'e' || peek(l, 0) == 'E') {
            size_t sign = (peek(l, 1) == '+' || peek(l, 1) == '-') ? 1 : 0;
            if (is_digit(peek(l, 1 + sign))) {
                is_float = true;
                advance(l);
                if (sign) advance(l);
                uint64_t ignored = 0;
                bool ignored_overflow = false;
                lex_digits(l, 10, &ignored, &ignored_overflow, &bad_underscore);
            }
        }

        if (int_len > 1 && t.start[0] == '0' && is_digit(t.start[1])) {
            Loc zl = start;
            zl.len = (uint32_t)int_len;
            size_t skip = 0;
            while (skip + 1 < int_len && (t.start[skip] == '0' || t.start[skip] == '_')) skip++;
            char fixed[80];
            int digits_len = (int)(int_len - skip) < 60 ? (int)(int_len - skip) : 60;
            Diag *d = diag_start(DIAG_ERROR, "E0002", zl, "integer literal `%.*s` has a leading zero", (int)int_len, t.start);
            diag_label(d, "C would silently read this as octal");
            snprintf(fixed, sizeof fixed, "%.*s", digits_len, t.start + skip);
            diag_fix(d, zl, fixed, "if you meant a decimal number, remove the leading zero");
            snprintf(fixed, sizeof fixed, "0o%.*s", digits_len, t.start + skip);
            diag_fix(d, zl, fixed, "if you meant octal, use the `0o` prefix");
            diag_end(d);
            t.kind = TOK_ERROR;
        }
    }

    if (is_ident_char(peek(l, 0))) {
        Loc bad = here(l);
        while (is_ident_char(peek(l, 0))) advance(l);
        bad = span_from(l, bad);
        char first = t.start[bad.offset - start.offset];
        Diag *d;
        if (base != 10 && (is_digit(first) || base == 16)) {
            const char *kind = base == 16 ? "hex" : base == 2 ? "binary" : "octal";
            bad.len = 1;
            d = diag_start(DIAG_ERROR, "E0004", bad, "invalid digit `%c` in %s literal", first, kind);
            diag_label(d, "not a %s digit", kind);
            diag_note(d, "%s digits are %s", kind, base == 16 ? "0-9 and a-f" : base == 2 ? "0 and 1" : "0-7");
        } else {
            d = diag_start(DIAG_ERROR, "E0004", bad, "invalid suffix `%.*s` on number", (int)bad.len,
                           t.start + (bad.offset - start.offset));
            diag_label(d, "invalid suffix");
            diag_help(d, "watrlang numbers have no suffixes; to pick a type, cast: `10 as u64`");
            diag_help(d, "if this is a separate name, put an operator or space before it");
        }
        diag_end(d);
        t.kind = TOK_ERROR;
    }

    t.loc = span_from(l, start);
    if (t.kind == TOK_ERROR) return t;

    if (bad_underscore) {
        Diag *d = diag_start(DIAG_ERROR, "E0005", t.loc, "`_` must be between two digits");
        diag_label(d, "misplaced `_` in this number");
        diag_help(d, "underscores only separate digits, as in `1_000_000`");
        diag_end(d);
        t.kind = TOK_ERROR;
        return t;
    }

    if (is_float) {
        /* strtod doesn't understand '_', so copy the literal without them. */
        char *buf = xmalloc(t.loc.len + 1);
        size_t n = 0;
        for (uint32_t i = 0; i < t.loc.len; i++)
            if (t.start[i] != '_') buf[n++] = t.start[i];
        buf[n] = '\0';
        t.kind = TOK_FLOAT;
        t.v.float_val = strtod(buf, NULL);
        free(buf);
    } else if (overflow) {
        Diag *d = diag_start(DIAG_ERROR, "E0006", t.loc, "integer literal is too large");
        diag_label(d, "doesn't fit in 64 bits");
        diag_note(d, "the largest integer literal is 18446744073709551615 (2^64 - 1)");
        diag_end(d);
        t.kind = TOK_ERROR;
    } else {
        t.v.int_val = val;
    }
    return t;
}

/* Lexes the escape after a backslash. Returns the byte value or -1 on error. */
static int lex_escape(Lexer *l) {
    Loc start = here(l);
    start.offset--; /* include the backslash */
    start.col--;
    if (at_end(l)) return -1;
    char c = advance(l);
    switch (c) {
    case 'n':  return '\n';
    case 't':  return '\t';
    case 'r':  return '\r';
    case '0':  return '\0';
    case '\\': return '\\';
    case '"':  return '"';
    case '\'': return '\'';
    case '{':  return '{';
    case '}':  return '}';
    case 'x': {
        int hi = digit_value(peek(l, 0), 16);
        int lo = hi >= 0 ? digit_value(peek(l, 1), 16) : -1;
        if (lo < 0) {
            start.len = 2;
            Diag *d = diag_start(DIAG_ERROR, "E0008", start, "`\\x` escape needs exactly two hex digits");
            diag_label(d, "incomplete escape");
            diag_help(d, "for example, `\\x41` is the byte 0x41 (`A`)");
            diag_end(d);
            return -1;
        }
        advance(l);
        advance(l);
        return hi * 16 + lo;
    }
    default:
        start.len = 2;
        Diag *d;
        if (c == '\n') {
            start.len = 1;
            d = diag_start(DIAG_ERROR, "E0007", start, "a backslash can't end a line inside a string");
            diag_label(d, "stray backslash");
            diag_help(d, "strings can span lines on their own; for a newline character write `\\n`");
        } else {
            d = diag_start(DIAG_ERROR, "E0007", start, "unknown escape sequence `\\%c`", c);
            diag_label(d, "unknown escape");
            if (c == '$') {
                diag_fix(d, start, "$", "`$` has no special meaning in watrlang strings, so it needs no backslash");
            } else {
                diag_note(d, "the escapes are \\n \\t \\r \\0 \\\\ \\\" \\' \\{ \\} and \\xHH");
                diag_help(d, "for a literal backslash, write `\\\\`");
            }
        }
        diag_end(d);
        return -1;
    }
}

/* Result of lexing a string body. */
typedef enum {
    STR_OK,
    STR_ERROR,     /* an error was already reported */
    STR_NEWLINE,   /* single_line was set and a newline came first; not reported */
} StrResult;

static StrResult lex_string_body(Lexer *l, Loc open, uint32_t *flags, bool single_line);

static void report_unclosed_interp(Loc open) {
    Diag *d = diag_start(DIAG_ERROR, "E0010", open, "unterminated interpolation");
    diag_label(d, "this `{` is never closed");
    diag_note(d, "an interpolation has to close on the same line it starts");
    diag_help(d, "add `}` after the expression, or write `\\{` for a literal brace");
    diag_end(d);
}

typedef enum {
    INTERP_OK,
    INTERP_RECOVERED,  /* error reported, but the string can carry on */
    INTERP_FATAL,      /* error reported, end the string token here */
} InterpResult;

/* Skips a {interpolation} inside a string, up to and including its `}`.
 * Nested strings and braces are allowed: "{f("x")}", "{m[{a}]}".
 * An interpolation must fit on one line, so a forgotten `}` is reported
 * at the `{` instead of swallowing the rest of the file. */
static InterpResult lex_interp(Lexer *l, uint32_t *flags) {
    Loc open = here(l);
    open.len = 1;
    advance(l); /* { */
    *flags |= TOKF_INTERP;

    if (peek(l, 0) == '}') {
        advance(l);
        open.len = 2;
        Diag *d = diag_start(DIAG_ERROR, "E0009", open, "empty interpolation");
        diag_label(d, "nothing to interpolate");
        diag_fix(d, open, "\\{}", "for literal braces, escape the opening one");
        diag_end(d);
        return INTERP_RECOVERED;
    }

    int depth = 1;
    bool ok = true;
    for (;;) {
        char c = peek(l, 0);
        if (at_end(l) || c == '\n') {
            report_unclosed_interp(open);
            return INTERP_FATAL;
        }
        if (c == '{') {
            depth++;
            advance(l);
        } else if (c == '}') {
            advance(l);
            if (--depth == 0) return ok ? INTERP_OK : INTERP_RECOVERED;
        } else if (c == '"') {
            Loc inner = here(l);
            inner.len = 1;
            advance(l);
            uint32_t ignored = 0;
            StrResult r = lex_string_body(l, inner, &ignored, true);
            if (r == STR_NEWLINE) {
                report_unclosed_interp(open);
                return INTERP_FATAL;
            }
            if (r == STR_ERROR) {
                /* Stopped at a line end: the nested string already reported why. */
                if (at_end(l) || peek(l, 0) == '\n') return INTERP_FATAL;
                ok = false;
            }
        } else if (c == '\'') {
            /* A char literal like '}' must not end the interpolation. */
            advance(l);
            if (peek(l, 0) == '\\') {
                advance(l);
                if (lex_escape(l) < 0) ok = false;
            } else if (!at_end(l) && peek(l, 0) != '\n') {
                advance(l);
            }
            if (peek(l, 0) == '\'') advance(l);
        } else {
            advance(l);
        }
    }
}

/* Lexes the rest of a string after its opening quote. */
static StrResult lex_string_body(Lexer *l, Loc open, uint32_t *flags, bool single_line) {
    bool ok = true;
    for (;;) {
        if (at_end(l)) {
            if (single_line) return STR_NEWLINE;
            Diag *d = diag_start(DIAG_ERROR, "E0011", open, "unterminated string literal");
            diag_label(d, "this string is never closed");
            diag_note(d, "strings can span lines, so it runs to the end of the file");
            diag_help(d, "add a closing `\"`");
            diag_end(d);
            return STR_ERROR;
        }
        char c = peek(l, 0);
        if (c == '\n' && single_line) {
            return STR_NEWLINE;
        } else if (c == '"') {
            advance(l);
            return ok ? STR_OK : STR_ERROR;
        } else if (c == '\\') {
            advance(l);
            if (lex_escape(l) < 0) ok = false;
        } else if (c == '{') {
            /* A broken interpolation ends the string token here; lexing
             * resumes on the same line instead of hunting for a far-off quote. */
            InterpResult r = lex_interp(l, flags);
            if (r == INTERP_FATAL) return STR_ERROR;
            if (r == INTERP_RECOVERED) ok = false;
        } else {
            advance(l);
        }
    }
}

static Token lex_string(Lexer *l) {
    Loc start = here(l);
    Token t = {TOK_STRING, 0, start, l->src + l->pos, {0}};
    Loc open = start;
    open.len = 1;
    advance(l); /* " */
    StrResult r = lex_string_body(l, open, &t.flags, false);
    t.loc = span_from(l, start);
    if (r != STR_OK) t.kind = TOK_ERROR;
    return t;
}

static Token lex_char(Lexer *l) {
    Loc start = here(l);
    Token t = {TOK_CHAR, 0, start, l->src + l->pos, {0}};
    advance(l); /* ' */

    int value = -1;
    char c = peek(l, 0);
    if (at_end(l) || c == '\n') {
        t.loc = span_from(l, start);
        Diag *d = diag_start(DIAG_ERROR, "E0012", t.loc, "unterminated char literal");
        diag_label(d, "this char literal is never closed");
        diag_help(d, "add a closing `'`");
        diag_end(d);
        t.kind = TOK_ERROR;
        return t;
    }
    if (c == '\'') {
        advance(l);
        t.loc = span_from(l, start);
        Diag *d = diag_start(DIAG_ERROR, "E0012", t.loc, "empty char literal");
        diag_label(d, "empty");
        diag_help(d, "a char literal holds exactly one byte, like `'a'`; a quote is `'\\''`");
        diag_end(d);
        t.kind = TOK_ERROR;
        return t;
    }
    if (c == '\\') {
        advance(l);
        value = lex_escape(l);
    } else {
        value = (unsigned char)advance(l);
    }

    if (peek(l, 0) != '\'') {
        /* Find the closing quote on this line so the error covers the whole literal. */
        while (!at_end(l) && peek(l, 0) != '\'' && peek(l, 0) != '\n') advance(l);
        bool closed = peek(l, 0) == '\'';
        if (closed) advance(l);
        t.loc = span_from(l, start);
        Diag *d;
        if (closed) {
            d = diag_start(DIAG_ERROR, "E0012", t.loc, "char literal holds more than one byte");
            diag_label(d, "a char literal is a single byte (u8)");
            if (t.loc.len < 70) {
                char fixed[80];
                snprintf(fixed, sizeof fixed, "\"%.*s\"", (int)t.loc.len - 2, t.start + 1);
                diag_fix(d, t.loc, fixed, "if you meant a string, use double quotes");
            }
        } else {
            d = diag_start(DIAG_ERROR, "E0012", t.loc, "unterminated char literal");
            diag_label(d, "this char literal is never closed");
            diag_help(d, "add a closing `'`");
        }
        diag_end(d);
        t.kind = TOK_ERROR;
        return t;
    }
    advance(l);
    t.loc = span_from(l, start);
    if (value < 0) {
        t.kind = TOK_ERROR;
        return t;
    }
    t.v.int_val = (uint64_t)value;
    return t;
}

Token lexer_next(Lexer *l) {
    skip_trivia(l);

    Loc start = here(l);
    Token t = {TOK_EOF, 0, start, l->src + l->pos, {0}};
    if (at_end(l)) return t;

    char c = peek(l, 0);

    if (is_ident_start(c)) {
        while (is_ident_char(peek(l, 0))) advance(l);
        if ((unsigned char)peek(l, 0) >= 0x80) {
            while ((unsigned char)peek(l, 0) >= 0x80 || is_ident_char(peek(l, 0))) advance(l);
            t.kind = TOK_ERROR;
            t.loc = span_from(l, start);
            report_non_ascii(t.loc);
            return t;
        }
        t.loc = span_from(l, start);
        t.kind = keyword_or_ident(t.start, t.loc.len);
        return t;
    }
    if (is_digit(c)) return lex_number(l);
    if (c == '"') return lex_string(l);
    if (c == '\'') return lex_char(l);

    for (size_t i = 0; i < sizeof puncts / sizeof puncts[0]; i++) {
        size_t n = strlen(puncts[i].text);
        if (l->pos + n <= l->len && memcmp(l->src + l->pos, puncts[i].text, n) == 0) {
            for (size_t j = 0; j < n; j++) advance(l);
            t.kind = puncts[i].kind;
            t.loc = span_from(l, start);
            return t;
        }
    }

    advance(l);
    t.kind = TOK_ERROR;
    t.loc = span_from(l, start);
    if (c == '$' && is_ident_start(peek(l, 0))) {
        const char *name = l->src + l->pos;
        size_t n = 0;
        while (is_ident_char(name[n])) n++;
        Diag *d = diag_start(DIAG_ERROR, "E0013", t.loc, "watrlang variables don't start with `$`");
        diag_label(d, "remove this `$`");
        diag_note(d, "watrlang looks like PHP, but names have no sigil: write `%.*s`, not `$%.*s`",
                  (int)n, name, (int)n, name);
        diag_fix(d, t.loc, "", "remove the `$`");
        diag_end(d);
    } else if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7f) {
        Diag *d = diag_start(DIAG_ERROR, "E0014", t.loc, "unexpected character `%c`", c);
        diag_label(d, "not valid here");
        if (c == '#') diag_fix(d, t.loc, "//", "comments start with `//`");
        else if (c == '`') diag_help(d, "strings use double quotes: \"text\"");
        diag_end(d);
    } else if ((unsigned char)c >= 0x80) {
        /* One error for the whole word (e.g. `été`), not one per byte. */
        while ((unsigned char)peek(l, 0) >= 0x80 || is_ident_char(peek(l, 0))) advance(l);
        t.loc = span_from(l, start);
        report_non_ascii(t.loc);
    } else {
        Diag *d = diag_start(DIAG_ERROR, "E0014", t.loc, "unexpected control character 0x%02X", (unsigned char)c);
        diag_label(d, "invisible character here");
        diag_end(d);
    }
    return t;
}
